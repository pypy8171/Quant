#include "ipc/SharedStrategyDictionary.h"

#include "ipc/SharedWriteLock.h"

#include <atomic>
#include <cstring>

namespace ipc
{

// 제어 칸은 캐시라인 배수여야 뒤따르는 이름 배열이 캐시라인 경계에서 시작한다.
static_assert(sizeof(SharedStrategyControl) % kSharedCacheLine == 0, "표 머리는 캐시라인 배수여야 한다");

size_t SharedStrategyDictionary::bytes_for(size_t capacity)
{
    return sizeof(SharedStrategyControl) + capacity * sizeof(strategy_table::StrategyName);
}

bool SharedStrategyDictionary::check_arguments(const std::byte* base, size_t bytes, size_t capacity)
{
    last_error_.clear();

    if (base == nullptr)
    {
        last_error_ = "표를 놓을 자리가 없다";
        return false;
    }

    if (capacity < 2)
    {
        last_error_ = "전략 수가 2보다 작다";
        return false;
    }

    if (bytes < bytes_for(capacity))
    {
        last_error_ = "구역이 표보다 작다 — 필요=" + std::to_string(bytes_for(capacity)) + " 받음=" + std::to_string(bytes);
        return false;
    }

    if (reinterpret_cast<uintptr_t>(base) % kSharedCacheLine != 0)
    {
        last_error_ = "표가 캐시라인 경계에서 시작하지 않는다";
        return false;
    }

    return true;
}

void SharedStrategyDictionary::bind(std::byte* base, size_t capacity)
{
    control_ = reinterpret_cast<SharedStrategyControl*>(base);

    auto* names = reinterpret_cast<strategy_table::StrategyName*>(base + sizeof(SharedStrategyControl));

    slots_ = strategy_table::TableSlots{names, &control_->count, capacity};
}

bool SharedStrategyDictionary::create(std::byte* base, size_t bytes, size_t capacity)
{
    if (!check_arguments(base, bytes, capacity))
    {
        return false;
    }

    // 배열까지 통째로 지우고 머리를 마지막에 적는다 — 붙는 쪽은 magic을 보고 들어온다. 다음에 줄 번호는
    //  1에서 시작한다(0번은 "전략 없음"이라 이름 자리로 쓰지 않는다).
    std::memset(base, 0, bytes_for(capacity));

    auto* control         = reinterpret_cast<SharedStrategyControl*>(base);
    control->record_bytes = static_cast<uint32_t>(sizeof(strategy_table::StrategyName));
    control->capacity     = capacity;
    control->count.store(1, std::memory_order_relaxed);
    control->write_lock.store(0, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(control->magic).store(kSharedStrategyDictionaryMagic, std::memory_order_release);

    bind(base, capacity);
    return true;
}

bool SharedStrategyDictionary::attach(std::byte* base, size_t bytes, size_t capacity)
{
    if (!check_arguments(base, bytes, capacity))
    {
        return false;
    }

    const auto* control = reinterpret_cast<const SharedStrategyControl*>(base);

    if (std::atomic_ref<uint32_t>(const_cast<uint32_t&>(control->magic)).load(std::memory_order_acquire) !=
            kSharedStrategyDictionaryMagic ||
        control->record_bytes != sizeof(strategy_table::StrategyName) || control->capacity != capacity)
    {
        last_error_ = "표 머리가 다르다 — 옛 exe가 새 배치에 붙었는지 본다";
        return false;
    }

    bind(base, capacity);
    return true;
}

void SharedStrategyDictionary::unbind() noexcept
{
    control_ = nullptr;
    slots_   = strategy_table::TableSlots{};
}

strategy_table::StrategyId SharedStrategyDictionary::intern(std::string_view name)
{
    if (control_ == nullptr)
    {
        return strategy_table::kNone;
    }

    // 빠른 길 — 이미 있으면 자물쇠를 잡지 않는다.
    if (const strategy_table::StrategyId found = strategy_table::table_lookup(slots_, name);
        found != strategy_table::kNone)
    {
        return found;
    }

    const SharedWriteLock write_lock(control_->write_lock);
    return strategy_table::table_insert(slots_, name);
}

size_t SharedStrategyDictionary::size() const
{
    if (control_ == nullptr)
    {
        return 0;
    }

    return control_->count.load(std::memory_order_acquire) - 1;
}

} // namespace ipc
