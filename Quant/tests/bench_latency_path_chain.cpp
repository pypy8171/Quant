// bench_latency_path — 7 디코드부터 on_trade까지 연쇄 지연.
//  공용 준비는 bench_latency_path_support.h, 실행 순서는 bench_latency_path.cpp의 main.
#include "bench_latency_path_support.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace latency_path_bench
{

// ─── 7. 연쇄 — 디코드 → intern → SPSC → 전략 스레드(캐시·라우터·on_trade) ──────────
// 09-13 조각을 실제 순서로 잇는다. 생산자는 WS 수신 콜백 자리, 소비자는 전략 스레드 자리.
void bench_chain_at(const std::vector<std::string>& tickers, double rate_per_s)
{
    const int64_t gap = rate_per_s > 0 ? static_cast<int64_t>(1e9 / rate_per_s) : 0;
    const size_t kTicks  = gap > 0 ? 300'000 : 1'000'000;
    constexpr size_t kStrategyCount = 40;

    std::vector<std::string> payloads;
    payloads.reserve(kSymbols);

    for (size_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
    {
        payloads.push_back(make_kr_trade_payload(tickers[symbol_index], 70000 + static_cast<int>(symbol_index)));
    }

    symbol::SymbolTable table(kSymbols + 1);
    std::vector<std::unique_ptr<OneSymStrategy>> owned;
    std::vector<StrategyBase*>                   pointers;

    for (size_t index = 0; index < kStrategyCount; ++index)
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
    auto array_price = std::make_unique<std::atomic<double>[]>(kSymbols + 1);

    RingBuffer<TradeData> queue(1u << 16);
    std::vector<double>   latencies;
    latencies.reserve(kTicks);
    std::atomic<bool> done{false};

    std::thread consumer([&]
    {
        size_t count = 0;

        while (count < kTicks)
        {
            auto option = queue.pop();

            if (!option)
            {
                if (done.load(std::memory_order_acquire) && queue.empty())
                {
                    break;
                }

                continue;
            }

            const symbol::SymbolId id = option->symbol_id;
            array_price[id].store(option->price, std::memory_order_relaxed);
            router.for_each(id, [&](StrategyBase* strategy)
            {
                (void)strategy->on_trade(*option);
            });
            latencies.push_back(static_cast<double>(now_ns() - option->received_ns));
            ++count;
        }
    });

    std::vector<std::string_view> fields;
    fields.reserve(64);
    size_t        size    = 13;
    const int64_t start_time   = now_ns();
    int64_t       next = start_time;

    for (size_t tick_index = 0; tick_index < kTicks; ++tick_index)
    {
        if (gap > 0)
        {
            spin_until(next);
            next += gap;
        }

        const std::string& payload = payloads[next_index(size)];
        kis_websocket::split_fields(payload, '^', fields);
        TradeData trade;
        (void)kis_websocket::decode_kr_trade(fields, trade);
        trade.received_ns = now_ns();
        trade.symbol_id     = table.intern(trade.ticker);

        while (!queue.push(std::move(trade)))
        {
            std::this_thread::yield();
        }
    }

    done.store(true, std::memory_order_release);
    consumer.join();
    const double seconds = static_cast<double>(now_ns() - start_time) / 1e9;
    const PercentileSummary    percent    = percentiles(latencies);
    int64_t      hits = 0;

    for (const auto& owned_ticker : owned)
    {
        hits += owned_ticker->hits;
    }

    if (gap > 0)
    {
        std::printf("  투입률 맞춤 %.0f 틱/s: 틱 %zu건, 전략 적중 %lld\n", static_cast<double>(kTicks) / seconds, kTicks,
                    static_cast<long long>(hits));
    }
    else
    {
        std::printf("  최대속도: 틱 %zu건 %.2f초 → %.0f 틱/s (틱당 %.0f ns), 전략 적중 %lld\n", kTicks, seconds,
                    static_cast<double>(kTicks) / seconds, seconds * 1e9 / static_cast<double>(kTicks),
                    static_cast<long long>(hits));
    }

    std::printf("  수신 시각 → on_trade 반환: p50 %.0f ns  p99 %.0f ns  p999 %.0f ns  max %.0f ns\n", percent.p50, percent.p99,
                percent.p999, percent.max);
}

} // namespace latency_path_bench
