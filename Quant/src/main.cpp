#include "api/KisWebSocket.h"
#include "core/Engine.h"
#include "strategy/DeviationScaleStrategy.h"
#include "strategy/FixedIntervalStrategy.h"
#include "strategy/IntradayBreakoutStrategy.h"
#include "strategy/MACrossStrategy.h"
#include "strategy/MarketMakingStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "strategy/PriceTargetStrategy.h"
#include "strategy/SupplyDemandPullbackStrategy.h"
#include "strategy/ThemeStrategy.h"
#include "strategy/ValueContraryStrategy.h"
#include "utils/Logger.h"
#include "utils/Utf8.h"
#include <atomic>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <unordered_set>

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

// ═══════════════════════════════════════════════════════════════════════════
//  FEED 모드 — 시세 표시 유틸
// ═══════════════════════════════════════════════════════════════════════════

static const std::map<std::string, std::string> TICKER_NAMES = {{"005380", "현대차  "}, {"005930", "삼성전자"},
                                                                {"000660", "SK하이닉"}, {"402340", "SK스퀘어"},
                                                                {"006400", "삼성SDI "}, {"009150", "삼성전기"}};

static std::string fmt_int_comma(long long v, int width, const std::string& empty)
{
    if (v <= 0)
        return empty;
    std::string s = std::to_string(v);
    std::string r;
    int cnt = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i)
    {
        if (cnt > 0 && cnt % 3 == 0)
            r = "," + r;
        r = s[i] + r;
        ++cnt;
    }
    while (static_cast<int>(r.size()) < width)
        r = " " + r;
    return r;
}

static std::string fmt_price(double v) { return fmt_int_comma(static_cast<long long>(v), 8, "       -"); }
static std::string fmt_qty(int64_t v)  { return fmt_int_comma(v, 7, "      -"); }

static std::string fmt_time_hms(const std::string& t)
{
    if (t.size() < 6)
        return "--:--:--";
    return t.substr(0, 2) + ":" + t.substr(2, 2) + ":" + t.substr(4, 2);
}

static std::string dir_str(int d)
{
    if (d == 1)
        return "\xE2\x96\xB2"; // UTF-8 ▲
    if (d == 5)
        return "\xE2\x96\xBC"; // UTF-8 ▼
    return "-";
}

// ─── 1초마다 콘솔에 시세 표시 ────────────────────────────────────────────
static void print_feed(const std::vector<std::string>& tickers, std::mutex& mtx,
                       const std::map<std::string, OrderBook>& ob_cache,
                       const std::map<std::string, TradeData>& td_cache)
{
    // 커서를 맨 위로 이동 (깜빡임 없이 덮어쓰기)
    std::cout << "\033[H";

    // 현재 시각
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
#ifdef _WIN32
    struct tm tm_info
    {
    };
    localtime_s(&tm_info, &tt);
#else
    struct tm tm_info
    {
    };
    localtime_r(&tt, &tm_info);
#endif
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);

    std::cout << "══════════════════════════ 실시간 시세 [" << tbuf << "] ══════════════════════════\n\n";

    std::lock_guard<std::mutex> lk(mtx);

    for (const auto& ticker : tickers)
    {
        auto name_it = TICKER_NAMES.find(ticker);
        const std::string& name = (name_it != TICKER_NAMES.end()) ? name_it->second : ticker;

        auto td_it = td_cache.find(ticker);
        auto ob_it = ob_cache.find(ticker);

        double trade_px = 0.0;
        int trade_dir = 0;
        std::string trade_t = "--:--:--";

        if (td_it != td_cache.end())
        {
            trade_px = td_it->second.price;
            trade_dir = td_it->second.direction;
            trade_t = fmt_time_hms(td_it->second.time);
        }

        // 종목 헤더
        std::cout << "  [" << name << " " << ticker << "]"
                  << "  체결: " << fmt_price(trade_px) << "원 " << dir_str(trade_dir) << "  (" << trade_t << ")\n";

        // 호가 테이블 헤더
        std::cout << "    매도호가         잔량    │    매수호가         잔량\n";

        if (ob_it != ob_cache.end())
        {
            const auto& ob = ob_it->second;
            // 매도5↔매수1, 매도4↔매수2, ..., 매도1↔매수5
            for (int i = 4; i >= 0; --i)
            {
                // 매도: asks[i] (i=4이 가장 멀리, i=0이 최우선)
                // 매수: bids[4-i] (최우선매수가 위, 멀수록 아래)
                int j = 4 - i;
                std::cout << "    매도" << (i + 1) << ": " << fmt_price(ob.asks[i].price) << " ("
                          << fmt_qty(ob.asks[i].quantity) << ")"
                          << "  │  "
                          << "매수" << (j + 1) << ": " << fmt_price(ob.bids[j].price) << " ("
                          << fmt_qty(ob.bids[j].quantity) << ")"
                          << "\n";
            }
        }
        else
        {
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
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    // 로그는 cwd 루트에 흩뿌리지 않고 logs/ 하위로 모은다(부모 폴더는 Logger가 자동 생성).
    Logger::instance().init("logs/quant_trader.log", LogLevel::INFO);
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
    //  FEED 모드: WebSocket 실시간 호가/체결 표시
    // ═══════════════════════════════════════════════════════════════════════
    if (mode == "FEED")
    {
        // SetConsoleOutputCP + ANSI 이스케이프는 main 상단에서 이미 설정됨
        Logger::instance().set_console_enabled(false);

        std::mutex cache_mtx;
        std::map<std::string, OrderBook> ob_cache;
        std::map<std::string, TradeData> td_cache;

        KisWebSocket ws(kis_cfg);
        ws.set_callbacks(
            [&](const OrderBook& ob)
            {
                std::lock_guard<std::mutex> lk(cache_mtx);
                ob_cache[ob.ticker] = ob;
            },
            [&](const TradeData& td)
            {
                std::lock_guard<std::mutex> lk(cache_mtx);
                td_cache[td.ticker] = td;
            });

        std::vector<WatchSpec> specs;
        for (const auto& t : tickers)
            specs.push_back({t, Market::KR, ""});

        LOG_INFO("[Main] FEED 모드 — WebSocket 연결 시도");
        if (!ws.connect(specs))
        {
            LOG_ERROR("[Main] WebSocket 연결 실패");
            return 1;
        }

        // 화면 초기화
        std::cout << "\033[2J";

        LOG_INFO("[Main] 시세 수신 시작 (Ctrl+C 로 종료)");

        // 1초 주기 표시 루프
        while (g_running.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!ws.is_connected())
            {
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
    if (mode == "KR_TEST")
    {
        // UTF-8 유틸 — utils/Utf8.h 참조
        auto utf8_display_width = [](const std::string& s) { return utf8::display_width(s); };
        auto utf8_pad_right     = [](const std::string& s, int t) { return utf8::pad_right(s, t); };
        auto utf8_trunc         = [](const std::string& s, int m) { return utf8::trunc(s, m); };

        KisClient kis(kis_cfg);
        if (!kis.authenticate())
        {
            LOG_ERROR("[KR_TEST] KIS 인증 실패");
            return 1;
        }

        struct StockPrice
        {
            std::string ticker, name;
            double price = 0;
            double base_price = 0;
            double change = 0;
            double change_rate = 0;
            double pbr = 0;
            double per = 0;
            double ma5 = 0, ma10 = 0, ma20 = 0, ma60 = 0;
            double market_cap = 0; // 억원
            int direction = 0;
            std::string updated;
        };

        // 이동평균 계산 헬퍼 — KIS는 최신봉이 bars[0]에 오므로 앞에서 period개를 사용
        auto calc_ma = [](const std::vector<MarketData>& bars, int period) -> double
        {
            if (static_cast<int>(bars.size()) < period)
                return 0.0;
            double sum = 0.0;
            for (int i = 0; i < period; ++i)
                sum += bars[i].close;
            return sum / period;
        };

        // 시가총액(억원) → 콤팩트 문자열 (543210억→"54.3조", 12345억→"1.2조", 500억→"500억")
        auto fmt_cap = [](double v) -> std::string
        {
            if (v <= 0)
                return "    --";
            char buf[16];
            if (v >= 10000.0)
                snprintf(buf, sizeof(buf), "%.1f조", v / 10000.0);
            else
                snprintf(buf, sizeof(buf), "%.0f억", v);
            return buf;
        };

        // MA값 → 콤팩트 문자열 (283250→"283K", 1900000→"1.9M")
        auto fmt_ma = [](double v) -> std::string
        {
            if (v <= 0)
                return "   --";
            char buf[16];
            if (v >= 1'000'000.0)
                snprintf(buf, sizeof(buf), "%.1fM", v / 1'000'000.0);
            else
                snprintf(buf, sizeof(buf), "%.0fK", v / 1'000.0);
            return buf;
        };

        // KOSPI 시가총액 상위 20 고정 목록 (KIS 랭킹 API가 거래량 기준이라 직접 정의)
        static const std::vector<std::pair<std::string, std::string>> KR_TOP20 = {
            {"005930", "삼성전자  "}, {"000660", "SK하이닉스"}, {"207940", "삼성바이오"}, {"005490", "POSCO홀딩 "},
            {"005380", "현대차    "}, {"000270", "기아      "}, {"105560", "KB금융    "}, {"055550", "신한지주  "},
            {"035420", "NAVER     "}, {"068270", "셀트리온  "}, {"051910", "LG화학    "}, {"066570", "LG전자    "},
            {"012330", "현대모비스"}, {"035720", "카카오    "}, {"003550", "LG        "}, {"086790", "하나금융  "},
            {"017670", "SK텔레콤  "}, {"009150", "삼성전기  "}, {"402340", "SK스퀘어  "}, {"316140", "우리금융  "},
        };

        // 관심 종목
        static const std::vector<std::pair<std::string, std::string>> KR_WATCH = {
            {"042700", "한미반도체"}, {"108490", "로보티즈  "}, {"097230", "HJ중공업  "},
            {"327260", "RF머트리얼"}, {"482630", "삼양엔씨켐"}, {"489790", "한화비전  "},
            {"295310", "에이치브이"}, {"100790", "미래에셋벤"},
            {"080220", "제주반도체"},
        };

        // 지수 캐시 (REST 5초 폴링)
        struct IdxSnap
        {
            KisClient::IndexPrice data;
            bool loaded = false;
        };
        std::mutex idx_mtx;
        std::map<std::string, IdxSnap> idx_cache;
        static const std::vector<std::pair<std::string, std::string>> INDICES = {
            {"0001", "코스피  "}, {"1001", "코스닥  "}, {"2001", "KOSPI200"},
        };

        std::mutex cache_mtx;
        std::map<std::string, StockPrice> cache;
        std::vector<std::string> display_order;

        // ── [REST 1회] 20종목 + 관심종목 fundamentals 로드 ──────────────────
        std::cout << "\033[2J\033[H";
        std::cout << "KOSPI 시가총액 상위 20 + 관심종목 로딩 중 (REST)...\n";
        std::cout.flush();

        // 모든 종목 합쳐서 순서대로 로드
        std::vector<std::pair<std::string, std::string>> all_stocks;
        for (const auto& s : KR_TOP20)
            all_stocks.push_back(s);
        for (const auto& s : KR_WATCH)
            all_stocks.push_back(s);

        for (const auto& [code, name] : all_stocks)
        {
            auto f = kis.get_fundamentals(code);
            auto bars = kis.get_daily_ohlcv(code, 65); // MA60 계산용

            StockPrice sp;
            sp.ticker = code;
            sp.name = name;
            sp.price = f.last;
            sp.change = f.diff;
            sp.change_rate = f.rate;
            sp.base_price = (f.diff != 0.0) ? f.last - f.diff : f.last;
            sp.pbr = f.pbr;
            sp.per = f.per;
            sp.ma5        = calc_ma(bars, 5);
            sp.ma10       = calc_ma(bars, 10);
            sp.ma20       = calc_ma(bars, 20);
            sp.ma60       = calc_ma(bars, 60);
            sp.market_cap = f.market_cap;
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

#ifdef HAS_ZMQ
        auto zmq_br = std::make_unique<ZmqBridge>();
        zmq_br->start();
#endif

        KisWebSocket ws(kis_cfg);
        ws.set_callbacks([](const OrderBook&) {},
                         [&](const TradeData& td)
                         {
                             auto now = std::chrono::system_clock::now();
                             auto tt = std::chrono::system_clock::to_time_t(now);
                             struct tm tmi
                             {
                             };
#ifdef _WIN32
                             localtime_s(&tmi, &tt);
#else
                              localtime_r(&tt, &tmi);
#endif
                             char tbuf[16];
                             std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmi);

                             std::lock_guard<std::mutex> lk(cache_mtx);
                             auto it = cache.find(td.ticker);
                             if (it == cache.end())
                                 return;
                             auto& sp = it->second;
                             sp.price = td.price;
                             sp.direction = td.direction;
                             sp.change = sp.price - sp.base_price;
                             sp.change_rate = (sp.base_price > 0) ? sp.change / sp.base_price * 100.0 : 0.0;
                             sp.updated = tbuf;
#ifdef HAS_ZMQ
                             zmq_br->publish_trade(td);
#endif
                         });

        std::vector<WatchSpec> specs;
        {
            std::lock_guard<std::mutex> lk(cache_mtx);
            for (const auto& code : display_order)
                specs.push_back({code, Market::KR, "", true}); // trade_only: 구독 28개로 한도 절약
        }

        bool ws_ok = ws.connect(specs);
        if (!ws_ok)
            LOG_WARN("[KR_TEST] WebSocket 연결 실패 — REST 초기값으로만 표시");

        // ── [백그라운드] 지수 5초 폴링 ───────────────────────────────────────
        std::thread idx_poller(
            [&]()
            {
                while (g_running.load())
                {
                    for (const auto& [code, name] : INDICES)
                    {
                        if (!g_running.load())
                            break;
                        auto ip = kis.get_index_price(code);
                        {
                            std::lock_guard<std::mutex> lk(idx_mtx);
                            idx_cache[code] = {ip, true};
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    }
                    // 5초 대기 (인터럽트 가능하게 100ms 단위로 분할)
                    for (int i = 0; i < 47 && g_running.load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });

        // ── 화면 표시: 1초 주기 ──────────────────────────────────────────────
        // alternate screen buffer: 스크롤 없이 독립 화면 사용 (top/vim 방식)
        // 종료 시 \033[?1049l 로 원래 화면 복원
        std::cout << "\033[?1049h\033[H";

        // 헬퍼: 한 종목 행 → string 반환 (줄 끝 \n 미포함)
        auto stock_row = [&](int rank, const std::string& code, const StockPrice& sp) -> std::string
        {
            const char* col = sp.change > 0   ? "\033[31m"
                              : sp.change < 0 ? "\033[34m"
                                              : "";
            const char* rst = "\033[0m";
            const char* arr = sp.change > 0   ? "\xE2\x96\xB2"
                              : sp.change < 0 ? "\xE2\x96\xBC"
                                              : " ";
            std::string disp_name = utf8_pad_right(utf8_trunc(sp.name, 10), 10);
            std::string scap = fmt_cap(sp.market_cap);
            std::string s5   = fmt_ma(sp.ma5);
            std::string s10  = fmt_ma(sp.ma10);
            std::string s20  = fmt_ma(sp.ma20);
            std::string s60  = fmt_ma(sp.ma60);
            char ln[512];
            snprintf(ln, sizeof(ln),
                     "%2d  %-6s  %s  %s%9.0f  %s %+8.0f  %+6.2f%%%s  %4.2f  %5.1f  %6s  %5s %5s %5s %5s  %s",
                     rank, code.c_str(), disp_name.c_str(), col, sp.price, arr, sp.change, sp.change_rate, rst,
                     sp.pbr, sp.per, scap.c_str(), s5.c_str(), s10.c_str(), s20.c_str(), s60.c_str(),
                     sp.updated.empty() ? "--:--:--" : sp.updated.c_str());
            return std::string(ln);
        };

        while (g_running.load())
        {
            std::map<std::string, StockPrice> snap;
            {
                std::lock_guard<std::mutex> lk(cache_mtx);
                snap = cache;
            }

            // 출력할 모든 줄을 벡터로 수집 후 한 번에 출력
            std::vector<std::string> lines;

            auto now2 = std::chrono::system_clock::now();
            auto tt2 = std::chrono::system_clock::to_time_t(now2);
            struct tm tmi2
            {
            };
#ifdef _WIN32
            localtime_s(&tmi2, &tt2);
#else
            localtime_r(&tt2, &tmi2);
#endif
            char hbuf[32];
            std::strftime(hbuf, sizeof(hbuf), "%H:%M:%S", &tmi2);

            // 헤더 + 지수 (2줄)
            {
                char h[160];
                snprintf(h, sizeof(h), "══ KR 실시간 시세 [%s] %s ══", hbuf,
                         ws_ok ? "[WS:연결]" : "[WS:끊김]");
                lines.push_back(h);
            }
            {
                std::string idx_line;
                std::lock_guard<std::mutex> ilk(idx_mtx);
                for (const auto& [code, name] : INDICES)
                {
                    auto it = idx_cache.find(code);
                    if (it == idx_cache.end() || !it->second.loaded)
                    {
                        idx_line += name + ":조회중  ";
                        continue;
                    }
                    const auto& ip = it->second.data;
                    const char* col = (ip.sign == 1 || ip.sign == 2)   ? "\033[31m"
                                      : (ip.sign == 4 || ip.sign == 5) ? "\033[34m"
                                                                        : "";
                    const char* rst = "\033[0m";
                    const char* arr = (ip.sign == 1 || ip.sign == 2)   ? "\xE2\x96\xB2"
                                      : (ip.sign == 4 || ip.sign == 5) ? "\xE2\x96\xBC"
                                                                        : " ";
                    char seg[120];
                    snprintf(seg, sizeof(seg), "%s:%s%.2f%s%+.2f(%+.2f%%)%s  ", name.c_str(), col, ip.price, arr,
                             ip.change, ip.change_rate, rst);
                    idx_line += seg;
                }
                lines.push_back(idx_line);
            }

            // ── KOSPI 상위 20 (구분선 + 헤더 각 1줄) ─────────────────────
            lines.push_back("\033[90m── KOSPI 시총 상위 20 " + std::string(50, '-') + "\033[0m");
            lines.push_back(" #  code    name            price         chg     chg%   PBR    PER     cap    MA5  MA10  MA20  MA60      time");
            for (size_t i = 0; i < KR_TOP20.size(); ++i)
            {
                const auto& code = KR_TOP20[i].first;
                auto it = snap.find(code);
                if (it == snap.end())
                {
                    char tmp[64];
                    snprintf(tmp, sizeof(tmp), "%2zu  %s  (로딩 중...)", i + 1, code.c_str());
                    lines.push_back(tmp);
                }
                else
                {
                    lines.push_back(stock_row(static_cast<int>(i + 1), code, it->second));
                }
            }

            // ── 관심 종목 (구분선만, 헤더 재사용) ────────────────────────
            lines.push_back("\033[90m── 관심 종목 " + std::string(59, '-') + "\033[0m");
            for (size_t i = 0; i < KR_WATCH.size(); ++i)
            {
                const auto& code = KR_WATCH[i].first;
                auto it = snap.find(code);
                if (it == snap.end())
                {
                    char tmp[64];
                    snprintf(tmp, sizeof(tmp), "%2zu  %s  (로딩 중...)", i + 1, code.c_str());
                    lines.push_back(tmp);
                }
                else
                {
                    lines.push_back(stock_row(static_cast<int>(i + 1), code, it->second));
                }
            }

            lines.push_back("");
            lines.push_back("Ctrl+C 종료");

            // ── 출력: 커서를 항상 화면 최상단으로 이동 후 덮어쓰기
            std::cout << "\033[H";
            for (const auto& line : lines)
                std::cout << line << "\033[K\n";
            std::cout << "\033[J";
            std::cout.flush();

            std::this_thread::sleep_for(std::chrono::seconds(1));
#ifdef HAS_ZMQ
            zmq_br->publish_health(0, 0, 0);
#endif
        }

        if (ws_ok)
            ws.disconnect();
        idx_poller.join();
        std::cout << "\033[?1049l"; // alternate screen 종료 → 원래 터미널 복원
        std::cout.flush();
        Logger::instance().set_console_enabled(true);
        LOG_INFO("[KR_TEST] 종료");
        return 0;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  US_TEST 모드: M7 REST 시세 확인 (장 외 시간에도 동작)
    //   - WebSocket 불필요 — KIS 해외주식 REST만 사용
    //   - 실시간 체결은 미국 정규장(KST 22:30~05:00)에만 가능
    // ═══════════════════════════════════════════════════════════════════════
    if (mode == "US_TEST")
    {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
        static const std::vector<std::pair<std::string, std::string>> M7 = {
            {"AAPL", "Apple"},     {"MSFT", "Microsoft"}, {"NVDA", "NVIDIA"}, {"AMZN", "Amazon"},
            {"GOOGL", "Alphabet"}, {"META", "Meta"},      {"TSLA", "Tesla"}};

        KisClient kis(kis_cfg);
        if (!kis.authenticate())
        {
            LOG_ERROR("[US_TEST] KIS 인증 실패");
            return 1;
        }

        Logger::instance().set_console_enabled(false);

        // 공유 캐시
        struct StockCache
        {
            Fundamentals fund;
            std::vector<MarketData> bars;
            std::string updated; // HH:MM:SS
        };
        std::mutex cache_mtx;
        std::map<std::string, StockCache> cache;
        double kr_px = 0.0;

        // ── 백그라운드 fetch 스레드 ──────────────────────────────────────────
        // 7종목을 순환하며 계속 갱신. 종목당 ~250ms → 전체 1.75s/cycle
        std::thread fetcher(
            [&]()
            {
                // 최초 1회: 삼성전자 현재가 확인
                kr_px = kis.get_current_price("005930");

                while (g_running.load())
                {
                    for (const auto& [sym, name] : M7)
                    {
                        if (!g_running.load())
                            break;

                        auto f = kis.get_us_fundamentals(sym, "NAS");
                        auto bars = kis.get_us_daily_ohlcv(sym, 5, "NAS");

                        auto now = std::chrono::system_clock::now();
                        auto tt = std::chrono::system_clock::to_time_t(now);
                        struct tm tmi
                        {
                        };
#ifdef _WIN32
                        localtime_s(&tmi, &tt);
#else
                        localtime_r(&tt, &tmi);
#endif
                        char tbuf[16];
                        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmi);

                        {
                            std::lock_guard<std::mutex> lk(cache_mtx);
                            cache[sym] = {f, bars, tbuf};
                        }
                        // KIS rate limit: 종목당 최소 200ms 간격
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                }
            });

        // ── 화면 표시 루프: 500ms 주기 ──────────────────────────────────────
        std::cout << "\033[2J";

        while (g_running.load())
        {
            std::cout << "\033[H";

            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            struct tm tm_info
            {
            };
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
            if (kr_px > 0)
                std::cout << kr_px << "원";
            else
                std::cout << "조회 중...";
            std::cout << "\n\n";

            std::lock_guard<std::mutex> lk(cache_mtx);

            bool any_ok = false;
            for (const auto& [sym, name] : M7)
            {
                auto it = cache.find(sym);

                std::cout << "  [" << name << " / " << sym << "]";
                if (it != cache.end())
                {
                    std::cout << "  갱신: " << it->second.updated;
                }
                std::cout << "\n";

                if (it == cache.end())
                {
                    std::cout << "    (조회 중...)\n\n";
                    continue;
                }

                const auto& f = it->second.fund;
                const auto& bars = it->second.bars;

                if (f.last > 0.0)
                {
                    any_ok = true;
                    std::string dir = (f.rate > 0) ? "▲" : (f.rate < 0 ? "▼" : "-");
                    std::cout << std::setprecision(2) << "    현재가: $" << f.last << " " << dir << std::showpos
                              << std::setprecision(2) << f.rate << "%"
                              << " (" << f.diff << ")\n"
                              << std::noshowpos;
                    if (f.open > 0.0)
                        std::cout << "    시가: $" << f.open << "  고가: $" << f.high << "  저가: $" << f.low << "\n";
                    if (f.pbid > 0.0 || f.pask > 0.0)
                        std::cout << "    매수호가: $" << f.pbid << "  매도호가: $" << f.pask << "\n";
                    std::cout << std::setprecision(1) << "    PER=" << f.per << "  PBR=" << f.pbr << "\n";
                }
                else
                {
                    std::cout << "    (시세 없음 — 장 외 또는 API 오류)\n";
                }

                if (!bars.empty())
                {
                    std::cout << "    일봉:";
                    for (const auto& b : bars)
                        std::cout << "  $" << std::setprecision(2) << b.close;
                    if (bars.size() >= 4)
                    {
                        bool dec = bars[0].close < bars[1].close && bars[1].close < bars[2].close &&
                                   bars[2].close < bars[3].close;
                        if (dec)
                            std::cout << "  ※3일연속하락";
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
    // G5: 기동 시 실계좌 보유분을 OrderGate 원장에 시드(ITB 매도수량·평단·손실한도 정합).
    engine.set_bootstrap_ledger(cfg.value("bootstrap_ledger_from_balance", false));
    // REST 현재가 폴링을 체결 피드로 사용(WS 실시간 세션 rt=9 폭주 우회). ITB가 이 틱으로 구동.
    engine.set_rest_price_feed(cfg.value("rest_price_feed", false));
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
        engine.set_risk_config(rc);
        LOG_INFO("[Main] risk 한도: 종목당 " + std::to_string(rc.max_qty_per_ticker) + "주, 일손실 " +
                 std::to_string((long long)rc.daily_loss_limit) + "원, " +
                 std::to_string(rc.max_orders_per_sec) + "/s·" +
                 std::to_string(rc.max_orders_per_min) + "/min");

        // 주문 페이싱(C-2/W-3) — 버스트 청산 EGW00201 회피 + 거부 SELL 재시도.
        int pace_ms = r.value("order_min_interval_ms", 350);
        int max_ret = r.value("order_max_retries", 3);
        engine.set_order_pacing(pace_ms, max_ret);
        LOG_INFO("[Main] 주문 페이싱: " + std::to_string(pace_ms) + "ms 간격, 청산 SELL 재시도 " +
                 std::to_string(max_ret) + "회");
    }

    for (auto& s : cfg["strategies"])
    {
        std::string type = s["type"];
        int qty = s.value("quantity", 1);
        size_t n_before = engine.strategy_count();

        if (type == "MA_CROSS")
        {
            int sp = s["short_period"].get<int>();
            int lp = s["long_period"].get<int>();

            if (s.value("universe_from_balance", false))
            {
                // 모의계좌 보유종목 전체를 유니버스로 — 종목마다 MACross 등록.
                // 보유분은 start_in_position=true 로 시드 → 데드크로스에 실제 보유수량 매도, 골든크로스에 재매수.
                KisClient bal_kis(kis_cfg);
                if (!bal_kis.authenticate())
                {
                    LOG_ERROR("[Main] universe_from_balance: 잔고조회용 인증 실패 — 건너뜀");
                }
                else
                {
                    nlohmann::json bal = bal_kis.get_balance();
                    int added = 0;
                    if (bal.contains("output1"))
                    {
                        for (auto& h : bal["output1"])
                        {
                            std::string code = h.value("pdno", "");
                            int hq           = std::atoi(h.value("hldg_qty", "0").c_str());
                            if (!code.empty() && hq > 0)
                            {
                                engine.add_strategy(std::make_unique<MACrossStrategy>(
                                    code, sp, lp, hq, /*start_in_position=*/true));
                                LOG_INFO("[Main]   + MACross " + code + " 보유 " + std::to_string(hq) + "주 (in_position 시드)");
                                ++added;
                            }
                        }
                    }
                    LOG_INFO("[Main] universe_from_balance: 보유 " + std::to_string(added) +
                             "종목 등록 (short=" + std::to_string(sp) + " long=" + std::to_string(lp) + ")");
                }
            }
            else
            {
                engine.add_strategy(std::make_unique<MACrossStrategy>(
                    s["ticker"].get<std::string>(), sp, lp, qty));
            }
        }
        else if (type == "INTRADAY_BREAKOUT")
        {
            // 첫 장중 자동매매 기준(ITB) — WS 체결 틱 기반 채널돌파 + 트레일/하드 스탑.
            int channel_min   = s.value("channel_min", 10);
            double eps         = s.value("breakout_eps", 0.002);
            double trail_pct   = s.value("trail_pct", 0.010);
            double hard_pct    = s.value("hard_pct", 0.015);
            int eod_hhmm       = s.value("eod_hhmm", 1515);
            int cooldown_sec   = s.value("reentry_cooldown_sec", 60);
            int entry_qty      = s.value("entry_qty", 1); // 신규 돌파 진입 수량(명목 미지정 시)
            double avg_loss_pct = s.value("avg_loss_pct", 0.0); // 평단 대비 손절률(0=비활성)
            // ── v2 파라미터(strategies/ITB/SPEC.md §2/§3) ──
            double seed_trail_pct      = s.value("seed_trail_pct", 0.0);      // 물린분 앵커 트레일(넓게)
            double exit_near_avg_pct   = s.value("exit_near_avg_pct", 0.0);   // 물린분 본전탈출 임계
            int    no_new_entry_hhmm   = s.value("no_new_entry_hhmm", 0);     // 신규진입 금지 시각(0→eod)
            double notional_per_position = s.value("notional_per_position", 0.0); // 종목당 명목(원)

            if (s.value("universe_from_scan", false))
            {
                // ── 거래대금 상위 스캔 유니버스(ITB v2) ─────────────────────────
                //  실전 도메인 키로 거래대금 랭킹 → 등락률/가격 필터 → (opt)수급 → 레짐 게이트
                //  통과 종목만 ITB(start_in_position=false, 명목 사이징)로 등록.
                int    scan_top_n    = s.value("scan_top_n", 30);
                double chg_min       = s.value("chg_min", 0.02);
                double chg_max       = s.value("chg_max", 0.12);
                double min_price     = s.value("min_price", 3000.0);
                bool   sd_filter     = s.value("sd_filter", true);
                double risk_off_idx  = s.value("risk_off_index_pct", -0.01);
                int    max_register  = s.value("max_concurrent_positions", 3) * 2; // 후보는 상한의 2배까지 등록(경쟁)

                if (!has_quote_kis)
                {
                    LOG_ERROR("[Main] universe_from_scan: quote_kis(실전 시세 키) 미설정 — 스캔 불가, 건너뜀");
                }
                else
                {
                    KisClient scan_kis(quote_kis_cfg);
                    if (!scan_kis.authenticate())
                    {
                        LOG_ERROR("[Main] universe_from_scan: 시세 키 인증 실패 — 건너뜀");
                    }
                    else
                    {
                        // 레짐 게이트: 코스피(0001) 당일 등락률이 risk_off 이하면 신규매수 유니버스 전면 스킵.
                        auto kospi = scan_kis.get_index_price("0001");
                        double idx_chg = kospi.change_rate / 100.0; // KIS는 % 단위
                        if (idx_chg < risk_off_idx)
                        {
                            LOG_WARN("[Main] universe_from_scan: 레짐 위험회피(코스피 " +
                                     std::to_string(kospi.change_rate) + "% < " +
                                     std::to_string(risk_off_idx * 100.0) + "%) — 신규매수 유니버스 미등록");
                        }
                        else
                        {
                            auto rank = scan_kis.fetch_value_ranking(scan_top_n, "J");
                            int added = 0;
                            for (const auto& r : rank)
                            {
                                if (added >= max_register)
                                    break;
                                double chg = r.change_rate / 100.0; // % → 비율
                                // 필터①: 등락률 밴드(강세 모멘텀, 급등 추격 배제)
                                if (chg < chg_min || chg > chg_max)
                                    continue;
                                // 필터②: 최소가(동전주·호가스프레드 배제)
                                if (r.price < min_price)
                                    continue;
                                // 필터③: 수급(opt) — 외국인 T-1 확정 순매수 > 0 (후보 소수에만 조회)
                                if (sd_filter)
                                {
                                    auto tr = scan_kis.get_investor_trend(r.ticker);
                                    if (tr.foreign_net <= 0)
                                    {
                                        LOG_INFO("[Main]   - ITB 스캔 제외 " + r.ticker + " 외국인순매수<=0");
                                        continue;
                                    }
                                }
                                // 통과 → 신규 진입 유니버스로 등록(당일 시가 앵커 주입).
                                //  ⚠️ 앵커는 랭킹 스냅샷 현재가(r.price)가 아니라 실제 당일 시가여야 함.
                                //  갭업일엔 스냅샷=장중 고점 근처라 앵커가 고점에 고정되어 돌파 진입이 영구 차단됨.
                                //  inquire-price(FHKST01010100)의 stck_oprc로 진짜 시가를 조회, 0이면 r.price 폴백.
                                double day_open = scan_kis.get_fundamentals(r.ticker).open;
                                if (day_open <= 0.0)
                                    day_open = r.price;
                                auto strat = std::make_unique<IntradayBreakoutStrategy>(
                                    r.ticker, entry_qty, /*hold_qty=*/0, /*start_in_position=*/false,
                                    channel_min, eps, trail_pct, hard_pct, eod_hhmm, cooldown_sec,
                                    /*avg_px=*/0.0, avg_loss_pct, seed_trail_pct, exit_near_avg_pct,
                                    no_new_entry_hhmm, notional_per_position, /*day_open_px=*/day_open);
                                strat->set_name(r.name);
                                engine.add_strategy(std::move(strat));
                                LOG_INFO("[Main]   + ITB 스캔 " + r.ticker + " " + r.name + " (등락 " +
                                         std::to_string(r.change_rate) + "% 가격 " +
                                         std::to_string((long long)r.price) + " 시가앵커 " +
                                         std::to_string((long long)day_open) + " 거래대금 " +
                                         std::to_string((long long)r.trade_value) + ")");
                                ++added;
                            }
                            LOG_INFO("[Main] universe_from_scan: 후보 " + std::to_string(rank.size()) +
                                     "종목 중 " + std::to_string(added) + "종목 등록");
                        }
                    }
                }
            }
            else if (s.value("universe_from_balance", false))
            {
                // 모의계좌 보유종목 전체를 유니버스로 — 종목마다 ITB 등록(보유분 in_position 시드).
                KisClient bal_kis(kis_cfg);
                if (!bal_kis.authenticate())
                {
                    LOG_ERROR("[Main] ITB universe_from_balance: 잔고조회용 인증 실패 — 건너뜀");
                }
                else
                {
                    nlohmann::json bal = bal_kis.get_balance();
                    int added = 0;
                    if (bal.contains("output1"))
                    {
                        for (auto& h : bal["output1"])
                        {
                            std::string code = h.value("pdno", "");
                            std::string pname = h.value("prdt_name", "");
                            int hq           = std::atoi(h.value("hldg_qty", "0").c_str());
                            double avg_px    = std::atof(h.value("pchs_avg_pric", "0").c_str());
                            if (!code.empty() && hq > 0)
                            {
                                auto strat = std::make_unique<IntradayBreakoutStrategy>(
                                    code, entry_qty, hq, /*start_in_position=*/true, channel_min, eps,
                                    trail_pct, hard_pct, eod_hhmm, cooldown_sec, avg_px, avg_loss_pct,
                                    seed_trail_pct, exit_near_avg_pct, no_new_entry_hhmm,
                                    /*notional=*/0.0, /*day_open_px=*/0.0);
                                strat->set_name(pname);
                                engine.add_strategy(std::move(strat));
                                LOG_INFO("[Main]   + ITB " + code + " " + pname + " 보유 " + std::to_string(hq) +
                                         "주 (in_position 시드, 평단=" + std::to_string((long long)avg_px) + ")");
                                ++added;
                            }
                        }
                    }
                    LOG_INFO("[Main] ITB universe_from_balance: 보유 " + std::to_string(added) + "종목 등록");
                }
            }
            else
            {
                engine.add_strategy(std::make_unique<IntradayBreakoutStrategy>(
                    s["ticker"].get<std::string>(), entry_qty, /*hold_qty=*/0, /*start_in_position=*/false,
                    channel_min, eps, trail_pct, hard_pct, eod_hhmm, cooldown_sec,
                    /*avg_px=*/0.0, avg_loss_pct, seed_trail_pct, exit_near_avg_pct,
                    no_new_entry_hhmm, notional_per_position, /*day_open_px=*/0.0));
            }
        }
        else if (type == "MOMENTUM")
        {
            engine.add_strategy(
                std::make_unique<MomentumStrategy>(s["ticker"].get<std::string>(), s["period"].get<int>(), qty));
        }
        else if (type == "VALUE_CONTRARY")
        {
            std::string market_str = s.value("market", "KR");
            Market market = (market_str == "US") ? Market::US : Market::KR;
            std::string exchange = s.value("exchange", "");
            double pbr_max = s.value("pbr_max", 1.0);
            int eod_hhmm = s.value("eod_exit_hhmm", 1520);
            engine.add_strategy(std::make_unique<ValueContraryStrategy>(market, exchange, pbr_max, qty, eod_hhmm));
        }
        else if (type == "FIXED_INTERVAL")
        {
            std::string ticker   = s["ticker"].get<std::string>();
            int buy_qty          = s.value("buy_qty", 1);
            int sell_qty         = s.value("sell_qty", 1);
            int interval_sec     = s.value("interval_sec", 300);
            engine.add_strategy(std::make_unique<FixedIntervalStrategy>(ticker, buy_qty, sell_qty, interval_sec));
        }
        else if (type == "PRICE_TARGET")
        {
            std::vector<PriceTargetStrategy::PriceTarget> price_targets;
            if (s.contains("price_targets"))
            {
                for (const auto& pt : s["price_targets"])
                {
                    PriceTargetStrategy::PriceTarget t;
                    t.ticker       = pt["ticker"].get<std::string>();
                    t.buy_price    = pt.value("buy_price",  0.0);
                    t.sell_price   = pt.value("sell_price", 0.0);
                    t.quantity     = pt.value("quantity",   1);
                    t.cooldown_sec = pt.value("cooldown_sec", 60);
                    price_targets.push_back(t);
                }
            }
            std::vector<PriceTargetStrategy::LimitOrder> limit_orders;
            if (s.contains("limit_orders"))
            {
                for (const auto& lo : s["limit_orders"])
                {
                    PriceTargetStrategy::LimitOrder l;
                    l.ticker   = lo["ticker"].get<std::string>();
                    l.side     = (lo.value("side", "BUY") == "SELL") ? OrderSide::SELL : OrderSide::BUY;
                    l.price    = lo.value("price",    0.0);
                    l.quantity = lo.value("quantity", 1);
                    limit_orders.push_back(l);
                }
            }
            engine.add_strategy(std::make_unique<PriceTargetStrategy>(
                std::move(price_targets), std::move(limit_orders)));
        }
        else if (type == "SUPPLY_DEMAND_PULLBACK")
        {
            SupplyDemandPullbackStrategy::Params sp;
            sp.market_div        = s.value("market_div",        "J");
            sp.universe_size     = s.value("universe_size",     50);
            sp.lookback_days     = s.value("lookback_days",     5);
            sp.min_dual_days     = s.value("min_dual_days",     3);
            sp.min_consec_days   = s.value("min_consec_days",   0);
            sp.net_buy_threshold = s.value("net_buy_threshold", (int64_t)0);
            sp.ma_period         = s.value("ma_period",         5);
            sp.pullback_band     = s.value("pullback_band",     0.01);
            sp.require_prev_above= s.value("require_prev_above",true);
            sp.quantity          = s.value("quantity",          10);
            sp.eod_exit_hhmm     = s.value("eod_exit_hhmm",    std::string("1500"));
            sp.stop_below_ma     = s.value("stop_below_ma",    0.0);
            std::string mode_str = s.value("entry_mode", "EOD");
            sp.mode = (mode_str == "INTRADAY")
                    ? SupplyDemandPullbackStrategy::EntryMode::INTRADAY
                    : SupplyDemandPullbackStrategy::EntryMode::EOD;
            engine.add_strategy(std::make_unique<SupplyDemandPullbackStrategy>(sp));
        }
        else if (type == "MARKET_MAKING")
        {
            std::string ticker      = s["ticker"].get<std::string>();
            int mm_qty              = s.value("quantity", 1);
            int half_spread_ticks   = s.value("half_spread_ticks", 1);
            int requote_move_ticks  = s.value("requote_move_ticks", 1);
            int min_requote_ms      = s.value("min_requote_ms", 1000); // ≥1000 권장(초당 4건 rate 백스톱)
            engine.add_strategy(std::make_unique<MarketMakingStrategy>(
                ticker, mm_qty, half_spread_ticks, requote_move_ticks, min_requote_ms));
        }
        else if (type == "DEVIATION_SCALE")
        {
            // 공통 파라미터(티커 제외) — 스캔 유니버스/단일 종목이 함께 쓴다.
            DeviationScaleStrategy::Params base;
            base.base_qty          = s.value("base_qty", 10);
            base.step_qty          = s.value("step_qty", 5);
            base.sma_period        = s.value("sma_period", 20);
            base.dev_sell          = s.value("dev_sell_pct", 1.5);
            base.dev_buy           = s.value("dev_buy_pct", 0.8);
            base.n_rungs           = s.value("n_rungs", 2);
            base.pullback_pct      = s.value("pullback_pct", 2.0);
            base.reprice_move_ticks = s.value("reprice_move_ticks", 2);
            base.eod_hhmm          = s.value("eod_exit_hhmm", 1515);
            base.interval_min      = s.value("interval_min", 3);
            base.min_action_ms     = s.value("min_action_ms", 3000);
            base.daily_lookback    = s.value("daily_lookback", 70);
            base.account           = s.value("account", std::string());

            // 티커 → DeviationScale 인스턴스 팩토리 (초기 스캔·주기적 재스캔 공용).
            auto factory = [base](const std::string& ticker) -> std::unique_ptr<StrategyBase> {
                DeviationScaleStrategy::Params dp = base;
                dp.ticker = ticker;
                return std::make_unique<DeviationScaleStrategy>(std::move(dp));
            };

            if (s.value("universe_from_scan", false))
            {
                // ── 전체 시장 자동 선정 ("둘 다": 시총 상위 ∪ 거래대금 상위) ─────────
                //  1단(여기): 시총 상위(넓은 유동 유니버스) + 거래대금 상위(장중 급변 종목)의
                //             합집합을 최소·최대가 필터로 압축.
                //  2단(전략): 등록된 각 DeviationScale이 자기 일봉으로 정배열+눌림 존을 판정 →
                //             자격 종목만 실제 오실레이션. 시장 스캔 + 종목별 자리판정 = 2단 선정.
                int    scan_top_n   = s.value("scan_top_n", 80);   // 시총 상위 스캔 수(넓은 유니버스)
                int    value_top_n  = s.value("value_top_n", 30);  // 거래대금 상위 스캔 수(장중 급변)
                double min_price    = s.value("min_price", 5000.0);
                double max_price    = s.value("max_price", 0.0);   // 0이면 상한 없음(고가주 포함)
                int    max_register = s.value("max_universe", 40);
                double risk_off_idx = s.value("risk_off_index_pct", -0.02);
                int    rescan_sec   = s.value("rescan_interval_sec", 600); // 주기적 재스캔 간격(초)

                // 유니버스 산출 람다 — 초기 등록과 주기적 재스캔이 공용으로 사용.
                //  레짐 위험회피면 빈 목록 반환(신규 미등록). 시총·거래대금 상위 합집합에
                //  가격 필터 적용, max_register개까지. 중복 티커는 seen으로 제거.
                auto universe = [=](KisClient& c) -> std::vector<std::string> {
                    std::vector<std::string> out;
                    auto kospi = c.get_index_price("0001");
                    if (kospi.change_rate / 100.0 < risk_off_idx)
                    {
                        LOG_WARN("[Main] DEVSCALE 스캔: 레짐 위험회피(코스피 " +
                                 std::to_string(kospi.change_rate) + "%) — 신규 유니버스 스킵");
                        return out;
                    }
                    std::unordered_set<std::string> seen;
                    auto take = [&](const auto& rank) {
                        for (const auto& r : rank)
                        {
                            if ((int)out.size() >= max_register) break;
                            if (r.price < min_price) continue;
                            if (max_price > 0.0 && r.price > max_price) continue;
                            if (!seen.insert(r.ticker).second) continue;
                            out.push_back(r.ticker);
                        }
                    };
                    take(c.fetch_kr_ranking(scan_top_n, "J"));     // 시총 상위
                    take(c.fetch_value_ranking(value_top_n, "J")); // 거래대금 상위
                    return out;
                };

                if (!has_quote_kis)
                {
                    LOG_ERROR("[Main] DEVSCALE universe_from_scan: quote_kis(실전 시세 키) 미설정 — 스캔 불가, 건너뜀");
                }
                else
                {
                    KisClient scan_kis(quote_kis_cfg);
                    if (!scan_kis.authenticate())
                    {
                        LOG_ERROR("[Main] DEVSCALE universe_from_scan: 시세 키 인증 실패 — 건너뜀");
                    }
                    else
                    {
                        auto tickers = universe(scan_kis);
                        int  added   = 0;
                        for (const auto& t : tickers)
                        {
                            engine.add_strategy(factory(t));
                            LOG_INFO("[Main]   + DEVSCALE 초기 " + t);
                            ++added;
                        }
                        LOG_INFO("[Main] DEVSCALE universe_from_scan: 초기 " + std::to_string(added) +
                                 "종목 등록 (각자 정배열+눌림 존 게이트로 자체 선별)");
                    }

                    // 주기적 재스캔 등록(동적) — data_thread가 rescan_sec마다 universe를 재호출해
                    //  신규 티커만 런타임 add. 인증 실패해도 재스캔은 엔진 내부 시세 클라이언트로 시도.
                    engine.set_universe_rescan(universe, factory, rescan_sec);
                    LOG_INFO("[Main] DEVSCALE 주기적 재스캔 활성: " + std::to_string(rescan_sec) + "초 간격");
                }
            }
            else
            {
                engine.add_strategy(factory(s["ticker"].get<std::string>()));
            }
        }
        else if (type == "THEME")
        {
            std::vector<std::string> sector_codes;
            if (s.contains("sector_codes") && s["sector_codes"].is_array())
                sector_codes = s["sector_codes"].get<std::vector<std::string>>();
            int top_n            = s.value("top_n_sectors", 2);
            double vol_surge     = s.value("volume_surge_mult", 2.0);
            bool inst_filter     = s.value("inst_filter", true);
            int eod_hhmm         = s.value("eod_exit_hhmm", 1520);
            engine.add_strategy(std::make_unique<ThemeStrategy>(
                sector_codes, top_n, vol_surge, inst_filter, qty, eod_hhmm));
        }
        else
        {
            LOG_WARN("[Main] 알 수 없는 전략: " + type);
        }

        // 전략-국면 매핑 (config "active_regimes", 미지정 시 전 국면). 추가된 전략에만 적용.
        if (engine.strategy_count() > n_before && s.contains("active_regimes") && s["active_regimes"].is_array())
        {
            std::vector<Regime> ar;
            for (const auto& r : s["active_regimes"])
            {
                std::string rs = r.get<std::string>();
                if (rs == "BULL")         ar.push_back(Regime::BULL);
                else if (rs == "NEUTRAL") ar.push_back(Regime::NEUTRAL);
                else if (rs == "BEAR")    ar.push_back(Regime::BEAR);
                else LOG_WARN("[Main] " + type + " 알 수 없는 active_regimes 값: '" + rs +
                              "' (BULL/NEUTRAL/BEAR만 유효) — 무시됨");   // G-2
            }
            if (!ar.empty())
            {
                engine.set_last_active_regimes(ar);
                LOG_INFO("[Main] " + type + " 활성국면: " + std::to_string(ar.size()) + "개");
            }
            else   // G-2: 키는 있는데 파싱 결과가 비면 게이트가 조용히 무력화됨 → 경고
                LOG_WARN("[Main] " + type + " active_regimes 파싱 결과 비어있음 — 전 국면 통과로 동작(게이트 무효)");
        }
    }

    engine.start();
    while (engine.is_running())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    LOG_INFO("[Main] 프로그램 종료");
    return 0;
}
