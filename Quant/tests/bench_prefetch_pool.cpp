// prefetch::Pool 부하 하네스 — 작업 N개를 주기 T로 돌릴 때 스레드 몇 개가 필요한가. [why D-115]
//  브로커 한도는 여기서 재지 않는다. 데이터를 당겨오는 자리는 길이를 정할 수 있는 가짜 작업으로 대신한다.
//  작업 성격 두 가지를 나눠 돌린다 — 대기형(네트워크 왕복처럼 자고 있음)과 계산형(코어를 씀).
//  빌드: cmake --build <directory> --target bench_prefetch_pool   (ctest 밖, 부하 하네스 관례)
//  결과 기록: docs/reports/stresstest/
#include "core/PrefetchPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

enum class WorkKind
{
    Wait, // 네트워크 왕복 흉내 — 스레드가 잠든다
    Spin, // 계산 흉내 — 코어를 붙잡는다
};

struct Options
{
    std::size_t tasks      = 2700;
    std::size_t threads    = 8;
    int         period_ms  = 1000;
    int         work_us    = 200;
    int         seconds    = 20;
    WorkKind    work_kind  = WorkKind::Wait;
    std::string out_path;
    std::string tag = "-"; // 같은 파일에 쌓인 행이 어느 코드에서 나왔는지 표시
    int         victim_us = 1000; // 지연에 민감한 이웃 스레드가 깨어나려는 간격(0이면 안 띄운다)
};

// 한 작업이 돌 때마다 직전 실행과의 간격을 남긴다. 표본은 미리 잡아 두고 덮어쓴다
//  — 측정 스레드가 힙을 만지면 재려는 값이 흔들린다. [inv] 한 칸은 한 작업만 쓴다
struct TaskProbe
{
    Clock::time_point     last;
    std::vector<double>   intervals_ms;
    std::size_t           written = 0;
    std::atomic<uint32_t> runs{0};
};

// 대기형 흉내 — 실제 REST 호출은 소켓에서 잠들지 Sleep 을 부르지 않는다.
//  Windows 의 sleep_for 는 타이머 눈금(15.6ms)으로 올림돼 200us 를 15.6ms 로 만든다. [why D-115]
//  조건변수 wait_for 는 그 눈금을 타지 않아 짧은 대기도 값대로 잰다.
void park_for(int microseconds)
{
    std::mutex              mutex;
    std::condition_variable idle;
    std::unique_lock<std::mutex> lock(mutex);
    idle.wait_for(lock, std::chrono::microseconds(microseconds));
}

void busy_spin(int microseconds)
{
    const auto deadline = Clock::now() + std::chrono::microseconds(microseconds);
    volatile double sink = 0.0;

    while (Clock::now() < deadline)
    {
        sink += 1.0;
    }

    (void)sink;
}

// 지연에 민감한 이웃 하나. 엔진의 주문·체결 스레드 자리다 — 할 일이 있는 채로 깨어 있고,
//  정해진 시각에 돌아야 한다. 자지 않고 기다렸다가(Windows 타이머 눈금을 피한다) 목표 시각을
//  얼마나 넘겨서야 실제로 돌았는지 잰다. 그 초과분이 곧 다른 스레드에 밀린 시간이다. [why D-115]
struct VictimResult
{
    double p50_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
};

VictimResult run_victim(const std::atomic<bool>& running, int wake_us, std::size_t sample_room)
{
    std::vector<double> overshoot;
    overshoot.reserve(sample_room);

    auto deadline = Clock::now() + std::chrono::microseconds(wake_us);

    while (running.load(std::memory_order_relaxed))
    {
        while (Clock::now() < deadline && running.load(std::memory_order_relaxed))
        {
            std::this_thread::yield();
        }

        const auto woke = Clock::now();

        if (overshoot.size() < sample_room)
        {
            overshoot.push_back(std::chrono::duration<double, std::micro>(woke - deadline).count());
        }

        deadline = woke + std::chrono::microseconds(wake_us);
    }

    VictimResult result;

    if (overshoot.empty())
    {
        return result;
    }

    for (double one : overshoot)
    {
        result.max_us = (std::max)(result.max_us, one);
    }

    auto at = [&overshoot](double ratio) {
        const std::size_t index = static_cast<std::size_t>(ratio * static_cast<double>(overshoot.size() - 1));
        std::nth_element(overshoot.begin(), overshoot.begin() + static_cast<std::ptrdiff_t>(index), overshoot.end());
        return overshoot[index];
    };

    result.p50_us = at(0.50);
    result.p99_us = at(0.99);
    return result;
}

// 프로세스가 실제로 쓰는 물리 메모리(MB). 스레드 스택은 예약만 하고 쓴 만큼 올라오므로
//  "스레드 하나가 얼마나 드는가"는 예약(1MB)이 아니라 이 값의 증가분으로 본다.
double resident_megabytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};

    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0)
    {
        return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }

    return 0.0;
#else
    std::FILE* file = std::fopen("/proc/self/statm", "r");

    if (file == nullptr)
    {
        return 0.0;
    }

    long      total_pages    = 0;
    long      resident_pages = 0;
    const int read           = std::fscanf(file, "%ld %ld", &total_pages, &resident_pages);
    std::fclose(file);

    if (read != 2)
    {
        return 0.0;
    }

    return static_cast<double>(resident_pages) * 4096.0 / (1024.0 * 1024.0);
#endif
}

double percentile(std::vector<double>& values, double ratio)
{
    if (values.empty())
    {
        return 0.0;
    }

    const std::size_t index = static_cast<std::size_t>(ratio * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

int run_one(const Options& options, bool print_header)
{
    const std::size_t samples_per_task =
        static_cast<std::size_t>(options.seconds) * 1000u / static_cast<std::size_t>(options.period_ms < 1 ? 1 : options.period_ms) + 8u;

    std::vector<std::unique_ptr<TaskProbe>> probes;
    probes.reserve(options.tasks);

    for (std::size_t index = 0; index < options.tasks; ++index)
    {
        auto probe = std::make_unique<TaskProbe>();
        probe->intervals_ms.resize(samples_per_task, 0.0);
        probes.push_back(std::move(probe));
    }

    // 스레드가 뜨기 전에 기준선을 잡는다 — 증가분이 곧 스레드 N개가 실제로 쓴 물리 메모리다.
    const double memory_before = resident_megabytes();

    prefetch::Pool pool(options.threads, std::chrono::milliseconds(options.period_ms));

    for (std::size_t index = 0; index < options.tasks; ++index)
    {
        TaskProbe* probe = probes[index].get();
        const int  work_us   = options.work_us;
        const bool wait_kind = options.work_kind == WorkKind::Wait;

        pool.add([probe, work_us, wait_kind] {
            const auto now = Clock::now();

            if (probe->runs.load(std::memory_order_relaxed) > 0 && probe->written < probe->intervals_ms.size())
            {
                probe->intervals_ms[probe->written++] =
                    std::chrono::duration<double, std::milli>(now - probe->last).count();
            }

            probe->last = now;
            probe->runs.fetch_add(1, std::memory_order_relaxed);

            if (wait_kind)
            {
                park_for(work_us);
            }
            else
            {
                busy_spin(work_us);
            }
        });
    }

    std::atomic<bool> victim_running{options.victim_us > 0};
    VictimResult      victim;
    std::thread       victim_thread;

    if (victim_running.load())
    {
        const std::size_t room =
            static_cast<std::size_t>(options.seconds) * 1000000u / static_cast<std::size_t>(options.victim_us) + 64u;
        victim_thread = std::thread([&victim, &victim_running, &options, room] {
            victim = run_victim(victim_running, options.victim_us, room);
        });
    }

    const auto start = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(options.seconds));
    const double memory_peak = resident_megabytes();
    victim_running.store(false);

    if (victim_thread.joinable())
    {
        victim_thread.join();
    }

    pool.stop();
    const double elapsed_sec = std::chrono::duration<double>(Clock::now() - start).count();

    std::vector<double> all_intervals;
    all_intervals.reserve(options.tasks * 4u);
    uint64_t total_runs = 0;

    for (const auto& probe : probes)
    {
        total_runs += probe->runs.load(std::memory_order_relaxed);

        for (std::size_t index = 0; index < probe->written; ++index)
        {
            all_intervals.push_back(probe->intervals_ms[index]);
        }
    }

    // 주기를 지켰는지 — 목표 주기의 1.5배를 넘긴 간격의 비율
    const double late_threshold = options.period_ms * 1.5;
    std::size_t  late_count     = 0;

    for (double one : all_intervals)
    {
        if (one > late_threshold)
        {
            ++late_count;
        }
    }

    const double late_percent = all_intervals.empty()
                                    ? 0.0
                                    : 100.0 * static_cast<double>(late_count) / static_cast<double>(all_intervals.size());
    const double p50 = percentile(all_intervals, 0.50);
    const double p99 = percentile(all_intervals, 0.99);
    double       max_interval = 0.0;

    for (double one : all_intervals)
    {
        max_interval = (std::max)(max_interval, one);
    }

    const char* kind_name = options.work_kind == WorkKind::Wait ? "wait" : "spin";
    char        line[512];
    std::snprintf(line, sizeof(line), "%zu,%zu,%d,%d,%s,%d,%.1f,%llu,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%s",
                  options.tasks, options.threads, options.period_ms, options.work_us, kind_name, options.seconds,
                  elapsed_sec, static_cast<unsigned long long>(total_runs),
                  static_cast<double>(total_runs) / elapsed_sec, p50, p99, max_interval, late_percent,
                  victim.p50_us, victim.p99_us, victim.max_us, memory_peak - memory_before,
                  options.tag.c_str());

    if (print_header)
    {
        std::printf("tasks,threads,period_ms,work_us,work_kind,seconds,elapsed_sec,runs,runs_per_sec,"
                    "interval_p50_ms,interval_p99_ms,interval_max_ms,late_pct,victim_p50_us,victim_p99_us,victim_max_us,memory_mb,tag\n");
    }

    std::printf("%s\n", line);
    std::fflush(stdout);

    if (!options.out_path.empty())
    {
        const bool exists = [&options] {
            std::FILE* probe_file = std::fopen(options.out_path.c_str(), "r");

            if (probe_file != nullptr)
            {
                std::fclose(probe_file);
                return true;
            }

            return false;
        }();

        std::FILE* file = std::fopen(options.out_path.c_str(), "a");

        if (file != nullptr)
        {
            if (!exists)
            {
                std::fprintf(file, "tasks,threads,period_ms,work_us,work_kind,seconds,elapsed_sec,runs,runs_per_sec,"
                                   "interval_p50_ms,interval_p99_ms,interval_max_ms,late_pct,victim_p50_us,victim_p99_us,victim_max_us,memory_mb,tag\n");
            }

            std::fprintf(file, "%s\n", line);
            std::fclose(file);
        }
    }

    return 0;
}

void print_usage()
{
    std::printf("사용법:\n");
    std::printf("  bench_prefetch_pool run   [--tasks N] [--threads N] [--period-ms N] [--work-us N]\n");
    std::printf("                            [--work-kind wait|spin] [--seconds N] [--out <경로>] [--tag <표시>]\n");
    std::printf("                            [--victim-us N] 지연에 민감한 이웃 스레드(0이면 안 띄움)\n");
    std::printf("  bench_prefetch_pool sweep --thread-list 2,4,8,16,32 [위 노브 그대로]\n");
    std::printf("기본값: 작업 2700개, 스레드 8개, 주기 1000ms, 작업 200us, 대기형, 20초\n");
}

std::vector<std::size_t> parse_list(const char* text)
{
    std::vector<std::size_t> values;
    std::string              one;

    for (const char* cursor = text; ; ++cursor)
    {
        if (*cursor == ',' || *cursor == '\0')
        {
            if (!one.empty())
            {
                values.push_back(static_cast<std::size_t>(std::strtoul(one.c_str(), nullptr, 10)));
                one.clear();
            }

            if (*cursor == '\0')
            {
                break;
            }
        }
        else
        {
            one.push_back(*cursor);
        }
    }

    return values;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        print_usage();
        return 1;
    }

    const std::string command = argv[1];
    Options           options;
    std::vector<std::size_t> thread_list;

    for (int index = 2; index < argc; ++index)
    {
        const std::string name = argv[index];
        const char*       value = (index + 1 < argc) ? argv[index + 1] : nullptr;

        if (name == "--tasks" && value != nullptr)
        {
            options.tasks = static_cast<std::size_t>(std::strtoul(value, nullptr, 10));
            ++index;
        }
        else if (name == "--threads" && value != nullptr)
        {
            options.threads = static_cast<std::size_t>(std::strtoul(value, nullptr, 10));
            ++index;
        }
        else if (name == "--period-ms" && value != nullptr)
        {
            options.period_ms = std::atoi(value);
            ++index;
        }
        else if (name == "--work-us" && value != nullptr)
        {
            options.work_us = std::atoi(value);
            ++index;
        }
        else if (name == "--seconds" && value != nullptr)
        {
            options.seconds = std::atoi(value);
            ++index;
        }
        else if (name == "--work-kind" && value != nullptr)
        {
            options.work_kind = (std::strcmp(value, "spin") == 0) ? WorkKind::Spin : WorkKind::Wait;
            ++index;
        }
        else if (name == "--victim-us" && value != nullptr)
        {
            options.victim_us = std::atoi(value);
            ++index;
        }
        else if (name == "--tag" && value != nullptr)
        {
            options.tag = value;
            ++index;
        }
        else if (name == "--out" && value != nullptr)
        {
            options.out_path = value;
            ++index;
        }
        else if (name == "--thread-list" && value != nullptr)
        {
            thread_list = parse_list(value);
            ++index;
        }
        else
        {
            print_usage();
            return 1;
        }
    }

    std::printf("하드웨어 스레드 %u개, 풀 기본 권고 %zu개\n", std::thread::hardware_concurrency(),
                prefetch::Pool::recommended_thread_count());

    if (command == "run")
    {
        return run_one(options, /*print_header=*/true);
    }

    if (command == "sweep")
    {
        if (thread_list.empty())
        {
            thread_list = {2, 4, 8, 16, 32};
        }

        bool header = true;

        for (std::size_t threads : thread_list)
        {
            options.threads = threads;
            run_one(options, header);
            header = false;
        }

        return 0;
    }

    print_usage();
    return 1;
}
