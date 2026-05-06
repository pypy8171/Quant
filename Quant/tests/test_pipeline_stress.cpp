// tests/test_pipeline_stress.cpp
// End-to-End 파이프라인 부하 테스트
//
// 시뮬레이션 구조:
//   WS recv_thread → ob_queue_/td_queue_ → strategy_thread
//                                        → order_queue_ → order_thread
//
// Engine.cpp의 실제 3-stage 큐 파이프를 mock 데이터로 재현 테스트.
// Windows Sleep 부정확성 회피를 위해 busy-wait 기반 rate limiting 사용.
//
// 사용법: test_pipeline_stress [duration_sec] [mode]
//   mode: "normal" (기본, 3,000 msg/sec) | "burst" (14,000 msg/sec)

#include "core/RingBuffer.h"
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

using clk = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

// ─────────────────────────────────────────────────────────────────────────────
// 티커 200개 동적 생성
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int N_TICKERS = 200;

static std::vector<std::string> make_tickers() {
    std::vector<std::string> v;
    v.reserve(N_TICKERS);
    for (int i = 1; i <= N_TICKERS; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%06d", i);
        v.emplace_back(buf);
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock 구조체 정의 (실제 Types.h와 동일 크기)
// ─────────────────────────────────────────────────────────────────────────────
struct MockOrderBook {
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t seq;
    double   ask_price[5];
    int64_t  ask_qty[5];
    double   bid_price[5];
    int64_t  bid_qty[5];
};

struct MockTradeData {
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t seq;
    double   price;
    int64_t  quantity;
    int      direction;
};

struct MockOrderSignal {
    char     ticker[8];
    int64_t  send_ts_ns;
    uint64_t origin_seq;   // 어느 입력 메시지에서 파생됐는가
    int      side;          // 0=BUY, 1=SELL
    int      quantity;
};

// ─────────────────────────────────────────────────────────────────────────────
// Busy-wait 기반 정확한 sleep (Windows Sleep 부정확성 회피)
// ─────────────────────────────────────────────────────────────────────────────
static inline void busy_wait_until(clk::time_point deadline) {
    while (clk::now() < deadline) {
        // pure busy spin — Windows scheduler 회피
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 통계
// ─────────────────────────────────────────────────────────────────────────────
struct PipelineStats {
    std::atomic<uint64_t> ob_produced{ 0 };
    std::atomic<uint64_t> ob_consumed{ 0 };
    std::atomic<uint64_t> td_produced{ 0 };
    std::atomic<uint64_t> td_consumed{ 0 };
    std::atomic<uint64_t> signals_generated{ 0 };
    std::atomic<uint64_t> orders_processed{ 0 };
    std::atomic<uint64_t> ob_drops{ 0 };
    std::atomic<uint64_t> td_drops{ 0 };
    std::atomic<uint64_t> order_drops{ 0 };

    // E2E latency: producer push → order_thread 처리 완료
    // NOTE: order_thread 단독 producer. 다른 스레드 추가 시 mutex 또는
    //       per-thread vector 후 합산 필요 (현재 std::vector는 thread-safe 아님)
    std::vector<int64_t> e2e_latencies_ns;
};

// ─────────────────────────────────────────────────────────────────────────────
// WS 시뮬레이션 Producer
//   - N_TICKERS 종목, 종목당 ob_rate OB/sec + td_rate TD/sec
// ─────────────────────────────────────────────────────────────────────────────
static void ws_producer_fn(RingBuffer<MockOrderBook>& ob_q,
    RingBuffer<MockTradeData>& td_q,
    PipelineStats& stats,
    std::atomic<bool>& stop_flag,
    int duration_sec,
    int ob_rate,
    int td_rate)
{
    static const auto TICKERS = make_tickers();  // 한 번만 생성

    const int     total_rate = N_TICKERS * (ob_rate + td_rate);
    const int64_t INTERVAL_US = 1'000'000 / total_rate;

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> ticker_dist(0, N_TICKERS - 1);
    // OB:TD 비율 = ob_rate:(td_rate) → type_dist 범위로 근사
    // ob_rate/(ob_rate+td_rate) 확률로 OB, 나머지 TD
    const int type_range = ob_rate + td_rate;
    std::uniform_int_distribution<int> type_dist(0, type_range - 1);

    auto deadline = clk::now() + std::chrono::seconds(duration_sec);
    auto next_send = clk::now();
    uint64_t seq = 0;

    while (!stop_flag.load(std::memory_order_relaxed) && clk::now() < deadline) {
        busy_wait_until(next_send);
        next_send += std::chrono::microseconds(INTERVAL_US);

        int     tk_idx = ticker_dist(rng);
        bool    is_ob = (type_dist(rng) < ob_rate);
        int64_t now_ns = std::chrono::duration_cast<ns>(
            clk::now().time_since_epoch()).count();

        if (is_ob) {
            MockOrderBook ob{};
            std::memcpy(ob.ticker, TICKERS[tk_idx].c_str(), 7);
            ob.send_ts_ns = now_ns;
            ob.seq = seq++;
            for (int i = 0; i < 5; ++i) {
                ob.ask_price[i] = 70000.0 + i * 10;
                ob.ask_qty[i] = 100 * (i + 1);
                ob.bid_price[i] = 69990.0 - i * 10;
                ob.bid_qty[i] = 100 * (i + 1);
            }
            if (ob_q.push(ob)) stats.ob_produced.fetch_add(1, std::memory_order_relaxed);
            else                stats.ob_drops.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            MockTradeData td{};
            std::memcpy(td.ticker, TICKERS[tk_idx].c_str(), 7);
            td.send_ts_ns = now_ns;
            td.seq = seq++;
            td.price = 70000.0 + (seq % 100);
            td.quantity = 10 + (seq % 50);
            td.direction = (seq % 2) ? 1 : 5;
            if (td_q.push(td)) stats.td_produced.fetch_add(1, std::memory_order_relaxed);
            else                stats.td_drops.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy Thread 시뮬레이션
//   - ob_queue, td_queue 소비하며 신호 생성
//   - 신호 발생 빈도: OB 100건당 1개, TD 200건당 1개
//   - OB와 TD 카운터 분리해서 빈도 의도대로 보장
// ─────────────────────────────────────────────────────────────────────────────
static void strategy_fn(RingBuffer<MockOrderBook>& ob_q,
    RingBuffer<MockTradeData>& td_q,
    RingBuffer<MockOrderSignal>& order_q,
    PipelineStats& stats,
    std::atomic<bool>& stop_flag)
{
    uint64_t ob_counter = 0;
    uint64_t td_counter = 0;

    while (!stop_flag.load(std::memory_order_relaxed)
        || !ob_q.empty() || !td_q.empty())
    {
        bool did_work = false;

        while (auto opt = ob_q.pop()) {
            stats.ob_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = 0.0;
            for (int i = 0; i < 5; ++i)
                sink += opt->ask_price[i] - opt->bid_price[i];
            (void)sink;

            if (++ob_counter % 100 == 0) {
                MockOrderSignal sig{};
                std::memcpy(sig.ticker, opt->ticker, 7);
                sig.send_ts_ns = opt->send_ts_ns;
                sig.origin_seq = opt->seq;
                sig.side = 0;
                sig.quantity = 10;
                if (order_q.push(sig))
                    stats.signals_generated.fetch_add(1, std::memory_order_relaxed);
                else
                    stats.order_drops.fetch_add(1, std::memory_order_relaxed);
            }
            did_work = true;
        }

        while (auto opt = td_q.pop()) {
            stats.td_consumed.fetch_add(1, std::memory_order_relaxed);
            volatile double sink = opt->price * opt->quantity;
            (void)sink;

            if (++td_counter % 200 == 0) {
                MockOrderSignal sig{};
                std::memcpy(sig.ticker, opt->ticker, 7);
                sig.send_ts_ns = opt->send_ts_ns;
                sig.origin_seq = opt->seq;
                sig.side = 1;
                sig.quantity = 5;
                if (order_q.push(sig))
                    stats.signals_generated.fetch_add(1, std::memory_order_relaxed);
                else
                    stats.order_drops.fetch_add(1, std::memory_order_relaxed);
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
static void order_fn(RingBuffer<MockOrderSignal>& order_q,
    PipelineStats& stats,
    std::atomic<bool>& stop_flag)
{
    while (!stop_flag.load(std::memory_order_relaxed) || !order_q.empty()) {
        auto opt = order_q.pop();
        if (!opt) {
            continue;   // busy spin
        }

        // E2E latency: producer push 시각 → 여기 도달 시각
        int64_t now_ns = std::chrono::duration_cast<ns>(
            clk::now().time_since_epoch()).count();
        int64_t latency = now_ns - opt->send_ts_ns;
        if (latency >= 0) stats.e2e_latencies_ns.push_back(latency);

        stats.orders_processed.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Latency 분위수 출력
// ─────────────────────────────────────────────────────────────────────────────
static void print_latency(std::vector<int64_t>& v, const char* label) {
    std::cout << label << " (count=" << v.size() << "):\n";
    if (v.empty()) { std::cout << "  (no samples)\n"; return; }
    std::sort(v.begin(), v.end());

    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(v.size() * p);
        if (idx >= v.size()) idx = v.size() - 1;
        return v[idx];
        };
    auto fmt = [](int64_t n) -> std::string {
        char buf[32];
        if (n < 1000)
            std::snprintf(buf, sizeof(buf), "%lld ns", (long long)n);
        else if (n < 1'000'000)
            std::snprintf(buf, sizeof(buf), "%lld us", (long long)(n / 1000));
        else
            std::snprintf(buf, sizeof(buf), "%lld ms", (long long)(n / 1'000'000));
        return std::string(buf);
        };

    std::cout << "  p50:  " << fmt(pct(0.50)) << "\n"
        << "  p90:  " << fmt(pct(0.90)) << "\n"
        << "  p99:  " << fmt(pct(0.99)) << "\n"
        << "  p999: " << fmt(pct(0.999)) << "\n"
        << "  max:  " << fmt(v.back()) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    int duration = (argc > 1) ? std::atoi(argv[1]) : 30;
    if (duration < 1) duration = 30;

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
    std::cout << "Tickers        : " << N_TICKERS << "\n";
    std::cout << "Mode           : " << mode << "\n";
    std::cout << "OB rate        : " << OB_PER_TICKER << " msg/sec/ticker (total "
        << N_TICKERS * OB_PER_TICKER << "/sec)\n";
    std::cout << "TD rate        : " << TD_PER_TICKER << " msg/sec/ticker (total "
        << N_TICKERS * TD_PER_TICKER << "/sec)\n";
    std::cout << "Total in       : " << N_TICKERS * (OB_PER_TICKER + TD_PER_TICKER) << " msg/sec\n\n";

    RingBuffer<MockOrderBook>   ob_q(16384);
    RingBuffer<MockTradeData>   td_q(16384);
    RingBuffer<MockOrderSignal> order_q(1024);

    PipelineStats stats;
    stats.e2e_latencies_ns.reserve(500'000);
    std::atomic<bool> stop_flag{ false };

    auto t0 = clk::now();

    std::thread t_ws(ws_producer_fn, std::ref(ob_q), std::ref(td_q),
        std::ref(stats), std::ref(stop_flag), duration,
        OB_PER_TICKER, TD_PER_TICKER);
    std::thread t_strat(strategy_fn, std::ref(ob_q), std::ref(td_q),
        std::ref(order_q), std::ref(stats), std::ref(stop_flag));
    std::thread t_ord(order_fn, std::ref(order_q), std::ref(stats), std::ref(stop_flag));

    // 진행 상황 5초마다 출력
    while (clk::now() - t0 < std::chrono::seconds(duration)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto el = std::chrono::duration_cast<std::chrono::seconds>(
            clk::now() - t0).count();
        std::cout << "  [t+" << std::setw(3) << el << "s] "
            << "OB " << stats.ob_produced.load() << "/" << stats.ob_consumed.load()
            << "/" << stats.ob_drops.load()
            << " | TD " << stats.td_produced.load() << "/" << stats.td_consumed.load()
            << "/" << stats.td_drops.load()
            << " | sig " << stats.signals_generated.load()
            << " | ord " << stats.orders_processed.load()
            << " | qsize ob=" << (stats.ob_produced.load() - stats.ob_consumed.load())
            << " td=" << (stats.td_produced.load() - stats.td_consumed.load())
            << "\n";
    }

    t_ws.join();
    stop_flag.store(true);
    t_strat.join();
    t_ord.join();

    auto t1 = clk::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::cout << "\n=== Results ===\n";
    std::cout << "Elapsed        : " << ms << " ms\n";
    std::cout << "OB    produced/consumed/dropped : "
        << stats.ob_produced.load() << " / "
        << stats.ob_consumed.load() << " / "
        << stats.ob_drops.load() << "\n";
    std::cout << "TD    produced/consumed/dropped : "
        << stats.td_produced.load() << " / "
        << stats.td_consumed.load() << " / "
        << stats.td_drops.load() << "\n";
    std::cout << "Order generated/processed/dropped : "
        << stats.signals_generated.load() << " / "
        << stats.orders_processed.load() << " / "
        << stats.order_drops.load() << "\n\n";

    print_latency(stats.e2e_latencies_ns, "E2E latency (input -> order_thread)");

    bool ok = (stats.ob_drops.load() == 0)
        && (stats.td_drops.load() == 0)
        && (stats.order_drops.load() == 0)
        && (stats.ob_produced.load() == stats.ob_consumed.load())
        && (stats.td_produced.load() == stats.td_consumed.load())
        && (stats.signals_generated.load() == stats.orders_processed.load());

    std::cout << "\n[" << (ok ? "PASS" : "FAIL")
        << "] no drops, no order backlog\n";
    return ok ? 0 : 1;
}