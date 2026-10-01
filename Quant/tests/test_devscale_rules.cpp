// DevScale 순수 판정(strategy/DevScaleRules.h) 단위 테스트 — 무장 후 고가 트레일의 무장·발동·미발동 경계,
//  체결 원장에서 종목별 순수량을 더하는 규칙(FILL만·슬리브 접두어·짧은 행·MANUAL 제외)과 재인수 판정, 일봉 ATR과 하루 단위 진입 필터를
//  고정한다. 헤더 전용이라 파일·로그 없이 돈다.
// 빌드: cmake --build <directory> --target test_devscale_rules
#include "strategy/DevScaleRules.h"

#include <cmath>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
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
    // 열은 Quant/src/ipc/OrderJournal.cpp kTradeHeader와 같다. fill_qty(11번째 칸)를 더한다.
    std::istringstream ledger(
        "ts_kst,event,order_id,odno,strategy,ticker,side,type,order_qty,order_price,fill_qty,fill_price,status\n"
        "09:00:01,ACCEPTED,1,A1,DEVSCALE_005930,005930,BUY,LIMIT,10,70000,0,0,ACCEPTED\n"
        "09:00:02,FILL,1,A1,DEVSCALE_005930,005930,BUY,LIMIT,10,70000,6,70000,ACCEPTED\n"   // 부분 체결 두 번
        "09:00:03,FILL,1,A1,DEVSCALE_005930,005930,BUY,LIMIT,10,70000,4,70000,FILLED\n"
        "09:10:00,FILL,2,A2,DEVSCALE_000660,000660,SELL,LIMIT,5,120000,5,120000,FILLED\n"   // 매도만 있는 종목
        "09:20:00,FILL,3,A3,ITB_035420,035420,BUY,LIMIT,3,200000,3,200000,FILLED\n"         // 다른 전략
        "09:30:00,FILL,4,A4,DEVSCALEX_051910,051910,BUY,LIMIT,1,300000,1,300000,FILLED\n"   // 접두어가 다르다(DEVSCALEX_)
        "09:40:00,CANCELLED,5,A5,DEVSCALE_068270,068270,BUY,LIMIT,2,150000,0,0,CANCELLED\n"
        "09:41:00,REJECTED,7,,DEVSCALE_068270,068270,BUY,LIMIT,9,150000,0,0,REJECTED\n"
        "short,line\n"
        "09:50:00,FILL,6,A6,DEVSCALE_068270,068270,BUY,LIMIT,2,150000,2,150000,FILLED\n");
    std::map<std::string, long long> net_quantity;
    add_net_quantity_from_ledger(ledger, "DEVSCALE", net_quantity);
    CHECK(net_quantity.size() == 3);
    CHECK(net_quantity["005930"] == 10);
    CHECK(net_quantity["068270"] == 2);   // 취소·거부 행은 세지 않는다
    CHECK(net_quantity["000660"] == -5);  // 음수도 남긴다 — 여러 날을 더한 뒤 거른다
    CHECK(net_quantity.count("035420") == 0);
    CHECK(net_quantity.count("051910") == 0);

    std::istringstream empty("");
    std::map<std::string, long long> none;
    add_net_quantity_from_ledger(empty, "DEVSCALE", none);
    CHECK(none.empty());
    return 0;
}

// 매수 뒤 전량 매도한 종목은 순수량 0이라 재인수하지 않는다. 날을 넘겨 더해도 같다.
int test_net_quantity_sold_out()
{
    std::map<std::string, long long> net_quantity;
    std::istringstream yesterday(
        "09:00:02,FILL,1,A1,DEVSCALE_005930,005930,BUY,LIMIT,10,70000,10,70000,FILLED\n");
    std::istringstream today(
        "10:00:00,FILL,2,A2,DEVSCALE_005930,005930,SELL,MARKET,10,0,7,71000,ACCEPTED\n"
        "10:00:01,FILL,2,A2,DEVSCALE_005930,005930,SELL,MARKET,10,0,3,71000,FILLED\n");
    add_net_quantity_from_ledger(yesterday, "DEVSCALE", net_quantity);
    add_net_quantity_from_ledger(today, "DEVSCALE", net_quantity);
    CHECK(net_quantity["005930"] == 0);
    CHECK(!devscale_owns_holding(net_quantity["005930"], 10)); // 같은 종목을 수동으로 10주 들고 있어도 안 맡는다
    return 0;
}

// 수동(MANUAL) 매수는 순수량에 넣지 않는다 — 순수량이 잔고보다 적으면 청산 관리에 둔다.
int test_net_quantity_ignores_manual()
{
    std::map<std::string, long long> net_quantity;
    std::istringstream ledger(
        "09:00:02,FILL,1,A1,DEVSCALE_005930,005930,BUY,LIMIT,10,70000,10,70000,FILLED\n"
        "09:30:00,FILL,2,A2,MANUAL,005930,BUY,MARKET,20,0,20,70500,FILLED\n"
        "09:40:00,FILL,3,A3,MANUAL,000660,BUY,MARKET,5,0,5,120000,FILLED\n");
    add_net_quantity_from_ledger(ledger, "DEVSCALE", net_quantity);
    CHECK(net_quantity.size() == 1);
    CHECK(net_quantity["005930"] == 10);
    CHECK(!devscale_owns_holding(net_quantity["005930"], 30)); // 잔고 30주 중 20주는 수동 몫
    CHECK(devscale_owns_holding(net_quantity["005930"], 10));  // 잔고가 전부 DevScale 몫이면 맡는다
    CHECK(devscale_owns_holding(net_quantity["005930"], 6));   // 수동으로 일부 판 뒤라 잔고가 더 적어도 맡는다
    CHECK(!devscale_owns_holding(0, 10));
    CHECK(!devscale_owns_holding(10, 0));
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

int test_entry_time_closed()
{
    CHECK(!entry_time_closed(1519, 1520));
    CHECK(entry_time_closed(1520, 1520));   // 경계 포함
    CHECK(entry_time_closed(1530, 1520));   // 09-29 15:30:10 마감 체결 뒤 재구성
    CHECK(!entry_time_closed(1600, 1940));  // 애프터마켓까지 사는 계좌
    CHECK(entry_time_closed(1945, 1940));
    CHECK(!entry_time_closed(1530, 0));     // 끔
    return 0;
}

int test_is_dust()
{
    // 먼지 기준 25만원. 12만원어치(10주 × 12,000원) 보유
    CHECK(is_dust(10, 12000.0, 250000.0, /*entered_today=*/false, 10));  // 전날 넘어온 자투리
    CHECK(!is_dust(10, 12000.0, 250000.0, /*entered_today=*/true, 10));  // 오늘 분할 첫 회차 체결(09-29 8건)
    CHECK(is_dust(4, 12000.0, 250000.0, /*entered_today=*/true, 10));    // 오늘 샀지만 익절로 줄어든 잔량
    CHECK(!is_dust(30, 12000.0, 250000.0, false, 30));                   // 36만원 — 기준 위
    CHECK(!is_dust(10, 12000.0, 0.0, false, 10));                        // 끔
    CHECK(!is_dust(0, 12000.0, 250000.0, false, 0));                     // 보유 없음
    CHECK(!is_dust(10, 0.0, 250000.0, false, 10));                       // 가격 모름
    return 0;
}

int test_zone_band()
{
    // entry_upper 5, pullback 8, hysteresis 4 (config_dev_paper.json 값)
    const ZoneBand entry = zone_band(5.0, 8.0, 4.0, /*widened=*/false);
    CHECK(entry.low_percent == -8.0 && entry.up_percent == 5.0);
    const ZoneBand hold = zone_band(5.0, 8.0, 4.0, /*widened=*/true); // 재기동 직후 보유분도 이 폭(011790 이격 6.3%)
    CHECK(hold.low_percent == -12.0 && hold.up_percent == 9.0);
    CHECK(6.3 <= hold.up_percent && 6.3 > entry.up_percent);
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

    if (test_net_quantity_sold_out() != 0)
    {
        return 1;
    }

    if (test_net_quantity_ignores_manual() != 0)
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

    if (test_entry_time_closed() != 0)
    {
        return 1;
    }

    if (test_is_dust() != 0)
    {
        return 1;
    }

    if (test_zone_band() != 0)
    {
        return 1;
    }

    std::cout << "test_devscale_rules: " << g_checks << " checks passed\n";
    return 0;
}
