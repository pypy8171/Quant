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
    // 음수 minutes는 시각을 앞으로 밀어 24시를 넘길 수 있다. 빼는 방향만 받는다.
    if (hhmmss.size() != 6 || minutes < 0)
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

    // [inv] 0 <= total <= 23:59:59라 세 칸 모두 두 자리다. snprintf 대신 자리를 직접 채워
    //  컴파일러가 long 범위로 잘림 경고(-Wformat-truncation)를 내지 않게 한다.
    const long parts[3] = {total / kSecondsPerHour, (total / 60) % 60, total % 60};
    std::string result(6, '0');

    for (std::size_t part_index = 0; part_index < 3; ++part_index)
    {
        result[part_index * 2] = static_cast<char>('0' + parts[part_index] / 10);
        result[part_index * 2 + 1] = static_cast<char>('0' + parts[part_index] % 10);
    }

    return result;
}
