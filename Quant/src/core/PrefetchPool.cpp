#include "core/PrefetchPool.h"

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

} // namespace prefetch
