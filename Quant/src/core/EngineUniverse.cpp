// 유니버스 재스캔의 Engine 쪽 — 재스캔 장부(Quant/src/core/UniverseRescan.cpp)와 전략 목록·구독 칸을 잇는다.
//  스캔·교체·차단·해제 판정은 장부가 하고, 여기는 전략 목록에서 떼기·거두기와 구독 칸 우선순위 보내기를 맡는다.
//  스레드는 data_thread(seed_universe_rescan만 기동 때 스레드 시작 전). [why D-077][why D-132]
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  seed_universe_rescan()     : 기동 때 스레드 시작 전
//  maybe_rescan_universe()    : data 스레드가 매 사이클
//  retire_strategy()          : 장부가 점수 교체·이탈 해제로 소유 종목을 뗄 때(콜백)
//  publish_watch_priorities() : 재스캔이 돈 뒤 구독 칸 우선순위를 다시 보낼 때
//  reap_retired()             : 떼어 둔 전략을 거둘 때(stop() 은 force)

#include "core/Engine.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <mutex>

// 기동 유니버스를 마지막 재스캔 슬리브의 소유로 — 스레드 시작 전 호출 전용(strategy_.list를 락 없이 읽는다).
void Engine::seed_universe_rescan(const std::vector<symbol::SymbolId>& symbols)
{
    if (universe_rescan_.empty() || symbols.empty())
    {
        return;
    }

    std::vector<std::pair<symbol::SymbolId, StrategyBase*>> started;

    for (auto& strategy : strategy_.list)
    {
        for (const auto& specification : strategy->get_watch_specifications())
        {
            if (specification.market != Market::KR)
            {
                continue;
            }

            // 구독 스펙의 티커는 문자열이라 여기서 id로 바꾼다(기동 1회).
            started.emplace_back(register_symbol(specification.ticker), strategy.get());
        }
    }

    universe_rescan_.seed(started, symbols);
}

void Engine::retire_strategy(StrategyBase* pointer, const std::function<std::string(const StrategyBase&)>& make_line)
{
    std::unique_ptr<StrategyBase> victim;
    uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lock(strategy_.mutex);
        auto strategy_iterator = std::find_if(strategy_.list.begin(), strategy_.list.end(),
                                [pointer](const std::unique_ptr<StrategyBase>& strategy) { return strategy.get() == pointer; });

        if (strategy_iterator != strategy_.list.end())
        {
            victim = std::move(*strategy_iterator);
            strategy_.list.erase(strategy_iterator);
        }

        version = strategy_.version.fetch_add(1, std::memory_order_release) + 1;
        rebuild_routes_locked();
    }

    if (victim)
    {
        LOG_INFO(make_line(*victim));
        strategy_.retired.push_back(Retired{std::move(victim), version});
    }
}

// 주기적 유니버스 재스캔 — 스캔·등록·차단·해제 판정은 UniverseRescan::run이 한다. 여기는 거두기·마감 판정·
//  스캔에 쓸 KIS 연결 고르기와, 스캔이 돌았으면 구독 칸 우선순위를 다시 보내는 것만 맡는다.
void Engine::maybe_rescan_universe()
{
    reap_retired(/*force=*/false);

    // 마지막 매매 창이 닫혔으면 더 스캔하지 않는다 — 새로 붙은 전략의 첫 진입 신호는 게이트가 창 밖이라 거부한다
    //  (09-22 15:30:30 005257이 붙어 "매매 창 밖 거부 1건"으로 하루 판정 FAIL). 종료 유예 120초는 주문 큐를 비우는
    //  시간이다(Quant/include/core/SessionEndJudge.h grace_sec). 창 0(리플레이·테스트)이면 판정하지 않는다.
    const int last_close_min = session_end_.config().close_min;

    if (last_close_min > 0 && kst::sec_of_day(std::time(nullptr)) >= last_close_min * 60)
    {
        return;
    }

    if (universe_rescan_.empty())
    {
        return;
    }

    KisClient* scan_kis = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

    if (!scan_kis)
    {
        return;
    }

    if (universe_rescan_.run(*scan_kis, *ledger_snapshot_, std::chrono::steady_clock::now()))
    {
        publish_watch_priorities();
    }
}

void Engine::publish_watch_priorities()
{
    const size_t extent = symbols_.table.capacity() + 1;

    if (watch_priority_sent_.size() < extent)
    {
        watch_priority_sent_.resize(extent, kUnsent);
    }

    // 점수 순위 — 슬리브가 여럿이면 가장 앞선 순위를 쓴다.
    const std::vector<int32_t> scan_rank = universe_rescan_.scan_rank(extent);

    // 보내는 동안 자물쇠를 쥐지 않으려고 사본을 뜬다(보내기 실패 로그가 끼어 있다).
    std::vector<WatchSpec> specifications;
    {
        std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
        specifications = watch_specifications_;
    }

    for (const auto& specification : specifications)
    {
        const symbol::SymbolId symbol = websocket_slot::is_managed(specification) ? symbols_.table.lookup(specification.ticker) : symbol::kNone;

        if (symbol == symbol::kNone || symbol >= extent)
        {
            continue;
        }

        // 보유·선점은 원장 사본에서 본다 — 재스캔 이탈 판정과 같은 자리다. [why D-114]
        const auto& row      = ledger_snapshot_->row(symbol);
        const bool  held     = row.position != 0;
        const bool  reserved = row.reserved != 0;

        // 떼어 낸 전략의 종목이 칸을 쥔 채 남으면 새 점수 상위가 칸을 못 받는다. 보는 전략도 보유·선점도 없으면 푼다.
        if (!held && !reserved && route_mask(symbol) == 0)
        {
            {
                std::lock_guard<std::mutex> specifications_lock(watch_specifications_mutex_);
                std::erase_if(watch_specifications_,
                              [&specification](const WatchSpec& watch) { return same_watch(watch, specification); });
            }

            send_watch_request(specification, ipc::ControlKind::kWatchUnsubscribe);
            watch_priority_sent_[symbol] = kUnsent;
            continue;
        }

        const int32_t priority = websocket_slot::priority_of(held, reserved, scan_rank[symbol]);

        if (watch_priority_sent_[symbol] != priority)
        {
            send_watch_request(specification, ipc::ControlKind::kWatchPriority, priority);
            watch_priority_sent_[symbol] = priority;
        }
    }
}

void Engine::reap_retired(bool force)
{
    if (strategy_.retired.empty())
    {
        return;
    }

    const uint64_t seen = strategy_.seen_version.load(std::memory_order_acquire);
    auto           keep = strategy_.retired.begin();

    for (auto iterator = strategy_.retired.begin(); iterator != strategy_.retired.end(); ++iterator)
    {
        if (!force && iterator->version > seen)
        {
            *keep++ = std::move(*iterator);
            continue;
        }

        try
        {
            iterator->strategy->on_stop();
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR("[Engine] 해제 전략 on_stop 예외 [" + iterator->strategy->id() + "]: " + exception.what());
        }
        catch (...)
        {
            LOG_ERROR("[Engine] 해제 전략 on_stop 알 수 없는 예외 [" + iterator->strategy->id() + "]");
        }

        iterator->strategy.reset();
    }

    strategy_.retired.erase(keep, strategy_.retired.end());
}
