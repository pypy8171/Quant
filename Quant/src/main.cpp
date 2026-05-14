#include "core/Engine.h"
#include "api/KisWebSocket.h"
#include "strategy/MACrossStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "strategy/ValueContraryStrategy.h"
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
#include <thread>
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
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    Logger::instance().init("quant_trader.log", LogLevel::INFO);
    LOG_INFO("=== Quant Trader v2.0 ===");

    // 인자 파싱: quant_trader [config] [MODE]
    //   quant_trader.exe                  → config/config.json, mode from json
    //   quant_trader.exe KR_TEST          → config/config.json, mode=KR_TEST
    //   quant_trader.exe US_TEST          → config/config.json, mode=US_TEST
    //   quant_trader.exe config.json TRADE → 지정 config, mode=TRADE
    std::string config_path  = "config/config.json";
    std::string mode_override = "";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "KR_TEST" || a == "US_TEST" || a == "FEED" || a == "TRADE")
            mode_override = a;
        else
            config_path = a;
    }

    json cfg;
    try {
        cfg = load_config(config_path);
        LOG_INFO("[Main] 설정 로드: " + config_path);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[Main] 설정 로드 실패: ") + e.what());
        return 1;
    }

    if (!mode_override.empty()) {
        cfg["mode"] = mode_override;
        LOG_INFO("[Main] 모드 오버라이드: " + mode_override);
    }

    // KIS 설정
    KisConfig kis_cfg;
    kis_cfg.app_key      = cfg["kis"]["app_key"];
    kis_cfg.app_secret   = cfg["kis"]["app_secret"];
    kis_cfg.account_no   = cfg["kis"]["account_no"];
    kis_cfg.account_type = cfg["kis"]["account_type"].get<std::string>();
    kis_cfg.is_paper     = cfg["kis"]["is_paper"].get<bool>();

    // FEED 모드 전용 — TRADE 모드는 전략이 동적으로 종목 구성
    std::vector<std::string> tickers;
    if (cfg.contains("tickers"))
        tickers = cfg["tickers"].get<std::vector<std::string>>();

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
        Logger::instance().set_console_enabled(false);

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

        std::vector<WatchSpec> specs;
        for (const auto& t : tickers)
            specs.push_back({t, Market::KR, ""});

        LOG_INFO("[Main] FEED 모드 — WebSocket 연결 시도");
        if (!ws.connect(specs)) {
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
    //  KR_TEST 모드
    //   [REST 1회] 인증 + PBR·PER·전일기준가 초기 로드 (종목당 300ms)
    //   [WS 실시간] H0STCNT0 체결 구독 → 가격 캐시 업데이트
    //   [화면] 1초 주기로 캐시 출력
    // ═══════════════════════════════════════════════════════════════════════
    if (mode == "KR_TEST") {
        // UTF-8 한글 포함 문자열을 terminal 표시폭 기준으로 패딩
        auto utf8_display_width = [](const std::string& s) -> int {
            int w = 0;
            size_t i = 0;
            while (i < s.size()) {
                unsigned char c = s[i];
                if      (c < 0x80) { i += 1; w += 1; }
                else if (c < 0xE0) { i += 2; w += 2; }
                else if (c < 0xF0) { i += 3; w += 2; }  // CJK 2칸
                else               { i += 4; w += 2; }
            }
            return w;
        };
        auto utf8_pad_right = [&](const std::string& s, int target) -> std::string {
            int w = utf8_display_width(s);
            return (w >= target) ? s : s + std::string(target - w, ' ');
        };
        // 표시폭 기준으로 최대 max_w칸에 맞게 truncate
        auto utf8_trunc = [](const std::string& s, int max_w) -> std::string {
            int w = 0; size_t i = 0;
            while (i < s.size()) {
                unsigned char c = s[i];
                int cw, cb;
                if      (c < 0x80) { cw = 1; cb = 1; }
                else if (c < 0xE0) { cw = 2; cb = 2; }
                else if (c < 0xF0) { cw = 2; cb = 3; }
                else               { cw = 2; cb = 4; }
                if (w + cw > max_w) break;
                w += cw; i += cb;
            }
            return s.substr(0, i);
        };

        KisClient kis(kis_cfg);
        if (!kis.authenticate()) {
            LOG_ERROR("[KR_TEST] KIS 인증 실패");
            return 1;
        }

        struct StockPrice {
            std::string ticker, name;
            double price      = 0;
            double base_price = 0;
            double change     = 0;
            double change_rate= 0;
            double pbr        = 0;
            double per        = 0;
            int    direction  = 0;   // 1=상승 5=하락 (WS 갱신)
            std::string updated;
        };

        // KOSPI 시가총액 상위 20 고정 목록 (KIS 랭킹 API가 거래량 기준이라 직접 정의)
        // 시가총액 기준 순서 — 분기 단위로 검토 필요
        static const std::vector<std::pair<std::string,std::string>> KR_TOP20 = {
            {"005930","삼성전자  "},
            {"000660","SK하이닉스"},
            {"207940","삼성바이오"},
            {"005490","POSCO홀딩 "},
            {"005380","현대차    "},
            {"000270","기아      "},
            {"105560","KB금융    "},
            {"055550","신한지주  "},
            {"035420","NAVER     "},
            {"068270","셀트리온  "},
            {"051910","LG화학    "},
            {"066570","LG전자    "},
            {"012330","현대모비스"},
            {"035720","카카오    "},
            {"003550","LG        "},
            {"086790","하나금융  "},
            {"017670","SK텔레콤  "},
            {"009150","삼성전기  "},
            {"402340","SK스퀘어  "},
            {"316140","우리금융  "},
        };

        std::mutex                        cache_mtx;
        std::map<std::string, StockPrice> cache;
        std::vector<std::string>          display_order;

        // ── [REST 1회] 20종목 fundamentals 로드 ─────────────────────────────
        std::cout << "\033[2J\033[H";
        std::cout << "KOSPI 시가총액 상위 20종목 로딩 중 (REST)...\n";
        std::cout.flush();

        for (const auto& [code, name] : KR_TOP20) {
            auto f = kis.get_fundamentals(code);
            StockPrice sp;
            sp.ticker      = code;
            sp.name        = name;
            sp.price       = f.last;
            sp.change      = f.diff;
            sp.change_rate = f.rate;
            sp.base_price  = (f.diff != 0.0) ? f.last - f.diff : f.last;
            sp.pbr         = f.pbr;
            sp.per         = f.per;
            std::cout << "  " << code << " " << name << " 완료\n";
            std::cout.flush();
            {
                std::lock_guard<std::mutex> lk(cache_mtx);
                cache[code] = sp;
                display_order.push_back(code);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        std::cout << "로딩 완료. WebSocket 연결 중...\n";
        std::cout.flush();

        // ── [WS] 상위 N종목 체결 구독 ────────────────────────────────────────
        Logger::instance().set_console_enabled(false);

        KisWebSocket ws(kis_cfg);
        ws.set_callbacks(
            [](const OrderBook&) {},
            [&](const TradeData& td) {
                auto now = std::chrono::system_clock::now();
                auto tt  = std::chrono::system_clock::to_time_t(now);
                struct tm tmi{};
#ifdef _WIN32
                localtime_s(&tmi, &tt);
#else
                localtime_r(&tt, &tmi);
#endif
                char tbuf[16];
                std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmi);

                std::lock_guard<std::mutex> lk(cache_mtx);
                auto it = cache.find(td.ticker);
                if (it == cache.end()) return;
                auto& sp       = it->second;
                sp.price       = td.price;
                sp.direction   = td.direction;
                sp.change      = sp.price - sp.base_price;
                sp.change_rate = (sp.base_price > 0)
                                 ? sp.change / sp.base_price * 100.0 : 0.0;
                sp.updated     = tbuf;
            }
        );

        std::vector<WatchSpec> specs;
        {
            std::lock_guard<std::mutex> lk(cache_mtx);
            for (const auto& code : display_order)
                specs.push_back({code, Market::KR, ""});
        }

        bool ws_ok = ws.connect(specs);
        if (!ws_ok)
            LOG_WARN("[KR_TEST] WebSocket 연결 실패 — REST 초기값으로만 표시");

        // ── 화면 표시: 1초 주기 ──────────────────────────────────────────────
        std::cout << "\033[2J";

        while (g_running.load()) {
            std::map<std::string, StockPrice> snap;
            std::vector<std::string> order;
            {
                std::lock_guard<std::mutex> lk(cache_mtx);
                snap  = cache;
                order = display_order;
            }

            std::cout << "\033[H";

            auto now2 = std::chrono::system_clock::now();
            auto tt2  = std::chrono::system_clock::to_time_t(now2);
            struct tm tmi2{};
#ifdef _WIN32
            localtime_s(&tmi2, &tt2);
#else
            localtime_r(&tt2, &tmi2);
#endif
            char hbuf[32];
            std::strftime(hbuf, sizeof(hbuf), "%H:%M:%S", &tmi2);

            std::cout << "══════ KR 시가총액 상위 KOSPI 20 실시간 시세 ["
                      << hbuf << "] "
                      << (ws_ok ? "[WS:연결]" : "[WS:끊김]")
                      << " ══════\n";
            std::cout << " #   코드     종목명          현재가     등락     등락률   PBR   PER   갱신\n";
            std::cout << std::string(80, '-') << "\n";

            int idx = 0;
            for (const auto& code : order) {
                ++idx;
                auto it = snap.find(code);
                if (it == snap.end()) {
                    std::cout << std::setw(2) << idx << "  " << code << "  (로딩 중...)\n";
                    continue;
                }
                const StockPrice& sp = it->second;

                // 당일 등락(change)으로 색/화살표 결정 — direction은 마지막 체결 틱 방향이라 부정확
                const char* col = sp.change > 0 ? "\033[31m"   // 상승 = 빨간색
                                : sp.change < 0 ? "\033[34m"   // 하락 = 파란색
                                : "";
                const char* rst = "\033[0m";
                const char* arr = sp.change > 0 ? "\xE2\x96\xB2"   // ▲
                                : sp.change < 0 ? "\xE2\x96\xBC"   // ▼
                                : " ";

                std::string disp_name = utf8_pad_right(utf8_trunc(sp.name, 10), 10);

                char ln[256];
                snprintf(ln, sizeof(ln),
                    "%2d  %-6s  %s  %s%9.0f  %s %+8.0f  %+6.2f%%%s  %4.2f  %5.1f  %s\n",
                    idx,
                    code.c_str(),
                    disp_name.c_str(),
                    col, sp.price,
                    arr, sp.change, sp.change_rate, rst,
                    sp.pbr, sp.per,
                    sp.updated.empty() ? "--:--:--" : sp.updated.c_str());
                std::cout << ln;
            }

            std::cout << "\nCtrl+C 종료\n";
            std::cout.flush();

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (ws_ok) ws.disconnect();
        Logger::instance().set_console_enabled(true);
        LOG_INFO("[KR_TEST] 종료");
        return 0;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  US_TEST 모드: M7 REST 시세 확인 (장 외 시간에도 동작)
    //   - WebSocket 불필요 — KIS 해외주식 REST만 사용
    //   - 실시간 체결은 미국 정규장(KST 22:30~05:00)에만 가능
    // ═══════════════════════════════════════════════════════════════════════
    if (mode == "US_TEST") {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD  dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
        static const std::vector<std::pair<std::string,std::string>> M7 = {
            {"AAPL","Apple"}, {"MSFT","Microsoft"}, {"NVDA","NVIDIA"},
            {"AMZN","Amazon"}, {"GOOGL","Alphabet"}, {"META","Meta"},
            {"TSLA","Tesla"}
        };

        KisClient kis(kis_cfg);
        if (!kis.authenticate()) {
            LOG_ERROR("[US_TEST] KIS 인증 실패");
            return 1;
        }

        Logger::instance().set_console_enabled(false);

        // 공유 캐시
        struct StockCache {
            Fundamentals            fund;
            std::vector<MarketData> bars;
            std::string             updated;  // HH:MM:SS
        };
        std::mutex                         cache_mtx;
        std::map<std::string, StockCache>  cache;
        double                             kr_px = 0.0;

        // ── 백그라운드 fetch 스레드 ──────────────────────────────────────────
        // 7종목을 순환하며 계속 갱신. 종목당 ~250ms → 전체 1.75s/cycle
        std::thread fetcher([&]() {
            // 최초 1회: 삼성전자 현재가 확인
            kr_px = kis.get_current_price("005930");

            while (g_running.load()) {
                for (const auto& [sym, name] : M7) {
                    if (!g_running.load()) break;

                    auto f    = kis.get_us_fundamentals(sym, "NAS");
                    auto bars = kis.get_us_daily_ohlcv(sym, 5, "NAS");

                    auto now = std::chrono::system_clock::now();
                    auto tt  = std::chrono::system_clock::to_time_t(now);
                    struct tm tmi{};
#ifdef _WIN32
                    localtime_s(&tmi, &tt);
#else
                    localtime_r(&tt, &tmi);
#endif
                    char tbuf[16];
                    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmi);

                    {
                        std::lock_guard<std::mutex> lk(cache_mtx);
                        cache[sym] = { f, bars, tbuf };
                    }
                    // KIS rate limit: 종목당 최소 200ms 간격
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        });

        // ── 화면 표시 루프: 500ms 주기 ──────────────────────────────────────
        std::cout << "\033[2J";

        while (g_running.load()) {
            std::cout << "\033[H";

            auto now = std::chrono::system_clock::now();
            auto tt  = std::chrono::system_clock::to_time_t(now);
            struct tm tm_info{};
#ifdef _WIN32
            localtime_s(&tm_info, &tt);
#else
            localtime_r(&tt, &tm_info);
#endif
            char tbuf[32];
            std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);

            std::cout << "══════════ M7 미국주식 시세 [" << tbuf << "] ══════════\n";
            std::cout << std::fixed << std::setprecision(0);
            std::cout << "  국내 삼성전자: ";
            if (kr_px > 0) std::cout << kr_px << "원";
            else            std::cout << "조회 중...";
            std::cout << "\n\n";

            std::lock_guard<std::mutex> lk(cache_mtx);

            bool any_ok = false;
            for (const auto& [sym, name] : M7) {
                auto it = cache.find(sym);

                std::cout << "  [" << name << " / " << sym << "]";
                if (it != cache.end()) {
                    std::cout << "  갱신: " << it->second.updated;
                }
                std::cout << "\n";

                if (it == cache.end()) {
                    std::cout << "    (조회 중...)\n\n";
                    continue;
                }

                const auto& f    = it->second.fund;
                const auto& bars = it->second.bars;

                if (f.last > 0.0) {
                    any_ok = true;
                    std::string dir = (f.rate > 0) ? "▲" : (f.rate < 0 ? "▼" : "-");
                    std::cout << std::setprecision(2)
                              << "    현재가: $" << f.last << " " << dir
                              << std::showpos << std::setprecision(2) << f.rate << "%"
                              << " (" << f.diff << ")\n" << std::noshowpos;
                    if (f.open > 0.0)
                        std::cout << "    시가: $" << f.open
                                  << "  고가: $" << f.high
                                  << "  저가: $" << f.low << "\n";
                    if (f.pbid > 0.0 || f.pask > 0.0)
                        std::cout << "    매수호가: $" << f.pbid << "  매도호가: $" << f.pask << "\n";
                    std::cout << std::setprecision(1)
                              << "    PER=" << f.per << "  PBR=" << f.pbr << "\n";
                } else {
                    std::cout << "    (시세 없음 — 장 외 또는 API 오류)\n";
                }

                if (!bars.empty()) {
                    std::cout << "    일봉:";
                    for (const auto& b : bars)
                        std::cout << "  $" << std::setprecision(2) << b.close;
                    if (bars.size() >= 4) {
                        bool dec = bars[0].close < bars[1].close
                                && bars[1].close < bars[2].close
                                && bars[2].close < bars[3].close;
                        if (dec) std::cout << "  ※3일연속하락";
                    }
                    std::cout << "\n";
                }
                std::cout << "\n";
            }

            std::cout << (any_ok ? "해외 API: OK" : "해외 API: 대기 중") << "\n";
            std::cout << "Ctrl+C 로 종료\n";
            std::cout.flush();

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        fetcher.join();
        Logger::instance().set_console_enabled(true);
        LOG_INFO("[US_TEST] 종료");
        return 0;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  TRADE 모드: 전략 매매 엔진 (tickers 설정 불필요 — 전략이 동적으로 구성)
    // ═══════════════════════════════════════════════════════════════════════
    int interval = cfg.value("fetch_interval_sec", 60);
    Engine engine(kis_cfg, interval);
    g_engine = &engine;

    for (auto& s : cfg["strategies"]) {
        std::string type = s["type"];
        int         qty  = s.value("quantity", 1);

        if (type == "MA_CROSS") {
            engine.add_strategy(std::make_unique<MACrossStrategy>(
                s["ticker"].get<std::string>(),
                s["short_period"].get<int>(),
                s["long_period"].get<int>(), qty));

        } else if (type == "MOMENTUM") {
            engine.add_strategy(std::make_unique<MomentumStrategy>(
                s["ticker"].get<std::string>(),
                s["period"].get<int>(), qty));

        } else if (type == "VALUE_CONTRARY") {
            std::string market_str = s.value("market", "KR");
            Market      market     = (market_str == "US") ? Market::US : Market::KR;
            std::string exchange   = s.value("exchange", "");
            double      pbr_max    = s.value("pbr_max", 1.0);
            int         eod_hhmm   = s.value("eod_exit_hhmm", 1520);
            engine.add_strategy(std::make_unique<ValueContraryStrategy>(
                market, exchange, pbr_max, qty, eod_hhmm));

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
