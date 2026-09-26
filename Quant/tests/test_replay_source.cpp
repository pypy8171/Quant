// ReplaySource 단위 테스트 — 캡처 파일을 같은 순서로 되돌리는지, 종목 필터, 속도 조절, 없는 파일, 도중 정지.
// 빌드: cmake --build <directory> --target test_replay_source
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

// 이 실행만의 꼬리표. 임시 폴더는 트리와 상관없이 한 곳이라, 세션 둘이 각자 워크트리에서 ctest를 돌리면
//  고정 이름으로는 서로 캡처 파일을 덮어쓴다 — 그러면 이 시험이 제 코드와 무관하게 깨진다.
std::string unique_suffix()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();

    return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

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

TradeData make_trade(int index, int64_t received_ns)
{
    TradeData trade;
    trade.ticker    = index % 2 == 0 ? "005930" : "000660";
    trade.hhmmss    = 90100;
    trade.price     = 70000.0 + index;
    trade.quantity  = 10 + index;
    trade.direction = 1;
    trade.market    = Market::KR;
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(1'700'000'000'000'000LL + index));
    trade.received_ns   = received_ns;
    return trade;
}

OrderBook make_book(int index)
{
    OrderBook order_book;
    order_book.ticker  = "005930";
    order_book.hhmmss  = 90100;
    order_book.received_ns = 1'050'000'000;

    for (int innermost_index = 0; innermost_index < 5; ++innermost_index)
    {
        order_book.asks[innermost_index] = {70100.0 + innermost_index * 100 + index, 100 + innermost_index};
        order_book.bids[innermost_index] = {70000.0 - innermost_index * 100 - index, 200 + innermost_index};
    }

    return order_book;
}

// 재생이 끝날 때까지 기다린다(최대 5초).
bool wait_finished(const feed::ReplaySource& source)
{
    for (int index = 0; index < 500 && !source.finished(); ++index)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return source.finished();
}

struct Seen
{
    std::vector<std::string> order; // "T:ticker:price" / "B:ticker"
};

void hook(feed::ReplaySource& source, Seen& seen)
{
    source.set_callbacks([&seen](const OrderBook& order_book)
    {
        seen.order.push_back("B:" + order_book.ticker.string());
    },
                      [&seen](const TradeData& trade)
                      {
                          seen.order.push_back("T:" + trade.ticker.string() + ":" + std::to_string(static_cast<int>(trade.price)));
                      });
}

} // namespace

int main()
{
    const auto path = std::filesystem::temp_directory_path() / ("quant_test_replay_source_" + unique_suffix() + ".bin");
    std::filesystem::remove(path);

    // 캡처: 체결 4(005930·000660 번갈아, 간격 100ms)·호가 1. 총 5.
    {
        feed::TickCapture capture(path);
        CHECK(capture.ok());
        capture.on_trade(make_trade(0, 1'000'000'000));
        capture.on_book(make_book(0));
        capture.on_trade(make_trade(1, 1'100'000'000));
        capture.on_trade(make_trade(2, 1'200'000'000));
        capture.on_trade(make_trade(3, 1'300'000'000));
        capture.flush();
        CHECK(capture.written() == 5);
    }

    // 1. 전 종목·최대 속도: 5개가 캡처 순서 그대로 온다.
    {
        feed::ReplaySource source(path, 0.0);
        Seen               seen;
        hook(source, seen);
        CHECK(source.connect({}));
        CHECK(source.is_connected());
        CHECK(wait_finished(source));
        CHECK(!source.is_connected());
        CHECK(!source.is_stale(0));
        CHECK(source.played() == 5 && source.skipped() == 0);
        const std::vector<std::string> expected = {"T:005930:70000", "B:005930", "T:000660:70001", "T:005930:70002",
                                               "T:000660:70003"};
        CHECK(seen.order == expected);
    }

    // 2. 종목 필터: 000660만 구독하면 체결 2개, 나머지 3개는 skipped.
    {
        feed::ReplaySource source(path, 0.0);
        Seen               seen;
        hook(source, seen);
        WatchSpec specification;
        specification.ticker = "000660";
        CHECK(source.connect({specification}));
        CHECK(wait_finished(source));
        CHECK(source.played() == 2 && source.skipped() == 3);
        CHECK(seen.order.size() == 2 && seen.order[0] == "T:000660:70001");
        CHECK(source.has_specification(specification));
        WatchSpec other;
        other.ticker = "005930";
        CHECK(!source.has_specification(other));
        CHECK(source.take_overflow_specifications().empty());
    }

    // 3. 속도 1.0: 캡처 간격(총 300ms)만큼은 걸린다. 속도 10: 그보다 뚜렷이 짧다.
    {
        feed::ReplaySource source(path, 1.0);
        Seen               seen;
        hook(source, seen);
        const auto start_time = std::chrono::steady_clock::now();
        CHECK(source.connect({}));
        CHECK(wait_finished(source));
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        CHECK(milliseconds >= 250);
        CHECK(seen.order.size() == 5);
    }

    {
        feed::ReplaySource source(path, 10.0);
        Seen               seen;
        hook(source, seen);
        const auto start_time = std::chrono::steady_clock::now();
        CHECK(source.connect({}));
        CHECK(wait_finished(source));
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        CHECK(milliseconds < 200);
    }

    // 4. 재생 도중 disconnect하면 스레드가 곧 멈추고 finished는 false로 남는다.
    {
        feed::ReplaySource source(path, 0.01); // 간격 100ms를 100배 늘려 10초짜리로
        Seen               seen;
        hook(source, seen);
        CHECK(source.connect({}));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto start_time = std::chrono::steady_clock::now();
        source.disconnect();
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        CHECK(milliseconds < 500);
        CHECK(!source.is_connected());
        CHECK(!source.finished());
        CHECK(seen.order.size() >= 1 && seen.order.size() < 5);
    }

    // 5. 없는 파일이면 connect가 false.
    {
        feed::ReplaySource source(std::filesystem::temp_directory_path() /
                                  ("quant_test_replay_missing_" + unique_suffix() + ".bin"));
        CHECK(!source.connect({}));
        CHECK(!source.is_connected());
    }

    // 6. subscribe_incremental: 새 종목이면 true, 같은 종목 두 번째는 false.
    {
        feed::ReplaySource source(path);
        WatchSpec          specification;
        specification.ticker = "005930";
        CHECK(source.subscribe_incremental(specification));
        CHECK(!source.subscribe_incremental(specification));
    }

    std::filesystem::remove(path);
    std::cout << "test_replay_source: " << g_checks << " checks passed\n";
    return 0;
}
