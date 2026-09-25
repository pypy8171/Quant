// 유니버스 재스캔 장부 — 선언과 스레드 약속은 Quant/include/core/UniverseRescan.h.
//  로그 문구("[Engine] … 재스캔 …")는 바꾸지 않는다 — 장 마감 스크립트(scripts/summarize_trading_day.py·
//  scripts/market_close_collect.py)가 이 문구로 줄을 찾는다.

#include "core/UniverseRescan.h"
#include "core/UniverseExit.h"
#include "ipc/LedgerSnapshot.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <algorithm>
#include <exception>

UniverseRescan::UniverseRescan(const symbol::SymbolTable& table, RegisterStrategy register_strategy,
                               RetireStrategy retire_strategy)
    : table_(table), register_strategy_(std::move(register_strategy)), retire_strategy_(std::move(retire_strategy))
{
}

void UniverseRescan::add_job(Job job)
{
    job.owned.resize(table_.capacity()); // 종목 id 인덱스 — id는 용량을 넘지 않는다
    jobs_.push_back(std::move(job));
}

void UniverseRescan::seed(const std::vector<std::pair<symbol::SymbolId, StrategyBase*>>& started,
                          const std::vector<symbol::SymbolId>& symbols)
{
    if (jobs_.empty() || symbols.empty())
    {
        return;
    }

    auto&             job = jobs_.back();
    std::vector<bool> wanted(job.owned.size(), false);

    for (symbol::SymbolId symbol : symbols)
    {
        if (symbol != symbol::kNone && symbol < wanted.size())
        {
            wanted[symbol] = true;
        }
    }

    for (const auto& [symbol, strategy] : started)
    {
        if (symbol < wanted.size() && wanted[symbol] && job.owned[symbol].strategy == nullptr)
        {
            job.owned[symbol].strategy = strategy;
            job.owned_ids.push_back(symbol);
            ++job.registered;   // 상한은 소유 수로 센다 — 떼면 줄어드는 자리에 기동 종목도 든다
        }
    }

    LOG_INFO("[Engine] 재스캔 소유 시드: " + std::to_string(job.owned_ids.size()) + "종목 (기동 유니버스)");
}

void UniverseRescan::reset_registered()
{
    registered_.assign(table_.capacity(), false);
    registered_count_ = 0;
}

void UniverseRescan::set_registered(symbol::SymbolId symbol, bool on)
{
    if (symbol == symbol::kNone)
    {
        return;
    }

    if (symbol >= registered_.size())
    {
        registered_.resize(std::max<size_t>(table_.capacity(), symbol + 1), false);
    }

    if (registered_[symbol] == on)
    {
        return;
    }

    registered_[symbol] = on;
    registered_count_ += on ? 1 : (registered_count_ > 0 ? -1 : 0);
}

int UniverseRescan::shortest_interval_sec(int ceiling) const
{
    int shortest = ceiling;

    for (const auto& job : jobs_)
    {
        if (job.interval_sec > 0 && job.interval_sec < shortest)
        {
            shortest = job.interval_sec;
        }
    }

    return shortest;
}

std::vector<int32_t> UniverseRescan::scan_rank(size_t extent) const
{
    std::vector<int32_t> rank_of(extent, -1);

    for (const auto& job : jobs_)
    {
        for (size_t rank = 0; rank < job.last_scanned.size(); ++rank)
        {
            const symbol::SymbolId symbol = job.last_scanned[rank];

            if (symbol < extent && (rank_of[symbol] < 0 || static_cast<int32_t>(rank) < rank_of[symbol]))
            {
                rank_of[symbol] = static_cast<int32_t>(rank);
            }
        }
    }

    return rank_of;
}

void UniverseRescan::retire_owned(Job& job, symbol::SymbolId symbol,
                                  const std::function<std::string(const StrategyBase&)>& make_line)
{
    if (symbol >= job.owned.size() || job.owned[symbol].strategy == nullptr)
    {
        return;
    }

    StrategyBase* pointer = job.owned[symbol].strategy;
    // 옛 스냅샷이 새 스냅샷으로 바뀔 때까지 틱은 계속 온다 — 그 사이 신규매수만 막는다.
    pointer->set_active(false);
    retire_strategy_(pointer, make_line);

    set_registered(symbol, false);
    job.owned[symbol] = Job::Owned{};
    std::erase(job.owned_ids, symbol);

    if (job.registered > 0)
    {
        --job.registered;
    }
}

bool UniverseRescan::run(KisClient& scan_client, const ipc::LedgerSnapshot& snapshot,
                         std::chrono::steady_clock::time_point now)
{
    bool ran_any = false;

    for (auto& job : jobs_)
    {
        ran_any = run_job(job, scan_client, snapshot, now) || ran_any;
    }

    return ran_any;
}

bool UniverseRescan::run_job(Job& job, KisClient& scan_client, const ipc::LedgerSnapshot& snapshot,
                             std::chrono::steady_clock::time_point now)
{
    if (job.interval_sec <= 0 || !job.universe_fn || !job.factory)
    {
        return false;
    }

    if (job.last_run.time_since_epoch().count() != 0 &&
        now - job.last_run < std::chrono::seconds(job.interval_sec))
    {
        return false;
    }

    // 첫 부재 스캔의 시계 원점 — 직전 스캔(마지막으로 보인 때)이다. 첫 실행이면 지금.
    const auto previous_run = job.last_run.time_since_epoch().count() != 0 ? job.last_run : now;
    job.last_run = now;

    std::vector<symbol::SymbolId> scanned;
    const auto scan_call_start = std::chrono::steady_clock::now();

    try
    {
        scanned = job.universe_fn(scan_client);
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR(std::string("[Engine] 유니버스 재스캔 예외: ") + exception.what());
        return false;
    }
    catch (...)
    {
        LOG_ERROR("[Engine] 유니버스 재스캔 알 수 없는 예외");
        return false;
    }

    // 칸 우선순위는 다음 스캔까지 이 순위표로 매긴다. 빈 결과(조회 실패)는 순위 근거가 아니라 직전 것을 둔다.
    if (!scanned.empty())
    {
        job.last_scanned = scanned;
    }

    // 계측(문항 2): 스캔 함수 자체의 경과와 직전 스캔부터의 실제 간격. 설정 주기보다 간격이 길면
    //  스캔이 느린 것(경과≈간격)인지, 이 스레드의 다른 일(잔고 대조·시세 보충)에 밀린 것인지 여기서 갈린다.
    {
        const long long scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - scan_call_start).count();
        const long long gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - previous_run).count();
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
    ipc::collect_all_rows(snapshot, ledger_ids, ledger_rows);

    for (size_t index = 0; index < ledger_rows.size(); ++index)
    {
        const symbol::SymbolId symbol = ledger_ids[index];

        if (symbol < extent && (ledger_rows[index].position != 0 || ledger_rows[index].reserved != 0))
        {
            held[symbol] = true;
        }
    }

    auto reserved_of = [&snapshot](symbol::SymbolId symbol) { return snapshot.row(symbol).reserved; };
    auto absent_sec_of = [&job, now](symbol::SymbolId symbol) -> long long
    {
        const auto since = job.owned[symbol].absent_since;
        return since.time_since_epoch().count() == 0
                   ? 0LL
                   : std::chrono::duration_cast<std::chrono::seconds>(now - since).count();
    };

    for (symbol::SymbolId code : scanned)
    {
        if (code == symbol::kNone || code >= extent || is_registered(code))
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
                       table_.name(code).string();
            });
        }

        auto strategy = job.factory(code);

        if (!strategy)
        {
            continue;
        }

        LOG_INFO("[Engine] 재스캔 신규 등록: " + strategy->describe());
        StrategyBase* raw = strategy.get();
        register_strategy_(std::move(strategy));

        // on_start 예외로 등록이 거부됐으면 registered에 안 들어간다.
        if (is_registered(code))
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
                 std::to_string(registered_count_) + "종목)" +
                 (capped ? " — 등록 상한 " + std::to_string(job.max_registered) +
                               " 도달, 신규 등록 중단"
                         : ""));
    }

    if (job.drop_after_sec <= 0 && job.block_after_sec <= 0)
    {
        return true;
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

        return true;
    }

    job.empty_scan_warned = false;

    // 이탈 판정. universe_fn은 보유 종목을 결과에서 이미 빼고 주므로(drop_held) 빠져 있다는
    //  것만으로는 이탈이 아니다 — in_scan·held는 위에서 이미 찍은 같은 스냅샷을 그대로 쓴다.
    const universe_exit::Thresholds thread{job.block_after_sec, job.drop_after_sec, job.return_confirm};
    std::vector<symbol::SymbolId> drop;

    for (symbol::SymbolId owned_symbol : job.owned_ids)
    {
        Job::Owned&   owned   = job.owned[owned_symbol];
        StrategyBase* pointer = owned.strategy;
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

        const auto absent_sec = std::chrono::duration_cast<std::chrono::seconds>(now - owned.absent_since).count();

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
                 std::to_string(registered_count_) + "종목)");
    }

    return true;
}
