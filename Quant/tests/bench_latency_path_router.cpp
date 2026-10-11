// bench_latency_path — 3·4 라우터 디스패치와 전략 상태 키 전후.
//  공용 준비는 bench_latency_path_support.h, 실행 순서는 bench_latency_path.cpp의 main.
#include "bench_latency_path_support.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace latency_path_bench
{

// ─── 3. 라우터 — 전 전략 방문 vs 종목 id 디스패치 (780597a) ───────────────────────

void bench_router_n(const std::vector<std::string>& tickers, size_t n_strats)
{
    constexpr size_t kIters = 1'000'000;
    symbol::SymbolTable table(kSymbols + 1);
    std::vector<std::unique_ptr<OneSymStrategy>> owned;
    std::vector<StrategyBase*>                   pointers;

    for (size_t index = 0; index < n_strats; ++index)
    {
        owned.push_back(std::make_unique<OneSymStrategy>(tickers[index]));
        owned.back()->bind(table.intern(tickers[index]));
        pointers.push_back(owned.back().get());
    }

    strategy::Router router;
    router.rebuild(pointers, [&table](const std::string& ticker)
    {
        return table.intern(ticker);
    });

    std::vector<TradeData> ticks(n_strats);

    for (size_t index = 0; index < n_strats; ++index)
    {
        ticks[index].ticker = tickers[index];
        ticks[index].symbol_id    = table.intern(tickers[index]);
        ticks[index].price  = 1000.0 + static_cast<double>(index);
    }

    size_t size = 7;
    const double old_ns = ns_per_op(kIters, [&](size_t)
    {
        const auto& trade = ticks[next_index(size) % n_strats];

        for (auto* pointer : pointers)
        {
            (void)pointer->on_trade(trade);
        }
    });

    size = 7;
    const double new_ns = ns_per_op(kIters, [&](size_t)
    {
        const auto& trade = ticks[next_index(size) % n_strats];
        router.for_each(trade.symbol_id, [&trade](StrategyBase* strategy)
        {
            (void)strategy->on_trade(trade);
        });
    });

    char name[64];
    std::snprintf(name, sizeof(name), "전략 %zu개(종목 1개씩) 틱 1건 디스패치", n_strats);
    row(name, old_ns, new_ns);
}

void bench_router(const std::vector<std::string>& tickers)
{
    head("3. 라우터 — 전 전략 방문(sym 비교 후 반환) vs Router::for_each");
    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    bench_router_n(tickers, 4);
    bench_router_n(tickers, 40);
    bench_router_n(tickers, 400);
}

// ─── 4. 전략 상태 키 — string vs SymbolId (0039468) ──────────────────────────────
struct State
{
    double  moving_average = 0;
    int64_t count = 0;
};

void bench_state_key(const std::vector<std::string>& tickers)
{
    head("4. 전략 상태 — unordered_map<string> vs unordered_map<SymbolId> vs vector[id], 2,600키");
    constexpr size_t kIters = 2'000'000;

    std::unordered_map<std::string, State>   by_string;
    std::unordered_map<symbol::SymbolId, State> by_id;
    std::vector<State>                       by_index(kSymbols + 1);
    symbol::SymbolTable                         table(kSymbols + 1);
    std::vector<symbol::SymbolId>               ids;

    for (const auto& ticker : tickers)
    {
        by_string[ticker] = {};
        const auto id = table.intern(ticker);
        by_id[id]     = {};
        ids.push_back(id);
    }

    size_t size = 3;
    const double string_ns = ns_per_op(kIters, [&](size_t)
    {
        auto& state = by_string[tickers[next_index(size)]];
        state.moving_average += 1.0;
        ++state.count;
        g_sink += state.count;
    });

    size = 3;
    const double id_ns = ns_per_op(kIters, [&](size_t)
    {
        auto& state = by_id[ids[next_index(size)]];
        state.moving_average += 1.0;
        ++state.count;
        g_sink += state.count;
    });

    size = 3;
    const double index_ns = ns_per_op(kIters, [&](size_t)
    {
        auto& state = by_index[ids[next_index(size)]];
        state.moving_average += 1.0;
        ++state.count;
        g_sink += state.count;
    });

    // 전략이 틱마다 하던 "내 종목인가" 비교 — 여섯 자리 문자열 vs 정수.
    const std::string mine = tickers[10];
    const symbol::SymbolId mine_id = ids[10];
    size = 5;
    const double compare_string = ns_per_op(kIters, [&](size_t)
    {
        g_sink += (tickers[next_index(size)] != mine) ? 1 : 0;
    });

    size = 5;
    const double compare_id = ns_per_op(kIters, [&](size_t)
    {
        g_sink += (ids[next_index(size)] != mine_id) ? 1 : 0;
    });

    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    row("상태 맵 조회+갱신: string 키 vs SymbolId 키", string_ns, id_ns);
    row("상태 맵 조회+갱신: string 키 vs vector[id]", string_ns, index_ns);
    row("내 종목인가: 문자열 != vs 정수 !=", compare_string, compare_id);
}

} // namespace latency_path_bench
