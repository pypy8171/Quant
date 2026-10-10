// 강한 종목 첫 VWAP 눌림 순수 판정(strategy/VwapPullbackRules.h) 단위 테스트 — 09:30 선정(등락률 띠·거래대금 순·제외),
//  1분 저장 파일에서 선정 시각 판만 읽기, 되밀림·VWAP 밴드·돌파·손절가·시간 청산 경계, 상태 기계의 무장·신호·무장 해제,
//  재인수 판정과 자기 몫 매도 수량을 고정한다. 파일·로그 없이 돈다.
//  관련 결정: D-109(슬리브 소유권).
// 빌드: cmake --build <directory> --target test_vwap_pullback_rules
#include "strategy/VwapPullbackRules.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace vwap_pullback;

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

bool near(double left, double right)
{
    return std::fabs(left - right) < 1e-6;
}

BoardRow row(const std::string& code, double price, double change_percent, double value, bool excluded = false)
{
    BoardRow board_row;
    board_row.code           = code;
    board_row.price          = price;
    board_row.change_percent = change_percent;
    board_row.value          = value;
    board_row.excluded       = excluded;
    return board_row;
}

int select_top_k_by_turnover_in_change_band()
{
    RuleParams rules;
    rules.turnover_top_n = 2;
    const std::vector<BoardRow> rows = {
        row("A", 10800, 8.0, 9e9),
        row("D", 10500, 5.0, 30e9),  // 등락률 하한 아래
        row("E", 10700, 7.0, 9e9),   // A와 거래대금이 같다 — 코드 순
        row("F", 11200, 12.0, 2e9),  // 거래대금 30억 아래
        row("G", 1650, 10.0, 10e9),  // 전일 종가 1,500원 — 2,000원 아래
    };
    const std::vector<Selected> picked = select_candidates(rows, rules);
    CHECK(picked.size() == 2);
    CHECK(picked[0].code == "A");
    CHECK(picked[1].code == "E");
    CHECK(near(picked[0].previous_close, 10000.0));

    rules.turnover_top_n = 10;
    CHECK(select_candidates(rows, rules).size() == 2); // 띠·하한을 넘는 것은 둘뿐
    return 0;
}

int excludes_over_20_and_vi()
{
    RuleParams rules;
    const std::vector<BoardRow> rows = {
        row("B", 12500, 25.0, 20e9),       // +20% 초과(상한가 근처)
        row("C", 11000, 10.0, 5e9, true),  // 호출자가 VI·ETF로 표시
        row("H", 12000, 20.0, 4e9),        // 경계 +20%는 든다
    };
    const std::vector<Selected> picked = select_candidates(rows, rules);
    CHECK(picked.size() == 1);
    CHECK(picked[0].code == "H");
    return 0;
}

int selection_uses_0930_row_not_now()
{
    std::istringstream csv(
        "# 주석\n"
        "kst_time,code,price,volume,turnover,change_pct,market_cap,board_count,failed_batches,batches\n"
        "09:29:58,A,10800,100,9000000000,8.00,1,3,0,1\n"
        "09:30:03,A,10800,100,9000000000,8.00,1,3,0,1\r\n"
        "09:30:03,B,11000,100,5000000000,10.00,1,3,0,1\n"
        "09:31:02,Z,11000,100,99000000000,10.00,1,3,0,1\n"); // 지금 판에만 있는 종목 — 들어오면 안 된다
    const std::vector<BoardRow> rows = rows_at_selection(csv, 930);
    CHECK(rows.size() == 2);
    CHECK(rows[0].code == "A");
    CHECK(rows[1].code == "B");
    CHECK(near(rows[1].change_percent, 10.0));
    CHECK(near(rows[0].value, 9e9));

    std::istringstream early("09:10:00,A,10800,100,9000000000,8.00,1,3,0,1\n");
    CHECK(rows_at_selection(early, 930).empty()); // 09:30 판이 아직 없으면 고르지 않는다
    return 0;
}

int retrace_ratio_bounds()
{
    CHECK(near(retrace_ratio(11000, 10500, 10000), 0.5));
    CHECK(near(retrace_ratio(11000, 10620, 10000), 0.38));
    CHECK(near(retrace_ratio(11000, 10380, 10000), 0.62));
    CHECK(retrace_ratio(10000, 9900, 10000) < 0.0); // 고점이 전일 종가 이하 — 정의 없음
    return 0;
}

int vwap_band_half_percent()
{
    CHECK(in_vwap_band(10050, 10000, 0.5));
    CHECK(in_vwap_band(9950, 10000, 0.5));
    CHECK(!in_vwap_band(10051, 10000, 0.5));
    CHECK(!in_vwap_band(9949, 10000, 0.5));
    CHECK(!in_vwap_band(10000, 0.0, 0.5));
    return 0;
}

int breakout_trigger_previous_minute_high()
{
    CHECK(breakout_trigger(10620, 10600, 10533));
    CHECK(!breakout_trigger(10600, 10600, 10533)); // 같으면 아니다
    CHECK(!breakout_trigger(10620, 10600, 10700)); // VWAP 아래
    CHECK(!breakout_trigger(10620, 0.0, 10533));   // 직전 봉 없음
    return 0;
}

int stop_price_low_minus_0_3()
{
    RuleParams rules;
    CHECK(near(stop_price(10000, 10100, rules), 9970));  // 10,000 × 0.997 = 9,970, 폭 1.29%
    CHECK(near(stop_price(10050, 10060, rules), 9990));  // 폭 0.5% → 진입 × 0.994 = 9,999.64 → 틱 내림 9,990
    CHECK(near(stop_price(10000, 10400, rules), 0.0));   // 폭 4.1% — 진입하지 않는다
    CHECK(near(stop_price(50100, 51000, rules), 49900)); // 49,949.7은 5만 원 미만 구간이라 50원 틱 → 49,900
    return 0;
}

int exit_due_1510()
{
    CHECK(!exit_due(1509, 1510));
    CHECK(exit_due(1510, 1510));
    CHECK(exit_due(1520, 1510));
    return 0;
}

int traded_today_blocks_second_entry()
{
    std::set<std::string> entered;
    CHECK(!traded_today(entered, "005930"));
    entered.insert("005930");
    CHECK(traded_today(entered, "005930"));
    CHECK(!traded_today(entered, "000660"));
    return 0;
}

int owns_full()
{
    CHECK(classify_holding(10, 10) == Ownership::Full);
    CHECK(classify_holding(12, 10) == Ownership::Full);
    return 0;
}

int owns_partial_hands_off()
{
    CHECK(classify_holding(6, 10) == Ownership::Partial);
    CHECK(own_exit_quantity(10, 6) == 6); // 장부 10, 자기 6 → 6만 판다
    CHECK(own_exit_quantity(5, 6) == 5);  // 확정 잔고가 더 적으면 잔고까지
    return 0;
}

int owns_none()
{
    CHECK(classify_holding(0, 10) == Ownership::None);
    CHECK(classify_holding(-3, 10) == Ownership::None);
    CHECK(classify_holding(5, 0) == Ownership::None);
    CHECK(own_exit_quantity(10, 0) == 0);
    return 0;
}

// 09:00 급등 봉(고가 11,000) 뒤 10,550 근처에서 30분 거래가 붙어 VWAP이 10,533 근처로 내려온 판.
PullbackMachine warmed_machine(const RuleParams& rules = RuleParams{})
{
    PullbackMachine machine(10000.0, rules);
    machine.on_bar(MinuteBar{900, 10000, 11000, 10000, 10600, 100});

    for (int minute = 1; minute <= 30; ++minute)
    {
        machine.on_bar(MinuteBar{900 + minute, 10550, 10600, 10450, 10550, 1000});
    }

    return machine;
}

int machine_arms_then_signals()
{
    PullbackMachine machine = warmed_machine();
    CHECK(machine.phase() == Phase::Waiting); // 09:30 봉까지는 무장 조건을 보지 않는다

    const StepResult armed = machine.on_bar(MinuteBar{931, 10550, 10600, 10500, 10520, 1000});
    CHECK(armed.event == Event::Armed);
    CHECK(near(armed.retrace, 0.5));
    CHECK(near(armed.pullback_low, 10500));

    const StepResult signal = machine.on_bar(MinuteBar{932, 10520, 10650, 10510, 10620, 1000});
    CHECK(signal.event == Event::Signal);
    CHECK(near(signal.entry_price, 10640)); // 10,620 + 2틱(10원)
    CHECK(near(signal.stop, 10460));        // 10,500 × 0.997 = 10,468.5 → 10,460
    CHECK(machine.phase() == Phase::Done);

    const StepResult after = machine.on_bar(MinuteBar{933, 10620, 10700, 10600, 10690, 1000});
    CHECK(after.event == Event::None); // 첫 눌림만 — 다시 신호를 내지 않는다
    return 0;
}

int machine_disarms()
{
    {
        PullbackMachine machine = warmed_machine();
        machine.on_bar(MinuteBar{931, 10550, 10600, 10500, 10520, 1000});
        const StepResult result = machine.on_bar(MinuteBar{932, 10520, 11050, 10510, 10900, 1000});
        CHECK(result.event == Event::Disarmed);
        CHECK(result.reason == "new_high");
    }

    {
        PullbackMachine machine = warmed_machine();
        machine.on_bar(MinuteBar{931, 10550, 10600, 10500, 10520, 1000});
        StepResult result;

        for (int minute = 0; minute < 15; ++minute)
        {
            result = machine.on_bar(MinuteBar{932 + minute, 10520, 10540, 10500, 10520, 1000});
        }

        CHECK(result.event == Event::Disarmed);
        CHECK(result.reason == "timeout");
    }

    {
        PullbackMachine machine = warmed_machine();
        machine.on_bar(MinuteBar{931, 10550, 10600, 10500, 10520, 1000});
        const StepResult result = machine.on_bar(MinuteBar{932, 10520, 10530, 10300, 10380, 1000});
        CHECK(result.event == Event::Disarmed);
        CHECK(result.reason == "below_vwap"); // 10,380 < VWAP × 0.99
    }

    {
        PullbackMachine machine = warmed_machine();
        machine.on_bar(MinuteBar{931, 10550, 10600, 10500, 10520, 0});
        const StepResult result = machine.on_bar(MinuteBar{932, 10520, 10520, 10520, 10520, 0});
        CHECK(result.event == Event::Disarmed);
        CHECK(result.reason == "vi"); // 거래량 0 봉 두 개 연속
    }

    {
        RuleParams rules;
        rules.no_new_entry_hhmm = 930;
        PullbackMachine  machine = warmed_machine(rules);
        const StepResult result  = machine.on_bar(MinuteBar{931, 10550, 10600, 10500, 10520, 1000});
        CHECK(result.event == Event::Disarmed);
        CHECK(result.reason == "late");
    }

    {
        PullbackMachine machine = warmed_machine();
        const StepResult repeat = machine.on_bar(MinuteBar{930, 10550, 10600, 10500, 10520, 1000});
        CHECK(repeat.event == Event::None); // 같은 분은 다시 넣어도 무시
        CHECK(machine.last_hhmm() == 930);
    }

    return 0;
}
} // namespace

int main()
{
    int failed = 0;
    failed |= select_top_k_by_turnover_in_change_band();
    failed |= excludes_over_20_and_vi();
    failed |= selection_uses_0930_row_not_now();
    failed |= retrace_ratio_bounds();
    failed |= vwap_band_half_percent();
    failed |= breakout_trigger_previous_minute_high();
    failed |= stop_price_low_minus_0_3();
    failed |= exit_due_1510();
    failed |= traded_today_blocks_second_entry();
    failed |= owns_full();
    failed |= owns_partial_hands_off();
    failed |= owns_none();
    failed |= machine_arms_then_signals();
    failed |= machine_disarms();

    if (failed != 0)
    {
        return 1;
    }

    std::cout << "test_vwap_pullback_rules: " << g_checks << " checks passed\n";
    return 0;
}
