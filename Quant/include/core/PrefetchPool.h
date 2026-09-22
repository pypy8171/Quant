#pragma once
#include "core/WakeGate.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// prefetch::Pool  —  무거운 REST를 미리 당기는 작업을 고정 스레드가 나눠 맡는다
//   전략마다 스레드를 하나 띄우면 종목 수가 곧 스레드 수가 된다. 41종목에서 이미
//   프로세스 스레드가 39개였고, 2,700종목이면 2,700개다. 그 상태의 perf(09-22 장중)는
//   finish_task_switch 16%·do_sched_yield 12% 대 Engine::shard_thread_fn 8% — 문맥 교환이
//   엔진 본체보다 세 배 비쌌다. 스레드 수를 코어·부하로 정하고 작업만 늘린다. [why D-071]
//
//   쓰는 쪽은 주기마다 한 번 불릴 함수 하나를 add()로 맡기고, 자기가 사라지기 전에
//   remove()로 뗀다. remove()는 그 작업이 실행 중이면 끝날 때까지 기다리고 돌아오므로,
//   돌아온 뒤에는 작업이 붙잡은 객체를 지워도 된다.
//   [lock-order] registry_mutex_ → Slot::mutex. 둘을 겹쳐 잡지 않는다(작업 실행 중에는
//    registry_mutex_를 놓는다 — 한 작업의 REST가 등록·해제를 막지 않게).
// ─────────────────────────────────────────────────────────────────────────────

namespace prefetch
{

class Pool
{
public:
    using Work   = std::function<void()>;
    using TaskId = uint64_t; // 0은 "없음"

    // 코어 수로 정하는 기본값. 작업이 REST 대기(블로킹 HTTP)라 코어를 다 쓸 이유는 없고,
    //  KIS 초당 한도가 진짜 상한이라 스레드를 늘려도 호출이 줄서기만 한다. 2~8로 접는다.
    static std::size_t recommended_thread_count()
    {
        const unsigned int cores = std::thread::hardware_concurrency();
        const std::size_t  half  = cores == 0 ? 2u : static_cast<std::size_t>(cores) / 2u;
        return half < 2u ? 2u : (half > 8u ? 8u : half);
    }

    Pool(std::size_t thread_count, std::chrono::milliseconds period)
        : thread_count_(thread_count < 1u ? 1u : thread_count)
        , period_(period)
    {
    }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

    ~Pool() { stop(); }

    // 작업 등록. 첫 등록에서 스레드가 뜬다(테스트·FEED 모드처럼 전략이 없으면 스레드도 없다).
    TaskId add(Work work)
    {
        TaskId id = 0;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            id = ++next_id_;
            slots_.push_back(std::make_shared<Slot>(id, std::move(work)));
        }

        start_threads();
        return id;
    }

    // 작업 해제. 돌아온 뒤에는 그 작업이 다시 불리지 않고, 실행 중이던 호출도 끝나 있다.
    void remove(TaskId id)
    {
        if (id == 0)
        {
            return;
        }

        std::shared_ptr<Slot> slot;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);

            for (auto iterator = slots_.begin(); iterator != slots_.end(); ++iterator)
            {
                if ((*iterator)->id == id)
                {
                    slot = *iterator;
                    slots_.erase(iterator);
                    break;
                }
            }
        }

        if (!slot)
        {
            return;
        }

        // 스레드가 이 슬롯을 잡고 실행 중이면 여기서 기다린다. 목록에서 이미 뗐으니
        //  다음 주기에는 고르지 않고, 앞 주기의 사본을 든 스레드는 removed를 보고 건너뛴다.
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->removed = true;
    }

    void stop()
    {
        std::vector<std::jthread> threads;
        {
            std::lock_guard<std::mutex> lock(threads_mutex_);
            stopped_ = true;
            threads.swap(threads_);
        }

        for (auto& thread : threads)
        {
            thread.request_stop();
        }

        // jthread 소멸자가 join한다 — threads가 스코프를 벗어날 때 전부 합류.
    }

    std::size_t task_count() const
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        return slots_.size();
    }

    std::size_t thread_count() const
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        return threads_.size();
    }

private:
    struct Slot
    {
        Slot(TaskId identifier, Work function)
            : id(identifier)
            , work(std::move(function))
        {
        }

        TaskId     id;
        Work       work;
        std::mutex mutex;         // 이 작업의 동시 실행·해제 대기를 막는다
        bool       removed = false;
    };

    void start_threads()
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);

        if (stopped_ || !threads_.empty())
        {
            return;
        }

        for (std::size_t index = 0; index < thread_count_; ++index)
        {
            threads_.emplace_back([this, index](std::stop_token stop_token) { worker_loop(stop_token, index); });
        }
    }

    // 작업은 id로 스레드에 붙는다(id % 스레드 수). id는 안 바뀌므로 등록·해제로 목록 자리가
    //  밀려도 같은 작업이 두 스레드에서 같은 주기에 겹쳐 돌지 않는다.
    void worker_loop(std::stop_token stop_token, std::size_t worker_index)
    {
        std::vector<std::shared_ptr<Slot>> mine;

        while (!stop_token.stop_requested())
        {
            mine.clear();
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);

                for (const auto& slot : slots_)
                {
                    if (slot->id % thread_count_ == worker_index)
                    {
                        mine.push_back(slot);
                    }
                }
            }

            for (const auto& slot : mine)
            {
                if (stop_token.stop_requested())
                {
                    break;
                }

                std::lock_guard<std::mutex> lock(slot->mutex);

                if (!slot->removed)
                {
                    slot->work();
                }
            }

            if (!wake::sleep_unless_stopped(stop_token, period_))
            {
                break;
            }
        }
    }

    const std::size_t               thread_count_;
    const std::chrono::milliseconds period_;

    mutable std::mutex                 registry_mutex_;
    std::vector<std::shared_ptr<Slot>> slots_;
    TaskId                             next_id_ = 0;

    mutable std::mutex        threads_mutex_;
    std::vector<std::jthread> threads_;
    bool                      stopped_ = false;
};

} // namespace prefetch
