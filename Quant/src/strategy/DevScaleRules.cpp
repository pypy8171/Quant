#include "strategy/DevScaleRules.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace devscale_rules
{
bool peak_trail_triggered(double peak, double average, double current, double arm_percent, double trail_percent)
{
    if (arm_percent <= 0.0 || average <= 0.0 || peak <= 0.0)
    {
        return false;
    }

    const bool armed = peak >= average * (1.0 + arm_percent / 100.0);

    return armed && current <= peak * (1.0 - trail_percent / 100.0);
}

namespace
{
struct LedgerFill
{
    std::string_view ticker;
    long long        signed_quantity = 0; // 매수 +, 매도 −
};

// 장부 한 행이 이 슬리브의 체결 행이면 fill을 채우고 true.
bool parse_ledger_fill(std::string_view line, const std::string& strategy_prefix, std::vector<std::string_view>& fields,
                       LedgerFill& fill)
{
    // 열 위치 — Quant/src/ipc/OrderJournal.cpp kTradeHeader. 열은 끝에만 붙이므로 앞 11칸은 움직이지 않는다.
    constexpr size_t kEventColumn        = 1;
    constexpr size_t kStrategyColumn     = 4;
    constexpr size_t kTickerColumn       = 5;
    constexpr size_t kSideColumn         = 6;
    constexpr size_t kFillQuantityColumn = 10;
    constexpr size_t kColumnsNeeded      = kFillQuantityColumn + 1;

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

    if (fields.size() < kColumnsNeeded || fields[kEventColumn] != "FILL" ||
        !fields[kStrategyColumn].starts_with(strategy_prefix))
    {
        return false;
    }

    const std::string_view side = fields[kSideColumn];
    const std::string_view quantity_text = fields[kFillQuantityColumn];
    long long quantity = 0;
    const auto [end, error] = std::from_chars(quantity_text.data(), quantity_text.data() + quantity_text.size(), quantity);

    if (error != std::errc{} || end != quantity_text.data() + quantity_text.size() || quantity <= 0 ||
        (side != "BUY" && side != "SELL"))
    {
        return false;
    }

    fill.ticker = fields[kTickerColumn];
    fill.signed_quantity = side == "BUY" ? quantity : -quantity;
    return true;
}

double close_average(const std::vector<MarketData>& daily, size_t first, int period)
{
    double sum = 0.0;

    for (size_t index = first; index < first + static_cast<size_t>(period); ++index)
    {
        sum += daily[index].close;
    }

    return sum / period;
}
} // namespace

void add_net_quantity_from_ledger(std::istream& ledger, const std::string& id_prefix,
                                  std::map<std::string, long long>& net_quantity)
{
    const std::string strategy_prefix = id_prefix + "_";
    std::string       line;
    std::vector<std::string_view> fields;
    LedgerFill        fill;

    while (std::getline(ledger, line))
    {
        if (parse_ledger_fill(line, strategy_prefix, fields, fill))
        {
            net_quantity[std::string(fill.ticker)] += fill.signed_quantity;
        }
    }
}

void add_holding_from_ledger(std::istream& ledger, const std::string& id_prefix, int session_date,
                             std::map<std::string, LedgerHolding>& holdings)
{
    const std::string strategy_prefix = id_prefix + "_";
    std::string       line;
    std::vector<std::string_view> fields;
    LedgerFill        fill;

    while (std::getline(ledger, line))
    {
        if (!parse_ledger_fill(line, strategy_prefix, fields, fill))
        {
            continue;
        }

        LedgerHolding& holding = holdings[std::string(fill.ticker)];
        const bool was_flat = holding.net_quantity <= 0;
        holding.net_quantity += fill.signed_quantity;

        if (was_flat && holding.net_quantity > 0)
        {
            holding.entry_date = session_date;
        }
    }
}

TrendExitJudgement judge_trend_exit(const std::vector<MarketData>& daily, int period, int entry_date)
{
    TrendExitJudgement judgement;

    if (period <= 0 || daily.size() < static_cast<size_t>(period) + 1 || daily[0].session_date == 0 ||
        daily[0].session_date <= entry_date)
    {
        return judgement;
    }

    judgement.close = daily[0].close;
    judgement.average = close_average(daily, 0, period);

    for (size_t index = 1; index + static_cast<size_t>(period) <= daily.size(); ++index)
    {
        if (daily[index].session_date < entry_date)
        {
            break; // 최신→과거 순이라 이 뒤는 전부 매수 전이다
        }

        if (daily[index].close > close_average(daily, index, period))
        {
            judgement.armed = true;
            break;
        }
    }

    judgement.sell = judgement.armed && judgement.average > 0.0 && judgement.close < judgement.average;
    return judgement;
}

bool devscale_owns_holding(long long net_quantity, int held_quantity)
{
    return held_quantity > 0 && net_quantity > 0 && net_quantity >= held_quantity;
}

double average_true_range(const std::vector<MarketData>& daily, int period)
{
    if (period <= 0 || daily.size() < static_cast<size_t>(period) + 1)
    {
        return 0.0;
    }

    double sum = 0.0;

    for (int index = 0; index < period; ++index)
    {
        const MarketData& bar = daily[static_cast<size_t>(index)];
        const double previous_close = daily[static_cast<size_t>(index) + 1].close;
        const double true_range =
            (std::max)({bar.high - bar.low, std::abs(bar.high - previous_close), std::abs(bar.low - previous_close)});
        sum += true_range;
    }

    return sum / period;
}

bool entry_day_allowed(double atr_percent, double open_deviation_percent, double atr_max_percent,
                       double open_deviation_min_percent, double open_deviation_max_percent)
{
    const bool atr_ok = atr_max_percent <= 0.0 || atr_percent <= atr_max_percent;
    const bool deviation_ok =
        open_deviation_min_percent <= open_deviation_percent && open_deviation_percent <= open_deviation_max_percent;

    return atr_ok && deviation_ok;
}

bool entry_time_closed(int hhmm, int no_new_entry_hhmm)
{
    return no_new_entry_hhmm > 0 && hhmm >= no_new_entry_hhmm;
}

bool is_dust(int position, double current_price, double dust_krw, bool entered_today, int peak_position)
{
    if (position <= 0 || dust_krw <= 0.0 || current_price <= 0.0 || position * current_price >= dust_krw)
    {
        return false;
    }

    const bool entry_in_progress = entered_today && position >= peak_position;

    return !entry_in_progress;
}

ZoneBand zone_band(double entry_upper_percent, double pullback_percent, double hysteresis_percent, bool widened)
{
    const double extra = widened ? hysteresis_percent : 0.0;
    ZoneBand band;
    band.low_percent = -(pullback_percent + extra);
    band.up_percent = entry_upper_percent + extra;
    return band;
}

int sell_room_after_cancel(int sellable, int cancelled_sell_quantity, int position)
{
    const int room = (std::max)(sellable, 0) + (std::max)(cancelled_sell_quantity, 0);
    return std::clamp(room, 0, (std::max)(position, 0));
}

bool sell_cover_missing(int position, int planned_sell_quantity, int free_sellable, long long seconds_since_rebuild,
                        int grace_sec)
{
    if (position <= 0 || planned_sell_quantity <= 0 || seconds_since_rebuild < grace_sec)
    {
        return false;
    }

    const int left_uncovered = position - (std::min)(planned_sell_quantity, position);
    return free_sellable > left_uncovered;
}

} // namespace devscale_rules
