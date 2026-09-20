// StrategyShard 단위 테스트 — 열 하나를 비우는 순서(호가→체결→봉 하나), 샤드마다 다른 전략 집합의 라우팅, 신호 봉투의
//  tick_ns·active·전략 id, id 없는 틱의 대체 조회, 현재가 콜백, 다건 발주의 NONE 걸러내기, 버전·고수위, 그리고
//  전략 계산이 든 틱을 샤드 1·2·4가 나눠 받을 때의 벽시계 측정(원칙 7).
// 빌드: cmake --build <directory> --target test_strategy_shard
#include "core/ShardMatrix.h"
#include "core/ShardRoutes.h"
#include "core/StrategyShard.h"
#include "core/SymbolTable.h"
#include "core/Types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 시험용 전략 — 구독 종목을 밝히면 그 종목만, 비우면 전부 받는다. 받은 틱의 (종목, 순번)을 기록하고
//  체결마다 BUY 신호를, 호가마다 다건 경로로 CANCEL(side NONE)과 NEW(side NONE) 하나씩 낸다.
class FakeStrategy final : public StrategyBase
{
public:
    FakeStrategy(std::string id, std::vector<std::string> tickers, int burn_rounds = 0)
        : id_(std::move(id)), tickers_(std::move(tickers)), burn_rounds_(burn_rounds)
    {
    }

    const std::string& id() const override
    {
        return id_;
    }

    std::string describe() const override
    {
        return id_;
    }

    std::optional<OrderSignal> on_data(const MarketData& market_data) override
    {
        ++bars;
        last_bar_symbol = market_data.symbol_id;
        return std::nullopt;
    }

    std::optional<OrderSignal> on_order_book(const OrderBook& order_book) override
    {
        ++books;
        last_book_symbol = order_book.symbol_id;
        return std::nullopt;
    }

    void on_order_book_batch(const OrderBook& order_book, std::vector<OrderSignal>& out) override
    {
        OrderSignal cancel;
        cancel.action = OrderAction::CANCEL;
        cancel.side   = OrderSide::NONE;
        cancel.ticker = order_book.ticker.string();
        cancel.symbol_id    = order_book.symbol_id;
        out.push_back(cancel);
        OrderSignal none;
        none.action = OrderAction::NEW;
        none.side   = OrderSide::NONE;
        none.ticker = order_book.ticker.string();
        out.push_back(none);
    }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        ++trades;
        seen.emplace_back(trade.symbol_id, static_cast<uint32_t>(trade.accumulated_volume));

        if (burn_rounds_ > 0)
        {
            // 전략 계산 흉내 — xorshift 몇 바퀴. 결과를 멤버에 접어 최적화로 사라지지 않게 한다.
            uint64_t x_value = (static_cast<uint64_t>(trade.symbol_id) * 0x9E3779B97F4A7C15ull) ^ static_cast<uint64_t>(trade.accumulated_volume + 1);

            for (int burn_round_index = 0; burn_round_index < burn_rounds_; ++burn_round_index)
            {
                x_value ^= x_value << 13;
                x_value ^= x_value >> 7;
                x_value ^= x_value << 17;
            }

            accumulator += x_value; // xorshift는 GF(2) 선형이라 xor로 접으면 짝수 번 반복이 0이 된다
            return std::nullopt;
        }

        OrderSignal signal;
        signal.side        = OrderSide::BUY;
        signal.type        = OrderType::MARKET;
        signal.quantity    = 1;
        signal.ticker      = trade.ticker.string();
        signal.strategy_id = id_;
        signal.symbol_id         = trade.symbol_id;
        return signal;
    }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        std::vector<WatchSpec> values;

        for (const auto& ticker : tickers_)
        {
            WatchSpec specification;
            specification.ticker = ticker;
            values.push_back(specification);
        }

        return values;
    }

    int                                             bars          = 0;
    int                                             books         = 0;
    int                                             trades        = 0;
    symbol::SymbolId                                   last_bar_symbol  = symbol::kNone;
    symbol::SymbolId                                   last_book_symbol = symbol::kNone;
    uint64_t                                        accumulator           = 0;
    std::vector<std::pair<symbol::SymbolId, uint32_t>> seen;

private:
    std::string              id_;
    std::vector<std::string> tickers_;
    int                      burn_rounds_;
};

// 샤드 스레드가 전부 받을 때까지 도는 헬퍼 — 봉투 수와 마지막 tick_ns를 센다.
struct Sink
{
    std::vector<strategy::Emitted> out;
    int64_t                     last_tick_ns = -1;
};

TradeData make_trade(symbol::SymbolId id, const std::string& ticker, uint32_t sequence, double price, int64_t received_ns)
{
    TradeData trade;
    trade.symbol_id         = id;
    trade.ticker      = symbol::Ticker(ticker);
    trade.accumulated_volume = sequence;
    trade.price       = price;
    trade.received_ns     = received_ns;
    return trade;
}
} // namespace

int main()
{
    symbol::SymbolTable table;
    const auto       symbol_id_of = [&](std::string_view ticker) { return table.intern(ticker); };

    // 1. 두 샤드 — 각자 다른 전략 집합. 구독 전략은 자기 종목이 자기 샤드로 올 때만, 전부 받는 전략은 그 샤드 종목 전부.
    //  같은 종목의 순서가 지켜지고 봉투는 received_ns·active·전략 id를 싣고 현재가 콜백은 체결마다 온다.
    {
        shard::Matrix<OrderBook>  order_book(1, 2, 64);
        shard::Matrix<TradeData>  trade(1, 2, 64);
        shard::Matrix<MarketData> bars(1, 2, 8);
        strategy::Shard              shard_a(0, {order_book, trade, bars});
        strategy::Shard              shard_b(1, {order_book, trade, bars});
        CHECK(shard_a.index() == 0 && shard_b.index() == 1);
        CHECK(shard_a.empty() && shard_b.empty());

        std::vector<std::string> names;
        std::vector<symbol::SymbolId> ids;

        for (int index = 0; index < 12; ++index)
        {
            names.push_back("00000" + std::to_string(index));
            ids.push_back(table.intern(names.back()));
        }

        // 구독 전략 A는 종목 0·1·2를 본다. 샤드마다 복제한다(같은 id, 다른 객체).
        FakeStrategy strategy_a_first("A", {names[0], names[1], names[2]}), strategy_a_second("A", {names[0], names[1], names[2]});
        FakeStrategy strategy_all_first("ALL", {}), strategy_all_second("ALL", {});
        strategy_all_second.set_active(false); // 샤드 1의 ALL은 비활성 — 봉투에 실려야 한다
        shard_a.rebuild({&strategy_a_first, &strategy_all_first}, 7, symbol_id_of);
        shard_b.rebuild({&strategy_a_second, &strategy_all_second}, 7, symbol_id_of);
        CHECK(shard_a.seen_version() == 7 && shard_b.seen_version() == 7);
        CHECK(shard_a.router().all_count() == 1 && shard_b.router().all_count() == 1);

        // 종목 12개 × 3건. 생산자는 push로 종목 해시 열에 넣는다.
        for (uint32_t innermost_index = 0; innermost_index < 3; ++innermost_index)
        {
            for (size_t ids_index = 0; ids_index < ids.size(); ++ids_index)
            {
                CHECK(trade.push(0, ids[ids_index], make_trade(ids[ids_index], names[ids_index], innermost_index, 100.0 + static_cast<double>(ids_index), 1000 + innermost_index)));
            }
        }

        CHECK(!shard_a.empty() || !shard_b.empty());
        Sink                       sink0, sink1;
        std::vector<symbol::SymbolId> priced0, priced1;
        const auto                 run = [&](strategy::Shard& shard, Sink& sink, std::vector<symbol::SymbolId>& priced)
        {
            while (shard.step(
                [&](StrategyBase* stop_token, const OrderSignal& signal, int64_t tick_ns)
                {
                    strategy::Emitted emitted;
                    emitted.signal           = signal;
                    emitted.signal.tick_at_ns = tick_ns;
                    emitted.strategy_id   = stop_token->id();
                    emitted.active        = stop_token->is_active();
                    sink.out.push_back(std::move(emitted));
                    sink.last_tick_ns = tick_ns;
                },
                [&](symbol::SymbolId id, double) { priced.push_back(id); }, symbol_id_of))
            {
            }
        };
        run(shard_a, sink0, priced0);
        run(shard_b, sink1, priced1);
        CHECK(shard_a.empty() && shard_b.empty());

        // 전부 받는 전략은 자기 샤드의 종목만, 총합이 36건. 구독 전략은 0·1·2가 해시로 간 샤드에서만.
        CHECK(strategy_all_first.trades + strategy_all_second.trades == 36);
        CHECK(priced0.size() + priced1.size() == 36);
        int a_expected0 = 0, a_expected1 = 0;

        for (size_t index = 0; index < 3; ++index)
        {
            (shard::shard_of(ids[index], 2) == 0 ? a_expected0 : a_expected1) += 3;
        }

        CHECK(strategy_a_first.trades == a_expected0 && strategy_a_second.trades == a_expected1);

        for (const auto& [id, sequence] : strategy_all_first.seen)
        {
            CHECK(shard::shard_of(id, 2) == 0);
        }

        for (const auto& [id, sequence] : strategy_all_second.seen)
        {
            CHECK(shard::shard_of(id, 2) == 1);
        }

        // 종목 안 순번 0,1,2 — 샤드 0의 첫 종목으로 본다.
        {
            std::vector<uint32_t> sequences;
            const auto            first = strategy_all_first.seen.front().first;

            for (const auto& [id, sequence] : strategy_all_first.seen)
            {
                if (id == first)
                {
                    sequences.push_back(sequence);
                }
            }

            CHECK(sequences.size() == 3 && sequences[0] == 0 && sequences[1] == 1 && sequences[2] == 2);
        }

        // 봉투 — 체결마다 전략마다 BUY 하나. tick_ns는 received_ns, 샤드 1의 ALL은 active=false.
        CHECK(sink0.out.size() == static_cast<size_t>(strategy_a_first.trades + strategy_all_first.trades));
        CHECK(sink1.out.size() == static_cast<size_t>(strategy_a_second.trades + strategy_all_second.trades));
        CHECK(sink0.last_tick_ns == 1002 && sink1.last_tick_ns == 1002);
        bool inactive_seen = false, id_ok = true;

        for (const auto& emitted : sink1.out)
        {
            if (emitted.strategy_id == "ALL")
            {
                inactive_seen = inactive_seen || !emitted.active;
            }

            id_ok = id_ok && (emitted.strategy_id == "A" || emitted.strategy_id == "ALL") && emitted.signal.side == OrderSide::BUY;
        }

        CHECK(inactive_seen && id_ok);
        CHECK(shard_a.high_water() >= 1 && shard_b.high_water() >= 1);
    }

    // 2. 한 바퀴의 순서 — 호가 전부, 체결 전부, 봉은 하나만. 호가 봉투는 tick_ns 0, 다건 경로의 CANCEL은 통과하고
    //  NEW+NONE은 걸러진다. id 없는 틱(리플레이·옛 경로)은 symbol_id_of로 채운다.
    {
        shard::Matrix<OrderBook>  order_book(1, 1, 8);
        shard::Matrix<TradeData>  trade(1, 1, 8);
        shard::Matrix<MarketData> bars(1, 1, 8);
        strategy::Shard              shard(0, {order_book, trade, bars});
        FakeStrategy              all("ALL", {});
        shard.rebuild({&all}, 1, symbol_id_of);
        const auto id = table.intern("005930");

        OrderBook other_order_book;
        other_order_book.symbol_id     = id;
        other_order_book.ticker  = symbol::Ticker("005930");
        other_order_book.received_ns = 7;
        CHECK(order_book.push(0, id, other_order_book));
        other_order_book.received_ns = 8;
        CHECK(order_book.push(0, id, other_order_book));
        TradeData tick = make_trade(symbol::kNone, "005930", 0, 70000.0, 55);
        CHECK(trade.push_to(0, 0, tick));
        MarketData m1, m2;
        m1.symbol_id = id;
        m2.symbol_id = id;
        CHECK(bars.push(0, id, m1));
        CHECK(bars.push(0, id, m2));

        std::vector<std::string> order;
        int                      priced = 0;
        const auto               emit   = [&](StrategyBase*, const OrderSignal& signal, int64_t tick_ns)
        {
            order.push_back((signal.action == OrderAction::CANCEL ? "C" : "B") + std::to_string(tick_ns));
        };
        CHECK(shard.step(emit, [&](symbol::SymbolId received, double price) { priced += (received == id && price == 70000.0) ? 1 : 0; }, symbol_id_of));
        // 호가 2건 → CANCEL 둘(tick은 호가의 received_ns 7·8), 체결 1건 → BUY(tick 55), 봉 하나.
        CHECK(order.size() == 3 && order[0] == "C7" && order[1] == "C8" && order[2] == "B55");
        CHECK(all.books == 2 && all.trades == 1 && all.bars == 1 && priced == 1);
        CHECK(all.seen.size() == 1 && all.seen[0].first == id); // kNone이 id로 채워졌다
        CHECK(!shard.empty());                                        // 봉 하나 남았다
        CHECK(shard.step(emit, [&](symbol::SymbolId, double) {}, symbol_id_of));
        CHECK(all.bars == 2 && shard.empty());
        CHECK(!shard.step(emit, [&](symbol::SymbolId, double) {}, symbol_id_of));
    }

    // 3. 측정(원칙 7) — 전략 계산이 든 틱(종목 240 × 1,000건, 틱당 xorshift 300바퀴)을 샤드 1·2·4가 나눠 받을 때 벽시계.
    //  샤드마다 전부 받는 전략 하나(복제). 코어 수에 따라 달라 값은 판정하지 않는다.
    //  09-13 이 머신(논리 코어 16, Release) 세 번 기록: 샤드 1개 103~111ms, 2개 52~54ms, 4개 26~28ms — 샤드 수만큼
    //  줄었다. 행렬 조각(test_shard_matrix 5절)이 못 보여 준 값이 이것이다: 큐가 아니라 전략 계산이 M으로 나뉜다.
    {
        std::cout << "[측정] hardware_concurrency=" << std::thread::hardware_concurrency() << '\n';
        constexpr uint32_t kSymbols = 240, kPer = 1000, kBurn = 300;
        std::vector<symbol::SymbolId> ids;

        for (uint32_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
        {
            ids.push_back(table.intern("M" + std::to_string(symbol_index)));
        }

        for (uint32_t row_count : {1u, 2u, 4u})
        {
            shard::Matrix<OrderBook>  order_book(1, row_count, 16);
            shard::Matrix<TradeData>  trade(1, row_count, 2048);
            shard::Matrix<MarketData> bars(1, row_count, 16);
            std::vector<std::unique_ptr<strategy::Shard>>  shards;
            std::vector<std::unique_ptr<FakeStrategy>>  strategies;
            std::vector<uint64_t>                       expected(row_count, 0);

            for (uint32_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
            {
                expected[shard::shard_of(ids[symbol_index], row_count)] += kPer;
            }

            for (uint32_t row = 0; row < row_count; ++row)
            {
                shards.push_back(std::make_unique<strategy::Shard>(row, strategy::ShardQueues{order_book, trade, bars}));
                strategies.push_back(std::make_unique<FakeStrategy>("ALL", std::vector<std::string>{}, kBurn));
                shards.back()->rebuild({strategies.back().get()}, 1, symbol_id_of);
            }

            std::atomic<bool>        go{false};
            std::vector<std::thread> threads;

            for (uint32_t row = 0; row < row_count; ++row)
            {
                threads.emplace_back([&, row]
                {
                    auto& shard  = *shards[row];
                    auto& stop_token = *strategies[row];

                    while (!go.load(std::memory_order_acquire))
                    {
                    }

                    while (static_cast<uint64_t>(stop_token.trades) < expected[row])
                    {
                        if (!shard.step([](StrategyBase*, const OrderSignal&, int64_t) {}, [](symbol::SymbolId, double) {},
                                    symbol_id_of))
                        {
                            std::this_thread::yield();
                        }
                    }
                });
            }

            std::thread producer([&]
            {
                while (!go.load(std::memory_order_acquire))
                {
                }

                for (uint32_t per_index = 0; per_index < kPer; ++per_index)
                {
                    for (uint32_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
                    {
                        const TradeData tick = make_trade(ids[symbol_index], "M", per_index, 1.0, 0);

                        while (!trade.push(0, ids[symbol_index], tick))
                        {
                            std::this_thread::yield();
                        }
                    }
                }
            });

            const auto start_time = std::chrono::steady_clock::now();
            go.store(true, std::memory_order_release);
            producer.join();

            for (auto& thread : threads)
            {
                thread.join();
            }

            const auto end_time = std::chrono::steady_clock::now();
            uint64_t   total = 0, accumulator = 0;

            for (uint32_t row = 0; row < row_count; ++row)
            {
                total += static_cast<uint64_t>(strategies[row]->trades);
                accumulator += strategies[row]->accumulator;
            }

            CHECK(total == static_cast<uint64_t>(kSymbols) * kPer);
            const double milliseconds = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count()) / 1e3;
            std::cout << "[측정] 샤드 " << row_count << "개: 틱 " << total << "건 전체 " << milliseconds << "ms (acc " << (accumulator & 0xff)
                      << ")\n";
        }
    }

    // 4. RouteTable — 종목 id → 그 종목을 보는 샤드 마스크. 전략은 샤드 하나가 갖고, 종목 하나를 여러 샤드의 전략이 보면
    //  마스크에 그 샤드들이 다 선다. 구독을 안 밝힌 전략은 모든 종목의 마스크에 자기 샤드를 더한다. 아무도 안 보면 0
    //  (호출자가 해시 열로 보낸다). [why D-110]
    {
        shard::RouteTable routes(table.capacity());
        const auto        id_a = table.intern("R00001");
        const auto        id_b = table.intern("R00002");
        const auto        id_c = table.intern("R00003");

        CHECK(routes.mask(id_a) == 0);
        CHECK(routes.mask(symbol::kNone) == 0);

        auto draft = routes.draft();
        draft.add(id_a, 0);  // 샤드0 전략이 A
        draft.add(id_a, 2);  // 샤드2 전략도 A — 걸치는 종목
        draft.add(id_b, 1);  // 샤드1 전략이 B
        routes.commit(draft);

        CHECK(routes.mask(id_a) == (shard::mask_of(0) | shard::mask_of(2)));
        CHECK(routes.mask(id_b) == shard::mask_of(1));
        CHECK(routes.mask(id_c) == 0);

        // 전부 받는 전략이 샤드3에 오면 모든 종목에 3이 더해진다. 다시 만들면 옛 항목은 사라진다.
        auto second = routes.draft();
        second.add(id_b, 1);
        second.add_all(3);
        routes.commit(second);

        CHECK(routes.mask(id_a) == shard::mask_of(3));
        CHECK(routes.mask(id_b) == (shard::mask_of(1) | shard::mask_of(3)));
        CHECK(routes.mask(symbol::kNone) == shard::mask_of(3));

        // for_each_shard — 낮은 번호부터, 0이면 fallback 하나만.
        std::vector<uint32_t> visited;
        shard::for_each_shard(shard::mask_of(5) | shard::mask_of(0) | shard::mask_of(63), 9, [&](uint32_t shard_index) { visited.push_back(shard_index); });
        CHECK((visited == std::vector<uint32_t>{0, 5, 63}));
        visited.clear();
        shard::for_each_shard(0, 9, [&](uint32_t shard_index) { visited.push_back(shard_index); });
        CHECK((visited == std::vector<uint32_t>{9}));
    }

    std::cout << "test_strategy_shard: " << g_checks << " checks passed\n";
    return 0;
}
