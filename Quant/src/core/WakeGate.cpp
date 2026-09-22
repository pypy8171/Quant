#include "core/WakeGate.h"

namespace wake
{
void WakeGate::notify()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (sleeping_.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        condition_variable_.notify_one();
    }
}

} // namespace wake
