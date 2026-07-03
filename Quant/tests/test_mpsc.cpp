// tests/test_mpsc.cpp
// MpscQueue / MutexQueue 정확성 검증 (D1)
//
// 검증 항목:
//   ① 무손실   : N 생산자가 각 M건 push → 소비자가 정확히 N*M건 pop (유실/중복 0)
//   ② 순번 보존 : 각 생산자의 seq가 소비자에서 0,1,2,... 순서로 도착 (역전/누락 0)
//   ③ backpressure: 작은 용량에서 push가 false를 반환해도 재시도로 무손실 유지
//
//   ②가 성립하는 이유: 단일 생산자는 순서대로 push하고, Vyukov 큐는 티켓(enqueue_pos)
//   순으로 소비되므로 "생산자별" FIFO가 보존된다. (전역 FIFO는 보장하지 않음)
//
// 사용법: test_mpsc

#include "core/MpscQueue.h"
#include "core/MutexQueue.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

struct Msg
{
    int producer;
    int seq;
};

template <typename Queue>
bool run_test(const char* name, int num_producers, int per_producer, size_t cap)
{
    Queue q(cap);
    const long long total = static_cast<long long>(num_producers) * per_producer;

    // ── 생산자 N개: 각자 (producer_id, 0..M-1) 를 순서대로 push (가득 차면 재시도)
    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (int p = 0; p < num_producers; ++p)
    {
        producers.emplace_back(
            [&q, p, per_producer]
            {
                for (int i = 0; i < per_producer; ++i)
                {
                    Msg m{p, i};
                    while (!q.push(m))
                        std::this_thread::yield(); // backpressure: 가득 참 → 양보 후 재시도
                }
            });
    }

    // ── 소비자 1개: 총 N*M건을 받을 때까지 pop, 생산자별 순번 검증
    std::vector<int> last_seq(num_producers, -1);
    long long received = 0;
    bool order_ok = true;
    while (received < total)
    {
        auto opt = q.pop();
        if (!opt)
        {
            std::this_thread::yield(); // 비어 있음 → 양보 후 재시도
            continue;
        }
        const Msg& m = *opt;
        if (m.producer < 0 || m.producer >= num_producers)
            order_ok = false; // 손상된 데이터
        else if (m.seq != last_seq[m.producer] + 1)
            order_ok = false; // 순번 역전/누락
        else
            last_seq[m.producer] = m.seq;
        ++received;
    }

    for (auto& t : producers)
        t.join();

    const bool count_ok = (received == total);
    bool complete = true;
    for (int p = 0; p < num_producers; ++p)
        if (last_seq[p] != per_producer - 1)
            complete = false;

    const bool pass = count_ok && order_ok && complete;
    std::printf("[%-14s] producers=%2d each=%d total=%lld received=%lld  order=%s complete=%s "
                "=> %s\n",
                name, num_producers, per_producer, total, received, order_ok ? "OK" : "FAIL",
                complete ? "OK" : "FAIL", pass ? "PASS" : "FAIL");
    return pass;
}

int main()
{
    bool ok = true;

    // 넉넉한 용량 — 순수 정확성
    ok &= run_test<MpscQueue<Msg>>("MPSC", 8, 100000, 1024);
    ok &= run_test<MutexQueue<Msg>>("MUTEX", 8, 100000, 1024);

    // 작은 용량 — backpressure 경로를 강하게 태워도 무손실 유지되는지
    ok &= run_test<MpscQueue<Msg>>("MPSC-tightcap", 16, 50000, 64);
    ok &= run_test<MutexQueue<Msg>>("MUTEX-tightcap", 16, 50000, 64);

    std::printf("\n%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
