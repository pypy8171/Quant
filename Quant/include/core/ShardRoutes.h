// 종목 id → 그 종목의 틱을 받아야 하는 샤드 집합(비트마스크). 샤딩 단위가 종목이 아니라 전략이라 그렇다 —
//  전략 객체는 샤드 하나가 통째로 갖고(등록 순 라운드로빈), 종목 하나를 여러 샤드의 전략이 보면 수신 스레드가 그 샤드
//  전부에 틱을 넣는다. 종목 해시로 열을 고르던 때는 한 전략의 종목이 여러 열에 흩어지면 전략 객체를 스레드 둘이
//  만지게 돼 샤드를 1로 내려야 했다(다종목 전략 하나뿐인 지금 config는 언제나 1이었다). [why D-110]
// 스레드: 읽기(mask)는 수신·데이터 스레드가 틱마다 락 없이(원자 load 한 번). 쓰기(commit)는 전략 목록이 바뀔 때
//  strategy_.mutex 하에서만.
#pragma once

#include "core/SymbolTable.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace shard
{

using ShardMask = uint64_t;

// 마스크 폭. 전략 샤드 수의 상한이다 — 코어 수보다 훨씬 크다.
constexpr uint32_t kMaxShards = 64;

[[nodiscard]] constexpr ShardMask mask_of(uint32_t shard_index) noexcept
{
    return ShardMask{1} << shard_index;
}

class RouteTable
{
public:
    // symbol_capacity: SymbolTable::capacity()와 같게 — id는 그 미만이라 배열 인덱스로 바로 쓴다.
    explicit RouteTable(size_t symbol_capacity = 8192)
    {
        publish(std::make_unique<Snapshot>(symbol_capacity));
    }

    RouteTable(const RouteTable&)            = delete;
    RouteTable& operator=(const RouteTable&) = delete;

    // 용량을 종목 테이블에 맞춘다. 읽는 스레드가 없을 때(기동 전)만.
    void reset(size_t symbol_capacity)
    {
        publish(std::make_unique<Snapshot>(symbol_capacity));
    }

    // 다시 만들 때 쓰는 초안. 쓰는 스레드가 채운 뒤 commit으로 한 번에 넘긴다.
    struct Draft
    {
        std::vector<ShardMask> masks;
        ShardMask              all = 0; // 구독 종목을 안 밝힌 전략(전부 받는 전략)이 있는 샤드들

        explicit Draft(size_t symbol_capacity) : masks(symbol_capacity, 0) {}

        void add(symbol::SymbolId id, uint32_t shard_index)
        {
            if (id != symbol::kNone && id < masks.size())
            {
                masks[id] |= mask_of(shard_index);
            }
        }

        void add_all(uint32_t shard_index)
        {
            all |= mask_of(shard_index);
        }
    };

    [[nodiscard]] Draft draft() const
    {
        return Draft(capacity());
    }

    // 표를 통째로 새로 만들어 포인터 하나만 바꾼다. 읽는 쪽은 옛 표 아니면 새 표를 보고, 둘을 섞어 보지 않는다.
    //  종목마다 원자 store를 하나씩 놓던 때는 표가 반쯤 바뀐 상태(종목 A는 새 마스크, 종목 B는 옛 마스크)가 보였고,
    //  all_을 마스크보다 먼저 놓아 새 샤드가 자기 종목 마스크보다 먼저 틱을 받기도 했다. 공개 지점이 하나면
    //  그런 중간 상태가 아예 없다. [why D-116]
    void commit(const Draft& draft)
    {
        auto snapshot = std::make_unique<Snapshot>(draft.masks.size());
        snapshot->masks = draft.masks;
        snapshot->all   = draft.all;
        publish(std::move(snapshot));
    }

    // id의 틱을 받을 샤드들. 아무도 안 보는 종목이면 0 — 호출자가 기본 열(해시)로 보내 현재가 캐시는 채운다.
    [[nodiscard]] ShardMask mask(symbol::SymbolId id) const noexcept
    {
        // [inv] 여기서 acquire 하나로 표 전체가 보인다 — 아래 masks는 평범한 읽기다.
        const Snapshot* snapshot = current_.load(std::memory_order_acquire);

        if (id == symbol::kNone || id >= snapshot->masks.size())
        {
            return snapshot->all;
        }

        return snapshot->masks[id] | snapshot->all;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return current_.load(std::memory_order_acquire)->masks.size();
    }

private:
    // 한 번 만들면 안 바뀐다. 바꿀 때는 새로 만들어 포인터를 옮긴다.
    struct Snapshot
    {
        std::vector<ShardMask> masks;
        ShardMask              all = 0;

        explicit Snapshot(size_t symbol_capacity) : masks(symbol_capacity, 0) {}
    };

    // [inv] 지난 표는 안 지운다 — 읽는 스레드가 아직 그 포인터를 들고 있을 수 있고, 여기엔 그것을 기다릴 장치가 없다.
    //  표 하나가 종목 8,192개 기준 64KB고 새로 만드는 것은 전략이 늘거나 줄 때뿐이라(기동 때 전략 수만큼) 수 MB에서 멈춘다.
    void publish(std::unique_ptr<Snapshot> snapshot)
    {
        const Snapshot* published = snapshot.get();

        retired_.push_back(std::move(snapshot));
        current_.store(published, std::memory_order_release);
    }

    std::vector<std::unique_ptr<Snapshot>> retired_;   // 지금 표와 지난 표 전부. 쓰는 쪽에서만 만진다.
    std::atomic<const Snapshot*>           current_{nullptr};
};

// 마스크의 샤드를 낮은 번호부터 방문한다. 마스크가 0이면 fallback 하나만.
template <class Fn>
void for_each_shard(ShardMask mask, uint32_t fallback, Fn&& callback)
{
    if (mask == 0)
    {
        callback(fallback);
        return;
    }

    while (mask != 0)
    {
        uint32_t shard_index = 0;

        while (((mask >> shard_index) & 1u) == 0)
        {
            ++shard_index;
        }

        callback(shard_index);
        mask &= mask - 1; // 가장 낮은 1비트를 지운다
    }
}

} // namespace shard
