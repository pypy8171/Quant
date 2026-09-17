// WakeGate(mutex+condvar_any) vs 순번 카운터 atomic::wait — 소비자 깨우기 지연과 생산자 notify 비용을 같은 틀로 잰다.
//  D-070 7단계의 판단 근거. 둘 다 Windows에서는 WaitOnAddress/조건변수 커널 깨우기라 큰 차이를 기대하지 않았고,
//  atomic::wait에는 시간 제한이 없어 capture·deadline이 필요한 호출부 3곳에는 그대로 못 쓴다. 숫자는 가이드 17절 표.
#include "core/RingBuffer.h"
#include "core/WakeGate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// 가이드 5절의 순번 카운터 꼴. seen을 잡은 뒤 큐를 다시 보고, 그동안 seq_가 안 바뀌었으면 잔다.
struct AtomicGate
{
    std::atomic<uint32_t> seq_{0};

    void notify()
    {
        seq_.fetch_add(1, std::memory_order_release);
        seq_.notify_one();
    }

    uint32_t snapshot() const
    {
        return seq_.load(std::memory_order_acquire);
    }

    void wait(uint32_t seen)
    {
        seq_.wait(seen, std::memory_order_acquire);
    }
};

// test_wake_gate 4번과 같은 왕복 — 소비자가 잠든 것을 확인하고 push+notify, 깨어나 pop한 시각까지를 잰다.
template <class Gate, class WaitFn, class SleepFn>
void run(const char* name, Gate& gate, WaitFn do_wait, SleepFn is_sleeping)
{
    RingBuffer<int>        queue{64};
    std::atomic<bool>      stop{false};
    std::atomic<int>       consumed{0};
    constexpr int          kRounds = 2000;
    std::vector<double>    wake_us;
    wake_us.reserve(kRounds);
    std::atomic<long long> pushed_at_ns{0};

    std::thread consumer([&]
    {
        while (!stop.load(std::memory_order_acquire))
        {
            if (auto value = queue.pop())
            {
                const auto now = Clock::now().time_since_epoch().count();
                wake_us.push_back(static_cast<double>(now - pushed_at_ns.load(std::memory_order_acquire)) / 1000.0);
                consumed.fetch_add(1, std::memory_order_release);
                continue;
            }

            do_wait(queue, stop);
        }
    });

    std::vector<double> notify_ns;
    notify_ns.reserve(kRounds);
    const auto start_time = Clock::now();

    for (int round_index = 0; round_index < kRounds; ++round_index)
    {
        while (!is_sleeping())
        {
            std::this_thread::yield();
        }

        // sleeping이 켜진 직후는 아직 재확인 단계일 수 있어 조금 더 기다려 wait 안으로 들어가게 한다.
        for (const auto spin0 = Clock::now(); Clock::now() - spin0 < 50us;)
        {
            std::this_thread::yield();
        }

        pushed_at_ns.store(Clock::now().time_since_epoch().count(), std::memory_order_release);

        if (!queue.push(round_index))
        {
            std::cerr << name << " push 실패(round " << round_index << ")\n";
            return;
        }

        const auto n0 = Clock::now();
        gate.notify();
        notify_ns.push_back(static_cast<double>((Clock::now() - n0).count()));

        while (consumed.load(std::memory_order_acquire) <= round_index)
        {
            std::this_thread::yield();
        }
    }

    const auto total = Clock::now() - start_time;
    stop.store(true, std::memory_order_release);
    gate.notify();
    consumer.join();

    std::sort(wake_us.begin(), wake_us.end());
    std::sort(notify_ns.begin(), notify_ns.end());
    std::cout << name << " wake us p50=" << wake_us[wake_us.size() / 2] << " p99=" << wake_us[wake_us.size() * 99 / 100]
              << " max=" << wake_us.back() << " | notify ns p50=" << notify_ns[notify_ns.size() / 2]
              << " p99=" << notify_ns[notify_ns.size() * 99 / 100]
              << " | total_ms=" << std::chrono::duration<double, std::milli>(total).count() << "\n";
}

int main()
{
    for (int rep = 0; rep < 3; ++rep)
    {
        {
            sync::WakeGate gate;
            run("condvar ", gate, [&](RingBuffer<int>& queue, std::atomic<bool>& stop)
            {
                gate.wait_for(1s, [&] { return queue.empty() && !stop.load(std::memory_order_acquire); });
            }, [&] { return gate.sleeping(); });
        }

        {
            AtomicGate        atomic_gate;
            std::atomic<bool> sleeping{false};
            run("atomic  ", atomic_gate, [&](RingBuffer<int>& queue, std::atomic<bool>& stop)
            {
                const uint32_t seen = atomic_gate.snapshot();
                sleeping.store(true, std::memory_order_seq_cst);

                if (queue.empty() && !stop.load(std::memory_order_acquire))
                {
                    atomic_gate.wait(seen);
                }

                sleeping.store(false, std::memory_order_relaxed);
            }, [&] { return sleeping.load(std::memory_order_acquire); });
        }
    }

    return 0;
}
