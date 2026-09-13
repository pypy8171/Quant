// 유휴 대기의 실제 해상도를 잰다 — sleep_for·condvar wait_for가 요청한 시간에 비해 얼마나 늦게 깨는지.
// 단일 스레드 측정 도구. Phase 0 실측(D-071): Windows 기본 타이머 격자(약 15.6ms)가 100us·1ms 대기를 어디까지
// 늘리는지, timeBeginPeriod(1)이 그것을 얼마나 줄이는지 앞뒤로 같은 표를 찍는다.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

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

Stats summarize(std::vector<double>& v)
{
    std::sort(v.begin(), v.end());
    Stats s;
    s.p50_us = v[v.size() / 2];
    s.p99_us = v[v.size() * 99 / 100];
    s.max_us = v.back();
    return s;
}

template <typename F>
Stats measure(int iters, F&& one_wait)
{
    std::vector<double> got;
    got.reserve(static_cast<size_t>(iters));

    for (int i = 0; i < iters; ++i)
    {
        const auto t0 = steady_clock::now();
        one_wait();
        got.push_back(duration<double, std::micro>(steady_clock::now() - t0).count());
    }

    return summarize(got);
}

void print_row(const char* name, double asked_us, const Stats& s)
{
    std::printf("  %-22s ask=%8.0fus  p50=%9.1fus  p99=%9.1fus  max=%9.1fus\n", name, asked_us, s.p50_us, s.p99_us,
                s.max_us);
}

void run_table(const char* title, int iters)
{
    std::printf("[%s] %d회\n", title, iters);
    print_row("sleep_for(100us)", 100.0, measure(iters, [] { std::this_thread::sleep_for(microseconds(100)); }));
    print_row("sleep_for(1ms)", 1000.0, measure(iters, [] { std::this_thread::sleep_for(milliseconds(1)); }));

    std::mutex m;
    std::condition_variable cv;
    print_row("cv.wait_for(1ms)", 1000.0, measure(iters, [&]
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, milliseconds(1));
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
