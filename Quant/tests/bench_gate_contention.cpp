// tests/bench_gate_contention.cpp
// OrderGate::positions_mtx_ 경합 벤치(감사 L4) — 전략 스레드가 틱마다 부르는 원장 읽기(position·
//  sellable_view)가 다른 스레드의 쓰기(체결 반영·잔고 대조 정리·운영단말 스냅샷)에 얼마나 막히는지 잰다.
//  읽기 한 번의 지연(중앙값(p50)·상위 1%(p99)·상위 0.1%(p999)·최악)을 쓰기 없는 기준선과 나란히 찍는다.
//
// [inv] 측정 범위 — 원장 40슬롯(보유 상한)·계좌 하나. 쓰기 쪽은 실물 빈도를 흉내낸다: 체결은 초당 N건
//   (라이브 하루 수백 건보다 훨씬 많게), 잔고 대조는 M ms마다 스냅샷+정리+재시드(실물 60초). 지연 샘플은
//   읽기 스레드 하나에서만 모은다. release 빌드로만 잰다.
//
// 사용법: bench_gate_contention [duration_sec=3] [fill_rate=1000] [reconcile_ms=100] [ops_ms=200]
//   fill_rate=0 이고 reconcile_ms=0 이고 ops_ms=0 이면 기준선(쓰기 없음)만 찍는다.
//   같은 인자로 기준선을 먼저 찍고 부하를 건 결과를 이어 찍는다.
// 관련 결정: D-058.
#include "risk/OrderGate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{

using clk = std::chrono::steady_clock;
using ns  = std::chrono::nanoseconds;

constexpr int kSlots = 40;

OrderGate::Config bench_cfg()
{
    OrderGate::Config c;
    c.max_qty_per_ticker = 1'000'000;
    c.max_orders_per_min = 1'000'000;
    c.max_orders_per_sec = 1'000'000;
    c.dedup_window_sec   = 0.0;
    return c;
}

std::string tkr(int i)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06d", i + 1);
    return buf;
}

// 지연 시뮬레이션은 sleep 금지(Windows 타이머 해상도)라 busy-wait
void spin_until(clk::time_point t)
{
    while (clk::now() < t)
    {
    }
}

double pct_ns(const std::vector<int64_t>& sorted, double p)
{
    if (sorted.empty())
    {
        return 0.0;
    }

    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[idx]);
}

struct Result
{
    uint64_t             reads = 0;
    std::vector<int64_t> lat_ns;
};

// 읽기 스레드(전략 스레드 역할). 틱마다 position 한 번 + sellable_view 한 번 — DeviationScale이 틱당 부르는 양.
Result run_reader(const OrderGate& gate, const std::string& acct, double duration_sec, std::atomic<bool>& stop)
{
    Result r;
    r.lat_ns.reserve(1 << 22);
    const auto end = clk::now() + std::chrono::milliseconds(static_cast<int>(duration_sec * 1000));
    int i = 0;
    volatile int sink = 0;

    while (clk::now() < end)
    {
        const std::string t = tkr(i % kSlots);
        const auto t0 = clk::now();
        sink = sink + gate.position(acct, t);
        sink = sink + gate.sellable_view(acct, t).psbl_cap;
        const auto t1 = clk::now();
        r.lat_ns.push_back(std::chrono::duration_cast<ns>(t1 - t0).count());
        ++r.reads;
        ++i;
    }

    stop.store(true, std::memory_order_release);
    return r;
}

// 체결 스레드 역할 — 선점 뒤 체결. BUY/SELL을 번갈아 보유를 1주씩 흔든다.
void run_filler(OrderGate& gate, const std::string& acct, int rate, std::atomic<bool>& stop)
{
    if (rate <= 0)
    {
        return;
    }

    const auto period = ns(1'000'000'000LL / rate);
    auto next = clk::now();
    int i = 0;

    while (!stop.load(std::memory_order_acquire))
    {
        const std::string t = tkr(i % kSlots);
        const OrderSide side = (i / kSlots) % 2 == 0 ? OrderSide::BUY : OrderSide::SELL;
        gate.on_accept(acct, t, side, 1, 10000.0);
        gate.on_fill_confirmed(acct, t, side, 1, 10000.0);
        ++i;
        next += period;
        spin_until(next);
    }
}

// 잔고 대조 역할(데이터 스레드) — 스냅샷을 뜨고, 살아 있는 종목만 남기고, 잔고대로 다시 시드한다.
void run_reconciler(OrderGate& gate, const std::string& acct, int every_ms, std::atomic<bool>& stop)
{
    if (every_ms <= 0)
    {
        return;
    }

    std::vector<std::string> live;

    for (int i = 0; i < kSlots; ++i)
    {
        live.push_back(tkr(i));
    }

    auto next = clk::now();

    while (!stop.load(std::memory_order_acquire))
    {
        next += std::chrono::milliseconds(every_ms);
        spin_until(next);
        (void)gate.snapshot_positions();
        (void)gate.prune_positions(live, 0);

        for (int i = 0; i < kSlots; ++i)
        {
            gate.seed_position(acct, tkr(i), 10, 10000.0, 10);
        }
    }
}

// 운영단말 역할 — 보유 스냅샷만 주기적으로 뜬다.
void run_ops(const OrderGate& gate, int every_ms, std::atomic<bool>& stop)
{
    if (every_ms <= 0)
    {
        return;
    }

    auto next = clk::now();

    while (!stop.load(std::memory_order_acquire))
    {
        next += std::chrono::milliseconds(every_ms);
        spin_until(next);
        (void)gate.snapshot_positions();
    }
}

void report(const char* label, Result& r, double duration_sec)
{
    std::sort(r.lat_ns.begin(), r.lat_ns.end());
    std::printf("%-22s reads=%llu (%.2fM/s)  p50=%.0fns  p99=%.0fns  p999=%.0fns  max=%.0fns\n", label,
                static_cast<unsigned long long>(r.reads), static_cast<double>(r.reads) / duration_sec / 1e6,
                pct_ns(r.lat_ns, 0.50), pct_ns(r.lat_ns, 0.99), pct_ns(r.lat_ns, 0.999),
                r.lat_ns.empty() ? 0.0 : static_cast<double>(r.lat_ns.back()));
}

void run_case(const char* label, double duration_sec, int fill_rate, int reconcile_ms, int ops_ms)
{
    OrderGate gate(bench_cfg());
    const std::string acct = "12345678-01";

    for (int i = 0; i < kSlots; ++i)
    {
        gate.seed_position(acct, tkr(i), 10, 10000.0, 10);
    }

    std::atomic<bool> stop{false};
    std::thread filler([&] { run_filler(gate, acct, fill_rate, stop); });
    std::thread reconciler([&] { run_reconciler(gate, acct, reconcile_ms, stop); });
    std::thread ops([&] { run_ops(gate, ops_ms, stop); });
    Result r = run_reader(gate, acct, duration_sec, stop);
    filler.join();
    reconciler.join();
    ops.join();
    report(label, r, duration_sec);
}

} // namespace

int main(int argc, char** argv)
{
    const double duration_sec = argc > 1 ? std::atof(argv[1]) : 3.0;
    const int    fill_rate    = argc > 2 ? std::atoi(argv[2]) : 1000;
    const int    reconcile_ms = argc > 3 ? std::atoi(argv[3]) : 100;
    const int    ops_ms       = argc > 4 ? std::atoi(argv[4]) : 200;

    std::printf("bench_gate_contention  slots=%d  duration=%.1fs  hw_threads=%u\n", kSlots, duration_sec,
                std::thread::hardware_concurrency());
    run_case("baseline(no writers)", duration_sec, 0, 0, 0);

    if (fill_rate > 0 || reconcile_ms > 0 || ops_ms > 0)
    {
        char label[64];
        std::snprintf(label, sizeof(label), "fill=%d/s rec=%dms ops=%dms", fill_rate, reconcile_ms, ops_ms);
        run_case(label, duration_sec, fill_rate, reconcile_ms, ops_ms);
    }

    return 0;
}
