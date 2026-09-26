// StrategyRouter 단위 테스트 — 종목 id 라우팅, 구독 미표기 전략의 전부 수신, 테이블 가득 참 대체, 중복 제거, 재구성.
//  끝에 전략 40개 × 틱 20만 건으로 "전부 순회 + 문자열 비교"와 라우터의 틱당 시간을 재서 찍고, 문자열이 남은
//  OrderSignal이 링을 지나는 신호당 시간도 같이 찍는다(단정하지 않음).
// 빌드: cmake --build <directory> --target test_strategy_router
#include "core/RingBuffer.h"
#include "core/StrategyRouter.h"
#include "core/SymbolTable.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
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

// 구독 종목 목록만 밝히는 가짜 전략. on_trade는 오늘 전략들처럼 자기 종목이 아니면 문자열 비교로 거른다.
class FakeStrategy final : public StrategyBase
{
public:
    FakeStrategy(std::string id, std::vector<std::string> tickers) : id_(std::move(id)), tickers_(std::move(tickers)) {}

    const std::string& id() const override
    {
        return id_;
    }

    std::string describe() const override
    {
        return id_;
    }

    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        for (const auto& ticker : tickers_)
        {
            if (trade.ticker == ticker)
            {
                ++hits;
                break;
            }
        }

        return std::nullopt;
    }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        std::vector<WatchSpec> values;

        for (const auto& ticker : tickers_)
        {
            values.push_back({ticker, Market::KR, ""});
        }

        return values;
    }

    int hits = 0;

private:
    std::string              id_;
    std::vector<std::string> tickers_;
};

std::vector<StrategyBase*> pointers(const std::vector<std::unique_ptr<FakeStrategy>>& values)
{
    std::vector<StrategyBase*> out;

    for (const auto& strategy : values)
    {
        out.push_back(strategy.get());
    }

    return out;
}
} // namespace

int main()
{
    // 1. 종목별 라우팅. A는 두 종목, B는 한 종목, C는 구독을 안 밝힘(전부 받는다).
    {
        symbol::SymbolTable                           table;
        std::vector<std::unique_ptr<FakeStrategy>> strategies;
        strategies.push_back(std::make_unique<FakeStrategy>("A", std::vector<std::string>{"005930", "000660"}));
        strategies.push_back(std::make_unique<FakeStrategy>("B", std::vector<std::string>{"000660"}));
        strategies.push_back(std::make_unique<FakeStrategy>("C", std::vector<std::string>{}));

        strategy::Router router;
        router.rebuild(pointers(strategies), [&table](const std::string& ticker) { return table.intern(ticker); });
        CHECK(router.routes() == 3 && router.all_count() == 1);
        CHECK(table.size() == 2);

        const auto samsung = table.lookup("005930");
        const auto hynix   = table.lookup("000660");
        CHECK(router.watchers(samsung) == 1 && router.watchers(hynix) == 2 && router.watchers(symbol::kNone) == 0);

        std::vector<std::string> seen;
        router.for_each(samsung, [&seen](StrategyBase* strategy) { seen.push_back(strategy->id()); });
        CHECK(seen.size() == 2 && seen[0] == "A" && seen[1] == "C");

        seen.clear();
        router.for_each(hynix, [&seen](StrategyBase* strategy) { seen.push_back(strategy->id()); });
        CHECK(seen.size() == 3 && seen[0] == "A" && seen[1] == "B" && seen[2] == "C");

        // 모르는 id·kNone은 전부 받는 전략만.
        seen.clear();
        router.for_each(symbol::kNone, [&seen](StrategyBase* strategy) { seen.push_back(strategy->id()); });
        CHECK(seen.size() == 1 && seen[0] == "C");
        seen.clear();
        router.for_each(999, [&seen](StrategyBase* strategy) { seen.push_back(strategy->id()); });
        CHECK(seen.size() == 1 && seen[0] == "C");
    }

    // 2. 같은 종목을 두 번 적은 전략은 한 번만 들어간다. 재구성하면 이전 배정은 사라진다.
    {
        symbol::SymbolTable                           table;
        std::vector<std::unique_ptr<FakeStrategy>> strategies;
        strategies.push_back(std::make_unique<FakeStrategy>("D", std::vector<std::string>{"005930", "005930"}));

        strategy::Router router;
        router.rebuild(pointers(strategies), [&table](const std::string& ticker) { return table.intern(ticker); });
        CHECK(router.routes() == 1 && router.watchers(table.lookup("005930")) == 1);

        strategies.clear();
        strategies.push_back(std::make_unique<FakeStrategy>("E", std::vector<std::string>{"000660"}));
        router.rebuild(pointers(strategies), [&table](const std::string& ticker) { return table.intern(ticker); });
        CHECK(router.routes() == 1 && router.watchers(table.lookup("005930")) == 0 && router.watchers(table.lookup("000660")) == 1);
        CHECK(router.all_count() == 0);
    }

    // 3. 테이블이 차서 kNone이 나오면 그 전략은 전부 받는 쪽으로 간다(다른 종목이 id를 받았어도 한쪽에만).
    {
        symbol::SymbolTable                           table(2);
        std::vector<std::unique_ptr<FakeStrategy>> strategies;
        strategies.push_back(std::make_unique<FakeStrategy>("F", std::vector<std::string>{"005930", "000660"}));

        strategy::Router router;
        router.rebuild(pointers(strategies), [&table](const std::string& ticker) { return table.intern(ticker); });
        CHECK(table.lookup("005930") != symbol::kNone && table.lookup("000660") == symbol::kNone);
        CHECK(router.routes() == 0 && router.all_count() == 1 && router.watchers(table.lookup("005930")) == 0);

        int count = 0;
        router.for_each(table.lookup("005930"), [&count](StrategyBase*) { ++count; });
        CHECK(count == 1);
    }

    // 4. 측정(원칙 7). 전략 40개(종목 하나씩) × 틱 20만 건 — 전부 순회 vs 라우터. 결과는 같아야 한다.
    {
        constexpr int kStrats = 40;
        constexpr int kTicks  = 200'000;

        symbol::SymbolTable                           table;
        std::vector<std::unique_ptr<FakeStrategy>> strategies;
        std::vector<TradeData>                     ticks(kStrats);

        for (int index = 0; index < kStrats; ++index)
        {
            const std::string ticker = "0" + std::to_string(10000 + index);
            strategies.push_back(std::make_unique<FakeStrategy>("S" + std::to_string(index), std::vector<std::string>{ticker}));
            ticks[index].ticker = ticker;
            ticks[index].symbol_id    = table.intern(ticker);
        }

        auto ps = pointers(strategies);
        strategy::Router router;
        router.rebuild(ps, [&table](const std::string& ticker) { return table.intern(ticker); });
        CHECK(router.routes() == kStrats && router.all_count() == 0);

        const auto start_time = std::chrono::steady_clock::now();

        for (int tick_index = 0; tick_index < kTicks; ++tick_index)
        {
            const auto& trade = ticks[tick_index % kStrats];

            for (auto* strategy : ps)
            {
                (void)strategy->on_trade(trade);
            }
        }

        const auto end_time = std::chrono::steady_clock::now();

        for (int tick_index = 0; tick_index < kTicks; ++tick_index)
        {
            const auto& trade = ticks[tick_index % kStrats];
            router.for_each(trade.symbol_id, [&trade](StrategyBase* strategy) { (void)strategy->on_trade(trade); });
        }

        const auto later_time = std::chrono::steady_clock::now();

        for (const auto& strategy : strategies)
        {
            CHECK(strategy->hits == 2 * kTicks / kStrats);
        }

        const auto ns_all = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() / kTicks;
        const auto ns_rt  = std::chrono::duration_cast<std::chrono::nanoseconds>(later_time - end_time).count() / kTicks;
        std::cout << "  틱당 전부 순회 " << ns_all << "ns, 라우터 " << ns_rt << "ns (전략 " << kStrats << "개)\n";
    }

    // 5. 측정(원칙 7). 문자열 필드가 남은 OrderSignal을 신호 링(push 복사 → pop 이동)으로 10만 건 보내는 신호당 시간.
    //  신호는 틱보다 훨씬 드물어 이 값이 틱당 라우터 시간의 수십 배여도 hot path에 안 보인다 — 문자열을 남긴 근거.
    {
        constexpr int kSignals = 100'000;
        RingBuffer<OrderSignal> ring(1024);

        OrderSignal signal;
        signal.ticker      = "005930";
        signal.symbol_id         = 1;
        signal.side        = OrderSide::BUY;
        signal.quantity    = 10;
        signal.strategy_id = "deviation_scale";
        signal.client_order_id  = "ds-20260913-000001";
        signal.reason      = "ma5>ma10>ma20, dev -3.1%";

        int         popped = 0;
        const auto  start_time     = std::chrono::steady_clock::now();

        for (int signal_index = 0; signal_index < kSignals; ++signal_index)
        {
            signal.sequence = static_cast<uint64_t>(signal_index);
            CHECK(ring.push(signal));

            if (auto popped_signal = ring.pop())
            {
                popped += popped_signal->symbol_id == 1 ? 1 : 0;
            }
        }

        const auto end_time = std::chrono::steady_clock::now();
        CHECK(popped == kSignals);
        const auto ns_signal = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() / kSignals;
        std::cout << "  신호당 OrderSignal 링 push+pop " << ns_signal << "ns (문자열 4개, " << sizeof(OrderSignal) << "B)\n";
    }

    std::cout << "test_strategy_router: " << g_checks << " checks passed\n";
    return 0;
}
