// yuanta::YuantaFeed 단위 테스트 — 체결 전문 읽기, 구독 목록 밖 종목 거르기, 구독 추가·해지, 깨진 전문 버리기.
// 소켓은 열지 않는다(start_receive_thread=false) — ingest()로 바이트를 직접 먹인다.
// 빌드: cmake --build <directory> --target test_yuanta_feed
#include "api/YuantaFeed.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        ++g_checks;                                                                             \
        if (!(condition))                                                                       \
        {                                                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                           \
        }                                                                                       \
    } while (false)

yuanta::TradeWireRecord make_record(const char* code, int64_t price_krw, int64_t quantity, int32_t direction)
{
    yuanta::TradeWireRecord record;
    std::memcpy(record.code, code, std::strlen(code));
    record.hhmmss             = 93001;
    record.direction          = direction;
    record.price_krw          = price_krw;
    record.quantity           = quantity;
    record.accumulated_volume = 123456;
    record.strength           = 105.5;

    return record;
}

// 파이썬 연결 프로그램이 struct.pack으로 만드는 것과 같은 바이트를 만든다.
std::vector<unsigned char> make_message(const std::vector<yuanta::TradeWireRecord>& records)
{
    yuanta::TradeWireHeader header;
    header.record_count = static_cast<uint16_t>(records.size());

    std::vector<unsigned char> bytes(yuanta::kTradeWireHeaderBytes + records.size() * yuanta::kTradeWireRecordBytes);
    std::memcpy(bytes.data(), &header, yuanta::kTradeWireHeaderBytes);

    for (size_t index = 0; index < records.size(); ++index)
    {
        std::memcpy(bytes.data() + yuanta::kTradeWireHeaderBytes + index * yuanta::kTradeWireRecordBytes, &records[index],
                    yuanta::kTradeWireRecordBytes);
    }

    return bytes;
}

WatchSpec make_specification(const char* ticker)
{
    WatchSpec specification;
    specification.ticker = ticker;

    return specification;
}

} // namespace

int main()
{
    yuanta::YuantaFeed::Options options;
    options.start_receive_thread = false;
    yuanta::YuantaFeed feed(options);

    std::vector<TradeData> received;
    feed.set_lane_callbacks(nullptr,
                            [&received](uint32_t lane, TradeData& trade)
                            {
                                if (lane == 0)
                                {
                                    received.push_back(trade);
                                }
                            });

    CHECK(!feed.owns_fill_notice());
    CHECK(feed.free_slots() == -1);
    CHECK(feed.connect({make_specification("005930")}));
    CHECK(feed.is_connected());
    CHECK(!feed.is_stale(1));

    // ── 구독 목록 안의 종목만 올라온다 ──────────────────────────────────────
    {
        const auto message = make_message({make_record("005930", 71000, 10, 1), make_record("000660", 180000, 3, 5)});
        feed.ingest(message.data(), message.size());

        CHECK(received.size() == 1);
        CHECK(received[0].ticker.view() == "005930");
        CHECK(received[0].hhmmss == 93001);
        CHECK(received[0].price == 71000.0);
        CHECK(received[0].quantity == 10);
        CHECK(received[0].direction == 1);
        CHECK(received[0].accumulated_volume == 123456);
        CHECK(received[0].strength == 105.5);
        CHECK(received[0].market == Market::KR);
        CHECK(received[0].received_ns != 0);

        const auto statistics = feed.statistics();
        CHECK(statistics.batches == 1);
        CHECK(statistics.records == 2);
        CHECK(statistics.published == 1);
    }

    // ── 구독을 더하면 그 종목도 올라오고, 풀면 다시 접힌다 ──────────────────
    {
        received.clear();
        CHECK(feed.subscribe_incremental(make_specification("000660")));
        CHECK(feed.has_specification(make_specification("000660")));

        const auto message = make_message({make_record("000660", 180000, 3, 5)});
        feed.ingest(message.data(), message.size());
        CHECK(received.size() == 1);
        CHECK(received[0].ticker.view() == "000660");
        CHECK(received[0].direction == 5);

        received.clear();
        CHECK(feed.unsubscribe_incremental(make_specification("000660")));
        CHECK(!feed.unsubscribe_incremental(make_specification("000660")));
        CHECK(!feed.has_specification(make_specification("000660")));
        feed.ingest(message.data(), message.size());
        CHECK(received.empty());
    }

    // ── 깨진 전문은 통째로 버린다 ───────────────────────────────────────────
    {
        received.clear();
        auto message = make_message({make_record("005930", 71000, 10, 1)});

        // 판번호가 다르다
        auto wrong_version = message;
        wrong_version[4] = 99;
        feed.ingest(wrong_version.data(), wrong_version.size());

        // 크기가 건수와 안 맞는다
        auto truncated = message;
        truncated.pop_back();
        feed.ingest(truncated.data(), truncated.size());

        // 앞머리보다 짧다
        feed.ingest(message.data(), yuanta::kTradeWireHeaderBytes - 1);

        CHECK(received.empty());
        CHECK(feed.statistics().malformed == 3);
    }

    feed.disconnect();
    CHECK(!feed.is_connected());

    std::cout << "test_yuanta_feed: " << g_checks << " checks passed\n";
    return 0;
}
