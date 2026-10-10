#pragma once
#include "core/MpscQueue.h"
#include "core/WakeGate.h"
#include "utils/ThreadGuard.h"
#include "utils/ThreadName.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// order_transport::Pool  —  주문 스레드가 연 주문을 KIS로 보내고 답을 기다리는 고정 스레드
//   주문 전송은 동기 HTTP라 보내는 스레드가 답이 올 때까지 선다. 주문 스레드가 직접 보내면 한 건의
//   왕복(09-14~18 p50 약 1.5초, p90 약 4.5초)마다 뒤 주문이 전부 멈춘다 — 09-14 청산 41건이 줄을 섰다.
//   주문 스레드는 판정·선점·장부까지만 하고, 보내기는 여기 넘긴 뒤 다음 주문으로 간다. [why D-151]
//
//   흐름: 주문 스레드 dispatch → 작업 큐(잠금 + 조건변수) → 전송 스레드가 send(job) → 완료 큐(MPSC) →
//    done_wake.notify() → 주문 스레드 take_done. 게이트·장부·이력은 주문 스레드만 만진다 — 여기는 job만 들고 다닌다.
//   [inv] dispatch·take_done·in_flight·full은 주문 스레드(소비자 하나)만 부른다. 보내는 중인 수는 스레드 수를
//    넘지 않게 호출자가 full()로 막는다 — 그래서 완료 큐는 넘치지 않는다.
// ─────────────────────────────────────────────────────────────────────────────

namespace order_transport
{

template <typename Job> class Pool
{
public:
    using Send = std::function<void(Job&)>;

    // done_wake는 주문 스레드가 잠드는 문이다 — 답이 오면 그 문을 두드린다. 풀보다 오래 살아야 한다.
    Pool(std::size_t thread_count, Send send, wake::WakeGate& done_wake)
        : thread_count_(thread_count < 1u ? 1u : thread_count)
        , send_(std::move(send))
        , done_wake_(done_wake)
        , done_(thread_count_ * 2u)
    {
        threads_.reserve(thread_count_);

        for (std::size_t worker_index = 0; worker_index < thread_count_; ++worker_index)
        {
            threads_.emplace_back([this, worker_index](std::stop_token stop_token)
            {
                worker_loop(stop_token, worker_index);
            });
        }
    }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

    // 정지 — 작업 큐에 남은 것은 버린다. 보내는 중인 것은 끝까지 보내고 합류한다. 호출자는 그 전에
    //  in_flight()가 0이 될 때까지 완료를 거둬 둔다(버려진 작업은 INTENT만 적힌 채 남는다).
    ~Pool()
    {
        for (auto& thread : threads_)
        {
            thread.request_stop();
        }

        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            jobs_.clear();
        }

        jobs_ready_.notify_all();
        // jthread 소멸자가 join한다.
    }

    // 주문 스레드: 보낼 것을 넘긴다. full()이면 부르지 않는다.
    void dispatch(Job&& job)
    {
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            jobs_.push_back(std::move(job));
        }

        ++in_flight_;
        jobs_ready_.notify_one();
    }

    // 주문 스레드: 끝난 것 하나. 없으면 비어 있다.
    [[nodiscard]] std::optional<Job> take_done()
    {
        std::optional<Job> job = done_.pop();

        if (job)
        {
            --in_flight_;
        }

        return job;
    }

    // 대기 조건에 넣는다 — 끝난 것이 쌓여 있으면 잠들지 않는다. 아무 스레드에서 불러도 된다.
    [[nodiscard]] bool done_empty() const noexcept
    {
        return done_.empty();
    }

    [[nodiscard]] std::size_t in_flight() const noexcept
    {
        return in_flight_;
    }

    [[nodiscard]] bool full() const noexcept
    {
        return in_flight_ >= thread_count_;
    }

    [[nodiscard]] std::size_t thread_count() const noexcept
    {
        return thread_count_;
    }

private:
    void worker_loop(std::stop_token stop_token, std::size_t worker_index)
    {
        // 이름이 없으면 그라파나 "스레드별 CPU" 범례에서 엔진 스레드와 구분되지 않는다. [why D-115]
        thread_name::set_current("OrderSend " + std::to_string(worker_index));

        while (!stop_token.stop_requested())
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(jobs_mutex_);

                if (!jobs_ready_.wait(lock, stop_token, [this]
                    {
                        return !jobs_.empty();
                    }))
                {
                    return;
                }

                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            // 보내기가 던져도 job은 완료 큐로 돌려보낸다 — 돌려보내지 않으면 주문 스레드가 그 주문을 닫지 못하고
            //  in_flight가 줄지 않아 full()이 풀리지 않는다. job은 send_가 채운 데까지만 채워진 채 간다.
            //  지금 유일한 send_(OrderRouter::send_new·send_modify)는 noexcept라 여기까지 오는 예외는 없다.
            thread_guard::run_and_log("OrderSend 전송",
                                      [this, &job]
                                      {
                                          send_(job);
                                      });

            // 완료 큐는 보내는 중인 수(스레드 수 이하)의 두 배 칸이라 차지 않는다. 그래도 차면 비울 때까지 양보한다 —
            //  답을 버리면 주문 스레드가 그 주문을 영영 닫지 못한다.
            while (!done_.push(std::move(job)))
            {
                std::this_thread::yield();
            }

            done_wake_.notify();
        }
    }

    const std::size_t thread_count_;
    const Send        send_;
    wake::WakeGate&   done_wake_;

    std::mutex                  jobs_mutex_;
    std::condition_variable_any jobs_ready_;
    std::deque<Job>             jobs_;

    MpscQueue<Job> done_;
    std::size_t    in_flight_ = 0; // [inv] 주문 스레드만 읽고 쓴다

    std::vector<std::jthread> threads_; // 마지막 멤버 — 소멸 때 먼저 합류해 위 멤버를 쓰는 스레드가 남지 않는다
};

} // namespace order_transport
