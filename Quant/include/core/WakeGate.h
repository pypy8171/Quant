// 큐 소비자 스레드를 "생산자가 깨우는" 대기 조각. 소비자 하나·생산자 여럿. Logger writer(D-045)·fill_thread(D-056)에
// 흩어져 있던 sleeping 깃발+fence+condvar 패턴을 한 곳에 둔다. [why D-071]
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace sync
{

// 왜 sleep_for 폴링이 아닌가: Windows 기본 타이머 격자에서 sleep_for(100us)·(1ms)가 실측 p50 15.6ms,
//  timeBeginPeriod(1)로도 2ms다(bench_sleep_res). notify로 깨우면 격자와 무관하게 수십 us 안에 돈다.
class WakeGate
{
public:
    // 생산자: 큐에 push한 뒤 부른다. 소비자가 자고 있을 때만 락을 잡고 notify — 바쁠 때는 원자 읽기 하나뿐이다.
    // [lock-order] push(release) → seq_cst fence → sleeping 읽기. 소비자는 sleeping 쓰기 → fence → 큐 확인.
    //  양쪽 다 store-fence-load라 둘 중 하나는 상대 store를 본다. notify는 락 안에서 한다 — 소비자가 "큐 비었다"
    //  확인과 wait 사이에 있을 때 락 없이 notify하면 그 신호가 새고 상한(cap)까지 잔다.
    void notify()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (sleeping_.load(std::memory_order_relaxed))
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cv_.notify_one();
        }
    }

    // 소비자: 큐가 비었을 때 부른다. still_idle()이 true인 동안만 잔다(재확인으로 유실 방지). cap은 종료·주기 작업의
    //  상한이지 깨우는 수단이 아니다. 잔 뒤 돌아오면 호출자가 큐를 다시 본다.
    template <typename Rep, typename Period, typename Pred>
    void wait_for(std::chrono::duration<Rep, Period> cap, Pred still_idle)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (still_idle())
        {
            cv_.wait_for(lk, cap);
        }

        sleeping_.store(false, std::memory_order_relaxed);
    }

    // 만기 시각이 있는 소비자용(주문 재시도). deadline이 지났으면 바로 돌아온다.
    template <typename Clock, typename Dur, typename Pred>
    void wait_until(std::chrono::time_point<Clock, Dur> deadline, Pred still_idle)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (still_idle())
        {
            cv_.wait_until(lk, deadline);
        }

        sleeping_.store(false, std::memory_order_relaxed);
    }

    bool sleeping() const
    {
        return sleeping_.load(std::memory_order_relaxed);
    }

private:
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::atomic<bool>       sleeping_{false};
};

} // namespace sync
