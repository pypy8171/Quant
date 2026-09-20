// DevScale 순수 판정(strategy/DevScaleRules.h) 단위 테스트 — 무장 후 고가 트레일의 무장·발동·미발동 경계,
//  체결 원장에서 당일 매수 종목을 고르는 규칙(FILL·BUY·슬리브 접두어·짧은 행), 일봉 ATR과 하루 단위 진입 필터를
//  고정한다. 헤더 전용이라 파일·로그 없이 돈다.
// 빌드: cmake --build <directory> --target test_devscale_rules
#include "strategy/DevScaleRules.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <vector>

using namespace devscale_rules;

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

int test_peak_trail()
{
    // 평단 10,000 · 무장 +1.0% · 트레일 −1.0%
    CHECK(!peak_trail_triggered(10050.0, 10000.0, 9900.0, 1.0, 1.0));   // 고가가 무장선(10,100) 아래 — 얼마나 빠져도 안 판다
    CHECK(!peak_trail_triggered(10100.0, 10000.0, 10050.0, 1.0, 1.0));  // 무장은 됐지만 고가 대비 −0.5%뿐
    CHECK(peak_trail_triggered(10100.0, 10000.0, 9999.0, 1.0, 1.0));    // 무장 뒤 고가 대비 −1.0% — 판다
    CHECK(peak_trail_triggered(10500.0, 10000.0, 10395.0, 1.0, 1.0));   // 더 올랐다 −1.0% — 이익 보존
    CHECK(!peak_trail_triggered(10500.0, 10000.0, 10396.0, 1.0, 1.0));  // 경계 바로 위는 안 판다
    CHECK(!peak_trail_triggered(10500.0, 0.0, 9000.0, 1.0, 1.0));       // 평단 없음 — 판정 보류
    CHECK(!peak_trail_triggered(0.0, 10000.0, 9000.0, 1.0, 1.0));       // 고가 없음(보유 직후)
    CHECK(!peak_trail_triggered(10500.0, 10000.0, 9000.0, 0.0, 1.0));   // 기능 끔
    return 0;
}

int test_ledger_reader()
{
    std::istringstream ledger(
        "ts_kst,event,order_id,odno,strategy,ticker,side,type,price,qty\n"
        "09:00:01,ACCEPTED,1,A1,DEVSCALE_005930,005930,BUY,LIMIT,70000,10\n"
        "09:00:02,FILL,1,A1,DEVSCALE_005930,005930,BUY,LIMIT,70000,10\n"
        "09:10:00,FILL,2,A2,DEVSCALE_000660,000660,SELL,LIMIT,120000,5\n"   // 매도만 있는 종목
        "09:20:00,FILL,3,A3,ITB_035420,035420,BUY,LIMIT,200000,3\n"         // 다른 슬리브
        "09:30:00,FILL,4,A4,DEVSCALEX_051910,051910,BUY,LIMIT,300000,1\n"   // 접두어가 다르다(DEVSCALEX_)
        "09:40:00,CANCELLED,5,A5,DEVSCALE_068270,068270,BUY,LIMIT,150000,2\n"
        "short,line\n"
        "09:50:00,FILL,6,A6,DEVSCALE_068270,068270,BUY,LIMIT,150000,2\n");
    const std::set<std::string> bought = tickers_bought_from_ledger(ledger, "DEVSCALE");
    CHECK(bought.size() == 2);
    CHECK(bought.count("005930") == 1);
    CHECK(bought.count("068270") == 1);
    CHECK(bought.count("000660") == 0);
    CHECK(bought.count("035420") == 0);
    CHECK(bought.count("051910") == 0);

    std::istringstream empty("");
    CHECK(tickers_bought_from_ledger(empty, "DEVSCALE").empty());
    return 0;
}

MarketData daily_bar(double high, double low, double close)
{
    MarketData bar;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    return bar;
}

int test_average_true_range()
{
    // 최신이 앞. 참범위: [0] 고-저 200 vs |고-전일종가 10000|=100 → 200, [1] 고-저 100 vs |저-전일종가 10300|=350 → 350 → 평균 275
    const std::vector<MarketData> daily = {daily_bar(10100.0, 9900.0, 10000.0), daily_bar(10050.0, 9950.0, 10000.0),
                                           daily_bar(10400.0, 10200.0, 10300.0)};
    CHECK(std::abs(average_true_range(daily, 2) - 275.0) < 1e-9);
    CHECK(average_true_range(daily, 3) == 0.0);     // 전일 종가가 모자란다(4개 필요)
    CHECK(average_true_range(daily, 0) == 0.0);
    CHECK(average_true_range({}, 14) == 0.0);
    return 0;
}

int test_entry_day_allowed()
{
    // ATR 상한 5% · 개장 이격 −3%~+99%
    CHECK(entry_day_allowed(4.9, 0.0, 5.0, -3.0, 99.0));
    CHECK(!entry_day_allowed(5.1, 0.0, 5.0, -3.0, 99.0));    // 변동 큰 날
    CHECK(!entry_day_allowed(3.0, -3.5, 5.0, -3.0, 99.0));   // 갭 하락 개장
    CHECK(entry_day_allowed(3.0, -3.0, 5.0, -3.0, 99.0));    // 경계 포함
    CHECK(entry_day_allowed(12.0, -8.0, 0.0, -99.0, 99.0));  // 필터 끔
    CHECK(!entry_day_allowed(3.0, 6.0, 5.0, -3.0, 5.0));     // 상단 밖
    return 0;
}
} // namespace

int main()
{
    if (test_peak_trail() != 0)
    {
        return 1;
    }

    if (test_ledger_reader() != 0)
    {
        return 1;
    }

    if (test_average_true_range() != 0)
    {
        return 1;
    }

    if (test_entry_day_allowed() != 0)
    {
        return 1;
    }

    std::cout << "test_devscale_rules: " << g_checks << " checks passed\n";
    return 0;
}
