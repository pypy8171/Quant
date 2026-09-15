#include "core/Engine.h"
#include "core/UniverseExit.h"
#include "core/KstTime.h"
#include "core/LatencyTrace.h"
#include "core/ReconcilePlan.h"
#include "utils/Logger.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

Engine::Engine(KisConfig kis_cfg, int fetch_interval_sec)
    : kis_cfg_(std::move(kis_cfg)), fetch_interval_sec_(fetch_interval_sec),
      last_px_arr_(std::make_unique<std::atomic<double>[]>(symbols_.capacity())),
      last_px_at_ns_(std::make_unique<std::atomic<int64_t>[]>(symbols_.capacity()))
{
}

Engine::~Engine()
{
    stop();
}

void Engine::add_strategy(std::unique_ptr<StrategyBase> strategy)
{
    LOG_INFO("[Engine] 전략 등록: " + strategy->describe());
    strat_.list.push_back(std::move(strategy));
}

// 런타임(장중) 전략 등록. start()의 초기화 루프와 동일한 준비를 하되, strat_.list
// push_back은 strat_.mutex 하에 수행하고 strat_.version을 증가시켜 strategy_thread가
// 스냅샷을 재구성하도록 한다. watch_specs_는 control_thread가 재연결 때 읽으므로 watch_specs_mtx_로 감싼다.
void Engine::register_strategy_runtime(std::unique_ptr<StrategyBase> strategy)
{
    if (!strategy)
    {
        return;
    }

    if (pipeline_.shards.size() > 1 && !strat::owner_shard(*strategy, static_cast<uint32_t>(pipeline_.shards.size()),
                                                  [this](std::string_view t) { return symbols_.intern(t); }))
    {
        LOG_ERROR("[Engine] 런타임 전략 등록 거부 — " + strategy->id() + "의 종목이 여러 샤드에 걸친다(샤드 " +
                  std::to_string(pipeline_.shards.size()) + "개)");
        return;
    }

    // 런타임 등록 전략도 차트 조회는 실전 시세키로(분봉 모의 HTTP500 회피) — start()와 동일 패턴.
    strategy->set_kis(feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get());
    strategy->set_account_kis(feed_.kis.get()); // 잔고·매도가능수량은 계좌를 가진 주문 클라이언트로
    strategy->set_position_provider([this](const std::string& account, const std::string& ticker) {
        return order_gate_.position(account, ticker);
    });
    strategy->set_entry_halt_provider([this] { return order_gate_.is_entry_halted(); });
    strategy->set_entry_scale_provider([this] { return order_gate_.entry_scale(); });
    strategy->set_sellable_provider([this](const std::string& account, const std::string& ticker) {
        return ledger_sellable(account, ticker);
    });
    strategy->set_symbol_resolver([this](std::string_view ticker) { return symbols_.intern(ticker); });

    try
    {
        strategy->on_start();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[Engine] 재스캔 on_start 예외 [" + strategy->id() + "]: " + e.what() + " — 등록 건너뜀");
        return;
    }
    catch (...)
    {
        LOG_ERROR("[Engine] 재스캔 on_start 알 수 없는 예외 [" + strategy->id() + "] — 등록 건너뜀");
        return;
    }

    // 새 전략의 초기 활성은 data_thread가 재스캔 직후 apply_regime_selection(strat_.last_selected_regime)로
    //  맵 기준으로 다시 정한다. 선택 국면을 아직 모르면(기동 직후) 기본 활성. [why D-084]

    // 구독 스펙 추가 (control_thread 재연결 읽기와 겹치므로 watch_specs_mtx_).
    //  REST 폴링 모드면 다음 폴링 사이클부터 현재가를 받는다. WS 모드는 connect()가 기동 때
    //  한 번만 돌아서, 여기서 늘어난 종목은 목록에 넣는 것만으로는 틱이 오지 않는다. 살아 있는
    //  연결에 증분 구독을 걸어 둔다. 이게 없으면 재스캔으로 등록된 전략이 on_data를 한 번도
    //  못 받아 조용히 매매하지 않는다(등록 로그만 남아 정상으로 보인다).
    for (auto& spec : strategy->get_watch_specs())
    {
        if (spec.market == Market::KR)
        {
            universe_rescan_.registered_tickers.insert(spec.ticker);
        }

        bool exists = false;
        {
            std::lock_guard<std::mutex> wl(watch_specs_mtx_);

            for (auto& w : watch_specs_)
            {
                if (w.market == spec.market && w.exchange == spec.exchange && w.ticker == spec.ticker)
                {
                    exists = true;
                    break;
                }
            }

            if (!exists)
            {
                watch_specs_.push_back(spec);
            }
        }

        if (!exists && feed_.ws)
        {
            // false 자체는 정상일 수 있다(연결 전·이미 구독). 목록에서까지 빠졌으면 구독 상한에
            //  밀린 것이고, 그대로 두면 이 종목은 틱 없이 조용히 매매하지 않는다(09-11 실측:
            //  40 초과 종목 체결 0건). 넘침 목록에 넣어 data_thread가 REST로 대신 흘린다.
            if (!feed_.ws->subscribe_incremental(spec) && !feed_.ws->has_spec(spec) && poller_->add_overflow(spec))
            {
                LOG_WARN("[Engine] WS 구독 상한 — " + spec.ticker + " 시세는 REST 폴링으로 대체(넘침 " +
                         std::to_string(poller_->overflow_count()) + "종목)");
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(strat_.mutex);
        strat_.list.push_back(std::move(strategy));
        strat_.version.fetch_add(1, std::memory_order_release);
    }
}

// G1: 국면 r에 맞춰 전략 활성셋을 재선택한다.
//  strat_.has_regime_map이면 국면별 id 목록이 권위적 선택자('*' 접두 매칭으로 스캐너 동적 id 포함),
//  아니면 기존 per-strategy active_regimes 폴백. 선택 결정(활성/비활성 목록)은 국면 변화 또는
//  force_log 시 [RegimeSelect]로 기록 → "왜 이 전략을 켰나"가 로그에 남는다.
//
//  ── 입력은 regime.json 라벨 하나다(D-084) ────────────────────────────────
//  poll_regime_file()이 RISK_ON→BULL·NEUTRAL·RISK_OFF→BEAR로 옮겨 라벨이 바뀐 회차에 부른다.
//  같은 파일이 entry_halt·매수비율·강제청산도 내므로 "무엇을 살까"와 "지금 사도 되나"가 한 입력에서
//  나온다. 코스피 200MA·정배열로 따로 판정하던 축은 이 스위치 말고 하는 일이 없어 지웠다. [why D-084][why D-085]
void Engine::apply_regime_selection(Regime r, bool force_log)
{
    // id 매칭: 목록 항목이 '*'로 끝나면 접두 매칭, 아니면 정확히 일치.
    auto matches = [](const std::string& id, const std::vector<std::string>& sel)
    {
        for (const auto& p : sel)
        {
            if (!p.empty() && p.back() == '*')
            {
                if (id.starts_with(std::string_view(p).substr(0, p.size() - 1)))
                {
                    return true;
                }
            }
            else if (id == p)
            {
                return true;
            }
        }

        return false;
    };

    const std::vector<std::string>* sel = nullptr;

    if (strat_.has_regime_map)
    {
        auto it = strat_.regime_map.find(r);

        if (it != strat_.regime_map.end())
        {
            sel = &it->second; // 없는 국면 키 = 아무 전략도 활성 안 함(전량 비활성)
        }
    }

    auto append = [](std::string& csv, const std::string& id)
    { csv += csv.empty() ? id : ", " + id; };

    std::string active_ids, inactive_ids;

    for (auto& s : strat_.list)
    {
        bool on;

        if (strat_.has_regime_map)
        {
            on = sel && matches(s->id(), *sel);
        }
        else
        {
            // per-strategy 폴백도 같은 입력(r)으로 판정한다. [why D-084]
            const auto ar = s->active_regimes();
            on            = std::find(ar.begin(), ar.end(), r) != ar.end();
        }

        s->set_active(on);
        append(on ? active_ids : inactive_ids, s->id());
    }

#ifdef HAS_ZMQ
    if (zmq_bridge_)
    {
        // FILL 페이로드가 그때그때 이 라벨을 실어 DB의 regime 열을 채운다(주문 시점이 아니라
        // publish 시점 기준 — 국면 전환 중 걸친 체결은 오차가 있을 수 있으나 근사로 충분).
        zmq_bridge_->set_regime_label(regime_bridge::label_of(r));
    }
#endif

    if (force_log || r != strat_.last_selected_regime)
    {
        // 국면은 regime.json 라벨로 적는다(RISK_ON·NEUTRAL·RISK_OFF) — 매매일지·대시보드가 이 값을 읽는다. [why D-085]
        const std::string line = "[RegimeSelect] 국면=" + regime_bridge::label_of(r) + " → 활성=[" + active_ids +
                                 "] 비활성=[" + inactive_ids + "]" +
                                 (strat_.has_regime_map ? "" : " (per-strategy 폴백)");

        // 맵이 있는데 하나도 안 켜지면 오타(접두어·대소문자)나 빈 항목일 수 있어 WARN으로 올린다.
        if (strat_.has_regime_map && active_ids.empty() && !inactive_ids.empty())
        {
            LOG_WARN(line + " — 맵이 등록 전략과 하나도 안 맞음(신규 진입 전면 차단 상태)");
        }
        else
        {
            LOG_INFO(line);
        }
    }

    strat_.last_selected_regime = r;
}

// 기동 유니버스를 마지막 재스캔 슬리브의 소유로 — 스레드 시작 전 호출 전용(strat_.list를 락 없이 읽는다).
void Engine::seed_universe_rescan(const std::vector<std::string>& tickers)
{
    if (universe_rescan_.jobs.empty() || tickers.empty())
    {
        return;
    }

    auto& job = universe_rescan_.jobs.back();
    std::unordered_set<std::string> want(tickers.begin(), tickers.end());

    for (auto& s : strat_.list)
    {
        for (const auto& spec : s->get_watch_specs())
        {
            if (spec.market == Market::KR && want.count(spec.ticker) && !job.owned.count(spec.ticker))
            {
                job.owned[spec.ticker] = s.get();
                ++job.registered;   // 상한은 소유 수로 센다 — 떼면 줄어드는 자리에 기동 종목도 든다
            }
        }
    }

    LOG_INFO("[Engine] 재스캔 소유 시드: " + std::to_string(job.owned.size()) + "종목 (기동 유니버스)");
}

// 주기적 유니버스 재스캔 — universe_fn_으로 티커 목록을 산출해 미등록 종목은 런타임 등록하고,
//  소유 종목이 스캔에서 연속으로 빠지면 block_after_sec에 신규매수를 막고 drop_after_sec에 뗀다(보유·선점 없을 때만).
void Engine::maybe_rescan_universe()
{
    reap_retired(/*force=*/false);

    if (universe_rescan_.jobs.empty())
    {
        return;
    }

    KisClient* scan_kis = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

    if (!scan_kis)
    {
        return;
    }

    const auto now_c = std::chrono::steady_clock::now();

    for (auto& job : universe_rescan_.jobs)
    {
        if (job.interval_sec <= 0 || !job.universe_fn || !job.factory)
        {
            continue;
        }

        if (job.last_run.time_since_epoch().count() != 0 &&
            now_c - job.last_run < std::chrono::seconds(job.interval_sec))
        {
            continue;
        }

        // 첫 부재 스캔의 시계 원점 — 직전 스캔(마지막으로 보인 때)이다. 첫 실행이면 지금.
        const auto prev_run = job.last_run.time_since_epoch().count() != 0 ? job.last_run : now_c;
        job.last_run = now_c;

        std::vector<std::string> tickers;

        try
        {
            tickers = job.universe_fn(*scan_kis);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(std::string("[Engine] 유니버스 재스캔 예외: ") + e.what());
            continue;
        }
        catch (...)
        {
            LOG_ERROR("[Engine] 유니버스 재스캔 알 수 없는 예외");
            continue;
        }

        int  added  = 0;
        bool capped = false;

        // 상한에 닿았을 때 자리를 내줄 후보 — 오늘 스캔 top-N에 없고(점수 밀림) 미보유인 등록 종목.
        //  누적 등록만 세면 한 번 자리 잡은 종목이 오늘 순위와 무관하게 영영 남는다 [why D-087].
        //  이탈 판정(아래)도 같은 스캔·같은 보유 스냅샷을 쓴다 — 두 번 찍으면 그 사이 체결로 어긋날 수 있다.
        std::unordered_set<std::string> in_scan(tickers.begin(), tickers.end());
        std::unordered_set<std::string> held;

        for (const auto& h : order_gate_.snapshot_positions())
        {
            if (h.qty != 0 || order_gate_.reserved(h.account, h.ticker) != 0)
            {
                held.insert(h.ticker);
            }
        }

        auto retire_owned_for_evict = [&](const std::string& tk, const std::string& beneficiary)
        {
            auto oit = job.owned.find(tk);

            if (oit == job.owned.end())
            {
                return;
            }

            StrategyBase* ptr = oit->second;
            ptr->set_active(false);

            std::unique_ptr<StrategyBase> victim;
            uint64_t ver = 0;
            {
                std::lock_guard<std::mutex> lk(strat_.mutex);
                auto sit = std::find_if(strat_.list.begin(), strat_.list.end(),
                                        [ptr](const std::unique_ptr<StrategyBase>& s) { return s.get() == ptr; });

                if (sit != strat_.list.end())
                {
                    victim = std::move(*sit);
                    strat_.list.erase(sit);
                }

                ver = strat_.version.fetch_add(1, std::memory_order_release) + 1;
            }

            if (victim)
            {
                LOG_INFO("[Engine] 재스캔 점수 교체 — 오늘 순위 밖·미보유 해제: " + victim->describe() +
                         " → " + beneficiary);
                strat_.retired.push_back(Retired{std::move(victim), ver});
            }

            universe_rescan_.registered_tickers.erase(tk);
            job.owned.erase(oit);
            job.absent_since.erase(tk);
            job.present_streak.erase(tk);

            if (job.registered > 0)
            {
                --job.registered;
            }
        };

        for (auto& t : tickers)
        {
            if (t.empty() || universe_rescan_.registered_tickers.count(t))
            {
                continue;
            }

            // 상한에 닿으면 오늘 순위 밖·미보유 종목을 찾아 그 자리를 내준다. 없으면 더 등록하지 않는다.
            if (job.max_registered > 0 && job.registered >= job.max_registered)
            {
                const std::string victim_ticker = universe_exit::pick_evict_candidate(
                    job.owned, in_scan, held,
                    [this](const std::string& tk) { return order_gate_.reserved(tk); },
                    [&job, now_c](const std::string& tk) -> long long
                    {
                        auto it = job.absent_since.find(tk);
                        return it == job.absent_since.end()
                                   ? 0LL
                                   : std::chrono::duration_cast<std::chrono::seconds>(now_c - it->second).count();
                    });

                if (victim_ticker.empty())
                {
                    capped = true;
                    break;
                }

                retire_owned_for_evict(victim_ticker, t);
            }

            auto strat = job.factory(t);

            if (!strat)
            {
                continue;
            }

            LOG_INFO("[Engine] 재스캔 신규 등록: " + strat->describe());
            StrategyBase* raw = strat.get();
            register_strategy_runtime(std::move(strat));

            // on_start 예외로 등록이 거부됐으면 universe_rescan_.registered_tickers에 안 들어간다.
            if (universe_rescan_.registered_tickers.count(t))
            {
                job.owned[t] = raw;
                ++added;
                ++job.registered;
            }
        }

        if (added > 0 || capped)
        {
            LOG_INFO("[Engine] 유니버스 재스캔 완료: +" + std::to_string(added) +
                     "종목 (이 슬리브 " + std::to_string(job.registered) + ", 전체 " +
                     std::to_string(universe_rescan_.registered_tickers.size()) + "종목)" +
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
        if (tickers.empty() && !job.owned.empty())
        {
            if (!job.empty_scan_warned)
            {
                LOG_WARN("[Engine] 유니버스 재스캔 결과 없음 — 이탈 판정 건너뜀(소유 " +
                         std::to_string(job.owned.size()) + "종목, 다음 결과까지 유지)");
                job.empty_scan_warned = true;
            }

            continue;
        }

        job.empty_scan_warned = false;

        // 이탈 판정. universe_fn은 보유 종목을 결과에서 이미 빼고 주므로(drop_held) 빠져 있다는
        //  것만으로는 이탈이 아니다 — in_scan·held는 위에서 이미 찍은 같은 스냅샷을 그대로 쓴다.
        const universe_exit::Thresholds th{job.block_after_sec, job.drop_after_sec, job.return_confirm};
        std::vector<std::string> drop;

        for (const auto& [t, ptr] : job.owned)
        {
            // 시계는 하나(연속 부재), 임계값은 둘 — block_after_sec에 신규매수를 막고 drop_after_sec에 뗀다.
            //  복귀는 present가 return_confirm회 연속일 때만 — 경계 종목이 한 번 보이자마자 풀리면 사고팔기를
            //  반복한다. 보유 종목은 스캔 결과에서 빠져 오므로(drop_held) 보유를 이탈로 보지 않는다 —
            //  분할 매수 추가는 전략 자신의 정배열 게이트가 판단한다 [why D-077].
            const bool present = in_scan.count(t) || held.count(t) || order_gate_.reserved(t) != 0;

            if (present)
            {
                job.absent_since.erase(t);

                if (!ptr->in_universe())
                {
                    const int streak = ++job.present_streak[t];

                    if (universe_exit::judge_return(streak, th, /*in_universe=*/false))
                    {
                        ptr->set_in_universe(true);
                        job.present_streak.erase(t);
                        LOG_INFO("[Engine] 유니버스 복귀 → 신규매수 허용: " + ptr->describe() + " (present " +
                                 std::to_string(streak) + "회 연속)");
                    }
                }

                continue;
            }

            job.present_streak.erase(t);
            auto it = job.absent_since.find(t);

            if (it == job.absent_since.end())
            {
                it = job.absent_since.emplace(t, prev_run).first;
            }

            const auto absent_sec = std::chrono::duration_cast<std::chrono::seconds>(now_c - it->second).count();

            switch (universe_exit::judge_absent(absent_sec, th, ptr->in_universe()))
            {
            case universe_exit::Absent::BLOCK:
                ptr->set_in_universe(false);
                LOG_INFO("[Engine] 유니버스 이탈 → 신규매수 차단: " + ptr->describe() + " (" +
                         std::to_string(absent_sec) + "초 부재, 해제는 " + std::to_string(job.drop_after_sec) + "초)");
                break;
            case universe_exit::Absent::DROP:
                drop.push_back(t);
                break;
            case universe_exit::Absent::KEEP:
                break;
            }
        }

        for (const auto& t : drop)
        {
            StrategyBase* ptr = job.owned[t];
            // 옛 스냅샷이 새 스냅샷으로 바뀔 때까지 틱은 계속 온다 — 그 사이 신규매수만 막는다.
            ptr->set_active(false);
            std::unique_ptr<StrategyBase> victim;
            uint64_t ver = 0;
            {
                std::lock_guard<std::mutex> lk(strat_.mutex);
                auto sit = std::find_if(strat_.list.begin(), strat_.list.end(),
                                        [ptr](const std::unique_ptr<StrategyBase>& s) { return s.get() == ptr; });

                if (sit != strat_.list.end())
                {
                    victim = std::move(*sit);
                    strat_.list.erase(sit);
                }

                ver = strat_.version.fetch_add(1, std::memory_order_release) + 1;
            }

            if (victim)
            {
                LOG_INFO("[Engine] 재스캔 이탈 해제: " + victim->describe() + " — " +
                         std::to_string(job.drop_after_sec) + "초 이상 유니버스 밖, 보유·선점 없음");
                strat_.retired.push_back(Retired{std::move(victim), ver});
            }

            universe_rescan_.registered_tickers.erase(t);
            job.owned.erase(t);
            job.absent_since.erase(t);
            job.present_streak.erase(t);

            if (job.registered > 0)
            {
                --job.registered;
            }
        }

        if (!drop.empty())
        {
            LOG_INFO("[Engine] 유니버스 재스캔 해제: -" + std::to_string(drop.size()) +
                     "종목 (이 슬리브 " + std::to_string(job.registered) + ", 전체 " +
                     std::to_string(universe_rescan_.registered_tickers.size()) + "종목)");
        }
    }
}

void Engine::reap_retired(bool force)
{
    if (strat_.retired.empty())
    {
        return;
    }

    const uint64_t seen = strat_.seen_version.load(std::memory_order_acquire);
    auto           keep = strat_.retired.begin();

    for (auto it = strat_.retired.begin(); it != strat_.retired.end(); ++it)
    {
        if (!force && it->ver > seen)
        {
            *keep++ = std::move(*it);
            continue;
        }

        try
        {
            it->strategy->on_stop();
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Engine] 해제 전략 on_stop 예외 [" + it->strategy->id() + "]: " + e.what());
        }
        catch (...)
        {
            LOG_ERROR("[Engine] 해제 전략 on_stop 알 수 없는 예외 [" + it->strategy->id() + "]");
        }

        it->strategy.reset();
    }

    strat_.retired.erase(keep, strat_.retired.end());
}

void Engine::start()
{
    if (running_.load())
    {
        return;
    }

    LOG_INFO("[Engine] ── 퀀트 엔진 시작 ──────────────────────────────");

    // 샤드는 WS 콜백·데이터 스레드가 push 뒤 깨우므로 소켓을 열기 전에 만든다. 스레드는 아래에서 같이 띄운다.
    //  stop() 뒤 다시 start()하면 옛 샤드(스레드는 join 뒤)를 버리고 새로 만든다.
    pipeline_.shard_threads.clear();
    pipeline_.shards.clear();
    uint32_t shard_count = pipeline_.strategy_shards;

    // 한 전략의 종목이 여러 열에 걸치면 샤드 둘이 그 전략을 같이 만진다 — 그 config는 받지 않고 1로 돌린다.
    //  기동 전략은 아직 id가 없어 여기서 intern한다(on_start의 symbol_of와 같은 테이블).
    if (shard_count > 1)
    {
        for (const auto& st : strat_.list)
        {
            if (!strat::owner_shard(*st, shard_count, [this](std::string_view t) { return symbols_.intern(t); }))
            {
                LOG_WARN("[Engine] strategy_shards=" + std::to_string(shard_count) + " 무시 — 전략 " + st->id() +
                         "의 종목이 여러 샤드에 걸친다(또는 구독 종목 없음). 샤드 1개로 돈다");
                shard_count = 1;
                break;
            }
        }
    }

    // [inv] WS 레인 수 = 소켓 수 — 아래 feed_.ws 생성과 같은 조건(리플레이·소켓 하나면 1, feed_keys가 있으면 1+N)이라
    //  feed_.ws->lanes()와 같다. 소켓을 만들기 전에 행 수가 필요해 config로 센다.
    pipeline_.ws_lanes = feed_.feed_override ? feed_.feed_override->lanes()
                : (feed_.replay_file.empty() && !feed_.extra_feed_cfgs.empty()) ? static_cast<uint32_t>(feed_.extra_feed_cfgs.size() + 1)
                                                                      : 1u;
    pipeline_.data_row = pipeline_.ws_lanes;
    pipeline_.ob_mx.reshape(pipeline_.ws_lanes, shard_count, 4096);
    pipeline_.td_mx.reshape(pipeline_.ws_lanes + 1, shard_count, 4096);
    pipeline_.bars_mx.reshape(1, shard_count, 1024);

    for (uint32_t m = 0; m < shard_count; ++m)
    {
        pipeline_.shards.push_back(std::make_unique<strat::Shard>(m, strat::ShardQueues{pipeline_.ob_mx, pipeline_.td_mx, pipeline_.bars_mx}));
    }

    LOG_INFO("[Engine] 전략 샤드 " + std::to_string(shard_count) + "개 (config strategy_shards=" +
             std::to_string(pipeline_.strategy_shards) + ")");

#ifdef HAS_ZMQ
    zmq_bridge_ = std::make_unique<ZmqBridge>();
    zmq_bridge_->set_bind_address(zmq_bind_addr_);
    zmq_bridge_->set_control_token(zmq_control_token_);
    zmq_bridge_->set_account_no(kis_cfg_.account_no); // 실계좌·모의계좌 원장 분리용 [why D-090]
    zmq_bridge_->set_command_handler(
        [this](const std::string& cmd) -> std::string
        {
            if (cmd == "KILL")
            {
                LOG_WARN("[ZMQ] KILL 명령 수신 — 신규 주문 차단 + 엔진 종료");
                order_gate_.set_kill_switch(true);
                request_shutdown();
                return "OK";
            }

            if (cmd == "STATUS")
            {
                return "{\"running\":true"
                       ",\"data\":" +
                       std::to_string(data_count_.load()) + ",\"signal\":" + std::to_string(signal_count_.load()) +
                       ",\"order\":" + std::to_string(order_count_.load()) + "}";
            }

            return "UNKNOWN";
        });
    zmq_bridge_->start();
#endif

    // 피드를 직접 받았거나 캡처 파일을 트는 것이면 브로커 없이 돈다 — feed_.kis는 비고, 아래 KIS를 보는 경로는 전부 null을
    //  "소스 없음"으로 다룬다. 리플레이의 종목은 config tickers(전략 구독)뿐이고 유니버스 스캔·REST 봉 시드는 없다. [why D-071]
    const bool offline = feed_.feed_override != nullptr || !feed_.replay_file.empty();

    if (offline)
    {
        LOG_INFO(std::string("[Engine] ") + (feed_.feed_override ? "피드 주입" : "리플레이") +
                 " — KIS 없이 기동(주문·잔고는 모의 체결기)");
    }
    else
    {
        feed_.kis = std::make_unique<KisClient>(kis_cfg_);

        if (!feed_.kis->authenticate())
        {
            LOG_ERROR("[Engine] KIS 인증 실패");
            return;
        }

        // 시세 전용 클라이언트(실전 도메인) — 모의 시세 REST가 HTTP 500이므로 시세만 실전으로 조회.
        //  실패해도 feed_.kis(모의)로 폴백하되, 모의 시세는 500이라 사실상 틱이 안 나옴을 경고.
        //  WS 모드에서도 만들어 둔다: WS가 죽어 폴링으로 낮출 때 쓸 시세 소스가 그때 가서는 없으면
        //  폴백이 무의미해진다(모의 도메인으로 폴링하면 500만 쌓인다).
        if (feed_.has_quote_kis)
        {
            feed_.quote_kis = std::make_unique<KisClient>(feed_.quote_kis_cfg);

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
    }

    // KIS가 없으면 주문·잔고는 모의 체결기가 받는다. [why D-071]
    if (offline)
    {
        feed_.paper = std::make_unique<feed::PaperExecutor>(feed_.replay_cash);
        LOG_INFO("[Engine] 모의 체결기: 현금 " + std::to_string(static_cast<long long>(feed_.replay_cash)) + "원");
    }

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

    // 잔고 → 원장 대조기. 브로커·라우터·종목명은 함수로 넘겨 대조기가 KisClient·OrderRouter를 모르게 한다. [why D-061]
    ledger_ = std::make_unique<LedgerReconciler>(order_gate_,
                                                 [this] { return feed_.paper ? feed_.paper->balance() : feed_.kis->get_balance(); });
    ledger_->set_account_no(feed_.kis ? feed_.kis->account_no() : std::string("PAPER"));
    ledger_->set_baseline_dir(Logger::instance().base_dir()); // 실행 위치와 무관하게 로그 폴더와 같은 곳
    ledger_->set_name_sink([this](const std::string& t, const std::string& n) { register_ticker_name(t, n); });
    ledger_->set_reconcile_sink([this](const reconcile::Row& r) { order_router_->record_reconcile(r); });

    // REST 현재가 폴러. 시세는 시세 전용 클라이언트가 있으면 그쪽(실전 도메인 초당 한도가 높다). [why D-062]
    //  [lock-order] 데이터 스레드는 pipeline_.td_mx의 WS 레인 행에 넣지 않는다 — 폴러의 틱은 자기 행(pipeline_.data_row)으로 간다.
    poller_ = std::make_unique<DataPoller>(
        [this](const std::string& ticker)
        {
            KisClient* qc = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();
            return qc ? qc->get_current_price(ticker) : 0.0;
        },
        [this](const TradeData& in)
        {
            TradeData td = in;
            td.sym       = symbols_.intern(td.ticker);
            const auto m = pipeline_.td_mx.consumer_of(td.sym);

            while (!pipeline_.td_mx.push_to(pipeline_.data_row, m, td) && running_.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(1ms);
            }

            pipeline_.shards[m]->wake().notify();
        });
    poller_->set_keep_going([this] { return running_.load(std::memory_order_acquire); });

    // G5: 실계좌 보유분을 원장에 시드 (스레드 시작 전, 단일스레드 구간)
    if (bootstrap_ledger_ && !ledger_->bootstrap())
    {
        // running_이 아직 false라 main 루프가 바로 빠지고, 감시자가 5초 뒤 다시 띄운다.
        LOG_ERROR("[Engine] 원장 없이 기동하지 않는다 — 프로세스 종료");
        return;
    }

    // 전략 초기화 (시세 클라이언트 주입 → on_start 내부에서 Universe 조회)
    // 전략의 feed_.kis는 차트(일봉·분봉)·랭킹 등 "읽기 전용 시세 조회"에만 쓰인다(실제 주문 발주는 OrderThread가 담당).
    // 분봉 TR(inquire-time-itemchartprice)은 모의 도메인에서 HTTP 500 → 시세 전용 실전 클라이언트가
    //  있으면 그걸로 조회(regime_·스캐너와 동일 패턴). 없으면 모의로 폴백.
    for (auto& s : strat_.list)
    {
        s->set_kis(feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get());
        // 잔고 조회는 시세 클라이언트가 아니라 계좌를 가진 주문 클라이언트로 한다.
        //  시세 전용 클라이언트에는 account_no가 없어 has_account()가 false가 되고,
        //  그러면 매도가능수량이 항상 0으로 떨어져 익절·존이탈청산·장 마감청산이 전부 발주되지 않는다.
        s->set_account_kis(feed_.kis.get());
        // D2: 확정 포지션 접근자 주입 — 전략이 OrderGate 원장(WS/REST 공용)을 진실원천으로 읽음.
        s->set_position_provider([this](const std::string& account, const std::string& ticker) {
            return order_gate_.position(account, ticker);
        });
        s->set_entry_halt_provider([this] { return order_gate_.is_entry_halted(); });
        s->set_entry_scale_provider([this] { return order_gate_.entry_scale(); });
        s->set_sellable_provider([this](const std::string& account, const std::string& ticker) {
            return ledger_sellable(account, ticker);
        });
        s->set_symbol_resolver([this](std::string_view ticker) { return symbols_.intern(ticker); });

        try
        {
            s->on_start();
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Engine] on_start 예외 [" + s->id() + "]: " + e.what() + " — 전략 건너뜀");
        }
        catch (...)
        {
            LOG_ERROR("[Engine] on_start 알 수 없는 예외 [" + s->id() + "] — 전략 건너뜀");
        }
    }

    // 전략별 구독 스펙 수집 (중복 제거)
    watch_specs_.clear();
    {
        std::unordered_set<std::string> seen;

        for (auto& s : strat_.list)
        {
            for (auto& spec : s->get_watch_specs())
            {
                std::string key = (spec.market == Market::US ? "US:" : "KR:") + spec.exchange + ":" + spec.ticker;

                if (seen.insert(key).second)
                {
                    watch_specs_.push_back(spec);
                }
            }
        }
    }

    LOG_INFO("[Engine] WS 구독 종목: " + std::to_string(watch_specs_.size()) + "개");

    // 재스캔 중복 방지 시드 — 기동 유니버스에 이미 등록된 KR 티커 기록.
    universe_rescan_.registered_tickers.clear();

    for (auto& spec : watch_specs_)
    {
        if (spec.market == Market::KR)
        {
            universe_rescan_.registered_tickers.insert(spec.ticker);
        }
    }

    running_.store(true);

    // 런타임 피드 상태를 config 의도로 초기화. 이후 WS 생사에 따라 control_thread가 토글한다.
    feed_.rest_feed_active.store(feed_.rest_price_feed, std::memory_order_relaxed);

    // WebSocket — 동적 구독 스펙으로 연결.
    //  feed_.rest_price_feed 모드에서는 WS를 열지 않는다: KIS는 app_key당 실시간 1세션만
    //  허용하는데, 세션 정리가 서버측에 걸려 rt=9(ALREADY IN USE) 재연결 폭주가 나므로
    //  체결 피드를 REST 현재가 폴링(data_thread_fn)으로 대체하고 WS 의존을 제거한다.
    //  주문은 REST(order_thread_fn)로 나가므로 매매에는 영향 없음(체결통보 on_fill만 없음).
    if (!feed_.rest_price_feed && !watch_specs_.empty())
    {
        if (feed_.feed_override)
        {
            feed_.ws = std::move(feed_.feed_override);
            LOG_INFO("[Engine] 주입된 피드 소스(레인 " + std::to_string(feed_.ws->lanes()) + "개)");
        }
        else if (!feed_.replay_file.empty())
        {
            feed_.ws = std::make_unique<feed::ReplaySource>(feed_.replay_file, feed_.replay_speed);
            LOG_INFO("[Engine] 리플레이 소스: " + feed_.replay_file + " (speed " + std::to_string(feed_.replay_speed) + ")");
        }
        else if (feed_.extra_feed_cfgs.empty())
        {
            feed_.ws = std::make_unique<KisWebSocket>(kis_cfg_);
        }
        else
        {
            // 소켓 여럿 — 첫 소스가 기본 키다(체결통보는 첫 소스만 받는다). 레인 모드라 소켓 i의 수신 스레드가
            //  행렬의 행 i에 직접 넣는다(mux 스레드 없음). 소켓이 하나면 FeedMux를 끼우지 않는다.
            std::vector<std::unique_ptr<feed::IFeedSource>> socks;
            socks.push_back(std::make_unique<KisWebSocket>(kis_cfg_));

            for (const auto& c : feed_.extra_feed_cfgs)
            {
                socks.push_back(std::make_unique<KisWebSocket>(c));
            }

            feed_.ws = std::make_unique<feed::FeedMux>(std::move(socks));
            LOG_INFO("[Engine] WS 소켓 " + std::to_string(feed_.extra_feed_cfgs.size() + 1) + "개를 FeedMux 레인 " +
                     std::to_string(pipeline_.ws_lanes) + "개로 묶는다");
        }

        // 리플레이를 다시 캡처하면 같은 틱이 두 파일에 남으므로 캡처는 WS일 때만 연다.
        if (!feed_.capture_dir.empty() && feed_.replay_file.empty())
        {
            // 파일명은 UTC 기동 시각 — 재기동이 같은 파일에 이어 쓰지 않도록.
            const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            const std::filesystem::path file =
                std::filesystem::path(feed_.capture_dir) / ("ticks_" + std::to_string(now_s) + ".bin");
            feed_.capture = std::make_unique<feed::TickCapture>(file);

            if (feed_.capture->ok())
            {
                LOG_INFO("[Engine] 틱 캡처 시작: " + file.string());
            }
            else
            {
                LOG_WARN("[Engine] 틱 캡처 파일을 열지 못해 캡처 없이 간다: " + file.string());
            }
        }

        // 레인 = 이 콜백을 부르는 수신 스레드 번호 = 행렬의 행. 한 행은 그 스레드만 넣는다(SPSC 셀, 원칙 5).
        feed_.ws->set_lane_callbacks([this](uint32_t lane, const OrderBook& in)
                           {
                               OrderBook ob = in;
                               ob.sym       = symbols_.intern(ob.ticker);

                               // 수신 스레드가 디코드 시점에 찍은 값을 지킨다. 안 찍힌 소스만 여기서 찍는다.
                               if (ob.recv_ns == 0)
                               {
                                   ob.recv_ns = trace::now_ns();
                               }

                               if (feed_.capture)
                               {
                                   feed_.capture->on_book(ob);
                               }

                               // 호가도 체결과 같은 규칙 — 버린 수를 세고 넘침이 시작될 때 한 번 남긴다.
                               const auto m = pipeline_.ob_mx.consumer_of(ob.sym);

                               if (!pipeline_.ob_mx.push_to(lane, m, ob))
                               {
                                   if (ob_drop_count_.fetch_add(1, std::memory_order_relaxed) == 0)
                                   {
                                       LOG_WARN("[WS] 호가 큐 가득 — 호가 폐기 시작 " + in.ticker.str() +
                                                " (샤드 스레드 정체 의심)");
                                   }

                                   return;
                               }

                               pipeline_.shards[m]->wake().notify();
                           },
                           [this](uint32_t lane, const TradeData& in)
                           {
                               // push가 어차피 한 번 복사하므로 여기서 복사해 id를 찍고 move로 넣는다. 수신 시각은
                               //  수신 스레드가 디코드 시점에 찍은 값을 지키고, 안 찍힌 소스만 여기서 찍는다.
                               TradeData td = in;
                               td.sym       = symbols_.intern(td.ticker);

                               if (td.recv_ns == 0)
                               {
                                   td.recv_ns = trace::now_ns();
                               }

                               if (feed_.capture)
                               {
                                   feed_.capture->on_trade(td);
                               }

                               // 모의 체결은 틱 스레드에서 — 체결통보 큐의 생산자가 이 스레드 하나로 남는다
                               //  (feed_.paper는 리플레이·피드 주입 전용이고 둘 다 레인 하나다).
                               if (feed_.paper)
                               {
                                   feed_.paper->on_tick(in.ticker, in.price, in.hhmmss);
                               }

                               // 전략 스레드가 멈추면 큐가 차고 틱이 여기서 사라진다 — 세어 두고
                               //  넘침이 시작될 때 한 번 남긴다(09-11 15:15 잔고 조회 정체). [why D-055]
                               const auto m = pipeline_.td_mx.consumer_of(td.sym);

                               if (!pipeline_.td_mx.push_to(lane, m, td))
                               {
                                   if (td_drop_count_.fetch_add(1, std::memory_order_relaxed) == 0)
                                   {
                                       LOG_WARN("[WS] 체결 큐 가득 — 틱 폐기 시작 " + in.ticker.str() +
                                                " (샤드 스레드 정체 의심)");
                                   }

                                   return;
                               }

                               pipeline_.shards[m]->wake().notify();
#ifdef HAS_ZMQ
                               if (zmq_bridge_)
                               {
                                   zmq_bridge_->publish_trade(td);
                               }
#endif
                           });
        auto push_fill = [this](const FillNotification& fn)
                               {
                                   // 수신 스레드는 큐에 넣고 바로 돌아간다. 가득 찼으면(1024건 밀림 = 소비자가 멈춘 것)
                                   //  기다리지 않고 버린다 — 여기서 대기하면 전 종목 틱이 같이 선다. 버린 건은
                                   //  잔고 대조(control_thread)가 원장에 메운다. [why D-056]
                                   if (!pipeline_.fill_queue.push(fn))
                                   {
                                       const auto n = pipeline_.fill_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
                                       LOG_ERROR("[Engine] 체결통보 큐 가득 참 — 드롭 " + fn.ticker + " ODNO=" + fn.odno +
                                                 " (누적 " + std::to_string(n) + "건)");
                                       return;
                                   }

                                   pipeline_.fill_wake.notify();
                               };
        feed_.ws->set_fill_callback(push_fill);

        if (feed_.paper)
        {
            feed_.paper->set_fill_callback(push_fill);
        }

        if (!feed_.ws->connect(watch_specs_))
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

    // jthread는 stop_token을 첫 인자로 넣으므로 멤버 함수 포인터(this가 첫 인자)는 람다로 감싼다.
    data_thread_     = std::jthread([this](std::stop_token st) { data_thread_fn(st); });

    for (uint32_t m = 0; m < static_cast<uint32_t>(pipeline_.shards.size()); ++m)
    {
        pipeline_.shard_threads.emplace_back([this, m](std::stop_token st) { shard_thread_fn(st, m); });
    }

    strategy_thread_ = std::jthread([this](std::stop_token st) { strategy_thread_fn(st); });
    order_thread_    = std::jthread([this](std::stop_token st) { order_thread_fn(st); });
    fill_thread_     = std::jthread([this](std::stop_token st) { fill_thread_fn(st); });
    control_thread_  = std::jthread([this](std::stop_token st) { control_thread_fn(st); });

    LOG_INFO("[Engine] 모든 스레드 시작 완료");
}

// ─── 티커→종목명 라벨 (로그 가독성) ─────────────────────────────────────────
//  스캔·청산 관리 부착 스레드가 write, 전략 스레드 신호 로그가 read라 뮤텍스로 보호.
void Engine::register_ticker_name(const std::string& ticker, const std::string& name)
{
    if (name.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lk(ticker_names_mu_);
    ticker_names_[ticker] = name;
}

double Engine::last_px(sym::SymbolId id) const noexcept
{
    return id < symbols_.capacity() ? last_px_arr_[id].load(std::memory_order_relaxed) : 0.0;
}

double Engine::last_px(const std::string& ticker) const
{
    return last_px(symbols_.lookup(ticker));
}

int64_t Engine::last_px_at_ns(const std::string& ticker) const
{
    const auto id = symbols_.lookup(ticker);
    return id < symbols_.capacity() ? last_px_at_ns_[id].load(std::memory_order_relaxed) : 0;
}

void Engine::set_last_px(sym::SymbolId id, double px) noexcept
{
    // id 0(미배선)과 상한 밖은 버린다 — 캐시가 틀리는 것보다 비는 쪽이 낫다.
    if (px <= 0.0 || id == sym::kNone || id >= symbols_.capacity())
    {
        return;
    }

    last_px_arr_[id].store(px, std::memory_order_relaxed);
    last_px_at_ns_[id].store(trace::now_ns(), std::memory_order_relaxed);
}

void Engine::set_last_px(const std::string& ticker, double px)
{
    set_last_px(symbols_.intern(ticker), px);
}

std::string Engine::ticker_label(const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(ticker_names_mu_);
    auto it = ticker_names_.find(ticker);

    if (it != ticker_names_.end() && !it->second.empty())
    {
        return ticker + "(" + it->second + ")";
    }

    return ticker;
}

std::string Engine::ticker_name(const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(ticker_names_mu_);
    auto it = ticker_names_.find(ticker);

    if (it != ticker_names_.end())
    {
        return it->second;
    }

    return std::string();
}

// 전략에 주는 매도가능수량. 게이트 clamp와 같은 식(psbl_cap - pending)이라 전략이 낸 수량이 게이트에서
//  다시 잘리지 않는다. psbl_cap은 잔고 대조(refresh_sellable)가 매 사이클 맞춘다. [why D-055]
StrategyBase::SellableInfo Engine::ledger_sellable(const std::string& account, const std::string& ticker) const
{
    const auto v = order_gate_.sellable_view(account, ticker);
    StrategyBase::SellableInfo r;
    const int room = v.psbl_cap - v.pending;
    r.sellable = room > 0 ? room : 0;
    r.avg_px   = order_gate_.avg_price(account, ticker);
    return r;
}

void Engine::request_shutdown()
{
    running_.store(false, std::memory_order_release);

    for (std::jthread* t : {&data_thread_, &strategy_thread_, &order_thread_, &fill_thread_, &control_thread_})
    {
        t->request_stop(); // WakeGate의 stop_token 대기와 sleep_unless_stopped가 여기서 깬다
    }

    for (auto& t : pipeline_.shard_threads)
    {
        t.request_stop();
    }
}

void Engine::stop()
{
    // 정지 요청과 회수를 나눈다. KILL이 먼저 running_을 내렸어도 join은 여기서 한다 — 예전엔 running_ exchange로
    //  조기 반환해 KILL 뒤 소멸 경로가 join 없이 std::thread를 부쉈다(joinable이면 terminate). 회수는 한 번만: 두 번째
    //  호출은 joinable이 없어 돌아간다.
    request_shutdown();

    if (!data_thread_.joinable())
    {
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

    // 샤드가 먼저 — 샤드가 넣던 봉투를 전략 스레드가 비운 뒤 선다.
    for (auto& t : pipeline_.shard_threads)
    {
        if (t.joinable())
        {
            t.join();
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

    if (feed_.ws)
    {
        feed_.ws->disconnect();
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

    for (auto& s : strat_.list)
    {
        s->on_stop();
    }

    reap_retired(/*force=*/true);
    print_stats();
    LOG_INFO("[Engine] 종료 완료");
}

// ─── 데이터 수집 스레드 ───────────────────────────────────────────────────
void Engine::data_thread_fn(std::stop_token st)
{
    LOG_INFO("[DataThread] 시작");
    bool was_market_open = false;

    while (!st.stop_requested())
    {
        bool market_now = is_any_market_open();

        // 장 시작 감지 → 일별 카운터 리셋 + 국면 판정
        //  "어느 시장이든 닫힘→열림" 전이라 KR 09:00과 US 22:30(KST) 두 번 발화한다. 22:30 리셋은
        //  그날 KR 손익 기록을 지우지만 그 시각 KR 주문은 나가지 않는다(W-9, 시장별 분리는 보류).
        if (market_now && !was_market_open)
        {
            order_gate_.reset_daily();

            if (order_router_)
            {
                order_router_->reset_daily();   // V-4: 중복방지 키 일별 정리(거래일 prefix와 함께 cross-day 충돌 차단)
            }

            ledger_->new_trading_day(); // C-1: 새 거래일 → 총평가금 기준선 재캡처
            LOG_INFO(std::string("[DataThread] 장 개장 전이(") + (is_kr_market_open() ? "KR" : "US") +
                     ") — OrderGate 일별 카운터 리셋");

            // 기동 뒤 첫 개장이면 아직 라벨 전이가 없었을 수 있다. 마지막 선택을 강제 로그로 다시 적용해
            //  "오늘 무엇이 켜져 있나"가 하루 한 줄은 남게 한다.
            if (strat_.last_selected_regime != Regime::UNKNOWN)
            {
                apply_regime_selection(strat_.last_selected_regime, /*force_log=*/true);
            }
        }

        was_market_open = market_now;

        if (!market_now)
        {
            sync::sleep_unless_stopped(st, 60s); // 정지 요청이면 바로 깬다 — 장 외 종료가 60초를 기다리지 않는다
            continue;
        }

        try
        {
            // 매크로 레짐 게이트: 보조 프로세스가 쓴 regime.json → OrderGate entry_halt 토글.
            //  재스캔/잔고 대조와 같은 "사이클 1회" 계층. rest·일봉 모드 공통 경로라 두 모드 다 커버.
            poll_regime_file();

            // 주기적 유니버스 재스캔(동적 등록) — 슬리브별 주기는 각 job이 자체 판단한다.
            if (!universe_rescan_.jobs.empty())
            {
                {
                    maybe_rescan_universe();

                    // G1: 재스캔으로 새로 등록된 전략도 현재 국면 선택에 맞춰 즉시 게이팅
                    //  (기본 active_=true로 잘못된 국면에 진입하는 창을 닫는다). 국면 불변이라
                    //  force_log=false → 로그 노이즈 없음.
                    if (strat_.last_selected_regime != Regime::UNKNOWN)
                    {
                        apply_regime_selection(strat_.last_selected_regime, /*force_log=*/false);
                    }
                }
            }

            const bool rest_now = feed_.rest_feed_active.load(std::memory_order_relaxed);

            // 매 사이클 잔고 대조. 폴링 모드는 원장까지 덮어쓰고(체결콜백 부재 보완),
            //  WS 모드는 총평가금·일손익만 갱신한다. WS 모드에서 이걸 건너뛰면 equity가 0에
            //  머물러 총노출 게이트가 조용히 통과만 하고, 일간손실 한도의 기준값도 안 움직인다.
            ledger_->reconcile(/*resync_positions=*/rest_now, std::time(nullptr));

            // 잔고가 실보유를 바로잡는 자리 옆에서, 라우터가 선점을 바로잡는다. 정본이 서로
            //  다르다 — 실보유는 브로커, 선점은 라우터 이력. 둘 다 슬롯을 세므로 같이 돈다.
            if (order_router_)
            {
                order_router_->sweep_stale_reservations();
            }

            // 틱이 끊긴 보유 종목은 REST 현재가로 보충한다 — 운영단말 현재가가 비어 있던 원인(09-11)은
            //  둘이었다: 유니버스 밖 보유(구독 자체가 없음)와 WS 구독 상한에 밀린 종목. 구독 여부를 따지지
            //  않고 "최근 틱이 없다"로만 고르면 둘 다 잡힌다. 전략이 볼 일은 없으니 td_queue_에는 넣지 않는다.
            //  모의 도메인 초당 한도가 낮아 300ms 간격.
            {
                std::vector<std::string> held;

                for (const auto& h : order_gate_.snapshot_positions())
                {
                    if (h.qty > 0)
                    {
                        held.push_back(h.ticker);
                    }
                }

                std::vector<std::string> stale = poller::select_stale(
                    held,
                    [this](const std::string& t) -> std::optional<std::chrono::steady_clock::time_point>
                    {
                        const int64_t at_ns = last_px_at_ns(t);

                        if (at_ns == 0)
                        {
                            return std::nullopt;
                        }

                        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at_ns));
                    },
                    std::chrono::steady_clock::now() - std::chrono::seconds(60));

                poller_->top_up(stale, [this](const std::string& t, double px) { set_last_px(t, px); });
            }

            if (rest_now)
            {

                // ── 당일 외국인·기관 추정 순매수 "관측 적재"(게이트 아님) ──────────────
                //  data-sourcer 판정: FHPTJ04400000는 추정/가집계 → 부호·상대크기만 신뢰.
                //  게이트로 승격 전, 매 사이클 스냅샷을 로그로 남겨 장중추정 vs 장후확정을
                //  나중에 대조한다(지금 안 남기면 영구 소실). 레이트 절약 위해 5분마다만.
                //  ⚠️ 스키마 미확정 — 첫 성공 응답의 원문 로그로 필드명 확정할 것.
                {
                    static int est_flow_tick = 0;
                    // 수급추정(EstInvestorFlow) 로그 주기. 폴 간격(fetch_interval_sec_)이
                    //  30초일 때 10틱이면 약 5분마다다. 폴 간격을 바꾸면 실제 분 주기도 바뀐다.
                    constexpr int kEstFlowLogEveryNTicks = 10;
                    KisClient* eqc = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

                    if (eqc && (est_flow_tick % kEstFlowLogEveryNTicks) == 0)
                    {
                        // 우리 유니버스(watch) 티커 집합 — 교집합만 강조 로깅.
                        std::unordered_set<std::string> ours;

                        for (const auto& s : watch_specs_)
                        {
                            if (s.market == Market::KR)
                            {
                                ours.insert(s.ticker);
                            }
                        }

                        auto buy_top  = eqc->fetch_est_investor_ranking("0000", "0", "0"); // 순매수 상위
                        std::this_thread::sleep_for(150ms);
                        auto sell_top = eqc->fetch_est_investor_ranking("0000", "1", "0"); // 순매도 상위

                        // 로그 축소: 전체시장 30행 덤프 대신 "우리 유니버스(★) 교집합만" 남긴다.
                        //  fetch(관측 적재)는 그대로 — 로그 볼륨만 스냅샷당 ~61줄→1~4줄로 줄인다.
                        //  전체 랭킹 아카이브가 필요하면 별도 sidecar(logs/supply_*.csv)로 후속 분리.
                        auto dump = [&](const char* label,
                                        const std::vector<KisClient::EstInvestorFlow>& v) -> int
                        {
                            int shown = 0;

                            for (const auto& f : v)
                            {
                                if (ours.count(f.ticker) == 0)
                                {
                                    continue; // 우리 종목만 로깅
                                }

                                int rank = 0;

                                for (const auto& g : v) { ++rank; if (g.ticker == f.ticker) break; }
                                LOG_INFO(std::string("[수급추정] ") + label + " ★" + f.ticker + " " +
                                         f.name + " (전체 " + std::to_string(rank) + "위)" +
                                         " 외인=" + std::to_string(f.foreign_net_qty) +
                                         " 기관=" + std::to_string(f.inst_net_qty) +
                                         " 외인금액=" + std::to_string((int64_t)f.foreign_net_amt));
                                ++shown;
                            }

                            return shown;
                        };
                        LOG_INFO("[수급추정] 스냅샷(관측) 매수상위 " + std::to_string(buy_top.size()) +
                                 "행·매도상위 " + std::to_string(sell_top.size()) + "행 수신");
                        int nb = dump("매수상위", buy_top);
                        int ns = dump("매도상위", sell_top);

                        if (nb + ns == 0)
                        {
                            LOG_INFO("[수급추정]   (우리 유니버스가 외인·기관 상위권 미포함)");
                        }
                    }

                    ++est_flow_tick;
                }

                // ── 섹터(업종) 강약 모니터(관측용) ─────────────────────────────────
                //  업종 지수 등락률을 강→약으로 로깅해 "오늘 어느 섹터가 주도하나"를 눈으로 본다.
                //  코드는 ThemeStrategy.h KOSPI_SECTORS와 동일(실전 시세키로 조회, 5분 주기).
                //  get_index_price(업종코드): inquire-index-price(FID_MRKT_DIV=U) → 등락률.
                {
                    static int sector_tick = 0;
                    KisClient* sqc = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

                    if (sqc && (sector_tick % 10) == 0) // 30s×10 ≈ 5분
                    {
                        // KRX 정본 업종코드. 2026-09-08 구성종목으로 확증했다 — 직전 표는 이름이
                        //  통째로 밀려 있어(0017을 "통신업"으로 불렀는데 실제 구성은 한국전력·
                        //  한국가스공사·지역난방공사, 즉 전기가스업) 관측 로그가 매일 거짓말을 했다.
                        //  0022(은행)·0023은 금융업 통합으로 폐지돼 지수가 0.00이라 뺐다.
                        //  0001~0004(종합·대형·중형·소형주)는 업종이 아니라 규모별 집계라 뺀다.
                        static const std::vector<std::pair<std::string, std::string>> kSectors = {
                            {"0005", "음식료품"},   {"0006", "섬유의복"}, {"0007", "종이목재"},
                            {"0008", "화학"},       {"0009", "의약품"},   {"0010", "비금속광물"},
                            {"0011", "철강금속"},   {"0012", "기계"},     {"0013", "전기전자"},
                            {"0014", "의료정밀"},   {"0015", "운수장비"}, {"0016", "유통업"},
                            {"0017", "전기가스업"}, {"0018", "건설업"},   {"0019", "운수창고"},
                            {"0020", "통신업"},     {"0021", "금융업"},   {"0024", "증권"},
                            {"0025", "보험"},       {"0026", "서비스업"}};

                        struct SecRate { std::string name; double rate; double price; };
                        std::vector<SecRate> secs;

                        for (const auto& [code, name] : kSectors)
                        {
                            std::this_thread::sleep_for(100ms); // rate limit 여유(20업종×100ms=2초)
                            auto ip = sqc->get_index_price(code);

                            if (ip.price > 0.0)
                            {
                                secs.push_back({name, ip.change_rate, ip.price});
                            }
                        }

                        std::ranges::sort(secs, std::ranges::greater{}, &SecRate::rate);

                        // 폭(breadth) 한 줄. 지수 등락률 하나로는 "지수는 빠졌는데 업종 절반이
                        //  플러스"인 회복 초입과 전 업종이 같이 밀리는 진짜 위험회피를 구분할 수
                        //  없다. 지금은 관측 전용이다 — 어떤 판정에도 쓰지 않는다. 회복일과
                        //  데드캣을 사후에 갈라 볼 표본이 쌓이기 전에는 게이트로 승격하지 않는다.
                        //  [why D-033]
                        if (!secs.empty())
                        {
                            int up = 0;

                            for (const auto& s : secs)
                            {
                                if (s.rate > 0.0)
                                {
                                    ++up;
                                }
                            }

                            // 정렬이 끝난 뒤라 중앙값은 가운데 원소다(짝수면 두 값의 평균).
                            const size_t n = secs.size();
                            double med = (n % 2 == 1)
                                             ? secs[n / 2].rate
                                             : (secs[n / 2 - 1].rate + secs[n / 2].rate) / 2.0;
                            char head[128];
                            std::snprintf(head, sizeof(head),
                                          "[섹터] 폭 %d/%zu 플러스, 중앙값 %+.2f%%", up, n, med);
                            LOG_INFO(head);
                        }

                        LOG_INFO("[섹터] ── 업종 등락률(강→약, 관측용) ──");

                        for (const auto& s : secs)
                        {
                            char line[128];
                            std::snprintf(line, sizeof(line), "[섹터] %-8s %+6.2f%%  지수=%.2f",
                                          s.name.c_str(), s.rate, s.price);
                            LOG_INFO(line);
                        }
                    }

                    ++sector_tick;
                }

                // ── 매크로 지표 모니터(관측용) ─────────────────────────────────────
                //  환율·미국지수·미국채10Y금리는 도메스틱 KIS 밖 → 보조 프로세스(macro_regime_feed.py)가
                //  FinanceDataReader로 계산해 regime.json에 쓴 components를 그대로 로깅한다.
                //  키 이름은 NQ_F·ES_F지만 실제 소스는 현물지수 일봉(IXIC·US500)이다. 이 환경에서
                //  yfinance가 전 심볼 실패해 2026-09-04에 FDR로 갈아탄 결과다. 그래서 5개 중 4개는
                //  KST 09:00~15:30 내내 값이 고정된다 — 이 줄들을 장중 신호로 읽지 않는다. [why D-033]
                //  regime.json 미존재(보조 프로세스 미실행) 시 조용히 스킵. entry_halt 게이트와 독립.
                {
                    static int macro_tick = 0;

                    if (!regime_file_.empty() && (macro_tick % 10) == 0) // 30s×10 ≈ 5분
                    {
                        std::error_code mec;

                        if (std::filesystem::exists(regime_file_, mec) && !mec)
                        {
                            try
                            {
                                std::ifstream mf(regime_file_);
                                nlohmann::json mj;

                                if (mf)
                                {
                                    mf >> mj;
                                }

                                if (mj.contains("components"))
                                {
                                    std::string reg = mj.value("regime", std::string("?"));
                                    int score = mj.value("risk_score", 0);
                                    bool valid = mj.value("valid", false);
                                    LOG_INFO("[매크로] ── regime=" + reg + " score=" +
                                             std::to_string(score) +
                                             (valid ? "" : " (valid=false)") + " ──");
                                    // 관심 순서: 환율·나스닥선물·미국채10Y·(참고)S&P선물·VIX
                                    static const std::vector<std::pair<std::string, std::string>> kMacro = {
                                        {"USDKRW", "환율(USD/KRW)"}, {"NQ_F", "나스닥선물"},
                                        {"TNX10", "미국채10Y금리"},  {"ES_F", "S&P500선물"},
                                        {"VIX", "VIX"}};
                                    auto& comps = mj["components"];

                                    for (const auto& [key, label] : kMacro)
                                    {
                                        if (!comps.contains(key))
                                        {
                                            continue;
                                        }

                                        auto& c = comps[key];

                                        if (c.contains("pct") && !c["pct"].is_null())
                                        {
                                            double pct = c["pct"].get<double>();
                                            double price = (c.contains("price") && !c["price"].is_null())
                                                               ? c["price"].get<double>() : 0.0;
                                            int vote = c.value("vote", 0);
                                            char line[160];
                                            std::snprintf(line, sizeof(line),
                                                          "[매크로] %s  %+.2f%%  price=%.2f  vote=%+d",
                                                          label.c_str(), pct, price, vote);
                                            LOG_INFO(line);
                                        }
                                        else
                                        {
                                            LOG_INFO("[매크로] " + label + " = NA(데이터 없음)");
                                        }
                                    }
                                }
                            }
                            catch (const std::exception&)
                            {
                                // 원자적 write라 정상은 완전한 json — 부분/손상은 다음 틱 재시도.
                            }
                        }
                        else if (macro_tick == 0)
                        {
                            LOG_INFO("[매크로] regime.json 없음 — 매크로 표시엔 보조 프로세스"
                                     "(macro_regime_feed.py) 실행 필요");
                        }
                    }

                    ++macro_tick;
                }

                // REST 현재가 폴링 → TradeData(WS on_trade 경로 대체). 깨진 일봉(G1/G2) 대신 살아있는
                //  get_current_price를 쓰고, ITB는 이 틱으로 1분 버킷 채널을 구성/스탑 평가한다.
                //  종전엔 WS와 같은 큐에 넣었는데, WS 폴백 중 WS가 되살아나면 생산자가 둘이 됐다 — 폴러의
                //  싱크는 행렬의 데이터 스레드 행이라 그 경우가 없다. [why D-062]
                data_count_ += poller_->poll_universe(watch_specs_, std::time(nullptr));
            }
            else
            {
                // 차트(일봉) TR은 모의 도메인에서 HTTP 500을 돌려준다. 주문 클라이언트로 부르면
                //  종목 수×사이클마다 500이 쌓여 로그가 그걸로 덮인다(3회 재시도까지 붙는다).
                //  위 rest 분기와 같이 시세 클라이언트로 부른다.
                KisClient* qc = feed_.quote_kis ? feed_.quote_kis.get() : feed_.kis.get();

                // 일봉을 받아 쓰는 전략이 하나도 없으면 폴링 자체를 건너뛴다. DevScale·ITB처럼
                //  호가·체결 이벤트로만 도는 구성에서는 이 루프가 종목 수만큼 차트 TR을 매 사이클
                //  때리고 결과는 아무도 안 본다. 그 호출량이 초당 한도를 밀어 다른 조회(3분봉·현재가)까지
                //  500으로 떨어뜨린다. 전략 집합은 국면 전환으로 바뀌므로 매 사이클 다시 확인한다.
                if (qc && daily_bars_needed())
                {
                    for (const auto& spec : watch_specs_)
                    {
                        std::vector<MarketData> bars;

                        if (spec.market == Market::KR)
                        {
                            // 여기만 당일 봉이 목적이다(파이프라인에 오늘 시세를 흘린다).
                            //  지표·앵커 용도의 다른 호출자는 전부 기본값(전일까지)을 쓴다.
                            bars = qc->get_daily_ohlcv(spec.ticker, 1, /*include_today=*/true);
                        }
                        else
                        {
                            bars = qc->get_us_daily_ohlcv(spec.ticker, 1, spec.exchange);
                        }

                        if (bars.empty())
                        {
                            continue;
                        }

                        auto& md = bars[0];
                        md.bar_index = static_cast<int>(data_count_.load());
                        md.sym       = symbols_.intern(md.ticker);
                        const auto m = pipeline_.bars_mx.consumer_of(md.sym);

                        while (!pipeline_.bars_mx.push_to(0, m, md) && !st.stop_requested())
                        {
                            std::this_thread::sleep_for(1ms);
                        }

                        pipeline_.shards[m]->wake().notify();
                        ++data_count_;
                    }
                }

                // WS 상한에 밀린 종목 — 재구독을 먼저 시도하고(드롭으로 슬롯이 비었을 수 있다) 안 되면 REST로.
                //  rest 분기가 도는 사이클에는 부르지 않는다(그쪽이 이미 전 종목을 폴링한다).
                if (feed_.ws)
                {
                    data_count_ += poller_->poll_overflow(
                        feed_.ws->take_overflow_specs(), [this](const WatchSpec& s) { return feed_.ws->subscribe_incremental(s); },
                        std::time(nullptr));
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[DataThread] 예외: " + std::string(e.what()));
        }

        // 사이클 tail 대기 — 재스캔 주기가 사이클보다 짧으면 그 간격으로 잘게 깨어난다.
        //  maybe_rescan_universe()는 이 루프 안에서만 불리므로, 그냥 두면 재스캔 주기를
        //  아무리 줄여도 fetch_interval_sec_ 단위로 반올림된다(30초 사이클 + 20초 재스캔 = 30초).
        //  잔고 대조·REST 폴백 폴링은 KIS REST를 쓰므로 사이클 주기 그대로 두고,
        //  일봉 캐시와 시세 파일만 보는 재스캔만 앞당긴다.
        {
            const int cycle = fetch_interval_sec_ > 0 ? fetch_interval_sec_ : 1;
            int slice = cycle;

            for (const auto& job : universe_rescan_.jobs)
            {
                if (job.interval_sec > 0 && job.interval_sec < slice)
                {
                    slice = job.interval_sec;
                }
            }

            if (slice < 1)
            {
                slice = 1;
            }

            int slept = 0;

            while (slept < cycle && !st.stop_requested())
            {
                const int step = (slice < cycle - slept) ? slice : (cycle - slept);

                if (!sync::sleep_unless_stopped(st, std::chrono::seconds(step)))
                {
                    break;
                }

                slept += step;

                if (slept < cycle)
                {
                    maybe_rescan_universe();   // 사이클 시작의 호출과 합쳐 재스캔 주기를 지킨다
                }
            }
        }
#ifdef HAS_ZMQ
        if (zmq_bridge_)
        {
            zmq_bridge_->publish_health(data_count_.load(), signal_count_.load(), order_count_.load());
        }
#endif
    }

    LOG_INFO("[DataThread] 종료");
}

// ─── 매크로 레짐 파일 폴링 → OrderGate entry_halt 토글 (data_thread 전용) ─────
//  Python macro_regime_feed.py가 원자적으로 쓰는 regime.json을 매 사이클 읽어,
//  entry_halt(신규 진입만 차단, 청산은 통과)를 국면에 맞춰 켜고 끈다.
//  set_entry_halt는 이 함수가 유일 호출자라 소유권 단순. 파일 없음/손상/
//  판정보류(valid=false)/stale이면 게이트를 새로 켜지 않는다(유지가 실패안전).
//  매크로 risk-off 오버레이 축(G2): entry_halt·force_liquidate(강제청산)를 건다.
//  전략선택 축(apply_regime_selection)과는 별개 관심사다.
//  판정(stale·시간 상자·1회 로그)은 core/RegimeFileBridge.h의 상태기계가 맡는다. [why D-060]
// 스캔 스레드가 슬리브마다 부른다(20초 간격). 파일은 임시 이름으로 쓰고 바꿔치기해
//  대시보드가 반쯤 쓰인 JSON을 읽지 않게 한다. 쓰기 실패는 매매와 무관하므로 경고만 남긴다.
void Engine::set_entry_priority(std::unordered_map<std::string, int> rank,
                                std::unordered_map<std::string, double> z, int total)
{
    nlohmann::json scores = nlohmann::json::object();

    for (const auto& kv : rank)
    {
        auto zit = z.find(kv.first);
        scores[kv.first] = {{"rank", kv.second}, {"z", zit == z.end() ? 0.0 : zit->second}};
    }

    order_gate_.set_entry_priority(std::move(rank), std::move(z), total);

    static std::mutex file_mtx; // 두 슬리브가 겹쳐 불러도 파일은 한 번에 하나만 쓴다
    std::lock_guard<std::mutex> lk(file_mtx);
    const auto path = Logger::instance().base_dir() / "entry_scores.json";
    const auto tmp  = Logger::instance().base_dir() / "entry_scores.json.tmp";
    std::error_code ec;
    {
        std::ofstream of(tmp, std::ios::trunc);

        if (!of.is_open())
        {
            LOG_WARN("[Engine] entry_scores.json 쓰기 실패: " + tmp.string());
            return;
        }

        const auto now = std::chrono::system_clock::now();
        of << nlohmann::json{{"ts", std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()},
                             {"total", total},
                             {"unscored_z", order_gate_.config().displace_unscored_z},
                             {"scores", std::move(scores)}}
                  .dump();
    }

    std::filesystem::rename(tmp, path, ec);

    if (ec)
    {
        LOG_WARN("[Engine] entry_scores.json 교체 실패: " + ec.message());
    }
}

// 파일 관측만 한다(존재·나이·파싱). 판정은 RegimeFileBridge::step이 하고, 여기서는 그 결과를
//  OrderGate·force_liquidate_에 옮기고 로그 문구를 붙인다. 원자적 write라 정상은 완전한 json이고
//  부분/손상은 kUnreadable로 조용히 넘긴다.
static regime_bridge::Observation observe_regime_file(const std::string& path, int stale_sec)
{
    regime_bridge::Observation observation;
    std::error_code ec;

    if (!std::filesystem::exists(path, ec) || ec)
    {
        return observation; // kMissing
    }

    // 갱신 지연: 보조 프로세스가 죽어 파일이 오래되면 신뢰 불가. 수정 시각을 못 읽으면 나이를 모르니 갱신된 것으로 본다.
    auto modified_time = std::filesystem::last_write_time(path, ec);

    if (!ec)
    {
        observation.age_sec = std::chrono::duration_cast<std::chrono::seconds>(
                        std::filesystem::file_time_type::clock::now() - modified_time).count();

        if (observation.age_sec > stale_sec)
        {
            observation.state = regime_bridge::FileState::kStale;
            return observation;
        }
    }

    observation.state = regime_bridge::FileState::kUnreadable;

    try
    {
        std::ifstream file(path);

        if (!file)
        {
            return observation;
        }

        nlohmann::json doc;
        file >> doc;
        observation.snap  = regime_bridge::parse_snapshot(doc);
        observation.state = regime_bridge::FileState::kFresh;
    }
    catch (const std::exception&)
    {
    }

    return observation;
}

void Engine::poll_regime_file()
{
    if (regime_file_.empty())
    {
        return; // 기능 미가동(기본)
    }

    const regime_bridge::Observation obs = observe_regime_file(regime_file_, regime_bridge_.stale_sec());
    const struct tm kst = ::kst::to_tm(std::time(nullptr));
    // 09:00~15:30을 분으로 편 값(is_kr_market_open과 같은 기준). 개장 전은 음수라 안 걸린다.
    const regime_bridge::KstClock clk{kst.tm_yday, kst.tm_hour * 60 + kst.tm_min - 540};
    const regime_bridge::Outcome  out = regime_bridge_.step(obs, clk);

    if (out.log_expiry)
    {
        LOG_WARN("[Regime] 매크로 진입정지 만료 — 개장 후 " + std::to_string(clk.minutes_after_open) +
                 "분 경과. 이 축은 장중 갱신되지 않으므로 오늘 남은 시간의 신규진입 판단은 "
                 "유니버스 지수 게이트와 종목 정배열에 맡긴다");
    }

    if (out.log_stale)
    {
        LOG_WARN("[Regime] regime.json " + std::to_string(obs.age_sec) + "s 경과(> " +
                 std::to_string(regime_bridge_.stale_sec()) +
                 "s) — 보조 프로세스 중단 의심, 게이트 신규 변경 보류(현 halt 유지)");
    }

    // set_entry_halt는 이 함수가 유일 호출자라 소유권이 단순하다.
    if (out.entry_halt)
    {
        order_gate_.set_entry_halt(*out.entry_halt);
    }

    if (out.log_halt_transition)
    {
        LOG_WARN(std::string("[Regime] 신규진입 ") +
                 (*out.entry_halt ? "정지(ENTRY_HALT ON)" : "재개(ENTRY_HALT OFF)") +
                 " — regime=" + obs.snap.regime + " score=" + std::to_string(obs.snap.risk_score));
    }

    // 전략 선택 — 라벨이 바뀐 회차에만. 같은 data_thread라 apply_regime_selection의 strat_.list 순회와 겹치지 않는다.
    if (out.selection)
    {
        apply_regime_selection(*out.selection, /*force_log=*/false);
    }

    // 비율은 halt와 같은 소유권(이 함수만 set). 전략은 다음 계획 회차에 rung 명목에 곱한다.
    if (out.entry_scale)
    {
        order_gate_.set_entry_scale(*out.entry_scale);
    }

    if (out.log_scale_change)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", *out.entry_scale);
        LOG_INFO(std::string("[Regime] 매수비율 ") + buf + " — regime=" + obs.snap.regime +
                 " score=" + std::to_string(obs.snap.risk_score));
    }

    // force_liquidate 배선(G3): 플래그만 세우고 실제 매도는 pipeline_.order_queue 단일 생산자인
    //  strategy_thread가 낸다(SPSC 준수). 여기(data_thread)는 원자 플래그 토글과 1회 로그뿐이다.
    if (out.log_liq_on)
    {
        LOG_ERROR("[Regime] force_liquidate=TRUE (극단 위험회피) — 보유 전량 강제청산 요청, "
                  "strategy_thread가 시장가 매도 발주");
    }

    if (out.log_liq_off)
    {
        LOG_WARN("[Regime] force_liquidate 해제 — 강제청산 중단");
    }

    if (out.force_liquidate)
    {
        force_liquidate_.store(*out.force_liquidate, std::memory_order_relaxed);
    }
}

// ─── 전략 처리 스레드 ─────────────────────────────────────────────────────
// ob_queue_(호가) → td_queue_(체결) → market_queue_(일봉) 순 우선처리
// 아이들 시 100µs 슬립 → 저지연 유지
void Engine::strategy_thread_fn(std::stop_token st)
{
    LOG_INFO("[StrategyThread] 시작");

    // 신호 순번·교체 보류·차단 로그는 이 스레드 소유라 디스패처를 여기에 둔다. 싱크가 pipeline_.order_queue에 넣는 유일한
    //  자리 — 단일 생산자 규약은 이 람다가 이 스레드에서만 불린다는 데 기댄다. [why D-063]
    SignalDispatcher dispatcher(
        order_gate_,
        [this](const OrderSignal& sig)
        {
            ++signal_count_;
#ifdef HAS_ZMQ
            if (zmq_bridge_)
            {
                zmq_bridge_->publish_signal(sig);
            }
#endif
            if (!pipeline_.order_queue.push(sig))
            {
                // 주문 스레드가 KIS 왕복에 묶여 큐가 찬 상태. 여기서 빌 때까지 돌면 전략 스레드가 서고 그 뒤로
                //  호가·체결 큐까지 밀려 판단이 옛 틱으로 흐른다 — 신호를 버리고 센다. 잃는 것은 신호 하나고
                //  조건이 남아 있으면 다음 틱·봉이 다시 만든다(FORCE_LIQ는 2초마다 재발주). [why D-073]
                const auto n = pipeline_.order_dropped.fetch_add(1, std::memory_order_relaxed) + 1;

                if (n == 1 || n % 100 == 0)
                {
                    LOG_WARN("[전략] 주문 큐 가득 — 신호 버림 " + sig.ticker + " " +
                             (sig.side == OrderSide::BUY ? "BUY" : "SELL") + " (누적 " + std::to_string(n) + ")");
                }

                return;
            }

            pipeline_.order_wake.notify();
        },
        std::chrono::steady_clock::now());
    dispatcher.set_label([this](const std::string& t) { return ticker_label(t); });
    dispatcher.set_guardian([this](const std::string& t) { return universe_rescan_.guardian_tickers.count(t) > 0; });
    auto push_signal = [&](const OrderSignal& sig) { dispatcher.submit(sig); };

    // 기동 점검 — 모의계좌 주문경로 검증용 1회성 시장가 매수(config startup_probe).
    //  이 스레드가 pipeline_.order_queue 단일 생산자라 여기서 딱 1번 push하면 SPSC 위반 없음.
    //  하루 한 번만 — 표식 파일이 있으면 재기동에서는 건너뛴다. 체결이 확인되면 되판다(아래 루프).
    //  장 전(09:00 전)에는 내지 않는다 — 08:5x 기동에서 시장가가 40570000(장전 거부)으로 튕겼는데 표식은
    //  남아 그날 점검이 없었다(09-14). 루프에서 09:00 이 되면 낸다. 120초 안에 체결이 없으면(거부·미체결)
    //  표식을 지워 다음 기동에서 다시 낸다.
    const auto startup_probe_marker = Logger::instance().path_for("startup_probe_" + ledger::kst_ymd(std::time(nullptr)));
    auto fire_startup_probe = [&]()
    {
        startup_probe_fired_ = true;
        std::error_code ec;

        if (std::filesystem::exists(startup_probe_marker, ec))
        {
            LOG_INFO("[Engine] 기동 점검 — 오늘 이미 냈다(" + startup_probe_marker.filename().string() + "), 건너뜀");
            return;
        }

        std::ofstream(startup_probe_marker) << "fired\n";
        int base = 0;

        for (const auto& h : order_gate_.snapshot_positions())
        {
            if (h.ticker == startup_probe_ticker_)
            {
                base += h.qty;
            }
        }

        OrderSignal probe;
        probe.ticker      = startup_probe_ticker_;
        probe.side        = OrderSide::BUY;
        probe.type        = OrderType::MARKET;
        probe.quantity    = startup_probe_qty_;
        probe.price       = 0.0;
        probe.strategy_id = "STARTUP_PROBE";
        LOG_INFO("[Engine] 기동 점검 — " + probe.ticker + " 시장가 BUY " +
                 std::to_string(probe.quantity) + "주 (모의계좌 주문경로 검증, 체결되면 되판다)");
        push_signal(probe);
        startup_probe_base_qty_ = base;
        startup_probe_fired_at_ = std::chrono::steady_clock::now();
        startup_probe_settled_  = false;
    };

    // 틱은 샤드 스레드가 돌린다(shard_thread_fn). 여기는 샤드가 보낸 봉투와 수동주문을 디스패처 한 곳으로 모아
    //  순번·슬롯·교체·강제청산 같은 종목 횡단 판단을 한 스레드에서 한다(원칙 4). [why D-071]
    // 유휴 전이: 마지막 일 뒤 이 시간은 yield로 돌고, 넘기면 pipeline_.strat_wake에서 잔다. [why D-071]
    constexpr auto kStratSpinBudget = std::chrono::microseconds(200);
    std::chrono::steady_clock::time_point idle_since{};

    while (!st.stop_requested())
    {
        const auto loop_now = std::chrono::steady_clock::now();
        dispatcher.flush_held(loop_now);

        // 운영단말 수동주문 — 소켓 스레드가 넣은 요청을 여기서 OrderSignal로 바꾼다(단일 생산자).
        drain_manual_inbox(push_signal);

        // G3 강제청산 — force_liquidate 동안 보유 전량(미체결 매도 제외) 시장가 매도를 2초마다 다시 낸다.
        if (force_liquidate_.load(std::memory_order_relaxed))
        {
            dispatcher.force_liquidate(loop_now);
        }

        // 기동 점검 — 09:00~15:20 KST 안에서만 낸다. 그 전 기동은 루프가 09:00 을 넘길 때 낸다.
        if (!startup_probe_fired_ && !startup_probe_ticker_.empty() && startup_probe_qty_ > 0)
        {
            const int32_t hhmmss = kst::hhmmss_int(std::time(nullptr));

            if (hhmmss >= 90000 && hhmmss < 152000)
            {
                fire_startup_probe();
            }
        }

        // 기동 점검 되팔기 — 보유가 base+qty 이상이면 체결로 보고 같은 수량을 시장가로 판다.
        //  체결통보를 직접 보지 않고 원장 수량으로 판정한다(원장이 진실원천). 120초 안에 안 늘면
        //  접수 거부·미체결로 보고 포기하고 표식을 지운다 — 다음 기동에서 다시 낸다.
        if (!startup_probe_settled_)
        {
            const auto  now_p = std::chrono::steady_clock::now();
            int         qty   = 0;
            std::string account;

            for (const auto& h : order_gate_.snapshot_positions())
            {
                if (h.ticker == startup_probe_ticker_)
                {
                    qty += h.qty;
                    account = h.account;
                }
            }

            if (qty >= startup_probe_base_qty_ + startup_probe_qty_)
            {
                OrderSignal back;
                back.ticker      = startup_probe_ticker_;
                back.account_id  = account;
                back.side        = OrderSide::SELL;
                back.type        = OrderType::MARKET;
                back.quantity    = startup_probe_qty_;
                back.price       = 0.0;
                back.ref_price   = order_gate_.avg_price(account, startup_probe_ticker_);
                back.strategy_id = "STARTUP_PROBE";
                back.reason      = "기동 점검 되팔기";
                LOG_INFO("[Engine] 기동 점검 체결 확인(보유 " + std::to_string(startup_probe_base_qty_) +
                         "→" + std::to_string(qty) + ") — " + back.ticker + " 시장가 SELL " +
                         std::to_string(back.quantity) + "주로 되판다");
                push_signal(back);
                startup_probe_settled_ = true;
            }
            else if (now_p - startup_probe_fired_at_ > std::chrono::seconds(120))
            {
                std::error_code ec;
                std::filesystem::remove(startup_probe_marker, ec);
                LOG_WARN("[Engine] 기동 점검 120초 안에 체결 확인 못 함(보유 " + std::to_string(qty) +
                         ") — 되팔기 생략, 표식을 지워 다음 기동에서 다시 낸다");
                startup_probe_settled_ = true;
            }
        }

        // 종목당 명목 한도 초과분 정리 — 기동 20초 뒤(잔고 시드가 끝난 뒤) 한 번.
        dispatcher.trim_excess_once(loop_now);

        bool did_work = false;

        try
        {
            // 샤드가 보낸 신호 봉투 — 국면 게이트(비활성 전략의 신규 매수)와 청산 관리 티커 차단은 디스패처가 한다.
            while (auto e = pipeline_.shard_out.pop())
            {
                dispatcher.from_strategy(e->active, e->strategy_id, e->sig);
                did_work = true;
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[StrategyThread] 예외: " + std::string(e.what()));
        }

        if (did_work)
        {
            idle_since = std::chrono::steady_clock::time_point{};
            continue;
        }

        // 유휴: 짧게 돌다가 잔다. 틱이 몰리는 구간은 spin 안에서 받고, 조용하면 생산자 notify로 깬다.
        //  상한 10ms는 flush_held·강제청산 2초 주기 같은 시각 기반 작업의 해상도다(예전 100us sleep이 실측 15.6ms였다).
        const auto now_i = std::chrono::steady_clock::now();

        if (idle_since == std::chrono::steady_clock::time_point{})
        {
            idle_since = now_i;
        }

        if (now_i - idle_since < kStratSpinBudget)
        {
            std::this_thread::yield();
            continue;
        }

        pipeline_.strat_wake.wait_for(10ms, st, [this] { return pipeline_.shard_out.empty() && ops_.manual_inbox.empty(); });
    }

    LOG_INFO("[StrategyThread] 종료");
}

void Engine::shard_thread_fn(std::stop_token st, uint32_t m)
{
    auto& shard = *pipeline_.shards[m];
    LOG_INFO("[Shard " + std::to_string(m) + "] 시작");

    // strat_.list 무락 순회용 StrategyBase* 스냅샷. data_thread의 재스캔 등록·해제가
    // strat_.version을 올릴 때만 락 하에 재구성한다(틱마다 락 회피). 뗀 전략은 strat_.retired가
    // 붙들고 있어 재구성 전의 옛 포인터도 유효하다(reap_retired가 seen 버전을 보고 파기).
    std::vector<StrategyBase*> snap;
    uint64_t                   seen_ver = static_cast<uint64_t>(-1);
    const auto                 sym_of   = [this](std::string_view t) { return symbols_.intern(t); };

    // 신호 봉투 — 전략 상태(active·id)는 여기서 읽는다. 전략 스레드는 전략 객체를 보지 않는다.
    //  tick_ns는 체결 경로만 0이 아니다 — 봉·호가는 CSV에서 -1(측정 불가)로 남는다.
    const auto emit = [this, m](StrategyBase* s, const OrderSignal& sig, int64_t tick_ns)
    {
        strat::Emitted e;
        e.sig           = sig;
        e.sig.t_tick_ns = tick_ns;

        // 전략이 안 찍었으면 여기서 한 번. 신호 종목이 지금 틱과 다를 수 있어(테마·청산) 틱 id를 그대로 쓰지 않는다.
        if (e.sig.sym == sym::kNone)
        {
            e.sig.sym = symbols_.intern(e.sig.ticker);
        }

        e.strategy_id = s->id();
        e.active      = s->is_active();

        // 전략 스레드가 정체돼 봉투 큐가 찬 상태 — 기다리면 이 샤드의 틱이 밀린다. 신호를 버리고 센다(D-073과 같은 규칙).
        if (!pipeline_.shard_out.push(std::move(e)))
        {
            const auto n = pipeline_.shard_dropped.fetch_add(1, std::memory_order_relaxed) + 1;

            if (n == 1 || n % 100 == 0)
            {
                LOG_WARN("[Shard " + std::to_string(m) + "] 봉투 큐 가득 — 신호 버림 " + sig.ticker + " (누적 " +
                         std::to_string(n) + ")");
            }

            return;
        }

        pipeline_.strat_wake.notify();
    };
    // 운영단말 현재가용 캐시 — id 배열에 relaxed store 둘. 종목은 샤드 하나만 지나므로 쓰는 스레드도 하나다.
    const auto on_price = [this](sym::SymbolId id, double px) { set_last_px(id, px); };

    // 유휴 전이: 전략 스레드와 같은 정책 — 200us yield 뒤 자기 게이트에서 잔다. [why D-071]
    constexpr auto                        kSpinBudget = std::chrono::microseconds(200);
    std::chrono::steady_clock::time_point idle_since{};

    while (!st.stop_requested())
    {
        const uint64_t ver = strat_.version.load(std::memory_order_acquire);

        if (ver != seen_ver)
        {
            {
                std::lock_guard<std::mutex> lk(strat_.mutex);
                snap.clear();
                snap.reserve(strat_.list.size());
                const auto shard_count = static_cast<uint32_t>(pipeline_.shards.size());

                // 자기 열의 전략만 — 다른 열의 전략은 이 샤드에 틱이 오지 않으니 라우터에 둘 이유가 없다. M=1이면 전부.
                for (auto& s : strat_.list)
                {
                    if (strat::owner_shard(*s, shard_count, sym_of).value_or(0u) == m)
                    {
                        snap.push_back(s.get());
                    }
                }

                shard.rebuild(snap, ver, sym_of);
            }

            LOG_INFO("[Shard " + std::to_string(m) + "] 라우팅 재구성: 전략 " + std::to_string(snap.size()) +
                     "개, 종목 배정 " + std::to_string(shard.router().routes()) + "건, 전부 받는 전략 " +
                     std::to_string(shard.router().all_count()) + "개");
            seen_ver = ver;
            // 뗀 전략은 모든 샤드가 새 스냅샷을 본 뒤에야 파기한다 — 가장 뒤처진 샤드의 버전을 알린다.
            uint64_t min_seen = ver;

            for (const auto& other : pipeline_.shards)
            {
                min_seen = std::min(min_seen, other->seen_version());
            }

            strat_.seen_version.store(min_seen, std::memory_order_release);
        }

        bool did_work = false;

        try
        {
            did_work = shard.step(emit, on_price, sym_of);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Shard " + std::to_string(m) + "] 예외: " + std::string(e.what()));
        }

        if (did_work)
        {
            idle_since = std::chrono::steady_clock::time_point{};
            continue;
        }

        const auto now_i = std::chrono::steady_clock::now();

        if (idle_since == std::chrono::steady_clock::time_point{})
        {
            idle_since = now_i;
        }

        if (now_i - idle_since < kSpinBudget)
        {
            std::this_thread::yield();
            continue;
        }

        shard.wake().wait_for(10ms, st, [&shard] { return shard.empty(); });
    }

    LOG_INFO("[Shard " + std::to_string(m) + "] 종료");
}

// ─── 주문 실행 스레드 ─────────────────────────────────────────────────────
void Engine::order_thread_fn(std::stop_token st)
{
    using std::chrono::steady_clock;
    LOG_INFO("[OrderThread] 시작");

    // 발주 간격과 거부 재시도는 이 스레드 소유라 조절기를 여기에 둔다. pipeline_.order_queue는 SPSC(생산자=전략 스레드)라
    //  되밀 수 없어 재시도는 조절기의 전용 버퍼에 산다. [why D-065]
    OrderPacer pacer({order_min_interval_ms_, order_max_retries_}, steady_clock::now());
    pacer.set_position([this](const std::string& a, const std::string& t) { return order_gate_.position(a, t); });

    // 구간 지연 CSV. 이 스레드만 쓰므로 지역 객체로 두고, 첫 주문 때 파일을 연다. [why D-071]
    trace::LatencyTrace lat_trace(Logger::instance().path_for("latency_trace.csv"));

    while (!st.stop_requested())
    {
        // 발주 대상 선택: 만기된 재시도분 우선, 없으면 신규 큐
        std::optional<OrderPacer::Pending> next = pacer.take_due_retry(steady_clock::now());
        int64_t                            pop_ns = 0;

        if (!next)
        {
            if (auto opt = pipeline_.order_queue.pop())
            {
                next   = OrderPacer::Pending{*opt, 0};
                pop_ns = trace::now_ns();
            }
        }

        if (!next)
        {
            // 재시도 만기가 있으면 그 시각까지, 없으면 100ms 상한(종료 확인). 신규 신호는 전략 스레드의 notify가 깨운다.
            const auto deadline = pacer.next_retry_at().value_or(steady_clock::now() + 100ms);
            pipeline_.order_wake.wait_until(deadline, st, [this] { return pipeline_.order_queue.empty(); });
            continue;
        }

        // 호출 간격 조절 — 직전 KIS 발주 후 min_interval 경과 보장(초당한도 하회로 EGW00201 회피)
        if (const auto wait = pacer.wait_before_send(steady_clock::now()); wait > steady_clock::duration::zero())
        {
            std::this_thread::sleep_for(wait);
        }

        const OrderSignal& sig = next->sig;

        try
        {
            // 간격은 KIS를 실제로 부른 뒤에만 센다. 로컬 거부(게이트·ENTRY_HALT)는 한도와 무관하다.
            const uint64_t calls_before = order_router_->kis_calls();
            auto mo = order_router_->submit(sig);
            const bool kis_called = order_router_->kis_calls() != calls_before;

            if (kis_called)
            {
                pacer.note_sent(steady_clock::now());
            }

            // 재시도 건은 pop 시각이 첫 시도 것이라 구간이 부풀지 않게 첫 시도만 남긴다.
            if (next->attempts == 0)
            {
                lat_trace.record(sig, trace::Marks{sig.t_tick_ns, sig.t_signal_ns, pop_ns, trace::now_ns()}, kis_called,
                                 mo.status == OrderStatus::ACCEPTED);
            }

            // 단말이 없으면 JSON 직렬화를 건너뛴다 — 주문 스레드 hot path에서 받는 이 없는 문자열을 만들지 않는다.
            //  client_count()는 뮤텍스 한 번이지만 직렬화보다 싸다. [why D-071]
            if (ops_.server && ops_.server->client_count() > 0)
            {
                // 게이트·브로커를 지난 최종 결과. 단말은 cid로 자기 ORDER_ACK와 잇고, 전략 주문도
                //  같은 채널로 보여 운영 화면이 자동매매를 함께 본다.
                ops_.server->broadcast(ops::OpsMsg::ORDER_RESULT,
                                       nlohmann::json{{"cid", sig.client_oid},
                                                      {"order_id", mo.order_id},
                                                      {"odno", mo.kis_order_no},
                                                      {"strategy", sig.strategy_id},
                                                      {"ticker", sig.ticker},
                                                      {"side", sig.side == OrderSide::BUY ? "BUY" : "SELL"},
                                                      {"qty", sig.quantity},
                                                      {"price", sig.price},
                                                      {"ok", mo.status == OrderStatus::ACCEPTED},
                                                      {"msg", mo.reject_reason}}
                                           .dump());
            }

            if (mo.status == OrderStatus::ACCEPTED)
            {
                ++order_count_;
            }
            else
            {
                pacer.on_rejected(*next, mo.status, mo.reject_reason, steady_clock::now());
            }
        }
        catch (const std::exception& e)
        {
            pacer.note_sent(steady_clock::now());
            LOG_ERROR("[OrderThread] 예외: " + std::string(e.what()));
        }
    }

    LOG_INFO("[OrderThread] 종료");
}

// 체결통보 소비 전용 스레드. 주문 스레드에 얹지 않은 이유: 주문 스레드는 KIS 발주(REST, 수십~수백 ms)와 발주 간격
//  대기에 묶여 있는 시간이 길어, 그 뒤에 선 체결이 원장에 늦게 들어가고 다음 SELL의 보유 수량 판단이 그만큼 낡는다.
//  큐가 비면 condvar에서 자고 WS 콜백이 깨운다 — 1ms 폴링은 Windows에서 실측 8~15ms 늦었다(test_pipeline_stress).
//  on_fill이 던지면 스레드가 죽어 이후 체결이 전부 큐에 쌓이므로 건마다 잡아 로그로 남긴다. [why D-056]
void Engine::fill_thread_fn(std::stop_token st)
{
    LOG_INFO("[FillThread] 시작");

    // 정지 요청 뒤에도 큐를 비운다 — stop()이 WS를 끊은 다음 join하므로 남은 통보가 여기서 빠진다.
    while (!st.stop_requested() || !pipeline_.fill_queue.empty())
    {
        auto opt = pipeline_.fill_queue.pop();

        if (!opt)
        {
            // 상한 100ms는 신호가 샐 때의 보험이다. 깨우는 것은 WS 콜백의 notify와 정지 요청.
            pipeline_.fill_wake.wait_for(100ms, st, [this] { return pipeline_.fill_queue.empty(); });
            continue;
        }

        const FillNotification& fn = *opt;

        try
        {
            if (order_router_)
            {
                order_router_->on_fill(fn);
            }

            // 체결 직후 몇 초는 잔고 대조를 미룬다 — 잔고 스냅샷이 체결을 따라오기 전이다. [why D-074]
            if (ledger_)
            {
                ledger_->note_fill(std::time(nullptr));
            }

            if (ops_.server && ops_.server->client_count() > 0)
            {
                ops_.server->broadcast(ops::OpsMsg::FILL,
                                       nlohmann::json{{"odno", fn.odno},
                                                      {"ticker", fn.ticker},
                                                      {"side", fn.side == OrderSide::BUY ? "BUY" : "SELL"},
                                                      {"qty", fn.filled_qty},
                                                      {"price", fn.filled_price},
                                                      {"time", fn.fill_time}}
                                           .dump());
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[FillThread] 체결 반영 예외 " + fn.ticker + " ODNO=" + fn.odno + ": " + e.what());
        }
    }

    LOG_INFO("[FillThread] 종료 (드롭 " + std::to_string(pipeline_.fill_dropped.load(std::memory_order_relaxed)) + "건)");
}

// ─── 장 시간 체크 (kst::to_tm — 머신 TZ 무관) ───────────────────────────
bool Engine::daily_bars_needed()
{
    std::lock_guard<std::mutex> lk(strat_.mutex);

    for (const auto& s : strat_.list)
    {
        if (s && s->is_active() && s->wants_daily_bars())
        {
            return true;
        }
    }

    return false;
}

bool Engine::is_kr_market_open() const
{
    const auto kst = ::kst::to_tm(std::time(nullptr));

    if (kst.tm_wday == 0 || kst.tm_wday == 6)
    {
        return false;
    }

    int m = kst.tm_hour * 60 + kst.tm_min;
    return m >= 540 && m < 930; // 09:00~15:30 KST
}

// 미국 정규장: ET 09:30~16:00 = KST 22:30~05:00 (다음날)
bool Engine::is_us_market_open() const
{
    const auto kst = ::kst::to_tm(std::time(nullptr));

    if (kst.tm_wday == 0 || kst.tm_wday == 6)
    {
        return false;
    }

    int m = kst.tm_hour * 60 + kst.tm_min;
    // 미국 정규장(KST 22:30~익일 05:00): 하루 분(min) 기준으로 당일 1350~1439분 또는
    //  익일 0~299분(05:00 직전까지). 하루는 최대 1439분이라 1500분은 존재하지 않는다.
    return (m >= 1350) || (m < 300);
}

bool Engine::is_any_market_open() const
{
    return is_kr_market_open() || is_us_market_open();
}

void Engine::print_stats() const
{
    LOG_INFO("[Engine] 수집: " + std::to_string(data_count_.load()) +
             "  신호: " + std::to_string(signal_count_.load()) + "  주문: " + std::to_string(order_count_.load()));
}

// ─── ZMQ 제어 스레드 ──────────────────────────────────────────────────────
// ZmqBridge 자체 스레드가 REP 소켓을 처리하므로 이 스레드는
// running_ 감시 + WebSocket stale 감지를 담당
// ─── WS 피드 끊김 시 REST 폴링으로 낮추기 / 복귀 ──────────────────────────────
//  WS 모드에서 호가·체결이 끊기면 전략은 입력을 하나도 못 받는다(폴링 분기가 꺼져 있으므로).
//  매매를 세우는 대신 data_thread의 폴링 분기를 켜서 30초 주기 현재가 틱으로 이어간다.
//  느려진 대가는 있지만 눈이 아주 감기는 것보다는 낫다는 판단이다.
bool Engine::activate_rest_fallback(const std::string& reason)
{
    if (feed_.rest_price_feed)
    {
        return true; // 처음부터 폴링 — 낮출 것이 없다
    }

    // 폴링이 쓸 시세 소스. 모의 도메인은 시세 REST가 HTTP 500이라 실전 시세 클라이언트가
    //  없고 주문계좌마저 모의면 낮춰봐야 틱이 안 나온다. 그때는 거짓 안심을 주지 않는다.
    //  브로커 없는 기동(피드 주입)도 같다 — 낮출 REST가 없다.
    if (!feed_.kis || (!feed_.quote_kis && kis_cfg_.is_paper))
    {
        return false;
    }

    if (!feed_.rest_fallback_engaged)
    {
        feed_.rest_fallback_engaged = true;
        feed_.rest_feed_active.store(true, std::memory_order_relaxed);
        LOG_ERROR("[Feed] WS → REST 폴링 폴백 (" + reason + ") — 틱 주기가 " +
                  std::to_string(fetch_interval_sec_) + "초로 떨어집니다. WS 복귀 시 자동 원복");
    }

    return true;
}

void Engine::deactivate_rest_fallback()
{
    if (!feed_.rest_fallback_engaged)
    {
        return;
    }

    feed_.rest_fallback_engaged = false;
    feed_.rest_feed_active.store(feed_.rest_price_feed, std::memory_order_relaxed);
    LOG_INFO("[Feed] WS 수신 정상 — REST 폴링 폴백 해제, 실시간 피드로 복귀");
}

void Engine::control_thread_fn(std::stop_token st)
{
    using namespace std::chrono_literals;
    constexpr int kCheckIntervalSec = 5;
    // 큐 고수위는 장 외에도 찍는다 — 큐 크기가 맞는지의 근거가 되므로 WS 유무·개장 여부와 무관하다. [why D-071]
    constexpr int kHighWaterEvery = 12; // 5초 × 12 = 1분
    int hw_tick = 0;
    constexpr int kTokenEvery = 60; // 5초 × 60 = 5분
    int token_tick = 0;

    while (sync::sleep_unless_stopped(st, std::chrono::seconds(kCheckIntervalSec)))
    {

        if (++hw_tick >= kHighWaterEvery)
        {
            hw_tick = 0;
            std::string shard_hw; // 샤드마다 세 열 가운데 가장 높았던 셀

            for (const auto& sh : pipeline_.shards)
            {
                shard_hw += (shard_hw.empty() ? "" : ",") + std::to_string(sh->high_water());
            }

            LOG_INFO("[큐 고수위] shard=" + shard_hw + "/4096 shard_out=" + std::to_string(pipeline_.shard_out.size()) + "/" +
                     std::to_string(pipeline_.shard_out.capacity()) + " shard_dropped=" +
                     std::to_string(pipeline_.shard_dropped.load(std::memory_order_relaxed)) + " order=" +
                     std::to_string(pipeline_.order_queue.high_water()) + "/" + std::to_string(pipeline_.order_queue.capacity()) +
                     " fill=" + std::to_string(pipeline_.fill_queue.high_water()) + "/" + std::to_string(pipeline_.fill_queue.capacity()) +
                     " fill_dropped=" + std::to_string(pipeline_.fill_dropped.load(std::memory_order_relaxed)) +
                     " order_dropped=" + std::to_string(pipeline_.order_dropped.load(std::memory_order_relaxed)));
        }

        if (++token_tick >= kTokenEvery)
        {
            token_tick = 0;

            // 만료 30분 전에 여기서 미리 갱신한다. 발급 왕복을 파이프라인 밖 스레드가 떠안아야
            //  전략·데이터 스레드의 http_get이 5분 margin에 걸리지 않는다. [why D-073]
            if (feed_.kis)
            {
                feed_.kis->refresh_token(std::chrono::minutes(30));
            }

            if (feed_.quote_kis)
            {
                feed_.quote_kis->refresh_token(std::chrono::minutes(30));
            }
        }

        if (!feed_.ws)
        {
            continue;
        }

        // 장 외 시간에는 stale이 정상 — 장 중에만 묻는다. 전이 판정은 감독기, 소켓·폴백 적용은 여기. [why D-071]
        const bool market_open = is_any_market_open();
        const bool stale       = market_open && feed_.ws->is_stale(feed_.feed_sup.config().stale_sec);
        const auto step        = feed_.feed_sup.observe(market_open, stale, std::chrono::steady_clock::now());

        if (step == feed::Supervisor::Step::kHealthy)
        {
            deactivate_rest_fallback(); // 폴백으로 낮춰 뒀다면 WS로 되돌린다(전이 시에만 동작)
            continue;
        }

        if (step != feed::Supervisor::Step::kReconnect)
        {
            continue;
        }

        LOG_WARN("[Control] WebSocket " + std::to_string(feed_.feed_sup.config().stale_sec) +
                 "초 이상 시세 미수신 — 재연결 시도");
        std::vector<WatchSpec> specs_copy;
        {
            std::lock_guard<std::mutex> wl(watch_specs_mtx_); // data_thread의 재스캔 push_back과 겹친다
            specs_copy = watch_specs_;
        }

        // 소켓이 여럿이면 멈춘 것만 다시 잇는다 — 살아 있는 소켓의 종목은 그 사이에도 틱이 흐른다. 하나면 끊고 다시 잇는 것.
        const bool ok    = feed_.ws->reconnect_stale(specs_copy, feed_.feed_sup.config().stale_sec);
        const auto after = feed_.feed_sup.on_reconnect(ok, std::chrono::steady_clock::now());

        if (ok)
        {
            LOG_INFO("[Control] WebSocket 재연결 성공");
            continue;
        }

        LOG_ERROR("[Control] WebSocket 재연결 실패(" + std::to_string(feed_.feed_sup.fail_streak()) + "회) — " +
                  std::to_string(feed_.feed_sup.last_backoff_sec()) + "초 후 재시도");

        // 반복 실패 시에만 대응한다(1회 실패로 즉시 조치하면 순간 장애에도 흔들린다).
        //  먼저 REST 폴링으로 낮춰 매매를 이어가고, 그것마저 불가할 때 kill switch로 멈춘다.
        //  예전에는 곧장 kill switch였다 — 시세 경로가 하나 죽었다고 매매 전체를 세울 이유는 없다.
        if (after == feed::Supervisor::After::kFallback)
        {
            if (!activate_rest_fallback("재연결 " + std::to_string(feed_.feed_sup.fail_streak()) + "회 실패"))
            {
                LOG_ERROR("[Control] 폴링 폴백 불가(시세 소스 없음) — kill switch 작동");
                order_gate_.set_kill_switch(true);
            }
        }
    }
}

// ─── 운영단말(OpsServer) 배선 ─────────────────────────────────────────────
//  서버 스레드에서 불리는 콜백은 큐에 넣거나 스냅샷을 읽기만 한다. 주문은 strategy_thread가
//  drain_manual_inbox에서 OrderSignal로 바꿔 push_signal로 낸다. [why D-043]

void Engine::start_ops_server()
{
    if (ops_.port <= 0)
    {
        return;
    }

    ops_.server = std::make_unique<OpsServer>();
    ops_.server->set_bind(ops_.bind_addr, ops_.port);
    ops_.server->set_token(ops_.token);
    ops_.server->set_paper(kis_cfg_.is_paper);
    ops_.server->set_status_provider([this] { return ops_status_json(); });
    ops_.server->set_positions_provider([this] { return ops_positions_json(); });
    ops_.server->set_kill_handler(
        [this]
        {
            LOG_WARN("[Ops] KILL — 신규 주문 차단 + 엔진 종료");
            order_gate_.set_kill_switch(true);
            request_shutdown();
        });
    ops_.server->set_order_handler(
        [this](const OpsOrderReq& r) -> std::string
        {
            if (r.cid.empty() || r.cid.size() > 64)
            {
                return "cid는 1~64자";
            }

            if (r.ticker.size() != 6 || !std::all_of(r.ticker.begin(), r.ticker.end(), ::isdigit))
            {
                return "ticker는 6자리 숫자";
            }

            if (r.side != "SELL" && r.side != "BUY")
            {
                return "side는 SELL|BUY";
            }

            if (r.qty <= 0 || r.qty > 100000)
            {
                return "qty 범위 1~100000";
            }

            if (r.price < 0.0 || r.ref_price < 0.0)
            {
                return "가격은 0 이상";
            }

            {
                std::lock_guard<std::mutex> lk(ops_.manual_cid_mtx);

                if (!ops_.manual_cids.insert(r.cid).second)
                {
                    return "중복 cid — 이미 접수";
                }
            }

            if (!ops_.manual_inbox.push(r))
            {
                std::lock_guard<std::mutex> lk(ops_.manual_cid_mtx);
                ops_.manual_cids.erase(r.cid);
                return "수동주문 인테이크 가득 참";
            }

            pipeline_.strat_wake.notify();
            return std::string();
        });

    if (!ops_.server->start())
    {
        ops_.server.reset();
    }
}

std::string Engine::ops_status_json() const
{
    return nlohmann::json{{"running", running_.load()},
                          {"data", data_count_.load()},
                          {"signal", signal_count_.load()},
                          {"order", order_count_.load()},
                          {"kill", order_gate_.is_killed()},
                          {"entry_halt", order_gate_.is_entry_halted()},
                          {"force_liq", force_liquidate_.load(std::memory_order_relaxed)},
                          {"paper", kis_cfg_.is_paper},
                          {"strategies", strat_.list.size()}}
        .dump();
}

std::string Engine::ops_positions_json() const
{
    nlohmann::json arr = nlohmann::json::array();

    for (const auto& h : order_gate_.snapshot_positions())
    {
        arr.push_back({{"account", h.account},
                       {"ticker", h.ticker},
                       {"name", ticker_name(h.ticker)},
                       {"qty", h.qty},
                       {"avg_price", h.avg_price},
                       {"reserved", order_gate_.reserved(h.account, h.ticker)},
                       {"last", last_px(h.ticker)}});
    }

    return nlohmann::json{{"positions", arr}}.dump();
}

void Engine::drain_manual_inbox(const std::function<void(const OrderSignal&)>& emit)
{
    while (auto req = ops_.manual_inbox.pop())
    {
        const OpsOrderReq& r = *req;
        std::string reject;
        double      ref = r.ref_price;

        // 단말이 기준가를 안 찍었으면 엔진의 최근 체결가로 채운다 — 시장가 명목 한도가 0으로 새지 않게.
        if (ref <= 0.0)
        {
            ref = last_px(r.ticker);
        }

        if (r.side == "SELL")
        {
            // 보유 범위 안에서만 — 보유가 없거나 보유를 넘는 요청은 여기서 끊는다. 매도가능(보유−미체결매도)이 0인
            //  것은 거부하지 않고 라우터로 보낸다 — 라우터가 그 종목의 예약매도를 취소해 수량을 풀고 다시 낸다
            //  (청산차단 자가정리). 예전엔 여기서 "매도가능 0"으로 끊어 그 길에 닿지 못했다(09-14 15:00 먼지 정리
            //  3건 중 2건이 예약 익절 때문에 거부). [why D-082]
            int held = 0;
            double avg = 0.0;

            for (const auto& h : order_gate_.snapshot_positions())
            {
                if (h.ticker == r.ticker && h.account == r.account)
                {
                    held = h.qty;
                    avg  = h.avg_price;
                    break;
                }
            }

            if (held <= 0)
            {
                reject = "보유 없음";
            }
            else if (r.qty > held)
            {
                reject = "보유 " + std::to_string(held) + " 초과 요청 " + std::to_string(r.qty);
            }

            if (ref <= 0.0)
            {
                ref = avg;
            }
        }

        if (!reject.empty())
        {
            LOG_WARN("[Ops] 수동주문 거부 cid=" + r.cid + " " + r.ticker + " " + r.side + " " + std::to_string(r.qty) +
                     " — " + reject);

            if (ops_.server)
            {
                ops_.server->broadcast(ops::OpsMsg::ORDER_RESULT,
                                       nlohmann::json{{"cid", r.cid},
                                                      {"order_id", ""},
                                                      {"odno", ""},
                                                      {"strategy", "MANUAL"},
                                                      {"ticker", r.ticker},
                                                      {"side", r.side},
                                                      {"qty", r.qty},
                                                      {"price", r.price},
                                                      {"ok", false},
                                                      {"msg", reject}}
                                           .dump());
            }

            continue;
        }

        OrderSignal s;
        s.ticker      = r.ticker;
        s.account_id  = r.account;
        s.side        = OrderSide::from_string(r.side);
        s.type        = r.price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
        s.quantity    = r.qty;
        s.price       = r.price;
        s.ref_price   = ref;
        s.strategy_id = "MANUAL";
        s.client_oid  = r.cid;
        s.reason      = "운영단말 수동주문 cid=" + r.cid;
        s.timestamp   = std::chrono::system_clock::now();
        LOG_INFO("[Ops] 수동주문 → 게이트 cid=" + r.cid + " " + r.ticker + " " + r.side + " " + std::to_string(r.qty) +
                 (r.price > 0.0 ? " @" + std::to_string(static_cast<long long>(r.price)) : " 시장가"));
        emit(s);
    }
}
