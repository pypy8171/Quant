#include "core/Engine.h"
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
// 스냅샷을 재구성하도록 한다. watch_specs_는 data_thread 전용이라 무락으로 추가한다.
void Engine::register_strategy_runtime(std::unique_ptr<StrategyBase> strategy)
{
    if (!strategy)
        return;

    strategy->set_kis(kis_.get());
    strategy->set_position_provider([this](const std::string& account, const std::string& ticker) {
        return order_gate_.position(account, ticker);
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
        strategy->set_active(regime_->is_active_for(strategy->active_regimes()));

    // 구독 스펙 추가 (data_thread 전용 → 무락). 다음 폴링 사이클부터 현재가를 받는다.
    for (auto& spec : strategy->get_watch_specs())
    {
        if (spec.market == Market::KR)
            registered_tickers_.insert(spec.ticker);
        bool exists = false;
        for (auto& w : watch_specs_)
            if (w.market == spec.market && w.exchange == spec.exchange && w.ticker == spec.ticker)
            {
                exists = true;
                break;
            }
        if (!exists)
            watch_specs_.push_back(spec);
    }

    {
        std::lock_guard<std::mutex> lk(strat_mutex_);
        strategies_.push_back(std::move(strategy));
        strat_version_.fetch_add(1, std::memory_order_release);
    }
}

// 주기적 유니버스 재스캔 — universe_fn_으로 티커 목록을 산출해 미등록 종목만 런타임 등록.
void Engine::maybe_rescan_universe()
{
    if (!universe_fn_ || !strategy_factory_)
        return;

    KisClient* scan_kis = quote_kis_ ? quote_kis_.get() : kis_.get();
    if (!scan_kis)
        return;

    std::vector<std::string> tickers;
    try
    {
        tickers = universe_fn_(*scan_kis);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(std::string("[Engine] 유니버스 재스캔 예외: ") + e.what());
        return;
    }
    catch (...)
    {
        LOG_ERROR("[Engine] 유니버스 재스캔 알 수 없는 예외");
        return;
    }

    int added = 0;
    for (auto& t : tickers)
    {
        if (t.empty() || registered_tickers_.count(t))
            continue;
        auto strat = strategy_factory_(t);
        if (!strat)
            continue;
        LOG_INFO("[Engine] 재스캔 신규 등록: " + strat->describe());
        register_strategy_runtime(std::move(strat));
        ++added;
    }
    if (added > 0)
        LOG_INFO("[Engine] 유니버스 재스캔 완료: +" + std::to_string(added) +
                 "종목 (총 " + std::to_string(registered_tickers_.size()) + "종목)");
}

void Engine::start()
{
    if (running_.load())
        return;

    LOG_INFO("[Engine] ── 퀀트 엔진 시작 ──────────────────────────────");

#ifdef HAS_ZMQ
    zmq_bridge_ = std::make_unique<ZmqBridge>();
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
    if (rest_price_feed_ && has_quote_kis_)
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

    // G5: 실계좌 보유분을 원장에 시드 (스레드 시작 전, 단일스레드 구간)
    if (bootstrap_ledger_)
        bootstrap_ledger();

    // RegimeController (국면 메타레이어) 초기화
    regime_ = std::make_unique<RegimeController>();
    // 업종 지수 일봉도 시세이므로 모의 도메인은 HTTP 500. 시세 전용 실전 클라이언트가
    //  있으면 그걸로 조회(없으면 모의로 폴백 → NEUTRAL 유지).
    regime_->set_kis(quote_kis_ ? quote_kis_.get() : kis_.get());
    LOG_INFO("[Engine] RegimeController 초기화 완료");

    // 전략 초기화 (kis_ 주입 → on_start 내부에서 Universe 조회)
    for (auto& s : strategies_)
    {
        s->set_kis(kis_.get());
        // D2: 확정 포지션 접근자 주입 — 전략이 OrderGate 원장(WS/REST 공용)을 진실원천으로 읽음.
        s->set_position_provider([this](const std::string& account, const std::string& ticker) {
            return order_gate_.position(account, ticker);
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
                    watch_specs_.push_back(spec);
            }
        }
    }
    LOG_INFO("[Engine] WS 구독 종목: " + std::to_string(watch_specs_.size()) + "개");

    // 재스캔 중복 방지 시드 — 기동 유니버스에 이미 등록된 KR 티커 기록.
    registered_tickers_.clear();
    for (auto& spec : watch_specs_)
        if (spec.market == Market::KR)
            registered_tickers_.insert(spec.ticker);

    running_.store(true);

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
                               td_queue_.push(td);
#ifdef HAS_ZMQ
                               if (zmq_bridge_)
                                   zmq_bridge_->publish_trade(td);
#endif
                           });
        ws_->set_fill_callback([this](const FillNotification& fn)
                               {
                                   if (order_router_)
                                       order_router_->on_fill(fn);
                               });
        if (!ws_->connect(watch_specs_))
            LOG_WARN("[Engine] WebSocket 연결 실패 — 호가/체결 이벤트 없이 동작");
    }

    data_thread_ = std::thread(&Engine::data_thread_fn, this);
    strategy_thread_ = std::thread(&Engine::strategy_thread_fn, this);
    order_thread_ = std::thread(&Engine::order_thread_fn, this);
    control_thread_ = std::thread(&Engine::control_thread_fn, this);

    LOG_INFO("[Engine] 모든 스레드 시작 완료");
}

// ─── G5: 실계좌 보유분 원장 부트스트랩 ──────────────────────────────────────
//  get_balance output1의 pdno/hldg_qty/pchs_avg_pric를 OrderGate.seed_position으로 시드.
//  계좌키는 account=""(전략 신호의 기본 account_id와 일치, C-1). 평단까지 시드해야
//  매도 실현손익·일일손실 한도가 실제와 정합(C-2). reset_daily가 positions_/avg_prices_를
//  보존하므로 장 시작 리셋 후에도 유지 — 프로세스 기동당 1회면 충분.
void Engine::bootstrap_ledger()
{
    try
    {
        nlohmann::json bal = kis_->get_balance();
        if (!bal.contains("output1"))
        {
            LOG_WARN("[Engine] 원장 부트스트랩: 잔고 output1 없음 — 건너뜀");
            return;
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
                order_gate_.seed_position(std::string(), code, q, av);
                LOG_INFO("[Engine]   시드 " + code + " " + pname + " " + std::to_string(q) + "주 @평단 " +
                         std::to_string(static_cast<long long>(av)));
                ++n;
            }
        }
        LOG_INFO("[Engine] 원장 부트스트랩 완료: " + std::to_string(n) + "종목 시드");
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[Engine] 원장 부트스트랩 예외: " + std::string(e.what()));
    }
}

// ─── C-1: rest 모드 주기적 잔고 리컨사일 ────────────────────────────────────
//  rest_price_feed_ 모드는 체결콜백(OrderRouter::on_fill)이 미등록이라 positions_/
//  daily_pnl_이 갱신되지 않는다(치명). 데이터 스레드가 매 사이클 잔고를 재조회해:
//   1) output1 보유분으로 positions_/avg_prices_ 재동기(seed_position 덮어쓰기),
//   2) output2 총평가금(tot_evlu_amt)의 당일 기준선 대비 델타를 daily_pnl_로 세팅
//      → BUY-only 손실컷(§4)이 정상 작동(C-3의 신규매수 차단 의도 재무장).
//  ⚠️ 절대 평가손익(evlu_pfls)이 아니라 "당일 기준선 델타"를 쓴다. 이미 -30% 물린
//     미실현손실을 daily_pnl로 넣으면 개장 즉시 모든 신규매수가 막혀버리기 때문.
void Engine::reconcile_from_balance()
{
    try
    {
        nlohmann::json bal = kis_->get_balance();

        // 1) 보유분 재동기 — 실체결 원장을 실제 잔고로 강제 일치.
        if (bal.contains("output1"))
        {
            for (auto& h : bal["output1"])
            {
                std::string code = h.value("pdno", "");
                int    q  = std::atoi(h.value("hldg_qty", "0").c_str());
                double av = std::atof(h.value("pchs_avg_pric", "0").c_str());
                if (!code.empty() && q > 0)
                    order_gate_.seed_position(std::string(), code, q, av);
            }
        }

        // 2) 당일 총평가금 델타 → daily_pnl_. output2는 배열 또는 객체로 올 수 있어 양쪽 수용.
        double tot_eval = 0.0;
        bool have_eval = false;
        if (bal.contains("output2"))
        {
            const auto& o2 = bal["output2"];
            const nlohmann::json* row = nullptr;
            if (o2.is_array() && !o2.empty())
                row = &o2[0];
            else if (o2.is_object())
                row = &o2;
            if (row)
            {
                std::string s = row->value("tot_evlu_amt", "");
                if (s.empty())
                    s = row->value("nass_amt", ""); // 순자산금액 폴백
                if (!s.empty())
                {
                    try { tot_eval = std::stod(s); have_eval = true; } catch (...) {}
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
                time_t kt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + 9 * 3600;
                struct tm ktm{};
#ifdef _WIN32
                gmtime_s(&ktm, &kt);
#else
                gmtime_r(&kt, &ktm);
#endif
                char dbuf[9];
                std::strftime(dbuf, sizeof(dbuf), "%Y%m%d", &ktm);
                std::string bpath = std::string("logs/pnl_baseline_") + dbuf + ".txt";

                double file_base = 0.0;
                bool from_file = false;
                {
                    std::ifstream bf(bpath);
                    if (bf.is_open() && (bf >> file_base) && file_base > 0.0)
                        from_file = true;
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
                        of << static_cast<long long>(pnl_baseline_) << "\n";
                    LOG_INFO("[Engine] 기준선 신규 캡처+저장(" + std::string(dbuf) + "): 총평가금 " +
                             std::to_string(static_cast<long long>(tot_eval)) + "원");
                }
                have_pnl_baseline_ = true;
            }
            double delta = tot_eval - pnl_baseline_;
            order_gate_.set_daily_pnl(delta);
            LOG_INFO("[Engine] 리컨사일: 당일손익 " + std::to_string(static_cast<long long>(delta)) +
                     "원 (총평가 " + std::to_string(static_cast<long long>(tot_eval)) + ")");
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[Engine] 리컨사일 예외: " + std::string(e.what()));
    }
}

void Engine::stop()
{
    // exchange로 중복 호출 방지 — 이미 false면 즉시 반환
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;

    LOG_INFO("[Engine] 종료 시작");

    // 역순 join 권장: control → order → strategy → data
    if (control_thread_.joinable())  control_thread_.join();
    if (order_thread_.joinable())    order_thread_.join();
    if (strategy_thread_.joinable()) strategy_thread_.join();
    if (data_thread_.joinable())     data_thread_.join();

    if (ws_)
        ws_->disconnect();

#ifdef HAS_ZMQ
    if (zmq_bridge_)
        zmq_bridge_->stop();
#endif

    for (auto& s : strategies_)
        s->on_stop();
    print_stats();
    LOG_INFO("[Engine] 종료 완료");
}

static struct tm utc_plus_hours(int offset_h); // KST 계산용(정의는 하단)

// ─── 데이터 수집 스레드 ───────────────────────────────────────────────────
void Engine::data_thread_fn()
{
    LOG_INFO("[DataThread] 시작");
    bool was_market_open = false;
    while (running_.load(std::memory_order_acquire))
    {
        bool market_now = is_any_market_open();

        // 장 시작 감지 → 일별 카운터 리셋 + 국면 판정
        if (market_now && !was_market_open)
        {
            order_gate_.reset_daily();
            if (order_router_)
                order_router_->reset_daily();   // V-4: 멱등키 일별 정리(거래일 prefix와 함께 cross-day 충돌 차단)
            have_pnl_baseline_ = false; // C-1: 새 거래일 → 총평가금 기준선 재캡처
            LOG_INFO("[DataThread] 장 시작 — OrderGate 일별 카운터 리셋");

            // 국면 판정(장 시작 1회) → 전략별 활성 국면 설정 (R-1a: 판정·플래그만, 진입차단은 R-1b)
            if (regime_)
            {
                regime_->evaluate();   // 내부에서 [Regime] 로그
                for (auto& s : strategies_)
                    s->set_active(regime_->is_active_for(s->active_regimes()));
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
            // 매크로 레짐 게이트: 사이드카가 쓴 regime.json → OrderGate entry_halt 토글.
            //  재스캔/리컨사일과 같은 "사이클 1회" 계층. rest·일봉 모드 공통 경로라 두 모드 다 커버.
            poll_regime_file();

            // 주기적 유니버스 재스캔(동적 등록) — rescan_interval_sec_마다 신규 티커 런타임 추가.
            if (rescan_interval_sec_ > 0)
            {
                auto now_c = std::chrono::steady_clock::now();
                if (last_rescan_.time_since_epoch().count() == 0 ||
                    now_c - last_rescan_ >= std::chrono::seconds(rescan_interval_sec_))
                {
                    maybe_rescan_universe();
                    last_rescan_ = now_c;
                }
            }

            if (rest_price_feed_)
            {
                // C-1: 매 사이클 잔고 리컨사일(체결콜백 부재 보완) — 원장/일손실 재동기.
                reconcile_from_balance();

                // ── 당일 외국인·기관 추정 순매수 "관측 적재"(게이트 아님) ──────────────
                //  data-sourcer 판정: FHPTJ04400000는 추정/가집계 → 부호·상대크기만 신뢰.
                //  게이트로 승격 전, 매 사이클 스냅샷을 로그로 남겨 장중추정 vs 장후확정을
                //  나중에 대조한다(지금 안 남기면 영구 소실). 레이트 절약 위해 5분마다만.
                //  ⚠️ 스키마 미확정 — 첫 성공 응답의 원문 로그로 필드명 확정할 것.
                {
                    static int est_flow_tick = 0;
                    KisClient* eqc = quote_kis_ ? quote_kis_.get() : kis_.get();
                    if (eqc && (est_flow_tick % 10) == 0) // 30s×10 ≈ 5분
                    {
                        // 우리 유니버스(watch) 티커 집합 — 교집합만 강조 로깅.
                        std::unordered_set<std::string> ours;
                        for (const auto& s : watch_specs_)
                            if (s.market == Market::KR)
                                ours.insert(s.ticker);

                        auto buy_top  = eqc->fetch_est_investor_ranking("0000", "0", "0"); // 순매수 상위
                        std::this_thread::sleep_for(150ms);
                        auto sell_top = eqc->fetch_est_investor_ranking("0000", "1", "0"); // 순매도 상위

                        auto dump = [&](const char* label,
                                        const std::vector<KisClient::EstInvestorFlow>& v)
                        {
                            int shown = 0;
                            for (const auto& f : v)
                            {
                                if (shown >= 15)
                                    break;
                                bool mine = ours.count(f.ticker) > 0;
                                LOG_INFO(std::string("[수급추정] ") + label + " " +
                                         (mine ? "★" : " ") + f.ticker + " " + f.name +
                                         " 외인=" + std::to_string(f.foreign_net_qty) +
                                         " 기관=" + std::to_string(f.inst_net_qty) +
                                         " 외인금액=" + std::to_string((int64_t)f.foreign_net_amt));
                                ++shown;
                            }
                        };
                        LOG_INFO("[수급추정] ── 당일 외국인·기관 추정 순매수 스냅샷(관측용) ──");
                        dump("매수상위", buy_top);
                        dump("매도상위", sell_top);
                    }
                    ++est_flow_tick;
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
                        continue;
                    // 실전 도메인 시세는 초당 호출 한도(~20/s)가 있어, 33종목을 무간격으로
                    //  몰아치면 뒷종목이 HTTP 500(초당 한도)로 떨어진다. 종목 간 소량 슬립으로
                    //  한도 밑에 깔아 전 종목이 매 사이클 틱을 받도록 한다. 33×150ms≈5s ≪ 30s.
                    std::this_thread::sleep_for(150ms);
                    double px = qc->get_current_price(spec.ticker);
                    if (px <= 0.0)
                        continue;
                    TradeData td;
                    td.ticker = spec.ticker;
                    td.time = hhmmss;
                    td.price = px;
                    td.quantity = 0;
                    td.direction = 0;
                    td.market = Market::KR;
                    td.timestamp = std::chrono::system_clock::now();
                    while (!td_queue_.push(td) && running_.load(std::memory_order_acquire))
                        std::this_thread::sleep_for(1ms);
                    ++data_count_;
                }
            }
            else
            {
                for (const auto& spec : watch_specs_)
                {
                    std::vector<MarketData> bars;
                    if (spec.market == Market::KR)
                        bars = kis_->get_daily_ohlcv(spec.ticker, 1);
                    else
                        bars = kis_->get_us_daily_ohlcv(spec.ticker, 1, spec.exchange);

                    if (bars.empty())
                        continue;
                    auto& md = bars[0];
                    md.bar_index = static_cast<int>(data_count_.load());
                    while (!market_queue_.push(md) && running_.load(std::memory_order_acquire))
                        std::this_thread::sleep_for(1ms);
                    ++data_count_;
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[DataThread] 예외: " + std::string(e.what()));
        }
        std::this_thread::sleep_for(std::chrono::seconds(fetch_interval_sec_));
#ifdef HAS_ZMQ
        if (zmq_bridge_)
            zmq_bridge_->publish_health(data_count_.load(), signal_count_.load(), order_count_.load());
#endif
    }
    LOG_INFO("[DataThread] 종료");
}

// ─── 매크로 레짐 파일 폴링 → OrderGate entry_halt 토글 (data_thread 전용) ─────
//  Python macro_regime_feed.py가 원자적으로 쓰는 regime.json을 매 사이클 읽어,
//  entry_halt(신규 진입만 차단, 청산은 통과)를 국면에 맞춰 켜고 끈다.
//  set_entry_halt는 이 함수가 유일 호출자 — 다른 곳에서 토글하지 않으므로 소유권 단순.
//  안전장치: 파일 없음/손상/판정보류(valid=false)/stale이면 게이트를 "새로 켜지" 않는다.
//  (entry_halt는 자본보호 측 — 신규매수만 막고 청산은 통과 — 이라 유지가 실패안전)
void Engine::poll_regime_file()
{
    if (regime_file_.empty())
        return; // 기능 미가동(기본)

    std::error_code ec;
    if (!std::filesystem::exists(regime_file_, ec) || ec)
        return; // 파일 없음 → 게이트 불변

    // 신선도: 사이드카가 죽어 파일이 오래되면 신뢰 불가 → halt를 새로 켜지 않는다.
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
                         "s) — 사이드카 중단 의심, 게이트 신규 변경 보류(현 halt 유지)");
                regime_stale_warned_ = true;
            }
            return;
        }
        regime_stale_warned_ = false;
    }

    // 파싱 — 원자적 write라 정상은 완전한 json. 실패(부분/손상)는 조용히 무시.
    nlohmann::json j;
    try
    {
        std::ifstream f(regime_file_);
        if (!f)
            return;
        f >> j;
    }
    catch (const std::exception&)
    {
        return;
    }

    if (!j.value("valid", false))
        return; // 사이드카가 데이터 부족으로 판정 보류 → 게이트 불변

    // ── entry_halt 전이 시에만 set + 로그 ────────────────────────────────────
    bool halt = j.value("entry_halt", false);
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

    // ── force_liquidate: MVP는 로그만(강제청산 배선은 후속 Task). 전이 시 1회 ──
    bool liq = j.value("force_liquidate", false);
    if (liq && !regime_liq_warned_)
    {
        LOG_ERROR("[Regime] force_liquidate=TRUE (극단 위험회피) — 강제청산 배선 미구현, "
                  "수동 개입 권장");
        regime_liq_warned_ = true;
    }
    if (!liq)
        regime_liq_warned_ = false;
}

// ─── 전략 처리 스레드 ─────────────────────────────────────────────────────
// ob_queue_(호가) → td_queue_(체결) → market_queue_(일봉) 순 우선처리
// 아이들 시 100µs 슬립 → 저지연 유지
void Engine::strategy_thread_fn()
{
    LOG_INFO("[StrategyThread] 시작");

    auto push_signal = [&](const OrderSignal& sig)
    {
        ++signal_count_;
        LOG_INFO("[Strategy] 신호: [" + sig.strategy_id + "] " + sig.ticker + " " +
                 (sig.side == OrderSide::BUY ? "BUY" : "SELL") + " " + std::to_string(sig.quantity));
#ifdef HAS_ZMQ
        if (zmq_bridge_)
            zmq_bridge_->publish_signal(sig);
#endif
        while (!order_queue_.push(sig) && running_.load())
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    };

    std::vector<OrderSignal> batch_buf; // MM 다건 발주 재사용 버퍼 (per-tick 할당 회피)

    // strategies_ 무락 순회용 StrategyBase* 스냅샷. data_thread의 재스캔 등록이
    // strat_version_을 올릴 때만 락 하에 재구성한다(틱마다 락 회피). 삭제는 없으므로
    // 스냅샷 포인터는 벡터 재할당 후에도 힙 객체를 유효하게 가리킨다.
    std::vector<StrategyBase*> snap;
    uint64_t seen_ver = static_cast<uint64_t>(-1);

    while (running_.load(std::memory_order_acquire))
    {
        uint64_t ver = strat_version_.load(std::memory_order_acquire);
        if (ver != seen_ver)
        {
            std::lock_guard<std::mutex> lk(strat_mutex_);
            snap.clear();
            snap.reserve(strategies_.size());
            for (auto& s : strategies_)
                snap.push_back(s.get());
            seen_ver = ver;
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
                        push_signal(*sig);

                    // 다건 발주 경로 (MM 등) — 취소/정정 포함. 기본 no-op.
                    // CANCEL/REPLACE는 side가 NONE이어도 통과(생명주기 액션은 NONE 가드 우회).
                    batch_buf.clear();
                    s->on_order_book_batch(*opt, batch_buf);
                    for (auto& b : batch_buf)
                        if (b.action != OrderAction::NEW || b.side != OrderSide::NONE)
                            push_signal(b);
                }
                did_work = true;
            }

            // 체결 (미국 + 국내)
            while (auto opt = td_queue_.pop())
            {
                for (auto* s : snap)
                {
                    auto sig = s->on_trade(*opt);
                    if (sig && sig->side != OrderSide::NONE)
                        push_signal(*sig);

                    // 다건 발주 경로 (이격도 분할매매 등) — 체결틱/현재가 하트비트 구동.
                    // CANCEL/REPLACE는 side가 NONE이어도 통과(생명주기 액션은 NONE 가드 우회).
                    batch_buf.clear();
                    s->on_trade_batch(*opt, batch_buf);
                    for (auto& b : batch_buf)
                        if (b.action != OrderAction::NEW || b.side != OrderSide::NONE)
                            push_signal(b);
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
                        push_signal(*sig);
                }
                did_work = true;
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[StrategyThread] 예외: " + std::string(e.what()));
        }

        if (!did_work)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
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

        // 페이싱 — 직전 발주 후 min_interval 경과 보장(초당한도 하회로 EGW00201 회피)
        now = steady_clock::now();
        if (now - last_submit < min_interval)
            std::this_thread::sleep_for(min_interval - (now - last_submit));

        try
        {
            auto mo = order_router_->submit(sig);
            last_submit = steady_clock::now();
            if (mo.status == OrderStatus::ACCEPTED)
            {
                ++order_count_;
            }
            else if (mo.status == OrderStatus::REJECTED && sig.action == OrderAction::NEW &&
                     sig.side == OrderSide::SELL && attempts < order_max_retries_)
            {
                // 청산 SELL 유실 방지(C-2) — dedup 창 밖에서 재시도 예약.
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

bool Engine::is_kr_market_open() const
{
    // KST = UTC+9, gmtime + 9h offset으로 머신 TZ 무관하게 계산
    auto kst = utc_plus_hours(9);
    if (kst.tm_wday == 0 || kst.tm_wday == 6)
        return false;
    int m = kst.tm_hour * 60 + kst.tm_min;
    return m >= 540 && m < 930; // 09:00~15:30 KST
}

// 미국 정규장: ET 09:30~16:00 = KST 22:30~05:00 (다음날)
bool Engine::is_us_market_open() const
{
    auto kst = utc_plus_hours(9);
    if (kst.tm_wday == 0 || kst.tm_wday == 6)
        return false;
    int m = kst.tm_hour * 60 + kst.tm_min;
    // KST 22:30~익일 05:00 → 1350~1500 (당일), 0~300 (익일)
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
            continue;

        // 장 외 시간에는 stale이 정상 — 장 중에만 체크
        if (!is_any_market_open())
            continue;

        if (!ws_->is_stale(kStaleThresholdSec))
        {
            fail_streak = 0;   // 정상 수신 → 백오프 리셋
            continue;
        }

        // 재연결 백오프: 실패가 누적될수록 재시도 간격을 늘려 KIS 측 연결한도 소진/스팸 방지
        if (std::chrono::steady_clock::now() < next_try)
            continue;

        LOG_WARN("[Control] WebSocket " + std::to_string(kStaleThresholdSec) +
                 "초 이상 시세 미수신 — 재연결 시도");
        ws_->disconnect();
        if (ws_->connect(watch_specs_))
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
            if (fail_streak >= 3)   // 반복 실패 시에만 kill switch (1회 실패로 즉시 중단 방지)
                order_gate_.set_kill_switch(true);
        }
    }
}
