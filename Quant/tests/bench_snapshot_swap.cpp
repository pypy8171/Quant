// 평가 1회가 스냅샷을 잡는 비용 — 전(벡터 복사) vs 후(shared_ptr 교체) vs 락 없이 atomic<shared_ptr> 하나.
//  일봉 250봉 + 3분봉 63봉. [why D-114]
// 빌드: cmake --build <directory> --target bench_snapshot_swap   (ctest 밖, 부하 하네스 관례)
// 결과 기록: docs/reports/stresstest/2026-09-22_prefetch_pool.md
#include "core/Types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
constexpr int kIterations = 200000;

// 락 없는 후보 — 스냅샷 네 값을 구조체 하나로 묶어 포인터 하나만 바꾼다.
struct Snapshot
{
    std::vector<MarketData> daily;
    std::vector<MarketData> minute;
    int                     bucket  = 0;
    uint64_t                version = 0;
};

std::vector<MarketData> make_bars(int count)
{
    std::vector<MarketData> bars(static_cast<size_t>(count));

    for (int index = 0; index < count; ++index)
    {
        bars[static_cast<size_t>(index)].close = 70000.0 + index;
        bars[static_cast<size_t>(index)].bar_index = index;
    }

    return bars;
}
} // namespace

int main()
{
    const std::vector<MarketData> daily = make_bars(250);
    const std::vector<MarketData> minute = make_bars(63);
    const auto daily_pointer  = std::make_shared<const std::vector<MarketData>>(daily);
    const auto minute_pointer = std::make_shared<const std::vector<MarketData>>(minute);

    std::mutex mutex;
    double     sink = 0.0;

    const auto before_start = std::chrono::steady_clock::now();

    for (int iteration = 0; iteration < kIterations; ++iteration)
    {
        std::vector<MarketData> local_daily;
        std::vector<MarketData> local_minute;
        {
            std::lock_guard<std::mutex> lock(mutex);
            local_daily  = daily;
            local_minute = minute;
        }

        sink += local_daily[0].close + local_minute[0].close;
    }

    const auto before_end = std::chrono::steady_clock::now();

    for (int iteration = 0; iteration < kIterations; ++iteration)
    {
        std::shared_ptr<const std::vector<MarketData>> local_daily;
        std::shared_ptr<const std::vector<MarketData>> local_minute;
        {
            std::lock_guard<std::mutex> lock(mutex);
            local_daily  = daily_pointer;
            local_minute = minute_pointer;
        }

        sink += (*local_daily)[0].close + (*local_minute)[0].close;
    }

    const auto after_end = std::chrono::steady_clock::now();

    std::atomic<std::shared_ptr<const Snapshot>> atomic_snapshot{
        std::make_shared<const Snapshot>(Snapshot{daily, minute, 0, 0})};

    for (int iteration = 0; iteration < kIterations; ++iteration)
    {
        const std::shared_ptr<const Snapshot> local = atomic_snapshot.load(std::memory_order_acquire);
        sink += local->daily[0].close + local->minute[0].close;
    }

    const auto atomic_end = std::chrono::steady_clock::now();

    const double before_ns = std::chrono::duration<double, std::nano>(before_end - before_start).count() / kIterations;
    const double after_ns  = std::chrono::duration<double, std::nano>(after_end - before_end).count() / kIterations;
    const double atomic_ns = std::chrono::duration<double, std::nano>(atomic_end - after_end).count() / kIterations;

    // CSV 한 줄(docs/reports/stresstest/data/ 에 그대로 붙인다). sink는 최적화가 루프를 지우지 못하게 잡아 두는 값이다.
    std::printf("iterations,bytes_per_snapshot,copy_ns,pointer_ns,atomic_ns,ratio\n");
    std::printf("%d,%zu,%.0f,%.0f,%.0f,%.1f\n", kIterations, sizeof(MarketData) * 313, before_ns, after_ns, atomic_ns,
                before_ns / after_ns);
    std::fprintf(stderr, "(sink=%f)\n", sink);
    return 0;
}
