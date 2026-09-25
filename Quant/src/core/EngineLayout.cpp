// 자리표 깔기 — 큐·장부 사본·박동을 놓을 바이트를 잡고(힙 또는 공유 쪽지), 종목·전략 이름표를 그 위 한 벌로 바꾼다.
//  Engine 클래스는 그대로다. Engine.cpp 가 길어 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  bind_layout() : 생성자(시세 한 줄로 먼저 깔기), configure()(설정을 읽은 뒤 소켓 수로 깔기), start()(줄 수가 달라졌을 때만
//                  다시 깔기). 스레드가 뜨기 전에만 부른다.

#include "core/Engine.h"
#include "utils/Logger.h"
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace
{

// 전략 프로세스가 공유 쪽지를 기다리는 시간. 주문 쪽은 토큰 발급·잔고 대조·원장 리플레이를 먼저 하므로
//  기동이 몇 초 늦을 수 있다 — 그보다 넉넉히 두되, 아예 안 뜬 경우에는 기다림이 끝나야 한다. [why D-114]
constexpr auto kSharedRegionAttachTimeout = std::chrono::seconds(30);

} // namespace

// 자리표를 깐다. 갈라 띄우면 이 바이트가 공유 쪽지가 되고, Both 로 돌면 힙 한 덩이가 그 자리를 대신한다 —
//  놓는 자리도 셈도 같아서 갈라 띄우는 날 처음 도는 코드가 없다. [why D-114]
//  [inv] 스레드가 뜨기 전에만 부른다. 돌던 큐 위에 다시 깔면 칸이 0으로 밀려 오가던 것이 사라진다.
bool Engine::bind_layout(uint32_t feed_lanes)
{
    // 큐 칸 수는 한 프로세스로 돌던 때 쓰던 상수를 그대로 쓴다 — 자리표에 따로 적으면 두 벌이 된다.
    layout_config_.feed_lanes        = feed_lanes;
    layout_config_.request_capacity  = ShardPipeline::kOrderQueueCapacity;
    layout_config_.response_capacity = ShardPipeline::kOrderResponseCapacity;
    layout_config_.control_capacity  = ControlPlane::kQueueCapacity;

    const size_t needed = ipc::SharedLayout::bytes_for(layout_config_);

    layout_.unbind();

    // 한 프로세스로 돌면 힙, 갈라 띄우면 공유 쪽지다. 고르는 자리는 여기 하나다. [why D-114]
    if (!(role_ == ProcessRole::Both ? bind_layout_on_heap(needed) : bind_layout_on_region(needed)))
    {
        return false;
    }

    // 자리표 위 면을 쓰는 자리에 꽂는다. [inv] 이 포인터들은 다음 bind_layout 까지만 유효하다.
    ledger_snapshot_             = layout_.ledger();
    control_plane_.bind(&layout_.controls(), &layout_.feed_controls());
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
