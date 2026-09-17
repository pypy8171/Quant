#include "modes/Monitors.h"
#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "core/KstTime.h"
#include "core/MarketSession.h"
#include "core/Types.h"
#include "utils/Logger.h"
#include "utils/Utf8.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
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
#ifdef ERROR
#undef ERROR // wingdi.h — LogLevel::ERROR와 부딪힌다
#endif
#endif
#ifdef HAS_ZMQ
#include "ipc/ZmqBridge.h"
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  FEED 모드 — 시세 표시 유틸
// ═══════════════════════════════════════════════════════════════════════════

static const std::map<std::string, std::string> TICKER_NAMES = {{"005380", "현대차  "}, {"005930", "삼성전자"},
                                                                {"000660", "SK하이닉"}, {"402340", "SK스퀘어"},
                                                                {"006400", "삼성SDI "}, {"009150", "삼성전기"}};

static std::string fmt_int_comma(long long value, int width, const std::string& empty)
{
    if (value <= 0)
    {
        return empty;
    }

    std::string text = std::to_string(value);
    std::string raw;
    int count = 0;

    for (int index = static_cast<int>(text.size()) - 1; index >= 0; --index)
    {
        if (count > 0 && count % 3 == 0)
        {
            raw = "," + raw;
        }

        raw = text[index] + raw;
        ++count;
    }

    while (static_cast<int>(raw.size()) < width)
    {
        raw = " " + raw;
    }

    return raw;
}

static std::string fmt_price(double value) { return fmt_int_comma(static_cast<long long>(value), 8, "       -"); }
static std::string fmt_qty(int64_t value)  { return fmt_int_comma(value, 7, "      -"); }

static std::string fmt_time_hms(int32_t hhmmss)
{
    if (hhmmss <= 0)
    {
        return "--:--:--";
    }

    const std::string ticker = krx::hhmmss_str(hhmmss);
    return ticker.substr(0, 2) + ":" + ticker.substr(2, 2) + ":" + ticker.substr(4, 2);
}

// 체결 방향(TradeData.direction, KIS 부호와 동일): 1=매수우위(상승) ▲, 5=매도우위(하락) ▼.
static std::string dir_str(int days)
{
    if (days == 1)
    {
        return "\xE2\x96\xB2"; // UTF-8 ▲
    }

    if (days == 5)
    {
        return "\xE2\x96\xBC"; // UTF-8 ▼
    }

    return "-";
}

// ─── 1초마다 콘솔에 시세 표시 ────────────────────────────────────────────
static void print_feed(const std::vector<std::string>& tickers, std::mutex& mutex,
                       const std::map<std::string, OrderBook>& ob_cache,
                       const std::map<std::string, TradeData>& td_cache)
{
    // 커서를 맨 위로 이동 (깜빡임 없이 덮어쓰기)
    std::cout << "\033[H";

    // 현재 시각
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    const struct tm tm_info = kst::to_tm(tt);
    char time_buffer[32];
    std::strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &tm_info);

    std::cout << "══════════════════════════ 실시간 시세 [" << time_buffer << " KST] ══════════════════════════\n\n";

    std::lock_guard<std::mutex> lock(mutex);

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
            trade_t = fmt_time_hms(td_it->second.hhmmss);
        }

        // 종목 헤더
        std::cout << "  [" << name << " " << ticker << "]"
                  << "  체결: " << fmt_price(trade_px) << "원 " << dir_str(trade_dir) << "  (" << trade_t << ")\n";

        // 호가 테이블 헤더
        std::cout << "    매도호가         잔량    │    매수호가         잔량\n";

        if (ob_it != ob_cache.end())
        {
            const auto& order_book = ob_it->second;

            // 매도5↔매수1, 매도4↔매수2, ..., 매도1↔매수5
            for (int index = 4; index >= 0; --index)
            {
                // 매도: asks[i] (i=4이 가장 멀리, i=0이 최우선)
                // 매수: bids[4-i] (최우선매수가 위, 멀수록 아래)
                int inner_index = 4 - index;
                std::cout << "    매도" << (index + 1) << ": " << fmt_price(order_book.asks[index].price) << " ("
                          << fmt_qty(order_book.asks[index].quantity) << ")"
                          << "  │  "
                          << "매수" << (inner_index + 1) << ": " << fmt_price(order_book.bids[inner_index].price) << " ("
                          << fmt_qty(order_book.bids[inner_index].quantity) << ")"
                          << "\n";
            }
        }
        else
        {
            for (int index = 0; index < 5; ++index)
            {
                std::cout << "    (데이터 수신 대기...)                     \n";
            }
        }

        std::cout << "\n";
    }

    std::cout.flush();
}

// ═══════════════════════════════════════════════════════════════════════════
//  FEED 모드: WebSocket 실시간 호가/체결 표시
// ═══════════════════════════════════════════════════════════════════════════
int run_feed(const KisConfig& kis_cfg, const std::vector<std::string>& tickers,
             const std::vector<std::string>& futures, const std::atomic<bool>& running)
{
    // SetConsoleOutputCP + ANSI 이스케이프는 main 상단에서 이미 설정됨
    Logger::instance().set_console_enabled(false);

    std::mutex cache_mtx;
    std::map<std::string, OrderBook> ob_cache;
    std::map<std::string, TradeData> td_cache;

    KisWebSocket ws(kis_cfg);
    ws.set_callbacks(
        [&](const OrderBook& order_book)
        {
            std::lock_guard<std::mutex> lock(cache_mtx);
            ob_cache[order_book.ticker.str()] = order_book;
        },
        [&](const TradeData& trade)
        {
            std::lock_guard<std::mutex> lock(cache_mtx);
            td_cache[trade.ticker.str()] = trade;
        });

    std::vector<WatchSpec> specs;

    for (const auto& ticker : tickers)
    {
        specs.push_back({ticker, Market::KR, ""});
    }

    // 국내 선물 — H0IFCNT0 체결·H0IFASP0 호가. 실계좌 WS 도메인 전용(모의 미지원)이라
    // kis 블록이 is_paper=false여야 데이터가 온다.
    for (const auto& fcode : futures)
    {
        WatchSpec fs;
        fs.ticker = fcode;
        fs.is_future = true;
        specs.push_back(fs);
    }

    if (kis_cfg.is_paper && !futures.empty())
    {
        LOG_WARN("[Main] FEED에 선물 종목이 있으나 kis.is_paper=true — 선물 실시간은 모의 미지원이라 "
                 "데이터가 안 옵니다. 실계좌 키 config로 실행하세요.");
    }

    // 화면 렌더용 통합 목록(현물 뒤에 선물). print_feed는 종목코드 키로 캐시를 찾으므로
    // 선물 코드도 그대로 표시된다.
    std::vector<std::string> display = tickers;
    display.insert(display.end(), futures.begin(), futures.end());

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
    while (running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!ws.is_connected())
        {
            LOG_WARN("[Main] WebSocket 연결 끊김");
            break;
        }

        print_feed(display, cache_mtx, ob_cache, td_cache);
    }

    ws.disconnect();
    LOG_INFO("[Main] FEED 종료");
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  KR_TEST 모드
//   [REST 1회] 인증 + PBR·PER·전일기준가 초기 로드 (종목당 300ms)
//   [WS 실시간] H0STCNT0 체결 구독 → 가격 캐시 업데이트
//   [화면] 1초 주기로 캐시 출력
// ═══════════════════════════════════════════════════════════════════════════
int run_kr_test(const KisConfig& kis_cfg, const std::atomic<bool>& running)
{
    // UTF-8 유틸 — utils/Utf8.h 참조
    auto utf8_pad_right = [](const std::string& text, int time_value) { return utf8::pad_right(text, time_value); };
    auto utf8_trunc     = [](const std::string& text, int row) { return utf8::trunc(text, row); };

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
        {
            return 0.0;
        }

        double sum = 0.0;

        for (int period_index = 0; period_index < period; ++period_index)
        {
            sum += bars[period_index].close;
        }

        return sum / period;
    };

    // 시가총액(억원) → 콤팩트 문자열 (543210억→"54.3조", 12345억→"1.2조", 500억→"500억")
    auto fmt_cap = [](double value) -> std::string
    {
        if (value <= 0)
        {
            return "    --";
        }

        char buffer[16];

        if (value >= 10000.0)
        {
            snprintf(buffer, sizeof(buffer), "%.1f조", value / 10000.0);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%.0f억", value);
        }

        return buffer;
    };

    // MA값 → 콤팩트 문자열 (283250→"283K", 1900000→"1.9M")
    auto fmt_ma = [](double value) -> std::string
    {
        if (value <= 0)
        {
            return "   --";
        }

        char buffer[16];

        if (value >= 1'000'000.0)
        {
            snprintf(buffer, sizeof(buffer), "%.1fM", value / 1'000'000.0);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%.0fK", value / 1'000.0);
        }

        return buffer;
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

    for (const auto& top20_entry : KR_TOP20)
    {
        all_stocks.push_back(top20_entry);
    }

    for (const auto& watch_entry : KR_WATCH)
    {
        all_stocks.push_back(watch_entry);
    }

    for (const auto& [code, name] : all_stocks)
    {
        auto fundamentals = kis.get_fundamentals(code);
        auto bars = kis.get_daily_ohlcv(code, 65); // MA60 계산용

        StockPrice sp;
        sp.ticker = code;
        sp.name = name;
        sp.price = fundamentals.last;
        sp.change = fundamentals.diff;
        sp.change_rate = fundamentals.rate;
        sp.base_price = (fundamentals.diff != 0.0) ? fundamentals.last - fundamentals.diff : fundamentals.last;
        sp.pbr = fundamentals.pbr;
        sp.per = fundamentals.per;
        sp.ma5        = calc_ma(bars, 5);
        sp.ma10       = calc_ma(bars, 10);
        sp.ma20       = calc_ma(bars, 20);
        sp.ma60       = calc_ma(bars, 60);
        sp.market_cap = fundamentals.market_cap;
        std::cout << "  " << code << " " << name << " 완료\n";
        std::cout.flush();
        {
            std::lock_guard<std::mutex> lock(cache_mtx);
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
                     [&](const TradeData& trade)
                     {
                         auto now = std::chrono::system_clock::now();
                         auto tt = std::chrono::system_clock::to_time_t(now);
                         const struct tm tmi = kst::to_tm(tt);
                         char time_buffer[16];
                         std::strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &tmi);

                         std::lock_guard<std::mutex> lock(cache_mtx);
                         auto iterator = cache.find(trade.ticker.str());

                         if (iterator == cache.end())
                         {
                             return;
                         }

                         auto& sp = iterator->second;
                         sp.price = trade.price;
                         sp.direction = trade.direction;
                         sp.change = sp.price - sp.base_price;
                         sp.change_rate = (sp.base_price > 0) ? sp.change / sp.base_price * 100.0 : 0.0;
                         sp.updated = time_buffer;
#ifdef HAS_ZMQ
                         zmq_br->publish_trade(trade);
#endif
                     });

    std::vector<WatchSpec> specs;
    {
        std::lock_guard<std::mutex> lock(cache_mtx);

        for (const auto& code : display_order)
        {
            specs.push_back({code, Market::KR, "", true}); // trade_only: 구독 28개로 한도 절약
        }
    }

    bool ws_ok = ws.connect(specs);

    if (!ws_ok)
    {
        LOG_WARN("[KR_TEST] WebSocket 연결 실패 — REST 초기값으로만 표시");
    }

    // ── [백그라운드] 지수 5초 폴링 ───────────────────────────────────────
    std::thread idx_poller(
        [&]()
        {
            while (running.load())
            {
                for (const auto& [code, name] : INDICES)
                {
                    if (!running.load())
                    {
                        break;
                    }

                    auto ip = kis.get_index_price(code);
                    {
                        std::lock_guard<std::mutex> lock(idx_mtx);
                        idx_cache[code] = {ip, true};
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }

                // 지수 3종 조회(각 300ms) 뒤 약 4.7초 대기 → 폴링 주기 대략 5초.
                // 종료 신호에 빨리 반응하도록 100ms 단위로 쪼갠다(47회 × 100ms = 4.7초).
                for (int index = 0; index < 47 && running.load(); ++index)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
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
        const char* array = sp.change > 0   ? "\xE2\x96\xB2"
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
                 rank, code.c_str(), disp_name.c_str(), col, sp.price, array, sp.change, sp.change_rate, rst,
                 sp.pbr, sp.per, scap.c_str(), s5.c_str(), s10.c_str(), s20.c_str(), s60.c_str(),
                 sp.updated.empty() ? "--:--:--" : sp.updated.c_str());
        return std::string(ln);
    };

    while (running.load())
    {
        std::map<std::string, StockPrice> snapshot;
        {
            std::lock_guard<std::mutex> lock(cache_mtx);
            snapshot = cache;
        }

        // 출력할 모든 줄을 벡터로 수집 후 한 번에 출력
        std::vector<std::string> lines;

        auto now2 = std::chrono::system_clock::now();
        auto tt2 = std::chrono::system_clock::to_time_t(now2);
        const struct tm tmi2 = kst::to_tm(tt2);
        char header_buffer[32];
        std::strftime(header_buffer, sizeof(header_buffer), "%H:%M:%S", &tmi2);

        // 헤더 + 지수 (2줄)
        {
            char character[160];
            snprintf(character, sizeof(character), "══ KR 실시간 시세 [%s KST] %s ══", header_buffer,
                     ws_ok ? "[WS:연결]" : "[WS:끊김]");
            lines.push_back(character);
        }

        {
            std::string idx_line;
            std::lock_guard<std::mutex> ilk(idx_mtx);

            for (const auto& [code, name] : INDICES)
            {
                auto iterator = idx_cache.find(code);

                if (iterator == idx_cache.end() || !iterator->second.loaded)
                {
                    idx_line += name + ":조회중  ";
                    continue;
                }

                const auto& ip = iterator->second.data;
                // KIS 전일대비 부호(sign): 1=상한, 2=상승, 3=보합, 4=하한, 5=하락.
                // 상승계열(1·2)=빨강↑, 하락계열(4·5)=파랑↓, 보합(3)=색 없음.
                const char* col = (ip.sign == 1 || ip.sign == 2)   ? "\033[31m"
                                  : (ip.sign == 4 || ip.sign == 5) ? "\033[34m"
                                                                    : "";
                const char* rst = "\033[0m";
                const char* array = (ip.sign == 1 || ip.sign == 2)   ? "\xE2\x96\xB2"
                                  : (ip.sign == 4 || ip.sign == 5) ? "\xE2\x96\xBC"
                                                                    : " ";
                char seg[120];
                snprintf(seg, sizeof(seg), "%s:%s%.2f%s%+.2f(%+.2f%%)%s  ", name.c_str(), col, ip.price, array,
                         ip.change, ip.change_rate, rst);
                idx_line += seg;
            }

            lines.push_back(idx_line);
        }

        // ── KOSPI 상위 20 (구분선 + 헤더 각 1줄) ─────────────────────
        lines.push_back("\033[90m── KOSPI 시총 상위 20 " + std::string(50, '-') + "\033[0m");
        lines.push_back(" #  code    name            price         chg     chg%   PBR    PER     cap    MA5  MA10  MA20  MA60      time");

        for (size_t kr_top20_index = 0; kr_top20_index < KR_TOP20.size(); ++kr_top20_index)
        {
            const auto& code = KR_TOP20[kr_top20_index].first;
            auto iterator = snapshot.find(code);

            if (iterator == snapshot.end())
            {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%2zu  %s  (로딩 중...)", kr_top20_index + 1, code.c_str());
                lines.push_back(tmp);
            }
            else
            {
                lines.push_back(stock_row(static_cast<int>(kr_top20_index + 1), code, iterator->second));
            }
        }

        // ── 관심 종목 (구분선만, 헤더 재사용) ────────────────────────
        lines.push_back("\033[90m── 관심 종목 " + std::string(59, '-') + "\033[0m");

        for (size_t kr_watch_index = 0; kr_watch_index < KR_WATCH.size(); ++kr_watch_index)
        {
            const auto& code = KR_WATCH[kr_watch_index].first;
            auto iterator = snapshot.find(code);

            if (iterator == snapshot.end())
            {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%2zu  %s  (로딩 중...)", kr_watch_index + 1, code.c_str());
                lines.push_back(tmp);
            }
            else
            {
                lines.push_back(stock_row(static_cast<int>(kr_watch_index + 1), code, iterator->second));
            }
        }

        lines.push_back("");
        lines.push_back("Ctrl+C 종료");

        // ── 출력: 커서를 항상 화면 최상단으로 이동 후 덮어쓰기
        std::cout << "\033[H";

        for (const auto& line : lines)
        {
            std::cout << line << "\033[K\n";
        }

        std::cout << "\033[J";
        std::cout.flush();

        std::this_thread::sleep_for(std::chrono::seconds(1));
#ifdef HAS_ZMQ
        zmq_br->publish_health(0, 0, 0);
#endif
    }

    if (ws_ok)
    {
        ws.disconnect();
    }

    idx_poller.join();
    std::cout << "\033[?1049l"; // alternate screen 종료 → 원래 터미널 복원
    std::cout.flush();
    Logger::instance().set_console_enabled(true);
    LOG_INFO("[KR_TEST] 종료");
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  US_TEST 모드: M7 REST 시세 확인 (장 외 시간에도 동작)
//   - WebSocket 불필요 — KIS 해외주식 REST만 사용
//   - 실시간 체결은 미국 정규장(KST 22:30~05:00)에만 가능
// ═══════════════════════════════════════════════════════════════════════════
int run_us_test(const KisConfig& kis_cfg, const std::atomic<bool>& running)
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

            while (running.load())
            {
                for (const auto& [ticker, name] : M7)
                {
                    if (!running.load())
                    {
                        break;
                    }

                    auto us_fundamentals = kis.get_us_fundamentals(ticker, "NAS");
                    auto bars = kis.get_us_daily_ohlcv(ticker, 5, "NAS");

                    auto now = std::chrono::system_clock::now();
                    auto tt = std::chrono::system_clock::to_time_t(now);
                    const struct tm tmi = kst::to_tm(tt);
                    char time_buffer[16];
                    std::strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &tmi);

                    {
                        std::lock_guard<std::mutex> lock(cache_mtx);
                        cache[ticker] = {us_fundamentals, bars, time_buffer};
                    }

                    // KIS rate limit: 종목당 최소 200ms 간격
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        });

    // ── 화면 표시 루프: 500ms 주기 ──────────────────────────────────────
    std::cout << "\033[2J";

    while (running.load())
    {
        std::cout << "\033[H";

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        const struct tm tm_info = kst::to_tm(tt);
        char time_buffer[32];
        std::strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &tm_info);

        std::cout << "══════════ M7 미국주식 시세 [" << time_buffer << " KST] ══════════\n";
        std::cout << std::fixed << std::setprecision(0);
        std::cout << "  국내 삼성전자: ";

        if (kr_px > 0)
        {
            std::cout << kr_px << "원";
        }
        else
        {
            std::cout << "조회 중...";
        }

        std::cout << "\n\n";

        std::lock_guard<std::mutex> lock(cache_mtx);

        bool any_ok = false;

        for (const auto& [ticker, name] : M7)
        {
            auto iterator = cache.find(ticker);

            std::cout << "  [" << name << " / " << ticker << "]";

            if (iterator != cache.end())
            {
                std::cout << "  갱신: " << iterator->second.updated;
            }

            std::cout << "\n";

            if (iterator == cache.end())
            {
                std::cout << "    (조회 중...)\n\n";
                continue;
            }

            const auto& fund = iterator->second.fund;
            const auto& bars = iterator->second.bars;

            if (fund.last > 0.0)
            {
                any_ok = true;
                std::string dir = (fund.rate > 0) ? "▲" : (fund.rate < 0 ? "▼" : "-");
                std::cout << std::setprecision(2) << "    현재가: $" << fund.last << " " << dir << std::showpos
                          << std::setprecision(2) << fund.rate << "%"
                          << " (" << fund.diff << ")\n"
                          << std::noshowpos;

                if (fund.open > 0.0)
                {
                    std::cout << "    시가: $" << fund.open << "  고가: $" << fund.high << "  저가: $" << fund.low << "\n";
                }

                if (fund.bid_price > 0.0 || fund.ask_price > 0.0)
                {
                    std::cout << "    매수호가: $" << fund.bid_price << "  매도호가: $" << fund.ask_price << "\n";
                }

                std::cout << std::setprecision(1) << "    PER=" << fund.per << "  PBR=" << fund.pbr << "\n";
            }
            else
            {
                std::cout << "    (시세 없음 — 장 외 또는 API 오류)\n";
            }

            if (!bars.empty())
            {
                std::cout << "    일봉:";

                for (const auto& bar : bars)
                {
                    std::cout << "  $" << std::setprecision(2) << bar.close;
                }

                if (bars.size() >= 4)
                {
                    bool dec = bars[0].close < bars[1].close && bars[1].close < bars[2].close &&
                               bars[2].close < bars[3].close;

                    if (dec)
                    {
                        std::cout << "  ※3일연속하락";
                    }
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
