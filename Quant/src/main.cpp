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
        g_engine->stop();
}

// ─── 설정 파일 로드 ───────────────────────────────────────────────────────
static json load_config(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("설정 파일 없음: " + path);
    return json::parse(f);
}

// ─── 실행파일이 놓인 디렉터리 (cwd와 무관) ──────────────────────────────────
//  로그·산출물을 실행 위치가 아니라 바이너리 기준으로 모으기 위한 앵커.
//  Windows 한글 경로(예: C:\Users\...\) 대비 wide 버퍼로 받아 path로만 다룬다.
static std::filesystem::path executable_dir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return std::filesystem::current_path();
    return p.parent_path();
#endif
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
    // 로그·산출물을 실행 위치(cwd)와 무관하게 한 폴더로 못박는다.
    //  기본: 실행파일 옆 logs/  ·  QUANT_LOG_DIR 환경변수가 있으면 그 절대경로 우선.
    //  (같은 바이너리를 어느 폴더에서 띄워도 trades/pnl_baseline/log가 흩어지지 않음)
    std::filesystem::path log_base;
    if (const char* env = std::getenv("QUANT_LOG_DIR"); env && *env)
        log_base = std::filesystem::path(env);
    else
        log_base = executable_dir() / "logs";
    Logger::instance().set_base_dir(log_base);
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
            mode_override = input_mode;
        else
            config_path = input_mode;
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
        tickers = cfg["tickers"].get<std::vector<std::string>>();

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string mode = cfg.value("mode", "FEED");

    // ═══════════════════════════════════════════════════════════════════════
    //  관찰용 모니터 모드 — 각자 자기 루프를 돌다 종료 (modes/Monitors.cpp)
    // ═══════════════════════════════════════════════════════════════════════
    if (mode == "FEED")
        return run_feed(kis_cfg, tickers, g_running);
    if (mode == "KR_TEST")
        return run_kr_test(kis_cfg, g_running);
    if (mode == "US_TEST")
        return run_us_test(kis_cfg, g_running);

    // ═══════════════════════════════════════════════════════════════════════
    //  TRADE 모드: 전략 매매 엔진 (tickers 설정 불필요 — 전략이 동적으로 구성)
    // ═══════════════════════════════════════════════════════════════════════
    int interval = cfg.value("fetch_interval_sec", 60);
    Engine engine(kis_cfg, interval);
    g_engine = &engine;
    // G5: 기동 시 실계좌 보유분을 OrderGate 원장에 시드(ITB 매도수량·평단·손실한도 정합).
    engine.set_bootstrap_ledger(cfg.value("bootstrap_ledger_from_balance", false));
    // REST 현재가 폴링을 체결 피드로 사용(WS 실시간 세션 rt=9 폭주 우회). ITB가 이 틱으로 구동.
    engine.set_rest_price_feed(cfg.value("rest_price_feed", false));
    // 매크로 레짐 사이드카 브리지(2026-08-09 회의): Python macro_regime_feed.py가 쓰는
    //  regime.json 경로. 지정 시 data_thread가 매 사이클 읽어 OrderGate entry_halt를 토글한다.
    //  빈 문자열(기본)이면 미가동 — 기존 동작 불변.
    engine.set_regime_file(cfg.value("regime_file", std::string()),
                           cfg.value("regime_stale_sec", 600));
    // 기동 스모크 프로브 — 서버 실행 시 지정 종목 시장가 1주 매수로 모의계좌 주문경로 검증.
    //  config "startup_probe": {"ticker":"005930","qty":1}. 없으면 미가동(기존 동작 불변).
    if (cfg.contains("startup_probe"))
    {
        const auto& sp = cfg["startup_probe"];
        std::string sp_ticker = sp.value("ticker", std::string());
        int         sp_qty    = sp.value("qty", 0);
        engine.set_startup_probe(sp_ticker, sp_qty);
        if (!sp_ticker.empty() && sp_qty > 0)
            LOG_INFO("[Main] 기동 스모크 프로브 설정: " + sp_ticker + " 시장가 " +
                     std::to_string(sp_qty) + "주 (모의계좌 주문경로 검증)");
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

    // 위험 한도(risk) + 주문 페이싱 — config로 노출(없으면 OrderGate 기본값·페이싱 기본값 유지).
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
        engine.set_risk_config(rc);
        LOG_INFO("[Main] risk 한도: 종목당 " + std::to_string(rc.max_qty_per_ticker) + "주, 일손실 " +
                 std::to_string((long long)rc.daily_loss_limit) + "원, " +
                 std::to_string(rc.max_orders_per_sec) + "/s·" +
                 std::to_string(rc.max_orders_per_min) + "/min");
        if (rc.max_notional_per_ticker > 0.0 || rc.max_concurrent_positions > 0)
            LOG_INFO("[Main] 사이징 백스톱: 종목당 명목 " +
                     std::to_string((long long)rc.max_notional_per_ticker) + "원, 동시보유 " +
                     std::to_string(rc.max_concurrent_positions) + "종목");

        // 주문 페이싱(C-2/W-3) — 버스트 청산 EGW00201 회피 + 거부 SELL 재시도.
        int pace_ms = r.value("order_min_interval_ms", 350);
        int max_ret = r.value("order_max_retries", 3);
        engine.set_order_pacing(pace_ms, max_ret);
        LOG_INFO("[Main] 주문 페이싱: " + std::to_string(pace_ms) + "ms 간격, 청산 SELL 재시도 " +
                 std::to_string(max_ret) + "회");
    }

    // 전략 로딩 — 타입별 로더 디스패치 + active_regimes 후처리 (strategy/StrategyFactory.cpp)
    StrategyLoadCtx sctx{engine, kis_cfg, quote_kis_cfg, has_quote_kis};
    load_strategies(sctx, cfg["strategies"]);

    engine.start();
    while (engine.is_running())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    LOG_INFO("[Main] 프로그램 종료");
    return 0;
}
