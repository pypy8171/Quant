// tests/bench_market_firehose.cpp
// 전종목 규모(~2,600) 시세 파이프라인 부하테스트 — 내부 3단 처리단의 전 구간(E2E) 지연 분포
//  (중앙값(p50)·상위 1%(p99)·상위 0.1%(p999))와 최대 지속가능 처리량(throughput)을 실측한다.
//  실행 절차의 정본은 `docs/guides/LOAD_TEST_GUIDE.market_data` §1, 결과는
//  `docs/reports/PIPELINE_LATENCY_REPORT.market_data`.
//
// [inv] 측정 범위 — 내부 처리단만 잰다. 실제 KIS REST/WS 네트워크 지연은 빠져 있고, 프로덕션
//   end-to-end 지연은 무료 API 폴링 주기(초 단위)가 좌우한다. 이 하네스로 "지연을 개선했다"를
//   주장할 수 없다. 보이는 것은 "처리단은 전종목 규모에서도 µs로 여유가 있다"까지다.
//
// 토폴로지 (Engine.cpp와 동일한 3-stage 락프리 파이프):
//   websocket_producer → [order_book_queue, trade_queue] → strategy_thread → order_queue → order_thread
//   종목별 메시지 rate는 Zipf(멱법칙) 배분 — 소수 대형주가 총 호가 팬아웃의 대부분을 차지하는
//   실제 시장 구조 근사다(균등 분포는 비현실적). 티커는 universe_full.json의 실제 상장 코드를
//   쓰고, 없으면 합성 6자리로 폴백한다.
//
// [inv] 측정 관례(bench_intake·test_pipeline_stress와 공통) — pacing은 sleep 금지(Windows
//   부정확)라 busy-wait. 지연 샘플은 소비자 단독 스레드에서만 수집한다(스레드별 독립 vector).
//   reserve로 미리 잡아 측정 중 재할당(소비자 스톨)을 막는다. 종료 후 드레인 구간 항목은
//   분위수(percentile) 오염원이라 카운트만 하고 표본에서 뺀다. release 빌드로만 유의미하며
//   (debug/ASan은 무시) 빌드타입을 배너에 병기한다.
//
// 사용법:
//   bench_market_firehose load  [--universe path] [--tickers N] [--rate MSGS_PER_SEC]
//                               [--duration SEC] [--order_book-ratio R] [--zipf S]
//   bench_market_firehose sweep [--universe path] [--tickers N]
//                               [--start R] [--step R] [--max R] [--dwell SEC] [--zipf S]
//   기본값: load  --tickers 2600 --rate 200000 --duration 20 --order_book-ratio 0.7 --zipf 1.0
//           sweep --tickers 2600 --start 50000 --step 100000 --max 2000000 --dwell 4

#include "core/RingBuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using steady_clock = std::chrono::steady_clock;

static inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static inline void busy_wait_until_ns(int64_t deadline_ns)
{
    while (now_ns() < deadline_ns)
    {
        /* pure spin — Windows scheduler 회피 */
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock 메시지 (실제 Types.h OrderBook/TradeData/OrderSignal와 동일 페이로드 크기)
//   send_ts_ns: 생산자 인테이크 시각 (E2E 기준점)
// ─────────────────────────────────────────────────────────────────────────────
struct MockOrderBook
{
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t sequence;
    double   ask_price[5];
    int64_t  ask_quantity[5];
    double   bid_price[5];
    int64_t  bid_quantity[5];
};

struct MockTradeData
{
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t sequence;
    double   price;
    int64_t  quantity;
    int      direction;
};

struct MockOrderSignal
{
    char     ticker[8];
    int64_t  send_ts_ns;      // 원본 시세 인테이크 시각 (E2E)
    int64_t  strategy_ts_ns;     // strategy가 신호를 push한 시각 (strategy→order 분해용)
    uint64_t origin_sequence;
    int      side;
    int      quantity;
};

// ─────────────────────────────────────────────────────────────────────────────
// 티커 로드: universe_full.json(코드 배열) → 없으면 합성 6자리
//   JSON 파서 미도입 — "codes":[ ... ] 안의 6자리 숫자열만 정규식 없이 스캔 추출.
//   (universe_full.json은 full_universe_dump.py가 생성; 스키마는 codes 배열 포함)
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> load_universe(const std::string& path, int fallback_n)
{
    std::vector<std::string> out;

    if (!path.empty())
    {
        std::ifstream file(path);

        if (file)
        {
            std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            // "codes" 배열 이후만 스캔(ticker 이름 등 다른 숫자 오염 방지). 없으면 전체 스캔.
            size_t start = body.find("\"codes\"");
            std::string scan = (start != std::string::npos) ? body.substr(start) : body;

            // 따옴표로 감싼 연속 6자리 숫자 토큰만 코드로 취급.
            for (size_t index = 0; index + 1 < scan.size(); ++index)
            {
                if (scan[index] != '"')
                {
                    continue;
                }

                size_t inner_index = index + 1, digits = 0;

                while (inner_index < scan.size() && scan[inner_index] >= '0' && scan[inner_index] <= '9')
                {
                    ++inner_index;
                    ++digits;
                }

                if (digits == 6 && inner_index < scan.size() && scan[inner_index] == '"')
                {
                    out.emplace_back(scan.substr(index + 1, 6));
                    index = inner_index;
                }
            }
        }
    }

    if (out.empty())
    {
        out.reserve(fallback_n);

        for (int fallback_index = 1; fallback_index <= fallback_n; ++fallback_index)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "%06d", fallback_index);
            out.emplace_back(buffer);
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zipf 가중 종목 선택기 — rank r의 확률 ∝ 1/r^s.
//   누적분포를 미리 만들어 uniform → binary search로 O(log N) 샘플.
//   s=0 → 균등, s=1 → 고전적 Zipf(상위 종목이 메시지 대부분 차지).
// ─────────────────────────────────────────────────────────────────────────────
struct ZipfPicker
{
    std::vector<double> cumulative_distribution;
    explicit ZipfPicker(size_t count, double sum)
    {
        cumulative_distribution.resize(count);
        double accumulator = 0.0;

        for (size_t index = 0; index < count; ++index)
        {
            accumulator += 1.0 / std::pow(static_cast<double>(index + 1), sum);
            cumulative_distribution[index] = accumulator;
        }

        const double total = accumulator;

        for (auto& cumulative : cumulative_distribution)
        {
            cumulative /= total;
        }
    }

    // u ∈ [0,1) → 종목 인덱스
    size_t pick(double upper) const
    {
        auto iterator = std::lower_bound(cumulative_distribution.begin(), cumulative_distribution.end(), upper);
        size_t index = static_cast<size_t>(iterator - cumulative_distribution.begin());
        return (index < cumulative_distribution.size()) ? index : cumulative_distribution.size() - 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 공유 통계
// ─────────────────────────────────────────────────────────────────────────────
struct Stats
{
    std::atomic<uint64_t> order_book_produced{0};
    std::atomic<uint64_t> trade_produced{0};
    std::atomic<uint64_t> order_book_consumed{0};
    std::atomic<uint64_t> trade_consumed{0};
    std::atomic<uint64_t> signals{0};
    std::atomic<uint64_t> orders{0};
    std::atomic<uint64_t> order_book_drops{0};
    std::atomic<uint64_t> trade_drops{0};
    std::atomic<uint64_t> order_drops{0};
    std::atomic<uint64_t> order_book_high_water_mark{0};   // 큐 high-water mark (근사)
    std::atomic<uint64_t> trade_high_water_mark{0};
    std::atomic<uint64_t> order_high_water_mark{0};

    // 스레드별 독립 수집 (합산은 join 후)
    std::vector<int64_t> intake_to_strategy_ns; // strategy 스레드가 채움
    std::vector<int64_t> strategy_to_order_ns;  // order 스레드가 채움
    std::vector<int64_t> e2e_ns;             // order 스레드가 채움
};

static inline void bump_high_water_mark(std::atomic<uint64_t>& high_water_mark, uint64_t value)
{
    uint64_t current = high_water_mark.load(std::memory_order_relaxed);

    while (value > current && !high_water_mark.compare_exchange_weak(current, value, std::memory_order_relaxed))
    {
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WS 시뮬레이션 Producer — 총 offered rate를 균등 간격 busy-wait pacing으로 방출,
//   종목은 Zipf로 선택, OB/TD는 ob_ratio로 분기.
// ─────────────────────────────────────────────────────────────────────────────
static void producer_fn(RingBuffer<MockOrderBook>& order_book_queue,
                        RingBuffer<MockTradeData>& trade_queue,
                        const std::vector<std::string>& tickers,
                        const ZipfPicker& zipf,
                        Stats& stop_token,
                        std::atomic<bool>& stop,
                        int64_t total_rate,
                        double order_book_ratio,
                        int64_t duration_ns)
{
    std::mt19937_64 random_engine(0xC0FFEE);
    std::uniform_real_distribution<double> uniform01(0.0, 1.0);

    const int64_t interval_ns = (total_rate > 0) ? static_cast<int64_t>(1e9 / total_rate) : 0;
    const int64_t start_time = now_ns();
    const int64_t t_end = start_time + duration_ns;
    int64_t next_emit = start_time;
    uint64_t sequence = 0;

    while (!stop.load(std::memory_order_relaxed))
    {
        const int64_t time_value = now_ns();

        if (time_value >= t_end)
        {
            break;
        }

        if (interval_ns > 0)
        {
            if (time_value < next_emit)
            {
                busy_wait_until_ns(next_emit);
            }

            next_emit += interval_ns;
        }

        const size_t ticker_index = zipf.pick(uniform01(random_engine));
        const int64_t timestamp = now_ns();

        if (uniform01(random_engine) < order_book_ratio)
        {
            MockOrderBook order_book{};
            std::memcpy(order_book.ticker, tickers[ticker_index].c_str(), 7);
            order_book.send_ts_ns = timestamp;
            order_book.sequence = sequence++;

            for (int index = 0; index < 5; ++index)
            {
                order_book.ask_price[index] = 70000.0 + index * 10;
                order_book.ask_quantity[index] = 100 * (index + 1);
                order_book.bid_price[index] = 69990.0 - index * 10;
                order_book.bid_quantity[index] = 100 * (index + 1);
            }

            if (order_book_queue.push(order_book))
            {
                stop_token.order_book_produced.fetch_add(1, std::memory_order_relaxed);
                bump_high_water_mark(stop_token.order_book_high_water_mark, order_book_queue.size());
            }
            else
            {
                stop_token.order_book_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            MockTradeData trade{};
            std::memcpy(trade.ticker, tickers[ticker_index].c_str(), 7);
            trade.send_ts_ns = timestamp;
            trade.sequence = sequence++;
            trade.price = 70000.0 + (sequence % 100);
            trade.quantity = 10 + (sequence % 50);
            trade.direction = (sequence % 2) ? 1 : 5;

            if (trade_queue.push(trade))
            {
                stop_token.trade_produced.fetch_add(1, std::memory_order_relaxed);
                bump_high_water_mark(stop_token.trade_high_water_mark, trade_queue.size());
            }
            else
            {
                stop_token.trade_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy Thread — order_book_queue/trade_queue 소비, intake→strategy 지연 수집(단독 스레드),
//   OB 100건당 1 / TD 200건당 1 신호 생성.
// ─────────────────────────────────────────────────────────────────────────────
static void strategy_fn(RingBuffer<MockOrderBook>& order_book_queue,
                        RingBuffer<MockTradeData>& trade_queue,
                        RingBuffer<MockOrderSignal>& order_queue,
                        Stats& stop_token,
                        std::atomic<bool>& stop,
                        size_t latency_cap)
{
    stop_token.intake_to_strategy_ns.reserve(latency_cap);
    uint64_t order_book_count = 0, trade_count = 0;

    while (!stop.load(std::memory_order_relaxed) || !order_book_queue.empty() || !trade_queue.empty())
    {
        while (auto option = order_book_queue.pop())
        {
            const int64_t time_value = now_ns();

            if (stop_token.intake_to_strategy_ns.size() < latency_cap)
            {
                stop_token.intake_to_strategy_ns.push_back(time_value - option->send_ts_ns);
            }

            stop_token.order_book_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = 0.0;

            for (int index = 0; index < 5; ++index)
            {
                sink = sink + option->ask_price[index] - option->bid_price[index];
            }

            (void)sink;

            if (++order_book_count % 100 == 0)
            {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.strategy_ts_ns = now_ns();
                signal.origin_sequence = option->sequence;
                signal.side = 0;
                signal.quantity = 10;

                if (order_queue.push(signal))
                {
                    stop_token.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_high_water_mark(stop_token.order_high_water_mark, order_queue.size());
                }
                else
                {
                    stop_token.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        while (auto option = trade_queue.pop())
        {
            const int64_t time_value = now_ns();

            if (stop_token.intake_to_strategy_ns.size() < latency_cap)
            {
                stop_token.intake_to_strategy_ns.push_back(time_value - option->send_ts_ns);
            }

            stop_token.trade_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = option->price * option->quantity;
            (void)sink;

            if (++trade_count % 200 == 0)
            {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.strategy_ts_ns = now_ns();
                signal.origin_sequence = option->sequence;
                signal.side = 1;
                signal.quantity = 5;

                if (order_queue.push(signal))
                {
                    stop_token.signals.fetch_add(1, std::memory_order_relaxed);
                    bump_high_water_mark(stop_token.order_high_water_mark, order_queue.size());
                }
                else
                {
                    stop_token.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order Thread — order_queue 소비, strategy→order & E2E 지연 수집(단독 스레드).
//   외부 REST 지연 시뮬레이션 없음 — 내부 파이프라인만.
// ─────────────────────────────────────────────────────────────────────────────
static void order_fn(RingBuffer<MockOrderSignal>& order_queue,
                     Stats& stop_token,
                     std::atomic<bool>& stop,
                     size_t latency_cap,
                     std::atomic<bool>& measuring)
{
    stop_token.strategy_to_order_ns.reserve(latency_cap);
    stop_token.e2e_ns.reserve(latency_cap);

    while (!stop.load(std::memory_order_relaxed) || !order_queue.empty())
    {
        auto option = order_queue.pop();

        if (!option)
        {
            continue;
        }

        const int64_t time_value = now_ns();

        // 측정창이 닫힌 뒤(드레인) 항목은 percentile 오염원 → 카운트만.
        if (measuring.load(std::memory_order_relaxed))
        {
            if (stop_token.strategy_to_order_ns.size() < latency_cap)
            {
                stop_token.strategy_to_order_ns.push_back(time_value - option->strategy_ts_ns);
            }

            if (stop_token.e2e_ns.size() < latency_cap)
            {
                stop_token.e2e_ns.push_back(time_value - option->send_ts_ns);
            }
        }

        stop_token.orders.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Percentile 유틸
// ─────────────────────────────────────────────────────────────────────────────
struct PercentileSummary
{
    int64_t p50 = 0, p99 = 0, p999 = 0, max_value = 0;
    size_t  count = 0;
};

static PercentileSummary percentiles(std::vector<int64_t>& values)
{
    PercentileSummary percentiles;
    percentiles.count = values.size();

    if (values.empty())
    {
        return percentiles;
    }

    std::sort(values.begin(), values.end());
    auto at = [&](double price) {
        size_t index = static_cast<size_t>(price * (values.size() - 1));
        return values[index];
    };
    percentiles.p50 = at(0.50);
    percentiles.p99 = at(0.99);
    percentiles.p999 = at(0.999);
    percentiles.max_value = values.back();
    return percentiles;
}

static std::string format_ns(int64_t count)
{
    char byte_value[32];

    if (count < 1000)
    {
        std::snprintf(byte_value, sizeof(byte_value), "%lld ns", static_cast<long long>(count));
    }
    else if (count < 1'000'000)
    {
        std::snprintf(byte_value, sizeof(byte_value), "%.2f us", count / 1000.0);
    }
    else
    {
        std::snprintf(byte_value, sizeof(byte_value), "%.2f ms", count / 1'000'000.0);
    }

    return std::string(byte_value);
}

static const char* build_type()
{
#ifdef NDEBUG
    return "Release (NDEBUG)";
#else
    return "Debug (⚠ 측정 무의미 — Release로 재빌드)";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 한 회 실행 (한 rate로 duration 동안). 결과 Pctl들과 드롭 여부를 채운다.
// ─────────────────────────────────────────────────────────────────────────────
struct RunResult
{
    PercentileSummary e2e, i2s, s2o;
    uint64_t order_book_produced = 0, trade_produced = 0, order_book_consumed = 0, trade_consumed = 0;
    uint64_t signals = 0, orders = 0;
    uint64_t drops = 0;
    uint64_t order_book_high_water_mark = 0, trade_high_water_mark = 0, order_high_water_mark = 0;
    bool lossless = false;
    double elapsed_sec = 0;
};

static RunResult run_once(const std::vector<std::string>& tickers,
                          const ZipfPicker& zipf,
                          int64_t total_rate,
                          double order_book_ratio,
                          int duration_sec,
                          size_t order_book_capacity,
                          size_t trade_capacity,
                          size_t order_capacity,
                          size_t latency_cap)
{
    RingBuffer<MockOrderBook>   order_book_queue(order_book_capacity);
    RingBuffer<MockTradeData>   trade_queue(trade_capacity);
    RingBuffer<MockOrderSignal> order_queue(order_capacity);

    Stats stop_token;
    std::atomic<bool> stop{false};
    std::atomic<bool> measuring{true};

    const int64_t start_time = now_ns();
    std::thread order_thread(order_fn, std::ref(order_queue), std::ref(stop_token), std::ref(stop), latency_cap, std::ref(measuring));
    std::thread strategy_thread(strategy_fn, std::ref(order_book_queue), std::ref(trade_queue), std::ref(order_queue), std::ref(stop_token),
                      std::ref(stop), latency_cap);
    std::thread websocket_thread(producer_fn, std::ref(order_book_queue), std::ref(trade_queue), std::cref(tickers), std::cref(zipf),
                     std::ref(stop_token), std::ref(stop), total_rate, order_book_ratio,
                     static_cast<int64_t>(duration_sec) * 1'000'000'000LL);

    websocket_thread.join();                                    // 방출 종료
    measuring.store(false, std::memory_order_relaxed); // 이후 소비분은 드레인 → 샘플 제외
    stop.store(true, std::memory_order_relaxed);
    strategy_thread.join();
    order_thread.join();

    RunResult result;
    result.elapsed_sec = (now_ns() - start_time) / 1e9;
    result.e2e = percentiles(stop_token.e2e_ns);
    result.i2s = percentiles(stop_token.intake_to_strategy_ns);
    result.s2o = percentiles(stop_token.strategy_to_order_ns);
    result.order_book_produced = stop_token.order_book_produced.load();
    result.trade_produced = stop_token.trade_produced.load();
    result.order_book_consumed = stop_token.order_book_consumed.load();
    result.trade_consumed = stop_token.trade_consumed.load();
    result.signals = stop_token.signals.load();
    result.orders = stop_token.orders.load();
    result.drops = stop_token.order_book_drops.load() + stop_token.trade_drops.load() + stop_token.order_drops.load();
    result.order_book_high_water_mark = stop_token.order_book_high_water_mark.load();
    result.trade_high_water_mark = stop_token.trade_high_water_mark.load();
    result.order_high_water_mark = stop_token.order_high_water_mark.load();
    result.lossless = (result.drops == 0) && (result.order_book_produced == result.order_book_consumed) && (result.trade_produced == result.trade_consumed) &&
                 (result.signals == result.orders);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 인자 파서 (--key value)
// ─────────────────────────────────────────────────────────────────────────────
static std::string argument_string(int argc, char** argv, const char* key, const std::string& default_value)
{
    for (int index = 2; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], key) == 0)
        {
            return argv[index + 1];
        }
    }

    return default_value;
}

static int64_t argument_int64(int argc, char** argv, const char* key, int64_t default_value)
{
    std::string text = argument_string(argc, argv, key, "");
    return text.empty() ? default_value : std::atoll(text.c_str());
}

static double argument_double(int argc, char** argv, const char* key, double default_value)
{
    std::string text = argument_string(argc, argv, key, "");
    return text.empty() ? default_value : std::atof(text.c_str());
}

static void print_banner(const char* mode, const std::vector<std::string>& tickers,
                         const std::string& uni_path, double order_book_ratio, double zipf_s)
{
    std::printf("=== Market Firehose Bench — mode=%s ===\n", mode);
    std::printf("build           : %s\n", build_type());
    std::printf("tickers         : %zu  (source: %s)\n", tickers.size(),
                uni_path.empty() ? "synthetic" : uni_path.c_str());
    std::printf("ob:td ratio     : %.2f : %.2f\n", order_book_ratio, 1.0 - order_book_ratio);
    std::printf("zipf s          : %.2f  (0=uniform, 1=대형주 편중)\n", zipf_s);
    std::printf("hw concurrency  : %u\n", std::thread::hardware_concurrency());
    std::printf("NOTE: 내부 처리단만 측정. 네트워크/REST 피드 지연 제외(실제 지배적 병목).\n\n");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string mode = (argc > 1) ? argv[1] : "load";
    const std::string uni_path = argument_string(argc, argv, "--universe", "");
    const int fallback_n = static_cast<int>(argument_int64(argc, argv, "--tickers", 2600));
    const double zipf_s = argument_double(argc, argv, "--zipf", 1.0);
    const double order_book_ratio = argument_double(argc, argv, "--ob-ratio", 0.7);

    std::vector<std::string> tickers = load_universe(uni_path, fallback_n);
    ZipfPicker zipf(tickers.size(), zipf_s);

    // 큐 용량: 스로틀 없는 순간 버스트를 흡수하되, 백로그가 tail을 지배하지 않게.
    const size_t OB_CAP = 1u << 16;    // 65,536
    const size_t TD_CAP = 1u << 16;
    const size_t ORDER_CAP = 1u << 12; // 4,096
    const size_t LAT_CAP = 1u << 23;   // 8.4M 샘플 상한

    if (mode == "sweep")
    {
        const int64_t start = argument_int64(argc, argv, "--start", 50000);
        const int64_t step = argument_int64(argc, argv, "--step", 100000);
        const int64_t max_records = argument_int64(argc, argv, "--max", 2'000'000);
        const int dwell = static_cast<int>(argument_int64(argc, argv, "--dwell", 4));

        print_banner("sweep", tickers, uni_path, order_book_ratio, zipf_s);
        std::printf("sweep: rate %lld → %lld step %lld, dwell %ds/step\n\n",
                    static_cast<long long>(start), static_cast<long long>(max_records), static_cast<long long>(step), dwell);
        std::printf("%-12s %-10s %-10s %-10s %-10s %-8s %-9s\n",
                    "offered/s", "e2e_p50", "e2e_p99", "e2e_p999", "e2e_max", "drops", "lossless");
        std::printf("%s\n", std::string(72, '-').c_str());

        int64_t ceiling = 0;

        for (int64_t rate = start; rate <= max_records; rate += step)
        {
            RunResult result = run_once(tickers, zipf, rate, order_book_ratio, dwell, OB_CAP, TD_CAP, ORDER_CAP, LAT_CAP);
            std::printf("%-12lld %-10s %-10s %-10s %-10s %-8llu %-9s\n",
                        static_cast<long long>(rate), format_ns(result.e2e.p50).c_str(), format_ns(result.e2e.p99).c_str(),
                        format_ns(result.e2e.p999).c_str(), format_ns(result.e2e.max_value).c_str(),
                        static_cast<unsigned long long>(result.drops), result.lossless ? "yes" : "NO");
            // CSV: rate,p50ns,p99ns,p999ns,maxns,drops,lossless,order_book_produced,trade_produced
            std::printf("CSV,%lld,%lld,%lld,%lld,%lld,%llu,%d,%llu,%llu\n",
                        static_cast<long long>(rate), static_cast<long long>(result.e2e.p50), static_cast<long long>(result.e2e.p99),
                        static_cast<long long>(result.e2e.p999), static_cast<long long>(result.e2e.max_value), static_cast<unsigned long long>(result.drops),
                        result.lossless ? 1 : 0, static_cast<unsigned long long>(result.order_book_produced),
                        static_cast<unsigned long long>(result.trade_produced));

            if (result.lossless)
            {
                ceiling = rate;
            }
            else
            {
                break; // 첫 드롭 발생 → 용량 천장 확정
            }
        }

        std::printf("\n용량 천장 (최대 무손실 offered rate): %lld msg/sec\n", static_cast<long long>(ceiling));
        return 0;
    }

    // ── load 모드 ──
    const int64_t rate = argument_int64(argc, argv, "--rate", 200000);
    const int duration = static_cast<int>(argument_int64(argc, argv, "--duration", 20));

    print_banner("load", tickers, uni_path, order_book_ratio, zipf_s);
    std::printf("offered rate    : %lld msg/sec (총)\n", static_cast<long long>(rate));
    std::printf("duration        : %d sec\n\n", duration);

    RunResult result = run_once(tickers, zipf, rate, order_book_ratio, duration, OB_CAP, TD_CAP, ORDER_CAP, LAT_CAP);

    std::printf("=== Throughput ===\n");
    std::printf("elapsed         : %.2f sec\n", result.elapsed_sec);
    std::printf("OB  produced/consumed/drop : %llu / %llu / (hwm %llu)\n",
                static_cast<unsigned long long>(result.order_book_produced), static_cast<unsigned long long>(result.order_book_consumed),
                static_cast<unsigned long long>(result.order_book_high_water_mark));
    std::printf("TD  produced/consumed/drop : %llu / %llu / (hwm %llu)\n",
                static_cast<unsigned long long>(result.trade_produced), static_cast<unsigned long long>(result.trade_consumed),
                static_cast<unsigned long long>(result.trade_high_water_mark));
    std::printf("signals/orders  : %llu / %llu (order_q hwm %llu)\n",
                static_cast<unsigned long long>(result.signals), static_cast<unsigned long long>(result.orders),
                static_cast<unsigned long long>(result.order_high_water_mark));
    std::printf("total in rate   : %.0f msg/sec (실측)\n",
                (result.order_book_produced + result.trade_produced) / (result.elapsed_sec > 0 ? result.elapsed_sec : 1));
    std::printf("total drops     : %llu\n\n", static_cast<unsigned long long>(result.drops));

    std::printf("=== Latency (내부 처리단, 네트워크 제외) ===\n");
    auto line = [](const char* label, const PercentileSummary& percentiles) {
        std::printf("%-26s n=%-9zu p50=%-10s p99=%-10s p999=%-10s max=%s\n", label, percentiles.count,
                    format_ns(percentiles.p50).c_str(), format_ns(percentiles.p99).c_str(), format_ns(percentiles.p999).c_str(),
                    format_ns(percentiles.max_value).c_str());
    };
    line("intake -> strategy", result.i2s);
    line("strategy -> order", result.s2o);
    line("E2E (intake -> order)", result.e2e);

    std::printf("\n[%s] no drops, no backlog (전종목 규모 무손실 판정)\n",
                result.lossless ? "PASS" : "FAIL");
    return result.lossless ? 0 : 1;
}
