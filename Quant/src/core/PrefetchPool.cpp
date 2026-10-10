#include "core/PrefetchPool.h"
#include "utils/ThreadGuard.h"
#include "utils/ThreadName.h"

namespace prefetch
{

void Pool::start_threads()
{
    std::lock_guard<std::mutex> lock(threads_mutex_);

    if (stopped_ || !threads_.empty())
    {
        return;
    }

    for (std::size_t index = 0; index < thread_count_; ++index)
    {
        threads_.emplace_back([this, index](std::stop_token stop_token)
        {
            worker_loop(stop_token, index);
        });
    }
}

// 작업은 id로 스레드에 붙는다(id % 스레드 수). id는 안 바뀌므로 등록·해제로 목록 자리가
//  밀려도 같은 작업이 두 스레드에서 같은 주기에 겹쳐 돌지 않는다.
void Pool::worker_loop(std::stop_token stop_token, std::size_t worker_index)
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
                // 작업 하나가 던져도 그 작업만 이번 주기를 건너뛰고 같은 스레드의 다른 작업과 다음 주기는 그대로 돈다.
                //  스레드를 끝내면 이 스레드에 붙은 작업(id % 스레드 수) 전부가 조용히 멈춘다.
                thread_guard::run_and_log("Prefetch 작업", slot->work);
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

} // namespace prefetch
