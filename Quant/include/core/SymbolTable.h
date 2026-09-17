// 종목 문자열 ↔ 정수 id(SymbolId) 테이블. 수신 스레드가 틱·호가에 id를 찍고, hot path의 캐시·디스패치는
//  id 배열 인덱스로 간다. 등록은 기동·재스캔·처음 보는 종목에서만 일어나고 id는 재사용하지 않는다. [why D-071]
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <ostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

    void assign(std::string_view text)
    {
        length = static_cast<uint8_t>(text.size() < kMax ? text.size() : kMax);
        std::memcpy(data, text.data(), length);
        std::memset(data + length, 0, kMax - length);
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
    explicit SymbolTable(size_t capacity = 8192) : capacity_(capacity)
    {
        names_.reserve(64);
        names_.emplace_back(); // id 0 자리
    }

    // 있으면 그 id, 없으면 새 id. 가득 차면 kNone — 호출 쪽은 문자열 경로로 돌아간다.
    //  읽기는 shared 락(수신 스레드가 틱마다 한 번), 삽입만 배타 락.
    SymbolId intern(std::string_view ticker)
    {
        {
            std::shared_lock<std::shared_mutex> read_lock(mutex_);
            auto                                iterator = ids_.find(ticker);

            if (iterator != ids_.end())
            {
                return iterator->second;
            }
        }

        std::unique_lock<std::shared_mutex> write_lock(mutex_);
        auto                                iterator = ids_.find(ticker);

        if (iterator != ids_.end())
        {
            return iterator->second;
        }

        if (names_.size() >= capacity_)
        {
            return kNone;
        }

        const SymbolId id = static_cast<SymbolId>(names_.size());
        names_.emplace_back(ticker);
        ids_.emplace(names_.back(), id);
        return id;
    }

    [[nodiscard]] SymbolId lookup(std::string_view ticker) const
    {
        std::shared_lock<std::shared_mutex> read_lock(mutex_);
        auto                                iterator = ids_.find(ticker);
        return iterator == ids_.end() ? kNone : iterator->second;
    }

    // 모르는 id면 빈 문자열. 복사해 돌려준다 — 참조를 내주면 재스캔의 벡터 재할당과 경쟁한다.
    [[nodiscard]] std::string name(SymbolId id) const
    {
        std::shared_lock<std::shared_mutex> read_lock(mutex_);
        return id < names_.size() ? names_[id] : std::string{};
    }

    // 등록된 종목 수(id 0 제외).
    [[nodiscard]] size_t size() const
    {
        std::shared_lock<std::shared_mutex> read_lock(mutex_);
        return names_.size() - 1;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    struct SvHash
    {
        using is_transparent = void;

        size_t operator()(std::string_view text) const noexcept
        {
            return std::hash<std::string_view>{}(text);
        }
    };

    mutable std::shared_mutex                                      mutex_;
    std::unordered_map<std::string, SymbolId, SvHash, std::equal_to<>> ids_;
    std::vector<std::string>                                       names_; // [inv] names_[ids_[t]] == t
    const size_t                                                   capacity_;
};

} // namespace symbol
