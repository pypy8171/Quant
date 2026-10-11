// WS 구독 칸 배정 — 칸 우선순위 계산, 칸 재배정, 시세 쪽 제어 요청(구독·해지·칸 우선순위) 적용. 제어 스레드가 부른다.
//  EngineFeed.cpp 에서 떼어 낸 것이다(같은 Engine 의 멤버 함수 본체). 관련 결정: D-114·D-132.

#include "core/Engine.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ─── 구독 칸 배정(제어 스레드) ────────────────────────────────────────────
bool Engine::is_websocket_pinned(std::string_view ticker) const
{
    return std::find(websocket_pin_tickers_.begin(), websocket_pin_tickers_.end(), ticker) != websocket_pin_tickers_.end();
}

int32_t Engine::websocket_slot_priority(const WatchSpec& specification) const
{
    if (!websocket_slot::is_managed(specification))
    {
        return websocket_slot::kHeld; // 칸 배정 밖(미국)은 기동 때처럼 먼저 건다
    }

    const symbol::SymbolId symbol = symbols_.table.lookup(specification.ticker);
    return symbol < websocket_slots_.size() ? websocket_slots_[symbol].priority : websocket_slot::kUnranked;
}

void Engine::rebalance_websocket_slots()
{
    // 칸 개념이 없는 소스(리플레이)거나 연결 전이면 하지 않는다 — 연결 전 목록은 connect()가 건다.
    if (!feed_.websocket || !feed_.websocket->is_connected())
    {
        return;
    }

    const int free_slots = feed_.websocket->free_slots();

    if (free_slots < 0)
    {
        return;
    }

    std::vector<WatchSpec> specifications;
    {
        std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
        specifications.reserve(watch_specifications_.size());

        for (const auto& specification : watch_specifications_)
        {
            if (websocket_slot::is_managed(specification))
            {
                specifications.push_back(specification);
            }
        }
    }

    if (websocket_slots_.size() < symbols_.table.capacity() + 1)
    {
        websocket_slots_.resize(symbols_.table.capacity() + 1);
    }

    const auto                    now = std::chrono::steady_clock::now();
    std::vector<websocket_slot::Entry>   entries;
    std::vector<symbol::SymbolId> ids;
    std::vector<size_t>           specification_index;
    int                           used = 0;
    entries.reserve(specifications.size());

    for (size_t index = 0; index < specifications.size(); ++index)
    {
        const WatchSpec&       specification = specifications[index];
        const symbol::SymbolId symbol        = symbols_.table.lookup(specification.ticker);

        if (symbol == symbol::kNone || symbol >= websocket_slots_.size())
        {
            continue;
        }

        WebSocketSlotState&   state = websocket_slots_[symbol];
        websocket_slot::Entry entry;
        entry.channels  = websocket_slot::channels_of(specification);
        entry.priority  = state.priority;
        entry.on_socket = feed_.websocket->has_specification(specification);

        if (entry.on_socket)
        {
            // 처음 칸에 오른 것을 본 때 — 재연결로 다시 걸린 종목이 REST로도 겹쳐 받지 않게 넘침에서 뺀다.
            if (state.on_since == std::chrono::steady_clock::time_point{})
            {
                state.on_since = now;

                if (poller_)
                {
                    poller_->remove_overflow(specification);
                }
            }

            entry.held_sec = std::chrono::duration_cast<std::chrono::seconds>(now - state.on_since).count();
            used          += entry.channels;
        }
        else
        {
            state.on_since = {};
        }

        entries.push_back(entry);
        ids.push_back(symbol);
        specification_index.push_back(index);
    }

    websocket_slot::Rules rules;
    rules.capacity = used + free_slots;
    const websocket_slot::Plan plan = websocket_slot::plan(entries, rules);

    for (size_t chosen : plan.release)
    {
        const WatchSpec& specification = specifications[specification_index[chosen]];

        if (!feed_.websocket->unsubscribe_incremental(specification))
        {
            continue;
        }

        if (poller_)
        {
            poller_->add_overflow(specification);
        }

        websocket_slots_[ids[chosen]].on_since = {};
        LOG_INFO("[WS칸] 칸 내줌 — " + specification.ticker + " 우선순위=" + std::to_string(entries[chosen].priority) +
                 " 쥔 시간=" + std::to_string(entries[chosen].held_sec) + "초, 이제 REST로 받는다");
    }

    std::vector<bool> taken(entries.size(), false);

    for (size_t chosen : plan.take)
    {
        const WatchSpec& specification = specifications[specification_index[chosen]];

        // 상한에 걸려 실패하면 소켓 쪽이 넘침 목록에 다시 올린다 — REST로 계속 받는다.
        if (!feed_.websocket->subscribe_incremental(specification) || !feed_.websocket->has_specification(specification))
        {
            continue;
        }

        if (poller_)
        {
            poller_->remove_overflow(specification);
        }

        taken[chosen]                   = true;
        websocket_slots_[ids[chosen]].on_since = now;
        LOG_INFO("[WS칸] 칸 받음 — " + specification.ticker + " 우선순위=" + std::to_string(entries[chosen].priority));
    }

    // 보유·선점 종목이 칸 밖에 남는 것은 칸 전부를 보유·선점이 쥔 때뿐이다. 바뀔 때만 한 줄 남긴다 — 판정 행이 센다.
    for (size_t index = 0; index < entries.size(); ++index)
    {
        WebSocketSlotState& state       = websocket_slots_[ids[index]];
        const bool   off_socket  = !entries[index].on_socket && !taken[index];
        const bool   must_listen = entries[index].priority <= websocket_slot::kProtected;
        const bool   warn        = must_listen && off_socket;

        if (warn != state.warned_off_socket)
        {
            state.warned_off_socket = warn;
            const std::string& ticker = specifications[specification_index[index]].ticker;

            if (warn)
            {
                LOG_WARN("[WS칸] 보유·선점 종목이 칸 밖 — " + ticker + " 는 REST로만 받는다(칸 전부를 보유·선점이 쥠)");
            }
            else
            {
                LOG_INFO("[WS칸] 보유·선점 종목 칸 밖 풀림 — " + ticker);
            }
        }
    }
}

void Engine::apply_feed_control_requests()
{
    ipc::ControlRequest request;

    while (control_plane_.pop_feed(request))
    {
        switch (request.kind)
        {
        // 목록에만 올리고 소켓에 거는 것은 이어지는 drain_pending_subscriptions()가 한다 — 꺼내는 자리가
        //  소켓 쓰기에 막히면 그동안 제어 줄이 밀린다. [why D-114]
        case ipc::ControlKind::kWatchSubscribe:
        {
            if (request.ticker.empty() || request.ticker.size() > symbol::Ticker::kMax)
            {
                break; // 통로 저쪽에서 온 칸은 믿지 않는다 — 길이가 칸을 넘으면 view() 가 칸 밖을 읽는다
            }

            WatchSpec specification = ipc::watch_specification_of(request);

            // 목록에 이미 있어도 구독은 건다 — Both 로 돌면 collect_watch_specifications() 가 채운 목록에 그대로 들어 있다.
            add_watch_specification(specification);

            {
                std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
                pending_subscriptions_.push_back(std::move(specification));
            }

            break;
        }

        // 보는 전략이 없어진 종목 — 소켓에서 푸는 것은 drain_pending_subscriptions()가 한다. 같은 바퀴에 앞서 온
        //  구독이 아직 안 걸렸으면 그것도 거둔다(순서가 뒤집혀 다시 걸리지 않게). [why D-132]
        case ipc::ControlKind::kWatchUnsubscribe:
        {
            if (request.ticker.empty() || request.ticker.size() > symbol::Ticker::kMax)
            {
                break;
            }

            WatchSpec specification = ipc::watch_specification_of(request);
            std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
            std::erase_if(pending_subscriptions_,
                          [&specification](const WatchSpec& watch)
                          {
                              return same_watch(watch, specification);
                          });
            pending_unsubscriptions_.push_back(std::move(specification));
            break;
        }

        // 칸 우선순위만 바꾼다. 칸을 실제로 옮기는 것은 이어지는 rebalance_websocket_slots()다. [why D-132]
        case ipc::ControlKind::kWatchPriority:
        {
            if (request.ticker.empty() || request.ticker.size() > symbol::Ticker::kMax)
            {
                break;
            }

            const symbol::SymbolId symbol = symbols_.table.lookup(request.ticker.view());

            if (symbol == symbol::kNone)
            {
                break; // 아직 구독 요청이 안 닿은 종목 — 우선순위가 바뀌면 다음 재스캔이 다시 보낸다
            }

            if (websocket_slots_.size() <= symbol)
            {
                websocket_slots_.resize(std::max<size_t>(symbols_.table.capacity() + 1, symbol + 1));
            }

            websocket_slots_[symbol].priority = request.rank;
            break;
        }

        default:
            // 이 줄로는 구독·해지·칸 우선순위만 온다. 다른 낱말이 보이면 가르는 규칙과 보내는 쪽이 어긋난 것이다.
            LOG_ERROR("[Engine] 시세 제어 줄에 엉뚱한 낱말이 왔다 — " +
                      std::to_string(static_cast<int>(request.kind)));
            break;
        }
    }
}
