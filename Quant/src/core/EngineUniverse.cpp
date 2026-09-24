// 유니버스 재스캔 — 스캔 결과로 새 종목을 등록하고, 스캔에서 빠진 소유 종목은 신규매수를 막았다가 뗀다.
//  Engine 클래스는 그대로다. Engine.cpp 가 4,000줄을 넘겨 열기 어려워 이 갈래만 따로 낸 것이다
//  (헤더는 한 줄도 안 바뀐다 — 같은 Engine 의 멤버 함수 본체가 여기 있을 뿐이다).
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  seed_universe_rescan()  : 기동 때 스레드 시작 전
//  maybe_rescan_universe() : data 스레드가 매 사이클
//  rescan_set_registered() : add_strategy()·retire_owned() 가 등록 표시를 켜고 끌 때
//  reap_retired()          : 떼어 둔 전략을 거둘 때(stop() 은 force)

#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/UniverseExit.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <mutex>

// 기동 유니버스를 마지막 재스캔 슬리브의 소유로 — 스레드 시작 전 호출 전용(strategy_.list를 락 없이 읽는다).
void Engine::seed_universe_rescan(const std::vector<symbol::SymbolId>& symbols)
{
    if (universe_rescan_.jobs.empty() || symbols.empty())
    {
        return;
    }

    auto&             job = universe_rescan_.jobs.back();
    std::vector<bool> wanted(job.owned.size(), false);

    for (symbol::SymbolId symbol : symbols)
    {
        if (symbol != symbol::kNone && symbol < wanted.size())
        {
            wanted[symbol] = true;
        }
    }

    for (auto& strategy : strategy_.list)
    {
        for (const auto& specification : strategy->get_watch_specifications())
        {
            if (specification.market != Market::KR)
            {
                continue;
            }

            // 구독 스펙의 티커는 문자열이라 여기서 id로 바꾼다(기동 1회).
            const symbol::SymbolId symbol = register_symbol(specification.ticker);

            if (symbol < wanted.size() && wanted[symbol] && job.owned[symbol].strategy == nullptr)
            {
                job.owned[symbol].strategy = strategy.get();
                job.owned_ids.push_back(symbol);
                ++job.registered;   // 상한은 소유 수로 센다 — 떼면 줄어드는 자리에 기동 종목도 든다
            }
        }
    }

    LOG_INFO("[Engine] 재스캔 소유 시드: " + std::to_string(job.owned_ids.size()) + "종목 (기동 유니버스)");
}

void Engine::rescan_set_registered(symbol::SymbolId symbol, bool on)
{
    if (symbol == symbol::kNone)
    {
        return;
    }

    if (symbol >= universe_rescan_.registered.size())
    {
        universe_rescan_.registered.resize(std::max<size_t>(symbols_.table.capacity(), symbol + 1), false);
    }

    if (universe_rescan_.registered[symbol] == on)
    {
        return;
    }

    universe_rescan_.registered[symbol] = on;
    universe_rescan_.registered_count += on ? 1 : (universe_rescan_.registered_count > 0 ? -1 : 0);
}

void Engine::retire_owned(RescanJob& job, symbol::SymbolId symbol,
                          const std::function<std::string(const StrategyBase&)>& make_line)
{
    if (symbol >= job.owned.size() || job.owned[symbol].strategy == nullptr)
    {
        return;
    }

    StrategyBase* pointer = job.owned[symbol].strategy;
    // 옛 스냅샷이 새 스냅샷으로 바뀔 때까지 틱은 계속 온다 — 그 사이 신규매수만 막는다.
    pointer->set_active(false);

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

    rescan_set_registered(symbol, false);
    job.owned[symbol] = RescanJob::Owned{};
    std::erase(job.owned_ids, symbol);

    if (job.registered > 0)
    {
        --job.registered;
    }
}

// 주기적 유니버스 재스캔 — universe_fn_으로 티커 목록을 산출해 미등록 종목은 런타임 등록하고,
//  소유 종목이 스캔에서 연속으로 빠지면 block_after_sec에 신규매수를 막고 drop_after_sec에 뗀다(보유·선점 없을 때만).
void Engine::maybe_rescan_universe()
{
    reap_retired(/*force=*/false);

    // 마지막 매매 창이 닫혔으면 더 스캔하지 않는다 — 여기서 새로 붙는 전략은 첫 봉에 진입 신호를 내는데
    // 게이트가 세션 창 밖으로 전부 막는다. 09-22 마감에 005257이 15:30:30에 붙어 매수를 냈고, 하루 판정이
    // "매매 창 밖 거부 1건"으로 FAIL 났다. 자기 종료 유예(120초)는 주문 큐를 비우라고 있는 시간이지
    // 새 종목을 담으라고 있는 시간이 아니다. 창 0(리플레이·테스트)이면 판정하지 않는다
    const int last_close_min = session_end_.config().close_min;

    if (last_close_min > 0 && kst::sec_of_day(std::time(nullptr)) >= last_close_min * 60)
    {
        return;
    }

    if (universe_rescan_.jobs.empty())
    {
        return;
    }

    KisClient* scan_kis = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

    if (!scan_kis)
    {
        return;
    }

    const auto now_steady = std::chrono::steady_clock::now();
    bool       ran_any    = false;

    for (auto& job : universe_rescan_.jobs)
    {
        if (job.interval_sec <= 0 || !job.universe_fn || !job.factory)
        {
            continue;
        }

        if (job.last_run.time_since_epoch().count() != 0 &&
            now_steady - job.last_run < std::chrono::seconds(job.interval_sec))
        {
            continue;
        }

        // 첫 부재 스캔의 시계 원점 — 직전 스캔(마지막으로 보인 때)이다. 첫 실행이면 지금.
        const auto previous_run = job.last_run.time_since_epoch().count() != 0 ? job.last_run : now_steady;
        job.last_run = now_steady;

        std::vector<symbol::SymbolId> scanned;
        const auto scan_call_start = std::chrono::steady_clock::now();

        try
        {
            scanned = job.universe_fn(*scan_kis);
        }
        catch (const std::exception& exception)
        {
            LOG_ERROR(std::string("[Engine] 유니버스 재스캔 예외: ") + exception.what());
            continue;
        }
        catch (...)
        {
            LOG_ERROR("[Engine] 유니버스 재스캔 알 수 없는 예외");
            continue;
        }

        // 칸 우선순위는 다음 스캔까지 이 순위표로 매긴다. 빈 결과(조회 실패)는 순위 근거가 아니라 직전 것을 둔다.
        if (!scanned.empty())
        {
            job.last_scanned = scanned;
        }

        ran_any = true;

        // 계측(문항 2): 스캔 함수 자체의 경과와 직전 스캔부터의 실제 간격. 설정 주기보다 간격이 길면
        //  스캔이 느린 것(경과≈간격)인지, 이 스레드의 다른 일(잔고 대조·시세 보충)에 밀린 것인지 여기서 갈린다.
        {
            const long long scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - scan_call_start).count();
            const long long gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_steady - previous_run).count();
            LOG_INFO("[Engine] 유니버스 재스캔 계측: 경과=" + std::to_string(scan_ms) + "ms 간격=" +
                     std::to_string(gap_ms) + "ms(설정 " + std::to_string(job.interval_sec * 1000) + "ms) 결과=" +
                     std::to_string(scanned.size()) + "종목 이 슬리브 등록=" + std::to_string(job.registered));
        }

        int  added  = 0;
        bool capped = false;

        // 상한에 닿았을 때 자리를 내줄 후보 — 오늘 스캔 top-N에 없고(점수 밀림) 미보유인 등록 종목.
        //  누적 등록만 세면 한 번 자리 잡은 종목이 오늘 순위와 무관하게 영영 남는다 [why D-087].
        //  이탈 판정(아래)도 같은 스캔·같은 보유 스냅샷을 쓴다 — 두 번 찍으면 그 사이 체결로 어긋날 수 있다.
        const size_t      extent = job.owned.size();
        std::vector<bool> in_scan(extent, false);
        std::vector<bool> held(extent, false);

        for (symbol::SymbolId symbol : scanned)
        {
            if (symbol != symbol::kNone && symbol < extent)
            {
                in_scan[symbol] = true;
            }
        }

        // 보유·선점 종목은 사본 한 판에서 함께 본다 — 줄마다 따로 읽으면 앞 종목과 뒤 종목이 다른 판의 것이 되어
        //  방금 판 종목을 아직 보유로 보고 유니버스에 붙들어 둔다. [why D-114]
        std::vector<symbol::SymbolId> ledger_ids;
        std::vector<ipc::LedgerRow>   ledger_rows;
        ipc::collect_all_rows(*ledger_snapshot_, ledger_ids, ledger_rows);

        for (size_t index = 0; index < ledger_rows.size(); ++index)
        {
            const symbol::SymbolId symbol = ledger_ids[index];

            if (symbol < extent && (ledger_rows[index].position != 0 || ledger_rows[index].reserved != 0))
            {
                held[symbol] = true;
            }
        }

        auto reserved_of = [this](symbol::SymbolId symbol) { return ledger_snapshot_->row(symbol).reserved; };
        auto absent_sec_of = [&job, now_steady](symbol::SymbolId symbol) -> long long
        {
            const auto since = job.owned[symbol].absent_since;
            return since.time_since_epoch().count() == 0
                       ? 0LL
                       : std::chrono::duration_cast<std::chrono::seconds>(now_steady - since).count();
        };

        for (symbol::SymbolId code : scanned)
        {
            if (code == symbol::kNone || code >= extent || rescan_is_registered(code))
            {
                continue;
            }

            // 상한에 닿으면 오늘 순위 밖·미보유 종목을 찾아 그 자리를 내준다. 없으면 더 등록하지 않는다.
            if (job.max_registered > 0 && job.registered >= job.max_registered)
            {
                const symbol::SymbolId victim_symbol =
                    universe_exit::pick_evict_candidate(job.owned_ids, in_scan, held, reserved_of, absent_sec_of);

                if (victim_symbol == symbol::kNone)
                {
                    capped = true;
                    break;
                }

                retire_owned(job, victim_symbol, [this, code](const StrategyBase& victim) {
                    return "[Engine] 재스캔 점수 교체 — 오늘 순위 밖·미보유 해제: " + victim.describe() + " → " +
                           symbols_.table.name(code).string();
                });
            }

            auto strategy = job.factory(code);

            if (!strategy)
            {
                continue;
            }

            LOG_INFO("[Engine] 재스캔 신규 등록: " + strategy->describe());
            StrategyBase* raw = strategy.get();
            register_strategy_runtime(std::move(strategy));

            // on_start 예외로 등록이 거부됐으면 registered에 안 들어간다.
            if (rescan_is_registered(code))
            {
                job.owned[code].strategy = raw;
                job.owned_ids.push_back(code);
                ++added;
                ++job.registered;
            }
        }

        if (added > 0 || capped)
        {
            LOG_INFO("[Engine] 유니버스 재스캔 완료: +" + std::to_string(added) +
                     "종목 (이 슬리브 " + std::to_string(job.registered) + ", 전체 " +
                     std::to_string(universe_rescan_.registered_count) + "종목)" +
                     (capped ? " — 등록 상한 " + std::to_string(job.max_registered) +
                                   " 도달, 신규 등록 중단"
                             : ""));
        }

        if (job.drop_after_sec <= 0 && job.block_after_sec <= 0)
        {
            continue;
        }

        // 빈 결과는 이탈 근거가 아니다 — 스캐너는 지수 조회 실패·위험회피 때 예외 대신 빈 목록을 돌려준다.
        //  그대로 부재로 세면 한 번의 실패에 소유 전부가 차단·해제된다. 시계도 세우지 않고 건너뛴다.
        if (scanned.empty() && !job.owned_ids.empty())
        {
            if (!job.empty_scan_warned)
            {
                LOG_WARN("[Engine] 유니버스 재스캔 결과 없음 — 이탈 판정 건너뜀(소유 " +
                         std::to_string(job.owned_ids.size()) + "종목, 다음 결과까지 유지)");
                job.empty_scan_warned = true;
            }

            continue;
        }

        job.empty_scan_warned = false;

        // 이탈 판정. universe_fn은 보유 종목을 결과에서 이미 빼고 주므로(drop_held) 빠져 있다는
        //  것만으로는 이탈이 아니다 — in_scan·held는 위에서 이미 찍은 같은 스냅샷을 그대로 쓴다.
        const universe_exit::Thresholds thread{job.block_after_sec, job.drop_after_sec, job.return_confirm};
        std::vector<symbol::SymbolId> drop;

        for (symbol::SymbolId owned_symbol : job.owned_ids)
        {
            RescanJob::Owned& owned   = job.owned[owned_symbol];
            StrategyBase*     pointer = owned.strategy;
            // 시계는 하나(연속 부재), 임계값은 둘 — block_after_sec에 신규매수를 막고 drop_after_sec에 뗀다.
            //  복귀는 present가 return_confirm회 연속일 때만 — 경계 종목이 한 번 보이자마자 풀리면 사고팔기를
            //  반복한다. 보유 종목은 스캔 결과에서 빠져 오므로(drop_held) 보유를 이탈로 보지 않는다 —
            //  분할 매수 추가는 전략 자신의 정배열 게이트가 판단한다 [why D-077].
            const bool present = in_scan[owned_symbol] || held[owned_symbol] || reserved_of(owned_symbol) != 0;

            if (present)
            {
                owned.absent_since = {};

                if (!pointer->in_universe())
                {
                    const int streak = ++owned.present_streak;

                    if (universe_exit::judge_return(streak, thread, /*in_universe=*/false))
                    {
                        pointer->set_in_universe(true);
                        owned.present_streak = 0;
                        LOG_INFO("[Engine] 유니버스 복귀 → 신규매수 허용: " + pointer->describe() + " (present " +
                                 std::to_string(streak) + "회 연속)");
                    }
                }

                continue;
            }

            owned.present_streak = 0;

            if (owned.absent_since.time_since_epoch().count() == 0)
            {
                owned.absent_since = previous_run;
            }

            const auto absent_sec = std::chrono::duration_cast<std::chrono::seconds>(now_steady - owned.absent_since).count();

            switch (universe_exit::judge_absent(absent_sec, thread, pointer->in_universe()))
            {
            case universe_exit::Absent::BLOCK:
                pointer->set_in_universe(false);
                LOG_INFO("[Engine] 유니버스 이탈 → 신규매수 차단: " + pointer->describe() + " (" +
                         std::to_string(absent_sec) + "초 부재, 해제는 " + std::to_string(job.drop_after_sec) + "초)");
                break;
            case universe_exit::Absent::DROP:
                drop.push_back(owned_symbol);
                break;
            case universe_exit::Absent::KEEP:
                break;
            }
        }

        for (symbol::SymbolId dropped_symbol : drop)
        {
            retire_owned(job, dropped_symbol, [&job](const StrategyBase& victim) {
                return "[Engine] 재스캔 이탈 해제: " + victim.describe() + " — " + std::to_string(job.drop_after_sec) +
                       "초 이상 유니버스 밖, 보유·선점 없음";
            });
        }

        if (!drop.empty())
        {
            LOG_INFO("[Engine] 유니버스 재스캔 해제: -" + std::to_string(drop.size()) +
                     "종목 (이 슬리브 " + std::to_string(job.registered) + ", 전체 " +
                     std::to_string(universe_rescan_.registered_count) + "종목)");
        }
    }

    if (ran_any)
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
    std::vector<int32_t> scan_rank(extent, -1);

    for (const auto& job : universe_rescan_.jobs)
    {
        for (size_t rank = 0; rank < job.last_scanned.size(); ++rank)
        {
            const symbol::SymbolId symbol = job.last_scanned[rank];

            if (symbol < extent && (scan_rank[symbol] < 0 || static_cast<int32_t>(rank) < scan_rank[symbol]))
            {
                scan_rank[symbol] = static_cast<int32_t>(rank);
            }
        }
    }

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
