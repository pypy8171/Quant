#include "api/KisClient.h"

namespace
{
constexpr long kSecondsPerHour = 3600;
} // namespace

const char* kis_order_exchange(const KisConfig& config) noexcept
{
    if (config.is_paper || config.exchange == "KRX")
    {
        return "KRX";
    }

    return config.exchange == "NXT" ? "NXT" : "SOR";
}

std::string kis_hhmmss_minus_minutes(const std::string& hhmmss, int minutes)
{
    if (hhmmss.size() != 6)
    {
        return "";
    }

    for (char character : hhmmss)
    {
        if (character < '0' || character > '9')
        {
            return "";
        }
    }

    const int hour = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
    const int minute = (hhmmss[2] - '0') * 10 + (hhmmss[3] - '0');
    const int second = (hhmmss[4] - '0') * 10 + (hhmmss[5] - '0');

    if (hour > 23 || minute > 59 || second > 59)
    {
        return "";
    }

    const long total = static_cast<long>(hour) * kSecondsPerHour + minute * 60 + second - static_cast<long>(minutes) * 60;

    if (total < 0)
    {
        return "";
    }

    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02ld%02ld%02ld", total / kSecondsPerHour, (total / 60) % 60, total % 60);
    return buffer;
}
