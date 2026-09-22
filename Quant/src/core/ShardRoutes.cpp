#include "core/ShardRoutes.h"

namespace shard
{
RouteTable::RouteTable(size_t symbol_capacity)
{
    publish(std::make_unique<Snapshot>(symbol_capacity));
}

void RouteTable::reset(size_t symbol_capacity)
{
    publish(std::make_unique<Snapshot>(symbol_capacity));
}

void RouteTable::Draft::add(symbol::SymbolId id, uint32_t shard_index)
{
    if (id != symbol::kNone && id < masks.size())
    {
        masks[id] |= mask_of(shard_index);
    }
}

void RouteTable::Draft::add_all(uint32_t shard_index)
{
    all |= mask_of(shard_index);
}

void RouteTable::commit(const Draft& draft)
{
    auto snapshot = std::make_unique<Snapshot>(draft.masks.size());
    snapshot->masks = draft.masks;
    snapshot->all = draft.all;
    publish(std::move(snapshot));
}

ShardMask RouteTable::mask(symbol::SymbolId id) const noexcept
{
    // [inv] 여기서 acquire 하나로 표 전체가 보인다 — 아래 masks는 평범한 읽기다.
    const Snapshot* snapshot = current_.load(std::memory_order_acquire);

    if (id == symbol::kNone || id >= snapshot->masks.size())
    {
        return snapshot->all;
    }

    return snapshot->masks[id] | snapshot->all;
}

void RouteTable::publish(std::unique_ptr<Snapshot> snapshot)
{
    const Snapshot* published = snapshot.get();

    retired_.push_back(std::move(snapshot));
    current_.store(published, std::memory_order_release);
}

} // namespace shard
