// exchange::OrderWire·ZmqOrderFeed 단위 테스트 — 전문 읽기·적기, 종목 순번 ↔ 종목 id, 동시호가 적재 →
//  단일가 체결이 TradeData로 올라오는 것, 전략 주문(submit)이 같은 오더북으로 들어가는 것.
// 소켓은 열지 않는다(start_receive_threads=false) — ingest()로 바이트를 직접 먹인다.
// 빌드: cmake --build <directory> --target test_zmq_order_feed
#include "exchange/ZmqOrderFeed.h"

#include <cstdint>
#include <iostream>
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

constexpr exchange::PriceKrw kReferencePrice = 10000;

exchange::OrderWireRecord make_record(uint64_t order_id, uint32_t symbol_index, exchange::PriceKrw price_krw,
                                      int32_t quantity, OrderSide side, exchange::WireCommand command)
{
    exchange::OrderWireRecord record;
    record.order_id     = order_id;
    record.symbol_index = symbol_index;
    record.price_krw    = price_krw;
    record.quantity     = quantity;
    record.side         = static_cast<uint8_t>(static_cast<OrderSide::Value>(side));
    record.command      = static_cast<uint8_t>(command);

    return record;
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
    // ── 전문 읽기·적기 ───────────────────────────────────────────────────────
    {
        std::vector<exchange::OrderWireRecord> records;
        records.push_back(make_record(1, 0, kReferencePrice, 10, OrderSide::BUY, exchange::WireCommand::MATCH));
        records.push_back(make_record(2, 1, kReferencePrice, 20, OrderSide::SELL, exchange::WireCommand::MATCH));

        uint8_t      buffer[256] = {0};
        const size_t bytes = exchange::encode_order_wire(buffer, sizeof(buffer), records.data(), records.size(), 77);

        CHECK(bytes == exchange::kOrderWireHeaderBytes + 2 * exchange::kOrderWireRecordBytes);

        exchange::OrderWireBatch batch;
        CHECK(exchange::decode_order_wire(buffer, bytes, batch));
        CHECK(batch.count == 2);
        CHECK(batch.header.sent_unix_ns == 77);
        CHECK(batch.records[1].order_id == 2);
        CHECK(batch.records[1].quantity == 20);

        // 자리가 모자라면 아무것도 적지 않는다.
        CHECK(exchange::encode_order_wire(buffer, exchange::kOrderWireHeaderBytes, records.data(), records.size(), 0) ==
              0);

        // 매직이 틀리거나 길이가 안 떨어지면 통째로 버린다.
        uint8_t broken[256] = {0};
        std::memcpy(broken, buffer, bytes);
        broken[0] = 0;
        CHECK(!exchange::decode_order_wire(broken, bytes, batch));
        CHECK(!exchange::decode_order_wire(buffer, bytes - 1, batch));
        CHECK(!exchange::decode_order_wire(buffer, exchange::kOrderWireHeaderBytes - 1, batch));
    }

    // ── 종목 순번 ↔ 종목 id ──────────────────────────────────────────────────
    symbol::SymbolTable            symbols;
    exchange::ZmqOrderFeed::Options options;
    options.lane_count            = 1;
    options.start_receive_threads = false;

    exchange::ZmqOrderFeed feed(symbols, options);

    std::vector<TradeData> trades;
    std::vector<OrderBook> order_books;
    feed.set_lane_callbacks([&](uint32_t, const OrderBook& order_book)
    {
        order_books.push_back(order_book);
    },
                            [&](uint32_t, const TradeData& trade)
                            {
                                trades.push_back(trade);
                            });

    std::vector<WatchSpec> specifications;
    specifications.push_back(make_specification("005930"));
    specifications.push_back(make_specification("000660"));

    CHECK(feed.connect(specifications));
    CHECK(feed.is_connected());
    CHECK(feed.lanes() == 1);
    CHECK(!feed.is_stale(1));

    const symbol::SymbolId first_symbol  = symbols.lookup("005930");
    const symbol::SymbolId second_symbol = symbols.lookup("000660");

    CHECK(first_symbol != symbol::kNone);
    CHECK(feed.symbol_id_of_index(0) == first_symbol);
    CHECK(feed.symbol_id_of_index(1) == second_symbol);
    CHECK(feed.symbol_id_of_index(2) == symbol::kNone);
    CHECK(feed.lane_of_symbol_index(0) == 0);
    CHECK(feed.lane_of_symbol_index(1) == 0);

    // ── 동시호가 적재 → 단일가 체결이 TradeData로 올라온다 ───────────────────
    {
        std::vector<exchange::OrderWireRecord> records;
        records.push_back(make_record(0, 0, kReferencePrice, 0, OrderSide::BUY, exchange::WireCommand::CONFIGURE));
        records.push_back(make_record(1, 0, 10100, 100, OrderSide::BUY, exchange::WireCommand::ACCUMULATE));
        records.push_back(make_record(2, 0, 9900, 100, OrderSide::SELL, exchange::WireCommand::ACCUMULATE));
        records.push_back(make_record(3, 0, 0, 0, OrderSide::BUY, exchange::WireCommand::RUN_AUCTION));

        uint8_t      buffer[512] = {0};
        const size_t bytes = exchange::encode_order_wire(buffer, sizeof(buffer), records.data(), records.size(), 0);
        feed.ingest(0, buffer, bytes);

        CHECK(trades.size() == 1);
        CHECK(trades[0].symbol_id == first_symbol);
        CHECK(trades[0].ticker == "005930");
        CHECK(trades[0].price == static_cast<double>(kReferencePrice));
        CHECK(trades[0].quantity == 100);
        CHECK(trades[0].received_ns > 0);

        const exchange::ZmqOrderFeed::Statistics statistics = feed.statistics();
        CHECK(statistics.batches == 1);
        CHECK(statistics.records == records.size());
        CHECK(statistics.executions == 1);
        CHECK(statistics.rejected == 0);
    }

    // ── 격자를 안 만든 종목·모르는 순번은 버린다 ─────────────────────────────
    {
        std::vector<exchange::OrderWireRecord> records;
        records.push_back(make_record(4, 1, kReferencePrice, 10, OrderSide::BUY, exchange::WireCommand::ACCUMULATE));
        records.push_back(make_record(5, 9, kReferencePrice, 10, OrderSide::BUY, exchange::WireCommand::ACCUMULATE));

        uint8_t      buffer[256] = {0};
        const size_t bytes = exchange::encode_order_wire(buffer, sizeof(buffer), records.data(), records.size(), 0);
        feed.ingest(0, buffer, bytes);

        CHECK(feed.statistics().rejected == 2);
    }

    // ── 전략이 낸 주문이 같은 오더북으로 들어간다 ────────────────────────────
    {
        trades.clear();

        std::vector<exchange::OrderWireRecord> setup;
        setup.push_back(make_record(0, 1, kReferencePrice, 0, OrderSide::BUY, exchange::WireCommand::CONFIGURE));

        uint8_t      setup_buffer[128] = {0};
        const size_t setup_bytes =
            exchange::encode_order_wire(setup_buffer, sizeof(setup_buffer), setup.data(), setup.size(), 0);
        feed.ingest(0, setup_buffer, setup_bytes);

        exchange::IncomingOrder resting;
        resting.order_id  = 1000;
        resting.symbol_id = second_symbol;
        resting.price_krw = 10050;
        resting.quantity  = 10;
        resting.side      = OrderSide::SELL;

        CHECK(feed.submit(resting));
        CHECK(feed.statistics().submitted == 1);

        // 모르는 종목은 큐에 넣지 않는다.
        exchange::IncomingOrder unknown = resting;
        unknown.symbol_id               = 9999;
        CHECK(!feed.submit(unknown));

        // 다음 ingest가 전략 주문을 먼저 비우고 전문을 처리한다 — 두 주문이 같은 오더북에서 맞는다.
        std::vector<exchange::OrderWireRecord> records;
        records.push_back(make_record(6, 1, 10050, 10, OrderSide::BUY, exchange::WireCommand::MATCH));

        uint8_t      buffer[128] = {0};
        const size_t bytes = exchange::encode_order_wire(buffer, sizeof(buffer), records.data(), records.size(), 0);
        feed.ingest(0, buffer, bytes);

        CHECK(trades.size() == 1);
        CHECK(trades[0].symbol_id == second_symbol);
        CHECK(trades[0].price == 10050.0);
        CHECK(trades[0].quantity == 10);
        CHECK(trades[0].direction == 1);
    }

    feed.disconnect();
    CHECK(!feed.is_connected());

    std::cout << "OK test_zmq_order_feed  checks=" << g_checks << "\n";

    return 0;
}
