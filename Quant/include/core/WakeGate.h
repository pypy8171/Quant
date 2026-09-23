// 큐 소비자 스레드를 "생산자가 깨우는" 대기 조각. 소비자 하나·생산자 여럿. fill_thread(D-056) 등에
// 흩어져 있던 sleeping 깃발+fence+condvar 패턴을 한 곳에 둔다. Logger writer(D-045)는 아직 같은 패턴을 따로 갖는다. [why D-071]
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace wake
{

// 왜 sleep_for 폴링이 아닌가: Windows 기본 타이머 격자에서 sleep_for(100us)·(1ms)가 실측 p50 15.6ms,
//  timeBeginPeriod(1)로도 2ms다(bench_sleep_res). notify로 깨우면 격자와 무관하게 수십 us 안에 돈다.
class WakeGate
{
public:
    // 생산자: 큐에 push한 뒤 부른다. 소비자가 자고 있을 때만 락을 잡고 notify — 바쁠 때는 원자 읽기 하나뿐이다.
    // [lock-order] push(release) → seq_cst fence → sleeping 읽기. 소비자는 sleeping 쓰기 → fence → 큐 확인.
    //  양쪽 다 store-fence-load라 둘 중 하나는 상대 store를 본다. notify는 락 안에서 한다 — 소비자가 "큐 비었다"
    //  확인과 wait 사이에 있을 때 락 없이 notify하면 그 신호가 새고 상한(capture)까지 잔다.
    void notify();

    // 소비자: 큐가 비었을 때 부른다. still_idle()이 true인 동안만 잔다(재확인으로 유실 방지). capture은 종료·주기 작업의
    //  상한이지 깨우는 수단이 아니다. 잔 뒤 돌아오면 호출자가 큐를 다시 본다.
    template <typename Representation, typename Period, typename Predicate>
    void wait_for(std::chrono::duration<Representation, Period> capture, Predicate still_idle)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (still_idle())
        {
            condition_variable_.wait_for(lock, capture);
        }

        sleeping_.store(false, std::memory_order_relaxed);
    }

    // 만기 시각이 있는 소비자용(주문 재시도). deadline이 지났으면 바로 돌아온다.
    template <typename Clock, typename Duration, typename Predicate>
    void wait_until(std::chrono::time_point<Clock, Duration> deadline, Predicate still_idle)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (still_idle())
        {
            condition_variable_.wait_until(lock, deadline);
        }

        sleeping_.store(false, std::memory_order_relaxed);
    }

    // jthread 소비자용 — 정지 요청(request_stop)이 오면 capture 전에 깬다. still_idle에 정지 깃발을 넣을 필요가 없다.
    //  condition_variable_any의 stop_token 오버로드는 정지 콜백으로 notify를 걸어 준다 [why D-070].
    template <typename Representation, typename Period, typename Predicate>
    void wait_for(std::chrono::duration<Representation, Period> capture, std::stop_token stop_token, Predicate still_idle)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (still_idle())
        {
            condition_variable_.wait_for(lock, stop_token, capture, [&] { return !still_idle(); });
        }

        sleeping_.store(false, std::memory_order_relaxed);
    }

    template <typename Clock, typename Duration, typename Predicate>
    void wait_until(std::chrono::time_point<Clock, Duration> deadline, std::stop_token stop_token, Predicate still_idle)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (still_idle())
        {
            condition_variable_.wait_until(lock, stop_token, deadline, [&] { return !still_idle(); });
        }

        sleeping_.store(false, std::memory_order_relaxed);
    }

    bool sleeping() const
    {
        return sleeping_.load(std::memory_order_relaxed);
    }

private:
    std::mutex                  mutex_;
    std::condition_variable_any condition_variable_; // stop_token 오버로드는 _any에만 있다
    std::atomic<bool>           sleeping_{false};
};

// 정지 요청이 오면 바로 깨는 sleep. 다 잤으면 true, 정지 요청으로 깼으면 false.
//  "잘게 끊어 자면서 깃발을 본다"(100ms×N)와 "정지가 sleep 만기까지 기다린다"(제어 5초·WS 백오프 30초)를 둘 다 대신한다.
template <typename Representation, typename Period>
bool sleep_unless_stopped(std::stop_token stop_token, std::chrono::duration<Representation, Period> duration)
{
    std::mutex                   mutex;
    std::condition_variable_any  condition_variable;
    std::unique_lock<std::mutex> lock(mutex);
    return !condition_variable.wait_for(lock, stop_token, duration, [&] { return stop_token.stop_requested(); });
}

} // namespace wake
