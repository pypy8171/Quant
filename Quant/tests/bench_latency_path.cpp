// tests/bench_latency_path.cpp
// 09-13 hot path 변경(D-071 Phase 2·3)의 항목별 전후 비교 — 옛 방식을 벤치 안에 최소 복제해 같은 입력으로 잰다.
//  결과는 `docs/reports/PIPELINE_LATENCY_REPORT.md` 결과 ⑥, 절차는 `docs/guides/LOAD_TEST_GUIDE.md` §6.
//  스레드: 항목 1~4는 단일 스레드, 5(캡처)·6(multiplexer)·7(연쇄)은 생산자 1·소비자 1. release 빌드로만 잰다. [why D-071]
//
// [inv] 측정 범위 — 실 KIS WS 프레임 수신·복호화·네트워크는 빠져 있다. 재는 것은 디코더 입구(`^` 페이로드)부터
//   전략 on_trade 반환까지의 in-process 비용이다. 옛 방식 복제는 지운 코드의 핵심 연산(문자열 키 맵·mutex·
//   substr+stoi·전 전략 방문)만 옮긴 것이라 절대값은 참고, 비교는 같은 입력에서의 상대값으로 읽는다.

#include "api/KisWsDecode.h"
#include "core/FeedMux.h"
#include "core/MarketSession.h"
#include "core/RingBuffer.h"
#include "core/StrategyRouter.h"
#include "core/SymbolTable.h"
#include "core/TickCapture.h"
#include "core/Types.h"
#include "strategy/StrategyBase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

volatile int64_t g_sink = 0;

int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// t(steady nanoseconds)까지 바쁘게 기다린다 — 생산자 투입률 맞춤용. sleep은 2ms 격자라 못 쓴다.
void spin_until(int64_t time_value)
{
    while (now_ns() < time_value)
    {
    }
}

// f를 iters번 돌린 평균 nanoseconds/op. 첫 1/16은 예열로 버린다.
template <class F>
double ns_per_op(size_t iters, F&& item)
{
    const size_t warm = iters / 16;

    for (size_t warm_index = 0; warm_index < warm; ++warm_index)
    {
        item(warm_index);
    }

    const int64_t start_time = now_ns();

    for (size_t index = 0; index < iters; ++index)
    {
        item(index);
    }

    return static_cast<double>(now_ns() - start_time) / static_cast<double>(iters);
}

struct PercentileSummary
{
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

PercentileSummary percentiles(std::vector<double>& values)
{
    PercentileSummary percent;

    if (values.empty())
    {
        return percent;
    }

    std::sort(values.begin(), values.end());
    auto at = [&](double quantity) { return values[std::min(values.size() - 1, static_cast<size_t>(quantity * static_cast<double>(values.size())))]; };
    percent.p50  = at(0.50);
    percent.p99  = at(0.99);
    percent.p999 = at(0.999);
    percent.max  = values.back();
    return percent;
}

void row(const char* name, double old_ns, double new_ns)
{
    const double ratio = new_ns > 0 ? old_ns / new_ns : 0.0;
    std::printf("  %-44s %9.1f  %9.1f  %6.2fx\n", name, old_ns, new_ns, ratio);
}

void print_row(const char* name, double nanoseconds)
{
    std::printf("  %-44s %9.1f\n", name, nanoseconds);
}

void head(const char* title)
{
    std::printf("\n== %s\n", title);
}

// 종목 코드 2,600개 — 여섯 자리 숫자열. 실 코스피·코스닥 규모.
constexpr size_t kSymbols = 2600;

std::vector<std::string> make_tickers()
{
    std::vector<std::string> parts;
    parts.reserve(kSymbols);

    for (size_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
    {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%06zu", 5930 + symbol_index * 7);
        parts.emplace_back(buffer);
    }

    return parts;
}

// 종목 접근 순서 — 선형이면 캐시가 다 먹으므로 LCG로 섞는다.
size_t next_index(size_t& size)
{
    size = size * 6364136223846793005ULL + 1442695040888963407ULL;
    return (size >> 33) % kSymbols;
}

// [wire] H0STCNT0 페이로드 — 46필드를 '^'로 잇는다. 디코더가 읽는 자리(0·1·2·12·13·18·21)만 실값, 나머지는 0.
std::string make_kr_trade_payload(const std::string& ticker, int price)
{
    std::string text;

    for (int index = 0; index < 46; ++index)
    {
        if (index > 0)
        {
            text += '^';
        }

        switch (index)
        {
            case 0: text += ticker; break;
            case 1: text += "093001"; break;
            case 2: text += std::to_string(price); break;
            case 12: text += "150"; break;
            case 13: text += "1234567"; break;
            case 18: text += "120.55"; break;
            case 21: text += "1"; break;
            default: text += '0'; break;
        }
    }

    return text;
}

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

// ─── 3. 라우터 — 전 전략 방문 vs 종목 id 디스패치 (780597a) ───────────────────────
// 구독 종목 하나를 밝히는 가짜 전략. on_trade는 오늘 전략들처럼 symbol_id이 다르면 바로 돌아간다.
class OneSymStrategy final : public StrategyBase
{
public:
    explicit OneSymStrategy(std::string ticker) : ticker_(std::move(ticker)), id_("bench_" + ticker_) {}

    void bind(symbol::SymbolId id)
    {
        symbol_id_ = id;
    }

    const std::string& id() const override
    {
        return id_;
    }

    std::string describe() const override
    {
        return "bench";
    }

    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        if (trade.symbol_id != symbol_id_)
        {
            return std::nullopt;
        }

        ++hits;
        return std::nullopt;
    }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        WatchSpec specification;
        specification.ticker = ticker_;
        return {specification};
    }

    int64_t hits = 0;

private:
    std::string   ticker_;
    std::string   id_;
    symbol::SymbolId symbol_id_ = symbol::kNone;
};

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
    router.rebuild(pointers, [&table](const std::string& ticker) { return table.intern(ticker); });

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
        router.for_each(trade.symbol_id, [&trade](StrategyBase* strategy) { (void)strategy->on_trade(trade); });
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

// ─── 7. 연쇄 — 디코드 → intern → SPSC → 전략 스레드(캐시·라우터·on_trade) ──────────
// 09-13 조각을 실제 순서로 잇는다. 생산자는 WS 수신 콜백 자리, 소비자는 전략 스레드 자리.
void bench_chain_at(const std::vector<std::string>& tickers, double rate_per_s)
{
    const int64_t gap = rate_per_s > 0 ? static_cast<int64_t>(1e9 / rate_per_s) : 0;
    const size_t kTicks  = gap > 0 ? 300'000 : 1'000'000;
    constexpr size_t kStrats = 40;

    std::vector<std::string> payloads;
    payloads.reserve(kSymbols);

    for (size_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
    {
        payloads.push_back(make_kr_trade_payload(tickers[symbol_index], 70000 + static_cast<int>(symbol_index)));
    }

    symbol::SymbolTable table(kSymbols + 1);
    std::vector<std::unique_ptr<OneSymStrategy>> owned;
    std::vector<StrategyBase*>                   pointers;

    for (size_t index = 0; index < kStrats; ++index)
    {
        owned.push_back(std::make_unique<OneSymStrategy>(tickers[index]));
        owned.back()->bind(table.intern(tickers[index]));
        pointers.push_back(owned.back().get());
    }

    strategy::Router router;
    router.rebuild(pointers, [&table](const std::string& ticker) { return table.intern(ticker); });
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
            router.for_each(id, [&](StrategyBase* strategy) { (void)strategy->on_trade(*option); });
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

} // namespace

int main()
{
#ifdef _WIN32
    std::system("chcp 65001 > nul");
#endif
    const auto tickers = make_tickers();
    std::printf("bench_latency_path — 09-13 hot path 변경 항목별 전후 (release, 종목 %zu개)\n", kSymbols);
    bench_decode();
    bench_last_price(tickers);
    bench_router(tickers);
    bench_state_key(tickers);
    bench_capture(tickers);
    bench_feed_mux(tickers);
    head("7. 연쇄 — split+decode → intern → td_queue push → pop → set_last_px → Router → on_trade(전략 40개)");
    std::printf("  소비자는 바쁘게 기다린다(엔진 전략 스레드의 200us yield 뒤 잠들기는 빠져 있다). 코어 %u개.\n",
                std::thread::hardware_concurrency());
    bench_chain_at(tickers, 100'000.0);
    bench_chain_at(tickers, 0.0);
    std::printf("\nsink=%lld\n", static_cast<long long>(g_sink));
    return 0;
}
