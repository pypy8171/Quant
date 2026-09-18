#include "core/AppConfig.h"
#include "core/Engine.h"
#include "core/Types.h"
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
#include <string>
#include <thread>
#include <vector>

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
static std::string write_minidump(const char* tag, EXCEPTION_POINTERS* exception_pointers)
{
    try
    {
        const auto directory  = Logger::default_base_directory();
        const auto path = directory / (std::string(tag) + "_" + std::to_string(GetCurrentProcessId()) + ".dmp");
        HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

        if (handle == INVALID_HANDLE_VALUE)
        {
            return " dump 실패 err=" + std::to_string(GetLastError());
        }

        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), exception_pointers, FALSE};
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), handle,
                                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                                     MiniDumpWithThreadInfo),
                                          exception_pointers ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(handle);
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
        if (auto exception_pointers = std::current_exception())
        {
            std::rethrow_exception(exception_pointers);
        }
    }
    catch (const std::exception& exception)
    {
        why += " — " + std::string(exception.what());
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
static LONG WINAPI on_seh(EXCEPTION_POINTERS* exception_pointers)
{
    const auto* record = exception_pointers ? exception_pointers->ExceptionRecord : nullptr;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "SEH 0x%08lX addr=%p tid=%lu",
                  record ? record->ExceptionCode : 0UL, record ? record->ExceptionAddress : nullptr,
                  GetCurrentThreadId());
    std::string why = buffer;
    why += write_minidump("crash", exception_pointers);
    log_and_die(why);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  초기화 단계 — main()이 부르는 순서가 곧 초기화 순서다. 새 초기화는 여기 함수 하나로 만들고 main() 목록에
//  한 줄을 더한다(정본 docs/guides/MAINTENANCE_AUTOMATION.md 4절 "초기화 위치"). json은 core/AppConfig.cpp만 읽는다.
// ═══════════════════════════════════════════════════════════════════════════

struct CommandLine
{
    std::string config_path = "config/config.json";
    std::string mode_override; // 비어 있으면 config의 "mode"
};

// quant_trader [config] [MODE]
//   quant_trader.exe                  → config/config.json, mode from json
//   quant_trader.exe KR_TEST          → config/config.json, mode=KR_TEST
//   quant_trader.exe US_TEST          → config/config.json, mode=US_TEST
//   quant_trader.exe config.json TRADE → 지정 config, mode=TRADE
static CommandLine parse_command_line(int argc, char* argv[])
{
    CommandLine command_line;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];

        if (argument == "KR_TEST" || argument == "US_TEST" || argument == "FEED" || argument == "TRADE")
        {
            command_line.mode_override = argument;
        }
        else
        {
            command_line.config_path = argument;
        }
    }

    return command_line;
}

#ifdef _WIN32
// 콘솔 UTF-8 + ANSI 이스케이프(색). 로그 파일과 무관하게 화면 출력만 바꾼다.
static void setup_console()
{
    SetConsoleOutputCP(CP_UTF8);
    HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  console_mode  = 0;
    GetConsoleMode(output_handle, &console_mode);
    SetConsoleMode(output_handle, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

// 시그널·terminate·SEH — 셋 다 정지 요청 또는 사유 한 줄 + 덤프만 하고 join은 main의 stop() 한 곳(signal_handler 주석).
static void install_crash_handlers()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGABRT, [](int) { log_and_die("SIGABRT"); });
    std::set_terminate(on_terminate);
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_seh);
#endif
}

// 로그 임계값: 기본 INFO. "DEBUG"는 봉 닫힘(D-069)·KIS 응답 본문 같은 줄을 연다 — 비교표 뽑는 날만 켠다.
static void apply_log_level(const AppConfig& app)
{
    if (app.debug_log)
    {
        Logger::instance().set_min_level(LogLevel::DEBUG);
        LOG_INFO("[Main] 로그 임계값 DEBUG (config log_level)");
    }
}

static void log_exchange_choice(const KisConfig& kis_config)
{
    LOG_INFO("[Main] 거래소 구분 " + kis_config.exchange + " — 주문 EXCG_ID_DVSN_CD=" + kis_order_exchange(kis_config) +
             ", 실시간 채널 " + (kis_unified_feed(kis_config) ? "KRX+NXT 통합(H0UN*)" : "KRX(H0ST*)") +
             (kis_config.is_paper && kis_config.exchange != "KRX" ? " (모의투자는 KRX만 받아 KRX로 내린다)" : ""));
}

// TRADE 모드: 전략 매매 엔진 (tickers 설정 불필요 — 전략이 동적으로 구성). configure() → 전략 로딩 → start() 순서이고,
//  configure() 안의 배선은 core/EngineConfigure.cpp, start() 안의 순서(샤드·인증·주문 라우터·원장·전략·피드·스레드)는
//  Engine::start()가 정본이다.
static int run_trade(const AppConfig& app)
{
    Engine engine(app.kis, app.fetch_interval_sec);
    g_engine = &engine;
    engine.configure(app);

    // 전략 로딩 — 타입별 로더 디스패치 + active_regimes 후처리 (strategy/StrategyFactory.cpp)
    const KisConfig  no_quote_kis;
    StrategyLoadCtx  load_context{engine, app.kis, app.quote_kis ? *app.quote_kis : no_quote_kis,
                                  app.quote_kis.has_value()};
    load_strategies(load_context, app.strategies);
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

// ═══════════════════════════════════════════════════════════════════════════
//  main — 아래 호출 순서가 초기화 순서 문서다. 각 단계는 위 함수 하나에 대응한다.
//   1 타이머 격자 · 2 콘솔 · 3 로거 · 4 인자 · 5 설정(json→AppConfig, 검증 포함) · 6 로그 임계값 ·
//   7 크래시 핸들러 · 8 모드 분기(관찰 모드는 modes/Monitors.cpp, TRADE는 run_trade)
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[])
{
#ifdef _WIN32
    TimerResolution timer_resolution; // 1. 1ms 격자, 소멸자에서 되돌린다
    setup_console();                  // 2.
#endif
    // 3. 로그·산출물 기준 폴더는 Logger::default_base_directory()(QUANT_LOG_DIR > 실행파일 옆 logs/).
    Logger::instance().set_base_directory(Logger::default_base_directory());
    Logger::instance().initialize(Logger::instance().path_for("quant_trader.log"), LogLevel::INFO);
    LOG_INFO("=== Quant Trader v2.0 ===");
    const CommandLine command_line = parse_command_line(argc, argv); // 4.
    AppConfig app;

    try // 5. 설정 — 키 누락·값 오류는 여기서 멈춘다. 네트워크는 아직 안 건드렸다
    {
        app = parse_config(load_config_file(command_line.config_path), command_line.mode_override);
        LOG_INFO("[Main] 설정 로드: " + command_line.config_path);
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR(std::string("[Main] 설정 로드 실패: ") + exception.what());
        return 1;
    }

    if (!command_line.mode_override.empty())
    {
        LOG_INFO("[Main] 모드 오버라이드: " + command_line.mode_override);
    }

    apply_log_level(app);        // 6.
    log_exchange_choice(app.kis);
    install_crash_handlers();    // 7.
    const Mode mode = Mode::from_string(app.mode); // 8.

    if (mode == Mode::FEED)
    {
        return run_feed(app.kis, app.tickers, app.futures, g_running);
    }

    if (mode == Mode::KR_TEST)
    {
        return run_kr_test(app.kis, g_running);
    }

    if (mode == Mode::US_TEST)
    {
        return run_us_test(app.kis, g_running);
    }

    return run_trade(app);
}
