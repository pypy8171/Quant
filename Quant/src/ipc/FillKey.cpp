#include "ipc/FillKey.h"

namespace fill_key
{
size_t FillKeyHash::operator()(const FillKey& key) const noexcept
{
    uint64_t hash = key.order_number * 0x9E3779B97F4A7C15ULL;
    hash ^= (static_cast<uint64_t>(key.trade_date) << 32) ^ key.fill_time;
    hash *= 0xBF58476D1CE4E5B9ULL;
    hash ^= (static_cast<uint64_t>(static_cast<uint32_t>(key.quantity)) << 32) ^ static_cast<uint64_t>(key.price_cents);
    hash ^= hash >> 31;
    return static_cast<size_t>(hash);
}

} // namespace fill_key
