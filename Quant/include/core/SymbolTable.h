// 종목 문자열 ↔ 정수 id(SymbolId) 테이블. 수신 스레드가 틱·호가에 id를 찍고, hot path의 캐시·디스패치는
//  id 배열 인덱스로 간다. 등록은 기동·재스캔·처음 보는 종목에서만 일어나고 id는 재사용하지 않는다. [why D-071]
//  읽기는 락 없이 간다 — 고정 크기 배열(재할당 없음) + 열린 주소법 버킷의 원자 id를 release/acquire로 발행.
//  shared_mutex의 읽기 락도 카운터를 고치는 쓰기라 수신 스레드가 여럿이면 캐시라인을 주고받았다(D-106).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace symbol
{

using SymbolId = uint32_t;

// 0은 "아직 id를 안 받았다". 기본 초기화된 TradeData·OrderBook이 이 값이라 배선이 빠진 경로가 드러난다.
constexpr SymbolId kNone = 0;

// 틱·호가·봉 구조체가 드는 종목 코드 — std::string 대신 고정 배열이라 구조체가 trivially copyable이고 링 복사가
//  memcpy다. 문자열이 필요한 곳(로그·REST·캡처 파일·화면)은 view()·string()로 꺼낸다. 최대 15자, 넘치면 잘린다
//  (KIS 현물 6·선물 8·미국 티커). 암시적으로 string_view가 되지만 std::string은 되지 않는다 — hot path에서
//  할당이 생기면 컴파일이 막히게. [why D-071]
struct Ticker
{
    static constexpr size_t kMax = 15;

    char    data[kMax] = {};
    uint8_t length        = 0;

    constexpr Ticker() = default;

    // string_view 하나만 받는다(const char*·std::string은 그 뒤에 선다) — 두 갈래를 두면 `t == "005930"`이 모호해진다.
    Ticker(std::string_view text)
    {
        assign(text);
    }

    // 바이트 루프다 — 길이가 실행 시간에 정해지는 memcpy·memset은 인라인되지 않아 호출 둘이 붙는데, 종목 코드는
    //  여섯 자리라 루프가 더 짧다(SymbolTable 조회 벤치 25.6 → 측정값은 D-106).
    void assign(std::string_view text)
    {
        length = static_cast<uint8_t>(text.size() < kMax ? text.size() : kMax);

        for (size_t index = 0; index < kMax; ++index)
        {
            data[index] = index < length ? text[index] : '\0';
        }
    }

    Ticker& operator=(std::string_view text)
    {
        assign(text);
        return *this;
    }

    [[nodiscard]] std::string_view view() const
    {
        return {data, length};
    }

    [[nodiscard]] std::string string() const
    {
        return std::string(data, length);
    }

    [[nodiscard]] bool empty() const
    {
        return length == 0;
    }

    [[nodiscard]] size_t size() const
    {
        return length;
    }

    operator std::string_view() const
    {
        return view();
    }

    friend bool operator==(const Ticker& ticker_a, const Ticker& ticker_b)
    {
        return ticker_a.length == ticker_b.length && std::memcmp(ticker_a.data, ticker_b.data, ticker_a.length) == 0;
    }

    friend bool operator==(const Ticker& ticker, std::string_view begin)
    {
        return ticker.view() == begin;
    }

    friend std::ostream& operator<<(std::ostream& os, const Ticker& ticker)
    {
        return os << ticker.view();
    }
};

static_assert(sizeof(Ticker) == 16);

class SymbolTable
{
public:
    // capacity는 id 상한(0 제외). 코스콤 전 종목이 2,500여 개라 기본 8,192면 재스캔 누적분까지 든다.
    //  버킷은 capacity의 2배 이상인 2의 제곱 — 절반 넘게 차지 않아 선형 탐사가 빈 칸에서 끝나는 것이 보장된다.
    explicit SymbolTable(size_t capacity = 8192)
        : capacity_(capacity < 2 ? 2 : capacity), bucket_mask_(bucket_count_for(capacity_) - 1),
          buckets_(std::make_unique<std::atomic<SymbolId>[]>(bucket_mask_ + 1)), names_(std::make_unique<Ticker[]>(capacity_))
    {
    }

    // 있으면 그 id, 없으면 새 id. 가득 차면 kNone — 호출 쪽은 문자열 경로로 돌아간다.
    //  읽기(수신 스레드가 틱마다 한 번)는 락도 원자 카운터 갱신도 없다 — 버킷 하나 acquire 읽기와 16바이트 비교.
    //  삽입만 write_mutex_로 직렬화한다. [why D-071]
    SymbolId intern(std::string_view ticker)
    {
        const Ticker   key(ticker);
        const uint64_t hash = hash_of(key);

        if (const SymbolId found = find(key, hash); found != kNone)
        {
            return found;
        }

        std::lock_guard<std::mutex> write_lock(write_mutex_);
        size_t                      slot = static_cast<size_t>(hash) & bucket_mask_;

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

        // [inv] 발행 순서 — names_[id]를 채운 뒤 버킷에 id를 release로 놓는다. 읽는 쪽은 버킷을 acquire로 읽으므로
        //  id를 본 순간 names_[id]는 완성돼 있다. count_도 그 뒤에 올려 name(id)·size()가 같은 보장을 받는다.
        names_[id] = key;
        buckets_[slot].store(id, std::memory_order_release);
        count_.store(id + 1, std::memory_order_release);
        return id;
    }

    [[nodiscard]] SymbolId lookup(std::string_view ticker) const
    {
        const Ticker key(ticker);
        return find(key, hash_of(key));
    }

    // 모르는 id면 빈 Ticker. 값으로 돌려준다(16바이트, 할당 없음) — 배열이 고정이라 참조도 안전하지만
    //  호출 쪽이 수명을 생각할 일이 없게 값이다.
    [[nodiscard]] Ticker name(SymbolId id) const
    {
        return id < count_.load(std::memory_order_acquire) ? names_[id] : Ticker{};
    }

    // 등록된 종목 수(id 0 제외).
    [[nodiscard]] size_t size() const
    {
        return count_.load(std::memory_order_acquire) - 1;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    static size_t bucket_count_for(size_t capacity)
    {
        size_t count = 16;

        while (count < capacity * 2)
        {
            count <<= 1;
        }

        return count;
    }

    // Ticker 16바이트를 uint64 둘로 — 비교·해시가 길이별 memcmp 호출 대신 정수 두 번이 된다(남는 바이트는 0으로 채워져 있다).
    static std::pair<uint64_t, uint64_t> words_of(const Ticker& ticker)
    {
        static_assert(sizeof(Ticker) == 16);
        uint64_t low  = 0;
        uint64_t high = 0;
        std::memcpy(&low, &ticker, 8);
        std::memcpy(&high, reinterpret_cast<const char*>(&ticker) + 8, 8);
        return {low, high};
    }

    static bool same_words(const Ticker& ticker_a, const Ticker& ticker_b)
    {
        return words_of(ticker_a) == words_of(ticker_b);
    }

    // [formula] Ticker 16바이트를 uint64 둘로 읽어 곱셈 믹스 — 종목 코드는 여섯 자리 숫자열이라 앞 8바이트만으로는
    //  하위 비트가 몰린다. splitmix64 상수.
    static uint64_t hash_of(const Ticker& ticker)
    {
        const auto [low, high] = words_of(ticker);
        uint64_t mixed = (low ^ 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
        mixed ^= mixed >> 31;
        mixed ^= high * 0x94D049BB133111EBULL;
        mixed ^= mixed >> 29;
        mixed *= 0xBF58476D1CE4E5B9ULL;
        mixed ^= mixed >> 32;
        return mixed;
    }

    // 선형 탐사. 빈 버킷(kNone)을 만나면 없는 것 — 삭제가 없고 절반 넘게 차지 않아 반드시 끝난다.
    [[nodiscard]] SymbolId find(const Ticker& key, uint64_t hash) const
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

    const size_t                                capacity_;
    const size_t                                bucket_mask_;
    std::unique_ptr<std::atomic<SymbolId>[]>    buckets_; // 0 = 빈 칸. 삭제 없음, id는 한 번 놓이면 안 바뀐다
    std::unique_ptr<Ticker[]>                   names_;   // [inv] names_[id] == 그 id를 받은 티커(15자 넘으면 잘린 채)
    std::atomic<SymbolId>                       count_{1}; // 다음에 줄 id = 채워진 names_ 수(0번 자리 포함)
    std::mutex                                  write_mutex_; // 삽입만 잡는다. 읽기 경로는 잡지 않는다
};

} // namespace symbol
