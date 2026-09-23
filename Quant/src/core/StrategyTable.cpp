#include "core/StrategyTable.h"

namespace strategy_table
{
namespace
{
// 이름 두 개가 같은가. 길이를 먼저 본다 — 접두가 같은 이름이 많아서(DEVSCALE_*) 길이만으로 대부분 갈린다.
[[nodiscard]] bool same_name(const StrategyName& stored, std::string_view name) noexcept
{
    if (stored.length != name.size())
    {
        return false;
    }

    for (size_t index = 0; index < stored.length; ++index)
    {
        if (stored.data[index] != name[index])
        {
            return false;
        }
    }

    return true;
}

// 1번부터 count 직전까지 훑는다. 등록·복원 때만 도는 길이라 선형으로 둔다(수백 개 규모).
[[nodiscard]] StrategyId find(const TableSlots& slots, std::string_view name, StrategyId count)
{
    for (StrategyId id = 1; id < count; ++id)
    {
        if (same_name(slots.names[id], name))
        {
            return id;
        }
    }

    return kNone;
}
} // namespace

void StrategyName::assign(std::string_view text) noexcept
{
    const size_t copied = text.size() < kMax ? text.size() : kMax;

    for (size_t index = 0; index < copied; ++index)
    {
        data[index] = text[index];
    }

    for (size_t index = copied; index < kMax; ++index)
    {
        data[index] = '\0';
    }

    length = static_cast<uint8_t>(copied);
}

bool fits(std::string_view name) noexcept
{
    return !name.empty() && name.size() <= StrategyName::kMax;
}

StrategyId table_lookup(const TableSlots& slots, std::string_view name)
{
    if (slots.empty() || !fits(name))
    {
        return kNone;
    }

    return find(slots, name, slots.count->load(std::memory_order_acquire));
}

StrategyId table_insert(const TableSlots& slots, std::string_view name)
{
    if (slots.empty() || !fits(name))
    {
        return kNone;
    }

    const StrategyId count = slots.count->load(std::memory_order_relaxed);

    if (const StrategyId found = find(slots, name, count); found != kNone)
    {
        return found;
    }

    if (count >= slots.capacity)
    {
        return kNone;
    }

    // [inv] 발행 순서 — names[id]를 채운 뒤 count를 release로 올린다. 읽는 쪽은 acquire로 count를 읽으므로
    //  번호를 본 순간 이름은 완성돼 있다. 순서를 뒤집으면 건너편이 빈 이름을 본다.
    slots.names[count].assign(name);
    slots.count->store(count + 1, std::memory_order_release);
    return count;
}

StrategyName table_name(const TableSlots& slots, StrategyId id)
{
    if (slots.empty() || id == kNone || id >= slots.count->load(std::memory_order_acquire))
    {
        return StrategyName{};
    }

    return slots.names[id];
}

StrategyTable::StrategyTable(size_t capacity)
{
    const size_t bounded_capacity = capacity < 2 ? 2 : capacity;

    names_ = std::make_unique<StrategyName[]>(bounded_capacity);
    slots_ = TableSlots{names_.get(), &count_, bounded_capacity};
}

void StrategyTable::adopt(const TableSlots& slots, std::function<StrategyId(std::string_view)> register_hook)
{
    slots_         = slots;
    register_hook_ = std::move(register_hook);
    names_.reset(); // 힙 배열은 여기서 놓는다 — slots_가 이미 남의 자리를 가리킨다
}

StrategyId StrategyTable::intern(std::string_view name)
{
    // 빠른 길 — 이미 있으면 자물쇠도 부탁도 없다.
    if (const StrategyId found = table_lookup(slots_, name); found != kNone)
    {
        return found;
    }

    if (register_hook_)
    {
        return register_hook_(name);
    }

    std::lock_guard<std::mutex> write_lock(write_mutex_);

    return table_insert(slots_, name);
}

} // namespace strategy_table
