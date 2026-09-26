#pragma once
#include "core/WakeGate.h"
#include "utils/ThreadName.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// prefetch::Pool  —  무거운 REST를 미리 당기는 작업을 고정 스레드가 나눠 맡는다
//   전략마다 스레드를 하나 띄우면 종목 수가 곧 스레드 수가 된다. 41종목에서 이미
//   프로세스 스레드가 39개였고, 2,700종목이면 2,700개다. 그 상태의 perf(09-22 장중)는
//   finish_task_switch 16%·do_sched_yield 12% 대 Engine::shard_thread_fn 8% — 문맥 교환이
//   엔진 본체보다 세 배 비쌌다. 스레드 수를 코어·부하로 정하고 작업만 늘린다. [why D-115]
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

    // 코어 수 / 4를 기본값으로 2~8에 접는다(16코어 = 4개). 필요한 수는 (작업 수 x 호출 지연 / 주기)로
    //  나오는데 지금 부하는 41종목 x 3초라 그 값이 1이 안 된다 — 4개면 네 배 여유다. 상한 8은 측정에서
    //  온 자리다(bench_prefetch_pool, 2026-09-22: 계산형 200us x 2700개가 8스레드에서 포화. 그 위로는
    //  처리량이 안 늘고 스레드만 는다). 대기형은 공식이 수백을 요구하지만 그때 늘릴 것은 스레드가 아니라
    //  입력이다 — 왕복 지연을 없애거나(WS 푸시) 작업 수를 줄인다(배치 조회). [why D-115]
    static std::size_t recommended_thread_count()
    {
        const unsigned int cores    = std::thread::hardware_concurrency();
        const std::size_t  quartered = cores == 0 ? 2u : static_cast<std::size_t>(cores) / 4u;
        return quartered < 2u ? 2u : (quartered > 8u ? 8u : quartered);
    }

    // 스레드를 미리 띄운다. Engine이 기동에서 한 번 부르면 장중에 전략이 붙어도 스레드를 새로 만들지 않는다.
    //  여러 번 불러도 안전하다(이미 떠 있으면 아무것도 안 한다).
    void start()
    {
        start_threads();
    }

    Pool(std::size_t thread_count, std::chrono::milliseconds period)
        : thread_count_(thread_count < 1u ? 1u : thread_count)
        , period_(period)
    {
    }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

    ~Pool()
    {
        stop();
    }

    // 작업 등록. 첫 등록에서 스레드가 뜬다(테스트처럼 전략이 없으면 스레드도 없다).
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

    void start_threads();

    // 작업은 id로 스레드에 붙는다(id % 스레드 수). id는 안 바뀌므로 등록·해제로 목록 자리가
    //  밀려도 같은 작업이 두 스레드에서 같은 주기에 겹쳐 돌지 않는다.
    void worker_loop(std::stop_token stop_token, std::size_t worker_index)
    {
        // 이름이 없으면 리눅스는 만든 쪽 comm(quant_trader)을 물려받아 그라파나 "스레드별 CPU" 범례에서
        //  엔진 스레드와 구분되지 않는다. 샤드와 같은 "이름 번호" 꼴로 붙인다. [why D-115]
        thread_name::set_current("Prefetch " + std::to_string(worker_index));

        std::vector<std::shared_ptr<Slot>> mine;

        while (!stop_token.stop_requested())
        {
            const auto cycle_start = std::chrono::steady_clock::now();
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

            // 주기는 '쉬는 시간'이 아니라 '도는 간격'이다 — 한 바퀴에 쓴 시간을 빼고 잔다.
            //  빼지 않으면 달성 간격이 주기 + 배치 시간으로 밀린다(2,700개 계산형에서 1.0s 주기가 1.5s로).
            //  이미 주기를 넘겨 썼으면 쉬지 않고 다음 바퀴로 간다. [why D-115]
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - cycle_start);

            if (spent >= period_)
            {
                continue;
            }

            if (!wake::sleep_unless_stopped(stop_token, period_ - spent))
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
