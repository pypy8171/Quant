#include "ipc/SharedWriteLock.h"

#include <thread>

namespace ipc
{

// 등록은 기동·재스캔·처음 보는 이름에서만 일어나 경합이 거의 없다 — 그래서 돌다가 양보하는 것으로 족하다.
SharedWriteLock::SharedWriteLock(std::atomic<uint32_t>& flag) : flag_(flag)
{
    uint32_t expected = 0;

    while (!flag_.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
    {
        expected = 0;
        std::this_thread::yield();
    }
}

SharedWriteLock::~SharedWriteLock()
{
    flag_.store(0, std::memory_order_release);
}

} // namespace ipc
