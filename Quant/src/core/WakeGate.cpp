#include "core/WakeGate.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#endif

namespace wake
{
#ifdef _WIN32
namespace
{
// 스레드마다 대기 타이머 하나를 만들어 두고 다시 쓴다. 만드는 것 자체가 커널 객체 할당이라 바퀴마다 하면
//  아끼려던 시간을 도로 쓴다. 스레드가 끝날 때 닫는다.
class ThreadTimer
{
public:
    ThreadTimer()
    {
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 은 Windows 10 1803부터다. 없는 판에서는 nullptr 이 오고
        //  부르는 쪽이 예전 길로 간다.
        handle_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    }

    ~ThreadTimer()
    {
        if (handle_ != nullptr)
        {
            CloseHandle(handle_);
        }
    }

    ThreadTimer(const ThreadTimer&)            = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;

    HANDLE handle() const
    {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

} // namespace
#endif

bool sleep_precise_unless_stopped(std::stop_token stop_token, std::chrono::nanoseconds duration)
{
    if (stop_token.stop_requested())
    {
        return false;
    }

    if (duration <= std::chrono::nanoseconds::zero())
    {
        return true;
    }

#ifdef _WIN32
    thread_local ThreadTimer timer;

    if (timer.handle() == nullptr)
    {
        return sleep_unless_stopped(stop_token, duration);
    }

    // 만기 시각의 단위는 100ns, 음수는 "지금부터 이만큼 뒤"라는 뜻이다. 0으로 내려가면 만기가 절대 시각
    //  0(1601년)이 되어 곧바로 깨므로 최소 한 칸은 준다.
    LARGE_INTEGER due_time;
    due_time.QuadPart = -(duration.count() / 100);

    if (due_time.QuadPart == 0)
    {
        due_time.QuadPart = -1;
    }

    if (SetWaitableTimer(timer.handle(), &due_time, 0, nullptr, nullptr, FALSE) == 0)
    {
        return sleep_unless_stopped(stop_token, duration);
    }

    WaitForSingleObject(timer.handle(), INFINITE);
#else
    constexpr int64_t kNanosecondsPerSecond = 1'000'000'000;

    timespec request{};
    request.tv_sec  = static_cast<time_t>(duration.count() / kNanosecondsPerSecond);
    request.tv_nsec = static_cast<long>(duration.count() % kNanosecondsPerSecond); // 칸 이름은 POSIX가 정했다 [wire]

    // 신호에 깨면 남은 만큼 마저 잔다 — 짧게 깨어난 채로 돌아가면 부르는 쪽이 헛바퀴를 돈다.
    while (nanosleep(&request, &request) == -1 && errno == EINTR)
    {
    }
#endif

    return !stop_token.stop_requested();
}

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
