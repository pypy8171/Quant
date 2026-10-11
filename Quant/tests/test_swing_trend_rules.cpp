// 스윙 추세 슬리브 순수 판정(strategy/SwingTrendRules.h) 단위 테스트 — 계획 파일 검증, 시가 대비 매수 여부와 수량,
//  확정 저점(같은 값 인정·0 저가 건너뜀)과 손절선 올리기(확정 전·매수 전 저점은 안 씀, 내려가지 않음), 매도 창 경계,
//  계획 날짜(오래됨) 경계를 고정한다. 파일·로그 없이 돈다. 관련 결정: D-160.
// 빌드: cmake --build <directory> --target test_swing_trend_rules
#include "strategy/SwingTrendRules.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace swing_trend;
using json = nlohmann::json;

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        ++g_checks;                                                                             \
        if (!(condition))                                                                       \
        {                                                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

int test_should_buy()
{
    CHECK(should_buy(10100.0, 10000));   // 시가가 매수가 위 — 되돌림을 기다린다
    CHECK(!should_buy(10000.0, 10000));  // 같은 값은 사지 않는다
    CHECK(!should_buy(9900.0, 10000));   // 아래에서 열렸다 — 그날은 사지 않는다
    CHECK(!should_buy(0.0, 10000));      // 시가 모름
    CHECK(!should_buy(10100.0, 0));      // 매수가 없음
    return 0;
}

int test_buy_quantity()
{
    CHECK(buy_quantity(500000, 10000) == 50);
    CHECK(buy_quantity(500000, 10001) == 49);  // 내림
    CHECK(buy_quantity(500000, 600000) == 0);  // 한 주도 못 산다
    CHECK(buy_quantity(500000, 0) == 0);
    CHECK(buy_quantity(0, 10000) == 0);
    return 0;
}

int test_is_extreme_low()
{
    const std::vector<int64_t> lows = {50, 40, 30, 40, 50};
    CHECK(is_extreme_low(lows, 2, 2));
    CHECK(!is_extreme_low(lows, 1, 1));

    const std::vector<int64_t> tie = {50, 30, 30, 50};
    CHECK(is_extreme_low(tie, 1, 1));  // 같은 값도 저점으로 인정
    CHECK(is_extreme_low(tie, 2, 1));

    const std::vector<int64_t> halted = {50, 0, 30, 40};
    CHECK(is_extreme_low(halted, 2, 1));   // 0 저가(거래정지)는 건너뛴다
    CHECK(!is_extreme_low(halted, 1, 1));  // 0 자체는 저점이 아니다
    CHECK(!is_extreme_low(lows, 9, 1));    // 범위 밖
    return 0;
}

int test_raised_stop()
{
    // 창 2. 저점 후보 위치 5(값 80)는 오른쪽 2봉(6·7)이 지나야 확정된다.
    std::vector<int64_t> lows = {100, 90, 95, 100, 98, 80, 85, 90};
    const size_t         entry_index = 4;  // 매수일 봉

    // 봉 0..7(T=8)까지면 후보 범위 [2, 5] — 위치 5의 80은 확정, 손절선 70 → 80
    CHECK(raised_stop(lows, entry_index, 2, 70) == 80);
    // 손절선이 더 높으면 내려가지 않는다
    CHECK(raised_stop(lows, entry_index, 2, 85) == 85);

    // 오른쪽 한 봉만 지났다(T=7) — 위치 5는 아직 확정 전
    const std::vector<int64_t> unconfirmed(lows.begin(), lows.begin() + 7);
    CHECK(raised_stop(unconfirmed, entry_index, 2, 70) == 70);

    // 매수 전에 확정된 저점은 쓰지 않는다 — 창 1, 위치 1(90)은 확정됐지만 후보 범위 [entry−1=3, 5] 밖
    const std::vector<int64_t> early = {100, 90, 95, 99, 98, 97, 96};
    CHECK(is_extreme_low(early, 1, 1));
    CHECK(raised_stop(early, 4, 1, 70) == 70);

    // 왼쪽 창이 모자란 위치는 후보가 아니다(후보 ≥ window)
    const std::vector<int64_t> left_short = {60, 100, 100, 100};
    CHECK(raised_stop(left_short, 0, 2, 50) == 50);

    // 매수일이 오늘(entry_index = T)이면 아직 오를 것이 없다 — 매수일 후보(T−2)는 T−1+…에야 확정
    CHECK(raised_stop(lows, lows.size(), 2, 70) == 70);
    CHECK(raised_stop(lows, entry_index, 0, 70) == 70);  // 창 0은 끔
    return 0;
}

int test_entry_bar_index()
{
    const std::vector<int32_t> dates = {20261005, 20261006, 20261007, 20261008};
    CHECK(entry_bar_index(dates, 20261007) == 2);
    CHECK(entry_bar_index(dates, 20261004) == 0);
    CHECK(entry_bar_index(dates, 20261012) == 4);  // 오늘 산 것 — 봉 끝
    CHECK(entry_bar_index({}, 20261012) == 0);
    return 0;
}

int test_should_sell()
{
    CHECK(!should_sell(1519, 9000.0, 10000, 1520, 1529));  // 창 앞
    CHECK(should_sell(1520, 9000.0, 10000, 1520, 1529));
    CHECK(should_sell(1529, 9999.0, 10000, 1520, 1529));
    CHECK(!should_sell(1530, 9000.0, 10000, 1520, 1529));   // 창 뒤
    CHECK(!should_sell(1525, 10000.0, 10000, 1520, 1529));  // 같은 값은 안 판다
    CHECK(!should_sell(1525, 0.0, 10000, 1520, 1529));      // 현재가 모름
    CHECK(!should_sell(1525, 9000.0, 0, 1520, 1529));       // 손절선 모름
    return 0;
}

int test_plan_stale()
{
    CHECK(!plan_stale(20261008, 20261012, 7));
    CHECK(!plan_stale(20261005, 20261012, 7));  // 딱 7일 — 아직 쓴다
    CHECK(plan_stale(20261004, 20261012, 7));   // 8일
    CHECK(plan_stale(20260930, 20261002, 1));   // 달 경계
    CHECK(plan_stale(0, 20261012, 7));          // as_of 못 읽음
    CHECK(plan_stale(20261399, 20261012, 7));   // 없는 날짜
    return 0;
}

int test_parse_plan()
{
    std::string reason;
    const json  good = json::parse(R"({"as_of":"2026-10-08","generated_at":"2026-10-08T20:00:00",
        "entries":[{"ticker":"005930","name":"삼성전자","entry_price":61000,"cancel_below":58000,"signal_date":"2026-10-07"},
                   {"ticker":"000660","entry_price":200000,"cancel_below":190000}]})");
    const auto plan = parse_plan(good, reason);
    CHECK(plan.has_value());
    CHECK(plan->as_of == "2026-10-08");
    CHECK(plan->entries.size() == 2);
    CHECK(plan->entries[0].entry_price == 61000);
    CHECK(plan->entries[0].cancel_below == 58000);
    CHECK(plan->entries[0].signal_date == "2026-10-07");
    CHECK(plan->entries[1].name.empty());

    const json empty_entries = json::parse(R"({"as_of":"2026-10-08","entries":[]})");
    CHECK(parse_plan(empty_entries, reason).has_value());

    const char* const broken[] = {
        R"([])",
        R"({"entries":[]})",
        R"({"as_of":"어제","entries":[]})",
        R"({"as_of":"2026-10-08"})",
        R"({"as_of":"2026-10-08","entries":[{"ticker":"5930","entry_price":100,"cancel_below":90}]})",
        R"({"as_of":"2026-10-08","entries":[{"ticker":"005930","entry_price":100.5,"cancel_below":90}]})",
        R"({"as_of":"2026-10-08","entries":[{"ticker":"005930","entry_price":100,"cancel_below":100}]})",
        R"({"as_of":"2026-10-08","entries":[{"ticker":"005930","entry_price":100,"cancel_below":0}]})",
        R"({"as_of":"2026-10-08","entries":[{"ticker":"005930","entry_price":100,"cancel_below":90},
                                            {"ticker":"005930","entry_price":200,"cancel_below":190}]})",
    };

    for (const char* text : broken)
    {
        reason.clear();
        CHECK(!parse_plan(json::parse(text), reason).has_value());
        CHECK(!reason.empty());
    }

    return 0;
}
} // namespace

int main()
{
    if (test_should_buy() != 0 || test_buy_quantity() != 0 || test_is_extreme_low() != 0 || test_raised_stop() != 0 ||
        test_entry_bar_index() != 0 || test_should_sell() != 0 || test_plan_stale() != 0 || test_parse_plan() != 0)
    {
        return 1;
    }

    std::cout << "test_swing_trend_rules: " << g_checks << " checks passed\n";
    return 0;
}
