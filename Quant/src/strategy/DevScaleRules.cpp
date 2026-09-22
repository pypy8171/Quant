#include "strategy/DevScaleRules.h"

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

std::set<std::string> tickers_bought_from_ledger(std::istream& ledger, const std::string& id_prefix)
{
    std::set<std::string> bought;
    const std::string strategy_prefix = id_prefix + "_";
    std::string line;

    while (std::getline(ledger, line))
    {
        std::vector<std::string_view> fields;
        std::string_view rest = line;

        while (fields.size() < 7)
        {
            const size_t comma = rest.find(',');
            fields.push_back(rest.substr(0, comma));

            if (comma == std::string_view::npos)
            {
                break;
            }

            rest.remove_prefix(comma + 1);
        }

        if (fields.size() < 7 || fields[1] != "FILL" || fields[6] != "BUY" || !fields[4].starts_with(strategy_prefix))
        {
            continue;
        }

        bought.emplace(fields[5]);
    }

    return bought;
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

} // namespace devscale_rules
