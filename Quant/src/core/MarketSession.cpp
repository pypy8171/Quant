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

OrderWindow order_window(int32_t hhmmss)
{
    constexpr int32_t kPreMarketOpen      = 80000;  // NXT 프리마켓 시작 08:00:00(nextrade.co.kr 거래제도, 2026-09-27 확인)
    constexpr int32_t kPreMarketClose     = 85000;  // NXT 프리마켓 끝 08:50:00
    constexpr int32_t kClosingAuctionOpen = 154000; // 장후 종가매매 시작 15:40:00
    constexpr int32_t kAfterMarketOpen    = 160000; // 애프터마켓 시작 16:00:00
    constexpr int32_t kAfterMarketClose   = 200000; // 애프터마켓 끝 20:00:00

    if (hhmmss >= kPreMarketOpen && hhmmss < kPreMarketClose)
    {
        return OrderWindow::PreMarket;
    }

    if (hhmmss >= kClosingAuctionOpen && hhmmss < kAfterMarketOpen)
    {
        return OrderWindow::ClosingAuction;
    }

    if (hhmmss >= kAfterMarketOpen && hhmmss < kAfterMarketClose)
    {
        return OrderWindow::AfterMarket;
    }

    return OrderWindow::Regular;
}

bool market_order_unavailable(OrderWindow window)
{
    return window == OrderWindow::PreMarket || window == OrderWindow::AfterMarket;
}

} // namespace krx
