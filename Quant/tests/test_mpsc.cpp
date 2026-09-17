// tests/test_mpsc.cpp
// MpscQueue / MutexQueue 정확성 검증 (D1)
//
// 검증 항목:
//   ① 무손실   : N 생산자가 각 M건 push → 소비자가 정확히 N*M건 pop (유실/중복 0)
//   ② 순번 보존 : 각 생산자의 sequence가 소비자에서 0,1,2,... 순서로 도착 (역전/누락 0)
//   ③ backpressure: 작은 용량에서 push가 false를 반환해도 재시도로 무손실 유지
//
//   ②가 성립하는 이유: 단일 생산자는 순서대로 push하고, Vyukov 큐는 티켓(enqueue_position)
//   순으로 소비되므로 "생산자별" FIFO가 보존된다. (전역 FIFO는 보장하지 않음)
//
// 사용법: test_mpsc

#include "core/MpscQueue.h"
#include "core/MutexQueue.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

struct Message
{
    int producer;
    int sequence;
};

template <typename Queue>
bool run_test(const char* name, int number_producers, int per_producer, size_t capture)
{
    Queue queue(capture);
    const long long total = static_cast<long long>(number_producers) * per_producer;

    // ── 생산자 N개: 각자 (producer_id, 0..M-1) 를 순서대로 push (가득 차면 재시도)
    std::vector<std::thread> producers;
    producers.reserve(number_producers);

    for (int producer_index = 0; producer_index < number_producers; ++producer_index)
    {
        producers.emplace_back(
            [&queue, producer_index, per_producer]
            {
                for (int per_producer_index = 0; per_producer_index < per_producer; ++per_producer_index)
                {
                    Message message{producer_index, per_producer_index};

                    while (!queue.push(message))
                    {
                        std::this_thread::yield(); // backpressure: 가득 참 → 양보 후 재시도
                    }
                }
            });
    }

    // ── 소비자 1개: 총 N*M건을 받을 때까지 pop, 생산자별 순번 검증
    std::vector<int> last_sequence(number_producers, -1);
    long long received = 0;
    bool order_ok = true;

    while (received < total)
    {
        auto option = queue.pop();

        if (!option)
        {
            std::this_thread::yield(); // 비어 있음 → 양보 후 재시도
            continue;
        }

        const Message& message = *option;

        if (message.producer < 0 || message.producer >= number_producers)
        {
            order_ok = false; // 손상된 데이터
        }
        else if (message.sequence != last_sequence[message.producer] + 1)
        {
            order_ok = false; // 순번 역전/누락
        }
        else
        {
            last_sequence[message.producer] = message.sequence;
        }

        ++received;
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    const bool count_ok = (received == total);
    bool complete = true;

    for (int producer_index = 0; producer_index < number_producers; ++producer_index)
    {
        if (last_sequence[producer_index] != per_producer - 1)
        {
            complete = false;
        }
    }

    const bool pass = count_ok && order_ok && complete;
    std::printf("[%-14s] producers=%2d each=%d total=%lld received=%lld  order=%s complete=%s "
                "=> %s\n",
                name, number_producers, per_producer, total, received, order_ok ? "OK" : "FAIL",
                complete ? "OK" : "FAIL", pass ? "PASS" : "FAIL");
    return pass;
}

int main()
{
    bool ok = true;

    // 넉넉한 용량 — 순수 정확성
    ok &= run_test<MpscQueue<Message>>("MPSC", 8, 100000, 1024);
    ok &= run_test<MutexQueue<Message>>("MUTEX", 8, 100000, 1024);

    // 작은 용량 — backpressure 경로를 강하게 태워도 무손실 유지되는지
    ok &= run_test<MpscQueue<Message>>("MPSC-tightcap", 16, 50000, 64);
    ok &= run_test<MutexQueue<Message>>("MUTEX-tightcap", 16, 50000, 64);

    std::printf("\n%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
