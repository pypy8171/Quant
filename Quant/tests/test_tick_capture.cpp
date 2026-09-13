// TickCapture/TickReader 단위 테스트 — 왕복 필드 보존, 이어 쓰기(머리 한 번), 잘린 꼬리, 없는 파일, 큐 넘침 계수.
// 빌드: cmake --build <dir> --target test_tick_capture
#include "core/RingBuffer.h"
#include "core/TickCapture.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
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

TradeData make_trade(int i)
{
    TradeData td;
    td.ticker      = i % 2 == 0 ? "005930" : "000660";
    td.sym         = static_cast<sym::SymbolId>(i % 2 + 1);
    td.hhmmss      = 90100 + i; // 0901ii
    td.price       = 70000.0 + i;
    td.quantity    = 10 + i;
    td.direction   = i % 2 == 0 ? 1 : 5;
    td.market      = Market::KR;
    td.timestamp   = std::chrono::system_clock::time_point(std::chrono::microseconds(1'700'000'000'000'000LL + i));
    td.strength    = 100.5 + i;
    td.acml_volume = 1000 + i;
    td.recv_ns     = 5'000'000 + i;
    return td;
}

OrderBook make_book(int i)
{
    OrderBook ob;
    ob.ticker    = "005930";
    ob.sym       = 1;
    ob.hhmmss    = 90100;
    ob.timestamp = std::chrono::system_clock::time_point(std::chrono::microseconds(1'700'000'000'000'000LL + i));

    for (int k = 0; k < 5; ++k)
    {
        ob.asks[k] = {70100.0 + k * 100 + i, 100 + k};
        ob.bids[k] = {70000.0 - k * 100 - i, 200 + k};
    }

    return ob;
}

} // namespace

int main()
{
    const auto path = std::filesystem::temp_directory_path() / "quant_test_tick_capture.bin";
    std::filesystem::remove(path);

    // 1. 체결 3·호가 2를 섞어 쓰고 그대로 읽는다.
    {
        feed::TickCapture cap(path);
        CHECK(cap.ok());
        cap.on_trade(make_trade(0));
        cap.on_book(make_book(0), 7'000'000);
        cap.on_trade(make_trade(1));
        cap.on_trade(make_trade(2));
        cap.on_book(make_book(1), 7'000'001);
        cap.flush();
        CHECK(cap.written() == 5);
        CHECK(cap.dropped() == 0);
    }

    {
        feed::TickReader rd(path);
        CHECK(rd.ok());
        CHECK(rd.start_utc_ms() > 1'600'000'000'000LL);
        feed::Record r;

        CHECK(rd.next(r) && r.kind == feed::kKindTrade);
        const TradeData t0 = feed::to_trade(r.trade);
        CHECK(t0.ticker == "005930" && t0.sym == 1 && t0.hhmmss == 90100);
        CHECK(t0.price == 70000.0 && t0.quantity == 10 && t0.direction == 1 && t0.market == Market::KR);
        CHECK(t0.strength == 100.5 && t0.acml_volume == 1000 && t0.recv_ns == 5'000'000);
        CHECK(t0.timestamp == make_trade(0).timestamp);

        CHECK(rd.next(r) && r.kind == feed::kKindBook);
        const OrderBook b0 = feed::to_book(r.book);
        CHECK(b0.ticker == "005930" && b0.sym == 1 && b0.hhmmss == 90100);
        CHECK(b0.asks[4].price == 70500.0 && b0.asks[4].quantity == 104);
        CHECK(b0.bids[0].price == 70000.0 && b0.bids[0].quantity == 200);
        CHECK(r.book.c.recv_ns == 7'000'000);
        CHECK(b0.timestamp == make_book(0).timestamp);

        CHECK(rd.next(r) && r.kind == feed::kKindTrade && feed::to_trade(r.trade).ticker == "000660");
        CHECK(rd.next(r) && r.kind == feed::kKindTrade && feed::to_trade(r.trade).price == 70002.0);
        CHECK(rd.next(r) && r.kind == feed::kKindBook && r.book.c.recv_ns == 7'000'001);
        CHECK(!rd.next(r));
        CHECK(!rd.next(r));
    }

    // 2. 같은 파일에 두 번째 인스턴스가 이어 쓰면 머리는 다시 안 붙고 레코드가 6개다.
    {
        feed::TickCapture cap(path);
        cap.on_trade(make_trade(3));
        cap.flush();
    }

    {
        feed::TickReader rd(path);
        feed::Record     r;
        int              n = 0;

        while (rd.next(r))
        {
            ++n;
        }

        CHECK(n == 6);
    }

    // 3. 꼬리가 잘린 파일: 마지막 레코드 중간에서 끊으면 앞 5개만 나온다.
    {
        const auto size = std::filesystem::file_size(path);
        std::filesystem::resize_file(path, size - 10);
        feed::TickReader rd(path);
        feed::Record     r;
        int              n = 0;

        while (rd.next(r))
        {
            ++n;
        }

        CHECK(n == 5);
    }

    // 4. 없는 파일·머리가 다른 파일은 ok()가 false.
    {
        feed::TickReader rd(std::filesystem::temp_directory_path() / "quant_test_tick_capture_missing.bin");
        CHECK(!rd.ok());
        feed::Record r;
        CHECK(!rd.next(r));

        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        std::fputs("not a capture", f);
        std::fclose(f);
        feed::TickReader bad(path);
        CHECK(!bad.ok());
    }

    std::filesystem::remove(path);

    // 5. 큐 상한 4에 6개를 넣으면(기록 스레드가 못 따라오게 하진 못하니) 버린 수 + 쓴 수 = 6.
    {
        feed::TickCapture cap(path, 4);

        for (int i = 0; i < 6; ++i)
        {
            cap.on_trade(make_trade(i));
        }

        cap.flush();
        CHECK(cap.written() + cap.dropped() == 6);
        CHECK(cap.written() >= 4);
    }

    std::filesystem::remove(path);

    // 6. 열 수 없는 경로면 ok()가 false고 on_*는 조용히 무시한다.
    {
        feed::TickCapture cap(std::filesystem::path("Z:/no_such_dir_quant/x/ticks.bin"));

        if (!cap.ok())
        {
            cap.on_trade(make_trade(0));
            CHECK(cap.written() == 0);
        }
    }

    // 7. 측정(원칙 7) — 틱 구조체 크기와, 틱 한 건이 링을 지나는 비용·캡처 블록에서 되돌리는 비용.
    //  종목 코드가 std::string이던 때(2026-09-13, Release): TradeData 96B·링 13ns·decode 11ns. sym::Ticker 고정 배열로
    //  바꾼 뒤: 80B·3ns·6ns. 구조체가 trivially copyable이 돼 링 복사가 memcpy로 내려간 몫이다.
    {
        constexpr int kN = 1'000'000;
        RingBuffer<TradeData> ring(1024);
        const TradeData src = make_trade(0);
        auto t0 = std::chrono::steady_clock::now();
        double sink = 0.0;

        for (int i = 0; i < kN; ++i)
        {
            (void)ring.push(src);
            auto got = ring.pop();
            sink += got ? got->price : 0.0;
        }

        auto t1 = std::chrono::steady_clock::now();
        const feed::TradeBody blk = feed::to_body(src);
        int64_t acc = 0;

        for (int i = 0; i < kN; ++i)
        {
            TradeData td = feed::to_trade(blk);
            acc += td.quantity + static_cast<int64_t>(td.ticker.size());
        }

        auto t2 = std::chrono::steady_clock::now();
        const auto ns = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); };
        std::cout << "[측정] sizeof TradeData=" << sizeof(TradeData) << " OrderBook=" << sizeof(OrderBook)
                  << " MarketData=" << sizeof(MarketData) << " | 링 push+pop " << ns(t0, t1) / kN << "ns/틱 | 캡처 decode "
                  << ns(t1, t2) / kN << "ns/틱 (sink " << sink << ' ' << acc << ")\n";
    }

    std::cout << "test_tick_capture: " << g_checks << " checks passed\n";
    return 0;
}
