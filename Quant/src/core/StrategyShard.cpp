#include "core/StrategyShard.h"

namespace strategy
{
size_t Shard::high_water() const noexcept
{
    size_t high_water = queue_.order_book.high_water(index_);
    high_water = high_water < queue_.trade.high_water(index_) ? queue_.trade.high_water(index_) : high_water;
    return high_water < queue_.bars.high_water(index_) ? queue_.bars.high_water(index_) : high_water;
}

} // namespace strategy
