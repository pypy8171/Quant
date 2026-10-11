// bench_latency_path — 1·2 디코더 시각 정수화와 현재가 캐시 전후.
//  공용 준비는 bench_latency_path_support.h, 실행 순서는 bench_latency_path.cpp의 main.
#include "bench_latency_path_support.h"

namespace latency_path_bench
{

// ─── 1. 디코더 — 시각 문자열 → 정수 hhmmss (9d838e0) ─────────────────────────────
// 옛: trade.time = f[1]로 문자열을 싣고 소비자마다 다시 파싱(IntradayBreakout은 stoi(substr), 나머지는 parse_hhmm).
// 새: 디코더가 parse_hhmmss 한 번, 소비자는 정수 나눗셈.
void bench_decode()
{
    head("1. 디코더 시각 — 문자열 실어 소비자 N회 파싱 vs 정수 한 번");
    const std::string payload = make_kr_trade_payload("005930", 71200);
    std::vector<std::string_view> fields;
    fields.reserve(64);
    constexpr size_t kIters     = 2'000'000;
    constexpr int    kConsumers = 4; // 봉 집계기 세션 판정 + 전략 셋이 같은 틱을 본다

    const double split_ns = ns_per_op(kIters, [&](size_t)
    {
        kis_websocket::split_fields(payload, '^', fields);
        g_sink += static_cast<int64_t>(fields.size());
    });

    kis_websocket::split_fields(payload, '^', fields);
    TradeData trade;

    const double decode_ns = ns_per_op(kIters, [&](size_t)
    {
        (void)kis_websocket::decode_kr_trade(fields, trade);
        g_sink += trade.hhmmss;
    });

    const double old_ns = ns_per_op(kIters, [&](size_t)
    {
        std::string time(fields[1]); // 옛 trade.time
        int value = std::stoi(time.substr(0, 6)); // 옛 IntradayBreakoutStrategy::parse_hhmmss

        for (int consumer_index = 1; consumer_index < kConsumers; ++consumer_index)
        {
            value += krx::parse_hhmm(time);
        }

        g_sink += value;
    });

    const double new_ns = ns_per_op(kIters, [&](size_t)
    {
        const int32_t height = krx::parse_hhmmss(fields[1]);
        int           value = height;

        for (int consumer_index = 1; consumer_index < kConsumers; ++consumer_index)
        {
            value += height / 100;
        }

        g_sink += value;
    });

    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    print_row("(참고) split_fields 46필드", split_ns);
    print_row("(참고) decode_kr_trade 전체(새 방식 포함)", decode_ns);
    row("시각: 문자열+소비자 4회 파싱 vs 정수 1회", old_ns, new_ns);
}

// ─── 2. 현재가 캐시 — mutex+map<string> vs id 배열 (f9026d5) ──────────────────────
struct OldLastPx
{
    double                   price = 0;
    Clock::time_point        at;
};

void bench_last_price(const std::vector<std::string>& tickers)
{
    head("2. 현재가 캐시 — mutex + unordered_map<string> vs atomic 배열[id]");
    constexpr size_t kIters = 2'000'000;

    std::unordered_map<std::string, OldLastPx> old_map;
    std::mutex                                 old_mutex;

    for (const auto& ticker : tickers)
    {
        old_map[ticker] = {};
    }

    symbol::SymbolTable table(kSymbols + 1);
    std::vector<symbol::SymbolId> ids;

    for (const auto& ticker : tickers)
    {
        ids.push_back(table.intern(ticker));
    }

    auto array_price = std::make_unique<std::atomic<double>[]>(kSymbols + 1);
    auto array_at = std::make_unique<std::atomic<int64_t>[]>(kSymbols + 1);

    size_t size = 1;
    const double old_set = ns_per_op(kIters, [&](size_t)
    {
        const auto& ticker = tickers[next_index(size)];
        std::lock_guard<std::mutex> lock(old_mutex);
        old_map[ticker] = OldLastPx{71200.0, Clock::now()};
    });

    size = 1;
    const double new_set = ns_per_op(kIters, [&](size_t)
    {
        const symbol::SymbolId id = ids[next_index(size)];
        array_price[id].store(71200.0, std::memory_order_relaxed);
        array_at[id].store(now_ns(), std::memory_order_relaxed);
    });

    size = 1;
    const double old_get = ns_per_op(kIters, [&](size_t)
    {
        const auto& ticker = tickers[next_index(size)];
        std::lock_guard<std::mutex> lock(old_mutex);
        auto iterator = old_map.find(ticker);
        g_sink += static_cast<int64_t>(iterator == old_map.end() ? 0.0 : iterator->second.price);
    });

    size = 1;
    const double new_get = ns_per_op(kIters, [&](size_t)
    {
        const symbol::SymbolId id = ids[next_index(size)];
        g_sink += static_cast<int64_t>(array_price[id].load(std::memory_order_relaxed));
    });

    // 수신 콜백이 아직 틱마다 부르는 문자열 → id 조회. 디코더가 id를 직접 찍는 후속이 없앨 비용.
    size = 1;
    const double intern_ns = ns_per_op(kIters, [&](size_t)
    {
        g_sink += table.intern(tickers[next_index(size)]);
    });

    // 수신 스레드가 여럿일 때(D-071 목표) 같은 표를 동시에 읽는 비용 — 스레드 4개가 각자 kIters번 조회,
    // 벽시계 / kIters = 스레드 하나가 본 ns/op. 읽기 락(shared_mutex)은 여기서 카운터 경합이 드러난다.
    constexpr int kReaderThreads = 4;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);

    for (int thread_index = 0; thread_index < kReaderThreads; ++thread_index)
    {
        readers.emplace_back([&, thread_index]
        {
            size_t  local_size = static_cast<size_t>(thread_index) + 1;
            int64_t local_sink = 0; // 스레드마다 따로 — g_sink를 같이 쓰면 그 줄의 캐시라인 경합을 재게 된다
            ready.fetch_add(1);

            while (!go.load(std::memory_order_acquire))
            {
            }

            for (size_t index = 0; index < kIters; ++index)
            {
                local_sink += table.intern(tickers[next_index(local_size)]);
            }

            g_sink += local_sink;
        });
    }

    while (ready.load() < kReaderThreads)
    {
    }

    const int64_t multi_start = now_ns();
    go.store(true, std::memory_order_release);

    for (auto& reader : readers)
    {
        reader.join();
    }

    const double intern_multi_ns = static_cast<double>(now_ns() - multi_start) / static_cast<double>(kIters);

    std::printf("  %-44s %9s  %9s  %7s\n", "항목", "옛 ns", "새 ns", "배");
    row("set_last_px(틱마다)", old_set, new_set);
    row("last_px(운영단말·발주 기준가)", old_get, new_get);
    print_row("(잔여) SymbolTable::intern(문자열) 틱마다", intern_ns);
    print_row("SymbolTable::intern 수신 스레드 4개 동시(스레드당)", intern_multi_ns);
}

} // namespace latency_path_bench
