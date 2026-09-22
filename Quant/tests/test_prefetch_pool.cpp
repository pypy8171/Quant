// prefetch::Pool 단위 테스트 — 스레드 수 고정(작업 수와 무관), 주기 실행, 해제 뒤 미호출, 해제 중 실행 대기, 정지.
// 빌드: cmake --build <directory> --target test_prefetch_pool
#include "core/PrefetchPool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

// 조건이 참이 될 때까지 최대 timeout만큼 기다린다(느린 CI에서 고정 sleep이 깨지는 것을 막는다).
template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return predicate();
}

// 작업 100개를 맡겨도 스레드는 생성자에서 정한 수 그대로다 — 전략당 스레드를 없앤 이유. [why D-071]
int test_thread_count_is_fixed()
{
    prefetch::Pool pool(3, std::chrono::milliseconds(10));
    std::atomic<int> calls{0};

    for (int index = 0; index < 100; ++index)
    {
        pool.add([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });
    }

    CHECK(pool.task_count() == 100);
    CHECK(pool.thread_count() == 3);
    CHECK(wait_until([&calls] { return calls.load(std::memory_order_relaxed) >= 100; }, std::chrono::seconds(5)));
    return 0;
}

// 등록 전에는 스레드가 없다(FEED 모드·테스트처럼 전략이 없는 구성).
int test_no_threads_before_add()
{
    prefetch::Pool pool(4, std::chrono::milliseconds(10));
    CHECK(pool.thread_count() == 0);
    CHECK(pool.task_count() == 0);
    return 0;
}

// 주기마다 다시 불린다.
int test_runs_repeatedly()
{
    prefetch::Pool pool(1, std::chrono::milliseconds(10));
    std::atomic<int> calls{0};
    pool.add([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });

    CHECK(wait_until([&calls] { return calls.load(std::memory_order_relaxed) >= 3; }, std::chrono::seconds(5)));
    return 0;
}

// 해제하면 목록에서 빠지고 더는 불리지 않는다.
int test_remove_stops_calls()
{
    prefetch::Pool pool(2, std::chrono::milliseconds(10));
    std::atomic<int> calls{0};
    const prefetch::Pool::TaskId id = pool.add([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });

    CHECK(wait_until([&calls] { return calls.load(std::memory_order_relaxed) >= 1; }, std::chrono::seconds(5)));
    pool.remove(id);
    CHECK(pool.task_count() == 0);

    const int after_remove = calls.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(calls.load(std::memory_order_relaxed) == after_remove);
    return 0;
}

// 실행 중에 해제하면 그 호출이 끝난 뒤에 돌아온다 — 전략이 스스로를 지우기 전에 기대는 성질.
int test_remove_waits_for_running_work()
{
    prefetch::Pool pool(1, std::chrono::milliseconds(5));
    std::atomic<bool> inside{false};
    std::atomic<bool> finished{false};

    const prefetch::Pool::TaskId id = pool.add([&inside, &finished] {
        inside.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        finished.store(true, std::memory_order_release);
    });

    CHECK(wait_until([&inside] { return inside.load(std::memory_order_acquire); }, std::chrono::seconds(5)));
    pool.remove(id);
    CHECK(finished.load(std::memory_order_acquire));
    return 0;
}

// 정지 뒤에는 아무 작업도 불리지 않고, 다시 등록해도 스레드가 살아나지 않는다.
int test_stop()
{
    prefetch::Pool pool(2, std::chrono::milliseconds(10));
    std::atomic<int> calls{0};
    pool.add([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });

    CHECK(wait_until([&calls] { return calls.load(std::memory_order_relaxed) >= 1; }, std::chrono::seconds(5)));
    pool.stop();
    CHECK(pool.thread_count() == 0);

    const int after_stop = calls.load(std::memory_order_relaxed);
    pool.add([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(calls.load(std::memory_order_relaxed) == after_stop);
    return 0;
}

} // namespace

int main()
{
    struct Case
    {
        const char* name;
        int (*run)();
    };

    const Case cases[] = {
        {"thread_count_is_fixed", test_thread_count_is_fixed},
        {"no_threads_before_add", test_no_threads_before_add},
        {"runs_repeatedly", test_runs_repeatedly},
        {"remove_stops_calls", test_remove_stops_calls},
        {"remove_waits_for_running_work", test_remove_waits_for_running_work},
        {"stop", test_stop},
    };

    for (const auto& one : cases)
    {
        if (one.run() != 0)
        {
            std::cerr << "테스트 실패: " << one.name << "\n";
            return 1;
        }

        std::cout << "  ok  " << one.name << "\n";
    }

    std::cout << "test_prefetch_pool 통과 — 검사 " << g_checks << "개\n";
    return 0;
}
