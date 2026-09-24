#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "core/ReconcilePlan.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"
#include "utils/Utf8.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace
{

// 전략 프로세스가 공유 쪽지를 기다리는 시간. 주문 쪽은 토큰 발급·잔고 대조·원장 리플레이를 먼저 하므로
//  기동이 몇 초 늦을 수 있다 — 그보다 넉넉히 두되, 아예 안 뜬 경우에는 기다림이 끝나야 한다. [why D-114]
constexpr auto kSharedRegionAttachTimeout = std::chrono::seconds(30);

} // namespace

Engine::Engine(KisConfig kis_config, int fetch_interval_sec)
    : kis_config_(std::move(kis_config)), fetch_interval_sec_(fetch_interval_sec)
{
    auto& ledger = order_gate_.ledger();

    // 쪽지 위 표의 칸 수는 힙 표와 같아야 한다 — 종목 번호로 바로 색인하는 배열(마지막 값·청산 관리·경로표)이
    //  힙 표의 칸 수로 잡혀 있어, 쪽지 쪽이 크면 그 배열 밖을 짚는다. [why D-114]
    layout_config_.symbol_capacity   = symbols_.table.capacity();
    layout_config_.strategy_capacity = ledger.strategy_table().capacity();

    // 자리표를 먼저 깐다 — 장부 사본·박동·응답 큐가 그 위에 있어 전략이 붙기 전에 자리가 서 있어야 한다.
    //  시세 줄 수는 아직 모른다(config를 안 읽었다). 한 줄로 깔아 두고 start()가 소켓 수로 다시 깐다. [why D-114]
    if (!bind_layout(1))
    {
        // 여기서 실패하는 길은 칸 수 상수나 자리 셈이 어긋났을 때뿐이다. start()가 이 상태를 보고 안 뜬다.
        LOG_ERROR("[Engine] 자리표를 못 깔았다 — " + std::string(layout_.last_error()));
    }

    // 원장 키의 종목 번호를 신호·틱과 같은 테이블에서 받는다. 첫 시드·체결 전에 묶어야 한다. [why D-105]
    ledger.set_symbol_table(&symbols_.table);
}

// 자리표를 깐다. 갈라 띄우면 이 바이트가 공유 쪽지가 되고, Both 로 돌면 힙 한 덩이가 그 자리를 대신한다 —
//  놓는 자리도 셈도 같아서 갈라 띄우는 날 처음 도는 코드가 없다. [why D-114]
//  [inv] 스레드가 뜨기 전에만 부른다. 돌던 큐 위에 다시 깔면 칸이 0으로 밀려 오가던 것이 사라진다.
bool Engine::bind_layout(uint32_t feed_lanes)
{
    // 큐 칸 수는 한 프로세스로 돌던 때 쓰던 상수를 그대로 쓴다 — 자리표에 따로 적으면 두 벌이 된다.
    layout_config_.feed_lanes        = feed_lanes;
    layout_config_.request_capacity  = ShardPipeline::kOrderQueueCapacity;
    layout_config_.response_capacity = ShardPipeline::kOrderResponseCapacity;
    layout_config_.control_capacity  = ShardPipeline::kControlQueueCapacity;

    const size_t needed = ipc::SharedLayout::bytes_for(layout_config_);

    layout_.unbind();

    // 한 프로세스로 돌면 힙, 갈라 띄우면 공유 쪽지다. 고르는 자리는 여기 하나다. [why D-114]
    if (!(role_ == ProcessRole::Both ? bind_layout_on_heap(needed) : bind_layout_on_region(needed)))
    {
        return false;
    }

    // 자리표 위 면을 쓰는 자리에 꽂는다. [inv] 이 포인터들은 다음 bind_layout 까지만 유효하다.
    ledger_snapshot_             = layout_.ledger();
    pipeline_.controls           = &layout_.controls();
    pipeline_.feed_controls      = &layout_.feed_controls();
    pipeline_.requests           = &layout_.requests();
    pipeline_.order_responses    = &layout_.responses();
    pipeline_.strategy_heartbeat = &layout_.heartbeats()->strategy;
    pipeline_.order_heartbeat    = &layout_.heartbeats()->order;
    pipeline_.feed_heartbeat     = &layout_.heartbeats()->feed;

    adopt_shared_dictionaries();
    return true;
}

// 종목 표·전략 이름표를 공유 쪽지 위 한 벌로 바꾼다. 주문 요청이 종목·전략을 정수로 나르므로(원칙 6)
//  양쪽 번호가 갈리면 엉뚱한 종목에 주문이 나가고 손익이 남의 전략에 붙는다. [why D-114]
//  [inv] 스레드가 뜨기 전에만 부른다 — 표를 바꾸면 그 전에 받아 둔 번호는 다른 표의 것이 된다.
void Engine::adopt_shared_dictionaries()
{
    auto& ledger = order_gate_.ledger();

    // 한 프로세스로 돌면 힙 표 그대로다 — 같이 볼 건너편이 없다.
    if (role_ == ProcessRole::Both)
    {
        return;
    }

    if (role_ == ProcessRole::Order)
    {
        // 넣는 쪽 — 쪽지 위 표에 바로 넣는다(그 안 쓰기 자물쇠가 이 프로세스의 스레드를 직렬화한다).
        symbols_.table.adopt(layout_.symbols().slots(),
                             [this](std::string_view ticker) { return layout_.symbols().intern(ticker); });
        ledger.adopt_strategy_table(layout_.strategies().slots(),
                                         [this](std::string_view name) { return layout_.strategies().intern(name); });
    }
    else if (role_ == ProcessRole::Feed)
    {
        // 시세는 찾기만 한다 — 넣어 달라는 부탁은 제어 줄을 타는데, 그 줄은 보내는 쪽 하나(전략)로 선
        //  한줄 큐다. 시세가 같은 줄에 끼면 깨진다. 표에 없는 티커는 청하지 않은 종목이 세션에 실려 온
        //  것이니 버리고 센다. 부르는 자리가 register_symbol 이든 표의 넣기 훅이든 같게 막는다.
        //  [why D-114 단계 5]
        symbols_.table.adopt(layout_.symbols().slots(),
                             [this](std::string_view)
                             {
                                 unknown_ticker_dropped_.fetch_add(1, std::memory_order_relaxed);

                                 return symbol::kNone;
                             });
        ledger.adopt_strategy_table(layout_.strategies().slots(),
                                         [](std::string_view)
                                         {
                                             // 시세는 주문을 내지 않아 전략 번호를 쓸 일이 없다. 여기 오면
                                             //  부르는 자리가 잘못 섞인 것이다.
                                             LOG_ERROR("[Engine] 시세 역할이 전략 번호를 청했다 — 부르는 자리가 잘못됐다");

                                             return strategy_table::kNone;
                                         });
    }
    else
    {
        // 읽는 쪽 — 찾기는 같은 배열에서 자물쇠 없이, 넣기는 주문 쪽에 부탁한다.
        symbols_.table.adopt(layout_.symbols().slots(),
                             [this](std::string_view ticker) { return request_symbol_registration(ticker); });
        ledger.adopt_strategy_table(layout_.strategies().slots(),
                                         [this](std::string_view name) { return request_strategy_registration(name); });
    }

    // 고정 이름 셋은 힙 표에 찍힌 번호다 — 표를 바꿨으니 새 표에서 다시 받는다. 안 받으면 강제청산·수동
    //  주문의 손익이 남의 전략에 붙는다.
    manual_strategy_index_   = ledger.strategy_index_of("MANUAL");
    force_liquidation_index_ = ledger.strategy_index_of("FORCE_LIQ");
    limit_trim_index_        = ledger.strategy_index_of("LIMIT_TRIM");

    LOG_INFO("[Engine] 공유 종목 표 " + std::to_string(symbols_.table.size()) + "종목 · 전략 이름표 " +
             std::to_string(ledger.strategy_table().size()) + "개 연결");
}

bool Engine::bind_layout_on_heap(size_t needed)
{
    // 자리표는 캐시라인 경계에서 시작해야 한다. 힙이 주는 경계는 그보다 작아 한 줄만큼 더 잡고 밀어 맞춘다.
    layout_region_.close();
    layout_storage_.assign(needed + ipc::kSharedCacheLine, std::byte{});

    void*  aligned   = layout_storage_.data();
    size_t available = layout_storage_.size();

    if (std::align(ipc::kSharedCacheLine, needed, aligned, available) == nullptr)
    {
        LOG_ERROR("[Engine] 자리표를 캐시라인 경계에 못 맞췄다");
        return false;
    }

    return layout_.create(static_cast<std::byte*>(aligned), available, layout_config_);
}

std::string Engine::shared_region_name() const
{
    // 계좌가 다르면 쪽지도 다르다 — 모의와 실계좌를 같이 띄우는 날(감시견 두 갈래)에 서로의 큐를 보지 않게.
    const std::string account = kis_config_.account_no.empty() ? std::string("default") : kis_config_.account_no;
    return std::string("quant.engine.") + (kis_config_.is_paper ? "paper." : "live.") + account;
}

bool Engine::bind_layout_on_region(size_t needed)
{
    const std::string name  = shared_region_name();
    const size_t      bytes = sizeof(ipc::SharedRegionHeader) + needed;

    layout_storage_.clear();
    layout_storage_.shrink_to_fit();
    layout_region_.close();

    if (role_ == ProcessRole::Order)
    {
        // 만드는 쪽은 주문 프로세스 하나다. 이미 있다는 것은 아직 누가 쥐고 있다는 뜻이라 뜨지 않는다 —
        //  엔진 둘이 같은 계좌에 뜨는 것을 여기서 잡는다(이중 발주가 A등급이다).
        if (!layout_region_.create(name, bytes, ipc::kSharedLayoutVersion))
        {
            LOG_ERROR("[Engine] 공유 쪽지를 못 만들었다 — " + name + " (" + std::string(layout_region_.last_error()) + ")");
            return false;
        }

        if (layout_region_.took_over_stale())
        {
            // 앞선 기동이 종료 사유를 안 적고 사라졌다 = 크래시다. 회차·재기동 사이에 쪽지가 깨끗이
            //  치워졌는지를 이 줄 하나로 본다. 물려받았으므로 기동 자체는 이어 간다. [why D-114]
            LOG_WARN("[Engine] 앞선 기동이 정상 종료로 끝나지 않았다 — 옛 쪽지를 물려받았다(기동 번호 " +
                     std::to_string(layout_region_.boot_generation()) + ")");
        }

        LOG_INFO("[Engine] 공유 쪽지 생성: " + name + " " + std::to_string(bytes) + "바이트 (기동 번호 " +
                 std::to_string(layout_region_.boot_generation()) + ")");
        return layout_.create(layout_region_.payload(), layout_region_.payload_bytes(), layout_config_);
    }

    // 붙는 쪽은 전략·시세 프로세스 둘이다. 주문 쪽이 먼저 떠야 쪽지가 있으므로 그동안 기다린다 — 감시견은
    //  셋을 같이 띄우고 순서를 정해 주지 않는다. 기다려도 없으면 뜨지 않는다(빈 큐로 돌면 신호가 사라진다).
    //  역할을 같이 넘기는 것은 면마다 제 끝(보내는 쪽·받는 쪽)이 다르기 때문이다 — 제 끝이 아닌 면에
    //  손대면 남의 소비자 칸을 덮는다. [why D-114 단계 5]
    const auto attach_role = role_ == ProcessRole::Feed ? ipc::SharedAttachRole::kFeed : ipc::SharedAttachRole::kStrategy;
    const auto deadline    = std::chrono::steady_clock::now() + kSharedRegionAttachTimeout;
    bool       waited      = false;

    while (!layout_region_.attach(name, bytes, ipc::kSharedLayoutVersion, attach_role))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            LOG_ERROR("[Engine] 공유 쪽지에 못 붙었다 — " + name + " (" + std::string(layout_region_.last_error()) +
                      ") 주문 프로세스가 떠 있는지 본다");
            return false;
        }

        if (!waited)
        {
            LOG_INFO("[Engine] 공유 쪽지 기다리는 중: " + name + " — 주문 프로세스가 만들면 붙는다");
            waited = true;
        }

        std::this_thread::sleep_for(100ms);
    }

    LOG_INFO("[Engine] 공유 쪽지 연결: " + name + " " + std::to_string(bytes) + "바이트 (기동 번호 " +
             std::to_string(layout_region_.boot_generation()) + ")");
    // 붙은 판의 기동 번호를 적어 둔다 — 제어 스레드가 이 값과 대조해 건너편 재기동을 잡는다. [why D-114]
    peer_boot_generation_ = layout_region_.boot_generation();
    return layout_.attach(layout_region_.payload(), layout_region_.payload_bytes(), layout_config_, attach_role);
}

Engine::~Engine()
{
    stop();
}

// ─── start() 단계 분리 (가독성용, 로직은 그대로) ──────────────────────────────
void Engine::setup_shards()
{
    // 샤드는 WS 콜백·데이터 스레드가 push 뒤 깨우므로 소켓을 열기 전에 만든다. 스레드는 아래에서 같이 띄운다.
    //  stop() 뒤 다시 start()하면 옛 샤드(스레드는 join 뒤)를 버리고 새로 만든다.
    pipeline_.shard_threads.clear();
    pipeline_.shards.clear();

    // [inv] WS 수신 스레드 수 = 소켓 수 — 아래 feed_.websocket 생성과 같은 조건(리플레이·소켓 하나면 1, feed_keys가 있으면 1+N)이라
    //  feed_.websocket->lanes()와 같다. 소켓을 만들기 전에 행 수가 필요해 config로 센다. 줄 수는 역할과 무관하게
    //  정한다 — 시세 역할도 공유 통로에 넣을 때 줄 번호를 쓴다(Engine::push_feed_trade). [why D-114]
    pipeline_.websocket_lanes = websocket_lane_count();
    pipeline_.data_row        = pipeline_.websocket_lanes;

    // 샤드와 그 앞 큐 행렬·라우팅 표는 전략 역할만 읽는다. 갈라 띄우면 시세 쪽 수신부는 공유 통로에 넣고
    //  바로 돌아가고(Engine::push_feed_trade), 주문 쪽에는 전략이 아예 없다(load_strategies가 전략 역할에서만
    //  돈다, Quant/src/main.cpp). 그래서 여기서 만들면 아무도 안 읽는 셀 (줄+1)×샤드×4096개를 들고 있게 된다.
    //  단계 4에서 "주문 역할에서는 샤드를 아예 안 만든다"로 미뤄 둔 몫이다. [why D-114]
    if (!runs_strategy_side())
    {
        LOG_INFO("[Engine] 전략 샤드 없음 — 이 역할은 샤드를 쓰지 않는다 (줄 " +
                 std::to_string(pipeline_.websocket_lanes) + ")");
        return;
    }

    // 샤드 수는 config 그대로(상한은 마스크 폭). 전략은 등록 순 라운드로빈으로 샤드 하나씩 갖는다 — 종목이 몇 개든
    //  객체는 스레드 하나만 만지므로 걸침 검사·1 폴백이 없다. [why D-110]
    uint32_t shard_count = pipeline_.strategy_shards;

    if (shard_count > shard::kMaxShards)
    {
        LOG_WARN("[Engine] strategy_shards=" + std::to_string(shard_count) + " → " + std::to_string(shard::kMaxShards) + "(마스크 폭 상한)");
        shard_count = shard::kMaxShards;
    }

    pipeline_.next_shard = 0;

    for (const auto& registered_strategy : strategy_.list)
    {
        registered_strategy->set_shard_index(pipeline_.next_shard);
        pipeline_.next_shard = (pipeline_.next_shard + 1) % shard_count;
    }

    pipeline_.order_book_matrix.reshape(pipeline_.websocket_lanes, shard_count, ShardPipeline::kTickCellCapacity);
    pipeline_.trade_matrix.reshape(pipeline_.websocket_lanes + 1, shard_count, ShardPipeline::kTickCellCapacity);
    pipeline_.bars_matrix.reshape(1, shard_count, ShardPipeline::kBarCellCapacity);

    for (uint32_t shard_index = 0; shard_index < shard_count; ++shard_index)
    {
        pipeline_.shards.push_back(std::make_unique<strategy::Shard>(shard_index, strategy::ShardQueues{pipeline_.order_book_matrix, pipeline_.trade_matrix, pipeline_.bars_matrix}));
    }

    if (pipeline_.routes.capacity() != symbols_.table.capacity())
    {
        pipeline_.routes.reset(symbols_.table.capacity());
    }

    rebuild_routes_locked(); // 스레드 시작 전이라 락 없이

    LOG_INFO("[Engine] 전략 샤드 " + std::to_string(shard_count) + "개 (config strategy_shards=" +
             std::to_string(pipeline_.strategy_shards) + ")");
}

uint32_t Engine::websocket_lane_count() const
{
    if (feed_.feed_override)
    {
        return feed_.feed_override->lanes();
    }

    if (feed_.replay_file.empty() && !feed_.extra_feed_cfgs.empty())
    {
        return static_cast<uint32_t>(feed_.extra_feed_cfgs.size() + 1);
    }

    return 1u;
}

void Engine::assign_shard(StrategyBase& strategy)
{
    const auto shard_count = static_cast<uint32_t>(pipeline_.shards.size());
    strategy.set_shard_index(shard_count <= 1 ? 0u : pipeline_.next_shard);
    pipeline_.next_shard = shard_count <= 1 ? 0u : (pipeline_.next_shard + 1) % shard_count;
}

void Engine::rebuild_routes_locked()
{
    auto draft = pipeline_.routes.draft();

    for (const auto& registered_strategy : strategy_.list)
    {
        const auto specifications = registered_strategy->get_watch_specifications();

        // 구독을 안 밝힌 전략은 전부 받는다(Router와 같은 규칙) — 그 샤드는 모든 종목을 받는다.
        if (specifications.empty())
        {
            draft.add_all(registered_strategy->shard_index());
            continue;
        }

        for (const auto& watch_specification : specifications)
        {
            const symbol::SymbolId id = register_symbol(watch_specification.ticker);

            if (id == symbol::kNone)
            {
                draft.add_all(registered_strategy->shard_index()); // 테이블이 찼다 — 틱을 놓치는 것보다 낫다
                break;
            }

            draft.add(id, registered_strategy->shard_index());
        }
    }

    pipeline_.routes.commit(draft);
}

#ifdef HAS_ZMQ
void Engine::setup_zmq_bridge()
{
    // 역할마다 발행 포트가 하나씩이다 — 주문은 config 포트에 제어(REP)까지 열고, 시세·전략은 발행만 연다.
    //  셋을 한 포트로 모으려면 중계가 한 자리 생기는데, 그 자리가 죽으면 셋이 같이 멎어 프로세스를 가른
    //  뜻이 없어진다. 구독자(SUB)는 bind가 아니라 connect라 포트 셋에 한꺼번에 붙을 수 있다. [why D-114]
    int publish_port = zmq_ports_.order_pub;
    int reply_port   = zmq_ports_.order_rep;

    if (role_ == ProcessRole::Feed)
    {
        publish_port = zmq_ports_.feed_pub;
        reply_port   = 0;
    }
    else if (role_ == ProcessRole::Strategy)
    {
        publish_port = zmq_ports_.strategy_pub;
        reply_port   = 0;
    }

    zmq_bridge_ = std::make_unique<ZmqBridge>(publish_port, reply_port);
    zmq_bridge_->set_bind_address(zmq_bind_address_);
    zmq_bridge_->set_account_no(kis_config_.account_no); // 실계좌·모의계좌 원장 분리용 [why D-090]
    zmq_bridge_->set_role_label(role_.to_string());       // HEALTH 를 역할별로 가르는 열 [why D-129]

    // 국면 칸을 꽂는다. 국면을 고르는 쪽은 전략이고 체결을 적는 쪽은 주문이라, 갈라 띄우면 프로세스 안
    //  정수만으로는 주문 쪽이 국면을 영영 모른다(체결의 regime 열이 통째로 빈다). [why D-129]
    //  [inv] bind_layout() 뒤에 온다 — 자리표를 다시 깔면 이 포인터가 옮겨간다.
    zmq_bridge_->set_regime_cell(layout_.regime_cell());

    // 제어(KILL·STATUS)는 주문 쪽 하나만 받는다 — 시세·전략 프로세스에는 REP 소켓 자체가 없어
    //  토큰도 핸들러도 걸 자리가 없다.
    if (reply_port <= 0)
    {
        zmq_bridge_->start();
        return;
    }

    zmq_bridge_->set_control_token(zmq_control_token_);
    zmq_bridge_->set_command_handler(
        [this](const std::string& command) -> std::string
        {
            if (command == "KILL")
            {
                LOG_WARN("[ZMQ] KILL 명령 수신 — 신규 주문 차단 + 엔진 종료");
                request_kill_switch(true);
                write_state_marker("kill_today", "ZMQ KILL");
                request_shutdown("ZMQ KILL 명령");
                return "OK";
            }

            if (command == "STATUS")
            {
                // 버린 건수 넷을 여기에도 싣는다 — HEALTH 로도 나가지만 그쪽은 발행 큐를 타고, 그 큐에서 HEALTH 는
                //  FILL·ORDER·SIGNAL 보다 작은 한도를 쓴다(ZmqBridge::enqueue). 큐가 차면 버린 건수를 알려 줄
                //  메시지가 가장 먼저 버려져, 정작 버리는 중일 때만 값이 사라진다. REP 는 그 큐를 안 거친다. [why D-125]
                return "{\"running\":true"
                       ",\"data\":" +
                       std::to_string(data_count_.load()) + ",\"signal\":" + std::to_string(signal_count_.load()) +
                       ",\"order\":" + std::to_string(order_count_.load()) +
                       ",\"drop\":" + std::to_string(zmq_bridge_->drop_count()) +
                       ",\"drop_socket_full\":" + std::to_string(zmq_bridge_->socket_full_drop_count()) +
                       ",\"drop_socket_error\":" + std::to_string(zmq_bridge_->socket_error_drop_count()) +
                       ",\"drop_send_queue_full\":" + std::to_string(zmq_bridge_->send_queue_full_drop_count()) +
                       ",\"drop_trade_ring_full\":" + std::to_string(zmq_bridge_->trade_ring_full_drop_count()) + "}";
            }

            return "UNKNOWN";
        });
    zmq_bridge_->start();
}
#endif

bool Engine::authenticate_feed(bool offline)
{
    // 피드를 직접 받았거나 캡처 파일을 트는 것이면 브로커 없이 돈다 — feed_.kis는 비고, 아래 KIS를 보는 경로는 전부 null을
    //  "소스 없음"으로 다룬다. 리플레이의 종목은 config tickers(전략 구독)뿐이고 유니버스 스캔·REST 봉 시드는 없다. [why D-071]
    if (offline)
    {
        LOG_INFO(std::string("[Engine] ") + (feed_.feed_override ? "피드 주입" : "리플레이") +
                 " — KIS 없이 기동(주문·잔고는 모의 체결기)");
        return true;
    }

    feed_.kis = std::make_unique<KisClient>(kis_config_);

    if (!feed_.kis->authenticate())
    {
        LOG_ERROR("[Engine] KIS 인증 실패");
        return false;
    }

    // 시세 전용 클라이언트(실전 도메인) — 모의 시세 REST가 HTTP 500이므로 시세만 실전으로 조회.
    //  실패해도 feed_.kis(모의)로 폴백하되, 모의 시세는 500이라 사실상 틱이 안 나옴을 경고.
    //  WS 모드에서도 만들어 둔다: WS가 죽어 폴링으로 낮출 때 쓸 시세 소스가 그때 가서는 없으면
    //  폴백이 무의미해진다(모의 도메인으로 폴링하면 500만 쌓인다).
    if (feed_.has_quote_kis)
    {
        feed_.quote_kis = std::make_unique<KisClient>(feed_.quote_kis_config);

        if (!feed_.quote_kis->authenticate())
        {
            LOG_ERROR("[Engine] 시세 클라이언트(실전) 인증 실패 — 모의 시세로 폴백(틱 없을 수 있음)");
            feed_.quote_kis.reset();
        }
        else
        {
            LOG_INFO("[Engine] 시세 클라이언트(실전 도메인) 인증 완료 — 현재가 폴링 소스");
        }
    }

    return true;
}

void Engine::setup_paper_executor(bool offline)
{
    // KIS가 없으면 주문·잔고는 모의 체결기가 받는다. [why D-071]
    if (offline)
    {
        feed_.paper = std::make_unique<feed::PaperExecutor>(feed_.replay_cash, symbols_.table);
        LOG_INFO("[Engine] 모의 체결기: 현금 " + std::to_string(static_cast<long long>(feed_.replay_cash)) + "원");
    }
}

void Engine::initialize_order_router()
{
    IOrderExecutor& executor = feed_.paper ? static_cast<IOrderExecutor&>(*feed_.paper) : *feed_.kis;

    // FEP OrderRouter 초기화
#ifdef HAS_ZMQ
    order_router_ = std::make_unique<OrderRouter>(order_gate_, executor, zmq_bridge_.get());
#else
    order_router_ = std::make_unique<OrderRouter>(order_gate_, executor);
#endif
    LOG_INFO("[Engine] OrderRouter (FEP) 초기화 완료");
    start_ops_server();

    // 이전 세션이 남긴 미체결 주문을 취소한다. 건당 왕복이 3~5초라 여기서 기다리면
    //  기동이 몇 분씩 멈춘다 — 목록을 읽고 부속 파일을 비우는 것만 여기서 하고 취소는
    //  별도 스레드가 이어서 낸다. 잔고 시드가 이를 기다릴 필요는 없다: 미체결 취소는
    //  보유수량을 바꾸지 않고 주문가능현금·매도가능수량만 푸는데 둘 다 주기 잔고 대조가
    //  다시 읽는다.
    order_router_->cancel_stale_orders_async();
}

void Engine::initialize_ledger_reconciler()
{
    // 잔고 → 원장 대조기. 브로커·라우터·종목명은 함수로 넘겨 대조기가 KisClient·OrderRouter를 모르게 한다. [why D-061]
    ledger_ = std::make_unique<LedgerReconciler>(order_gate_,
                                                 [this] { return feed_.paper ? feed_.paper->balance() : feed_.kis->get_balance(); });
    ledger_->set_account_no(feed_.kis ? feed_.kis->account_no() : std::string("PAPER"));
    ledger_->set_baseline_directory(Logger::instance().base_directory()); // 실행 위치와 무관하게 로그 폴더와 같은 곳
    ledger_->set_name_sink([this](const std::string& ticker, const std::string& name) { register_ticker_name(ticker, name); });
    ledger_->set_reconcile_sink([this](const reconcile::Row& row) { order_router_->record_reconcile(row); });
}

void Engine::initialize_data_poller()
{
    // REST 현재가 폴러. 시세는 시세 전용 클라이언트가 있으면 그쪽(실전 도메인 초당 한도가 높다). [why D-062]
    //  [lock-order] 데이터 스레드는 pipeline_.trade_matrix의 WS 수신 스레드 행에 넣지 않는다 — 폴러의 틱은 자기 행(pipeline_.data_row)으로 간다.
    poller_ = std::make_unique<DataPoller>(
        [this](const std::string& ticker)
        {
            KisClient* quote_client = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();
            return quote_client ? quote_client->get_current_price(ticker) : 0.0;
        },
        [this](TradeData trade)
        {
            trade.symbol_id       = lookup_symbol(trade.ticker);

            // 갈라 띄우면 샤드가 저쪽에 있다 — WS 수신 스레드와 같은 길로 통로에 넣고, 꺼내 가르는 일은
            //  전략 쪽 줄 스레드가 한다. 줄 번호는 행렬의 데이터 스레드 행과 같은 자리다(폴러 몫 한 줄).
            //  [inv] 이 줄에 넣는 스레드는 데이터 스레드 하나다 — SPSC가 그 위에 서 있다. [why D-114]
            //  단계 5부터 소켓과 넘침 폴러를 쥔 쪽은 시세 프로세스다 — 여기 넣는 쪽도 그쪽 하나다.
            if (role_ == ProcessRole::Feed)
            {
                push_feed_trade(pipeline_.data_row, trade);
                return;
            }

            shard::for_each_shard(pipeline_.routes.mask(trade.symbol_id), pipeline_.trade_matrix.consumer_of(trade.symbol_id),
                                  [&](uint32_t consumer)
            {
                while (!pipeline_.trade_matrix.push_to(pipeline_.data_row, consumer, trade) && running_.load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(1ms);
                }

                pipeline_.shards[consumer]->wake().notify();
            });
        });
    poller_->set_keep_going([this] { return running_.load(std::memory_order_acquire); });
}

bool Engine::try_open_ledger_journal()
{
    auto& ledger = order_gate_.ledger();

    if (ledger_journal_directory_.empty())
    {
        return true;
    }

    // 오늘 파일을 열고 처음부터 다시 적용한다 — 재기동 전 선점·체결·대조가 원장에 되살아난다. 못 열면 원장 없이
    //  주문이 나가는 셈이라 기동을 거부한다(감시견이 다시 띄운다). [why D-113]
    if (!ledger.set_journal(utf8::path_from_utf8(ledger_journal_directory_), kst::date_yyyymmdd(std::time(nullptr)),
                                 ledger_journal_fsync_))
    {
        LOG_ERROR("[Engine] 원장 저널을 못 열어 기동하지 않는다: " + ledger_journal_directory_);
        return false;
    }

    const auto& replay = ledger.journal_replay();
    LOG_INFO("[Engine] 원장 저널 리플레이: " + std::to_string(replay.applied) + "건 (마지막 seq " +
             std::to_string(replay.last_sequence) + (replay.truncated_tail ? ", 꼬리 잘림" : "") + ")");
    return true;
}

// 저널 리플레이가 남긴 미결 주문 — 엔진이 죽은 순간 호가창에 살아 있었을 주문이다. 잔고 시드가 끝난 뒤에 부른다:
//  체결로 닫힌 주문은 시드가 이미 보유에 반영했고, 여기서는 주문 쪽(이력·선점)만 맞춘다. [why D-113]
void Engine::resolve_open_intents()
{
    auto& ledger = order_gate_.ledger();

    const auto intents = ledger.open_intents();

    if (intents.empty())
    {
        LOG_INFO("[Engine] 원장 미결 주문 대조: 되살림 0건 · 선점해제 0건 · 저널기록실패 " +
                 std::to_string(ledger.journal_failures()) + "건");
        return;
    }

    const auto adopted = order_router_->adopt_open_intents(intents);
    LOG_INFO("[Engine] 원장 미결 주문 대조: 되살림 " + std::to_string(adopted.restored) + "건 · 선점해제 " +
             std::to_string(adopted.released) + "건 · 저널기록실패 " + std::to_string(ledger.journal_failures()) + "건");
}

bool Engine::try_bootstrap_ledger()
{
    // G5: 실계좌 보유분을 원장에 시드 (스레드 시작 전, 단일스레드 구간)
    if (!bootstrap_ledger_)
    {
        return true;
    }

    if (ledger_->bootstrap())
    {
        return true;
    }

    // running_이 아직 false라 main 루프가 바로 빠지고, 감시자가 5초 뒤 다시 띄운다.
    LOG_ERROR("[Engine] 원장 없이 기동하지 않는다 — 프로세스 종료");
    return false;
}

void Engine::collect_watch_specifications()
{
    // 전략별 구독 스펙 수집 (중복 제거)
    watch_specifications_.clear();
    {
        // 키가 "시장:거래소:티커" 문자열 셋의 조합이고 설정에서 온 스펙을 기동 때 한 번 거르는 자리라 문자열 집합을 쓴다.
        std::unordered_set<std::string> seen;

        for (auto& strategy : strategy_.list)
        {
            for (auto& specification : strategy->get_watch_specifications())
            {
                std::string key = (specification.market == Market::US ? "US:" : "KR:") + specification.exchange + ":" + specification.ticker;

                if (seen.insert(std::move(key)).second)
                {
                    watch_specifications_.push_back(std::move(specification)); // get_watch_specifications()가 준 임시 벡터라 옮겨도 된다
                }
            }
        }
    }

    LOG_INFO("[Engine] WS 구독 종목: " + std::to_string(watch_specifications_.size()) + "개");

    // 재스캔 중복 방지 시드 — 기동 유니버스에 이미 등록된 KR 종목 기록(스펙의 문자열 티커는 여기서 id가 된다).
    universe_rescan_.registered.assign(symbols_.table.capacity(), false);
    universe_rescan_.registered_count = 0;

    for (auto& specification : watch_specifications_)
    {
        if (specification.market == Market::KR)
        {
            rescan_set_registered(register_symbol(specification.ticker), true);
        }

        // 거는 자리는 소켓을 쥔 주문 쪽 하나다. Both 로 돌면 connect_feed() 가 이미 이 목록을 통째로
        //  걸어 둔 뒤라 이 요청은 "이미 구독 중"으로 끝난다 — 갈라 띄운 날 처음 도는 코드를 안 만들려고
        //  양쪽이 같은 길을 쓴다. [why D-114]
        send_watch_request(specification);
    }
}

bool Engine::add_watch_specification(const WatchSpec& specification)
{
    std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);

    for (const auto& watch_specification : watch_specifications_)
    {
        if (watch_specification.market == specification.market && watch_specification.exchange == specification.exchange &&
            watch_specification.ticker == specification.ticker && watch_specification.is_future == specification.is_future)
        {
            return false;
        }
    }

    watch_specifications_.push_back(specification);
    return true;
}

void Engine::send_watch_request(const WatchSpec& specification, ipc::ControlKind kind, int32_t priority)
{
    // 칸을 넘는 종목 코드는 잘라 보내지 않는다 — 잘린 코드로 구독하면 엉뚱한 종목의 틱이 이 종목 것으로 온다.
    if (specification.ticker.size() > symbol::Ticker::kMax || specification.exchange.size() >= ipc::kControlExchangeMax)
    {
        LOG_ERROR("[Engine] 구독 스펙이 칸을 넘어 보내지 못했다 — " + specification.ticker + "(" + specification.exchange + ")");
        return;
    }

    ipc::ControlRequest request;
    request.kind       = kind;
    request.rank       = priority;
    request.ticker     = std::string_view(specification.ticker);
    request.market     = static_cast<uint8_t>(specification.market);
    request.trade_only = specification.trade_only ? 1 : 0;
    request.is_future  = specification.is_future ? 1 : 0;
    ipc::set_exchange(request, specification.exchange);

    if (!send_control(request))
    {
        // 사라지면 그 종목은 틱이 영영 오지 않는다(09-11 실측: 구독 밖 종목 체결 0건). 큰 소리로 남긴다.
        LOG_ERROR("[Engine] 구독 요청을 못 보냈다 — " + specification.ticker + " 는 시세를 받지 못한다");
    }
}

void Engine::drain_pending_subscriptions()
{
    std::vector<WatchSpec> specifications;
    std::vector<WatchSpec> unsubscriptions;
    {
        std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
        specifications.swap(pending_subscriptions_); // 소켓 쓰기는 자물쇠 밖에서 한다
        unsubscriptions.swap(pending_unsubscriptions_);

        // 해지는 목록에서도 뺀다 — 남기면 REST 대체(poll_universe)와 재연결이 다시 건다.
        for (const auto& specification : unsubscriptions)
        {
            std::erase_if(watch_specifications_,
                          [&specification](const WatchSpec& watch) { return same_watch(watch, specification); });
        }
    }

    if ((specifications.empty() && unsubscriptions.empty()) || !feed_.websocket)
    {
        return;
    }

    // 해지를 먼저 푼다 — 돌려받은 칸을 같은 바퀴의 구독이 쓴다. [why D-132]
    for (const auto& specification : unsubscriptions)
    {
        const bool released = feed_.websocket->unsubscribe_incremental(specification);

        if (poller_)
        {
            poller_->remove_overflow(specification);
        }

        const symbol::SymbolId symbol = symbols_.table.lookup(specification.ticker);

        if (symbol < websocket_slots_.size())
        {
            websocket_slots_[symbol] = WebSocketSlotState{};
        }

        LOG_INFO("[WS칸] 구독 해지 — " + specification.ticker + (released ? " (칸 반납)" : " (REST 대체만 멈춤)") +
                 ", 보는 전략·보유·선점 없음");
    }

    for (const auto& specification : specifications)
    {
        if (feed_.capture)
        {
            // 그날 무엇을 구독했는지 캡처 파일 머리에 남긴다 — 호가가 빈 종목이 trade_only 인지 파일만 보고 알 수 있게.
            feed_.capture->on_universe(specification.ticker, static_cast<uint8_t>(specification.market),
                                       symbols_.table.intern(specification.ticker), specification.trade_only);
        }

        // 거짓 자체는 정상일 수 있다(연결 전·이미 구독). 목록에서까지 빠졌으면 구독 상한에 밀린 것이고,
        //  그대로 두면 이 종목은 틱 없이 조용히 매매하지 않는다(09-11 실측: 40 초과 종목 체결 0건).
        if (feed_.websocket->subscribe_incremental(specification) || feed_.websocket->has_specification(specification))
        {
            continue;
        }

        watch_overflow_.fetch_add(1, std::memory_order_relaxed);

        // 넘침 목록에 넣어 데이터 스레드가 REST 로 대신 흘린다. 폴러는 양쪽에 있고, 갈라 띄우면 주문 쪽
        //  폴러가 받아 시세 통로의 마지막 줄로 보낸다(구독을 거는 쪽과 같은 프로세스다). [why D-114]
        if (poller_ && poller_->add_overflow(specification))
        {
            LOG_WARN("[Engine] WS 구독 상한 — " + specification.ticker + " 시세는 REST 폴링으로 대체(넘침 " +
                     std::to_string(poller_->overflow_count()) + "종목)");
        }
        else if (!poller_)
        {
            LOG_ERROR("[Engine] WS 구독 상한 — " + specification.ticker + " 는 REST 대체가 아직 없어 틱을 못 받는다");
        }
    }
}

// 소켓을 쥔 시세 프로세스가 디코드한 체결을 전략 프로세스로 넘긴다. 기다리지 않는다 — 큐가 차면 버리고 센다(원칙 3).
//  [inv] 한 줄의 보내는 쪽은 그 소켓의 수신 스레드 하나다. 여기를 다른 스레드가 부르면 SPSC가 깨진다. [why D-114]
void Engine::push_feed_trade(uint32_t lane, const TradeData& trade)
{
    if (!layout_.feed().push_trade(lane, trade) &&
        feed_channel_overflow_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[Engine] 시세 통로 가득 — 체결 버리기 시작 " + trade.ticker.string() + " (전략 프로세스 정체 의심)");
    }
}

size_t Engine::feed_channel_pending_trades(uint32_t lane)
{
    return layout_.feed().pending_trades(lane);
}

uint64_t Engine::feed_channel_sent()
{
    return layout_.feed().sent_trades() + layout_.feed().sent_order_books();
}

uint64_t Engine::feed_channel_received()
{
    return layout_.feed().received_trades() + layout_.feed().received_order_books();
}

uint32_t Engine::feed_channel_lanes()
{
    return layout_.feed().lanes();
}

uint64_t Engine::fill_channel_overflows()
{
    return layout_.fills().overflow();
}

uint64_t Engine::fill_channel_discarded()
{
    return layout_.fills().discarded();
}

uint64_t Engine::fill_channel_sent()
{
    return layout_.fills().sent();
}

uint64_t Engine::fill_channel_received()
{
    return layout_.fills().received();
}

void Engine::push_feed_order_book(uint32_t lane, const OrderBook& order_book)
{
    if (!layout_.feed().push_order_book(lane, order_book) &&
        feed_channel_overflow_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[Engine] 시세 통로 가득 — 호가 버리기 시작 " + order_book.ticker.string() + " (전략 프로세스 정체 의심)");
    }
}

// 받은 호가를 이 종목을 보는 샤드 전부에 넣는다(아무도 안 보면 해시 열 하나). 버린 수를 세고 넘침이
//  시작될 때 한 번 남긴다. [why D-110]
//  부르는 쪽은 둘이다 — 한 프로세스로 돌면 수신 스레드가, 갈라 띄우면 전략 쪽 줄 스레드가 부른다. [why D-114]
void Engine::fan_out_order_book(uint32_t lane, const OrderBook& order_book)
{
    bool dropped = false;

    shard::for_each_shard(pipeline_.routes.mask(order_book.symbol_id),
                          pipeline_.order_book_matrix.consumer_of(order_book.symbol_id), [&](uint32_t consumer)
    {
        if (!pipeline_.order_book_matrix.push_to(lane, consumer, order_book))
        {
            dropped = true;
            return;
        }

        pipeline_.shards[consumer]->wake().notify();
    });

    if (dropped && order_book_drop_count_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[WS] 호가 큐 가득 — 호가 폐기 시작 " + order_book.ticker.string() + " (샤드 스레드 정체 의심)");
    }
}

// 체결도 같은 규칙. 전략 스레드가 멈추면 큐가 차고 틱이 여기서 사라진다 — 세어 두고 넘침이 시작될 때
//  한 번 남긴다(09-11 15:15 잔고 조회 정체). [why D-055]
void Engine::fan_out_trade(uint32_t lane, const TradeData& trade)
{
    bool dropped = false;

    shard::for_each_shard(pipeline_.routes.mask(trade.symbol_id), pipeline_.trade_matrix.consumer_of(trade.symbol_id),
                          [&](uint32_t consumer)
    {
        if (!pipeline_.trade_matrix.push_to(lane, consumer, trade))
        {
            dropped = true;
            return;
        }

        pipeline_.shards[consumer]->wake().notify();
    });

    if (dropped && trade_drop_count_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        LOG_WARN("[WS] 체결 큐 가득 — 틱 폐기 시작 " + trade.ticker.string() + " (샤드 스레드 정체 의심)");
    }
}

void Engine::connect_feed()
{
    // WebSocket — 동적 구독 스펙으로 연결.
    //  feed_.rest_price_feed 모드에서는 WS를 열지 않는다: KIS는 app_key당 실시간 1세션만
    //  허용하는데, 세션 정리가 서버측에 걸려 rt=9(ALREADY IN USE) 재연결 폭주가 나므로
    //  체결 피드를 REST 현재가 폴링(data_thread_fn)으로 대체하고 WS 의존을 제거한다.
    //  주문은 REST(order_thread_fn)로 나가므로 매매에는 영향 없음(체결통보 on_fill만 없음).
    //  구독할 종목이 없어도 hts_id 가 있으면 열어둔다 — 체결통보(H0STCNI)는 종목 구독과
    //  별개라, 유니버스가 비었다고 닫아버리면 이월 보유분을 청산하는 주문의 체결을 못 듣고
    //  원장이 빈다. 2026-09-23 실계좌 첫날 청산 체결이 이렇게 사라졌다. [why D-097]
    //  갈라 띄울 때도 같다 — 구독 목록은 전략 쪽에서 요청으로 오므로 시세만 맡은 프로세스는
    //  목록이 비어도 소켓을 열어 둔다. 그때 소켓이 없으면 걸 곳이 없다. [why D-114]
    if (feed_.rest_price_feed ||
        (watch_specifications_.empty() && kis_config_.hts_id.empty() && role_ != ProcessRole::Feed))
    {
        // 안 열었다는 것도 남긴다 — 이 줄이 없으면 건강 점검은 체결통보가 끈겼는지를 못 가른다.
        LOG_INFO(std::string("[Engine] 체결통보 세션: 없음(") +
                 (feed_.rest_price_feed ? "REST 시세 모드라 WS를 열지 않는다"
                                        : "구독 종목 0개·hts_id 비었다") +
                 ")");
        return;
    }

    if (feed_.feed_override)
    {
        feed_.websocket = std::move(feed_.feed_override);
        LOG_INFO("[Engine] 주입된 피드 소스(수신 스레드 " + std::to_string(feed_.websocket->lanes()) + "개)");
    }
    else if (!feed_.replay_file.empty())
    {
        feed_.websocket = std::make_unique<feed::ReplaySource>(utf8::path_from_utf8(feed_.replay_file), feed_.replay_speed);
        LOG_INFO("[Engine] 리플레이 소스: " + feed_.replay_file + " (speed " + std::to_string(feed_.replay_speed) + ")");
    }
    else if (feed_.extra_feed_cfgs.empty())
    {
        feed_.websocket = std::make_unique<KisWebSocket>(kis_config_);
        LOG_INFO(std::string("[Engine] 체결통보 세션: ") + (kis_config_.hts_id.empty() ? "없음(hts_id가 비어 있다)" : "소켓 0"));
    }
    else
    {
        // 소켓 여럿 — 첫 소스가 기본 키다. 체결통보는 hts_id를 가진 소켓 하나가 맡는다(자리가 아니라 설정이 정한다,
        //  D-114 단계 3). 직접 호출 모드라 소켓 i의 수신 스레드가
        //  행렬의 행 i에 직접 넣는다(multiplexer 스레드 없음). 소켓이 하나면 FeedMux를 끼우지 않는다.
        std::vector<std::unique_ptr<feed::IFeedSource>> socks;
        socks.push_back(std::make_unique<KisWebSocket>(kis_config_));

        for (const auto& extra_feed_config : feed_.extra_feed_cfgs)
        {
            socks.push_back(std::make_unique<KisWebSocket>(extra_feed_config));
        }

        auto                      multiplexed  = std::make_unique<feed::FeedMux>(std::move(socks));
        const std::vector<size_t> fill_sources = multiplexed->fill_notice_sources();
        feed_.websocket                        = std::move(multiplexed);
        LOG_INFO("[Engine] WS 소켓 " + std::to_string(feed_.extra_feed_cfgs.size() + 1) + "개를 FeedMux 수신 스레드 " +
                 std::to_string(pipeline_.websocket_lanes) + "개로 묶는다");

        if (fill_sources.size() > 1)
        {
            LOG_ERROR("[Engine] 체결통보 세션: " + std::to_string(fill_sources.size()) +
                      "개 — 하나만 맡아야 한다. KIS는 세션마다 같은 통보를 보내 원장이 체결을 두 번 센다");
        }
        else if (fill_sources.empty())
        {
            LOG_INFO("[Engine] 체결통보 세션: 없음(hts_id가 비어 있다)");
        }
        else
        {
            LOG_INFO("[Engine] 체결통보 세션: 소켓 " + std::to_string(fill_sources.front()));
        }
    }

    // 리플레이를 다시 캡처하면 같은 틱이 두 파일에 남으므로 캡처는 WS일 때만 연다.
    if (!feed_.capture_directory.empty() && feed_.replay_file.empty())
    {
        // 파일명은 UTC 기동 시각 — 재기동이 같은 파일에 이어 쓰지 않도록.
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const std::string           file_name = "ticks_" + std::to_string(now_s) + ".bin";
        const std::filesystem::path file      = utf8::path_from_utf8(feed_.capture_directory) / file_name;
        feed_.capture = std::make_unique<feed::TickCapture>(file);

        if (feed_.capture->ok())
        {
            LOG_INFO("[Engine] 틱 캡처 시작: " + feed_.capture_directory + "/" + file_name);

            // 그날 무엇을 구독했는지를 파일 머리에 남긴다 — 호가가 비어 있는 종목이 trade_only인지 파일만 보고 알 수 있게.
            for (const auto& specification : watch_specifications_)
            {
                feed_.capture->on_universe(specification.ticker, static_cast<uint8_t>(specification.market),
                                           symbols_.table.intern(specification.ticker), specification.trade_only);
            }
        }
        else
        {
            LOG_WARN("[Engine] 틱 캡처 파일을 열지 못해 캡처 없이 간다: " + feed_.capture_directory + "/" + file_name);
        }
    }

    // 수신 스레드 = 이 콜백을 부르는 수신 스레드 번호 = 행렬의 행. 한 행은 그 스레드만 넣는다(SPSC 셀, 원칙 5).
    feed_.websocket->set_lane_callbacks([this](uint32_t lane, const OrderBook& in)
                       {
                           OrderBook order_book = in;
                           order_book.symbol_id       = symbols_.table.intern(order_book.ticker);

                           // 번호가 안 붙은 호가는 접는다 — 체결 줄과 같은 규칙이다(아래 체결 콜백). [why D-114 단계 5]
                           if (order_book.symbol_id == symbol::kNone)
                           {
                               return;
                           }

                           // 수신 스레드가 디코드 시점에 찍은 값을 지킨다. 안 찍힌 소스만 여기서 찍는다.
                           if (order_book.received_ns == 0)
                           {
                               order_book.received_ns = trace::now_ns();
                           }

                           if (feed_.capture)
                           {
                               feed_.capture->on_book(order_book);
                           }

                           // 갈라 띄우면 소켓을 쥔 쪽은 통로에 넣기까지만 한다 — 샤드도 전략도 저쪽에 있다(원칙 3). [why D-114]
                           if (role_ == ProcessRole::Feed)
                           {
                               push_feed_order_book(lane, order_book);
                               return;
                           }

                           fan_out_order_book(lane, order_book);
                       },
                       [this](uint32_t lane, const TradeData& in)
                       {
                           // 트리비얼 복사 타입이라 이동이 곧 복사다 — 여기서 한 번 복사해 id를 찍고 push가 셀에 한 번 더 베낀다. 수신 시각은
                           //  수신 스레드가 디코드 시점에 찍은 값을 지키고, 안 찍힌 소스만 여기서 찍는다.
                           TradeData trade = in;
                           trade.symbol_id       = symbols_.table.intern(trade.ticker);

                           // 번호가 안 붙었으면 이 줄을 접는다. 시세 역할은 표에 없는 티커를 버리고 세고
                           //  (청하지 않은 종목이 세션에 실려 온 것이다), 전략 역할은 주문 쪽이 제때 안 달아
                           //  준 때다. 그대로 보내면 통로 저쪽이 못 알아보고, 샤드로 가면 번호 없는 값이
                           //  남의 샤드를 깨운다. 센 것에도 넣지 않는다 — 못 알아본 줄은 받은 시세가 아니다.
                           //  [why D-114 단계 5]
                           if (trade.symbol_id == symbol::kNone)
                           {
                               return;
                           }

                           // 받은 체결을 센다. 예전엔 REST 폴링 경로(data_thread_fn)에서만 올려서, WS로만 도는
                           //  구성(DevScale 27종목)에서는 HEALTH의 data가 늘 0이었다 — 그라파나 "초당 틱 처리량"이
                           //  항상 0선이고, 이 값이 줄어드는 것으로 재기동을 세는 패널도 영영 0이었다.
                           data_count_.fetch_add(1, std::memory_order_relaxed);

                           if (trade.received_ns == 0)
                           {
                               trade.received_ns = trace::now_ns();
                           }

                           if (feed_.capture)
                           {
                               feed_.capture->on_trade(trade);
                           }

                           // 모의 체결은 틱 스레드에서 낸다(feed_.paper는 리플레이·피드 주입 전용). 수신 스레드가
                           //  여럿이면 여기가 동시에 불리므로 체결기가 전달을 한 줄로 세운다(PaperExecutor::on_tick, W-6).
                           if (feed_.paper)
                           {
                               feed_.paper->on_tick(trade);
                           }

#ifdef HAS_ZMQ
                           // 발행은 팬아웃보다 먼저 한다 — 샤드 큐가 차서 돌아가던 예전 순서에서는 정체 때
                           //  그라파나까지 같이 멎었다. [why D-114]
                           if (zmq_bridge_)
                           {
                               zmq_bridge_->publish_trade(trade);
                           }
#endif

                           if (role_ == ProcessRole::Feed)
                           {
                               push_feed_trade(lane, trade);
                               return;
                           }

                           fan_out_trade(lane, trade);
                       });
    // 갈라 띄운 날의 체결통보는 이 프로세스 것이 아니다 — 소켓이 시세로 오면서 체결통보도 같이 따라왔고,
    //  원장을 쥔 쪽은 주문이다. 그래서 시세 역할일 때만 공유 체결 통로로 넘긴다. 한 프로세스로 돌면
    //  예전처럼 프로세스 안 큐로 가 홉이 늘지 않는다. [why D-114 단계 5]
    const bool fill_crosses_boundary = role_ == ProcessRole::Feed;

    auto push_fill = [this, fill_crosses_boundary](const FillNotification& fill_notification)
                           {
                               // 넣는 쪽은 한 번에 하나다(pipeline_.fill_producing 설명). 겹치면 뒤에 온 쪽이
                               //  기다려 한 줄로 서고 센다 — 그대로 넣으면 SPSC 큐의 칸 번호가 어긋나 체결이 사라진다.
                               if (pipeline_.fill_producing.exchange(true, std::memory_order_acquire))
                               {
                                   const auto count = pipeline_.fill_producer_overlap.fetch_add(1, std::memory_order_relaxed) + 1;
                                   LOG_ERROR("[Engine] 체결통보 생산자 겹침 — 두 스레드가 같이 넣으려 했다 ODNO=" +
                                             fill_notification.kis_order_no + " (누적 " + std::to_string(count) + "건, 기대 0)");

                                   while (pipeline_.fill_producing.exchange(true, std::memory_order_acquire))
                                   {
                                       std::this_thread::yield();
                                   }
                               }

                               struct ProducerTurn
                               {
                                   std::atomic<bool>& producing;

                                   ~ProducerTurn()
                                   {
                                       producing.store(false, std::memory_order_release);
                                   }
                               };

                               const ProducerTurn producer_turn{pipeline_.fill_producing};

                               // 두 길 모두 문자열 없는 레코드로 옮겨 넣는다 — 큐 칸에 힙 문자열이 있으면 넣고 뺄 때마다
                               //  할당·해제가 수신 스레드에서 일어난다. 옮기는 일은 여기서 한 번이다. [why CODE_REVIEW W-7]
                               bool           truncated = false;
                               const uint64_t sequence  = pipeline_.fill_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
                               const auto     notice    = ipc::to_notice(fill_notification, sequence, trace::now_ns(), &truncated);

                               if (truncated)
                               {
                                   // 잘린 주문번호로는 취소·정정을 증권사에 되돌려 줄 수 없다. 넘기기는 하되 남긴다.
                                   LOG_ERROR("[Engine] 체결통보 칸이 모자라 글자가 잘렸다 — " + fill_notification.ticker +
                                             " ODNO=" + fill_notification.kis_order_no);
                               }

                               if (fill_crosses_boundary)
                               {
                                   if (!layout_.fills().push(notice))
                                   {
                                       const auto count = pipeline_.fill_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
                                       LOG_ERROR("[Engine] 체결 통로 가득 참 — 드롭 " + fill_notification.ticker + " ODNO=" +
                                                 fill_notification.kis_order_no + " (누적 " + std::to_string(count) +
                                                 "건) 주문 쪽 예약 수량이 안 풀린다");
                                   }

                                   return;
                               }

                               // 수신 스레드는 큐에 넣고 바로 돌아간다. 가득 찼으면(1024건 밀림 = 소비자가 멈춘 것)
                               //  기다리지 않고 버린다 — 여기서 대기하면 전 종목 틱이 같이 선다. 버린 건은
                               //  잔고 대조(control_thread)가 원장에 메운다. [why D-056]
                               if (!pipeline_.fill_queue.push(notice))
                               {
                                   const auto count = pipeline_.fill_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
                                   LOG_ERROR("[Engine] 체결통보 큐 가득 참 — 드롭 " + fill_notification.ticker + " ODNO=" + fill_notification.kis_order_no +
                                             " (누적 " + std::to_string(count) + "건)");
                                   return;
                               }

                               pipeline_.fill_wake.notify();
                           };
    feed_.websocket->set_fill_callback(push_fill);

    if (feed_.paper)
    {
        feed_.paper->set_fill_callback(push_fill);
    }

    if (!feed_.websocket->connect(watch_specifications_))
    {
        // 예전에는 경고만 남기고 넘어갔는데, 그러면 전략이 호가·체결을 하나도 못 받아
        //  매매가 조용히 멈춘다(폴링 경로가 꺼져 있으므로). 폴링으로 낮춰 계속 돈다.
        //  control_thread가 재연결을 계속 시도하고, 붙으면 WS로 되돌린다.
        LOG_ERROR("[Engine] WebSocket 최초 연결 실패");

        if (!activate_rest_fallback("최초 연결 실패"))
        {
            LOG_ERROR("[Engine] 폴링 폴백도 불가(시세 소스 없음) — 호가/체결 이벤트 없이 동작");
        }
    }
}

void Engine::spawn_threads()
{
    // 전략 스레드가 돌기 전에 사본을 한 판 내 둔다. 재기동 직후 잔고 재시드로 보유가 이미 들어와 있는데
    //  전략이 빈 판을 보면 "보유 0"으로 읽고 같은 종목을 또 산다(이중 발주, A등급). 첫 주문이 들어와야
    //  첫 판이 나가는 구조라 여기서 한 번 먼저 낸다. [why D-114]
    if (runs_order_side())
    {
        order_gate_.publish_ledger(*ledger_snapshot_);
    }

    // 데이터 스레드는 양쪽에 하나씩 둔다 — 보는 일감이 다르다. 전략 쪽은 국면·재스캔·시세 보충이고,
    //  주문 쪽은 원장 대조·선점 정리·하루 초기화다(가르는 선은 docs/DECISIONS.md D-114). [why D-114]
    // jthread는 stop_token을 첫 인자로 넣으므로 멤버 함수 포인터(this가 첫 인자)는 람다로 감싼다.
    data_thread_ = std::jthread([this](std::stop_token stop_token) { data_thread_fn(stop_token); });

    if (runs_strategy_side())
    {
        // 소켓이 저쪽에 있는 전략 프로세스만 줄 스레드를 띄운다. 한 프로세스로 돌면 수신 스레드가 곧바로
        //  샤드에 넣으므로 이 홉이 없다 — 갈라 띄우는 날에만 한 홉이 는다. [why D-114]
        if (role_ == ProcessRole::Strategy)
        {
            for (uint32_t lane = 0; lane < layout_.feed().lanes(); ++lane)
            {
                feed_lane_threads_.emplace_back([this, lane](std::stop_token stop_token) { feed_lane_thread_fn(stop_token, lane); });
            }

            LOG_INFO("[Engine] 시세 줄 스레드 " + std::to_string(feed_lane_threads_.size()) + "개 — 통로에서 꺼내 샤드로 나눈다");
        }

        for (uint32_t shard_index = 0; shard_index < static_cast<uint32_t>(pipeline_.shards.size()); ++shard_index)
        {
            pipeline_.shard_threads.emplace_back([this, shard_index](std::stop_token stop_token) { shard_thread_fn(stop_token, shard_index); });
        }

        strategy_thread_ = std::jthread([this](std::stop_token stop_token) { strategy_thread_fn(stop_token); });
    }

    if (runs_order_side())
    {
        order_thread_ = std::jthread([this](std::stop_token stop_token) { order_thread_fn(stop_token); });
        fill_thread_  = std::jthread([this](std::stop_token stop_token) { fill_thread_fn(stop_token); });
    }

    // 감시 스레드도 양쪽에 하나씩 — 시세 소켓을 쥔 쪽이 재연결을 보고, 주문 쪽이 마감 종료를 본다.
    control_thread_ = std::jthread([this](std::stop_token stop_token) { control_thread_fn(stop_token); });
}

void Engine::start()
{
    if (running_.load())
    {
        return;
    }

    // 자리표가 없으면 장부 사본도 박동도 응답 큐도 없다 — 그 상태로는 뜨지 않는다. [why D-114]
    if (!layout_.is_bound())
    {
        LOG_ERROR("[Engine] 자리표 없이는 뜨지 않는다 — " + std::string(layout_.last_error()));
        return;
    }

    start_was_called_.store(true, std::memory_order_release);

    LOG_INFO("[Engine] ── 퀀트 엔진 시작 ──────────────────────────────");

    // 자리표부터 맞춘다. 뒤에 오는 setup_shards 가 전략이 다루는 종목을 표에 올리는데, 그때 표가
    //  아직 힙 것이면 거기 찍힌 번호가 곧 버려진다 — 갈라 띄운 쪽은 그 번호로 시세를 못 알아본다.
    //  소켓 수는 setup_shards 가 쓰는 것과 같은 셈(websocket_lane_count)이라 먼저 물어도 된다.
    //  줄 하나를 더 둔다 — 마지막 줄은 구독 상한에 밀린 종목을 REST로 대신 흘리는 자리다(넣는 쪽은
    //  시세 역할 프로세스의 데이터 스레드 하나). 행렬이 데이터 스레드 행을 따로 두는 것과 같은 모양이다. [why D-114]
    const uint32_t feed_lane_count = websocket_lane_count() + 1;

    if (layout_config_.feed_lanes != feed_lane_count)
    {
        // 쪽지를 이미 열었으면 여기서 다시 깔면 안 된다 — 쪽지 위 종목 표·전략 이름표가 0으로 밀려,
        //  설정을 읽고 전략을 올리며 찍어 둔 번호 수백 개가 통째로 사라진다. configure()가 같은 셈으로
        //  미리 깔아 두므로 여기서 갈리는 것은 그 뒤에 피드가 더 붙었다는 뜻이다 — 조용히 넘기지 않는다.
        //  아직 안 열었으면(설정을 안 읽고 역할만 준 길) 여기서 처음 여는 것이라 잃을 번호가 없다. [why D-114]
        if (layout_region_.is_open())
        {
            LOG_ERROR("[Engine] 시세 줄 수가 설정을 읽을 때와 다르다(" + std::to_string(layout_config_.feed_lanes) +
                      "→" + std::to_string(feed_lane_count) + ") — 갈라 띄운 채로는 다시 깔 수 없어 뜨지 않는다");
            return;
        }

        if (!bind_layout(feed_lane_count))
        {
            LOG_ERROR("[Engine] 시세 줄 " + std::to_string(feed_lane_count) + "개로 자리표를 다시 못 깔았다");
            return;
        }
    }

    // 틱 파이프라인 자리는 역할에 상관없이 그대로 깐다. 샤드 스레드를 띄우는 것은 전략 역할뿐이라
    //  (spawn_threads) 주문·시세 쪽에서는 아무도 돌지 않지만, 자리는 아직 잡는다 — 떼는 것은 남은 일이다
    //  (docs/DECISIONS.md D-114 단계 5 "남은 것"). [why D-114 단계 5]
    setup_shards();

    // [inv] 자리표를 깔 때 쓴 셈과 샤드가 실제로 연 줄 수는 같아야 한다 — 다르면 통로의 줄 자리가
    //  어긋나 건너편이 남의 줄을 읽는다.
    if (pipeline_.websocket_lanes + 1 != layout_config_.feed_lanes)
    {
        LOG_ERROR("[Engine] 시세 줄 수가 자리표와 어긋난다(" + std::to_string(layout_config_.feed_lanes) + "→" +
                  std::to_string(pipeline_.websocket_lanes + 1) + ") — 통로 자리가 밀려 뜨지 않는다");
        return;
    }

#ifdef HAS_ZMQ
    // 발행 채널은 역할마다 하나씩 둔다 — 시세는 체결을, 전략은 신호를, 주문은 주문·체결통보를 낸다.
    //  제어(KILL·잔고 조회)는 주문 쪽 포트에만 붙어 전략이 멎어도 살아 있다. [why D-114]
    if (zmq_enabled_)
    {
        setup_zmq_bridge();
    }
#endif

    const bool offline = feed_.feed_override != nullptr || !feed_.replay_file.empty();

    // 인증은 양쪽이 한다 — 주문 쪽은 주문·잔고에, 전략 쪽은 시세·일봉 조회에 REST를 쓴다.
    if (!authenticate_feed(offline))
    {
        return;
    }

    // REST 폴러는 역할마다 둔다 — 전략 쪽은 유니버스·일봉·시세 보충에 쓰고, 시세 쪽은 WS 구독 상한에 밀린
    //  종목의 대체 시세와 WS가 죽었을 때의 폴백에 쓴다. 넘침 목록은 소켓을 쥔 쪽에만 있어 폴링도
    //  그쪽이 해야 한다. [why D-114 단계 5]
    initialize_data_poller();

    if (runs_order_side())
    {
        setup_paper_executor(offline);
        initialize_order_router();
        initialize_ledger_reconciler();

        // 원장 파일·미결주문 파일은 한 프로세스만 연다 — 둘이 같은 파일을 쓰면 줄이 섞인다. [why D-114]
        if (!try_open_ledger_journal() || !try_bootstrap_ledger())
        {
            return;
        }

        resolve_open_intents();
    }

    if (runs_strategy_side())
    {
        // 프리페치 스레드는 전략이 붙기 전에 미리 띄운다 — 장중 전략 등록이 스레드를 새로 만들지 않게. [why D-115]
        prefetch_pool_.start();

        start_strategies();
        collect_watch_specifications();
    }

    running_.store(true);

    // 런타임 피드 상태를 config 의도로 초기화. 이후 WS 생사에 따라 control_thread가 토글한다.
    feed_.rest_feed_active.store(feed_.rest_price_feed, std::memory_order_relaxed);

    // 시세 소켓은 시세 쪽이 쥔다. 앱키 하나에 실시간 세션 하나라 소켓도 하나뿐이고, 체결통보(H0STCNI)가
    //  시세와 같은 세션에 실린다 — 그래서 체결통보가 시세 → 주문으로 경계를 넘는다. 단계 4까지는 그 반대였다
    //  (소켓을 주문에 두어 체결통보를 안 넘겼다). 앱키를 더 딸 수 없어 뒤집었다. [why D-114 단계 5]
    if (runs_feed_side())
    {
        connect_feed();
    }

    spawn_threads();

    LOG_INFO("[Engine] 모든 스레드 시작 완료");
}

void Engine::set_role(ProcessRole role)
{
    role_ = role;
}

// 전략에 주는 매도가능수량. 게이트 clamp와 같은 식(possible_quantity_cap - pending)이라 전략이 낸 수량이 게이트에서
//  다시 잘리지 않는다. psbl_cap은 잔고 대조(refresh_sellable)가 매 사이클 맞춘다. [why D-055]
//  그 셈은 이제 사본을 낼 때 주문 쪽이 한다 — 여기서는 한 판에서 매도가능과 평단을 함께 읽기만 한다.
//  따로 읽으면 그 사이에 판이 바뀌어 "매도가능 3인데 평단 0" 같은 조합을 본다. [why D-114]
StrategyBase::SellableInfo Engine::ledger_sellable(const std::string&, const std::string& ticker) const
{
    const ipc::LedgerRow row = ledger_snapshot_->row(symbols_.table.lookup(ticker));
    StrategyBase::SellableInfo sellable_info;
    sellable_info.sellable      = row.sellable;
    sellable_info.average_price = row.average_price;
    return sellable_info;
}

// 전략이 보는 보유 수량 — 문자열 티커를 종목 번호로 한 번 바꿔 사본에서 읽는다. 모르는 종목은 번호가
//  kNone이라 빈 줄이 오고, 보유 0이 된다(등록하지 않는다 — 읽기만 하는 자리다).
int Engine::ledger_position(const std::string& ticker) const
{
    return ledger_snapshot_->row(symbols_.table.lookup(ticker)).position;
}

void Engine::request_shutdown(std::string_view reason, ipc::SharedShutdownReason recorded_reason)
{
    // 이미 내려가는 중이면 사유를 다시 적지 않는다 — stop()이 KILL 뒤에 한 번 더 부른다.
    if (running_.exchange(false, std::memory_order_acq_rel))
    {
        LOG_WARN("[Engine] 종료 요청 — " + std::string(reason));
        // 쪽지에 남길 사유도 먼저 부른 쪽 것으로 굳힌다. 적는 것은 stop() 끝이다. [why D-114]
        shutdown_reason_.store(static_cast<uint32_t>(recorded_reason), std::memory_order_relaxed);
    }

    for (std::jthread* thread : {&data_thread_, &strategy_thread_, &order_thread_, &fill_thread_, &control_thread_})
    {
        thread->request_stop(); // WakeGate의 stop_token 대기와 sleep_unless_stopped가 여기서 깬다
    }

    for (auto& shard_thread : pipeline_.shard_threads)
    {
        shard_thread.request_stop();
    }

    for (auto& feed_lane_thread : feed_lane_threads_)
    {
        feed_lane_thread.request_stop();
    }
}

void Engine::stop()
{
    // 정지 요청과 회수를 나눈다. KILL이 먼저 running_을 내렸어도 join은 여기서 한다 — 예전엔 running_ exchange로
    //  조기 반환해 KILL 뒤 소멸 경로가 join 없이 std::thread를 부쉈다(joinable이면 terminate). 회수는 한 번만: 두 번째
    //  호출은 joinable이 없어 돌아간다.
    request_shutdown("stop() 호출(main 종료 경로)");

    if (!data_thread_.joinable())
    {
        // 회수할 스레드가 없다 — 기동하다 접었거나 이미 한 번 내려갔다. 앞이면 여기서 사유를 적어야
        //  다음 기동이 크래시로 읽지 않는다. 뒤면 먼저 적힌 사유가 그대로 남는다. [why D-114]
        layout_region_.mark_clean_shutdown(ipc::SharedShutdownReason::kStartupFail);
        return;
    }

    LOG_INFO("[Engine] 종료 시작");

    // 역순 join 권장: control → order → strategy → data. fill_thread는 WS를 끊은 뒤에 join한다(아래).
    if (control_thread_.joinable())
    {
        control_thread_.join();
    }

    if (order_thread_.joinable())
    {
        order_thread_.join();
    }

    // 줄 스레드가 먼저 — 줄이 넣던 시세를 샤드가 비운 뒤 선다(샤드 행렬의 생산자가 이 스레드다).
    for (auto& feed_lane_thread : feed_lane_threads_)
    {
        if (feed_lane_thread.joinable())
        {
            feed_lane_thread.join();
        }
    }

    // 그 다음 샤드 — 샤드가 넣던 봉투를 전략 스레드가 비운 뒤 선다.
    for (auto& shard_thread : pipeline_.shard_threads)
    {
        if (shard_thread.joinable())
        {
            shard_thread.join();
        }
    }

    if (strategy_thread_.joinable())
    {
        strategy_thread_.join();
    }

    if (data_thread_.joinable())
    {
        data_thread_.join();
    }

    if (feed_.websocket)
    {
        feed_.websocket->disconnect();
    }

    // disconnect()가 수신 스레드를 join하므로 이 뒤로는 push가 없다. fill_thread는 큐가 빌 때까지 돌고 끝난다.
    if (fill_thread_.joinable())
    {
        fill_thread_.join();
    }

#ifdef HAS_ZMQ
    if (zmq_bridge_)
    {
        zmq_bridge_->stop();
    }
#endif

    if (ops_.server)
    {
        ops_.server->stop();
    }

    for (auto& strategy : strategy_.list)
    {
        strategy->on_stop();
    }

    reap_retired(/*force=*/true);
    prefetch_pool_.stop(); // 전략이 전부 자기 작업을 뗀 뒤 스레드를 접는다
    print_statistics();

    // 여기까지 왔으면 스레드를 다 회수한 깨끗한 종료다. 쪽지에 사유를 적어 둬야 짝과 다음 기동이
    //  크래시와 가른다 — 안 적힌 0 이 크래시다. 만든 쪽(주문 프로세스)만 적힌다. [why D-114]
    layout_region_.mark_clean_shutdown(
        static_cast<ipc::SharedShutdownReason>(shutdown_reason_.load(std::memory_order_relaxed)));
    LOG_INFO("[Engine] 종료 완료");
}

// 제어 요청 한 줄을 전략 스레드가 옮겨 줄 통로에 싣는다. 순번과 보낸 시각은 여기서 찍는다. 통로가 가득이면
//  거짓을 돌려주고 버린 수(control_dropped)만 센다 — 표를 보내는 쪽이 그 판을 접는다. [why D-114]
// 스캔 스레드가 슬리브마다 부른다(20초 간격). 파일은 임시 이름으로 쓰고 바꿔치기해
//  대시보드가 반쯤 쓰인 JSON을 읽지 않게 한다. 쓰기 실패는 매매와 무관하므로 경고만 남긴다.
void Engine::set_entry_priority(const std::vector<OrderGate::PriorityEntry>& entries, int total)
{
    nlohmann::json scores = nlohmann::json::object();

    for (const auto& entry : entries)
    {
        scores[symbols_.table.name(entry.symbol).string()] = {{"rank", entry.rank}, {"z", entry.z_score}};
    }

    // 표를 게이트에 바로 걸지 않고 주문 스레드로 보낸다 — 표를 고치는 것은 단일 시퀀서다(원칙 4).
    //  부르는 쪽이 스캔 스레드라 프로세스가 갈리면 이 자리가 통째로 막힌다. [why D-114]
    {
        ipc::ControlRequest open;
        open.kind = ipc::ControlKind::kEntryPriorityBegin;

        if (send_control(open))
        {
            uint32_t sent = 0;
            bool     full = false;

            for (const OrderGate::PriorityEntry& entry : entries)
            {
                ipc::ControlRequest row;
                row.kind      = ipc::ControlKind::kEntryPriorityEntry;
                row.batch     = open.sequence;
                row.symbol_id = entry.symbol;
                row.rank      = entry.rank;
                row.score_z   = entry.z_score;

                if (!send_control(row))
                {
                    full = true;
                    break;
                }

                ++sent;
            }

            if (full)
            {
                LOG_WARN("[Engine] 진입 우선순위 표를 보내다 통로가 가득 찼다 — 이번 판을 접는다(보낸 " +
                         std::to_string(sent) + "줄)");
            }
            else
            {
                ipc::ControlRequest close;
                close.kind      = ipc::ControlKind::kEntryPriorityCommit;
                close.batch     = open.sequence;
                close.rank      = total;
                close.row_count = sent;

                if (!send_control(close))
                {
                    LOG_WARN("[Engine] 진입 우선순위 표 마무리를 못 보냈다 — 이번 판은 걸리지 않는다");
                }
            }
        }
        else
        {
            LOG_WARN("[Engine] 진입 우선순위 " + std::to_string(entries.size()) +
                     "종목 — 제어 통로가 가득 차 이번 판을 접는다");
        }
    }

    static std::mutex file_mutex; // 두 슬리브가 겹쳐 불러도 파일은 한 번에 하나만 쓴다
    std::lock_guard<std::mutex> lock(file_mutex);
    const auto path = Logger::instance().base_directory() / "entry_scores.json";
    const auto temporary  = Logger::instance().base_directory() / "entry_scores.json.tmp";
    std::error_code error_code;
    {
        std::ofstream output_file(temporary, std::ios::trunc);

        if (!output_file.is_open())
        {
            LOG_WARN("[Engine] entry_scores.json 쓰기 실패: " + temporary.string());
            return;
        }

        const auto now = std::chrono::system_clock::now();
        output_file << nlohmann::json{{"ts", std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()},
                             {"total", total},
                             {"unscored_z", order_gate_.config().displace_unscored_z},
                             {"scores", std::move(scores)}}
                  .dump();
    }

    std::filesystem::rename(temporary, path, error_code);

    if (error_code)
    {
        LOG_WARN("[Engine] entry_scores.json 교체 실패: " + error_code.message());
    }
}

// ─── 시세 줄 스레드(전략 역할) ────────────────────────────────────────────
void Engine::feed_lane_thread_fn(std::stop_token stop_token, uint32_t lane)
{
    thread_name::set_current("Feed " + std::to_string(lane));
    LOG_INFO("[Feed " + std::to_string(lane) + "] 시작 — 통로에서 꺼내 샤드로 나눈다");

    // 꺼낸 칸이 말이 되는지 보는 기준. 종목 표는 기동 때 다 차고 장중 재스캔이 뒤에 더 붙일 수 있어
    //  줄 한 바퀴마다 다시 읽는다 — 새로 등록된 종목의 시세를 "번호가 표 밖"이라고 버리지 않도록. [why D-114]
    ipc::MarketLimits limits;

    // 유휴 전이: 샤드 스레드와 같은 정책이되 게이트가 없다 — 건너편은 다른 프로세스라 깨울 수 없다. [why D-071]
    //  놀린 뒤에도 빈 채면 짧게 잔다 — 타이머 격자를 2ms로 내려 둔 위에서(main.cpp timeBeginPeriod) 시세 지연을 1ms 밑으로 둔다.
    constexpr auto                        kSpinBudget = std::chrono::microseconds(200);
    constexpr auto                        kIdleSleep  = std::chrono::microseconds(500);
    std::chrono::steady_clock::time_point idle_since{};

    TradeData trade;
    OrderBook order_book;

    // 호가 행렬은 WS 줄 수만큼만 행이 있다 — 마지막 REST 대체 줄(lane == WS 줄 수)은 체결만 흘린다. 그 줄에서 꺼낸
    //  호가는 넘길 행이 없으니 버린다(꺼내기는 한다 — 안 꺼내면 통로 칸이 찬다). [why D-114]
    const bool lane_has_order_book_row = lane < pipeline_.order_book_matrix.producers();

    while (!stop_token.stop_requested())
    {
        limits.symbol_count = static_cast<uint32_t>(symbols_.table.size());

        bool did_work = false;

        // 체결을 먼저 비우고 호가를 비운다 — 둘은 다른 큐라 순서에 걸린 규칙이 없다(종목 안 순서는 큐가 지킨다).
        while (layout_.feed().pop_trade(lane, limits, trade))
        {
            data_count_.fetch_add(1, std::memory_order_relaxed);
            fan_out_trade(lane, trade);
            did_work = true;
        }

        while (layout_.feed().pop_order_book(lane, limits, order_book))
        {
            if (lane_has_order_book_row)
            {
                fan_out_order_book(lane, order_book);
            }

            did_work = true;
        }

        // 말이 안 돼 버린 수는 꺼내는 쪽만 안다 — 감시 스레드가 읽을 자리에 옮겨 둔다. 한 건도 못 건진 바퀴에도
        //  옮긴다 — 번호 표가 반쪽이면 꺼내는 족족 버려 did_work가 계속 거짓이고, 그때가 바로 봐야 할 때다. [why D-114]
        if (const uint64_t discarded_now = layout_.feed().discarded();
            discarded_now != feed_channel_discarded_.load(std::memory_order_relaxed))
        {
            feed_channel_discarded_.store(discarded_now, std::memory_order_relaxed);
        }

        if (did_work)
        {
            idle_since = std::chrono::steady_clock::time_point{};
            continue;
        }

        const auto now_idle = std::chrono::steady_clock::now();

        if (idle_since == std::chrono::steady_clock::time_point{})
        {
            idle_since = now_idle;
        }

        if (now_idle - idle_since < kSpinBudget)
        {
            std::this_thread::yield();
            continue;
        }

        wake::sleep_unless_stopped(stop_token, kIdleSleep);
    }

    LOG_INFO("[Feed " + std::to_string(lane) + "] 종료");
}

void Engine::set_last_active_regimes(const std::vector<Regime>& last_active_regimes)
{
    if (!strategy_.list.empty())
    {
        strategy_.list.back()->set_active_regimes(last_active_regimes);
    }
}

void Engine::set_session_end(int close_min, int grace_sec)
{
    session_end::Config config;
    config.close_min = close_min;
    config.grace_sec = grace_sec;
    session_end_ = session_end::Judge(config);
}

void Engine::set_universe_rescan(std::function<std::vector<symbol::SymbolId>(KisClient&)> universe_fn,
                                 std::function<std::unique_ptr<StrategyBase>(symbol::SymbolId)> factory,
                                 int interval_sec, size_t max_registered, int drop_after_sec, int block_after_sec,
                                 int return_confirm)
{
    // 슬리브마다 한 번씩 부른다 — 덮어쓰지 않고 쌓는다. 예전에는 단일 슬롯이라
    //  두 번째 호출이 첫 번째를 조용히 지웠다(먼저 건 재스캔이 사라짐).
    RescanJob rescan_job;
    rescan_job.universe_fn = std::move(universe_fn);
    rescan_job.factory = std::move(factory);
    rescan_job.owned.resize(symbols_.table.capacity()); // 종목 id 인덱스 — id는 용량을 넘지 않는다
    rescan_job.interval_sec = interval_sec;
    rescan_job.max_registered = max_registered;
    rescan_job.drop_after_sec = drop_after_sec;
    rescan_job.block_after_sec = block_after_sec;
    rescan_job.return_confirm = return_confirm;
    universe_rescan_.jobs.push_back(std::move(rescan_job));
}

void Engine::set_zmq_control(const std::string& bind_address, const std::string& token)
{
    if (!bind_address.empty())
    {
        zmq_bind_address_ = bind_address;
    }

    zmq_control_token_ = token;
}

void Engine::set_protective_orders(const std::string& mode, int interval_ms, int retry_ms)
{
    protective_book_.set_mode(risk::protective_mode_from_string(mode));
    protective_interval_ = std::chrono::milliseconds(interval_ms > 0 ? interval_ms : kProtectiveIntervalMsDefault);
    protective_book_.set_retry_interval(std::chrono::milliseconds(retry_ms > 0 ? retry_ms : kProtectiveRetryMsDefault));
}

void Engine::mark_exit_managed(symbol::SymbolId symbol)
{
    if (symbol != symbol::kNone && symbol < symbols_.exit_managed.size())
    {
        symbols_.exit_managed[symbol] = true;
    }
}
