// StrategyRouter 단위 테스트 — 종목 id 라우팅, 구독 미표기 전략의 전부 수신, 테이블 가득 참 대체, 중복 제거, 재구성.
//  끝에 전략 40개 × 틱 20만 건으로 "전부 순회 + 문자열 비교"와 라우터의 틱당 시간을 재서 찍는다(단정하지 않음).
// 빌드: cmake --build <dir> --target test_strategy_router
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

// 구독 종목 목록만 밝히는 가짜 전략. on_trade는 오늘 전략들처럼 자기 종목이 아니면 문자열 비교로 거른다.
class FakeStrategy final : public StrategyBase
{
public:
    FakeStrategy(std::string id, std::vector<std::string> tickers) : id_(std::move(id)), tickers_(std::move(tickers)) {}

    std::string id() const override
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

    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        for (const auto& t : tickers_)
        {
            if (td.ticker == t)
            {
                ++hits;
                break;
            }
        }

        return std::nullopt;
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        std::vector<WatchSpec> v;

        for (const auto& t : tickers_)
        {
            v.push_back({t, Market::KR, ""});
        }

        return v;
    }

    int hits = 0;

private:
    std::string              id_;
    std::vector<std::string> tickers_;
};

std::vector<StrategyBase*> ptrs(const std::vector<std::unique_ptr<FakeStrategy>>& v)
{
    std::vector<StrategyBase*> out;

    for (const auto& s : v)
    {
        out.push_back(s.get());
    }

    return out;
}
} // namespace

int main()
{
    // 1. 종목별 라우팅. A는 두 종목, B는 한 종목, C는 구독을 안 밝힘(전부 받는다).
    {
        sym::SymbolTable                           tab;
        std::vector<std::unique_ptr<FakeStrategy>> ss;
        ss.push_back(std::make_unique<FakeStrategy>("A", std::vector<std::string>{"005930", "000660"}));
        ss.push_back(std::make_unique<FakeStrategy>("B", std::vector<std::string>{"000660"}));
        ss.push_back(std::make_unique<FakeStrategy>("C", std::vector<std::string>{}));

        strat::Router r;
        r.rebuild(ptrs(ss), [&tab](const std::string& t) { return tab.intern(t); });
        CHECK(r.routes() == 3 && r.all_count() == 1);
        CHECK(tab.size() == 2);

        const auto samsung = tab.lookup("005930");
        const auto hynix   = tab.lookup("000660");
        CHECK(r.watchers(samsung) == 1 && r.watchers(hynix) == 2 && r.watchers(sym::kNone) == 0);

        std::vector<std::string> seen;
        r.for_each(samsung, [&seen](StrategyBase* s) { seen.push_back(s->id()); });
        CHECK(seen.size() == 2 && seen[0] == "A" && seen[1] == "C");

        seen.clear();
        r.for_each(hynix, [&seen](StrategyBase* s) { seen.push_back(s->id()); });
        CHECK(seen.size() == 3 && seen[0] == "A" && seen[1] == "B" && seen[2] == "C");

        // 모르는 id·kNone은 전부 받는 전략만.
        seen.clear();
        r.for_each(sym::kNone, [&seen](StrategyBase* s) { seen.push_back(s->id()); });
        CHECK(seen.size() == 1 && seen[0] == "C");
        seen.clear();
        r.for_each(999, [&seen](StrategyBase* s) { seen.push_back(s->id()); });
        CHECK(seen.size() == 1 && seen[0] == "C");
    }

    // 2. 같은 종목을 두 번 적은 전략은 한 번만 들어간다. 재구성하면 이전 배정은 사라진다.
    {
        sym::SymbolTable                           tab;
        std::vector<std::unique_ptr<FakeStrategy>> ss;
        ss.push_back(std::make_unique<FakeStrategy>("D", std::vector<std::string>{"005930", "005930"}));

        strat::Router r;
        r.rebuild(ptrs(ss), [&tab](const std::string& t) { return tab.intern(t); });
        CHECK(r.routes() == 1 && r.watchers(tab.lookup("005930")) == 1);

        ss.clear();
        ss.push_back(std::make_unique<FakeStrategy>("E", std::vector<std::string>{"000660"}));
        r.rebuild(ptrs(ss), [&tab](const std::string& t) { return tab.intern(t); });
        CHECK(r.routes() == 1 && r.watchers(tab.lookup("005930")) == 0 && r.watchers(tab.lookup("000660")) == 1);
        CHECK(r.all_count() == 0);
    }

    // 3. 테이블이 차서 kNone이 나오면 그 전략은 전부 받는 쪽으로 간다(다른 종목이 id를 받았어도 한쪽에만).
    {
        sym::SymbolTable                           tab(2);
        std::vector<std::unique_ptr<FakeStrategy>> ss;
        ss.push_back(std::make_unique<FakeStrategy>("F", std::vector<std::string>{"005930", "000660"}));

        strat::Router r;
        r.rebuild(ptrs(ss), [&tab](const std::string& t) { return tab.intern(t); });
        CHECK(tab.lookup("005930") != sym::kNone && tab.lookup("000660") == sym::kNone);
        CHECK(r.routes() == 0 && r.all_count() == 1 && r.watchers(tab.lookup("005930")) == 0);

        int n = 0;
        r.for_each(tab.lookup("005930"), [&n](StrategyBase*) { ++n; });
        CHECK(n == 1);
    }

    // 4. 측정(원칙 7). 전략 40개(종목 하나씩) × 틱 20만 건 — 전부 순회 vs 라우터. 결과는 같아야 한다.
    {
        constexpr int kStrats = 40;
        constexpr int kTicks  = 200'000;

        sym::SymbolTable                           tab;
        std::vector<std::unique_ptr<FakeStrategy>> ss;
        std::vector<TradeData>                     ticks(kStrats);

        for (int i = 0; i < kStrats; ++i)
        {
            const std::string tk = "0" + std::to_string(10000 + i);
            ss.push_back(std::make_unique<FakeStrategy>("S" + std::to_string(i), std::vector<std::string>{tk}));
            ticks[i].ticker = tk;
            ticks[i].sym    = tab.intern(tk);
        }

        auto ps = ptrs(ss);
        strat::Router r;
        r.rebuild(ps, [&tab](const std::string& t) { return tab.intern(t); });
        CHECK(r.routes() == kStrats && r.all_count() == 0);

        const auto t0 = std::chrono::steady_clock::now();

        for (int n = 0; n < kTicks; ++n)
        {
            const auto& td = ticks[n % kStrats];

            for (auto* s : ps)
            {
                (void)s->on_trade(td);
            }
        }

        const auto t1 = std::chrono::steady_clock::now();

        for (int n = 0; n < kTicks; ++n)
        {
            const auto& td = ticks[n % kStrats];
            r.for_each(td.sym, [&td](StrategyBase* s) { (void)s->on_trade(td); });
        }

        const auto t2 = std::chrono::steady_clock::now();

        for (const auto& s : ss)
        {
            CHECK(s->hits == 2 * kTicks / kStrats);
        }

        const auto ns_all = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / kTicks;
        const auto ns_rt  = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count() / kTicks;
        std::cout << "  틱당 전부 순회 " << ns_all << "ns, 라우터 " << ns_rt << "ns (전략 " << kStrats << "개)\n";
    }

    std::cout << "test_strategy_router: " << g_checks << " checks passed\n";
    return 0;
}
