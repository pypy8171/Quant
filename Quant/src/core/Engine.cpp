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

    // 장부 키의 종목 번호를 신호·틱과 같은 테이블에서 받는다. 첫 시드·체결 전에 묶어야 한다. [why D-105]
    ledger.set_symbol_table(&symbols_.table);
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
    zmq_bridge_->set_account_no(kis_config_.account_no); // 실계좌·모의계좌 장부 분리용 [why D-090]
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
                // 버린 건수 넷과 체결 겹수 하나를 여기에도 싣는다 — HEALTH 로도 나가지만 그쪽은 발행 큐를 타고, 그 큐에서 HEALTH 는
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
                       ",\"drop_trade_ring_full\":" + std::to_string(zmq_bridge_->trade_ring_full_drop_count()) +
                       ",\"drop_trade_socket\":" + std::to_string(zmq_bridge_->trade_socket_drop_count()) + "}";
            }

            return "UNKNOWN";
        });
    zmq_bridge_->start();
}
#endif

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
    //  주문 쪽은 장부 대조·선점 정리·하루 초기화다(가르는 선은 docs/DECISIONS.md D-114). [why D-114]
    // jthread는 stop_token을 첫 인자로 넣으므로 멤버 함수 포인터(this가 첫 인자)는 람다로 감싼다.
    data_thread_ = std::jthread([this](std::stop_token stop_token)
    {
        data_thread_fn(stop_token);
    });
    start_rest_poll_loop();

    if (runs_strategy_side())
    {
        // 소켓이 저쪽에 있는 전략 프로세스만 줄 스레드를 띄운다. 한 프로세스로 돌면 수신 스레드가 곧바로
        //  샤드에 넣으므로 이 홉이 없다 — 갈라 띄우는 날에만 한 홉이 는다. [why D-114]
        if (role_ == ProcessRole::Strategy)
        {
            for (uint32_t lane = 0; lane < layout_.feed().lanes(); ++lane)
            {
                feed_lane_threads_.emplace_back([this, lane](std::stop_token stop_token)
                {
                    feed_lane_thread_fn(stop_token, lane);
                });
            }

            LOG_INFO("[Engine] 시세 줄 스레드 " + std::to_string(feed_lane_threads_.size()) + "개 — 통로에서 꺼내 샤드로 나눈다");
        }

        for (uint32_t shard_index = 0; shard_index < static_cast<uint32_t>(pipeline_.shards.size()); ++shard_index)
        {
            pipeline_.shard_threads.emplace_back([this, shard_index](std::stop_token stop_token)
            {
                shard_thread_fn(stop_token, shard_index);
            });
        }

        strategy_thread_ = std::jthread([this](std::stop_token stop_token)
        {
            strategy_thread_fn(stop_token);
        });
    }

    if (runs_order_side())
    {
        order_thread_  = std::jthread([this](std::stop_token stop_token)
        {
            order_thread_fn(stop_token);
        });
        fill_thread_   = std::jthread([this](std::stop_token stop_token)
        {
            fill_thread_fn(stop_token);
        });
        ledger_thread_ = std::jthread([this](std::stop_token stop_token)
        {
            ledger_thread_fn(stop_token);
        });
    }

    // 감시 스레드도 양쪽에 하나씩 — 시세 소켓을 쥔 쪽이 재연결을 보고, 주문 쪽이 마감 종료를 본다.
    control_thread_ = std::jthread([this](std::stop_token stop_token)
    {
        control_thread_fn(stop_token);
    });
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

    if (!prepare_layout_and_shards())
    {
        return;
    }

    start_database_and_bridge();

    const bool offline = feed_.feed_override != nullptr || !feed_.replay_file.empty() || feed_.broker_offline;

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
        if (!start_order_side(offline))
        {
            return;
        }
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

// start() 단계 — 자리표를 소켓 수에 맞추고 샤드를 깐다. 뜨면 안 되는 상태면 거짓을 돌려주고 start()가 접는다.
bool Engine::prepare_layout_and_shards()
{
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
            return false;
        }

        if (!bind_layout(feed_lane_count))
        {
            LOG_ERROR("[Engine] 시세 줄 " + std::to_string(feed_lane_count) + "개로 자리표를 다시 못 깔았다");
            return false;
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
        return false;
    }

    return true;
}

// start() 단계 — DB 적재 워커와 ZMQ 다리를 다른 스레드보다 먼저 띄운다.
void Engine::start_database_and_bridge()
{
#ifdef HAS_PQ
    // DB 적재 워커는 다른 스레드보다 먼저 띄워 끝까지 둔다. 체결은 시세 쪽 수신 스레드로만 들어오므로
    //  체결 워커는 시세 쪽 프로세스에만 둔다. [why D-148]
    //  신호·헬스는 역할마다 제 것을 넣는다 — 신호는 전략 쪽, 헬스는 모든 역할의 데이터 스레드. [why D-154]
    if (database_config_.enabled)
    {
        db::DbConfig config = database_config_;
        config.tick_workers = runs_feed_side() ? config.tick_workers : 0;
        config.account      = kis_config_.account_no;
        config.role         = role_.to_string();
        database_           = std::make_unique<db::DbManager>(std::move(config));
    }
#endif

#ifdef HAS_ZMQ
    // 발행 채널은 역할마다 하나씩 둔다 — 시세는 체결을, 전략은 신호를, 주문은 주문·체결통보를 낸다.
    //  제어(KILL·잔고 조회)는 주문 쪽 포트에만 붙어 전략이 멎어도 살아 있다. [why D-114]
    if (zmq_enabled_)
    {
        setup_zmq_bridge();
    }
#endif
}

// start() 단계 — 주문 쪽 준비(모의 체결기·주문 라우터·잔고 대조기·장부 저널·잔고 시드). 장부를 못 열면 거짓.
bool Engine::start_order_side(bool offline)
{
    setup_paper_executor(offline);
    initialize_order_router();
    initialize_ledger_reconciler();

    // 갈라 띄운 주문 프로세스의 모의 체결기에는 체결을 낼 길이 없다 — 틱은 시세 프로세스로만 가고
    //  (connect_feed 가 거기서 콜백을 건다), 그대로 두면 대기 주문이 영영 안 차서 보유 수량이 0이고
    //  매도가 전량 40240000으로 거부된다(09-25 실측 6,060건). 여기서 콜백만 걸고, 틱은 주문 스레드가
    //  접수를 마친 뒤 지어 먹인다(feed_paper_fill_tick) — 이 역할에는 WS 생산자가 없어 큐 생산자는
    //  주문 스레드 하나로 남는다. [why D-114 단계 5]
    if (feed_.paper && !runs_feed_side())
    {
        feed_.paper->set_fill_callback(
            [this](const FillNotification& fill_notification)
            {
                push_fill_notification(fill_notification);
            });
    }

    // 장부 파일·미결주문 파일은 한 프로세스만 연다 — 둘이 같은 파일을 쓰면 줄이 섞인다. [why D-114]
    if (!try_open_ledger_journal() || !try_bootstrap_ledger())
    {
        return false;
    }

    resolve_open_intents();
    return true;
}

// stop() 단계 — 스레드를 정해진 순서로 회수한다. 순서의 까닭은 줄마다 주석에 적었다.
void Engine::join_threads()
{
    // 역순 join 권장: control → order → strategy → data. fill_thread는 WS를 끊은 뒤에 join한다(아래).
    if (control_thread_.joinable())
    {
        control_thread_.join();
    }

    if (order_thread_.joinable())
    {
        order_thread_.join();
    }

    // 폴러 조회 스레드는 샤드보다 먼저 — 폴러 행의 생산자라 샤드가 선 뒤에 넣으면 받을 쪽이 없다.
    if (poller_)
    {
        poller_->join();
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

    // 장부 사본 스레드는 맨 뒤 — 고치는 쪽(주문·체결)이 다 선 뒤에 마지막 한 판을 내고 끝나야
    //  읽는 쪽이 보는 마지막 사본이 실제 마지막 상태가 된다.
    if (ledger_thread_.joinable())
    {
        ledger_thread_.join();
    }
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
    sellable_info.reserved      = row.reserved;
    return sellable_info;
}

// 전략이 보는 보유 수량 — 문자열 티커를 종목 번호로 한 번 바꿔 사본에서 읽는다. 모르는 종목은 번호가
//  kNone이라 빈 줄이 오고, 보유 0이 된다(등록하지 않는다 — 읽기만 하는 자리다).
int Engine::ledger_position(const std::string& ticker) const
{
    return ledger_snapshot_->row(symbols_.table.lookup(ticker)).position;
}

void Engine::start_rest_poll_loop()
{
    // REST 폴백·넘침 종목 조회는 데이터 스레드 30초 사이클에서 떼어 폴러 스레드가 1초 목표로 돈다.
    //  30종목 묶음 한 건에 100ms 간격이라 160종목이면 6건 약 0.6초다. 300종목을 넘거나 묶음이 실패해 한 종목씩
    //  받으면 한 바퀴가 1초를 넘는다 — 초당 호출 한도가 먼저라 그 늘어남은 받아들인다. [why D-138] [why D-150]
    if (!runs_feed_side() || !poller_)
    {
        return;
    }

    DataPoller::LoopSources sources;
    sources.rest_mode = [this]
    {
        return feed_.rest_feed_active.load(std::memory_order_relaxed);
    };
    sources.universe  = [this]
    {
        std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
        return watch_specifications_; // 조회가 락 밖에서 돌도록 사본을 낸다
    };
    sources.from_websocket = [this]
    {
        return feed_.websocket ? feed_.websocket->take_overflow_specifications() : std::vector<WatchSpec>{};
    };
    sources.on_ticks       = [this](int ticks)
    {
        data_count_ += static_cast<uint64_t>(ticks);
    };
    poller_->start(std::move(sources), std::chrono::milliseconds(kRestPollRoundMs));
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

    for (std::jthread* thread : {&data_thread_, &strategy_thread_, &order_thread_, &fill_thread_, &ledger_thread_, &control_thread_})
    {
        thread->request_stop(); // WakeGate의 stop_token 대기와 sleep_unless_stopped가 여기서 깬다
    }

    for (auto& shard_thread : pipeline_.shard_threads)
    {
        shard_thread.request_stop();
    }

    if (poller_)
    {
        poller_->request_stop();
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

    join_threads();

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

#ifdef HAS_PQ
    // 맨 먼저 띄운 DB 워커는 맨 나중에 거둔다 — 체결을 넣는 수신 스레드가 다 멈춘 뒤라야 남은 행이 다 들어간다.
    if (database_)
    {
        database_->stop();
    }
#endif

    print_statistics();

    // 여기까지 왔으면 스레드를 다 회수한 깨끗한 종료다. 쪽지에 사유를 적어 둬야 짝과 다음 기동이
    //  크래시와 가른다 — 안 적힌 0 이 크래시다. 만든 쪽(주문 프로세스)만 적힌다. [why D-114]
    layout_region_.mark_clean_shutdown(
        static_cast<ipc::SharedShutdownReason>(shutdown_reason_.load(std::memory_order_relaxed)));
    LOG_INFO("[Engine] 종료 완료");
}
