#include "ipc/SharedSymbolDictionary.h"

#include "ipc/SharedWriteLock.h"

#include <atomic>
#include <cstring>

namespace ipc
{
// 제어 칸은 캐시라인 배수여야 뒤따르는 버킷 배열이 캐시라인 경계에서 시작한다.
static_assert(sizeof(SharedDictionaryControl) % kSharedCacheLine == 0, "표 머리는 캐시라인 배수여야 한다");

size_t SharedSymbolDictionary::bytes_for(size_t capacity)
{
    return sizeof(SharedDictionaryControl) + symbol::bucket_count_for(capacity) * sizeof(std::atomic<symbol::SymbolId>) +
           capacity * sizeof(symbol::Ticker);
}

bool SharedSymbolDictionary::check_arguments(const std::byte* base, size_t bytes, size_t capacity)
{
    last_error_.clear();

    if (base == nullptr)
    {
        last_error_ = "표를 놓을 자리가 없다";
        return false;
    }

    if (capacity < 2)
    {
        last_error_ = "종목 수가 2보다 작다";
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

void SharedSymbolDictionary::bind(std::byte* base, size_t capacity)
{
    const size_t bucket_count = symbol::bucket_count_for(capacity);

    control_ = reinterpret_cast<SharedDictionaryControl*>(base);

    auto* buckets = reinterpret_cast<std::atomic<symbol::SymbolId>*>(base + sizeof(SharedDictionaryControl));
    auto* names   = reinterpret_cast<symbol::Ticker*>(base + sizeof(SharedDictionaryControl) +
                                                    bucket_count * sizeof(std::atomic<symbol::SymbolId>));

    slots_ = symbol::TableSlots{buckets, names, &control_->count, capacity, bucket_count - 1};
}

bool SharedSymbolDictionary::create(std::byte* base, size_t bytes, size_t capacity)
{
    if (!check_arguments(base, bytes, capacity))
    {
        return false;
    }

    // 배열까지 통째로 지우고 머리를 마지막에 적는다 — 붙는 쪽은 magic을 보고 들어온다. 버킷 0은 "빈 칸",
    //  다음에 줄 번호는 1에서 시작한다(0번은 "아직 번호를 안 받았다"라 이름 자리로 쓰지 않는다).
    std::memset(base, 0, bytes_for(capacity));

    auto* control         = reinterpret_cast<SharedDictionaryControl*>(base);
    control->record_bytes = static_cast<uint32_t>(sizeof(symbol::Ticker));
    control->capacity     = capacity;
    control->bucket_count = symbol::bucket_count_for(capacity);
    control->count.store(1, std::memory_order_relaxed);
    control->write_lock.store(0, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(control->magic).store(kSharedDictionaryMagic, std::memory_order_release);

    bind(base, capacity);
    return true;
}

bool SharedSymbolDictionary::attach(std::byte* base, size_t bytes, size_t capacity)
{
    if (!check_arguments(base, bytes, capacity))
    {
        return false;
    }

    const auto* control = reinterpret_cast<const SharedDictionaryControl*>(base);

    if (std::atomic_ref<uint32_t>(const_cast<uint32_t&>(control->magic)).load(std::memory_order_acquire) !=
            kSharedDictionaryMagic ||
        control->record_bytes != sizeof(symbol::Ticker) ||
        control->capacity != capacity || control->bucket_count != symbol::bucket_count_for(capacity))
    {
        last_error_ = "표 머리가 다르다 — 옛 exe가 새 배치에 붙었는지 본다";
        return false;
    }

    bind(base, capacity);
    return true;
}

void SharedSymbolDictionary::unbind() noexcept
{
    control_ = nullptr;
    slots_   = symbol::TableSlots{};
}

symbol::SymbolId SharedSymbolDictionary::intern(std::string_view ticker)
{
    if (control_ == nullptr)
    {
        return symbol::kNone;
    }

    // 빠른 길 — 이미 있으면 자물쇠를 잡지 않는다.
    if (const symbol::SymbolId found = symbol::table_lookup(slots_, ticker); found != symbol::kNone)
    {
        return found;
    }

    const SharedWriteLock write_lock(control_->write_lock);
    return symbol::table_insert(slots_, ticker);
}

size_t SharedSymbolDictionary::size() const
{
    if (control_ == nullptr)
    {
        return 0;
    }

    return control_->count.load(std::memory_order_acquire) - 1;
}

} // namespace ipc
