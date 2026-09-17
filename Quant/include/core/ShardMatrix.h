#pragma once
// 수신 스레드 N개 × 전략 샤드 M개의 SPSC 링 행렬 — 전 시장 피드의 앞단 팬아웃 조각.
//  생산자 n(소켓 하나를 읽는 수신 스레드)이 종목 해시로 고른 샤드 m의 셀 [n][m]에 push하고, 소비자 m(전략 샤드
//  스레드)이 자기 열 [*][m]을 라운드로빈으로 pop한다. 셀마다 생산자·소비자가 하나라 락 없는 RingBuffer가 그대로
//  쓰인다(원칙 5 — MPSC 하나보다 N×M SPSC를 먼저). 종목은 소켓 하나에만 있으므로(FeedMux) 한 종목의 틱은
//  셀 하나만 지나고 순서가 지켜진다(원칙 2). 채널·종목 사이 순서는 지키지 않는다.
//  깨우기(WakeGate)·넘침 카운트는 배선 쪽 몫이다 — push가 false를 돌려주면 셀이 가득 찬 것. [why D-071]
#include "core/RingBuffer.h"
#include "core/SymbolTable.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace shard
{

// 종목 id → 샤드 번호. id는 intern 순서의 촘촘한 정수라 나머지만 취해도 고르지만, 종목 추가 순서가 채널·시장별로
//  몰릴 수 있어(현물 뒤 선물, 재스캔 신규) 곱셈 해시로 한 번 섞는다. shards가 1이면 항상 0. [formula]
[[nodiscard]] constexpr uint32_t shard_of(symbol::SymbolId symbol_id, uint32_t shards) noexcept
{
    const uint64_t mixed = (static_cast<uint64_t>(symbol_id) * 0x9E3779B97F4A7C15ull) >> 32;
    return shards <= 1 ? 0u : static_cast<uint32_t>(mixed % shards);
}

template <typename T> class Matrix
{
public:
    // 셀 하나의 용량이 capacity(2의 거듭제곱으로 올림). 셀은 producers × consumers개라 메모리는 N·M·capacity·sizeof(T).
    Matrix(uint32_t producers, uint32_t consumers, size_t capacity)
        : producers_(producers), consumers_(consumers), cursors_(consumers)
    {
        cells_.reserve(static_cast<size_t>(producers) * consumers);

        for (size_t consumer_index = 0; consumer_index < static_cast<size_t>(producers) * consumers; ++consumer_index)
        {
            cells_.push_back(std::make_unique<RingBuffer<T>>(capacity));
        }
    }

    Matrix(const Matrix&)            = delete;
    Matrix& operator=(const Matrix&) = delete;

    // 크기를 다시 잡는다 — 셀을 전부 버리고 새로 만든다. 생산자·소비자 스레드가 하나도 없을 때만(기동 전 config 반영).
    void reshape(uint32_t producers, uint32_t consumers, size_t capacity)
    {
        producers_ = producers;
        consumers_ = consumers;
        cells_.clear();
        cells_.reserve(static_cast<size_t>(producers) * consumers);

        for (size_t consumer_index = 0; consumer_index < static_cast<size_t>(producers) * consumers; ++consumer_index)
        {
            cells_.push_back(std::make_unique<RingBuffer<T>>(capacity));
        }

        cursors_.assign(consumers, Cursor{});
    }

    [[nodiscard]] uint32_t producers() const noexcept
    {
        return producers_;
    }

    [[nodiscard]] uint32_t consumers() const noexcept
    {
        return consumers_;
    }

    // 생산자 producer 스레드에서만 부른다. 셀이 가득 차면 false — 호출자가 세고 버린다(수신 스레드는 기다리지 않는다, 원칙 3).
    [[nodiscard]] bool push(uint32_t producer, symbol::SymbolId symbol_id, const T& value)
    {
        return cell(producer, shard_of(symbol_id, consumers_)).push(value);
    }

    // 종목이 가는 열. 생산자가 push 뒤 그 샤드만 깨우거나 호가·체결 두 행렬에 같은 열로 넣을 때 쓴다.
    [[nodiscard]] uint32_t consumer_of(symbol::SymbolId symbol_id) const noexcept
    {
        return shard_of(symbol_id, consumers_);
    }

    // 샤드를 호출자가 이미 안다면(같은 종목의 호가·체결을 같은 곳으로) 여기로.
    [[nodiscard]] bool push_to(uint32_t producer, uint32_t consumer, const T& value)
    {
        return cell(producer, consumer).push(value);
    }

    // 소비자 consumer 스레드에서만 부른다. 마지막으로 꺼낸 생산자의 다음 칸부터 훑어 한 생산자가 몰아쳐도
    //  다른 생산자의 틱이 굶지 않는다. 열이 전부 비면 nullopt.
    [[nodiscard]] std::optional<T> pop(uint32_t consumer)
    {
        uint32_t& next = cursors_[consumer].next;

        for (uint32_t producer_index = 0; producer_index < producers_; ++producer_index)
        {
            const uint32_t count = (next + producer_index) % producers_;

            if (auto value = cell(count, consumer).pop())
            {
                next = (count + 1) % producers_;
                return value;
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] bool empty(uint32_t consumer) const noexcept
    {
        for (uint32_t producer_index = 0; producer_index < producers_; ++producer_index)
        {
            if (!cell(producer_index, consumer).empty())
            {
                return false;
            }
        }

        return true;
    }

    // 열의 셀 고수위 중 최대 — [큐 고수위] 로그용.
    [[nodiscard]] size_t high_water(uint32_t consumer) const noexcept
    {
        size_t hw = 0;

        for (uint32_t producer_index = 0; producer_index < producers_; ++producer_index)
        {
            const size_t height = cell(producer_index, consumer).high_water();
            hw             = height > hw ? height : hw;
        }

        return hw;
    }

private:
    // 소비자마다 한 줄 — 소비자 스레드만 쓰지만 이웃 소비자의 커서와 같은 캐시라인에 두지 않는다.
    struct alignas(kCacheLine) Cursor
    {
        uint32_t next = 0;
    };

    [[nodiscard]] RingBuffer<T>& cell(uint32_t count, uint32_t row) noexcept
    {
        return *cells_[static_cast<size_t>(count) * consumers_ + row];
    }

    [[nodiscard]] const RingBuffer<T>& cell(uint32_t count, uint32_t row) const noexcept
    {
        return *cells_[static_cast<size_t>(count) * consumers_ + row];
    }

    uint32_t                                    producers_;
    uint32_t                                    consumers_;
    std::vector<std::unique_ptr<RingBuffer<T>>> cells_; // [n * M + m]. unique_ptr라 링 주소가 고정이다
    std::vector<Cursor>                         cursors_;
};

} // namespace shard
