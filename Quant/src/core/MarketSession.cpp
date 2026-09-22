#include "core/MarketSession.h"

namespace krx
{
int parse_hhmm(std::string_view ticker)
{
    if (ticker.size() < 4)
    {
        return 0;
    }

    for (int index = 0; index < 4; ++index)
    {
        if (ticker[index] < '0' || ticker[index] > '9')
        {
            return 0;
        }
    }

    return (ticker[0] - '0') * 1000 + (ticker[1] - '0') * 100 + (ticker[2] - '0') * 10 + (ticker[3] - '0');
}

int32_t parse_hhmmss(std::string_view ticker)
{
    if (ticker.size() < 6)
    {
        return 0;
    }

    int32_t value = 0;

    for (int index = 0; index < 6; ++index)
    {
        if (ticker[index] < '0' || ticker[index] > '9')
        {
            return 0;
        }

        value = value * 10 + (ticker[index] - '0');
    }

    return value;
}

std::string hhmmss_string(int32_t hhmmss)
{
    char buffer[7];

    for (int index = 5; index >= 0; --index)
    {
        buffer[index] = static_cast<char>('0' + hhmmss % 10);
        hhmmss /= 10;
    }

    buffer[6] = '\0';
    return std::string(buffer);
}

} // namespace krx
