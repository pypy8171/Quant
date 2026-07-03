// tests/bench_intake.cpp
// 멀티클라이언트 주문 인테이크 부하 벤치 (D2)
//
//   N개 생산자(클라이언트)가 동시에 OrderSignal을 push → 단일 소비자(FEP)가 pop.
//   큐 종류(mpsc/mutex)와 소비자 처리지연(exec_delay_us)을 바꿔가며
//   처리량(orders/sec)·E2E 지연(p50/p99/p999)·backpressure·계좌 fairness를 측정.
//
//   ⚠ 이 단계(D2)는 "큐 자체"를 격리 측정한다. 소비자는 실제 OrderRouter::submit 대신
//     FEP 처리시간을 busy-wait로 시뮬레이션한다(exec_delay_us). 실 OrderRouter·계좌별
//     원장 결합은 D3에서. 큐 비교(MPSC vs Mutex)가 downstream 노이즈에 묻히지 않게 함.
//
//   측정 원칙(개발계획 7장 함정 대응):
//     - 지연 시뮬레이션은 sleep 금지(Windows 부정확) → busy-wait
//     - 지연 샘플은 소비자 단일 스레드에서만 수집 (vector 다중 push는 UB)
//     - release 빌드로만 측정 (debug/ASan 무의미)
//     - N > 물리코어면 컨텍스트 스위칭이 지연 지배 → hardware_concurrency 병기
//
//   사용법: bench_intake [N_producers=8] [duration_sec=3] [mpsc|mutex] [exec_delay_us=0]
//                        [capacity=65536] [rate_per_producer=0]
//     rate_per_producer=0 → 풀스로틀(open-loop): 최대 처리량 측정용. 큐 포화 → 지연은
//       "큐깊이/처리율" 백로그 지연이라 지연 수치는 무의미. throughput만 신뢰.
//     rate_per_producer>0 → 목표 rate로 pacing(초당 건수): 정상 부하에서 E2E 지연 측정용.
//       총 offered = N × rate. 처리능력 미만으로 걸면 큐가 얕게 유지돼 µs 단위 지연이 나온다.

#include "core/MpscQueue.h"
#include "core/MutexQueue.h"
#include "core/Types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using clk = std::chrono::steady_clock;

static inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count();
}

// FEP 처리시간 시뮬레이션 — sleep은 부정확하므로 busy-wait
static inline void busy_wait_ns(int64_t dur_ns)
{
    if (dur_ns <= 0)
        return;
    const int64_t end = now_ns() + dur_ns;
    while (now_ns() < end)
    {
        /* spin */
    }
}

// 큐에 싣는 원소: 실제 OrderSignal 페이로드(현실적 크기) + 인테이크 타임스탬프 + 계좌 태그
struct IntakeMsg
{
    OrderSignal sig;
    int64_t enqueue_ns = 0;
    int account = 0;
};

struct Result
{
    long long produced = 0;    // 생산자가 성공적으로 push한 총건수
    long long consumed = 0;    // 소비자가 pop해 처리한 총건수
    long long push_retries = 0; // backpressure(가득 참)로 재시도한 횟수
    double dur_sec = 0;
    std::vector<int64_t> lat_ns;      // E2E 지연 샘플 (소비자 단독 수집)
    std::vector<long long> per_account; // 계좌별 처리건수 (fairness)
};

template <typename Queue>
Result run(int N, double duration_sec, int64_t exec_delay_ns, size_t cap, double rate_per_producer)
{
    Queue q(cap);
    std::atomic<bool> stop{false};
    std::atomic<long long> produced{0};
    std::atomic<long long> push_retries{0};

    // ── 생산자 N개: 각자 계좌 태그(p)로 OrderSignal을 계속 밀어넣음(가득 차면 yield 재시도)
    std::vector<std::thread> producers;
    producers.reserve(N);
    for (int p = 0; p < N; ++p)
    {
        producers.emplace_back(
            [&q, &stop, &produced, &push_retries, p, rate_per_producer]
            {
                OrderSignal base;
                base.ticker = "005930";
                base.side = OrderSide::BUY;
                base.type = OrderType::LIMIT;
                base.quantity = 1;
                base.price = 75000.0;
                base.strategy_id = "BENCH";
                base.market = Market::KR;

                // rate>0이면 목표 간격으로 pacing (busy-wait). rate=0이면 풀스로틀.
                const int64_t interval_ns =
                    (rate_per_producer > 0) ? static_cast<int64_t>(1e9 / rate_per_producer) : 0;
                int64_t next_emit = now_ns();

                long long local = 0, retries = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    if (interval_ns > 0)
                    {
                        const int64_t t = now_ns();
                        if (t < next_emit)
                            busy_wait_ns(next_emit - t); // 스케줄 시각까지 대기
                        next_emit += interval_ns;
                    }
                    IntakeMsg m;
                    m.sig = base;
                    m.account = p;
                    m.enqueue_ns = now_ns();
                    if (q.push(m))
                        ++local;
                    else
                    {
                        ++retries;
                        std::this_thread::yield(); // backpressure: 가득 참 → 양보 후 재시도
                    }
                }
                produced.fetch_add(local, std::memory_order_relaxed);
                push_retries.fetch_add(retries, std::memory_order_relaxed);
            });
    }

    // ── 소비자 1개 (메인 스레드): duration 동안 pop→처리, 지연 측정
    // 지연 샘플 상한 — 초과분은 수집 중단. reserve로 미리 잡아 측정 중 재할당(소비자 스톨)
    // 을 원천 차단한다 (W-1: 재할당이 throughput·tail을 왜곡하던 문제).
    const size_t LAT_CAP = 1u << 23; // 8.4M
    std::vector<int64_t> lat;
    lat.reserve(LAT_CAP);
    std::vector<long long> per_account(N, 0);
    long long consumed = 0;

    const auto t0 = clk::now();
    const auto t_end =
        t0 + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(duration_sec));

    while (clk::now() < t_end)
    {
        auto opt = q.pop();
        if (!opt)
        {
            std::this_thread::yield();
            continue;
        }
        const int64_t latency = now_ns() - opt->enqueue_ns;
        busy_wait_ns(exec_delay_ns); // FEP 처리 시뮬레이션
        if (opt->account >= 0 && opt->account < N)
            ++per_account[opt->account];
        if (lat.size() < LAT_CAP)
            lat.push_back(latency);
        ++consumed;
    }

    // ── 종료: 생산자 정지 후 큐 잔량 드레인 (무손실 검증 위해 끝까지 비움)
    // 이 구간 항목은 측정창이 닫힌 뒤의 백로그라 지연이 크게 부풀어 있다. percentile을
    // 오염시키므로 lat에 넣지 않고 consumed만 카운트한다 (W-2).
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : producers)
        t.join();
    while (auto opt = q.pop())
    {
        if (opt->account >= 0 && opt->account < N)
            ++per_account[opt->account];
        ++consumed;
    }

    Result r;
    r.produced = produced.load();
    r.consumed = consumed;
    r.push_retries = push_retries.load();
    r.dur_sec = std::chrono::duration<double>(clk::now() - t0).count();
    r.lat_ns = std::move(lat);
    r.per_account = std::move(per_account);
    return r;
}

static double pct_us(std::vector<int64_t>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
    return sorted[idx] / 1000.0; // ns → µs
}

// 계좌별 처리건수의 변동계수(CV = stdev/mean) — 0에 가까울수록 공평
static double fairness_cv(const std::vector<long long>& v)
{
    if (v.empty())
        return 0.0;
    double sum = 0;
    for (auto x : v)
        sum += static_cast<double>(x);
    double mean = sum / v.size();
    if (mean == 0)
        return 0.0;
    double var = 0;
    for (auto x : v)
        var += (x - mean) * (x - mean);
    var /= v.size();
    return std::sqrt(var) / mean;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    int N = (argc > 1) ? std::atoi(argv[1]) : 8;
    double duration = (argc > 2) ? std::atof(argv[2]) : 3.0;
    std::string qtype = (argc > 3) ? argv[3] : "mpsc";
    int64_t delay_ns = (argc > 4) ? static_cast<int64_t>(std::atoll(argv[4])) * 1000 : 0;
    size_t cap = (argc > 5) ? static_cast<size_t>(std::atoll(argv[5])) : 65536;
    double rate = (argc > 6) ? std::atof(argv[6]) : 0.0;
    if (N < 1)
        N = 1;
    // 두 큐가 동일 실용량을 갖도록 2^n로 정규화(최소 2) — 대조군 공정성 + cap=0 라이브락 방지 (W-3)
    cap = mpsc_detail::round_up_pow2(cap);

    Result r;
    if (qtype == "mutex")
        r = run<MutexQueue<IntakeMsg>>(N, duration, delay_ns, cap, rate);
    else
    {
        qtype = "mpsc";
        r = run<MpscQueue<IntakeMsg>>(N, duration, delay_ns, cap, rate);
    }

    std::sort(r.lat_ns.begin(), r.lat_ns.end());
    const double thr = (r.dur_sec > 0) ? r.consumed / r.dur_sec : 0.0;
    const long long lost = r.produced - r.consumed; // 무손실이면 0
    const unsigned hw = std::thread::hardware_concurrency();

    std::printf("\n=== bench_intake (%s) ===\n", qtype.c_str());
    std::printf("producers(N)   : %d  (hardware_concurrency=%u)\n", N, hw);
    std::printf("duration        : %.3f sec   capacity=%zu   exec_delay=%.1f us\n", r.dur_sec, cap,
                delay_ns / 1000.0);
    if (rate > 0)
        std::printf("offered load    : %.0f/producer × %d = %.0f orders/sec (paced, 지연 측정 모드)\n",
                    rate, N, rate * N);
    else
        std::printf("offered load    : unlimited (풀스로틀, 처리량 측정 모드 — 지연은 백로그 지배)\n");
    std::printf("produced        : %lld\n", r.produced);
    std::printf("consumed        : %lld   (lost = produced-consumed = %lld)\n", r.consumed, lost);
    std::printf("throughput      : %.0f orders/sec\n", thr);
    std::printf("E2E latency (us): p50=%.2f  p99=%.2f  p999=%.2f  max=%.2f\n",
                pct_us(r.lat_ns, 0.50), pct_us(r.lat_ns, 0.99), pct_us(r.lat_ns, 0.999),
                r.lat_ns.empty() ? 0.0 : r.lat_ns.back() / 1000.0);
    std::printf("push_retries    : %lld  (backpressure)\n", r.push_retries);
    std::printf("fairness CV     : %.4f  (계좌별 처리건수 편차, 0=완전공평)\n",
                fairness_cv(r.per_account));

    // CSV 한 줄 (D4 스윕 집계용): queue,N,delay_us,dur,produced,consumed,lost,thr,p50,p99,p999,retries,cv,hw
    std::printf("CSV,%s,%d,%.1f,%.3f,%lld,%lld,%lld,%.0f,%.2f,%.2f,%.2f,%lld,%.4f,%u\n", qtype.c_str(),
                N, delay_ns / 1000.0, r.dur_sec, r.produced, r.consumed, lost, thr,
                pct_us(r.lat_ns, 0.50), pct_us(r.lat_ns, 0.99), pct_us(r.lat_ns, 0.999),
                r.push_retries, fairness_cv(r.per_account), hw);

    return (lost == 0) ? 0 : 1; // 무손실 아니면 실패
}
