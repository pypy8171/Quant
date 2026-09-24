#include "core/Types.h"

uint64_t digits_to_number(std::string_view digits) noexcept
{
    if (digits.empty())
    {
        return 0;
    }

    uint64_t value = 0;

    for (const char character : digits)
    {
        if (character < '0' || character > '9')
        {
            return 0;
        }

        value = value * 10 + static_cast<uint64_t>(character - '0');
    }

    return value;
}

uint64_t next_client_order_number() noexcept
{
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

Regime Regime::from_string(std::string_view text)
{
    static constexpr std::pair<std::string_view, Value> kNames[] = {
        {"BULL", BULL},
        {"NEUTRAL", NEUTRAL},
        {"BEAR", BEAR},
    };

    for (const auto& [name, value] : kNames)
    {
        if (name == text)
        {
            return Regime(value);
        }
    }

    return Regime(UNKNOWN);
}

StrategyType StrategyType::from_string(std::string_view text)
{
    static constexpr std::pair<std::string_view, Value> kNames[] = {
        {"MA_CROSS", MA_CROSS},
        {"INTRADAY_BREAKOUT", INTRADAY_BREAKOUT},
        {"MOMENTUM", MOMENTUM},
        {"VALUE_CONTRARY", VALUE_CONTRARY},
        {"FIXED_INTERVAL", FIXED_INTERVAL},
        {"PRICE_TARGET", PRICE_TARGET},
        {"SUPPLY_DEMAND_PULLBACK", SUPPLY_DEMAND_PULLBACK},
        {"MARKET_MAKING", MARKET_MAKING},
        {"DEVIATION_SCALE", DEVIATION_SCALE},
        {"THEME", THEME},
        {"TARGET_BASKET", TARGET_BASKET},
    };

    for (const auto& [name, value] : kNames)
    {
        if (name == text)
        {
            return StrategyType(value);
        }
    }

    return StrategyType(UNKNOWN);
}
