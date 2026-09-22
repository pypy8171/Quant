#include "core/SymbolTable.h"

namespace symbol
{
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

SymbolId SymbolTable::intern(std::string_view ticker)
{
    const TickerWords key = words_of(ticker);
    const uint64_t hash = hash_of(key);

    if (const SymbolId found = find(key, hash); found != kNone)
    {
        return found;
    }

    std::lock_guard<std::mutex> write_lock(write_mutex_);
    size_t slot = static_cast<size_t>(hash) & bucket_mask_;

    // 락을 잡은 뒤 다시 탐사 — 다른 쓰기 스레드가 먼저 넣었을 수 있다. 빈 칸이 곧 넣을 자리.
    while (true)
    {
        const SymbolId id = buckets_[slot].load(std::memory_order_relaxed);

        if (id == kNone)
        {
            break;
        }

        if (same_words(names_[id], key))
        {
            return id;
        }

        slot = (slot + 1) & bucket_mask_;
    }

    const SymbolId id = count_.load(std::memory_order_relaxed);

    if (id >= capacity_)
    {
        return kNone;
    }

    // [inv] 발행 순서 — names_[id]를 채우고, count_를 올리고, 마지막에 버킷에 id를 release로 놓는다. 읽는 쪽은
    //  버킷을 acquire로 읽으므로 id를 본 순간 names_[id]와 count_(>id)가 둘 다 보인다 — name(id)가 빈 값을 내지
    //  않는다. 버킷을 count_보다 먼저 놓으면 그 사이에 lookup은 id를 주는데 name(id)는 빈 Ticker를 준다
    //  (test_symbol_table 6번이 부하 아래서 40번에 5번 잡았다, 09-22). Ticker는 새로 넣을 때만 만든다.
    names_[id].assign(ticker);
    count_.store(id + 1, std::memory_order_release);
    buckets_[slot].store(id, std::memory_order_release);
    return id;
}

SymbolId SymbolTable::lookup(std::string_view ticker) const
{
    const TickerWords key = words_of(ticker);
    return find(key, hash_of(key));
}

size_t SymbolTable::bucket_count_for(size_t capacity)
{
    size_t count = 16;

    while (count < capacity * 2)
    {
        count <<= 1;
    }

    return count;
}

SymbolTable::TickerWords SymbolTable::words_of(const Ticker& ticker)
{
    static_assert(sizeof(Ticker) == 16);
    TickerWords words;
    std::memcpy(&words.low, &ticker, 8);
    std::memcpy(&words.high, reinterpret_cast<const char*>(&ticker) + 8, 8);
    return words;
}

SymbolTable::TickerWords SymbolTable::words_of(std::string_view text)
{
    const size_t length = text.size() < Ticker::kMax ? text.size() : Ticker::kMax;
    TickerWords words;

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

uint64_t SymbolTable::hash_of(const TickerWords& words)
{
    const uint64_t low = words.low;
    const uint64_t high = words.high;
    uint64_t mixed = (low ^ 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
    mixed ^= mixed >> 31;
    mixed ^= high * 0x94D049BB133111EBULL;
    mixed ^= mixed >> 29;
    mixed *= 0xBF58476D1CE4E5B9ULL;
    mixed ^= mixed >> 32;
    return mixed;
}

SymbolId SymbolTable::find(const TickerWords& key, uint64_t hash) const
{
    for (size_t slot = static_cast<size_t>(hash) & bucket_mask_;; slot = (slot + 1) & bucket_mask_)
    {
        const SymbolId id = buckets_[slot].load(std::memory_order_acquire);

        if (id == kNone)
        {
            return kNone;
        }

        if (same_words(names_[id], key))
        {
            return id;
        }
    }
}

} // namespace symbol
