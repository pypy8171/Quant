// StrategyShard 단위 테스트 — 열 하나를 비우는 순서(호가→체결→봉 하나), 샤드마다 다른 전략 집합의 라우팅, 신호 봉투의
//  tick_ns·active·전략 id, id 없는 틱의 대체 조회, 현재가 콜백, 다건 발주의 NONE 걸러내기, 버전·고수위, 그리고
//  전략 계산이 든 틱을 샤드 1·2·4가 나눠 받을 때의 벽시계 측정(원칙 7).
// 빌드: cmake --build <dir> --target test_strategy_shard
#include "core/ShardMatrix.h"
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

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
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

    std::string id() const override
    {
        return id_;
    }

    std::string describe() const override
    {
        return id_;
    }

    std::optional<OrderSignal> on_data(const MarketData& md) override
    {
        ++bars;
        last_bar_sym = md.sym;
        return std::nullopt;
    }

    std::optional<OrderSignal> on_order_book(const OrderBook& ob) override
    {
        ++books;
        last_book_sym = ob.sym;
        return std::nullopt;
    }

    void on_order_book_batch(const OrderBook& ob, std::vector<OrderSignal>& out) override
    {
        OrderSignal cancel;
        cancel.action = OrderAction::CANCEL;
        cancel.side   = OrderSide::NONE;
        cancel.ticker = ob.ticker.str();
        cancel.sym    = ob.sym;
        out.push_back(cancel);
        OrderSignal none;
        none.action = OrderAction::NEW;
        none.side   = OrderSide::NONE;
        none.ticker = ob.ticker.str();
        out.push_back(none);
    }

    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        ++trades;
        seen.emplace_back(td.sym, static_cast<uint32_t>(td.acml_volume));

        if (burn_rounds_ > 0)
        {
            // 전략 계산 흉내 — xorshift 몇 바퀴. 결과를 멤버에 접어 최적화로 사라지지 않게 한다.
            uint64_t x = (static_cast<uint64_t>(td.sym) * 0x9E3779B97F4A7C15ull) ^ static_cast<uint64_t>(td.acml_volume + 1);

            for (int i = 0; i < burn_rounds_; ++i)
            {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
            }

            acc += x; // xorshift는 GF(2) 선형이라 xor로 접으면 짝수 번 반복이 0이 된다
            return std::nullopt;
        }

        OrderSignal sig;
        sig.side        = OrderSide::BUY;
        sig.type        = OrderType::MARKET;
        sig.quantity    = 1;
        sig.ticker      = td.ticker.str();
        sig.strategy_id = id_;
        sig.sym         = td.sym;
        return sig;
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        std::vector<WatchSpec> v;

        for (const auto& t : tickers_)
        {
            WatchSpec w;
            w.ticker = t;
            v.push_back(w);
        }

        return v;
    }

    int                                             bars          = 0;
    int                                             books         = 0;
    int                                             trades        = 0;
    sym::SymbolId                                   last_bar_sym  = sym::kNone;
    sym::SymbolId                                   last_book_sym = sym::kNone;
    uint64_t                                        acc           = 0;
    std::vector<std::pair<sym::SymbolId, uint32_t>> seen;

private:
    std::string              id_;
    std::vector<std::string> tickers_;
    int                      burn_rounds_;
};

// 샤드 스레드가 전부 받을 때까지 도는 헬퍼 — 봉투 수와 마지막 tick_ns를 센다.
struct Sink
{
    std::vector<strat::Emitted> out;
    int64_t                     last_tick_ns = -1;
};

TradeData make_td(sym::SymbolId id, const std::string& ticker, uint32_t seq, double px, int64_t recv_ns)
{
    TradeData td;
    td.sym         = id;
    td.ticker      = sym::Ticker(ticker);
    td.acml_volume = seq;
    td.price       = px;
    td.recv_ns     = recv_ns;
    return td;
}
} // namespace

int main()
{
    sym::SymbolTable table;
    const auto       sym_of = [&](std::string_view t) { return table.intern(t); };

    // 1. 두 샤드 — 각자 다른 전략 집합. 구독 전략은 자기 종목이 자기 샤드로 올 때만, 전부 받는 전략은 그 샤드 종목 전부.
    //  같은 종목의 순서가 지켜지고 봉투는 recv_ns·active·전략 id를 싣고 현재가 콜백은 체결마다 온다.
    {
        shard::Matrix<OrderBook>  ob(1, 2, 64);
        shard::Matrix<TradeData>  td(1, 2, 64);
        shard::Matrix<MarketData> bars(1, 2, 8);
        strat::Shard              s0(0, {ob, td, bars});
        strat::Shard              s1(1, {ob, td, bars});
        CHECK(s0.index() == 0 && s1.index() == 1);
        CHECK(s0.empty() && s1.empty());

        std::vector<std::string> names;
        std::vector<sym::SymbolId> ids;

        for (int i = 0; i < 12; ++i)
        {
            names.push_back("00000" + std::to_string(i));
            ids.push_back(table.intern(names.back()));
        }

        // 구독 전략 A는 종목 0·1·2를 본다. 샤드마다 복제한다(같은 id, 다른 객체).
        FakeStrategy a0("A", {names[0], names[1], names[2]}), a1("A", {names[0], names[1], names[2]});
        FakeStrategy all0("ALL", {}), all1("ALL", {});
        all1.set_active(false); // 샤드 1의 ALL은 비활성 — 봉투에 실려야 한다
        s0.rebuild({&a0, &all0}, 7, sym_of);
        s1.rebuild({&a1, &all1}, 7, sym_of);
        CHECK(s0.seen_version() == 7 && s1.seen_version() == 7);
        CHECK(s0.router().all_count() == 1 && s1.router().all_count() == 1);

        // 종목 12개 × 3건. 생산자는 push로 종목 해시 열에 넣는다.
        for (uint32_t k = 0; k < 3; ++k)
        {
            for (size_t i = 0; i < ids.size(); ++i)
            {
                CHECK(td.push(0, ids[i], make_td(ids[i], names[i], k, 100.0 + static_cast<double>(i), 1000 + k)));
            }
        }

        CHECK(!s0.empty() || !s1.empty());
        Sink                       sink0, sink1;
        std::vector<sym::SymbolId> priced0, priced1;
        const auto                 run = [&](strat::Shard& s, Sink& sink, std::vector<sym::SymbolId>& priced)
        {
            while (s.step(
                [&](StrategyBase* st, const OrderSignal& sig, int64_t tick_ns)
                {
                    strat::Emitted e;
                    e.sig           = sig;
                    e.sig.t_tick_ns = tick_ns;
                    e.strategy_id   = st->id();
                    e.active        = st->is_active();
                    sink.out.push_back(std::move(e));
                    sink.last_tick_ns = tick_ns;
                },
                [&](sym::SymbolId id, double) { priced.push_back(id); }, sym_of))
            {
            }
        };
        run(s0, sink0, priced0);
        run(s1, sink1, priced1);
        CHECK(s0.empty() && s1.empty());

        // 전부 받는 전략은 자기 샤드의 종목만, 총합이 36건. 구독 전략은 0·1·2가 해시로 간 샤드에서만.
        CHECK(all0.trades + all1.trades == 36);
        CHECK(priced0.size() + priced1.size() == 36);
        int a_expected0 = 0, a_expected1 = 0;

        for (size_t i = 0; i < 3; ++i)
        {
            (shard::shard_of(ids[i], 2) == 0 ? a_expected0 : a_expected1) += 3;
        }

        CHECK(a0.trades == a_expected0 && a1.trades == a_expected1);

        for (const auto& [id, seq] : all0.seen)
        {
            CHECK(shard::shard_of(id, 2) == 0);
        }

        for (const auto& [id, seq] : all1.seen)
        {
            CHECK(shard::shard_of(id, 2) == 1);
        }

        // 종목 안 순번 0,1,2 — 샤드 0의 첫 종목으로 본다.
        {
            std::vector<uint32_t> seqs;
            const auto            first = all0.seen.front().first;

            for (const auto& [id, seq] : all0.seen)
            {
                if (id == first)
                {
                    seqs.push_back(seq);
                }
            }

            CHECK(seqs.size() == 3 && seqs[0] == 0 && seqs[1] == 1 && seqs[2] == 2);
        }

        // 봉투 — 체결마다 전략마다 BUY 하나. tick_ns는 recv_ns, 샤드 1의 ALL은 active=false.
        CHECK(sink0.out.size() == static_cast<size_t>(a0.trades + all0.trades));
        CHECK(sink1.out.size() == static_cast<size_t>(a1.trades + all1.trades));
        CHECK(sink0.last_tick_ns == 1002 && sink1.last_tick_ns == 1002);
        bool inactive_seen = false, id_ok = true;

        for (const auto& e : sink1.out)
        {
            if (e.strategy_id == "ALL")
            {
                inactive_seen = inactive_seen || !e.active;
            }

            id_ok = id_ok && (e.strategy_id == "A" || e.strategy_id == "ALL") && e.sig.side == OrderSide::BUY;
        }

        CHECK(inactive_seen && id_ok);
        CHECK(s0.high_water() >= 1 && s1.high_water() >= 1);
    }

    // 2. 한 바퀴의 순서 — 호가 전부, 체결 전부, 봉은 하나만. 호가 봉투는 tick_ns 0, 다건 경로의 CANCEL은 통과하고
    //  NEW+NONE은 걸러진다. id 없는 틱(리플레이·옛 경로)은 sym_of로 채운다.
    {
        shard::Matrix<OrderBook>  ob(1, 1, 8);
        shard::Matrix<TradeData>  td(1, 1, 8);
        shard::Matrix<MarketData> bars(1, 1, 8);
        strat::Shard              s(0, {ob, td, bars});
        FakeStrategy              all("ALL", {});
        s.rebuild({&all}, 1, sym_of);
        const auto id = table.intern("005930");

        OrderBook b;
        b.sym    = id;
        b.ticker = sym::Ticker("005930");
        CHECK(ob.push(0, id, b));
        CHECK(ob.push(0, id, b));
        TradeData t = make_td(sym::kNone, "005930", 0, 70000.0, 55);
        CHECK(td.push_to(0, 0, t));
        MarketData m1, m2;
        m1.sym = id;
        m2.sym = id;
        CHECK(bars.push(0, id, m1));
        CHECK(bars.push(0, id, m2));

        std::vector<std::string> order;
        int                      priced = 0;
        const auto               emit   = [&](StrategyBase*, const OrderSignal& sig, int64_t tick_ns)
        {
            order.push_back((sig.action == OrderAction::CANCEL ? "C" : "B") + std::to_string(tick_ns));
        };
        CHECK(s.step(emit, [&](sym::SymbolId got, double px) { priced += (got == id && px == 70000.0) ? 1 : 0; }, sym_of));
        // 호가 2건 → CANCEL 둘(tick 0), 체결 1건 → BUY(tick 55), 봉 하나.
        CHECK(order.size() == 3 && order[0] == "C0" && order[1] == "C0" && order[2] == "B55");
        CHECK(all.books == 2 && all.trades == 1 && all.bars == 1 && priced == 1);
        CHECK(all.seen.size() == 1 && all.seen[0].first == id); // kNone이 id로 채워졌다
        CHECK(!s.empty());                                        // 봉 하나 남았다
        CHECK(s.step(emit, [&](sym::SymbolId, double) {}, sym_of));
        CHECK(all.bars == 2 && s.empty());
        CHECK(!s.step(emit, [&](sym::SymbolId, double) {}, sym_of));
    }

    // 3. 측정(원칙 7) — 전략 계산이 든 틱(종목 240 × 1,000건, 틱당 xorshift 300바퀴)을 샤드 1·2·4가 나눠 받을 때 벽시계.
    //  샤드마다 전부 받는 전략 하나(복제). 코어 수에 따라 달라 값은 판정하지 않는다.
    //  09-13 이 머신(논리 코어 16, Release) 세 번 기록: 샤드 1개 103~111ms, 2개 52~54ms, 4개 26~28ms — 샤드 수만큼
    //  줄었다. 행렬 조각(test_shard_matrix 5절)이 못 보여 준 값이 이것이다: 큐가 아니라 전략 계산이 M으로 나뉜다.
    {
        std::cout << "[측정] hardware_concurrency=" << std::thread::hardware_concurrency() << '\n';
        constexpr uint32_t kSymbols = 240, kPer = 1000, kBurn = 300;
        std::vector<sym::SymbolId> ids;

        for (uint32_t i = 0; i < kSymbols; ++i)
        {
            ids.push_back(table.intern("M" + std::to_string(i)));
        }

        for (uint32_t M : {1u, 2u, 4u})
        {
            shard::Matrix<OrderBook>  ob(1, M, 16);
            shard::Matrix<TradeData>  td(1, M, 2048);
            shard::Matrix<MarketData> bars(1, M, 16);
            std::vector<std::unique_ptr<strat::Shard>>  shards;
            std::vector<std::unique_ptr<FakeStrategy>>  strategies;
            std::vector<uint64_t>                       expected(M, 0);

            for (uint32_t i = 0; i < kSymbols; ++i)
            {
                expected[shard::shard_of(ids[i], M)] += kPer;
            }

            for (uint32_t m = 0; m < M; ++m)
            {
                shards.push_back(std::make_unique<strat::Shard>(m, strat::ShardQueues{ob, td, bars}));
                strategies.push_back(std::make_unique<FakeStrategy>("ALL", std::vector<std::string>{}, kBurn));
                shards.back()->rebuild({strategies.back().get()}, 1, sym_of);
            }

            std::atomic<bool>        go{false};
            std::vector<std::thread> threads;

            for (uint32_t m = 0; m < M; ++m)
            {
                threads.emplace_back([&, m]
                {
                    auto& s  = *shards[m];
                    auto& st = *strategies[m];

                    while (!go.load(std::memory_order_acquire))
                    {
                    }

                    while (static_cast<uint64_t>(st.trades) < expected[m])
                    {
                        if (!s.step([](StrategyBase*, const OrderSignal&, int64_t) {}, [](sym::SymbolId, double) {},
                                    sym_of))
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

                for (uint32_t k = 0; k < kPer; ++k)
                {
                    for (uint32_t i = 0; i < kSymbols; ++i)
                    {
                        const TradeData t = make_td(ids[i], "M", k, 1.0, 0);

                        while (!td.push(0, ids[i], t))
                        {
                            std::this_thread::yield();
                        }
                    }
                }
            });

            const auto t0 = std::chrono::steady_clock::now();
            go.store(true, std::memory_order_release);
            producer.join();

            for (auto& t : threads)
            {
                t.join();
            }

            const auto t1 = std::chrono::steady_clock::now();
            uint64_t   total = 0, acc = 0;

            for (uint32_t m = 0; m < M; ++m)
            {
                total += static_cast<uint64_t>(strategies[m]->trades);
                acc += strategies[m]->acc;
            }

            CHECK(total == static_cast<uint64_t>(kSymbols) * kPer);
            const double ms = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1e3;
            std::cout << "[측정] 샤드 " << M << "개: 틱 " << total << "건 전체 " << ms << "ms (acc " << (acc & 0xff)
                      << ")\n";
        }
    }

    // 4. owner_shard — 전략이 어느 샤드 것인가. M=1이면 언제나 0. 종목 하나면 그 종목의 열, 여럿이 같은 열이면 그 열,
    //  열이 갈리거나 구독을 안 밝혔거나 id를 못 받으면 없음(M>1로 띄우면 안 되는 전략).
    {
        std::vector<std::string> same, split;
        const uint32_t           M   = 4;
        const auto               m0  = shard::shard_of(table.intern("A00001"), M);
        same.push_back("A00001");

        for (int i = 2; i < 40 && (same.size() < 3 || split.size() < 2); ++i)
        {
            const std::string t  = "A000" + std::to_string(10 + i);
            const auto        mm = shard::shard_of(table.intern(t), M);

            if (mm == m0 && same.size() < 3)
            {
                same.push_back(t);
            }
            else if (mm != m0 && split.size() < 2)
            {
                split.push_back(t);
            }
        }

        CHECK(same.size() == 3 && split.size() == 2);
        FakeStrategy one("one", {same[0]});
        FakeStrategy three("three", same);
        FakeStrategy spanning("spanning", {same[0], split[0]});
        FakeStrategy all("all", {});
        const auto   lookup = [&](std::string_view t) { return table.lookup(t); };

        CHECK(strat::owner_shard(one, 1, sym_of) == std::optional<uint32_t>(0u));
        CHECK(strat::owner_shard(spanning, 1, sym_of) == std::optional<uint32_t>(0u));
        CHECK(strat::owner_shard(all, 1, sym_of) == std::optional<uint32_t>(0u));
        CHECK(strat::owner_shard(one, M, sym_of) == std::optional<uint32_t>(m0));
        CHECK(strat::owner_shard(three, M, sym_of) == std::optional<uint32_t>(m0));
        CHECK(!strat::owner_shard(spanning, M, sym_of));
        CHECK(!strat::owner_shard(all, M, sym_of));
        FakeStrategy unknown("unknown", {"Z99999"});
        CHECK(!strat::owner_shard(unknown, M, lookup));
        CHECK(strat::owner_shard(unknown, M, sym_of).has_value());
    }

    std::cout << "test_strategy_shard: " << g_checks << " checks passed\n";
    return 0;
}
