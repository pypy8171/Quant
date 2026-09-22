#include "core/TickSize.h"

namespace krx
{
double round_to_tick(double price, OrderSide side)
{
    const double threshold = tick_size(price);

    if (threshold <= 0.0)
    {
        return price;
    }

    return (side == OrderSide::BUY) ? std::floor(price / threshold) * threshold
                                    : std::ceil(price / threshold) * threshold;
}

} // namespace krx
