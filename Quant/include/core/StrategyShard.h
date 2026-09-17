// 전략 샤드 — 링 행렬의 열 하나(호가·체결·봉)를 비우고 라우터로 그 종목을 보는 전략만 방문한다. 종목 해시로 나뉜 열이라
//  한 종목은 한 샤드만 지나고(원칙 2), 신호는 봉투에 담아 싱크로 넘길 뿐 순번·슬롯·교체·강제청산 같은 종목 횡단 판단은
//  디스패치 스레드 몫이다(원칙 4). 전략 객체는 샤드 안에서만 만진다 — 샤드가 여럿이면 전략 집합도 샤드마다 하나다.
// 스레드: 샤드 스레드 하나가 rebuild·step을 부른다. wake는 생산자가 push 뒤 notify한다.
//  [why D-071]
#pragma once

#include "core/ShardMatrix.h"
#include "core/StrategyRouter.h"
#include "core/SymbolTable.h"
#include "core/Types.h"
#include "core/WakeGate.h"
#include "strategy/StrategyBase.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace strategy
{
// 전략을 맡을 샤드. 구독 종목이 전부 한 열로 해시되면 그 열, 아니면(종목이 여러 열에 걸치거나 구독을 안 밝혔거나
//  아직 id가 없으면) 없음 — 그 전략은 샤드 둘이 같이 만지게 되므로 M>1로 띄우면 안 된다. M이 1이면 언제나 0.
//  종목마다 전략 하나인 지금 전략(DevScale_*·ITB_*)은 전부 한 열이다. [why D-071]
template <typename SymbolIdOf>
std::optional<uint32_t> owner_shard(const StrategyBase& strategy, uint32_t shards, SymbolIdOf&& symbol_id_of)
{
    if (shards <= 1)
    {
        return 0u;
    }

    const auto              specifications = strategy.get_watch_specifications();
    std::optional<uint32_t> owner;

    if (specifications.empty())
    {
        return std::nullopt;
    }

    for (const auto& watch_specification : specifications)
    {
        const symbol::SymbolId id = symbol_id_of(watch_specification.ticker);

        if (id == symbol::kNone)
        {
            return std::nullopt;
        }

        const uint32_t row = shard::shard_of(id, shards);

        if (owner && *owner != row)
        {
            return std::nullopt;
        }

        owner = row;
    }

    return owner;
}

// 샤드가 디스패치 스레드로 보내는 봉투. 게이트 판단에 필요한 전략 상태를 샤드 스레드에서 읽어 같이 싣는다 —
//  디스패치 스레드는 전략 객체를 보지 않는다.
struct Emitted
{
    OrderSignal signal;
    std::string strategy_id;
    bool        active = true; // StrategyBase::is_active() — 국면 축 AND 유니버스 축
};

// 샤드가 비우는 세 행렬. 생산자 행은 호출자가 정한다(수신 스레드·데이터 스레드).
struct ShardQueues
{
    shard::Matrix<OrderBook>&  order_book;
    shard::Matrix<TradeData>&  trade;
    shard::Matrix<MarketData>& bars;
};

class Shard
{
public:
    Shard(uint32_t index, ShardQueues shard_queues)
        : index_(index), queue_(shard_queues)
    {
    }

    Shard(const Shard&)            = delete;
    Shard& operator=(const Shard&) = delete;

    [[nodiscard]] uint32_t index() const noexcept
    {
        return index_;
    }

    [[nodiscard]] sync::WakeGate& wake() noexcept
    {
        return wake_;
    }

    [[nodiscard]] const Router& router() const noexcept
    {
        return router_;
    }

    // 전략 목록이 바뀔 때만(버전) 샤드 스레드에서. 샤드가 여럿이면 각자 자기 전략 집합으로 부른다.
    template <class SymbolIdOf>
    void rebuild(const std::vector<StrategyBase*>& strategies, uint64_t version, SymbolIdOf&& symbol_id_of)
    {
        router_.rebuild(strategies, std::forward<SymbolIdOf>(symbol_id_of));
        seen_version_.store(version, std::memory_order_release);
    }

    // 마지막으로 라우터에 반영한 전략 목록 버전. 뗀 전략은 모든 샤드가 이 값을 넘긴 뒤에 파기한다.
    [[nodiscard]] uint64_t seen_version() const noexcept
    {
        return seen_version_.load(std::memory_order_acquire);
    }

    // 세 열이 다 비었나 — 잠들기 전 술어.
    [[nodiscard]] bool empty() const noexcept
    {
        return queue_.order_book.empty(index_) && queue_.trade.empty(index_) && queue_.bars.empty(index_);
    }

    // 세 열 가운데 가장 높았던 셀 — [큐 고수위] 줄.
    [[nodiscard]] size_t high_water() const noexcept
    {
        size_t high_water = queue_.order_book.high_water(index_);
        high_water        = high_water < queue_.trade.high_water(index_) ? queue_.trade.high_water(index_) : high_water;
        return high_water < queue_.bars.high_water(index_) ? queue_.bars.high_water(index_) : high_water;
    }

    // 한 바퀴 — 호가 전부, 체결 전부, 봉 하나. 돌려주는 값은 하나라도 처리했나.
    //  emit(StrategyBase*, const OrderSignal&, tick_ns): 신호 봉투를 만드는 자리. tick_ns는 체결 경로만 0이 아니다.
    //  on_price(SymbolId, double): 체결마다 현재가 캐시. symbol_id_of(ticker): 생산자가 id를 안 찍은 틱(리플레이·옛 경로)만 부른다.
    template <class Emit, class OnPrice, class SymbolIdOf>
    bool step(Emit&& emit, OnPrice&& on_price, SymbolIdOf&& symbol_id_of)
    {
        bool did_work = false;

        while (auto option = queue_.order_book.pop(index_))
        {
            router_.for_each(option->symbol_id, [&](StrategyBase* strategy)
            {
                auto signal = strategy->on_order_book(*option);

                if (signal && signal->side != OrderSide::NONE)
                {
                    emit(strategy, *signal, int64_t{0});
                }

                // 다건 발주 경로 — 취소·정정은 side가 NONE이어도 통과한다(생명주기 액션).
                batch_buffer_.clear();
                strategy->on_order_book_batch(*option, batch_buffer_);

                for (auto& batch : batch_buffer_)
                {
                    if (batch.action != OrderAction::NEW || batch.side != OrderSide::NONE)
                    {
                        emit(strategy, batch, option->received_ns);
                    }
                }
            });

            did_work = true;
        }

        while (auto option = queue_.trade.pop(index_))
        {
            const symbol::SymbolId id = option->symbol_id != symbol::kNone ? option->symbol_id : symbol_id_of(option->ticker);
            option->symbol_id               = id; // 전략은 trade.symbol_id으로만 비교한다 — 여기서 한 번 채운다
            on_price(id, option->price);

            router_.for_each(id, [&](StrategyBase* strategy)
            {
                auto signal = strategy->on_trade(*option);

                if (signal && signal->side != OrderSide::NONE)
                {
                    emit(strategy, *signal, option->received_ns);
                }

                batch_buffer_.clear();
                strategy->on_trade_batch(*option, batch_buffer_);

                for (auto& batch : batch_buffer_)
                {
                    if (batch.action != OrderAction::NEW || batch.side != OrderSide::NONE)
                    {
                        emit(strategy, batch, option->received_ns);
                    }
                }
            });

            did_work = true;
        }

        if (auto option = queue_.bars.pop(index_))
        {
            router_.for_each(option->symbol_id, [&](StrategyBase* strategy)
            {
                auto signal = strategy->on_data(*option);

                if (signal && signal->side != OrderSide::NONE)
                {
                    emit(strategy, *signal, int64_t{0});
                }
            });

            did_work = true;
        }

        return did_work;
    }

private:
    uint32_t                 index_;
    ShardQueues              queue_;
    Router                   router_;
    sync::WakeGate           wake_;
    std::atomic<uint64_t>    seen_version_{0};
    std::vector<OrderSignal> batch_buffer_; // 다건 발주 재사용 버퍼 — 틱마다 할당하지 않는다
};
} // namespace strategy
