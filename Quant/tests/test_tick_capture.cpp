// TickCapture/TickReader 단위 테스트 — 왕복 필드 보존, 이어 쓰기(머리 한 번), 잘린 꼬리, 없는 파일, 큐 넘침 계수.
// 빌드: cmake --build <directory> --target test_tick_capture
#include "core/RingBuffer.h"
#include "core/TickCapture.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
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

TradeData make_trade(int index)
{
    TradeData trade;
    trade.ticker      = index % 2 == 0 ? "005930" : "000660";
    trade.symbol_id         = static_cast<symbol::SymbolId>(index % 2 + 1);
    trade.hhmmss      = 90100 + index; // 0901ii
    trade.price       = 70000.0 + index;
    trade.quantity    = 10 + index;
    trade.direction   = index % 2 == 0 ? 1 : 5;
    trade.market      = Market::KR;
    trade.timestamp   = std::chrono::system_clock::time_point(std::chrono::microseconds(1'700'000'000'000'000LL + index));
    trade.strength    = 100.5 + index;
    trade.accumulated_volume = 1000 + index;
    trade.received_ns     = 5'000'000 + index;
    return trade;
}

OrderBook make_book(int index)
{
    OrderBook order_book;
    order_book.ticker    = "005930";
    order_book.symbol_id       = 1;
    order_book.hhmmss    = 90100;
    order_book.received_ns   = 7'000'000 + index; // 수신 스레드가 찍은 값이 캡처에 그대로 남는다
    order_book.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(1'700'000'000'000'000LL + index));

    for (int innermost_index = 0; innermost_index < 5; ++innermost_index)
    {
        order_book.asks[innermost_index] = {70100.0 + innermost_index * 100 + index, 100 + innermost_index};
        order_book.bids[innermost_index] = {70000.0 - innermost_index * 100 - index, 200 + innermost_index};
    }

    return order_book;
}

} // namespace

int main()
{
    const auto path = std::filesystem::temp_directory_path() / "quant_test_tick_capture.bin";
    std::filesystem::remove(path);

    // 1. 체결 3·호가 2를 섞어 쓰고 그대로 읽는다.
    {
        feed::TickCapture capture(path);
        CHECK(capture.ok());
        capture.on_trade(make_trade(0));
        capture.on_book(make_book(0));
        capture.on_trade(make_trade(1));
        capture.on_trade(make_trade(2));
        capture.on_book(make_book(1));
        capture.flush();
        CHECK(capture.written() == 5);
        CHECK(capture.dropped() == 0);
    }

    {
        feed::TickReader reader(path);
        CHECK(reader.ok());
        CHECK(reader.start_utc_ms() > 1'600'000'000'000LL);
        feed::Record record;

        CHECK(reader.next(record) && record.kind == feed::kKindTrade);
        const TradeData start_time = feed::to_trade(record.trade);
        CHECK(start_time.ticker == "005930" && start_time.symbol_id == 1 && start_time.hhmmss == 90100);
        CHECK(start_time.price == 70000.0 && start_time.quantity == 10 && start_time.direction == 1 && start_time.market == Market::KR);
        CHECK(start_time.strength == 100.5 && start_time.accumulated_volume == 1000 && start_time.received_ns == 5'000'000);
        CHECK(start_time.timestamp == make_trade(0).timestamp);

        CHECK(reader.next(record) && record.kind == feed::kKindBook);
        const OrderBook decoded_book = feed::to_book(record.book);
        CHECK(decoded_book.ticker == "005930" && decoded_book.symbol_id == 1 && decoded_book.hhmmss == 90100);
        CHECK(decoded_book.asks[4].price == 70500.0 && decoded_book.asks[4].quantity == 104);
        CHECK(decoded_book.bids[0].price == 70000.0 && decoded_book.bids[0].quantity == 200);
        CHECK(record.book.common.received_ns == 7'000'000 && decoded_book.received_ns == 7'000'000);
        CHECK(decoded_book.timestamp == make_book(0).timestamp);

        CHECK(reader.next(record) && record.kind == feed::kKindTrade && feed::to_trade(record.trade).ticker == "000660");
        CHECK(reader.next(record) && record.kind == feed::kKindTrade && feed::to_trade(record.trade).price == 70002.0);
        CHECK(reader.next(record) && record.kind == feed::kKindBook && record.book.common.received_ns == 7'000'001);
        CHECK(!reader.next(record));
        CHECK(!reader.next(record));
    }

    // 2. 같은 파일에 두 번째 인스턴스가 이어 쓰면 머리는 다시 안 붙고 레코드가 6개다.
    {
        feed::TickCapture capture(path);
        capture.on_trade(make_trade(3));
        capture.flush();
    }

    {
        feed::TickReader reader(path);
        feed::Record     record;
        int              count = 0;

        while (reader.next(record))
        {
            ++count;
        }

        CHECK(count == 6);
    }

    // 3. 꼬리가 잘린 파일: 마지막 레코드 중간에서 끊으면 앞 5개만 나온다.
    {
        const auto size = std::filesystem::file_size(path);
        std::filesystem::resize_file(path, size - 10);
        feed::TickReader reader(path);
        feed::Record     record;
        int              count = 0;

        while (reader.next(record))
        {
            ++count;
        }

        CHECK(count == 5);
    }

    // 4. 없는 파일·머리가 다른 파일은 ok()가 false.
    {
        feed::TickReader reader(std::filesystem::temp_directory_path() / "quant_test_tick_capture_missing.bin");
        CHECK(!reader.ok());
        feed::Record record;
        CHECK(!reader.next(record));

        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        std::fputs("not a capture", file);
        std::fclose(file);
        feed::TickReader bad(path);
        CHECK(!bad.ok());
    }

    std::filesystem::remove(path);

    // 5. 큐 상한 4에 6개를 넣으면(기록 스레드가 못 따라오게 하진 못하니) 버린 수 + 쓴 수 = 6.
    {
        feed::TickCapture capture(path, 4);

        for (int index = 0; index < 6; ++index)
        {
            capture.on_trade(make_trade(index));
        }

        capture.flush();
        CHECK(capture.written() + capture.dropped() == 6);
        CHECK(capture.written() >= 4);
    }

    std::filesystem::remove(path);

    // 6. 열 수 없는 경로면 ok()가 false고 on_*는 조용히 무시한다.
    {
        feed::TickCapture capture(std::filesystem::path("Z:/no_such_dir_quant/x/ticks.bin"));

        if (!capture.ok())
        {
            capture.on_trade(make_trade(0));
            CHECK(capture.written() == 0);
        }
    }

    // 7. 측정(원칙 7) — 틱 구조체 크기와, 틱 한 건이 링을 지나는 비용·캡처 블록에서 되돌리는 비용.
    //  종목 코드가 std::string이던 때(2026-09-13, Release): TradeData 96B·링 13ns·decode 11ns. symbol::Ticker 고정 배열로
    //  바꾼 뒤: 80B·3ns·6ns. 구조체가 trivially copyable이 돼 링 복사가 memcpy로 내려간 몫이다.
    {
        constexpr int kN = 1'000'000;
        RingBuffer<TradeData> ring(1024);
        const TradeData source = make_trade(0);
        auto start_time = std::chrono::steady_clock::now();
        double sink = 0.0;

        for (int index = 0; index < kN; ++index)
        {
            (void)ring.push(source);
            auto received = ring.pop();
            sink += received ? received->price : 0.0;
        }

        auto end_time = std::chrono::steady_clock::now();
        const feed::TradeBody blk = feed::to_body(source);
        int64_t accumulator = 0;

        for (int index = 0; index < kN; ++index)
        {
            TradeData trade = feed::to_trade(blk);
            accumulator += trade.quantity + static_cast<int64_t>(trade.ticker.size());
        }

        auto later_time = std::chrono::steady_clock::now();
        const auto nanoseconds = [](auto left, auto right) { return std::chrono::duration_cast<std::chrono::nanoseconds>(right - left).count(); };
        std::cout << "[측정] sizeof TradeData=" << sizeof(TradeData) << " OrderBook=" << sizeof(OrderBook)
                  << " MarketData=" << sizeof(MarketData) << " | 링 push+pop " << nanoseconds(start_time, end_time) / kN << "ns/틱 | 캡처 decode "
                  << nanoseconds(end_time, later_time) / kN << "ns/틱 (sink " << sink << ' ' << accumulator << ")\n";
    }

    // 8. 생산자 여럿(FeedMux 레인) — 스레드 4개가 500건씩 동시에 넣어도 쓴 수 + 버린 수 = 2000이고 파일도 그만큼이다.
    {
        {
            feed::TickCapture        capture(path, 4096);
            std::vector<std::thread> threads;

            for (int time_value = 0; time_value < 4; ++time_value)
            {
                threads.emplace_back(
                    [&capture, time_value]
                    {
                        for (int index = 0; index < 500; ++index)
                        {
                            capture.on_trade(make_trade(time_value * 500 + index));
                        }
                    });
            }

            for (auto& thread : threads)
            {
                thread.join();
            }

            capture.flush();
            CHECK(capture.written() + capture.dropped() == 2000);
            CHECK(capture.written() == 2000);
        }

        feed::TickReader reader(path);
        feed::Record     record;
        size_t           count = 0;

        while (reader.next(record))
        {
            ++count;
        }

        CHECK(count == 2000);
    }

    std::filesystem::remove(path);

    std::cout << "test_tick_capture: " << g_checks << " checks passed\n";
    return 0;
}
