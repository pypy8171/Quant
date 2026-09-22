#include "core/StrategyTable.h"

namespace strategy_table
{
StrategyId StrategyTable::intern(std::string_view name)
{
    if (name.empty())
    {
        return kNone;
    }

    std::lock_guard<std::mutex> write_lock(write_mutex_);
    const StrategyId count = count_.load(std::memory_order_relaxed);

    for (StrategyId id = 1; id < count; ++id)
    {
        if (names_[id] == name)
        {
            return id;
        }
    }

    if (count >= capacity_)
    {
        return kNone;
    }

    // [inv] 발행 순서 — names_[id]를 채운 뒤 count_를 release로 올린다. name(id)는 acquire로 count_를 읽으므로
    //  번호를 본 순간 이름은 완성돼 있다.
    names_[count] = std::string(name);
    count_.store(count + 1, std::memory_order_release);
    return count;
}

StrategyId StrategyTable::lookup(std::string_view name) const
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    const StrategyId count = count_.load(std::memory_order_relaxed);

    for (StrategyId id = 1; id < count; ++id)
    {
        if (names_[id] == name)
        {
            return id;
        }
    }

    return kNone;
}

} // namespace strategy_table
