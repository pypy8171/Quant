#include "strategy/DevScaleRules.h"

#include <charconv>
#include <system_error>

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

void add_net_quantity_from_ledger(std::istream& ledger, const std::string& id_prefix,
                                  std::map<std::string, long long>& net_quantity)
{
    // 열 위치 — Quant/src/ipc/OrderJournal.cpp kTradeHeader. 열은 끝에만 붙이므로 앞 11칸은 움직이지 않는다.
    constexpr size_t kEventColumn        = 1;
    constexpr size_t kStrategyColumn     = 4;
    constexpr size_t kTickerColumn       = 5;
    constexpr size_t kSideColumn         = 6;
    constexpr size_t kFillQuantityColumn = 10;
    constexpr size_t kColumnsNeeded      = kFillQuantityColumn + 1;

    const std::string strategy_prefix = id_prefix + "_";
    std::string       line;
    std::vector<std::string_view> fields;

    while (std::getline(ledger, line))
    {
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
            continue;
        }

        const std::string_view side = fields[kSideColumn];
        const std::string_view quantity_text = fields[kFillQuantityColumn];
        long long quantity = 0;
        const auto [end, error] = std::from_chars(quantity_text.data(), quantity_text.data() + quantity_text.size(), quantity);

        if (error != std::errc{} || end != quantity_text.data() + quantity_text.size() || quantity <= 0 ||
            (side != "BUY" && side != "SELL"))
        {
            continue;
        }

        long long& net = net_quantity[std::string(fields[kTickerColumn])];
        net += side == "BUY" ? quantity : -quantity;
    }
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

} // namespace devscale_rules
