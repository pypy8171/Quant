// bench_ws_parse — KIS 실시간 전문 파서(KisWebSocket::parse_message) 한 건 처리 시간.
//  소켓 없이 체결(H0STCNT0)·호가(H0STASP0) 전문 문자열을 만들어 parse_message에 바로 넣는다.
//  3단계 리팩터(parse_message를 단계 함수로 나눔, 2026-10-11)의 전후 비교용이다(원칙 7).
//  실행: bench_ws_parse [--iters N] (기본 1,000,000)
#include "api/KisClient.h"
#include "api/KisWebSocket.h"
#include "bench_common.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

// [wire] H0STCNT0 페이로드 — 46필드를 '^'로 잇는다. 디코더가 읽는 자리(0·1·2·12·13·18·21)만 실값, 나머지는 0.
std::string make_trade_payload(const std::string& ticker, int price)
{
    std::string text;

    for (int index = 0; index < 46; ++index)
    {
        if (index > 0)
        {
            text += '^';
        }

        switch (index)
        {
            case 0: text += ticker; break;
            case 1: text += "093001"; break;
            case 2: text += std::to_string(price); break;
            case 12: text += "150"; break;
            case 13: text += "1234567"; break;
            case 18: text += "120.55"; break;
            case 21: text += "1"; break;
            default: text += '0'; break;
        }
    }

    return text;
}

// [wire] H0STASP0 페이로드 — 호가 10단계·잔량을 담는 60칸. 디코더 최소 폭(38칸)을 넘긴다.
std::string make_orderbook_payload(const std::string& ticker, int price)
{
    std::string text;

    for (int index = 0; index < 60; ++index)
    {
        if (index > 0)
        {
            text += '^';
        }

        if (index == 0)
        {
            text += ticker;
        }
        else if (index == 1)
        {
            text += "093001";
        }
        else if (index >= 3 && index <= 22)
        {
            text += std::to_string(price + (index - 3) * 100);
        }
        else
        {
            text += "100";
        }
    }

    return text;
}

struct Measured
{
    double  ns_per_call = 0.0;
    int64_t callbacks   = 0;
};

template <typename Fn>
Measured measure(int64_t iterations, Fn&& body, const int64_t& callback_count)
{
    const int64_t before_callbacks = callback_count;
    const auto    start            = std::chrono::steady_clock::now();

    for (int64_t index = 0; index < iterations; ++index)
    {
        body(index);
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    Measured   result;
    result.ns_per_call = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
                         static_cast<double>(iterations);
    result.callbacks = callback_count - before_callbacks;
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    const int64_t iterations = bench::argument_int64(argc, argv, "--iters", 1'000'000);

    KisConfig config;
    config.is_paper = true;
    KisWebSocket websocket(config);

    int64_t trade_callbacks     = 0;
    int64_t orderbook_callbacks = 0;
    int64_t sink                = 0;

    websocket.set_callbacks(
        [&](const OrderBook& book)
        {
            ++orderbook_callbacks;
            sink += static_cast<int64_t>(book.bids[0].price);
        },
        [&](const TradeData& trade)
        {
            ++trade_callbacks;
            sink += static_cast<int64_t>(trade.price);
        });

    const std::vector<std::string> tickers = {"005930", "000660", "035420", "051910"};
    std::vector<std::string>       trade_frames;
    std::vector<std::string>       orderbook_frames;

    for (size_t index = 0; index < tickers.size(); ++index)
    {
        trade_frames.push_back("0|H0STCNT0|001|" + make_trade_payload(tickers[index], 71200 + static_cast<int>(index) * 100));
        orderbook_frames.push_back("0|H0STASP0|001|" + make_orderbook_payload(tickers[index], 71000 + static_cast<int>(index) * 100));
    }

    // 예열 — 첫 호출의 할당·캐시 적재를 측정에서 뺀다.
    for (int index = 0; index < 10'000; ++index)
    {
        websocket.parse_message(trade_frames[static_cast<size_t>(index) % trade_frames.size()]);
        websocket.parse_message(orderbook_frames[static_cast<size_t>(index) % orderbook_frames.size()]);
    }

    const Measured trade = measure(
        iterations,
        [&](int64_t index)
        {
            websocket.parse_message(trade_frames[static_cast<size_t>(index) % trade_frames.size()]);
        },
        trade_callbacks);

    const Measured orderbook = measure(
        iterations,
        [&](int64_t index)
        {
            websocket.parse_message(orderbook_frames[static_cast<size_t>(index) % orderbook_frames.size()]);
        },
        orderbook_callbacks);

    // 한 건씩 시계를 재서 분위수도 낸다(시계 호출 자체가 20ns 안팎 더해진다).
    std::vector<int64_t> samples;
    samples.reserve(static_cast<size_t>(iterations / 4));

    for (int64_t index = 0; index < iterations / 4; ++index)
    {
        const auto start = std::chrono::steady_clock::now();
        websocket.parse_message(trade_frames[static_cast<size_t>(index) % trade_frames.size()]);
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }

    const bench::PercentileSummary summary = bench::percentiles(samples);

    std::printf("build            : %s\n", bench::build_type());
    std::printf("iterations       : %lld per kind\n", static_cast<long long>(iterations));
    std::printf("trade  (H0STCNT0): %.1f ns/call  callbacks=%lld\n", trade.ns_per_call, static_cast<long long>(trade.callbacks));
    std::printf("book   (H0STASP0): %.1f ns/call  callbacks=%lld\n", orderbook.ns_per_call, static_cast<long long>(orderbook.callbacks));
    std::printf("trade per-call   : p50=%s p99=%s max=%s (n=%zu)\n", bench::format_ns(summary.p50).c_str(),
                bench::format_ns(summary.p99).c_str(), bench::format_ns(summary.max_value).c_str(), samples.size());
    std::printf("sink             : %lld\n", static_cast<long long>(sink));
    return 0;
}
