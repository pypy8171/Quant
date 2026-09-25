// 유휴 대기의 실제 해상도를 잰다 — sleep_for·condvar wait_for가 요청한 시간에 비해 얼마나 늦게 깨는지.
// 단일 스레드 측정 도구. Phase 0 실측(D-071): Windows 기본 타이머 격자(약 15.6ms)가 100us·1ms 대기를 어디까지
// 늘리는지, timeBeginPeriod(1)이 그것을 얼마나 줄이는지 앞뒤로 같은 표를 찍는다.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "core/WakeGate.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

using namespace std::chrono;

namespace
{

struct Stats
{
    double p50_us{};
    double p99_us{};
    double max_us{};
};

Stats summarize(std::vector<double>& values)
{
    std::sort(values.begin(), values.end());
    Stats statistics;
    statistics.p50_us = values[values.size() / 2];
    statistics.p99_us = values[values.size() * 99 / 100];
    statistics.max_us = values.back();
    return statistics;
}

template <typename F>
Stats measure(int iters, F&& one_wait)
{
    std::vector<double> received;
    received.reserve(static_cast<size_t>(iters));

    for (int index = 0; index < iters; ++index)
    {
        const auto start_time = steady_clock::now();
        one_wait();
        received.push_back(duration<double, std::micro>(steady_clock::now() - start_time).count());
    }

    return summarize(received);
}

void print_row(const char* name, double asked_us, const Stats& statistics)
{
    std::printf("  %-22s ask=%8.0fus  p50=%9.1fus  p99=%9.1fus  max=%9.1fus\n", name, asked_us, statistics.p50_us, statistics.p99_us,
                statistics.max_us);
}

void run_table(const char* title, int iters)
{
    std::printf("[%s] %d회\n", title, iters);
    print_row("sleep_for(100us)", 100.0, measure(iters, [] { std::this_thread::sleep_for(microseconds(100)); }));
    print_row("sleep_for(1ms)", 1000.0, measure(iters, [] { std::this_thread::sleep_for(milliseconds(1)); }));

    std::mutex mutex;
    std::condition_variable condition_variable;
    print_row("cv.wait_for(1ms)", 1000.0, measure(iters, [&]
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition_variable.wait_for(lock, milliseconds(1));
    }));
    // 격자를 안 타는 길(Windows 고해상도 대기 타이머 / POSIX nanosleep). 시세 줄 스레드의 유휴 잠이
    //  이것을 쓴다 — 위 두 줄과 같은 500us 를 부탁해 얼마나 자는지 나란히 본다. [why D-137]
    std::stop_source stop_source;
    print_row("precise(500us)", 500.0, measure(iters, [&]
    {
        wake::sleep_precise_unless_stopped(stop_source.get_token(), microseconds(500));
    }));
    print_row("cv.wait_for(500us)", 500.0, measure(iters, [&]
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition_variable.wait_for(lock, microseconds(500));
    }));
    print_row("yield()", 0.0, measure(iters, [] { std::this_thread::yield(); }));
}

} // namespace

int main(int argc, char** argv)
{
    const int iters = argc > 1 ? std::atoi(argv[1]) : 1000;
    run_table("기본 타이머", iters);

#ifdef _WIN32
    // Windows 10 2004+에서는 호출 프로세스에만 적용된다. 종료 시 되돌린다.
    if (timeBeginPeriod(1) == TIMERR_NOERROR)
    {
        run_table("timeBeginPeriod(1)", iters);
        timeEndPeriod(1);
    }
    else
    {
        std::printf("timeBeginPeriod(1) 실패 — 두 번째 표 생략\n");
    }
#else
    std::printf("(POSIX: 타이머 해상도 변경 없음, 표 하나만)\n");
#endif

    return 0;
}
