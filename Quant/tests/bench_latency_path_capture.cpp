// bench_latency_path — 5·6 틱 캡처 push 비용과 FeedMux 홉 지연.
//  공용 준비는 bench_latency_path_support.h, 실행 순서는 bench_latency_path.cpp의 main.
#include "bench_latency_path_support.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace latency_path_bench
{

// ─── 5. 틱 캡처 — 수신 콜백에 더해진 push 비용 (13559a2) ──────────────────────────
// rate_per_s가 0이면 쉬지 않고 밀어 넣는다(링·기록 스레드 상한), 아니면 그 투입률에 맞춘다(실 부하 자리).
void bench_capture_at(const std::vector<TradeData>& ticks, double rate_per_s)
{
    constexpr size_t kTicks = 400'000;
    const auto file = std::filesystem::temp_directory_path() / "bench_latency_path_capture.bin";
    std::error_code error_code;
    std::filesystem::remove(file, error_code);
    const int64_t gap = rate_per_s > 0 ? static_cast<int64_t>(1e9 / rate_per_s) : 0;

    double   push_ns = 0;
    uint64_t dropped = 0;
    int64_t  total   = 0;
    {
        feed::TickCapture capture(file);

        if (!capture.ok())
        {
            std::printf("  캡처 파일을 못 열었다: %s\n", file.string().c_str());
            return;
        }

        size_t        size    = 11;
        int64_t       next = now_ns();
        const int64_t start_time   = next;
        int64_t       busy = 0;

        for (size_t tick_index = 0; tick_index < kTicks; ++tick_index)
        {
            if (gap > 0)
            {
                spin_until(next);
                next += gap;
            }

            const int64_t first_value = now_ns();
            capture.on_trade(ticks[next_index(size)]);
            busy += now_ns() - first_value;
        }

        capture.flush();
        total   = now_ns() - start_time;
        push_ns = static_cast<double>(busy) / static_cast<double>(kTicks);
        dropped = capture.dropped();
    }

    const auto bytes = std::filesystem::file_size(file, error_code);
    std::filesystem::remove(file, error_code);
    const double written = static_cast<double>(kTicks - dropped);
    std::printf("  %-12s on_trade %6.1f ns/틱 | 드롭 %6llu / %zu | 기록 %.0f 건/s (%.1f MB, flush까지 %.0f ms)\n",
                gap > 0 ? "투입률 맞춤" : "최대속도", push_ns, static_cast<unsigned long long>(dropped), kTicks,
                written / (static_cast<double>(total) / 1e9), static_cast<double>(bytes) / 1e6,
                static_cast<double>(total) / 1e6);
}

void bench_capture(const std::vector<std::string>& tickers)
{
    head("5. 틱 캡처 — 수신 콜백의 capture_->on_trade(td) 비용과 기록 스레드 상한(링 65,536)");
    std::vector<TradeData> ticks(kSymbols);

    for (size_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
    {
        ticks[symbol_index].ticker   = tickers[symbol_index];
        ticks[symbol_index].symbol_id      = static_cast<symbol::SymbolId>(symbol_index + 1);
        ticks[symbol_index].hhmmss   = 93001;
        ticks[symbol_index].price    = 71200.0;
        ticks[symbol_index].quantity = 150;
        ticks[symbol_index].received_ns  = now_ns();
    }

    std::printf("  전 시장 100k/s(투입률 맞춤)와 상한(최대속도) 두 줄. 드롭은 기록 스레드가 못 따라온 양.\n");
    bench_capture_at(ticks, 100'000.0);
    bench_capture_at(ticks, 0.0);
}

// ─── 6. FeedMux 홉 — 소켓 수신 스레드 → 링 → multiplexer 스레드 → 콜백 (824408f) ──────────
// 구독·연결만 흉내 내는 소스. emit은 호출 스레드에서 콜백을 부른다(수신 스레드 자리).
struct SpinSource final : feed::IFeedSource
{
    TradeCb on_trade;
    bool    connected = false;

    void set_callbacks(OrderBookCb, TradeCb trade) override
    {
        on_trade = std::move(trade);
    }

    bool connect(const std::vector<WatchSpec>&) override
    {
        connected = true;
        return true;
    }

    void disconnect() override
    {
        connected = false;
    }

    bool subscribe_incremental(const WatchSpec&) override
    {
        return true;
    }

    bool has_specification(const WatchSpec&) const override
    {
        return true;
    }

    std::vector<WatchSpec> take_overflow_specifications() override
    {
        return {};
    }

    bool is_connected() const override
    {
        return connected;
    }

    bool is_stale(int) const override
    {
        return false;
    }
};

void bench_multiplexer(const std::vector<std::string>& tickers, size_t n_sources, double total_rate_per_s)
{
    constexpr size_t kTicks = 200'000;
    const int64_t    gap    = total_rate_per_s > 0 ? static_cast<int64_t>(1e9 * static_cast<double>(n_sources) / total_rate_per_s) : 0;
    std::vector<std::unique_ptr<feed::IFeedSource>> sources;
    std::vector<SpinSource*>                        raw;

    for (size_t source_index = 0; source_index < n_sources; ++source_index)
    {
        auto spin_source = std::make_unique<SpinSource>();
        raw.push_back(spin_source.get());
        sources.push_back(std::move(spin_source));
    }

    feed::FeedMux multiplexer(std::move(sources));
    std::vector<double> latencies;
    latencies.reserve(kTicks * n_sources);
    std::atomic<size_t> received{0};

    multiplexer.set_callbacks([](const OrderBook&) {},
                      [&](const TradeData& trade)
                      {
                          latencies.push_back(static_cast<double>(now_ns() - trade.received_ns));
                          received.fetch_add(1, std::memory_order_release);
                      });

    std::vector<WatchSpec> specifications(n_sources);
    (void)multiplexer.connect(specifications);

    std::vector<std::thread> producers;
    const int64_t start_time = now_ns();

    for (size_t source_index = 0; source_index < n_sources; ++source_index)
    {
        producers.emplace_back([&, source_index]
        {
            TradeData trade;
            trade.ticker = tickers[source_index];
            trade.symbol_id    = static_cast<symbol::SymbolId>(source_index + 1);
            trade.price  = 1000.0;

            int64_t next = now_ns();

            for (size_t tick_index = 0; tick_index < kTicks; ++tick_index)
            {
                if (gap > 0)
                {
                    spin_until(next);
                    next += gap;
                }

                trade.received_ns = now_ns();
                raw[source_index]->on_trade(trade);
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    while (received.load(std::memory_order_acquire) + multiplexer.dropped() < kTicks * n_sources)
    {
        std::this_thread::yield();
    }

    const double seconds = static_cast<double>(now_ns() - start_time) / 1e9;
    const PercentileSummary    percent    = percentiles(latencies);
    std::printf("  소스 %zu개: 홉 지연 p50 %.0f ns  p99 %.0f ns  p999 %.0f ns  max %.0f ns  |  %.0f 건/s, 드롭 %llu\n",
                n_sources, percent.p50, percent.p99, percent.p999, percent.max, static_cast<double>(latencies.size()) / seconds,
                static_cast<unsigned long long>(multiplexer.dropped()));
}

void bench_feed_mux(const std::vector<std::string>& tickers)
{
    head("6. FeedMux 홉 — 소스 수신 스레드 emit → mux 스레드 콜백까지(직결이면 0). 투입률 맞춤은 합계 100k/s");
    std::printf("  생산자는 바쁘게 기다리며 코어를 하나씩 잡는다(코어 %u개) — 소스 수가 코어에 가까우면 p99는 스케줄러 몫.\n",
                std::thread::hardware_concurrency());
    bench_multiplexer(tickers, 1, 100'000.0);
    bench_multiplexer(tickers, 2, 100'000.0);
    bench_multiplexer(tickers, 4, 100'000.0);
    bench_multiplexer(tickers, 1, 0.0);
    bench_multiplexer(tickers, 4, 0.0);
}

} // namespace latency_path_bench
