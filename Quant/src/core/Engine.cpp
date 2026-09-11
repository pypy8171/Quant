#include "core/Engine.h"
#include "api/KisErrorCodes.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

// 오늘 날짜 YYYYMMDD(KST). 날짜별 표식 파일 이름에 쓴다 — 손익 기준선 파일과 같은 기준이다.
static std::string today_ymd()
{
    constexpr int kKstOffsetSec = 9 * 3600;
    time_t        kt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + kKstOffsetSec;
    struct tm     ktm{};
#ifdef _WIN32
    gmtime_s(&ktm, &kt);
#else
    gmtime_r(&kt, &ktm);
#endif
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &ktm);
    return std::string(buf);
}

Engine::Engine(KisConfig kis_cfg, int fetch_interval_sec)
    : kis_cfg_(std::move(kis_cfg)), fetch_interval_sec_(fetch_interval_sec)
{
}

Engine::~Engine()
{
    stop();
}

void Engine::add_strategy(std::unique_ptr<StrategyBase> strategy)
{
    LOG_INFO("[Engine] 전략 등록: " + strategy->describe());
    strategies_.push_back(std::move(strategy));
}

// 런타임(장중) 전략 등록. start()의 초기화 루프와 동일한 준비를 하되, strategies_
// push_back은 strat_mutex_ 하에 수행하고 strat_version_을 증가시켜 strategy_thread가
// 스냅샷을 재구성하도록 한다. watch_specs_는 control_thread가 재연결 때 읽으므로 watch_specs_mtx_로 감싼다.
void Engine::register_strategy_runtime(std::unique_ptr<StrategyBase> strategy)
{
    if (!strategy)
    {
        return;
    }

    // 런타임 등록 전략도 차트 조회는 실전 시세키로(분봉 모의 HTTP500 회피) — start()와 동일 패턴.
    strategy->set_kis(quote_kis_ ? quote_kis_.get() : kis_.get());
    strategy->set_account_kis(kis_.get()); // 잔고·매도가능수량은 계좌를 가진 주문 클라이언트로
    strategy->set_position_provider([this](const std::string& account, const std::string& ticker) {
        return order_gate_.position(account, ticker);
    });
    strategy->set_entry_halt_provider([this] { return order_gate_.is_entry_halted(); });
    strategy->set_sellable_provider([this](const std::string& account, const std::string& ticker) {
        return ledger_sellable(account, ticker);
    });

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

    if (regime_)
    {
        strategy->set_active(regime_->is_active_for(strategy->active_regimes()));
    }

    // 구독 스펙 추가 (control_thread 재연결 읽기와 겹치므로 watch_specs_mtx_).
    //  REST 폴링 모드면 다음 폴링 사이클부터 현재가를 받는다. WS 모드는 connect()가 기동 때
    //  한 번만 돌아서, 여기서 늘어난 종목은 목록에 넣는 것만으로는 틱이 오지 않는다. 살아 있는
    //  연결에 증분 구독을 걸어 둔다. 이게 없으면 재스캔으로 등록된 전략이 on_data를 한 번도
    //  못 받아 조용히 매매하지 않는다(등록 로그만 남아 정상으로 보인다).
    for (auto& spec : strategy->get_watch_specs())
    {
        if (spec.market == Market::KR)
        {
            registered_tickers_.insert(spec.ticker);
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

        if (!exists && ws_)
        {
            // false 자체는 정상일 수 있다(연결 전·이미 구독). 목록에서까지 빠졌으면 구독 상한에
            //  밀린 것이고, 그대로 두면 이 종목은 틱 없이 조용히 매매하지 않는다(09-11 실측:
            //  40 초과 종목 체결 0건). 넘침 목록에 넣어 data_thread가 REST로 대신 흘린다.
            if (!ws_->subscribe_incremental(spec) && !ws_->has_spec(spec))
            {
                std::lock_guard<std::mutex> lk(watch_specs_mtx_);
                ws_overflow_specs_.push_back(spec);
                LOG_WARN("[Engine] WS 구독 상한 — " + spec.ticker + " 시세는 REST 폴링으로 대체(넘침 " +
                         std::to_string(ws_overflow_specs_.size()) + "종목)");
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(strat_mutex_);
        strategies_.push_back(std::move(strategy));
        strat_version_.fetch_add(1, std::memory_order_release);
    }
}

// G1: 국면 r에 맞춰 전략 활성셋을 재선택한다.
//  has_regime_map_이면 국면별 id 목록이 권위적 선택자('*' 접두 매칭으로 스캐너 동적 id 포함),
//  아니면 기존 per-strategy active_regimes 폴백. 선택 결정(활성/비활성 목록)은 국면 변화 또는
//  force_log 시 [RegimeSelect]로 기록 → "왜 이 전략을 켰나"가 로그에 남는다.
//
//  ── 국면 두 축은 별개(G2) ─────────────────────────────────────────────────
//  ① RegimeController(내부 지수 국면)는 전략 선택 축이다. BULL/NEUTRAL/BEAR로
//     어떤 전략을 켤지 고른다(이 함수). ② regime.json(매크로 risk-off)은
//     리스크 오버레이 축이다. poll_regime_file()이 entry_halt/force_liquidate로
//     신규진입 정지·강제청산을 건다. 서로 다른 관심사라 통합하지 않는다. ①은
//     "무엇을 살까", ②는 "지금 사도 되나/다 팔아야 하나"를 각각 결정한다.
void Engine::apply_regime_selection(Regime r, bool force_log)
{
    // id 매칭: 목록 항목이 '*'로 끝나면 접두 매칭, 아니면 정확히 일치.
    auto matches = [](const std::string& id, const std::vector<std::string>& sel)
    {
        for (const auto& p : sel)
        {
            if (!p.empty() && p.back() == '*')
            {
                if (id.compare(0, p.size() - 1, p, 0, p.size() - 1) == 0)
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

    if (has_regime_map_)
    {
        auto it = regime_strategies_.find(r);

        if (it != regime_strategies_.end())
        {
            sel = &it->second; // 없는 국면 키 = 아무 전략도 활성 안 함(전량 비활성)
        }
    }

    auto append = [](std::string& csv, const std::string& id)
    { csv += csv.empty() ? id : ", " + id; };

    std::string active_ids, inactive_ids;

    for (auto& s : strategies_)
    {
        bool on;

        if (has_regime_map_)
        {
            on = sel && matches(s->id(), *sel);
        }
        else
        {
            on = regime_ ? regime_->is_active_for(s->active_regimes()) : true;
        }

        s->set_active(on);
        append(on ? active_ids : inactive_ids, s->id());
    }

    if (force_log || r != last_selected_regime_)
    {
        LOG_INFO("[RegimeSelect] 국면=" + to_string(r) + " → 활성=[" + active_ids +
                 "] 비활성=[" + inactive_ids + "]" +
                 (has_regime_map_ ? "" : " (per-strategy 폴백)"));
    }

    last_selected_regime_ = r;
}

// 주기적 유니버스 재스캔 — universe_fn_으로 티커 목록을 산출해 미등록 종목은 런타임 등록하고,
//  drop_after_sec 이상 연속으로 빠져 있는 종목(보유·선점 없음)은 뗀다.
void Engine::maybe_rescan_universe()
{
    reap_retired(/*force=*/false);

    if (rescan_jobs_.empty())
    {
        return;
    }

    KisClient* scan_kis = quote_kis_ ? quote_kis_.get() : kis_.get();

    if (!scan_kis)
    {
        return;
    }

    const auto now_c = std::chrono::steady_clock::now();

    for (auto& job : rescan_jobs_)
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

        for (auto& t : tickers)
        {
            if (t.empty() || registered_tickers_.count(t))
            {
                continue;
            }

            // 상한에 닿으면 더 등록하지 않는다. 해제 경로가 없어 한번 등록한 종목은 남으므로,
            //  상한이 없으면 재스캔마다 조회량이 계단식으로 늘어난다.
            if (job.max_registered > 0 && job.registered >= job.max_registered)
            {
                capped = true;
                break;
            }

            auto strat = job.factory(t);

            if (!strat)
            {
                continue;
            }

            LOG_INFO("[Engine] 재스캔 신규 등록: " + strat->describe());
            StrategyBase* raw = strat.get();
            register_strategy_runtime(std::move(strat));

            // on_start 예외로 등록이 거부됐으면 registered_tickers_에 안 들어간다.
            if (registered_tickers_.count(t))
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
                     std::to_string(registered_tickers_.size()) + "종목)" +
                     (capped ? " — 등록 상한 " + std::to_string(job.max_registered) +
                                   " 도달, 신규 등록 중단"
                             : ""));
        }

        if (job.drop_after_sec <= 0)
        {
            continue;
        }

        // 해제 판정. universe_fn은 보유 종목을 결과에서 이미 빼고 주므로(drop_held) 빠져 있다는
        //  것만으로는 이탈이 아니다 — 보유·선점을 원장에서 다시 보고, 있으면 시계를 지운다.
        std::unordered_set<std::string> in_scan(tickers.begin(), tickers.end());
        std::unordered_set<std::string> held;

        for (const auto& h : order_gate_.snapshot_positions())
        {
            if (h.qty != 0 || order_gate_.reserved(h.account, h.ticker) != 0)
            {
                held.insert(h.ticker);
            }
        }

        std::vector<std::string> drop;

        for (const auto& [t, ptr] : job.owned)
        {
            if (in_scan.count(t) || held.count(t) || order_gate_.reserved(t) != 0)
            {
                job.absent_since.erase(t);
                continue;
            }

            auto it = job.absent_since.find(t);

            if (it == job.absent_since.end())
            {
                job.absent_since.emplace(t, now_c);
                continue;
            }

            if (now_c - it->second >= std::chrono::seconds(job.drop_after_sec))
            {
                drop.push_back(t);
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
                std::lock_guard<std::mutex> lk(strat_mutex_);
                auto sit = std::find_if(strategies_.begin(), strategies_.end(),
                                        [ptr](const std::unique_ptr<StrategyBase>& s) { return s.get() == ptr; });

                if (sit != strategies_.end())
                {
                    victim = std::move(*sit);
                    strategies_.erase(sit);
                }

                ver = strat_version_.fetch_add(1, std::memory_order_release) + 1;
            }

            if (victim)
            {
                LOG_INFO("[Engine] 재스캔 이탈 해제: " + victim->describe() + " — " +
                         std::to_string(job.drop_after_sec) + "초 이상 유니버스 밖, 보유·선점 없음");
                retired_.push_back(Retired{std::move(victim), ver});
            }

            registered_tickers_.erase(t);
            job.owned.erase(t);
            job.absent_since.erase(t);

            if (job.registered > 0)
            {
                --job.registered;
            }
        }

        if (!drop.empty())
        {
            LOG_INFO("[Engine] 유니버스 재스캔 해제: -" + std::to_string(drop.size()) +
                     "종목 (이 슬리브 " + std::to_string(job.registered) + ", 전체 " +
                     std::to_string(registered_tickers_.size()) + "종목)");
        }
    }
}

void Engine::reap_retired(bool force)
{
    if (retired_.empty())
    {
        return;
    }

    const uint64_t seen = strat_seen_version_.load(std::memory_order_acquire);
    auto           keep = retired_.begin();

    for (auto it = retired_.begin(); it != retired_.end(); ++it)
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

    retired_.erase(keep, retired_.end());
}

void Engine::start()
{
    if (running_.load())
    {
        return;
    }

    LOG_INFO("[Engine] ── 퀀트 엔진 시작 ──────────────────────────────");

#ifdef HAS_ZMQ
    zmq_bridge_ = std::make_unique<ZmqBridge>();
    zmq_bridge_->set_bind_address(zmq_bind_addr_);
    zmq_bridge_->set_control_token(zmq_control_token_);
    zmq_bridge_->set_command_handler(
        [this](const std::string& cmd) -> std::string
        {
            if (cmd == "KILL")
            {
                LOG_WARN("[ZMQ] KILL 명령 수신 — 신규 주문 차단 + 엔진 종료");
                order_gate_.set_kill_switch(true);
                running_.store(false);
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

    kis_ = std::make_unique<KisClient>(kis_cfg_);

    if (!kis_->authenticate())
    {
        LOG_ERROR("[Engine] KIS 인증 실패");
        return;
    }

    // 시세 전용 클라이언트(실전 도메인) — 모의 시세 REST가 HTTP 500이므로 시세만 실전으로 조회.
    //  실패해도 kis_(모의)로 폴백하되, 모의 시세는 500이라 사실상 틱이 안 나옴을 경고.
    //  WS 모드에서도 만들어 둔다: WS가 죽어 폴링으로 낮출 때 쓸 시세 소스가 그때 가서는 없으면
    //  폴백이 무의미해진다(모의 도메인으로 폴링하면 500만 쌓인다).
    if (has_quote_kis_)
    {
        quote_kis_ = std::make_unique<KisClient>(quote_kis_cfg_);

        if (!quote_kis_->authenticate())
        {
            LOG_ERROR("[Engine] 시세 클라이언트(실전) 인증 실패 — 모의 시세로 폴백(틱 없을 수 있음)");
            quote_kis_.reset();
        }
        else
        {
            LOG_INFO("[Engine] 시세 클라이언트(실전 도메인) 인증 완료 — 현재가 폴링 소스");
        }
    }

    // FEP OrderRouter 초기화
#ifdef HAS_ZMQ
    order_router_ = std::make_unique<OrderRouter>(order_gate_, *kis_, zmq_bridge_.get());
#else
    order_router_ = std::make_unique<OrderRouter>(order_gate_, *kis_);
#endif
    LOG_INFO("[Engine] OrderRouter (FEP) 초기화 완료");
    start_ops_server();

    // 이전 세션이 남긴 미체결 주문을 취소한다. 건당 왕복이 3~5초라 여기서 기다리면
    //  기동이 몇 분씩 멈춘다 — 목록을 읽고 부속 파일을 비우는 것만 여기서 하고 취소는
    //  별도 스레드가 이어서 낸다. 잔고 시드가 이를 기다릴 필요는 없다: 미체결 취소는
    //  보유수량을 바꾸지 않고 주문가능현금·매도가능수량만 푸는데 둘 다 주기 잔고 대조가
    //  다시 읽는다.
    order_router_->cancel_stale_orders_async();

    // G5: 실계좌 보유분을 원장에 시드 (스레드 시작 전, 단일스레드 구간)
    if (bootstrap_ledger_ && !bootstrap_ledger())
    {
        // running_이 아직 false라 main 루프가 바로 빠지고, 감시자가 5초 뒤 다시 띄운다.
        LOG_ERROR("[Engine] 원장 없이 기동하지 않는다 — 프로세스 종료");
        return;
    }

    // RegimeController (국면 메타레이어) 초기화
    regime_ = std::make_unique<RegimeController>(regime_cfg_);
    // 업종 지수 일봉도 시세이므로 모의 도메인은 HTTP 500. 시세 전용 실전 클라이언트가
    //  있으면 그걸로 조회(없으면 모의로 폴백 → NEUTRAL 유지).
    regime_->set_kis(quote_kis_ ? quote_kis_.get() : kis_.get());
    LOG_INFO("[Engine] RegimeController 초기화 완료");

    // 전략 초기화 (시세 클라이언트 주입 → on_start 내부에서 Universe 조회)
    // 전략의 kis_는 차트(일봉·분봉)·랭킹 등 "읽기 전용 시세 조회"에만 쓰인다(실제 주문 발주는 OrderThread가 담당).
    // 분봉 TR(inquire-time-itemchartprice)은 모의 도메인에서 HTTP 500 → 시세 전용 실전 클라이언트가
    //  있으면 그걸로 조회(regime_·스캐너와 동일 패턴). 없으면 모의로 폴백.
    for (auto& s : strategies_)
    {
        s->set_kis(quote_kis_ ? quote_kis_.get() : kis_.get());
        // 잔고 조회는 시세 클라이언트가 아니라 계좌를 가진 주문 클라이언트로 한다.
        //  시세 전용 클라이언트에는 account_no가 없어 has_account()가 false가 되고,
        //  그러면 매도가능수량이 항상 0으로 떨어져 익절·존이탈청산·장 마감청산이 전부 발주되지 않는다.
        s->set_account_kis(kis_.get());
        // D2: 확정 포지션 접근자 주입 — 전략이 OrderGate 원장(WS/REST 공용)을 진실원천으로 읽음.
        s->set_position_provider([this](const std::string& account, const std::string& ticker) {
            return order_gate_.position(account, ticker);
        });
        s->set_entry_halt_provider([this] { return order_gate_.is_entry_halted(); });
        s->set_sellable_provider([this](const std::string& account, const std::string& ticker) {
            return ledger_sellable(account, ticker);
        });

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

        for (auto& s : strategies_)
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
    registered_tickers_.clear();

    for (auto& spec : watch_specs_)
    {
        if (spec.market == Market::KR)
        {
            registered_tickers_.insert(spec.ticker);
        }
    }

    running_.store(true);

    // 런타임 피드 상태를 config 의도로 초기화. 이후 WS 생사에 따라 control_thread가 토글한다.
    rest_feed_active_.store(rest_price_feed_, std::memory_order_relaxed);

    // WebSocket — 동적 구독 스펙으로 연결.
    //  rest_price_feed_ 모드에서는 WS를 열지 않는다: KIS는 app_key당 실시간 1세션만
    //  허용하는데, 세션 정리가 서버측에 걸려 rt=9(ALREADY IN USE) 재연결 폭주가 나므로
    //  체결 피드를 REST 현재가 폴링(data_thread_fn)으로 대체하고 WS 의존을 제거한다.
    //  주문은 REST(order_thread_fn)로 나가므로 매매에는 영향 없음(체결통보 on_fill만 없음).
    if (!rest_price_feed_ && !watch_specs_.empty())
    {
        ws_ = std::make_unique<KisWebSocket>(kis_cfg_);
        ws_->set_callbacks([this](const OrderBook& ob) { ob_queue_.push(ob); },
                           [this](const TradeData& td)
                           {
                               // 전략 스레드가 멈추면 큐가 차고 틱이 여기서 사라진다 — 세어 두고
                               //  넘침이 시작될 때 한 번 남긴다(09-11 15:15 잔고 조회 정체). [why D-055]
                               if (!td_queue_.push(td))
                               {
                                   if (td_drop_count_.fetch_add(1, std::memory_order_relaxed) == 0)
                                   {
                                       LOG_WARN("[WS] 체결 큐 가득 — 틱 폐기 시작 " + td.ticker +
                                                " (전략 스레드 정체 의심)");
                                   }
                               }
#ifdef HAS_ZMQ
                               if (zmq_bridge_)
                               {
                                   zmq_bridge_->publish_trade(td);
                               }
#endif
                           });
        ws_->set_fill_callback([this](const FillNotification& fn)
                               {
                                   if (order_router_)
                                   {
                                       order_router_->on_fill(fn);
                                   }

                                   if (ops_server_)
                                   {
                                       ops_server_->broadcast(
                                           ops::OpsMsg::FILL,
                                           nlohmann::json{{"odno", fn.odno},
                                                          {"ticker", fn.ticker},
                                                          {"side", fn.side == OrderSide::BUY ? "BUY" : "SELL"},
                                                          {"qty", fn.filled_qty},
                                                          {"price", fn.filled_price},
                                                          {"time", fn.fill_time}}
                                               .dump());
                                   }
                               });

        if (!ws_->connect(watch_specs_))
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

    data_thread_ = std::thread(&Engine::data_thread_fn, this);
    strategy_thread_ = std::thread(&Engine::strategy_thread_fn, this);
    order_thread_ = std::thread(&Engine::order_thread_fn, this);
    control_thread_ = std::thread(&Engine::control_thread_fn, this);

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

double Engine::last_px(const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(last_px_mu_);
    auto it = last_px_.find(ticker);
    return it == last_px_.end() ? 0.0 : it->second.px;
}

void Engine::set_last_px(const std::string& ticker, double px)
{
    if (px <= 0.0)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(last_px_mu_);
    last_px_[ticker] = LastPx{px, std::chrono::steady_clock::now()};
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

// ─── G5: 실계좌 보유분 원장 부트스트랩 ──────────────────────────────────────
//  get_balance output1의 pdno/hldg_qty/pchs_avg_pric를 OrderGate.seed_position으로 시드.
//  계좌키는 account=""(전략 신호의 기본 account_id와 일치, C-1). 평단까지 시드해야
//  매도 실현손익·일일손실 한도가 실제와 정합(C-2). reset_daily가 positions_/avg_prices_를
//  보존하므로 장 시작 리셋 후에도 유지 — 프로세스 기동당 1회면 충분.
bool Engine::bootstrap_ledger()
{
    try
    {
        nlohmann::json bal;

        // 기동 직후는 유령주문 취소·유니버스 스캔과 같은 초 안에 겹쳐 한도(초당 5건)에 자주
        //  걸린다. 실패를 "보유 0"으로 읽고 넘어가면 빈 원장으로 매매한다(09-11 09:17 사례).
        //  잠깐 쉬고 다시 묻는다. 끝내 못 읽으면 기동을 접는 게 맞다 — 감시자가 다시 띄운다.
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            bal = kis_->get_balance();

            if (bal.contains("output1"))
            {
                break;
            }

            LOG_WARN("[Engine] 원장 부트스트랩: 잔고 응답 없음 — 재시도 " +
                     std::to_string(attempt + 1) + "/5");
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }

        if (!bal.contains("output1"))
        {
            LOG_ERROR("[Engine] 원장 부트스트랩: 잔고를 끝내 못 읽음 — 기동 중단");
            return false;
        }

        int n = 0;

        for (auto& h : bal["output1"])
        {
            std::string code = h.value("pdno", "");
            std::string pname = h.value("prdt_name", "");
            int    q  = std::atoi(h.value("hldg_qty", "0").c_str());
            double av = std::atof(h.value("pchs_avg_pric", "0").c_str());

            if (!code.empty() && q > 0)
            {
                // 주문가능수량(ord_psbl_qty)을 게이트에 함께 시드한다. 보유수량과 다르면(직전
                //  세션이 남긴 미체결 매도·미결제분) 전량 청산이 40240000으로 통째 거부돼
                //  한 주도 못 빠져나온다(09-08 047050 254주·381주 연속 거부). 필드가 없거나
                //  파싱 실패면 -1을 넘겨 "모름"으로 두고 보유수량을 그대로 쓴다.
                std::string psbl = h.value("ord_psbl_qty", std::string("(field없음)"));
                int psbl_q = -1;

                if (!psbl.empty() && psbl.find_first_not_of("0123456789 ") == std::string::npos)
                {
                    psbl_q = std::atoi(psbl.c_str());
                }

                order_gate_.seed_position(std::string(), code, q, av, psbl_q);
                register_ticker_name(code, pname); // 로그 라벨(초기 보유분 종목명)
                LOG_INFO("[Engine]   시드 " + code + " " + pname + " " + std::to_string(q) + "주 @평단 " +
                         std::to_string(static_cast<long long>(av)) + " 주문가능=" + psbl);
                ++n;
            }
        }

        LOG_INFO("[Engine] 원장 부트스트랩 완료: " + std::to_string(n) + "종목 시드");
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[Engine] 원장 부트스트랩 예외: " + std::string(e.what()));
        return false;
    }
}

// ─── C-1: rest 모드 주기적 잔고 대조 ────────────────────────────────────
//  rest_price_feed_ 모드는 체결콜백(OrderRouter::on_fill)이 미등록이라 positions_/
//  daily_pnl_이 갱신되지 않는다(치명). 데이터 스레드가 매 사이클 잔고를 재조회해:
//   1) output1 보유분으로 positions_/avg_prices_ 재동기(seed_position 덮어쓰기),
//   2) output2 총평가금(tot_evlu_amt)의 당일 기준선 대비 델타를 daily_pnl_로 세팅
//      → BUY-only 손실컷(§4)이 정상 작동(C-3의 신규매수 차단 의도 재무장).
//  ⚠️ 절대 평가손익(evlu_pfls)이 아니라 "당일 기준선 델타"를 쓴다. 이미 -30% 물린
//     미실현손실을 daily_pnl로 넣으면 개장 즉시 모든 신규매수가 막혀버리기 때문.
// 잔고에 없는 원장 보유를 걷어내기 전에 두는 유예(초). 잔고 조회 왕복(수 초)보다 넉넉히 길게
//  잡아, 방금 체결된 신규 보유가 아직 잔고에 안 보이는 것을 유령으로 오인하지 않게 한다.
static constexpr int kPrunePositionAgeSec = 90;

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

void Engine::reconcile_from_balance(bool resync_positions)
{
    // 서킷브레이커: 잔고조회가 연속 실패 중이면 이번 사이클은 조회를 건너뛴다.
    //  (get_balance 12002 타임아웃 시 GET 3회 재시도로 ~60s를 태워 데이터 스레드를 정체시킴)
    if (reconcile_skip_remaining_ > 0)
    {
        --reconcile_skip_remaining_;
        return;
    }

    bool responded = false; // 잔고 응답을 실제 파싱했는가(서킷브레이커 판정용)

    try
    {
        nlohmann::json bal = kis_->get_balance();

        // 1) 보유분 재동기 — 실체결 원장을 실제 잔고로 강제 일치.
        if (bal.contains("output1"))
        {
            responded = true;

            // 잔고는 서버 확정 스냅샷 → 미체결 선점(reserved_)을 통째로 비우고 실보유만 신뢰.
            //  체결피드(H0STCNI0) 부재로 누적된 H-1 드리프트(과잉 선점 → 정상신호 과잉차단)를
            //  동기화 시점마다 해소한다(#1). reset은 seed 재기록 전에 1회.
            //  체결통보가 오는 WS 모드에서는 건너뛴다 — 그쪽은 원장이 체결로 이미 갱신되고,
            //  reserved_에는 아직 살아 있는 지정가 주문이 잡혀 있어 비우면 재발주를 부른다.
            if (resync_positions)
            {
                order_gate_.reset_reserved();
            }

            // 잔고에 있는 종목을 모으면서, 재동기 모드면 원장까지 덮어쓴다.
            std::vector<std::string> held;

            for (auto& h : bal["output1"])
            {
                std::string code = h.value("pdno", "");
                int    q  = std::atoi(h.value("hldg_qty", "0").c_str());
                double av = std::atof(h.value("pchs_avg_pric", "0").c_str());

                if (code.empty() || q <= 0)
                {
                    continue;
                }

                held.push_back(code);

                if (resync_positions)
                {
                    order_gate_.seed_position(std::string(), code, q, av);
                }

                // 매도가능수량은 재동기 모드와 무관하게 매번 맞춘다(기동 시드 0 고착 해소).
                const std::string psbl = h.value("ord_psbl_qty", "");

                if (!psbl.empty() && psbl.find_first_not_of("0123456789 ") == std::string::npos)
                {
                    order_gate_.refresh_sellable(std::string(), code, std::atoi(psbl.c_str()));
                }

                // 체결통보 모드는 원장을 덮어쓰지 않는다. 대신 재연결 사이에 빠진 매도 체결만
                //  잔고로 되메운다 — 두 번 연속 같은 부족분이 보이고 미체결 매도 이내일 때.
                //  (09-11 11:00 WS 끊김 4초에 248170 매도 52주 통보 유실 → 18분 유령 보유)
                if (!resync_positions)
                {
                    const int absorbed = order_gate_.absorb_missed_sell(std::string(), code, q);

                    if (absorbed > 0)
                    {
                        LOG_WARN("[Engine] 잔고 대조: " + code + " 원장이 잔고보다 " +
                                 std::to_string(absorbed) + "주 많아 놓친 매도 체결로 보고 " +
                                 std::to_string(q) + "주로 맞춘다");
                    }
                }
            }

            // 잔고에 없는데 원장에 남은 종목을 걷어낸다. 이 유령이 슬롯을 물고 있으면
            //  실제 보유가 20인데 "동시 보유 종목 한도 초과 (25 >= 25)"가 난다(09-09 관측).
            //  체결통보가 오는 WS 모드에서도 통보를 놓치면 같은 자리에 남으므로 두 모드 다 돈다.
            //  갓 열린 포지션은 잔고 왕복이 체결보다 빨랐을 수 있어 남긴다.
            //
            // [inv] held가 비면 한 종목도 걷어내지 않는다. output1이 있는데 비어 있는 응답은
            //  "보유가 없다"보다 "잔고를 못 받았다"일 때가 압도적으로 많다 — 초당 한도(EGW00201)나
            //  일시 오류가 빈 배열로 돌아온다. 그걸 정본으로 믿고 지우면 원장이 통째로 날아가고,
            //  엔진은 미보유로 읽어 같은 종목을 다시 산다(09-09 14:04, 재기동 직후 한도 폭주 중에
            //  25종목 전부 정리됨). 진짜로 빈 계좌라면 걷어낼 것도 없으니 건너뛰어 잃는 것이 없다.
            const auto gone = held.empty()
                            ? std::vector<std::string>{}
                            : order_gate_.prune_positions(held, kPrunePositionAgeSec);

            if (held.empty())
            {
                LOG_WARN("[Engine] 잔고 대조: output1이 비어 유령 정리를 건너뛴다 "
                         "(잔고 조회 실패로 본다 — 원장 유지)");
            }

            if (!gone.empty())
            {
                std::string list;

                for (const auto& t : gone)
                {
                    list += (list.empty() ? "" : ",") + t;
                }

                LOG_WARN("[Engine] 잔고 대조: 잔고에 없는 원장 보유 " +
                         std::to_string(gone.size()) + "종목 정리 (" + list + ")");
            }
        }

        // 2) 당일 총평가금 델타 → daily_pnl_. output2는 배열 또는 객체로 올 수 있어 양쪽 수용.
        double tot_eval = 0.0;
        bool have_eval = false;
        // 표시 전용: 전일 총자산(bfdy_tot_asst_evlu_amt) 대비 오늘 손익 — launch 시점과 무관하게
        //  "전일종가 대비 당일손익"을 정확히 찍기 위함. 손실컷 기준선(세션 앵커)과는 분리(리스크 동작 불변).
        double bfdy_asset = 0.0;
        bool have_bfdy = false;

        if (bal.contains("output2"))
        {
            const auto& o2 = bal["output2"];
            const nlohmann::json* row = nullptr;

            if (o2.is_array() && !o2.empty())
            {
                row = &o2[0];
            }
            else if (o2.is_object())
            {
                row = &o2;
            }

            if (row)
            {
                std::string s = row->value("tot_evlu_amt", "");

                if (s.empty())
                {
                    s = row->value("nass_amt", ""); // 순자산금액 폴백
                }

                if (!s.empty())
                {
                    try { tot_eval = std::stod(s); have_eval = true; } catch (...) {}
                }

                // 주문가능현금 — 매수 클램프의 진짜 상한. 가수도정산금액(실질 주문가능)을
                //  우선 쓰고 없으면 예수금총금액으로 떨어진다. 평가금과 달리 미체결 지정가와
                //  미결제 매수로 묶인 몫이 빠져 있어야 40250000 도배를 막을 수 있다.
                std::string cs = row->value("prvs_rcdl_excc_amt", "");

                if (cs.empty())
                {
                    cs = row->value("dnca_tot_amt", "");
                }

                if (!cs.empty())
                {
                    try
                    {
                        const double cash = std::stod(cs);

                        if (cash >= 0.0)
                        {
                            order_gate_.set_available_cash(cash);
                        }
                    }
                    catch (...) {}
                }

                // 전일 총자산(없으면 순자산 폴백 없이 스킵) — 표시 전용.
                std::string bs = row->value("bfdy_tot_asst_evlu_amt", "");

                if (!bs.empty())
                {
                    try { bfdy_asset = std::stod(bs); have_bfdy = true; } catch (...) {}
                }
            }
        }

        if (have_eval)
        {
            if (!have_pnl_baseline_)
            {
                // 손실컷 재시작 리셋 구멍 방지: 기준선을 거래일(KST)별 파일로 영속화.
                //  같은 날 재시작 → 저장된 기준선 재사용(손실 한도 유지), 새 거래일 → 신규 캡처+저장.
                //  장 시작(09:00)부터 연속 구동 시 파일이 그날 시가 기준선을 담아 당일손익이 정확.
                constexpr int kKstOffsetSec = 9 * 3600; // KST = UTC+9
                time_t kt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + kKstOffsetSec;
                struct tm ktm{};
#ifdef _WIN32
                gmtime_s(&ktm, &kt);
#else
                gmtime_r(&kt, &ktm);
#endif
                char dbuf[9];
                std::strftime(dbuf, sizeof(dbuf), "%Y%m%d", &ktm);
                // 실행 위치와 무관하게 로그 폴더(main에서 고정)와 같은 곳에 기준선 저장.
                //  파일명에 계좌번호를 포함 → 같은 거래일에 계좌를 갈아끼면(모의계좌 재발급 등)
                //  옛 계좌 기준선을 재사용해 당일손익이 오염되는 것을 막는다(계좌 바뀌면 새로 캡처).
                std::string acct = kis_ ? kis_->account_no() : std::string();
                std::string bname = std::string("pnl_baseline_") + dbuf;

                if (!acct.empty())
                {
                    bname += "_" + acct;
                }

                bname += ".txt";
                std::filesystem::path bpath = Logger::instance().path_for(bname);

                double file_base = 0.0;
                bool from_file = false;
                {
                    std::ifstream bf(bpath);

                    if (bf.is_open() && (bf >> file_base) && file_base > 0.0)
                    {
                        from_file = true;
                    }
                }

                if (from_file)
                {
                    pnl_baseline_ = file_base;
                    LOG_INFO("[Engine] 기준선 파일 재사용(" + std::string(dbuf) + "): " +
                             std::to_string(static_cast<long long>(pnl_baseline_)) +
                             "원 — 재시작해도 당일 손실컷 유지");
                }
                else
                {
                    pnl_baseline_ = tot_eval;
                    std::ofstream of(bpath, std::ios::trunc);

                    if (of.is_open())
                    {
                        of << static_cast<long long>(pnl_baseline_) << "\n";
                    }

                    LOG_INFO("[Engine] 기준선 신규 캡처+저장(" + std::string(dbuf) + "): 총평가금 " +
                             std::to_string(static_cast<long long>(tot_eval)) + "원");
                }

                have_pnl_baseline_ = true;
            }

            double delta = tot_eval - pnl_baseline_;
            order_gate_.set_daily_pnl(delta); // 손실컷용(세션 앵커) — 리스크게이트 동작 유지
            order_gate_.set_equity(tot_eval); // 총노출 게이트(§3d) 분모 — 총평가금 스냅샷 갱신
            LOG_INFO("[Engine] 잔고 대조: 당일손익 " + std::to_string(static_cast<long long>(delta)) +
                     "원 (총평가 " + std::to_string(static_cast<long long>(tot_eval)) + ")");

            // 표시 전용: 전일종가 대비 진짜 당일손익 — launch 시점과 무관하게 정확·연속 누적.
            if (have_bfdy && bfdy_asset > 0.0)
            {
                double day_delta = tot_eval - bfdy_asset;
                LOG_INFO("[Engine] 당일손익(전일대비): " +
                         std::to_string(static_cast<long long>(day_delta)) +
                         "원 (전일총자산 " + std::to_string(static_cast<long long>(bfdy_asset)) + ")");
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[Engine] 잔고 대조 예외: " + std::string(e.what()));
    }

    // 서킷브레이커 갱신 — 성공 시 즉시 복귀, 실패 시 지수 백오프(1·2·4·8 사이클, 상한 8).
    //  B2: 잔고조회가 지속 실패하는 동안 daily_pnl_은 낡아 §4 손실컷이 트립하지 못한다.
    //      스트릭이 임계(2)를 넘으면 OrderGate.pnl_stale를 세워 그 창에서 BUY NEW를 보수 정지하고,
    //      복구되는 즉시 해제한다. 임계 2 = 단발 타임아웃(streak 1)엔 발동 않고 지속 정체만 잡음.
    static constexpr int kPnlStaleStreak = 2;

    if (responded)
    {
        if (reconcile_fail_streak_ > 0)
        {
            LOG_INFO("[Engine] 잔고조회 복구 — 잔고 대조 정상화");
        }

        if (reconcile_fail_streak_ >= kPnlStaleStreak)
        {
            LOG_INFO("[Engine] daily_pnl 신선도 복구 — 신규 진입 정지 해제(B2)");
        }

        reconcile_fail_streak_ = 0;
        reconcile_skip_remaining_ = 0;
        order_gate_.set_pnl_stale(false);
    }
    else
    {
        ++reconcile_fail_streak_;
        int cap = reconcile_fail_streak_ - 1;

        if (cap > 3)
        {
            cap = 3;  // 백오프 상한: 2^3 = 8 사이클
        }

        reconcile_skip_remaining_ = 1 << cap;
        LOG_WARN("[Engine] 잔고조회 실패(streak=" + std::to_string(reconcile_fail_streak_) +
                 ", 12002 타임아웃 등) — 잔고 대조 " + std::to_string(reconcile_skip_remaining_) +
                 "사이클 백오프(핫루프 보호)");

        // 지속 정체 → daily_pnl 낡음: 신규 진입 보수 정지(SELL 청산은 통과). 임계 진입 시 1회 경고.
        if (reconcile_fail_streak_ == kPnlStaleStreak)
        {
            LOG_WARN("[Engine] daily_pnl 신선도 상실(streak≥" + std::to_string(kPnlStaleStreak) +
                     ") — BUY NEW 보수 정지(B2), 손실컷 신뢰불가 창 방어");
        }

        if (reconcile_fail_streak_ >= kPnlStaleStreak)
        {
            order_gate_.set_pnl_stale(true);
        }
    }
}

void Engine::stop()
{
    // exchange로 중복 호출 방지 — 이미 false면 즉시 반환
    if (!running_.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    LOG_INFO("[Engine] 종료 시작");

    // 역순 join 권장: control → order → strategy → data
    if (control_thread_.joinable())
    {
        control_thread_.join();
    }

    if (order_thread_.joinable())
    {
        order_thread_.join();
    }

    if (strategy_thread_.joinable())
    {
        strategy_thread_.join();
    }

    if (data_thread_.joinable())
    {
        data_thread_.join();
    }

    if (ws_)
    {
        ws_->disconnect();
    }

#ifdef HAS_ZMQ
    if (zmq_bridge_)
    {
        zmq_bridge_->stop();
    }
#endif

    if (ops_server_)
    {
        ops_server_->stop();
    }

    for (auto& s : strategies_)
    {
        s->on_stop();
    }

    reap_retired(/*force=*/true);
    print_stats();
    LOG_INFO("[Engine] 종료 완료");
}

static struct tm utc_plus_hours(int offset_h); // KST 계산용(정의는 하단)

// WS 상한에 밀린 종목의 시세 대체. 먼저 재구독을 시도하고(드롭으로 슬롯이 비었을 수 있다),
//  안 되면 REST 현재가를 TradeData로 흘린다 — rest 분기와 같은 모양이라 전략은 구분하지 못한다.
//  rest 분기가 도는 사이클에는 부르지 않는다(그쪽이 이미 전 종목을 폴링한다).
void Engine::poll_ws_overflow()
{
    if (!ws_)
    {
        return;
    }

    std::vector<WatchSpec> pending;
    {
        // 최초 연결·재연결에서 상한에 밀린 종목도 여기로 합친다 — 재스캔 등록분만 챙기면
        //  기동 시 뒤쪽에 선 종목(청산 관리 시드)이 틱을 영영 못 받는다.
        auto from_ws = ws_->take_overflow_specs();
        std::lock_guard<std::mutex> lk(watch_specs_mtx_);

        for (const auto& spec : from_ws)
        {
            bool dup = false;

            for (const auto& w : ws_overflow_specs_)
            {
                if (w.ticker == spec.ticker && w.market == spec.market && w.is_future == spec.is_future)
                {
                    dup = true;
                    break;
                }
            }

            if (!dup)
            {
                ws_overflow_specs_.push_back(spec);
                LOG_WARN("[Engine] WS 구독 상한 — " + spec.ticker + " 시세는 REST 폴링으로 대체(넘침 " +
                         std::to_string(ws_overflow_specs_.size()) + "종목)");
            }
        }

        pending = ws_overflow_specs_;
    }

    if (pending.empty())
    {
        return;
    }

    struct tm kst = utc_plus_hours(9);
    char hhmmss[8];
    std::snprintf(hhmmss, sizeof(hhmmss), "%02d%02d%02d", kst.tm_hour, kst.tm_min, kst.tm_sec);
    KisClient* qc = quote_kis_ ? quote_kis_.get() : kis_.get();

    for (const auto& spec : pending)
    {
        if (ws_->subscribe_incremental(spec))
        {
            std::lock_guard<std::mutex> lk(watch_specs_mtx_);

            for (auto it = ws_overflow_specs_.begin(); it != ws_overflow_specs_.end(); ++it)
            {
                if (it->ticker == spec.ticker && it->market == spec.market && it->is_future == spec.is_future)
                {
                    ws_overflow_specs_.erase(it);
                    break;
                }
            }

            LOG_INFO("[Engine] WS 슬롯 확보 — " + spec.ticker + " 구독 복귀");
            continue;
        }

        if (spec.market != Market::KR || !qc)
        {
            continue;
        }

        std::this_thread::sleep_for(150ms);  // 초당 호출 한도 밑에 깔기(rest 분기와 같은 간격)
        const double px = qc->get_current_price(spec.ticker);

        // 종목당 첫 성공·첫 실패만 남긴다 — 대체 경로가 실제로 틱을 흘리는지 로그로 확인할 수 있어야 한다.
        //  데이터 스레드만 부르므로 함수 정적 집합으로 충분하다.
        static std::unordered_set<std::string> rest_seen;
        static std::unordered_set<std::string> rest_failed;

        if (px <= 0.0)
        {
            if (rest_failed.insert(spec.ticker).second)
            {
                LOG_WARN("[Engine] REST 대체 시세 실패 " + spec.ticker + " — 현재가 0(응답 없음/파싱 실패)");
            }

            continue;
        }

        if (rest_seen.insert(spec.ticker).second)
        {
            LOG_INFO("[Engine] REST 대체 시세 첫 수신 " + spec.ticker + " px=" + std::to_string(px));
        }

        TradeData td;
        td.ticker = spec.ticker;
        td.time = hhmmss;
        td.price = px;
        td.quantity = 0;
        td.direction = 0;
        td.market = Market::KR;
        td.timestamp = std::chrono::system_clock::now();

        // [lock-order] 데이터 스레드는 td_queue_의 생산자가 아니다 — 전용 SPSC 큐로 보낸다.
        while (!rest_td_queue_.push(td) && running_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(1ms);
        }

        ++data_count_;
    }
}

// ─── 데이터 수집 스레드 ───────────────────────────────────────────────────
void Engine::data_thread_fn()
{
    LOG_INFO("[DataThread] 시작");
    bool was_market_open = false;

    while (running_.load(std::memory_order_acquire))
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

            have_pnl_baseline_ = false; // C-1: 새 거래일 → 총평가금 기준선 재캡처
            LOG_INFO(std::string("[DataThread] 장 개장 전이(") + (is_kr_market_open() ? "KR" : "US") +
                     ") — OrderGate 일별 카운터 리셋");

            // 국면 판정(장 시작) → 국면에 맞는 전략셋 선택·적용 (G1). 선택 결정은 로그로 기록.
            if (regime_)
            {
                auto snap = regime_->evaluate();   // 내부에서 [Regime] 로그
                apply_regime_selection(snap.regime, /*force_log=*/true);
                last_regime_eval_ = std::chrono::steady_clock::now();
            }
        }

        was_market_open = market_now;

        if (!market_now)
        {
            std::this_thread::sleep_for(60s);
            continue;
        }

        try
        {
            // 매크로 레짐 게이트: 보조 프로세스가 쓴 regime.json → OrderGate entry_halt 토글.
            //  재스캔/잔고 대조와 같은 "사이클 1회" 계층. rest·일봉 모드 공통 경로라 두 모드 다 커버.
            poll_regime_file();

            // G1: 장중 국면 재평가 → 국면이 바뀌면 전략셋 동적 재선택(국면 전환 시 교체).
            //  일봉 기반 국면 신호라 장중 변화는 드물지만, 재평가로 국면 전이를 놓치지 않는다.
            //  RegimeController::evaluate()는 이 data_thread 단일 호출자라 재호출 계약 위반 없음.
            if (regime_ && regime_reeval_interval_sec_ > 0)
            {
                auto now_r = std::chrono::steady_clock::now();

                if (last_regime_eval_.time_since_epoch().count() == 0 ||
                    now_r - last_regime_eval_ >= std::chrono::seconds(regime_reeval_interval_sec_))
                {
                    auto snap = regime_->evaluate();
                    apply_regime_selection(snap.regime, /*force_log=*/false); // 변화 시에만 로그
                    last_regime_eval_ = now_r;
                }
            }

            // 주기적 유니버스 재스캔(동적 등록) — 슬리브별 주기는 각 job이 자체 판단한다.
            if (!rescan_jobs_.empty())
            {
                {
                    maybe_rescan_universe();

                    // G1: 재스캔으로 새로 등록된 전략도 현재 국면 선택에 맞춰 즉시 게이팅
                    //  (기본 active_=true로 잘못된 국면에 진입하는 창을 닫는다). 국면 불변이라
                    //  force_log=false → 로그 노이즈 없음.
                    if (regime_ && last_selected_regime_ != Regime::UNKNOWN)
                    {
                        apply_regime_selection(last_selected_regime_, /*force_log=*/false);
                    }
                }
            }

            const bool rest_now = rest_feed_active_.load(std::memory_order_relaxed);

            // 매 사이클 잔고 대조. 폴링 모드는 원장까지 덮어쓰고(체결콜백 부재 보완),
            //  WS 모드는 총평가금·일손익만 갱신한다. WS 모드에서 이걸 건너뛰면 equity가 0에
            //  머물러 총노출 게이트가 조용히 통과만 하고, 일간손실 한도의 기준값도 안 움직인다.
            reconcile_from_balance(/*resync_positions=*/rest_now);

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
                std::vector<std::string> stale;
                const auto               cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(60);

                {
                    std::lock_guard<std::mutex> lk(last_px_mu_);

                    for (const auto& h : order_gate_.snapshot_positions())
                    {
                        auto it = last_px_.find(h.ticker);

                        if (h.qty > 0 && (it == last_px_.end() || it->second.at < cutoff))
                        {
                            stale.push_back(h.ticker);
                        }
                    }
                }

                KisClient* qc = quote_kis_ ? quote_kis_.get() : kis_.get();

                for (const auto& ticker : stale)
                {
                    if (!running_.load(std::memory_order_acquire))
                    {
                        break;
                    }

                    std::this_thread::sleep_for(300ms);
                    set_last_px(ticker, qc->get_current_price(ticker));
                }
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
                    KisClient* eqc = quote_kis_ ? quote_kis_.get() : kis_.get();

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
                    KisClient* sqc = quote_kis_ ? quote_kis_.get() : kis_.get();

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

                        std::sort(secs.begin(), secs.end(),
                                  [](const SecRate& a, const SecRate& b) { return a.rate > b.rate; });

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

                // REST 현재가 폴링 → TradeData로 td_queue_에 주입(WS on_trade 경로 대체).
                //  깨진 일봉(G1/G2) 대신 살아있는 get_current_price를 쓰고, ITB는 이 틱으로
                //  1분 버킷 채널을 구성/스탑 평가한다. KST HHMMSS를 time에 채워 넣는다.
                struct tm kst = utc_plus_hours(9);
                char hhmmss[8];
                std::snprintf(hhmmss, sizeof(hhmmss), "%02d%02d%02d", kst.tm_hour, kst.tm_min,
                              kst.tm_sec);
                KisClient* qc = quote_kis_ ? quote_kis_.get() : kis_.get();

                for (const auto& spec : watch_specs_)
                {
                    if (spec.market != Market::KR)
                    {
                        continue;
                    }

                    // 실전 도메인 시세는 초당 호출 한도(~20/s)가 있어, 수십 종목을 무간격으로
                    //  몰아치면 뒷종목이 HTTP 500(초당 한도)로 떨어진다. 종목 간 소량 슬립(150ms)으로
                    //  한도 밑에 깔아 전 종목이 매 사이클 틱을 받도록 한다(종목 수×150ms가 30초 사이클 안에 들게).
                    std::this_thread::sleep_for(150ms);
                    double px = qc->get_current_price(spec.ticker);

                    if (px <= 0.0)
                    {
                        continue;
                    }

                    TradeData td;
                    td.ticker = spec.ticker;
                    td.time = hhmmss;
                    td.price = px;
                    td.quantity = 0;
                    td.direction = 0;
                    td.market = Market::KR;
                    td.timestamp = std::chrono::system_clock::now();

                    while (!td_queue_.push(td) && running_.load(std::memory_order_acquire))
                    {
                        std::this_thread::sleep_for(1ms);
                    }

                    ++data_count_;
                }
            }
            else
            {
                // 차트(일봉) TR은 모의 도메인에서 HTTP 500을 돌려준다. 주문 클라이언트로 부르면
                //  종목 수×사이클마다 500이 쌓여 로그가 그걸로 덮인다(3회 재시도까지 붙는다).
                //  위 rest 분기와 같이 시세 클라이언트로 부른다.
                KisClient* qc = quote_kis_ ? quote_kis_.get() : kis_.get();

                // 일봉을 받아 쓰는 전략이 하나도 없으면 폴링 자체를 건너뛴다. DevScale·ITB처럼
                //  호가·체결 이벤트로만 도는 구성에서는 이 루프가 종목 수만큼 차트 TR을 매 사이클
                //  때리고 결과는 아무도 안 본다. 그 호출량이 초당 한도를 밀어 다른 조회(3분봉·현재가)까지
                //  500으로 떨어뜨린다. 전략 집합은 국면 전환으로 바뀌므로 매 사이클 다시 확인한다.
                if (daily_bars_needed())
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

                        while (!market_queue_.push(md) && running_.load(std::memory_order_acquire))
                        {
                            std::this_thread::sleep_for(1ms);
                        }

                        ++data_count_;
                    }
                }

                poll_ws_overflow();
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

            for (const auto& job : rescan_jobs_)
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

            while (slept < cycle && running_.load(std::memory_order_acquire))
            {
                const int step = (slice < cycle - slept) ? slice : (cycle - slept);
                std::this_thread::sleep_for(std::chrono::seconds(step));
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
//  RegimeController의 전략선택 축과는 별개 관심사 — 선택 축은 apply_regime_selection() 참조.
// 시간 상자가 지났는지만 답한다. 매크로 축의 지표 5개 중 4개(IXIC·US500·VIX·DGS10)는 간밤
//  미국 종가라 KST 09:00~15:30 내내 값이 안 변한다. 회복을 볼 수 없는 신호가 장중 거부권을 계속
//  쥐면 2026-09-10처럼 지수가 되돌아온 뒤에도 4시간 동안 청산만 나간다. 만료 뒤 통제는 장중을
//  실제로 보는 축(UniverseScanner 코스피 게이트·종목 정배열)이 갖는다.
//  호출자는 둘이다 — poll_regime_file 진입부의 해제와, 정상 판정에서 halt를 켜지 않는 판정. [why D-033]
bool Engine::regime_halt_time_box_passed()
{
    if (regime_halt_expire_min_ <= 0)
    {
        return false;   // 기능 끔
    }

    struct tm kst = utc_plus_hours(9);

    if (kst.tm_yday != regime_halt_expire_yday_)
    {
        regime_halt_expire_yday_ = kst.tm_yday;
        regime_halt_expired_     = false;
    }

    // 09:00~15:30을 분으로 편 값(is_kr_market_open과 같은 기준). 개장 전은 음수라 안 걸린다.
    const int after_open = kst.tm_hour * 60 + kst.tm_min - 540;
    return after_open >= regime_halt_expire_min_ && after_open < 390;
}

// 만료 로그는 하루 한 번이면 된다. 폴링 주기마다 같은 줄이 쌓이면 로그가 못 쓰게 된다.
void Engine::log_regime_halt_expiry_once()
{
    if (regime_halt_expired_)
    {
        return;
    }

    LOG_WARN("[Regime] 매크로 진입정지 만료 — 개장 후 " +
             std::to_string(regime_halt_expire_min_) +
             "분 경과. 이 축은 장중 갱신되지 않으므로 오늘 남은 시간의 신규진입 판단은 "
             "유니버스 지수 게이트와 종목 정배열에 맡긴다");
    regime_halt_expired_ = true;
}

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

void Engine::poll_regime_file()
{
    if (regime_file_.empty())
    {
        return; // 기능 미가동(기본)
    }

    // 시간 상자는 파일을 읽기 전에, 한 곳에서 집행한다. 이 아래로는 halt를 건 채 빠져나가는
    //  경로가 다섯이다(파일 없음·stale·열기 실패·파싱 예외·판정 보류). 보조 프로세스가 죽는
    //  가장 흔한 모양이 파일 삭제와 쓰기 중 잘림인데, 분기마다 같은 해제를 적으면 한 곳은
    //  빠지고 그날 halt가 안 풀린다 — 그게 09-10의 실패 모양이었다.
    //  force_liquidate 중에는 풀지 않는다(극단 위험회피를 시계로 풀지 않는다). [why D-033]
    if (regime_halt_on_ && !regime_liq_warned_ && regime_halt_time_box_passed())
    {
        log_regime_halt_expiry_once();
        order_gate_.set_entry_halt(false);
        regime_halt_on_ = false;
    }

    std::error_code ec;

    if (!std::filesystem::exists(regime_file_, ec) || ec)
    {
        return; // 파일 없음 → 게이트 불변
    }

    // 신선도: 보조 프로세스가 죽어 파일이 오래되면 신뢰 불가 → halt를 새로 켜지 않는다.
    auto ftime = std::filesystem::last_write_time(regime_file_, ec);

    if (!ec)
    {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
                       std::filesystem::file_time_type::clock::now() - ftime).count();

        if (age > regime_stale_sec_)
        {
            if (!regime_stale_warned_)
            {
                LOG_WARN("[Regime] regime.json " + std::to_string(age) + "s 경과(> " +
                         std::to_string(regime_stale_sec_) +
                         "s) — 보조 프로세스 중단 의심, 게이트 신규 변경 보류(현 halt 유지)");
                regime_stale_warned_ = true;
            }

            return;   // 해제는 진입부에서 이미 집행했다
        }

        regime_stale_warned_ = false;
    }

    // 파싱 — 원자적 write라 정상은 완전한 json. 실패(부분/손상)는 조용히 무시.
    nlohmann::json j;

    try
    {
        std::ifstream f(regime_file_);

        if (!f)
        {
            return;
        }

        f >> j;
    }
    catch (const std::exception&)
    {
        return;
    }

    if (!j.value("valid", false))
    {
        return; // 보조 프로세스가 데이터 부족으로 판정 보류 → 게이트 불변(해제는 진입부에서 집행)
    }

    // ── force_liquidate: 청산 중엔 신규 진입도 반드시 정지(entry_halt에 OR) ────────
    bool liq = j.value("force_liquidate", false);

    // ── entry_halt 전이 시에만 set + 로그 (liq이면 강제 halt) ────────────────────
    bool halt = j.value("entry_halt", false) || liq;

    // 시간 상자: 개장 후 N분이 지나면 매크로 축의 진입정지를 스스로 만료시킨다.
    //  force_liquidate는 만료 대상이 아니다 — 극단 위험회피를 시계로 풀지 않는다. [why D-033]
    if (halt && !liq && regime_halt_time_box_passed())
    {
        log_regime_halt_expiry_once();
        halt = false;
    }

    if (halt != regime_halt_on_)
    {
        order_gate_.set_entry_halt(halt);
        regime_halt_on_ = halt;
        std::string reg = j.value("regime", std::string("?"));
        int score = j.value("risk_score", 0);
        LOG_WARN(std::string("[Regime] 신규진입 ") +
                 (halt ? "정지(ENTRY_HALT ON)" : "재개(ENTRY_HALT OFF)") +
                 " — regime=" + reg + " score=" + std::to_string(score));
    }

    // ── force_liquidate 배선(G3): 전이 시 플래그를 세워 strategy_thread가 전량 청산 ──
    //  실제 매도는 order_queue_ 단일 생산자인 strategy_thread에서만 낸다(SPSC 준수).
    //  여기(data_thread)는 원자 플래그만 토글하고 1회 로그만 남긴다.
    if (liq && !regime_liq_warned_)
    {
        LOG_ERROR("[Regime] force_liquidate=TRUE (극단 위험회피) — 보유 전량 강제청산 요청, "
                  "strategy_thread가 시장가 매도 발주");
        regime_liq_warned_ = true;
    }

    if (!liq && regime_liq_warned_)
    {
        LOG_WARN("[Regime] force_liquidate 해제 — 강제청산 중단");
        regime_liq_warned_ = false;
    }

    force_liquidate_.store(liq, std::memory_order_relaxed);
}

// ─── 전략 처리 스레드 ─────────────────────────────────────────────────────
// ob_queue_(호가) → td_queue_(체결) → market_queue_(일봉) 순 우선처리
// 아이들 시 100µs 슬립 → 저지연 유지
void Engine::strategy_thread_fn()
{
    LOG_INFO("[StrategyThread] 시작");

    auto raw_push = [&](const OrderSignal& sig)
    {
        ++signal_count_;

        // 취소·정정은 수량이 0이라 side만 찍으면 "BUY 0"으로 나온다. 09-09에 그 줄을 보고
        //  0주 매수 결함으로 오인해 한참 뒤졌다. 무엇을 하는 신호인지 앞에 적는다.
        const char* act = (sig.action == OrderAction::CANCEL)  ? "취소 "
                        : (sig.action == OrderAction::REPLACE) ? "정정 "
                                                               : "";
        const std::string tgt = sig.orig_client_oid.empty()
                              ? std::string()
                              : " 대상=" + sig.orig_client_oid;

        LOG_INFO("[Strategy] 신호: [" + sig.strategy_id + "] " + ticker_label(sig.ticker) + " " +
                 act + (sig.side == OrderSide::BUY ? "BUY" : "SELL") + " " +
                 std::to_string(sig.quantity) + tgt +
                 (sig.reason.empty() ? "" : " | 근거: " + sig.reason));
#ifdef HAS_ZMQ
        if (zmq_bridge_)
        {
            zmq_bridge_->publish_signal(sig);
        }
#endif
        while (!order_queue_.push(sig) && running_.load())
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    // 교체 진입 — 슬롯이 꽉 찬 상태에서 더 높은 점수의 신규 종목이 오면 최약체를 먼저 비운다.
    //  비우고 끝내는 이유: 매도 체결은 비동기라 같은 틱에 매수를 붙이면 노출이 이중 계상된다.
    //  게이트가 빈 자리를 이 종목에게 예약해 두고, 매수 신호는 이 스레드가 들고 있다가 자리가
    //  나면 낸다. 흘리기만 하면 안 되는 이유: 전략은 자기 예약이 살아 있다고 낙관하므로(계획
    //  시그니처 가드) 게이트 거부를 모르고 다시 내지 않는다. 그러면 예약된 슬롯이
    //  displace_slot_hold_sec 동안 비어 있다가 만료되고, 그동안 다른 종목까지 "예약분" 거부를
    //  받는다(09-11 10:05 322000: 교체 매도 뒤 매수는 40>=40 거부, 5분간 아무도 못 삼).
    //  order_queue_ 단일 생산자 규약을 지키려면 발주는 반드시 이 스레드에서만 나가야 한다.
    std::vector<OrderSignal> displace_held;        // 수혜 종목의 매수 신호(rung 전부)
    std::string              displace_held_ticker;
    auto                     displace_held_until = std::chrono::steady_clock::time_point{};

    auto push_signal = [&](const OrderSignal& sig)
    {
        const auto& gcfg   = order_gate_.config();
        const bool  buy_new = sig.side == OrderSide::BUY && sig.action == OrderAction::NEW;

        if (!displace_held_ticker.empty() && sig.ticker == displace_held_ticker)
        {
            if (sig.action == OrderAction::CANCEL)
            {
                displace_held.clear(); // 전략이 분할 매수를 다시 깐다 — 새 rung이 뒤따른다
            }
            else if (buy_new && std::chrono::steady_clock::now() < displace_held_until &&
                     order_gate_.capacity_full())
            {
                displace_held.push_back(sig); // 아직 자리가 안 났다 — 같은 분할 매수의 다음 rung
                return;
            }
        }

        if (gcfg.displace_enabled && buy_new &&
            order_gate_.position(sig.account_id, sig.ticker) == 0 &&
            order_gate_.reserved(sig.account_id, sig.ticker) == 0 &&
            order_gate_.capacity_full())
        {
            auto plan = order_gate_.plan_displacement(sig.account_id, sig.ticker);

            if (plan.ok)
            {
                OrderSignal ev;
                ev.ticker      = plan.ticker;
                ev.account_id  = plan.account;
                ev.side        = OrderSide::SELL;
                ev.type        = OrderType::MARKET;
                ev.quantity    = plan.qty;
                ev.price       = 0.0;
                ev.ref_price   = plan.avg_price; // 시장가 명목 백스톱이 우회되지 않게 평단을 stamp
                ev.strategy_id = "DISPLACE";
                ev.reason      = plan.reason;
                LOG_INFO("[Displace] " + ticker_label(plan.ticker) + " 전량 매도 " +
                         std::to_string(plan.qty) + "주 — " + plan.reason);
                raw_push(ev);
                order_gate_.note_displacement(plan, sig.ticker);
                displace_held.clear();
                displace_held_ticker = sig.ticker;
                displace_held_until  = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(gcfg.displace_slot_hold_sec > 0
                                                            ? gcfg.displace_slot_hold_sec : 120);
                displace_held.push_back(sig); // 매도 체결로 자리가 나면 루프 머리에서 낸다
                return;
            }
        }

        raw_push(sig);
    };

    // 교체 매도가 체결돼 자리가 났으면 들고 있던 수혜 종목 매수를 낸다. 예약 시한이 지나면 버린다 —
    //  그 뒤엔 게이트 예약도 풀려 있어 전략의 다음 재구성이 보통 경로로 들어온다.
    auto flush_displace_held = [&]()
    {
        if (displace_held_ticker.empty())
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        if (now >= displace_held_until)
        {
            if (!displace_held.empty())
            {
                LOG_WARN("[Displace] 보류 매수 만료 " + ticker_label(displace_held_ticker) + " " +
                         std::to_string(displace_held.size()) + "건 — 자리가 안 나 버린다");
            }

            displace_held.clear();
            displace_held_ticker.clear();
            return;
        }

        if (displace_held.empty() || order_gate_.capacity_full())
        {
            return;
        }

        LOG_INFO("[Displace] 자리가 나 보류 매수 " + std::to_string(displace_held.size()) + "건 발주 " +
                 ticker_label(displace_held_ticker));

        for (const auto& s : displace_held)
        {
            raw_push(s);
        }

        displace_held.clear();
    };

    std::vector<OrderSignal> batch_buf; // MM 다건 발주 재사용 버퍼 (per-tick 할당 회피)

    // 기동 점검 — 모의계좌 주문경로 검증용 1회성 시장가 매수(config startup_probe).
    //  이 스레드가 order_queue_ 단일 생산자라 여기서 딱 1번 push하면 SPSC 위반 없음.
    //  하루 한 번만 — 표식 파일이 있으면 재기동에서는 건너뛴다. 체결이 확인되면 되판다(아래 루프).
    if (!startup_probe_fired_ && !startup_probe_ticker_.empty() && startup_probe_qty_ > 0)
    {
        startup_probe_fired_ = true;
        const auto marker = Logger::instance().path_for("startup_probe_" + today_ymd());
        std::error_code ec;

        if (std::filesystem::exists(marker, ec))
        {
            LOG_INFO("[Engine] 기동 점검 — 오늘 이미 냈다(" + marker.filename().string() + "), 건너뜀");
        }
        else
        {
            std::ofstream(marker) << "fired\n";
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
        }
    }

    // strategies_ 무락 순회용 StrategyBase* 스냅샷. data_thread의 재스캔 등록·해제가
    // strat_version_을 올릴 때만 락 하에 재구성한다(틱마다 락 회피). 뗀 전략은 retired_가
    // 붙들고 있어 재구성 전의 옛 포인터도 유효하다(reap_retired가 seen 버전을 보고 파기).
    // 국면 게이트 적용 지점. apply_regime_selection()이 set_active로 표시만 해 두고
    //  틱 디스패치는 그 표시를 보지 않아, 국면-전략 자동선택이 실제로는 아무것도 막지
    //  않았다(2026-09-08 확인). 신규매수만 막고 청산·취소·정정은 통과시킨다 —
    //  entry_halt와 같은 규약이다. 비활성 전략이 보유분을 못 팔면 보호가 사라진다.
    std::unordered_set<std::string> guard_block_logged;
    auto emit_from = [&](StrategyBase* s, const OrderSignal& sig)
    {
        if (s && !s->is_active() && sig.action == OrderAction::NEW &&
            sig.side == OrderSide::BUY)
        {
            return;
        }

        // 청산 관리가 맡은 티커는 스캔 슬리브가 새로 사지도, 팔지도 않는다. 소유자를 하나로
        //  두지 않으면 청산 관리가 턴 물량을 스캔 전략이 되사는 회전이 나고, 매도가 둘에서
        //  나가면 같은 보유분에 두 장의 매도가 걸린다(sellable_qty 클램프가 있어도 순서에
        //  따라 한쪽이 0을 받아 분할 주문을 3초마다 되감는다). 취소·정정은 통과한다 — 이미 낸
        //  주문을 거두는 길까지 막으면 미체결이 미연결 주문이 된다.
        if (s && sig.action == OrderAction::NEW &&
            guardian_tickers_.count(sig.ticker) && s->id().rfind("ITB_", 0) != 0)
        {
            if (guard_block_logged.insert(sig.ticker).second)
            {
                LOG_INFO("[Engine] 청산 관리 보유종목 신규 " +
                         std::string(sig.side == OrderSide::BUY ? "매수" : "매도") + " 차단 " +
                         ticker_label(sig.ticker) + " (요청 " + s->id() + ") — 매매 소유권은 청산 관리에 있다");
            }

            return;
        }

        push_signal(sig);
    };

    std::vector<StrategyBase*> snap;
    uint64_t seen_ver = static_cast<uint64_t>(-1);

    // G3 강제청산 재발주 스로틀 — dedup 윈도우(1s)보다 길게 잡아 재발주가 통과되게.
    auto last_liq_attempt = std::chrono::steady_clock::now();
    // 기동 시 종목당 명목 한도 초과분 정리(1회). 재기동으로 미체결 분할 매수 기억이 사라지면
    //  게이트가 보유를 0으로 보고 같은 층을 다시 깔아 한도를 넘겨 체결시킨다(09-08 047050:
    //  한도 800만인데 명목 2,080만). 넘긴 채로 두면 다음 재기동에서 또 얹히므로, 시드가
    //  끝난 뒤 초과분을 시장가로 덜어낸다. 전량 청산이 아니라 한도까지만 줄인다.
    bool trim_done  = false;
    auto trim_after = std::chrono::steady_clock::now() + std::chrono::seconds(20);

    while (running_.load(std::memory_order_acquire))
    {
        uint64_t ver = strat_version_.load(std::memory_order_acquire);

        if (ver != seen_ver)
        {
            std::lock_guard<std::mutex> lk(strat_mutex_);
            snap.clear();
            snap.reserve(strategies_.size());

            for (auto& s : strategies_)
            {
                snap.push_back(s.get());
            }

            seen_ver = ver;
            // 뗀 전략은 이 시점부터 스냅샷에 없다. data_thread는 이 값을 보고 파기한다.
            strat_seen_version_.store(ver, std::memory_order_release);
        }

        flush_displace_held();

        // ── G3 강제청산: force_liquidate 동안 매 주기(≤2s) 보유 전량 시장가 매도 ──
        //  order_queue_ 단일 생산자(이 스레드)에서만 발주 → SPSC 준수. reserved(미체결
        //  매도)만큼 차감해 오버셀 방지, 잔량이 남는 한 재발주(G3-2 좌초 방지).
        //  entry_halt가 함께 켜져 SELL만 통과(check §1b). 대량은 fat-finger/레이트리밋에
        //  일부 막힐 수 있으나 다음 주기에 잔량 재시도된다.
        // 운영단말 수동주문 — 소켓 스레드가 넣은 요청을 여기서 OrderSignal로 바꾼다(단일 생산자).
        drain_manual_inbox(push_signal);

        if (force_liquidate_.load(std::memory_order_relaxed))
        {
            auto now = std::chrono::steady_clock::now();

            if (now - last_liq_attempt >= std::chrono::seconds(2))
            {
                last_liq_attempt = now;

                for (const auto& h : order_gate_.snapshot_positions())
                {
                    int resv = order_gate_.reserved(h.account, h.ticker);
                    int sell_pending = (resv < 0) ? -resv : 0; // 이미 낸 미체결 매도
                    int sellable = h.qty - sell_pending;

                    if (sellable <= 0)
                    {
                        continue;
                    }

                    OrderSignal s;
                    s.ticker      = h.ticker;
                    s.account_id  = h.account;
                    s.side        = OrderSide::SELL;
                    s.type        = OrderType::MARKET;
                    s.quantity    = sellable;
                    s.price       = 0.0;
                    // 시장가라 price=0 → 게이트 명목 백스톱이 우회되지 않도록 매입평단을 참조가로
                    // stamp(라이브 현재가가 없는 경로라 평단이 최선의 근사). fat-finger 명목 한도가
                    // 급락장 대량 강제청산에도 걸리게 한다.
                    s.ref_price   = h.avg_price;
                    s.strategy_id = "FORCE_LIQ";
                    s.reason      = "강제청산(force_liquidate) 보유=" + std::to_string(h.qty) +
                                    " 미체결매도=" + std::to_string(sell_pending);
                    push_signal(s);
                }
            }
        }

        // ── 종목당 명목 한도 초과분 정리 (기동 후 1회) ─────────────────────────
        //  강제청산과 같은 스레드(단일 생산자)에서 낸다 — order_queue_ SPSC 준수.
        // 기동 점검 되팔기 — 보유가 base+qty 이상이면 체결로 보고 같은 수량을 시장가로 판다.
        //  체결통보를 직접 보지 않고 원장 수량으로 판정한다(원장이 진실원천). 120초 안에 안 늘면
        //  접수 거부·미체결로 보고 포기한다 — 그때는 다음 기동에서 표식 때문에 다시 내지도 않는다.
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
                LOG_WARN("[Engine] 기동 점검 120초 안에 체결 확인 못 함(보유 " + std::to_string(qty) +
                         ") — 되팔기 생략");
                startup_probe_settled_ = true;
            }
        }

        if (!trim_done && std::chrono::steady_clock::now() >= trim_after)
        {
            trim_done = true;
            const double cap_notional = order_gate_.config().max_notional_per_ticker;

            if (cap_notional > 0.0)
            {
                for (const auto& h : order_gate_.snapshot_positions())
                {
                    if (h.qty <= 0 || h.avg_price <= 0.0)
                    {
                        continue;
                    }

                    const int cap_qty = static_cast<int>(cap_notional / h.avg_price);
                    int excess = h.qty - cap_qty;

                    if (excess <= 0)
                    {
                        continue;
                    }

                    // 이미 낸 미체결 매도만큼은 곧 줄어든다 — 중복으로 덜어내지 않는다.
                    const int resv = order_gate_.reserved(h.account, h.ticker);

                    if (resv < 0)
                    {
                        excess -= -resv;
                    }

                    if (excess <= 0)
                    {
                        continue;
                    }

                    OrderSignal s;
                    s.ticker      = h.ticker;
                    s.account_id  = h.account;
                    s.side        = OrderSide::SELL;
                    s.type        = OrderType::MARKET;
                    s.quantity    = excess;
                    s.price       = 0.0;
                    s.ref_price   = h.avg_price; // 시장가 명목 백스톱용 참조가
                    s.strategy_id = "LIMIT_TRIM";
                    s.reason      = "종목당 명목 한도 초과분 정리 보유=" + std::to_string(h.qty) +
                                    " 한도수량=" + std::to_string(cap_qty) +
                                    " 평단=" + std::to_string(static_cast<long long>(h.avg_price));
                    LOG_WARN("[Engine] 한도 초과분 정리 " + h.ticker + " " + std::to_string(h.qty) +
                             "주 -> " + std::to_string(cap_qty) + "주 (매도 " +
                             std::to_string(excess) + "주)");
                    push_signal(s);
                }
            }
        }

        bool did_work = false;

        try
        {
            // 호가 (국내 — 고주파)
            while (auto opt = ob_queue_.pop())
            {
                for (auto* s : snap)
                {
                    auto sig = s->on_order_book(*opt);

                    if (sig && sig->side != OrderSide::NONE)
                    {
                        emit_from(s, *sig);
                    }

                    // 다건 발주 경로 (MM 등) — 취소/정정 포함. 기본 no-op.
                    // CANCEL/REPLACE는 side가 NONE이어도 통과(생명주기 액션은 NONE 가드 우회).
                    batch_buf.clear();
                    s->on_order_book_batch(*opt, batch_buf);

                    for (auto& b : batch_buf)
                    {
                        if (b.action != OrderAction::NEW || b.side != OrderSide::NONE)
                        {
                            emit_from(s, b);
                        }
                    }
                }

                did_work = true;
            }

            // 체결 (미국 + 국내) — WS 콜백 큐가 비면 데이터 스레드의 REST 대체 틱 큐를 본다.
            auto pop_trade = [this]() -> std::optional<TradeData>
            {
                auto r = td_queue_.pop();

                if (!r)
                {
                    r = rest_td_queue_.pop();
                }

                return r;
            };

            while (auto opt = pop_trade())
            {
                // 운영단말 현재가용 캐시. 틱마다 짧은 락 한 번 — 전략 호출보다 훨씬 싸다.
                set_last_px(opt->ticker, opt->price);

                for (auto* s : snap)
                {
                    auto sig = s->on_trade(*opt);

                    if (sig && sig->side != OrderSide::NONE)
                    {
                        emit_from(s, *sig);
                    }

                    // 다건 발주 경로 (이격도 분할매매 등) — 체결틱/현재가 하트비트 구동.
                    // CANCEL/REPLACE는 side가 NONE이어도 통과(생명주기 액션은 NONE 가드 우회).
                    batch_buf.clear();
                    s->on_trade_batch(*opt, batch_buf);

                    for (auto& b : batch_buf)
                    {
                        if (b.action != OrderAction::NEW || b.side != OrderSide::NONE)
                        {
                            emit_from(s, b);
                        }
                    }
                }

                did_work = true;
            }

            // 일봉
            if (auto opt = market_queue_.pop())
            {
                for (auto* s : snap)
                {
                    auto sig = s->on_data(*opt);

                    if (sig && sig->side != OrderSide::NONE)
                    {
                        emit_from(s, *sig);
                    }
                }

                did_work = true;
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[StrategyThread] 예외: " + std::string(e.what()));
        }

        if (!did_work)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    LOG_INFO("[StrategyThread] 종료");
}

// ─── 주문 실행 스레드 ─────────────────────────────────────────────────────
void Engine::order_thread_fn()
{
    using std::chrono::steady_clock;
    using std::chrono::milliseconds;
    LOG_INFO("[OrderThread] 시작");

    // 로컬 재시도 큐 — order_queue_는 SPSC(생산자=전략 스레드)라 order_thread가 되밀면
    //  불변식이 깨진다. 거부된 청산 SELL만 이 전용 버퍼로 재시도한다.
    //  BUY는 제외: 빈-ODNO 응답이 실제로는 접수됐을 수 있어 재시도가 중복주문을 낳을 위험(C-2 주석).
    struct Retry { OrderSignal sig; int attempts; steady_clock::time_point not_before; };
    std::deque<Retry> retry_q;

    const auto min_interval = milliseconds(order_min_interval_ms_);
    // 재시도 지연은 dedup 창(기본 1s) 위 — 그 이하로 되쏘면 게이트 dedup(§5)에 또 막힌다.
    const auto retry_delay = milliseconds(std::max(order_min_interval_ms_, 1200));
    auto last_submit = steady_clock::now() - min_interval;

    while (running_.load(std::memory_order_acquire))
    {
        // 발주 대상 선택: 만기된 재시도분 우선, 없으면 신규 큐
        OrderSignal sig;
        int attempts = 0;
        bool have = false;
        auto now = steady_clock::now();

        if (!retry_q.empty() && now >= retry_q.front().not_before)
        {
            sig = retry_q.front().sig;
            attempts = retry_q.front().attempts;
            retry_q.pop_front();

            // 청산이 이미 끝났으면 재시도를 버린다. 원주문이 체결되는 동안 예약된 청산 SELL
            //  재시도가 큐에 남아 있다가, 보유가 0이 된 뒤에 발주돼 40240000(주문가능분 없음)으로
            //  거부되곤 했다. 거부라 원장은 다치지 않지만 청산 한 건마다 오거부가 몇 줄씩 쌓여
            //  진짜 거부를 덮는다. 재시도의 목적은 미청산분을 마저 파는 것이니 보유가 0이면 목적이
            //  이미 달성된 것이다.
            if (sig.action == OrderAction::NEW && sig.side == OrderSide::SELL &&
                order_gate_.position(sig.account_id, sig.ticker) <= 0)
            {
                LOG_INFO("[OrderThread] 청산 완료 — 재시도 취소 " + sig.ticker + " " +
                         std::to_string(sig.quantity) + "주");
                continue;
            }

            have = true;
        }
        else if (auto opt = order_queue_.pop())
        {
            sig = *opt;
            have = true;
        }

        if (!have)
        {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        // 호출 간격 조절 — 직전 KIS 발주 후 min_interval 경과 보장(초당한도 하회로 EGW00201 회피)
        now = steady_clock::now();

        if (now - last_submit < min_interval)
        {
            std::this_thread::sleep_for(min_interval - (now - last_submit));
        }

        try
        {
            // 간격은 KIS를 실제로 부른 뒤에만 센다. 로컬 거부(게이트·ENTRY_HALT)는 한도와 무관하다.
            const uint64_t calls_before = order_router_->kis_calls();
            auto mo = order_router_->submit(sig);

            if (order_router_->kis_calls() != calls_before)
            {
                last_submit = steady_clock::now();
            }

            if (ops_server_)
            {
                // 게이트·브로커를 지난 최종 결과. 단말은 cid로 자기 ORDER_ACK와 잇고, 전략 주문도
                //  같은 채널로 보여 운영 화면이 자동매매를 함께 본다.
                ops_server_->broadcast(ops::OpsMsg::ORDER_RESULT,
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
            else if (mo.status == OrderStatus::REJECTED && attempts < order_max_retries_ &&
                     (mo.reject_reason.find(kis_err::kRateLimit) != std::string::npos ||
                      mo.reject_reason.rfind("Rate limit", 0) == 0))
            {
                // 유량 한도 거부 — KIS가 '접수 전' 거부라 중복주문 위험 없음(빈-ODNO 모호성 없음).
                //  모든 action(취소·정정·매수·매도)을 dedup 창 밖으로 재예약해 유실 없이 자가치유한다.
                //  예전엔 취소·매수가 드롭돼 미연결 주문이 남고, 다음 사이클에 다시 처리되다 또
                //  한도초과가 나는 악순환이었다. 간격은 짧게 두고, 한도에 부딪힐 때만 물러난다.
                // [wire] KIS 서버 거부는 EGW00201, OrderGate 자체 거부는 "Rate limit 초과 (…)" 문자열이라
                //  둘을 같이 받는다. 앞엣것만 보던 동안 게이트 분당한도에 걸린 BUY가 조용히 드롭됐다.
                const std::string act = sig.action == OrderAction::CANCEL ? "CANCEL"
                                      : sig.action == OrderAction::REPLACE ? "REPLACE" : "NEW";

                // 분당 창은 비기까지 최대 60초다. 1.2초로 재예약하면 3회가 4초 안에 다 소진되고
                //  결국 같은 드롭이 된다. 분당 거부만 20초로 물러나 3회가 창 하나를 덮게 한다.
                const bool per_min = mo.reject_reason.find("분당") != std::string::npos;
                const auto delay   = per_min ? milliseconds(20000) : retry_delay;

                retry_q.push_back({sig, attempts + 1, steady_clock::now() + delay});
                LOG_WARN("[OrderThread] 유량한도 거부 → 재시도 예약 " + sig.ticker + " " + act + " (" +
                         std::to_string(attempts + 1) + "/" + std::to_string(order_max_retries_) +
                         ") 이유=" + mo.reject_reason);
            }
            else if (mo.status == OrderStatus::REJECTED && sig.action == OrderAction::NEW &&
                     sig.side == OrderSide::SELL && attempts < order_max_retries_ &&
                     mo.reject_reason.find(kis_err::kNoSellableQty) == std::string::npos)
            {
                // 청산 SELL 유실 방지(C-2) — dedup 창 밖에서 재시도 예약.
                //  단 40240000(주문가능분 없음)은 제외: 보유수량이 예약매도/미결제로 묶인 '지속성'
                //  조건이라 그냥 되쏘면 매번 같은 거부다. 유일 해법(예약매도 취소→시장가 재매도)은
                //  new_route의 reconcile_blocked_sell이 이미 인라인으로 1회 시도했으므로,
                //  여기서 또 재시도하면 reconcile만 중복 실행하고 결국 같은 거부가 반복된다.
                retry_q.push_back({sig, attempts + 1, steady_clock::now() + retry_delay});
                LOG_WARN("[OrderThread] 청산 SELL 거부 → 재시도 예약 " + sig.ticker + " (" +
                         std::to_string(attempts + 1) + "/" + std::to_string(order_max_retries_) +
                         ") 이유=" + mo.reject_reason);
            }
        }
        catch (const std::exception& e)
        {
            last_submit = steady_clock::now();
            LOG_ERROR("[OrderThread] 예외: " + std::string(e.what()));
        }
    }

    LOG_INFO("[OrderThread] 종료");
}

// ─── 장 시간 체크 (UTC 기반 → 머신 TZ 무관) ─────────────────────────────
static struct tm utc_plus_hours(int offset_h)
{
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    t += static_cast<time_t>(offset_h) * 3600;
    struct tm tm_out{};
#ifdef _WIN32
    gmtime_s(&tm_out, &t);
#else
    gmtime_r(&t, &tm_out);
#endif
    return tm_out;
}

bool Engine::daily_bars_needed()
{
    std::lock_guard<std::mutex> lk(strat_mutex_);

    for (const auto& s : strategies_)
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
    // KST = UTC+9, gmtime + 9h offset으로 머신 TZ 무관하게 계산
    auto kst = utc_plus_hours(9);

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
    auto kst = utc_plus_hours(9);

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
    if (rest_price_feed_)
    {
        return true; // 처음부터 폴링 — 낮출 것이 없다
    }

    // 폴링이 쓸 시세 소스. 모의 도메인은 시세 REST가 HTTP 500이라 실전 시세 클라이언트가
    //  없고 주문계좌마저 모의면 낮춰봐야 틱이 안 나온다. 그때는 거짓 안심을 주지 않는다.
    if (!quote_kis_ && kis_cfg_.is_paper)
    {
        return false;
    }

    if (!rest_fallback_engaged_)
    {
        rest_fallback_engaged_ = true;
        rest_feed_active_.store(true, std::memory_order_relaxed);
        LOG_ERROR("[Feed] WS → REST 폴링 폴백 (" + reason + ") — 틱 주기가 " +
                  std::to_string(fetch_interval_sec_) + "초로 떨어집니다. WS 복귀 시 자동 원복");
    }

    return true;
}

void Engine::deactivate_rest_fallback()
{
    if (!rest_fallback_engaged_)
    {
        return;
    }

    rest_fallback_engaged_ = false;
    rest_feed_active_.store(rest_price_feed_, std::memory_order_relaxed);
    LOG_INFO("[Feed] WS 수신 정상 — REST 폴링 폴백 해제, 실시간 피드로 복귀");
}

void Engine::control_thread_fn()
{
    using namespace std::chrono_literals;
    constexpr int kStaleThresholdSec = 30;
    constexpr int kCheckIntervalSec  = 5;

    int fail_streak = 0;
    auto next_try = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::seconds(kCheckIntervalSec));

        if (!ws_)
        {
            continue;
        }

        // 장 외 시간에는 stale이 정상 — 장 중에만 체크
        if (!is_any_market_open())
        {
            continue;
        }

        if (!ws_->is_stale(kStaleThresholdSec))
        {
            fail_streak = 0;   // 정상 수신 → 백오프 리셋
            deactivate_rest_fallback(); // 폴백으로 낮춰 뒀다면 WS로 되돌린다(전이 시에만 동작)
            continue;
        }

        // 재연결 백오프: 실패가 누적될수록 재시도 간격을 늘려 KIS 측 연결한도 소진/스팸 방지
        if (std::chrono::steady_clock::now() < next_try)
        {
            continue;
        }

        LOG_WARN("[Control] WebSocket " + std::to_string(kStaleThresholdSec) +
                 "초 이상 시세 미수신 — 재연결 시도");
        ws_->disconnect();
        std::vector<WatchSpec> specs_copy;
        {
            std::lock_guard<std::mutex> wl(watch_specs_mtx_); // data_thread의 재스캔 push_back과 겹친다
            specs_copy = watch_specs_;
        }

        if (ws_->connect(specs_copy))
        {
            LOG_INFO("[Control] WebSocket 재연결 성공");
            fail_streak = 0;
        }
        else
        {
            ++fail_streak;
            int backoff = std::min(30 * fail_streak, 300);   // 30s → ... → 최대 300s
            next_try = std::chrono::steady_clock::now() + std::chrono::seconds(backoff);
            LOG_ERROR("[Control] WebSocket 재연결 실패(" + std::to_string(fail_streak) +
                      "회) — " + std::to_string(backoff) + "초 후 재시도");

            // 반복 실패 시에만 대응한다(1회 실패로 즉시 조치하면 순간 장애에도 흔들린다).
            //  먼저 REST 폴링으로 낮춰 매매를 이어가고, 그것마저 불가할 때 kill switch로 멈춘다.
            //  예전에는 곧장 kill switch였다 — 시세 경로가 하나 죽었다고 매매 전체를 세울 이유는 없다.
            if (fail_streak >= 3 && !rest_fallback_engaged_)
            {
                if (!activate_rest_fallback("재연결 " + std::to_string(fail_streak) + "회 실패"))
                {
                    LOG_ERROR("[Control] 폴링 폴백 불가(시세 소스 없음) — kill switch 작동");
                    order_gate_.set_kill_switch(true);
                }
            }
        }
    }
}

// ─── 운영단말(OpsServer) 배선 ─────────────────────────────────────────────
//  서버 스레드에서 불리는 콜백은 큐에 넣거나 스냅샷을 읽기만 한다. 주문은 strategy_thread가
//  drain_manual_inbox에서 OrderSignal로 바꿔 push_signal로 낸다. [why D-043]

void Engine::start_ops_server()
{
    if (ops_port_ <= 0)
    {
        return;
    }

    ops_server_ = std::make_unique<OpsServer>();
    ops_server_->set_bind(ops_bind_addr_, ops_port_);
    ops_server_->set_token(ops_token_);
    ops_server_->set_paper(kis_cfg_.is_paper);
    ops_server_->set_status_provider([this] { return ops_status_json(); });
    ops_server_->set_positions_provider([this] { return ops_positions_json(); });
    ops_server_->set_kill_handler(
        [this]
        {
            LOG_WARN("[Ops] KILL — 신규 주문 차단 + 엔진 종료");
            order_gate_.set_kill_switch(true);
            running_.store(false);
        });
    ops_server_->set_order_handler(
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
                std::lock_guard<std::mutex> lk(manual_cid_mtx_);

                if (!manual_cids_.insert(r.cid).second)
                {
                    return "중복 cid — 이미 접수";
                }
            }

            if (!manual_inbox_.push(r))
            {
                std::lock_guard<std::mutex> lk(manual_cid_mtx_);
                manual_cids_.erase(r.cid);
                return "수동주문 인테이크 가득 참";
            }

            return std::string();
        });

    if (!ops_server_->start())
    {
        ops_server_.reset();
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
                          {"strategies", strategies_.size()}}
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
    while (auto req = manual_inbox_.pop())
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
            // 보유−미체결매도 범위 안에서만. 초과분을 브로커까지 보내면 40240000 거부로 재시도만 돈다.
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

            const int resv         = order_gate_.reserved(r.account, r.ticker);
            const int sell_pending = (resv < 0) ? -resv : 0;
            const int sellable     = held - sell_pending;

            if (sellable <= 0)
            {
                reject = "매도가능 0 (보유=" + std::to_string(held) + " 미체결매도=" + std::to_string(sell_pending) + ")";
            }
            else if (r.qty > sellable)
            {
                reject = "매도가능 " + std::to_string(sellable) + " 초과 요청 " + std::to_string(r.qty);
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

            if (ops_server_)
            {
                ops_server_->broadcast(ops::OpsMsg::ORDER_RESULT,
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
        s.side        = r.side == "SELL" ? OrderSide::SELL : OrderSide::BUY;
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
