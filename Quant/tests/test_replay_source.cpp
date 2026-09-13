// ReplaySource 단위 테스트 — 캡처 파일을 같은 순서로 되돌리는지, 종목 필터, 속도 조절, 없는 파일, 도중 정지.
// 빌드: cmake --build <dir> --target test_replay_source
#include "core/ReplaySource.h"
#include "core/TickCapture.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

TradeData make_trade(int i, int64_t recv_ns)
{
    TradeData td;
    td.ticker    = i % 2 == 0 ? "005930" : "000660";
    td.hhmmss    = 90100;
    td.price     = 70000.0 + i;
    td.quantity  = 10 + i;
    td.direction = 1;
    td.market    = Market::KR;
    td.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(1'700'000'000'000'000LL + i));
    td.recv_ns   = recv_ns;
    return td;
}

OrderBook make_book(int i)
{
    OrderBook ob;
    ob.ticker = "005930";
    ob.hhmmss = 90100;

    for (int k = 0; k < 5; ++k)
    {
        ob.asks[k] = {70100.0 + k * 100 + i, 100 + k};
        ob.bids[k] = {70000.0 - k * 100 - i, 200 + k};
    }

    return ob;
}

// 재생이 끝날 때까지 기다린다(최대 5초).
bool wait_finished(const feed::ReplaySource& src)
{
    for (int i = 0; i < 500 && !src.finished(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return src.finished();
}

struct Seen
{
    std::vector<std::string> order; // "T:ticker:price" / "B:ticker"
};

void hook(feed::ReplaySource& src, Seen& seen)
{
    src.set_callbacks([&seen](const OrderBook& ob) { seen.order.push_back("B:" + ob.ticker); },
                      [&seen](const TradeData& td)
                      { seen.order.push_back("T:" + td.ticker + ":" + std::to_string(static_cast<int>(td.price))); });
}

} // namespace

int main()
{
    const auto path = std::filesystem::temp_directory_path() / "quant_test_replay_source.bin";
    std::filesystem::remove(path);

    // 캡처: 체결 4(005930·000660 번갈아, 간격 100ms)·호가 1. 총 5.
    {
        feed::TickCapture cap(path);
        CHECK(cap.ok());
        cap.on_trade(make_trade(0, 1'000'000'000));
        cap.on_book(make_book(0), 1'050'000'000);
        cap.on_trade(make_trade(1, 1'100'000'000));
        cap.on_trade(make_trade(2, 1'200'000'000));
        cap.on_trade(make_trade(3, 1'300'000'000));
        cap.flush();
        CHECK(cap.written() == 5);
    }

    // 1. 전 종목·최대 속도: 5개가 캡처 순서 그대로 온다.
    {
        feed::ReplaySource src(path, 0.0);
        Seen               seen;
        hook(src, seen);
        CHECK(src.connect({}));
        CHECK(src.is_connected());
        CHECK(wait_finished(src));
        CHECK(!src.is_connected());
        CHECK(!src.is_stale(0));
        CHECK(src.played() == 5 && src.skipped() == 0);
        const std::vector<std::string> want = {"T:005930:70000", "B:005930", "T:000660:70001", "T:005930:70002",
                                               "T:000660:70003"};
        CHECK(seen.order == want);
    }

    // 2. 종목 필터: 000660만 구독하면 체결 2개, 나머지 3개는 skipped.
    {
        feed::ReplaySource src(path, 0.0);
        Seen               seen;
        hook(src, seen);
        WatchSpec spec;
        spec.ticker = "000660";
        CHECK(src.connect({spec}));
        CHECK(wait_finished(src));
        CHECK(src.played() == 2 && src.skipped() == 3);
        CHECK(seen.order.size() == 2 && seen.order[0] == "T:000660:70001");
        CHECK(src.has_spec(spec));
        WatchSpec other;
        other.ticker = "005930";
        CHECK(!src.has_spec(other));
        CHECK(src.take_overflow_specs().empty());
    }

    // 3. 속도 1.0: 캡처 간격(총 300ms)만큼은 걸린다. 속도 10: 그보다 뚜렷이 짧다.
    {
        feed::ReplaySource src(path, 1.0);
        Seen               seen;
        hook(src, seen);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(src.connect({}));
        CHECK(wait_finished(src));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 250);
        CHECK(seen.order.size() == 5);
    }

    {
        feed::ReplaySource src(path, 10.0);
        Seen               seen;
        hook(src, seen);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(src.connect({}));
        CHECK(wait_finished(src));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 200);
    }

    // 4. 재생 도중 disconnect하면 스레드가 곧 멈추고 finished는 false로 남는다.
    {
        feed::ReplaySource src(path, 0.01); // 간격 100ms를 100배 늘려 10초짜리로
        Seen               seen;
        hook(src, seen);
        CHECK(src.connect({}));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto t0 = std::chrono::steady_clock::now();
        src.disconnect();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 500);
        CHECK(!src.is_connected());
        CHECK(!src.finished());
        CHECK(seen.order.size() >= 1 && seen.order.size() < 5);
    }

    // 5. 없는 파일이면 connect가 false.
    {
        feed::ReplaySource src(std::filesystem::temp_directory_path() / "quant_test_replay_missing.bin");
        CHECK(!src.connect({}));
        CHECK(!src.is_connected());
    }

    // 6. subscribe_incremental: 새 종목이면 true, 같은 종목 두 번째는 false.
    {
        feed::ReplaySource src(path);
        WatchSpec          spec;
        spec.ticker = "005930";
        CHECK(src.subscribe_incremental(spec));
        CHECK(!src.subscribe_incremental(spec));
    }

    std::filesystem::remove(path);
    std::cout << "test_replay_source: " << g_checks << " checks passed\n";
    return 0;
}
