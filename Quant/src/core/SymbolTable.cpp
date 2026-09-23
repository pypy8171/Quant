#include "core/SymbolTable.h"

#include <bit>

namespace symbol
{
namespace
{
// Ticker 16바이트를 uint64 둘로 본 것 — 비교·해시가 길이별 memcmp 호출 대신 정수 두 번이 된다.
//  low = data[0..7], high = data[8..14] + 마지막 바이트에 length. 남는 바이트는 0.
struct TickerWords
{
    uint64_t low  = 0;
    uint64_t high = 0;

    friend bool operator==(const TickerWords& words_a, const TickerWords& words_b)
    {
        return words_a.low == words_b.low && words_a.high == words_b.high;
    }
};

// [inv] 아래 두 words_of는 같은 문자열에 같은 워드를 내야 한다 — 하나는 names의 Ticker를 그대로 읽고, 하나는
//  string_view에서 Ticker를 거치지 않고 바로 만든다(조회마다 16바이트 임시 객체를 채우고 다시 읽던 두 단계를 한 단계로).
//  바이트를 아래 자리부터 쌓으므로 리틀 엔디언에서만 Ticker의 메모리 배치와 같다.
static_assert(std::endian::native == std::endian::little);

TickerWords words_of(const Ticker& ticker)
{
    static_assert(sizeof(Ticker) == 16);
    TickerWords words;
    std::memcpy(&words.low, &ticker, 8);
    std::memcpy(&words.high, reinterpret_cast<const char*>(&ticker) + 8, 8);
    return words;
}

// Ticker::assign과 같은 규칙으로 자른다(kMax 넘으면 잘림).
TickerWords words_of(std::string_view text)
{
    const size_t length = text.size() < Ticker::kMax ? text.size() : Ticker::kMax;
    TickerWords  words;

    for (size_t index = 0; index < length; ++index)
    {
        const uint64_t byte = static_cast<uint8_t>(text[index]);

        if (index < 8)
        {
            words.low |= byte << (index * 8);
        }
        else
        {
            words.high |= byte << ((index - 8) * 8);
        }
    }

    words.high |= static_cast<uint64_t>(length) << 56;
    return words;
}

bool same_words(const Ticker& ticker, const TickerWords& key)
{
    return words_of(ticker) == key;
}

// [formula] Ticker 16바이트를 uint64 둘로 읽어 곱셈 믹스 — 종목 코드는 여섯 자리 숫자열이라 앞 8바이트만으로는
//  하위 비트가 몰린다. splitmix64 상수.
uint64_t hash_of(const TickerWords& words)
{
    const uint64_t low  = words.low;
    const uint64_t high = words.high;
    uint64_t       mixed = (low ^ 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
    mixed ^= mixed >> 31;
    mixed ^= high * 0x94D049BB133111EBULL;
    mixed ^= mixed >> 29;
    mixed *= 0xBF58476D1CE4E5B9ULL;
    mixed ^= mixed >> 32;
    return mixed;
}

// 선형 탐사. 빈 버킷(kNone)을 만나면 없는 것 — 삭제가 없고 절반 넘게 차지 않아 반드시 끝난다.
SymbolId find(const TableSlots& slots, const TickerWords& key, uint64_t hash)
{
    for (size_t slot = static_cast<size_t>(hash) & slots.bucket_mask;; slot = (slot + 1) & slots.bucket_mask)
    {
        const SymbolId id = slots.buckets[slot].load(std::memory_order_acquire);

        if (id == kNone)
        {
            return kNone;
        }

        if (same_words(slots.names[id], key))
        {
            return id;
        }
    }
}

} // namespace

bool is_korean_ticker(std::string_view ticker) noexcept
{
    if (ticker.size() != kKoreanTickerLength)
    {
        return false;
    }

    for (char character : ticker)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
    }

    return true;
}

Ticker::Ticker(std::string_view text)
{
    assign(text);
}

void Ticker::assign(std::string_view text)
{
    length = static_cast<uint8_t>(text.size() < kMax ? text.size() : kMax);

    for (size_t index = 0; index < kMax; ++index)
    {
        data[index] = index < length ? text[index] : '\0';
    }
}

Ticker& Ticker::operator=(std::string_view text)
{
    assign(text);
    return *this;
}

size_t bucket_count_for(size_t capacity)
{
    size_t count = 16;

    while (count < capacity * 2)
    {
        count <<= 1;
    }

    return count;
}

SymbolId table_lookup(const TableSlots& slots, std::string_view ticker)
{
    if (slots.empty())
    {
        return kNone;
    }

    const TickerWords key = words_of(ticker);
    return find(slots, key, hash_of(key));
}

Ticker table_name(const TableSlots& slots, SymbolId id)
{
    if (slots.empty())
    {
        return Ticker{};
    }

    return id < slots.count->load(std::memory_order_acquire) ? slots.names[id] : Ticker{};
}

SymbolId table_insert(const TableSlots& slots, std::string_view ticker)
{
    if (slots.empty())
    {
        return kNone;
    }

    const TickerWords key  = words_of(ticker);
    const uint64_t    hash = hash_of(key);
    size_t            slot = static_cast<size_t>(hash) & slots.bucket_mask;

    // 자물쇠를 잡은 뒤 다시 탐사 — 다른 쓰기 스레드가 먼저 넣었을 수 있다. 빈 칸이 곧 넣을 자리.
    while (true)
    {
        const SymbolId id = slots.buckets[slot].load(std::memory_order_relaxed);

        if (id == kNone)
        {
            break;
        }

        if (same_words(slots.names[id], key))
        {
            return id;
        }

        slot = (slot + 1) & slots.bucket_mask;
    }

    const SymbolId id = slots.count->load(std::memory_order_relaxed);

    if (id >= slots.capacity)
    {
        return kNone;
    }

    // [inv] 발행 순서 — names[id]를 채우고, count를 올리고, 마지막에 버킷에 id를 release로 놓는다. 읽는 쪽은
    //  버킷을 acquire로 읽으므로 id를 본 순간 names[id]와 count(>id)가 둘 다 보인다 — name(id)가 빈 값을 내지
    //  않는다. 버킷을 count보다 먼저 놓으면 그 사이에 lookup은 id를 주는데 name(id)는 빈 Ticker를 준다
    //  (test_symbol_table 6번이 부하 아래서 40번에 5번 잡았다, 09-22). Ticker는 새로 넣을 때만 만든다.
    slots.names[id].assign(ticker);
    slots.count->store(id + 1, std::memory_order_release);
    slots.buckets[slot].store(id, std::memory_order_release);
    return id;
}

SymbolTable::SymbolTable(size_t capacity)
{
    const size_t bounded_capacity = capacity < 2 ? 2 : capacity;
    const size_t bucket_count     = bucket_count_for(bounded_capacity);

    buckets_ = std::make_unique<std::atomic<SymbolId>[]>(bucket_count);
    names_   = std::make_unique<Ticker[]>(bounded_capacity);
    slots_   = TableSlots{buckets_.get(), names_.get(), &count_, bounded_capacity, bucket_count - 1};
}

SymbolId SymbolTable::intern(std::string_view ticker)
{
    // 빠른 길 — 이미 있으면 자물쇠를 잡지 않는다. 등록은 기동·재스캔에서만 일어난다.
    if (const SymbolId found = table_lookup(slots_, ticker); found != kNone)
    {
        return found;
    }

    std::lock_guard<std::mutex> write_lock(write_mutex_);
    return table_insert(slots_, ticker);
}

} // namespace symbol
