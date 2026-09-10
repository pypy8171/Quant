#include "core/Engine.h"
#include "modes/Monitors.h"
#include "strategy/StrategyFactory.h"
#include "utils/Logger.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

// ─── 전역 종료 플래그 ─────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static Engine* g_engine = nullptr;

void signal_handler(int)
{
    g_running.store(false);

    if (g_engine)
    {
        g_engine->stop();
    }
}

// ─── 설정 파일 로드 ───────────────────────────────────────────────────────
static json load_config(const std::string& path)
{
    std::ifstream f(path);

    if (!f.is_open())
    {
        throw std::runtime_error("설정 파일 없음: " + path);
    }

    return json::parse(f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  main — 설정·인증 부트스트랩 후 모드별 실행으로 디스패치한다.
//   · 관찰 모드(FEED/KR_TEST/US_TEST) → modes/Monitors.cpp
//   · TRADE 모드 → 엔진 구성 + strategy/StrategyFactory.cpp 로 전략 로딩
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    // 로그·산출물 기준 폴더는 Logger::default_base_dir()(QUANT_LOG_DIR > 실행파일 옆 logs/).
    Logger::instance().set_base_dir(Logger::default_base_dir());
    Logger::instance().init(Logger::instance().path_for("quant_trader.log"), LogLevel::INFO);
    LOG_INFO("=== Quant Trader v2.0 ===");

    // 인자 파싱: quant_trader [config] [MODE]
    //   quant_trader.exe                  → config/config.json, mode from json
    //   quant_trader.exe KR_TEST          → config/config.json, mode=KR_TEST
    //   quant_trader.exe US_TEST          → config/config.json, mode=US_TEST
    //   quant_trader.exe config.json TRADE → 지정 config, mode=TRADE
    std::string config_path = "config/config.json";
    std::string mode_override = "";

    for (int i = 1; i < argc; ++i)
    {
        std::string input_mode = argv[i];

        if (input_mode == "KR_TEST" || input_mode == "US_TEST" || input_mode == "FEED" || input_mode == "TRADE")
        {
            mode_override = input_mode;
        }
        else
        {
            config_path = input_mode;
        }
    }

    json cfg;

    try
    {
        cfg = load_config(config_path);
        LOG_INFO("[Main] 설정 로드: " + config_path);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(std::string("[Main] 설정 로드 실패: ") + e.what());
        return 1;
    }

    if (!mode_override.empty())
    {
        cfg["mode"] = mode_override;
        LOG_INFO("[Main] 모드 오버라이드: " + mode_override);
    }

    // KIS 설정
    KisConfig kis_cfg;
    kis_cfg.app_key      = cfg["kis"]["app_key"];
    kis_cfg.app_secret   = cfg["kis"]["app_secret"];
    kis_cfg.account_no   = cfg["kis"]["account_no"];
    kis_cfg.account_type = cfg["kis"]["account_type"].get<std::string>();
    kis_cfg.hts_id       = cfg["kis"].value("hts_id", ""); // 미설정 시 account_no 사용
    kis_cfg.is_paper     = cfg["kis"]["is_paper"].get<bool>();

    // FEED 모드 전용 — TRADE 모드는 전략이 동적으로 종목 구성
    std::vector<std::string> tickers;

    if (cfg.contains("tickers"))
    {
        tickers = cfg["tickers"].get<std::vector<std::string>>();
    }

    // 국내 선물 실시간(H0IFCNT0/H0IFASP0). 실계좌 WS 도메인 전용이라 kis.is_paper=false 필요.
    std::vector<std::string> futures;

    if (cfg.contains("futures"))
    {
        futures = cfg["futures"].get<std::vector<std::string>>();
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string mode = cfg.value("mode", "FEED");

    // ═══════════════════════════════════════════════════════════════════════
    //  관찰용 모니터 모드 — 각자 자기 루프를 돌다 종료 (modes/Monitors.cpp)
    // ═══════════════════════════════════════════════════════════════════════
    if (mode == "FEED")
    {
        return run_feed(kis_cfg, tickers, futures, g_running);
    }
    else if (mode == "KR_TEST")
    {
        return run_kr_test(kis_cfg, g_running);
    }
    else if (mode == "US_TEST")
    {
        return run_us_test(kis_cfg, g_running);
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  TRADE 모드: 전략 매매 엔진 (tickers 설정 불필요 — 전략이 동적으로 구성)
    // ═══════════════════════════════════════════════════════════════════════
    int interval = cfg.value("fetch_interval_sec", 60);
    Engine engine(kis_cfg, interval);
    g_engine = &engine;
    // G5: 기동 시 실계좌 보유분을 읽어 OrderGate 내부 장부의 초기값으로 채운다.
    //  (재시작하면 장부는 0이지만 실계좌엔 보유분이 남아, 안 맞추면 매도수량·평단·손실한도가 어긋남)
    engine.set_bootstrap_ledger(cfg.value("bootstrap_ledger_from_balance", false));
    // WS 세션이 rt_cd=9(ALREADY IN USE) 로 폭주할 때의 우회책. REST 현재가를 주기적으로 폴링해
    //  실시간 체결 틱처럼 전략에 먹인다. ITB(IntradayBreakoutStrategy, 장중 돌파)가 이 틱으로 돈다.
    engine.set_rest_price_feed(cfg.value("rest_price_feed", false));
    // 매크로 레짐 사이드카 브리지(2026-08-09 회의): Python macro_regime_feed.py가 쓰는 regime.json
    //  경로. 지정 시 data_thread가 매 사이클 읽어, 시장이 위험하면 OrderGate 의 신규매수 정지 스위치
    //  (entry_halt)를 켜고 풀리면 끈다. 빈 문자열(기본)이면 미가동 — 기존 동작 불변.
    engine.set_regime_file(cfg.value("regime_file", std::string()),
                           cfg.value("regime_stale_sec", kDefaultRegimeStaleSec));
    // ZMQ 제어 채널(config "zmq_bind_addr"·"zmq_control_token"). 주소를 안 주면 127.0.0.1에
    //  묶이고, 토큰이 비면 KILL 명령은 거부된다. 스레드 시작 전에만 유효하다.
    engine.set_zmq_control(cfg.value("zmq_bind_addr", std::string()),
                           cfg.value("zmq_control_token", std::string()));

    // G1: 국면→전략 자동선택 맵. config "regime_strategies": {"BULL":[id...], "NEUTRAL":[...], "BEAR":[...]}.
    //  id 항목이 '*'로 끝나면 접두 매칭(스캐너 동적 id: "DevScale_*"). 미지정이면 기존
    //  per-strategy active_regimes 방식 유지(하위호환). 지정 시 국면이 전략셋을 선택한다.
    if (cfg.contains("regime_strategies"))
    {
        auto to_regime = [](const std::string& k) -> Regime
        {
            if (k == "BULL")
            {
                return Regime::BULL;
            }

            if (k == "BEAR")
            {
                return Regime::BEAR;
            }

            if (k == "NEUTRAL")
            {
                return Regime::NEUTRAL;
            }

            return Regime::UNKNOWN;
        };
        std::map<Regime, std::vector<std::string>> rmap;

        for (auto it = cfg["regime_strategies"].begin(); it != cfg["regime_strategies"].end(); ++it)
        {
            Regime r = to_regime(it.key());

            if (r == Regime::UNKNOWN)
            {
                LOG_WARN("[Main] regime_strategies: 알 수 없는 국면 키 '" + it.key() + "' 무시");
                continue;
            }

            rmap[r] = it.value().get<std::vector<std::string>>();
        }

        engine.set_regime_strategies(rmap);
        engine.set_regime_reeval_interval(cfg.value("regime_reeval_sec", 300));
        LOG_INFO("[Main] 국면→전략 자동선택 맵 " + std::to_string(rmap.size()) +
                 "개 국면 적용(재평가 " + std::to_string(cfg.value("regime_reeval_sec", 300)) + "s)");
    }

    // 국면 판정기(RegimeController) 파라미터. config "regime_tuning":
    //  {"index_code":"0001","ma_long":200,"ma_mid":60,"ma_short":20,"ma_align3":120,
    //   "score_bull_threshold":2,"score_bear_threshold":-2,"fail_fallback_n":3}
    //  미지정이면 기본값 그대로(기존 동작 불변). 이 배선이 없던 동안 지수코드마저 하드코딩이라
    //  코스닥 지수로 판정할 수단이 없었고, BULL/BEAR 분기를 실데이터로 태울 방법도 없었다
    //  (docs/DEFERRED_ISSUES.md D-15).
    if (cfg.contains("regime_tuning"))
    {
        const auto& rt = cfg["regime_tuning"];
        const RegimeController::Config def;       // 기본값 스냅샷(오버라이드 판별·되돌림용)
        RegimeController::Config rc;              // 기본값에서 시작해 준 항목만 덮어쓴다
        rc.index_code      = rt.value("index_code",      rc.index_code);
        rc.ma_long         = rt.value("ma_long",         rc.ma_long);
        rc.ma_mid          = rt.value("ma_mid",          rc.ma_mid);
        rc.ma_short        = rt.value("ma_short",        rc.ma_short);
        rc.ma_align3       = rt.value("ma_align3",       rc.ma_align3);
        rc.fail_fallback_n = rt.value("fail_fallback_n", rc.fail_fallback_n);

        // 점수 임계값은 국면 판정을 통째로 뒤집는 스위치다(±2가 v0 2축의 만장일치 규칙).
        //  검증·드릴 목적으로만 열어두고, 실계좌에서는 무시하고 기본값으로 되돌린다.
        const int bull = rt.value("score_bull_threshold", def.score_bull_threshold);
        const int bear = rt.value("score_bear_threshold", def.score_bear_threshold);
        const bool overridden = (bull != def.score_bull_threshold) || (bear != def.score_bear_threshold);

        if (overridden && !kis_cfg.is_paper)
        {
            LOG_ERROR("[Main] regime_tuning 점수 임계값 오버라이드는 실계좌에서 무시한다 "
                      "(요청 bull=" + std::to_string(bull) + " bear=" + std::to_string(bear) +
                      " → 기본 " + std::to_string(def.score_bull_threshold) + "/" +
                      std::to_string(def.score_bear_threshold) + "). 모의계좌에서만 쓴다.");
        }
        else
        {
            rc.score_bull_threshold = bull;
            rc.score_bear_threshold = bear;

            if (overridden)
            {
                LOG_WARN("[Main] 국면 점수 임계값 오버라이드 — bull>=" + std::to_string(bull) +
                         ", bear<=" + std::to_string(bear) + " (기본 " +
                         std::to_string(def.score_bull_threshold) + "/" +
                         std::to_string(def.score_bear_threshold) +
                         "). 국면이 실제 시장과 다르게 판정되니 검증용으로만 둔다.");
            }
        }

        // classify()는 BULL을 먼저 보므로 bull<=bear면 NEUTRAL이 도달 불능이 된다.
        if (rc.score_bull_threshold <= rc.score_bear_threshold)
        {
            LOG_ERROR("[Main] regime_tuning: bull(" + std::to_string(rc.score_bull_threshold) +
                      ") <= bear(" + std::to_string(rc.score_bear_threshold) +
                      ") — NEUTRAL이 도달 불능이라 기본값으로 되돌린다.");
            rc.score_bull_threshold = def.score_bull_threshold;
            rc.score_bear_threshold = def.score_bear_threshold;
        }

        engine.set_regime_config(rc);
        LOG_INFO("[Main] 국면 판정 파라미터 — 지수=" + rc.index_code +
                 " ma(" + std::to_string(rc.ma_short) + "/" + std::to_string(rc.ma_mid) + "/" +
                 std::to_string(rc.ma_align3) + "/" + std::to_string(rc.ma_long) + ")" +
                 " 임계(bull>=" + std::to_string(rc.score_bull_threshold) +
                 ", bear<=" + std::to_string(rc.score_bear_threshold) + ")" +
                 " fail_fallback=" + std::to_string(rc.fail_fallback_n));
    }

    // 기동 스모크 테스트 — 서버 실행 직후 지정 종목을 시장가 1주 매수해, 주문 경로 전체가
    //  살아있는지 최소 점검한다. config "startup_probe": {"ticker":"005930","qty":1}.
    //  없으면 미가동(기존 동작 불변).
    if (cfg.contains("startup_probe"))
    {
        const auto& sp = cfg["startup_probe"];
        std::string sp_ticker = sp.value("ticker", std::string());
        int         sp_qty    = sp.value("qty", 0);
        engine.set_startup_probe(sp_ticker, sp_qty);

        if (!sp_ticker.empty() && sp_qty > 0)
        {
            LOG_INFO("[Main] 기동 점검 설정: " + sp_ticker + " 시장가 " +
                     std::to_string(sp_qty) + "주 (모의계좌 주문경로 검증)");
        }
    }

    // 시세 전용(실전 도메인) 키: 모의(openapivts)는 시세 REST가 HTTP 500이므로 시세만 실전으로 조회.
    // 스캔 유니버스 분기(universe_from_scan)도 이 실전 키로 거래대금 랭킹/지수를 조회하므로 바깥 스코프로 보관.
    KisConfig quote_kis_cfg;
    bool has_quote_kis = false;

    if (cfg.contains("quote_kis"))
    {
        KisConfig q;
        q.app_key      = cfg["quote_kis"]["app_key"];
        q.app_secret   = cfg["quote_kis"]["app_secret"];
        q.account_no   = cfg["quote_kis"].value("account_no", "");
        q.account_type = cfg["quote_kis"].value("account_type", "01");
        q.hts_id       = cfg["quote_kis"].value("hts_id", "");
        q.is_paper     = false; // 시세는 실전 도메인
        engine.set_quote_kis_config(q);
        quote_kis_cfg = q;
        has_quote_kis = true;
        LOG_INFO("[Main] 시세 전용 클라이언트(실전 도메인) 설정됨");
    }

    // 위험 한도(risk) + 주문 호출 간격 조절 — config로 노출(없으면 OrderGate 기본값·호출 간격 조절 기본값 유지).
    //  지정된 키만 기본값에서 덮어쓴다. 실제 돈 규율 튜닝을 재빌드 없이 하기 위함(S-1).
    if (cfg.contains("risk"))
    {
        const auto& r = cfg["risk"];
        OrderGate::Config rc; // OrderGate::Config 기본값에서 시작
        rc.max_qty_per_ticker     = r.value("max_qty_per_ticker", rc.max_qty_per_ticker);
        rc.daily_loss_limit       = r.value("daily_loss_limit", rc.daily_loss_limit);
        rc.max_orders_per_min     = r.value("max_orders_per_min", rc.max_orders_per_min);
        rc.max_orders_per_sec     = r.value("max_orders_per_sec", rc.max_orders_per_sec);
        rc.dedup_window_sec       = r.value("dedup_window_sec", rc.dedup_window_sec);
        rc.max_qty_per_order      = r.value("max_qty_per_order", rc.max_qty_per_order);
        rc.max_notional_per_order = r.value("max_notional_per_order", rc.max_notional_per_order);
        rc.max_notional_per_ticker  = r.value("max_notional_per_ticker", rc.max_notional_per_ticker);
        rc.max_concurrent_positions = r.value("max_concurrent_positions", rc.max_concurrent_positions);
        rc.max_gross_exposure_pct   = r.value("max_gross_exposure_pct", rc.max_gross_exposure_pct);
        // 슬롯 경합 시 점수 상위부터 채운다(선착순 금지). 스캐너가 랭크를 게이트에 주입한다.
        rc.entry_priority_enabled   = r.value("entry_priority_enabled", rc.entry_priority_enabled);
        // 슬롯이 꽉 찬 뒤에도 더 높은 점수가 오면 최약체를 비우고 자리를 넘긴다(교체 진입).
        rc.displace_enabled         = r.value("displace_enabled", rc.displace_enabled);
        rc.displace_min_z_gap       = r.value("displace_min_z_gap", rc.displace_min_z_gap);
        rc.displace_min_hold_sec    = r.value("displace_min_hold_sec", rc.displace_min_hold_sec);
        rc.displace_cooldown_sec    = r.value("displace_cooldown_sec", rc.displace_cooldown_sec);
        rc.displace_max_per_day     = r.value("displace_max_per_day", rc.displace_max_per_day);
        rc.displace_slot_hold_sec   = r.value("displace_slot_hold_sec", rc.displace_slot_hold_sec);
        // 오늘 스캔에 안 잡힌 이월 보유분에 매길 점수. 0이면 그런 보유분은 교체 후보에서 빠진다.
        rc.displace_unscored_z      = r.value("displace_unscored_z", rc.displace_unscored_z);
        engine.set_risk_config(rc);
        LOG_INFO("[Main] risk 한도: 종목당 " + std::to_string(rc.max_qty_per_ticker) + "주, 일손실 " +
                 std::to_string((long long)rc.daily_loss_limit) + "원, " +
                 std::to_string(rc.max_orders_per_sec) + "/s·" +
                 std::to_string(rc.max_orders_per_min) + "/min");

        if (rc.max_notional_per_ticker > 0.0 || rc.max_concurrent_positions > 0)
        {
            LOG_INFO("[Main] 사이징 백스톱: 종목당 명목 " +
                     std::to_string((long long)rc.max_notional_per_ticker) + "원, 동시보유 " +
                     std::to_string(rc.max_concurrent_positions) + "종목");
        }

        if (rc.max_gross_exposure_pct > 0.0)
        {
            LOG_INFO("[Main] 총노출 상한: 자본의 " +
                     std::to_string(rc.max_gross_exposure_pct) + " (잔고 대조 총평가금 기준)");
        }

        // 주문 호출 간격 조절(C-2/W-3) — 버스트 청산 EGW00201 회피 + 거부 SELL 재시도.
        int pace_ms = r.value("order_min_interval_ms", 350);
        int max_ret = r.value("order_max_retries", 3);
        engine.set_order_pacing(pace_ms, max_ret);
        LOG_INFO("[Main] 주문 호출 간격 조절: " + std::to_string(pace_ms) + "ms 간격, 청산 SELL 재시도 " +
                 std::to_string(max_ret) + "회");
    }

    // 전략 로딩 — 타입별 로더 디스패치 + active_regimes 후처리 (strategy/StrategyFactory.cpp)
    StrategyLoadCtx sctx{engine, kis_cfg, quote_kis_cfg, has_quote_kis};
    load_strategies(sctx, cfg["strategies"]);

    engine.start();

    while (engine.is_running())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG_INFO("[Main] 프로그램 종료");
    return 0;
}
