#include "strategy/VwapPullbackRules.h"

#include "core/TickSize.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

namespace vwap_pullback
{
namespace
{
constexpr double kPercent = 100.0;
// 퍼센트 경계 비교 여유 — 9,950/10,000처럼 딱 경계인 값이 부동소수 오차(0.5000000000000004%)로 밖에 떨어지지 않게 한다.
constexpr double kPercentTolerance = 1e-9;

// "HH:MM:SS" → HHMMSS 정수. 모양이 다르면 −1.
int parse_clock(std::string_view text)
{
    constexpr size_t kClockLength = 8;

    if (text.size() != kClockLength || text[2] != ':' || text[5] != ':')
    {
        return -1;
    }

    int hours   = 0;
    int minutes = 0;
    int seconds = 0;
    std::from_chars(text.data(), text.data() + 2, hours);
    std::from_chars(text.data() + 3, text.data() + 5, minutes);
    std::from_chars(text.data() + 6, text.data() + 8, seconds);
    constexpr int kShift = 100;
    return (hours * kShift + minutes) * kShift + seconds;
}

double parse_number(std::string_view text)
{
    double value = 0.0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

// 가격 자신의 호가단위로 n틱 올린다.
double add_ticks(double price, int ticks)
{
    for (int step = 0; step < ticks; ++step)
    {
        price += krx::tick_size(price);
    }

    return price;
}
} // namespace

double previous_close(double price, double change_percent)
{
    const double ratio = 1.0 + change_percent / kPercent;

    if (ratio <= 0.0)
    {
        return 0.0;
    }

    return price / ratio;
}

std::vector<Selected> select_candidates(const std::vector<BoardRow>& rows, const RuleParams& rules)
{
    std::vector<Selected> passed;

    for (const BoardRow& row : rows)
    {
        if (row.excluded || row.change_percent < rules.change_min_percent ||
            row.change_percent > rules.change_max_percent || row.value < rules.min_turnover_krw)
        {
            continue;
        }

        const double base = previous_close(row.price, row.change_percent);

        if (base < rules.min_previous_close_krw)
        {
            continue;
        }

        passed.push_back(Selected{row.code, row.name, base, row.change_percent, row.value});
    }

    std::sort(passed.begin(), passed.end(), [](const Selected& left, const Selected& right)
    {
        if (left.value != right.value)
        {
            return left.value > right.value;
        }

        return left.code < right.code;
    });

    if (rules.turnover_top_n >= 0 && passed.size() > static_cast<size_t>(rules.turnover_top_n))
    {
        passed.resize(static_cast<size_t>(rules.turnover_top_n));
    }

    return passed;
}

std::vector<BoardRow> rows_at_selection(std::istream& board_csv, int select_hhmm)
{
    // 열 위치 — Quant/src/universe/MarketBoard.cpp board_minute_header.
    constexpr size_t kTimeColumn    = 0;
    constexpr size_t kCodeColumn    = 1;
    constexpr size_t kPriceColumn   = 2;
    constexpr size_t kVolumeColumn  = 3;
    constexpr size_t kValueColumn   = 4;
    constexpr size_t kChangeColumn  = 5;
    constexpr size_t kColumnsNeeded = kChangeColumn + 1;
    constexpr int    kSecondsShift  = 100;

    const int target_clock = select_hhmm * kSecondsShift;
    int       chosen_clock = -1;
    std::vector<BoardRow>         rows;
    std::string                   line;
    std::vector<std::string_view> fields;

    while (std::getline(board_csv, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        fields.clear();
        std::string_view rest = line;

        while (fields.size() < kColumnsNeeded)
        {
            const size_t comma = rest.find(',');
            fields.push_back(rest.substr(0, comma));

            if (comma == std::string_view::npos)
            {
                break;
            }

            rest.remove_prefix(comma + 1);
        }

        if (fields.size() < kColumnsNeeded)
        {
            continue; // 주석·짧은 행
        }

        const int clock = parse_clock(fields[kTimeColumn]);

        if (clock < target_clock)
        {
            continue; // 헤더(-1)와 선정 시각 앞 판
        }

        if (chosen_clock < 0)
        {
            chosen_clock = clock;
        }

        if (clock != chosen_clock)
        {
            break; // 다음 판 — 읽지 않는다
        }

        BoardRow row;
        row.code           = std::string(fields[kCodeColumn]);
        row.price          = parse_number(fields[kPriceColumn]);
        row.volume         = parse_number(fields[kVolumeColumn]);
        row.value          = parse_number(fields[kValueColumn]);
        row.change_percent = parse_number(fields[kChangeColumn]);
        rows.push_back(std::move(row));
    }

    return rows;
}

double retrace_ratio(double session_high, double low, double previous_close_price)
{
    const double run = session_high - previous_close_price;

    if (run <= 0.0)
    {
        return -1.0;
    }

    return (session_high - low) / run;
}

bool in_vwap_band(double low, double vwap, double band_percent)
{
    if (vwap <= 0.0)
    {
        return false;
    }

    return std::fabs(low / vwap - 1.0) * kPercent <= band_percent + kPercentTolerance;
}

bool breakout_trigger(double close, double previous_high, double vwap)
{
    return previous_high > 0.0 && close > previous_high && close > vwap;
}

double floor_to_tick(double price)
{
    if (price <= 0.0)
    {
        return 0.0;
    }

    const double tick = krx::tick_size(price);
    return std::floor(price / tick) * tick;
}

double stop_price(double pullback_low, double entry_price, const RuleParams& rules)
{
    if (pullback_low <= 0.0 || entry_price <= 0.0)
    {
        return 0.0;
    }

    double       stop  = floor_to_tick(pullback_low * (1.0 - rules.stop_below_low_percent / kPercent));
    const double width = (entry_price - stop) / entry_price * kPercent;

    if (width > rules.max_stop_width_percent + kPercentTolerance)
    {
        return 0.0;
    }

    if (width < rules.min_stop_width_percent - kPercentTolerance)
    {
        stop = floor_to_tick(entry_price * (1.0 - rules.min_stop_width_percent / kPercent));
    }

    return stop;
}

bool exit_due(int hhmm, int exit_hhmm)
{
    return hhmm >= exit_hhmm;
}

bool traded_today(const std::set<std::string>& entered_today, const std::string& ticker)
{
    return entered_today.count(ticker) > 0;
}

Ownership classify_holding(long long net_quantity, int held_quantity)
{
    if (net_quantity <= 0 || held_quantity <= 0)
    {
        return Ownership::None;
    }

    return net_quantity >= held_quantity ? Ownership::Full : Ownership::Partial;
}

int own_exit_quantity(int confirmed_quantity, long long own_net_quantity)
{
    if (confirmed_quantity <= 0 || own_net_quantity <= 0)
    {
        return 0;
    }

    return static_cast<int>(std::min<long long>(confirmed_quantity, own_net_quantity));
}

PullbackMachine::PullbackMachine(double previous_close_price, RuleParams rules)
    : previous_close_(previous_close_price)
    , rules_(rules)
{
}

double PullbackMachine::vwap() const
{
    return volume_sum_ > 0.0 ? price_volume_sum_ / volume_sum_ : 0.0;
}

StepResult PullbackMachine::disarm(StepResult result, const char* reason)
{
    phase_        = Phase::Done;
    result.event  = Event::Disarmed;
    result.reason = reason;
    return result;
}

StepResult PullbackMachine::on_bar(const MinuteBar& bar)
{
    StepResult result;

    if (bar.hhmm <= last_hhmm_)
    {
        return result;
    }

    last_hhmm_ = bar.hhmm;

    constexpr double kTypicalDivisor = 3.0;
    const double     typical         = (bar.high + bar.low + bar.close) / kTypicalDivisor;
    price_volume_sum_ += typical * static_cast<double>(bar.volume);
    volume_sum_       += static_cast<double>(bar.volume);
    session_high_      = std::max(session_high_, bar.high);

    result.vwap         = vwap() > 0.0 ? vwap() : bar.close;
    result.session_high = session_high_;
    result.retrace      = retrace_ratio(session_high_, bar.low, previous_close_);

    // VI 근사(SPEC 2.3-5): 09:01 봉부터 거래량 0 봉이 이어지거나 한 봉 변동이 크면 그날 제외. 라이브 대체는 KIS
    //  변동성완화장치(VI) 현황 조회(/uapi/domestic-stock/v1/quotations/inquire-vi-status, TR FHPST01390000 —
    //  2026-10-01 MCP kis-code-assistant 확인)로 하되, 백테스트와 같은 규칙을 먼저 둔다.
    constexpr int kViCheckFromHhmm = 901;
    bool          vi_suspected     = false;

    if (bar.hhmm >= kViCheckFromHhmm)
    {
        zero_volume_run_ = bar.volume == 0 ? zero_volume_run_ + 1 : 0;
        const bool jumped = previous_close_bar_ > 0.0 &&
                            std::fabs(bar.close / previous_close_bar_ - 1.0) * kPercent >=
                            rules_.vi_jump_percent - kPercentTolerance;
        vi_suspected = zero_volume_run_ >= rules_.vi_zero_volume_bars || jumped;
    }

    const double previous_high = previous_high_;
    previous_high_             = bar.high;
    previous_close_bar_        = bar.close;

    if (phase_ == Phase::Done)
    {
        return result;
    }

    if (vi_suspected)
    {
        return disarm(result, "vi");
    }

    if (phase_ == Phase::Waiting)
    {
        if (bar.hhmm < rules_.first_arm_hhmm)
        {
            return result;
        }

        if (bar.hhmm > rules_.no_new_entry_hhmm)
        {
            return disarm(result, "late");
        }

        const bool retraced = result.retrace >= rules_.retrace_min && result.retrace <= rules_.retrace_max;
        const bool near_vwap = in_vwap_band(bar.low, result.vwap, rules_.vwap_band_percent);
        const bool not_overrun =
            previous_close_ > 0.0 && (session_high_ / previous_close_ - 1.0) * kPercent <=
                                                 rules_.max_run_percent + kPercentTolerance;

        if (retraced && near_vwap && not_overrun)
        {
            phase_              = Phase::Armed;
            armed_high_         = session_high_;
            pullback_low_       = bar.low;
            bars_since_arm_     = 0;
            result.event        = Event::Armed;
            result.pullback_low = pullback_low_;
        }

        return result;
    }

    // 무장 중
    ++bars_since_arm_;
    pullback_low_       = std::min(pullback_low_, bar.low);
    result.pullback_low = pullback_low_;

    if (bar.high > armed_high_)
    {
        return disarm(result, "new_high");
    }

    if (bar.close < result.vwap * (1.0 - rules_.disarm_below_vwap_percent / kPercent))
    {
        return disarm(result, "below_vwap");
    }

    if (result.retrace > rules_.disarm_retrace)
    {
        return disarm(result, "deep_retrace");
    }

    if (breakout_trigger(bar.close, previous_high, result.vwap))
    {
        const double entry = add_ticks(bar.close, rules_.entry_ticks);
        const double stop  = stop_price(pullback_low_, entry, rules_);

        if (stop <= 0.0)
        {
            return disarm(result, "stop_too_wide");
        }

        phase_             = Phase::Done;
        result.event       = Event::Signal;
        result.entry_price = entry;
        result.stop        = stop;
        return result;
    }

    if (bars_since_arm_ >= rules_.arm_timeout_bars)
    {
        return disarm(result, "timeout");
    }

    return result;
}
} // namespace vwap_pullback
