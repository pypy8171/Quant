// WakeGate 단위 테스트 — 생산자 notify가 소비자를 상한(cap) 전에 깨우는지, 신호 유실이 없는지, 만기 시각이 지켜지는지.
// 빌드: cmake --build <dir> --target test_wake_gate
#include "core/RingBuffer.h"
#include "core/WakeGate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

} // namespace

int main()
{
    // 1. still_idle이 false면 자지 않고 바로 돌아온다(큐에 이미 일이 있는 경우).
    {
        sync::WakeGate g;
        const auto t0 = Clock::now();
        g.wait_for(500ms, [] { return false; });
        CHECK(Clock::now() - t0 < 50ms);
        CHECK(!g.sleeping());
    }

    // 2. 지난 만기는 바로 돌아온다.
    {
        sync::WakeGate g;
        const auto t0 = Clock::now();
        g.wait_until(Clock::now() - 1ms, [] { return true; });
        CHECK(Clock::now() - t0 < 50ms);
    }

    // 3. 만기가 있으면 notify 없이도 그 시각에 깬다(주문 재시도 경로).
    {
        sync::WakeGate g;
        const auto t0 = Clock::now();
        g.wait_until(t0 + 30ms, [] { return true; });
        const auto took = Clock::now() - t0;
        CHECK(took >= 30ms);
        CHECK(took < 300ms);
    }

    // 4. 생산자 push+notify가 상한(1s)보다 훨씬 먼저 소비자를 깨운다 — 1,000회 왕복이 신호 유실 없이 끝나야 한다.
    //    유실이 한 번이라도 나면 그 회차가 1s를 다 자므로 총 시간이 튄다.
    {
        sync::WakeGate         g;
        RingBuffer<int>  q{64};
        std::atomic<bool>      stop{false};
        std::atomic<int>       consumed{0};
        constexpr int          kRounds = 1000;
        std::vector<double>    wake_us;
        wake_us.reserve(kRounds);
        std::atomic<long long> pushed_at_ns{0};

        std::thread consumer([&]
        {
            while (!stop.load(std::memory_order_acquire))
            {
                if (auto v = q.pop())
                {
                    const auto now = Clock::now().time_since_epoch().count();
                    wake_us.push_back(static_cast<double>(now - pushed_at_ns.load(std::memory_order_acquire)) / 1000.0);
                    consumed.fetch_add(1, std::memory_order_release);
                    continue;
                }

                g.wait_for(1s, [&] { return q.empty() && !stop.load(std::memory_order_acquire); });
            }
        });

        const auto t0 = Clock::now();

        for (int i = 0; i < kRounds; ++i)
        {
            // 소비자가 정말 잠들 때까지 기다렸다가 넣는다 — 깨우기 경로만 재기 위해서. sleeping이 켜진 직후는
            //  아직 "재확인" 단계일 수 있어 조금 더 기다려 wait 안으로 들어가게 한다.
            while (!g.sleeping())
            {
                std::this_thread::yield();
            }

            for (const auto spin0 = Clock::now(); Clock::now() - spin0 < 50us;)
            {
                std::this_thread::yield();
            }

            pushed_at_ns.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
            CHECK(q.push(i));
            g.notify();

            while (consumed.load(std::memory_order_acquire) <= i)
            {
                std::this_thread::yield();
            }
        }

        const auto total = Clock::now() - t0;
        stop.store(true, std::memory_order_release);
        g.notify();
        consumer.join();

        CHECK(consumed.load() == kRounds);
        // 유실 1회 = 1s. 1,000회가 정상이면 Windows에서도 수백 ms 안이다.
        CHECK(total < 5s);

        std::sort(wake_us.begin(), wake_us.end());
        std::cout << "wake latency us: p50=" << wake_us[wake_us.size() / 2] << " p99=" << wake_us[wake_us.size() * 99 / 100]
                  << " max=" << wake_us.back() << " total_ms=" << std::chrono::duration<double, std::milli>(total).count()
                  << "\n";
        // 타이머 격자(15.6ms)와 무관해야 한다 — p99가 격자 아래.
        CHECK(wake_us[wake_us.size() * 99 / 100] < 15000.0);
    }

    // 5. 생산자 여럿이 동시에 notify해도 소비자는 전부 받는다.
    {
        sync::WakeGate        g;
        RingBuffer<int> q{4096};
        std::atomic<bool>     stop{false};
        std::atomic<int>      consumed{0};
        constexpr int         kProducers = 4;
        constexpr int         kPerProducer = 500;

        std::thread consumer([&]
        {
            while (!stop.load(std::memory_order_acquire) || !q.empty())
            {
                if (q.pop())
                {
                    consumed.fetch_add(1, std::memory_order_release);
                    continue;
                }

                g.wait_for(100ms, [&] { return q.empty() && !stop.load(std::memory_order_acquire); });
            }
        });

        // RingBuffer는 SPSC라 생산자 쪽은 뮤텍스로 직렬화한다 — 여기서 재는 것은 notify의 다중 호출뿐이다.
        std::mutex               push_mtx;
        std::vector<std::thread> producers;

        for (int p = 0; p < kProducers; ++p)
        {
            producers.emplace_back([&]
            {
                for (int i = 0; i < kPerProducer; ++i)
                {
                    {
                        std::lock_guard<std::mutex> lk(push_mtx);

                        while (!q.push(i))
                        {
                            std::this_thread::yield();
                        }
                    }

                    g.notify();
                }
            });
        }

        for (auto& t : producers)
        {
            t.join();
        }

        stop.store(true, std::memory_order_release);
        g.notify();
        consumer.join();
        CHECK(consumed.load() == kProducers * kPerProducer);
    }

    std::cout << "test_wake_gate: " << g_checks << " checks passed\n";
    return 0;
}
