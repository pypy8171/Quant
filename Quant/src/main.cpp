#include "core/Engine.h"
#include "modes/Monitors.h"
#include "strategy/StrategyFactory.h"
#include "utils/Logger.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
// KisWebSocket.h가 windows.h를 끌어오던 때의 조건(LEAN_AND_MEAN·NOMINMAX·ERROR 해제)을 여기서 직접 맞춘다. [why D-049]
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#include <dbghelp.h>
#ifdef ERROR
#undef ERROR // wingdi.h — LogLevel::ERROR와 부딪힌다
#endif

namespace
{

// 프로세스 타이머 격자를 1ms로. 기본 15.6ms에서는 sleep_for(1ms)·(100us)가 실측 p50 15.6ms였고 1ms로 내리면 2ms다
//  (bench_sleep_res). 큐 소비자는 notify로 깨우지만(WakeGate) 발주 간격·재시도·데이터 폴링의 sleep은 이 격자를 탄다.
//  Windows 10 2004+에서는 이 프로세스에만 적용된다. 종료 시 되돌린다. [why D-071]
struct TimerResolution
{
    bool ok = false;

    TimerResolution()
    {
        ok = (timeBeginPeriod(1) == TIMERR_NOERROR);
    }

    ~TimerResolution()
    {
        if (ok)
        {
            timeEndPeriod(1);
        }
    }
};

} // namespace
#endif

using json = nlohmann::json;

// ─── 전역 종료 플래그 ─────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static Engine* g_engine = nullptr;

// 시그널 스레드는 정지 요청만 한다. 예전엔 여기서 stop()(join 전부)을 돌렸는데, running_이 내려가자마자 main이
//  루프를 빠져 Engine을 부수기 시작해 두 스레드가 같은 jthread를 join했다(09-11 15:32 `프로그램 종료` 0.03초 뒤
//  std::terminate, 사유 없음). join은 main 스레드의 stop() 한 곳만.
void signal_handler(int)
{
    g_running.store(false);

    if (g_engine)
    {
        g_engine->request_shutdown();
    }
}

// ─── 조용한 죽음 방지 ─────────────────────────────────────────────────────
//  Logger는 비동기라 abort()·미처리 예외로 죽으면 마지막 줄들이 파일에 닿지 못한다.
//  09-11 08:53·09:14 두 번, 로그 한 줄·종료코드·덤프 없이 프로세스가 사라졌다.
//  종료 직전에 사유 한 줄을 남기고 flush한 뒤에 죽는다 — 원인 분석은 그 다음 일이다.
static void log_and_die(const std::string& why)
{
    LOG_ERROR("[Main] 비정상 종료: " + why);
    Logger::instance().flush();
    std::_Exit(3);
}

#ifdef _WIN32
// 로그 옆에 미니덤프를 남기고 사유에 붙일 꼬리를 돌려준다. SEH 와 std::terminate 둘 다 쓴다 — terminate 는
//  예외 정보가 없어도 부른 스레드의 스택이 덤프에 남는다(09-11 terminate 는 사유 한 줄뿐이라 위치를 못 찾았다).
//  덤프는 스택·스레드·모듈만(MiniDumpWithIndirectlyReferencedMemory) — 힙 전체는 수백 MB라 뺀다.
static std::string write_minidump(const char* tag, EXCEPTION_POINTERS* ep)
{
    try
    {
        const auto dir  = Logger::default_base_dir();
        const auto path = dir / (std::string(tag) + "_" + std::to_string(GetCurrentProcessId()) + ".dmp");
        HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

        if (h == INVALID_HANDLE_VALUE)
        {
            return " dump 실패 err=" + std::to_string(GetLastError());
        }

        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                                     MiniDumpWithThreadInfo),
                                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(h);
        return ok ? " dump=" + path.string() : " dump 실패 err=" + std::to_string(GetLastError());
    }
    catch (...)
    {
        return " dump 실패(예외)";
    }
}
#endif

static void on_terminate()
{
    std::string why = "std::terminate tid=" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    try
    {
        if (auto ep = std::current_exception())
        {
            std::rethrow_exception(ep);
        }
    }
    catch (const std::exception& e)
    {
        why += " — " + std::string(e.what());
    }
    catch (...)
    {
        why += " — 비표준 예외";
    }

#ifdef _WIN32
    why += write_minidump("terminate", nullptr);
#endif
    log_and_die(why);
}

#ifdef _WIN32
// 코드 한 줄("SEH 0xC0000005")만으로는 어느 스레드의 어느 명령인지 알 수 없다(09-14 09:46 실측 —
//  재스캔 직후 접근 위반, 위치 불명). 사유에 주소·스레드를 붙이고 로그 옆에 미니덤프를 남긴다.
static LONG WINAPI on_seh(EXCEPTION_POINTERS* ep)
{
    const auto* rec = ep ? ep->ExceptionRecord : nullptr;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "SEH 0x%08lX addr=%p tid=%lu",
                  rec ? rec->ExceptionCode : 0UL, rec ? rec->ExceptionAddress : nullptr,
                  GetCurrentThreadId());
    std::string why = buf;
    why += write_minidump("crash", ep);
    log_and_die(why);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

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
    TimerResolution timer_res;
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

    // 로그 임계값: 기본 INFO. "DEBUG"는 봉 닫힘(D-069)·KIS 응답 본문 같은 줄을 연다 — 비교표 뽑는 날만 켠다.
    if (cfg.value("log_level", std::string("INFO")) == "DEBUG")
    {
        Logger::instance().set_min_level(LogLevel::DEBUG);
        LOG_INFO("[Main] 로그 임계값 DEBUG (config log_level)");
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
    std::signal(SIGABRT, [](int) { log_and_die("SIGABRT"); });
    std::set_terminate(on_terminate);
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_seh);
#endif

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
    // WS 틱·호가 raw 캡처 폴더(D-071 원칙 8). 비우면 끈다. 리플레이 백테스트의 입력.
    engine.set_capture_dir(cfg.value("capture_dir", std::string()));
    // 전략 샤드 수(D-071 원칙 2). 종목 해시로 열을 나눠 전략 계산을 샤드 스레드 M개가 나눈다. 기본 1.
    engine.set_strategy_shards(cfg.value("strategy_shards", 1u));

    // 추가 WS 세션 키(D-071 원칙 1). 기본 kis 키와 함께 소켓을 여럿 열어 구독 상한을 소켓 수만큼 늘린다.
    //  계좌·모의 여부는 기본 키와 같고 app_key·app_secret만 다르다. 체결통보(hts_id)는 기본 키만 받는다.
    for (const auto& k : cfg.value("feed_keys", json::array()))
    {
        KisConfig c   = kis_cfg;
        c.app_key     = k.at("app_key").get<std::string>();
        c.app_secret  = k.at("app_secret").get<std::string>();
        c.hts_id.clear();
        engine.add_feed_config(c);
    }

    // 캡처 파일 리플레이(D-071 원칙 8). WS 대신 파일을 틀어 같은 파이프라인을 돌린다. Engine이 KisClient를 만들지 않으므로
    //  app_key·계좌가 없어도 되고 실주문 경로도 없다 — 종목은 config tickers, 주문은 모의 체결기.
    {
        const std::string replay_file = cfg.value("replay_file", std::string());

        if (!replay_file.empty())
        {
            engine.set_replay(replay_file, cfg.value("replay_speed", 1.0), cfg.value("replay_cash", 100'000'000.0));
        }
    }

    // 매크로 레짐 보조 프로세스 브리지(2026-08-09 회의): Python macro_regime_feed.py가 쓰는 regime.json
    //  경로. 지정 시 data_thread가 매 사이클 읽어, 시장이 위험하면 OrderGate 의 신규매수 정지 스위치
    //  (entry_halt)를 켜고 풀리면 끈다. 빈 문자열(기본)이면 미가동 — 기존 동작 불변.
    engine.set_regime_file(cfg.value("regime_file", std::string()),
                           cfg.value("regime_stale_sec", kDefaultRegimeStaleSec));
    engine.set_regime_halt_expire_min(
        cfg.value("regime_halt_expire_min", kDefaultRegimeHaltExpireMin));
    // ZMQ 제어 채널(config "zmq_bind_addr"·"zmq_control_token"). 주소를 안 주면 127.0.0.1에
    //  묶이고, 토큰이 비면 KILL 명령은 거부된다. 스레드 시작 전에만 유효하다.
    engine.set_zmq_control(cfg.value("zmq_bind_addr", std::string()),
                           cfg.value("zmq_control_token", std::string()));
    // 운영단말 TCP 채널(config "ops_bind_addr"·"ops_port"·"ops_token"). port를 안 주면 열지 않는다.
    //  주소 기본 127.0.0.1, 루프백 밖으로 열 때는 토큰이 있어야 서버가 뜬다.
    engine.set_ops_control(cfg.value("ops_bind_addr", std::string()), cfg.value("ops_port", 0),
                           cfg.value("ops_token", std::string()));

    // G1: 국면→전략 자동선택 맵. config "regime_strategies": {"BULL":[id...], "NEUTRAL":[...], "BEAR":[...]}.
    //  id 항목이 '*'로 끝나면 접두 매칭(스캐너 동적 id: "DevScale_*"). 미지정이면 기존
    //  per-strategy active_regimes 방식 유지(하위호환). 지정 시 국면이 전략셋을 선택한다.
    if (cfg.contains("regime_strategies"))
    {
        // 키는 regime.json 라벨(RISK_ON·NEUTRAL·RISK_OFF)이고, 09-14까지 쓰던 BULL·BEAR도 같은 뜻으로 받는다. [why D-084]
        auto to_regime = [](const std::string& k) -> Regime
        {
            if (k == "BULL" || k == "RISK_ON")
            {
                return Regime::BULL;
            }

            if (k == "BEAR" || k == "RISK_OFF")
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

        // 선택 입력은 regime.json 라벨이라 파일이 없으면 맵이 한 번도 적용되지 않는다(전 전략 기본 활성).
        if (cfg.value("regime_file", std::string()).empty())
        {
            LOG_WARN("[Main] regime_strategies가 있는데 regime_file이 비어 있다 — 국면별 전략 선택이 동작하지 않는다");
        }

        engine.set_regime_strategies(rmap);
        LOG_INFO("[Main] 국면→전략 자동선택 맵 " + std::to_string(rmap.size()) + "개 국면 적용");
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

    // join 은 여기 한 곳 — 시그널·KILL 핸들러는 request_shutdown() 만 한다(위 signal_handler 주석).
    engine.stop();
    g_engine = nullptr;
    LOG_INFO("[Main] 프로그램 종료");
    return 0;
}
