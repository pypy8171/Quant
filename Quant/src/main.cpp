#include "core/Engine.h"
#include "api/KisWebSocket.h"
#include "strategy/MACrossStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "utils/Logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <memory>
#include <fstream>
#include <map>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

// ─── 전역 종료 플래그 ─────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static Engine*           g_engine = nullptr;

void signal_handler(int) {
    g_running.store(false);
    if (g_engine) g_engine->stop();
}

// ─── 설정 파일 로드 ───────────────────────────────────────────────────────
static json load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("설정 파일 없음: " + path);
    return json::parse(f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FEED 모드 — 시세 표시 유틸
// ═══════════════════════════════════════════════════════════════════════════

static const std::map<std::string, std::string> TICKER_NAMES = {
    {"005380", "현대차  "},
    {"005930", "삼성전자"},
    {"000660", "SK하이닉"},
    {"402340", "SK스퀘어"},
    {"006400", "삼성SDI "},
    {"009150", "삼성전기"}
};

static std::string fmt_price(double v) {
    if (v <= 0.0) return "       -";
    long long iv = static_cast<long long>(v);
    std::string s = std::to_string(iv);
    std::string r;
    int cnt = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) {
        if (cnt > 0 && cnt % 3 == 0) r = "," + r;
        r = s[i] + r;
        ++cnt;
    }

    // 오른쪽 정렬 8자리
    while (static_cast<int>(r.size()) < 8) r = " " + r;
    return r;
}

static std::string fmt_qty(int64_t v) {
    if (v <= 0) return "      -";
    std::string s = std::to_string(v);
    std::string r;
    int cnt = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) {
        if (cnt > 0 && cnt % 3 == 0) r = "," + r;
        r = s[i] + r;
        ++cnt;
    }
    while (static_cast<int>(r.size()) < 7) r = " " + r;
    return r;
}

static std::string fmt_time_hms(const std::string& t) {
    if (t.size() < 6) return "--:--:--";
    return t.substr(0, 2) + ":" + t.substr(2, 2) + ":" + t.substr(4, 2);
}

static std::string dir_str(int d) {
    if (d == 1) return "\xE2\x96\xB2";  // UTF-8 ▲
    if (d == 5) return "\xE2\x96\xBC";  // UTF-8 ▼
    return "-";
}

// ─── 1초마다 콘솔에 시세 표시 ────────────────────────────────────────────
static void print_feed(const std::vector<std::string>& tickers,
                       std::mutex& mtx,
                       const std::map<std::string, OrderBook>&  ob_cache,
                       const std::map<std::string, TradeData>&  td_cache) {
    // 커서를 맨 위로 이동 (깜빡임 없이 덮어쓰기)
    std::cout << "\033[H";

    // 현재 시각
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
#ifdef _WIN32
    struct tm tm_info{};
    localtime_s(&tm_info, &tt);
#else
    struct tm tm_info{};
    localtime_r(&tt, &tm_info);
#endif
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);

    std::cout << "══════════════════════════ 실시간 시세 ["
              << tbuf << "] ══════════════════════════\n\n";

    std::lock_guard<std::mutex> lk(mtx);

    for (const auto& ticker : tickers) {
        auto name_it = TICKER_NAMES.find(ticker);
        const std::string& name = (name_it != TICKER_NAMES.end())
                                      ? name_it->second : ticker;

        auto td_it = td_cache.find(ticker);
        auto ob_it = ob_cache.find(ticker);

        double      trade_px  = 0.0;
        int         trade_dir = 0;
        std::string trade_t   = "--:--:--";

        if (td_it != td_cache.end()) {
            trade_px  = td_it->second.price;
            trade_dir = td_it->second.direction;
            trade_t   = fmt_time_hms(td_it->second.time);
        }

        // 종목 헤더
        std::cout << "  [" << name << " " << ticker << "]"
                  << "  체결: " << fmt_price(trade_px) << "원 "
                  << dir_str(trade_dir)
                  << "  (" << trade_t << ")\n";

        // 호가 테이블 헤더
        std::cout << "    매도호가         잔량    │    매수호가         잔량\n";

        if (ob_it != ob_cache.end()) {
            const auto& ob = ob_it->second;
            // 매도5↔매수1, 매도4↔매수2, ..., 매도1↔매수5
            for (int i = 4; i >= 0; --i) {
                // 매도: asks[i] (i=4이 가장 멀리, i=0이 최우선)
                // 매수: bids[4-i] (최우선매수가 위, 멀수록 아래)
                int j = 4 - i;
                std::cout
                    << "    매도" << (i + 1) << ": "
                    << fmt_price(ob.asks[i].price)
                    << " (" << fmt_qty(ob.asks[i].quantity) << ")"
                    << "  │  "
                    << "매수" << (j + 1) << ": "
                    << fmt_price(ob.bids[j].price)
                    << " (" << fmt_qty(ob.bids[j].quantity) << ")"
                    << "\n";
            }
        } else {
            for (int i = 0; i < 5; ++i)
                std::cout << "    (데이터 수신 대기...)                     \n";
        }
        std::cout << "\n";
    }
    std::cout.flush();
}

// ═══════════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    Logger::instance().init("quant_trader.log", LogLevel::INFO);
    LOG_INFO("=== Quant Trader v2.0 ===");

    std::string config_path = (argc > 1) ? argv[1] : "config/config.json";
    json cfg;
    try {
        cfg = load_config(config_path);
        LOG_INFO("[Main] 설정 로드: " + config_path);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[Main] 설정 로드 실패: ") + e.what());
        return 1;
    }

    // KIS 설정
    KisConfig kis_cfg;
    kis_cfg.app_key      = cfg["kis"]["app_key"];
    kis_cfg.app_secret   = cfg["kis"]["app_secret"];
    kis_cfg.account_no   = cfg["kis"]["account_no"];
    kis_cfg.account_type = cfg["kis"]["account_type"].get<std::string>();
    kis_cfg.is_paper     = cfg["kis"]["is_paper"].get<bool>();

    std::vector<std::string> tickers =
        cfg["tickers"].get<std::vector<std::string>>();

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string mode = cfg.value("mode", "FEED");

    // ═══════════════════════════════════════════════════════════════════════
    //  FEED 모드: WebSocket 실시간 호가/체결 표시
    // ═══════════════════════════════════════════════════════════════════════
    if (mode == "FEED") {
#ifdef _WIN32
        // Windows 콘솔 UTF-8 + ANSI 이스케이프 활성화
        SetConsoleOutputCP(CP_UTF8);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD  dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif

        std::mutex                          cache_mtx;
        std::map<std::string, OrderBook>    ob_cache;
        std::map<std::string, TradeData>    td_cache;

        KisWebSocket ws(kis_cfg);
        ws.set_callbacks(
            [&](const OrderBook& ob) {
                std::lock_guard<std::mutex> lk(cache_mtx);
                ob_cache[ob.ticker] = ob;
            },
            [&](const TradeData& td) {
                std::lock_guard<std::mutex> lk(cache_mtx);
                td_cache[td.ticker] = td;
            }
        );

        LOG_INFO("[Main] FEED 모드 — WebSocket 연결 시도");
        if (!ws.connect(tickers)) {
            LOG_ERROR("[Main] WebSocket 연결 실패");
            return 1;
        }

        // 화면 초기화
        std::cout << "\033[2J";

        LOG_INFO("[Main] 시세 수신 시작 (Ctrl+C 로 종료)");

        // 1초 주기 표시 루프
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!ws.is_connected()) {
                LOG_WARN("[Main] WebSocket 연결 끊김");
                break;
            }

            print_feed(tickers, cache_mtx, ob_cache, td_cache);
        }

        ws.disconnect();
        LOG_INFO("[Main] FEED 종료");
        return 0;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  TRADE 모드: 기존 전략 매매 엔진
    // ═══════════════════════════════════════════════════════════════════════
    int interval = cfg.value("fetch_interval_sec", 60);
    Engine engine(kis_cfg, tickers, interval);
    g_engine = &engine;

    for (auto& s : cfg["strategies"]) {
        std::string type   = s["type"];
        std::string ticker = s["ticker"];
        int         qty    = s["quantity"];

        if (type == "MA_CROSS") {
            engine.add_strategy(std::make_unique<MACrossStrategy>(
                ticker, s["short_period"].get<int>(),
                s["long_period"].get<int>(), qty));
        } else if (type == "MOMENTUM") {
            engine.add_strategy(std::make_unique<MomentumStrategy>(
                ticker, s["period"].get<int>(), qty));
        } else {
            LOG_WARN("[Main] 알 수 없는 전략: " + type);
        }
    }

    engine.start();
    while (engine.is_running())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    LOG_INFO("[Main] 프로그램 종료");
    return 0;
}
