// tests/test_pipeline_stress.cpp
// End-to-End 파이프라인 부하 테스트
//
// 시뮬레이션 구조:
//   WS recv_thread → order_book_queue_/trade_queue_ → strategy_thread
//                                        → order_queue_ → order_thread
//                 → fill_queue_ → fill_thread            (체결통보, D-056 — 소비자는 condvar로 잠들고 생산자가 깨운다)
//
// Engine.cpp의 실제 3-stage 큐 파이프를 mock 데이터로 재현 테스트.
// Windows Sleep 부정확성 회피를 위해 busy-wait 기반 rate limiting 사용.
//
// 사용법: test_pipeline_stress [duration_sec] [mode]
//   mode: "normal" (기본, 3,000 message/seconds) | "burst" (14,000 message/seconds)

#include "core/RingBuffer.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <cstring>
#include <cstdint>
#include <cstdio>

using steady_clock = std::chrono::steady_clock;
using nanoseconds = std::chrono::nanoseconds;

// ─────────────────────────────────────────────────────────────────────────────
// 티커 200개 동적 생성
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int N_TICKERS = 200;

static std::vector<std::string> make_tickers() {
    std::vector<std::string> parts;
    parts.reserve(N_TICKERS);

    for (int ticker_index = 1; ticker_index <= N_TICKERS; ++ticker_index) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%06d", ticker_index);
        parts.emplace_back(buffer);
    }

    return parts;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock 구조체 정의 (실제 Types.h와 동일 크기)
// ─────────────────────────────────────────────────────────────────────────────
struct MockOrderBook {
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t sequence;
    double   ask_price[5];
    int64_t  ask_quantity[5];
    double   bid_price[5];
    int64_t  bid_quantity[5];
};

struct MockTradeData {
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t sequence;
    double   price;
    int64_t  quantity;
    int      direction;
};

struct MockFill {
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t sequence;
    int      quantity;
    double   price;
};

struct MockOrderSignal {
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t origin_sequence;   // 어느 입력 메시지에서 파생됐는가
    int      side;          // 0=BUY, 1=SELL
    int      quantity;
};

// ─────────────────────────────────────────────────────────────────────────────
// Busy-wait 기반 정확한 sleep (Windows Sleep 부정확성 회피)
// ─────────────────────────────────────────────────────────────────────────────
static inline void busy_wait_until(steady_clock::time_point deadline) {
    while (steady_clock::now() < deadline) {
        // pure busy spin — Windows scheduler 회피
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 통계
// ─────────────────────────────────────────────────────────────────────────────
struct PipelineStats {
    std::atomic<uint64_t> order_book_produced{ 0 };
    std::atomic<uint64_t> order_book_consumed{ 0 };
    std::atomic<uint64_t> trade_produced{ 0 };
    std::atomic<uint64_t> trade_consumed{ 0 };
    std::atomic<uint64_t> signals_generated{ 0 };
    std::atomic<uint64_t> orders_processed{ 0 };
    std::atomic<uint64_t> order_book_drops{ 0 };
    std::atomic<uint64_t> trade_drops{ 0 };
    std::atomic<uint64_t> order_drops{ 0 };
    std::atomic<uint64_t> fill_produced{ 0 };
    std::atomic<uint64_t> fill_consumed{ 0 };
    std::atomic<uint64_t> fill_drops{ 0 };
    // fill_thread 깨우기(Engine::fill_wake_*와 같은 방식)
    std::mutex              fill_wake_mutex;
    std::condition_variable fill_wake_condition_variable;
    std::atomic<bool>       fill_sleeping{ false };

    // 전 구간(E2E) latency: producer push → order_thread 처리 완료
    // NOTE: order_thread 단독 producer. 다른 스레드 추가 시 mutex 또는
    //       per-thread vector 후 합산 필요 (현재 std::vector는 thread-safe 아님)
    std::vector<int64_t> e2e_latencies_ns;
    // 체결 경로 latency: WS push → fill_thread 처리. fill_thread 단독 writer.
    std::vector<int64_t> fill_latencies_ns;
};

// ─────────────────────────────────────────────────────────────────────────────
// WS 시뮬레이션 Producer
//   - N_TICKERS 종목, 종목당 order_book_rate OB/seconds + trade_rate TD/seconds
// ─────────────────────────────────────────────────────────────────────────────
static void websocket_producer_fn(RingBuffer<MockOrderBook>& order_book_queue,
    RingBuffer<MockTradeData>& trade_queue,
    RingBuffer<MockFill>& fill_queue,
    PipelineStats& statistics,
    std::atomic<bool>& stop_flag,
    int duration_sec,
    int order_book_rate,
    int trade_rate)
{
    static const auto TICKERS = make_tickers();  // 한 번만 생성

    const int     total_rate = N_TICKERS * (order_book_rate + trade_rate);
    const int64_t INTERVAL_US = 1'000'000 / total_rate;

    std::mt19937 random_engine(42);
    std::uniform_int_distribution<int> ticker_dist(0, N_TICKERS - 1);
    // OB:TD 비율을 type_dist 범위로 근사 — order_book_rate/(order_book_rate+trade_rate) 확률로 OB, 나머지 TD
    const int type_range = order_book_rate + trade_rate;
    std::uniform_int_distribution<int> type_dist(0, type_range - 1);

    auto deadline = steady_clock::now() + std::chrono::seconds(duration_sec);
    auto next_send = steady_clock::now();
    uint64_t sequence = 0;
    uint64_t fill_sequence = 0;

    while (!stop_flag.load(std::memory_order_relaxed) && steady_clock::now() < deadline) {
        busy_wait_until(next_send);
        next_send += std::chrono::microseconds(INTERVAL_US);

        int     ticker_index = ticker_dist(random_engine);
        bool    is_order_book = (type_dist(random_engine) < order_book_rate);
        int64_t now_ns = std::chrono::duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();

        // 체결통보는 같은 수신 스레드가 시세 사이에 끼워 넣는다(실물과 같은 단일 생산자). 시세 500건당 1건 —
        //  실장(하루 수백 건)보다 훨씬 잦게 넣어 소비자 폴링이 밀리는지 본다.
        if (sequence % 500 == 0) {
            MockFill mock_fill{};
            std::memcpy(mock_fill.ticker, TICKERS[ticker_index].c_str(), 7);
            mock_fill.send_ts_ns = now_ns;
            mock_fill.sequence = fill_sequence++;
            mock_fill.quantity = 1 + static_cast<int>(fill_sequence % 10);
            mock_fill.price = 70000.0;

            if (fill_queue.push(mock_fill))
            {
                statistics.fill_produced.fetch_add(1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);

                if (statistics.fill_sleeping.load(std::memory_order_relaxed))
                {
                    statistics.fill_wake_condition_variable.notify_one();
                }
            }
            else
            {
                statistics.fill_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (is_order_book) {
            MockOrderBook order_book{};
            std::memcpy(order_book.ticker, TICKERS[ticker_index].c_str(), 7);
            order_book.send_ts_ns = now_ns;
            order_book.sequence = sequence++;

            for (int index = 0; index < 5; ++index) {
                order_book.ask_price[index] = 70000.0 + index * 10;
                order_book.ask_quantity[index] = 100 * (index + 1);
                order_book.bid_price[index] = 69990.0 - index * 10;
                order_book.bid_quantity[index] = 100 * (index + 1);
            }

            if (order_book_queue.push(order_book))
            {
                statistics.order_book_produced.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                statistics.order_book_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else {
            MockTradeData trade{};
            std::memcpy(trade.ticker, TICKERS[ticker_index].c_str(), 7);
            trade.send_ts_ns = now_ns;
            trade.sequence = sequence++;
            trade.price = 70000.0 + (sequence % 100);
            trade.quantity = 10 + (sequence % 50);
            trade.direction = (sequence % 2) ? 1 : 5;

            if (trade_queue.push(trade))
            {
                statistics.trade_produced.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                statistics.trade_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy Thread 시뮬레이션
//   - order_book_queue, trade_queue 소비하며 신호 생성
//   - 신호 발생 빈도: OB 100건당 1개, TD 200건당 1개
//   - OB와 TD 카운터 분리해서 빈도 의도대로 보장
// ─────────────────────────────────────────────────────────────────────────────
static void strategy_fn(RingBuffer<MockOrderBook>& order_book_queue,
    RingBuffer<MockTradeData>& trade_queue,
    RingBuffer<MockOrderSignal>& order_queue,
    PipelineStats& statistics,
    std::atomic<bool>& stop_flag)
{
    uint64_t order_book_counter = 0;
    uint64_t trade_counter = 0;

    while (!stop_flag.load(std::memory_order_relaxed)
        || !order_book_queue.empty() || !trade_queue.empty())
    {
        bool did_work = false;

        while (auto option = order_book_queue.pop()) {
            statistics.order_book_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = 0.0;

            for (int index = 0; index < 5; ++index)
            {
                sink = sink + option->ask_price[index] - option->bid_price[index];
            }

            (void)sink;

            if (++order_book_counter % 100 == 0) {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.origin_sequence = option->sequence;
                signal.side = 0;
                signal.quantity = 10;

                if (order_queue.push(signal))
                {
                    statistics.signals_generated.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    statistics.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }

            did_work = true;
        }

        while (auto option = trade_queue.pop()) {
            statistics.trade_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = option->price * option->quantity;
            (void)sink;

            if (++trade_counter % 200 == 0) {
                MockOrderSignal signal{};
                std::memcpy(signal.ticker, option->ticker, 7);
                signal.send_ts_ns = option->send_ts_ns;
                signal.origin_sequence = option->sequence;
                signal.side = 1;
                signal.quantity = 5;

                if (order_queue.push(signal))
                {
                    statistics.signals_generated.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    statistics.order_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }

            did_work = true;
        }

        (void)did_work;  // busy spin
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order Thread 시뮬레이션
//   - 내부 파이프라인 latency만 측정 (외부 REST 지연 시뮬레이션 제거)
// ─────────────────────────────────────────────────────────────────────────────
static void order_fn(RingBuffer<MockOrderSignal>& order_queue,
    PipelineStats& statistics,
    std::atomic<bool>& stop_flag)
{
    while (!stop_flag.load(std::memory_order_relaxed) || !order_queue.empty()) {
        auto option = order_queue.pop();

        if (!option) {
            continue;   // busy spin
        }

        // E2E latency: producer push 시각 → 여기 도달 시각
        int64_t now_ns = std::chrono::duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();
        int64_t latency = now_ns - option->send_ts_ns;

        if (latency >= 0)
        {
            statistics.e2e_latencies_ns.push_back(latency);
        }

        statistics.orders_processed.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Fill Thread 시뮬레이션 — Engine::fill_thread_fn과 같은 condvar 잠들기·깨우기. 원장 반영 비용은 넣지 않는다.
// ─────────────────────────────────────────────────────────────────────────────
static void fill_fn(RingBuffer<MockFill>& fill_queue,
    PipelineStats& statistics,
    std::atomic<bool>& stop_flag)
{
    while (!stop_flag.load(std::memory_order_relaxed) || !fill_queue.empty()) {
        auto option = fill_queue.pop();

        if (!option) {
            std::unique_lock<std::mutex> lock(statistics.fill_wake_mutex);
            statistics.fill_sleeping.store(true, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (fill_queue.empty() && !stop_flag.load(std::memory_order_relaxed))
            {
                statistics.fill_wake_condition_variable.wait_for(lock, std::chrono::milliseconds(100));
            }

            statistics.fill_sleeping.store(false, std::memory_order_relaxed);
            continue;
        }

        int64_t now_ns = std::chrono::duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();
        int64_t latency = now_ns - option->send_ts_ns;

        if (latency >= 0)
        {
            statistics.fill_latencies_ns.push_back(latency);
        }

        statistics.fill_consumed.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Latency 분위수 출력
// ─────────────────────────────────────────────────────────────────────────────
static void print_latency(std::vector<int64_t>& values, const char* label) {
    std::cout << label << " (count=" << values.size() << "):\n";

    if (values.empty()) { std::cout << "  (no samples)\n"; return; }
    std::sort(values.begin(), values.end());

    auto percent = [&](double price) {
        size_t index = static_cast<size_t>(values.size() * price);

        if (index >= values.size())
        {
            index = values.size() - 1;
        }

        return values[index];
        };
    auto format = [](int64_t count) -> std::string {
        char buffer[32];

        if (count < 1000)
        {
            std::snprintf(buffer, sizeof(buffer), "%lld ns", static_cast<long long>(count));
        }
        else if (count < 1'000'000)
        {
            std::snprintf(buffer, sizeof(buffer), "%lld us", static_cast<long long>(count / 1000));
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%lld ms", static_cast<long long>(count / 1'000'000));
        }

        return std::string(buffer);
        };

    std::cout << "  p50:  " << format(percent(0.50)) << "\n"
        << "  p90:  " << format(percent(0.90)) << "\n"
        << "  p99:  " << format(percent(0.99)) << "\n"
        << "  p999: " << format(percent(0.999)) << "\n"
        << "  max:  " << format(values.back()) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    int duration = (argc > 1) ? std::atoi(argv[1]) : 30;

    if (duration < 1)
    {
        duration = 30;
    }

    std::string mode = (argc > 2) ? argv[2] : "normal";
    int OB_PER_TICKER, TD_PER_TICKER;

    if (mode == "burst") {
        OB_PER_TICKER = 20; TD_PER_TICKER = 50;
    }
    else {
        OB_PER_TICKER = 5;  TD_PER_TICKER = 10;
    }

    std::cout << "=== Pipeline E2E Stress Test ===\n";
    std::cout << "Duration       : " << duration << " sec\n";
    std::cout << "Topology       : WS_producer -> [ob_q, td_q] -> strategy -> order_q -> order_thread\n";
    std::cout << "                 WS_producer -> fill_q -> fill_thread (1 fill / 500 msg)\n";
    std::cout << "Tickers        : " << N_TICKERS << "\n";
    std::cout << "Mode           : " << mode << "\n";
    std::cout << "OB rate        : " << OB_PER_TICKER << " msg/sec/ticker (total "
        << N_TICKERS * OB_PER_TICKER << "/sec)\n";
    std::cout << "TD rate        : " << TD_PER_TICKER << " msg/sec/ticker (total "
        << N_TICKERS * TD_PER_TICKER << "/sec)\n";
    std::cout << "Total in       : " << N_TICKERS * (OB_PER_TICKER + TD_PER_TICKER) << " msg/sec\n\n";

    RingBuffer<MockOrderBook>   order_book_queue(16384);
    RingBuffer<MockTradeData>   trade_queue(16384);
    RingBuffer<MockOrderSignal> order_queue(1024);
    RingBuffer<MockFill>        fill_queue(1024);   // Engine::fill_queue_와 같은 깊이

    PipelineStats statistics;
    statistics.e2e_latencies_ns.reserve(500'000);
    statistics.fill_latencies_ns.reserve(10'000);
    std::atomic<bool> stop_flag{ false };

    auto start_time = steady_clock::now();

    std::thread websocket_thread(websocket_producer_fn, std::ref(order_book_queue), std::ref(trade_queue), std::ref(fill_queue),
        std::ref(statistics), std::ref(stop_flag), duration,
        OB_PER_TICKER, TD_PER_TICKER);
    std::thread strategy_thread(strategy_fn, std::ref(order_book_queue), std::ref(trade_queue),
        std::ref(order_queue), std::ref(statistics), std::ref(stop_flag));
    std::thread order_thread(order_fn, std::ref(order_queue), std::ref(statistics), std::ref(stop_flag));
    std::thread t_fill(fill_fn, std::ref(fill_queue), std::ref(statistics), std::ref(stop_flag));

    // 진행 상황 5초마다 출력
    while (steady_clock::now() - start_time < std::chrono::seconds(duration)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto el = std::chrono::duration_cast<std::chrono::seconds>(
            steady_clock::now() - start_time).count();
        std::cout << "  [t+" << std::setw(3) << el << "s] "
            << "OB " << statistics.order_book_produced.load() << "/" << statistics.order_book_consumed.load()
            << "/" << statistics.order_book_drops.load()
            << " | TD " << statistics.trade_produced.load() << "/" << statistics.trade_consumed.load()
            << "/" << statistics.trade_drops.load()
            << " | sig " << statistics.signals_generated.load()
            << " | ord " << statistics.orders_processed.load()
            << " | fill " << statistics.fill_produced.load() << "/" << statistics.fill_consumed.load()
            << "/" << statistics.fill_drops.load()
            << " | qsize ob=" << (statistics.order_book_produced.load() - statistics.order_book_consumed.load())
            << " td=" << (statistics.trade_produced.load() - statistics.trade_consumed.load())
            << "\n";
    }

    websocket_thread.join();
    stop_flag.store(true);
    strategy_thread.join();
    order_thread.join();
    t_fill.join();

    auto end_time = steady_clock::now();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "\n=== Results ===\n";
    std::cout << "Elapsed        : " << milliseconds << " ms\n";
    std::cout << "OB    produced/consumed/dropped : "
        << statistics.order_book_produced.load() << " / "
        << statistics.order_book_consumed.load() << " / "
        << statistics.order_book_drops.load() << "\n";
    std::cout << "TD    produced/consumed/dropped : "
        << statistics.trade_produced.load() << " / "
        << statistics.trade_consumed.load() << " / "
        << statistics.trade_drops.load() << "\n";
    std::cout << "Order generated/processed/dropped : "
        << statistics.signals_generated.load() << " / "
        << statistics.orders_processed.load() << " / "
        << statistics.order_drops.load() << "\n";
    std::cout << "Fill  produced/consumed/dropped : "
        << statistics.fill_produced.load() << " / "
        << statistics.fill_consumed.load() << " / "
        << statistics.fill_drops.load() << "\n\n";

    print_latency(statistics.e2e_latencies_ns, "E2E latency (input -> order_thread)");
    print_latency(statistics.fill_latencies_ns, "Fill latency (ws -> fill_thread, condvar wake)");

    bool ok = (statistics.order_book_drops.load() == 0)
        && (statistics.trade_drops.load() == 0)
        && (statistics.order_drops.load() == 0)
        && (statistics.order_book_produced.load() == statistics.order_book_consumed.load())
        && (statistics.trade_produced.load() == statistics.trade_consumed.load())
        && (statistics.signals_generated.load() == statistics.orders_processed.load())
        && (statistics.fill_drops.load() == 0)
        && (statistics.fill_produced.load() == statistics.fill_consumed.load());

    std::cout << "\n[" << (ok ? "PASS" : "FAIL")
        << "] no drops, no order/fill backlog\n";
    return ok ? 0 : 1;
}