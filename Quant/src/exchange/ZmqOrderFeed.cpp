// 부하시험 주문 수신단 구현 — 배선 그림과 스레드 약속은 include/exchange/ZmqOrderFeed.h.
#include "exchange/ZmqOrderFeed.h"

#include "core/KstTime.h"
#include "utils/Logger.h"
#include "utils/Utf8.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <utility>

#ifdef HAS_ZMQ
#include <zmq.hpp>
#endif

namespace exchange
{
namespace
{
// 받는 쪽 대기 한도. 넘으면 PUSH 쪽이 막힌다 — 버리는 대신 막아야 인젝터가 낸 만큼이 곧 처리량이 된다.
constexpr int kReceiveHighWaterMark = 100000;

// 소켓에 아무것도 없을 때 기다리는 시간. 이 주기로 전략 주문 큐도 같이 비운다.
constexpr int kPollTimeoutMilliseconds = 2;

// 한 번에 연달아 꺼내는 전문 통 수. 이걸 안 두면 소켓이 계속 차 있는 동안 전략 주문이 밀린다.
constexpr int kMaxBatchesPerPoll = 256;

// 받은 쪽 처리량을 로그로 남기는 간격. 부하시험의 기준선은 보낸 쪽이 아니라 받아서 맞춘 쪽 숫자다.
constexpr long long kReportIntervalMilliseconds = 1000;
constexpr long long kMillisecondsPerSecond      = 1000;

// 하루를 초로 편 값 ↔ HHMMSS. 장중 시각을 흐른 초만큼 밀 때만 쓴다.
constexpr int32_t kSecondsPerHour   = 3600;
constexpr int32_t kSecondsPerMinute = 60;
constexpr int32_t kSecondsPerDay    = 24 * kSecondsPerHour;

int32_t hhmmss_to_second_of_day(int32_t hhmmss)
{
    return (hhmmss / 10000) * kSecondsPerHour + ((hhmmss / 100) % 100) * kSecondsPerMinute + (hhmmss % 100);
}

int32_t second_of_day_to_hhmmss(int32_t second_of_day)
{
    const int32_t wrapped = ((second_of_day % kSecondsPerDay) + kSecondsPerDay) % kSecondsPerDay;

    return (wrapped / kSecondsPerHour) * 10000 + ((wrapped % kSecondsPerHour) / kSecondsPerMinute) * 100 +
           (wrapped % kSecondsPerMinute);
}

int64_t steady_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr uint32_t kNoIndex = static_cast<uint32_t>(-1);

} // namespace

ZmqOrderFeed::Lane::Lane(size_t submit_queue_capacity) : submit_queue(submit_queue_capacity)
{
}

ZmqOrderFeed::ZmqOrderFeed(symbol::SymbolTable& symbols, Options options)
    : symbols_(symbols), options_(std::move(options))
{
    if (options_.lane_count == 0)
    {
        options_.lane_count = 1;
    }

    lanes_.reserve(options_.lane_count);

    for (uint32_t lane = 0; lane < options_.lane_count; ++lane)
    {
        lanes_.push_back(std::make_unique<Lane>(options_.submit_queue_capacity));
    }
}

ZmqOrderFeed::~ZmqOrderFeed()
{
    disconnect();
}

void ZmqOrderFeed::set_callbacks(OrderBookCb on_order_book, TradeCb on_trade)
{
    set_lane_callbacks([callback = std::move(on_order_book)](uint32_t, const OrderBook& book)
                       {
                           if (callback)
                           {
                               callback(book);
                           }
                       },
                       [callback = std::move(on_trade)](uint32_t, const TradeData& trade)
                       {
                           if (callback)
                           {
                               callback(trade);
                           }
                       });
}

void ZmqOrderFeed::set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade)
{
    on_lane_order_book_ = std::move(on_order_book);
    on_lane_trade_      = std::move(on_trade);
}

uint32_t ZmqOrderFeed::lane_of_symbol_index(uint32_t symbol_index) const
{
    return symbol_index % static_cast<uint32_t>(lanes_.size());
}

symbol::SymbolId ZmqOrderFeed::symbol_id_of_index(uint32_t symbol_index) const
{
    return symbol_index < index_to_symbol_id_.size() ? index_to_symbol_id_[symbol_index] : symbol::kNone;
}

void ZmqOrderFeed::write_universe_file() const
{
    if (options_.universe_out_path.empty())
    {
        return;
    }

    std::ofstream file(utf8::path_from_utf8(options_.universe_out_path), std::ios::binary | std::ios::trunc);

    if (!file)
    {
        LOG_ERROR("[ZmqOrderFeed] 종목 순번표를 적지 못했다: " + options_.universe_out_path);

        return;
    }

    // 손으로 적는다 — 순번이 곧 배열 자리라 json 라이브러리의 키 순서에 기대지 않는다.
    file << "{\n  \"schema\": 1,\n  \"lanes\": " << lanes_.size() << ",\n  \"base_port\": "
         << options_.base_port << ",\n  \"count\": " << specifications_.size() << ",\n  \"tickers\": [";

    for (size_t index = 0; index < specifications_.size(); ++index)
    {
        file << (index == 0 ? "\n    \"" : ",\n    \"") << specifications_[index].ticker << "\"";
    }

    file << "\n  ]\n}\n";
    LOG_INFO("[ZmqOrderFeed] 종목 순번표 " + std::to_string(specifications_.size()) + "개를 적었다: " +
             options_.universe_out_path);
}

bool ZmqOrderFeed::connect(const std::vector<WatchSpec>& specifications)
{
    if (running_.load(std::memory_order_acquire))
    {
        return false;
    }

    specifications_ = specifications;
    session_start_  = std::chrono::steady_clock::now();
    index_to_symbol_id_.clear();
    index_to_symbol_id_.reserve(specifications_.size());

    symbol::SymbolId highest_symbol_id = symbol::kNone;

    for (const WatchSpec& specification : specifications_)
    {
        const symbol::SymbolId symbol_id = symbols_.intern(specification.ticker);
        index_to_symbol_id_.push_back(symbol_id);

        if (symbol_id > highest_symbol_id)
        {
            highest_symbol_id = symbol_id;
        }
    }

    symbol_id_to_index_.assign(highest_symbol_id + 1, kNoIndex);

    for (size_t index = 0; index < index_to_symbol_id_.size(); ++index)
    {
        const symbol::SymbolId symbol_id = index_to_symbol_id_[index];

        if (symbol_id != symbol::kNone)
        {
            symbol_id_to_index_[symbol_id] = static_cast<uint32_t>(index);
        }
    }

    for (std::unique_ptr<Lane>& lane : lanes_)
    {
        lane->engine.reserve(highest_symbol_id);
    }

    write_universe_file();
    running_.store(true, std::memory_order_release);

#ifdef HAS_ZMQ
    if (options_.start_receive_threads)
    {
        for (uint32_t lane = 0; lane < lanes_.size(); ++lane)
        {
            lanes_[lane]->thread = std::thread(&ZmqOrderFeed::receive_loop, this, lane);
        }
    }
#endif

    return true;
}

void ZmqOrderFeed::disconnect()
{
    if (!running_.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    for (std::unique_ptr<Lane>& lane : lanes_)
    {
        if (lane->thread.joinable())
        {
            lane->thread.join();
        }
    }
}

bool ZmqOrderFeed::subscribe_incremental(const WatchSpec& specification)
{
    // 돌고 있는 동안 종목을 더하면 수신 스레드가 읽는 순번 표를 갈아 끼우게 된다 — 부하시험에 필요 없어 막는다.
    if (running_.load(std::memory_order_acquire))
    {
        return false;
    }

    specifications_.push_back(specification);

    return true;
}

bool ZmqOrderFeed::has_specification(const WatchSpec& specification) const
{
    for (const WatchSpec& known : specifications_)
    {
        if (known.ticker == specification.ticker && known.market == specification.market)
        {
            return true;
        }
    }

    return false;
}

std::vector<WatchSpec> ZmqOrderFeed::take_overflow_specifications()
{
    return {};
}

bool ZmqOrderFeed::is_connected() const
{
    return running_.load(std::memory_order_acquire);
}

bool ZmqOrderFeed::is_stale(int /*threshold_sec*/) const
{
    return false;
}

bool ZmqOrderFeed::submit(const IncomingOrder& order)
{
    const uint32_t symbol_index =
        order.symbol_id < symbol_id_to_index_.size() ? symbol_id_to_index_[order.symbol_id] : kNoIndex;

    if (symbol_index == kNoIndex)
    {
        return false;
    }

    Lane& lane = *lanes_[lane_of_symbol_index(symbol_index)];

    if (!lane.submit_queue.push(order))
    {
        lane.submit_dropped.fetch_add(1, std::memory_order_relaxed);

        return false;
    }

    lane.submitted.fetch_add(1, std::memory_order_relaxed);

    return true;
}

void ZmqOrderFeed::publish_execution(uint32_t lane, const Execution& execution, int direction, int32_t hhmmss_now)
{
    if (!on_lane_trade_)
    {
        return;
    }

    TradeData trade;
    trade.ticker      = symbols_.name(execution.symbol_id);
    trade.symbol_id   = execution.symbol_id;
    trade.hhmmss      = hhmmss_now;
    trade.price       = static_cast<double>(execution.price_krw);
    trade.quantity    = execution.quantity;
    trade.direction   = direction;
    trade.market      = Market::KR;
    trade.timestamp   = std::chrono::system_clock::now();
    trade.received_ns = steady_now_ns();

    on_lane_trade_(lane, trade);
}

void ZmqOrderFeed::publish_order_book(uint32_t lane, symbol::SymbolId symbol_id, int32_t hhmmss_now)
{
    if (!on_lane_order_book_)
    {
        return;
    }

    const SymbolBook* book = lanes_[lane]->engine.book_of(symbol_id);

    if (book == nullptr)
    {
        return;
    }

    // 최우선호가 한 단만 채운다 — 다섯 단을 다 만들려면 빈 레벨을 훑어야 하는데 여기는 체결마다 지나는 자리다.
    OrderBook order_book;
    order_book.ticker           = symbols_.name(symbol_id);
    order_book.symbol_id        = symbol_id;
    order_book.hhmmss           = hhmmss_now;
    order_book.bids[0].price    = static_cast<double>(book->best_bid_krw());
    order_book.asks[0].price    = static_cast<double>(book->best_ask_krw());
    order_book.timestamp        = std::chrono::system_clock::now();
    order_book.received_ns      = steady_now_ns();

    on_lane_order_book_(lane, order_book);
}

void ZmqOrderFeed::process_record(uint32_t lane, const OrderWireRecord& record, int32_t hhmmss_now)
{
    Lane&                  own       = *lanes_[lane];
    const symbol::SymbolId symbol_id = symbol_id_of_index(record.symbol_index);

    if (symbol_id == symbol::kNone)
    {
        own.rejected.fetch_add(1, std::memory_order_relaxed);

        return;
    }

    const WireCommand command = static_cast<WireCommand>(record.command);

    if (command == WireCommand::CONFIGURE)
    {
        own.engine.configure_symbol(symbol_id, record.price_krw);

        return;
    }

    if (command == WireCommand::RUN_AUCTION)
    {
        own.engine.set_execution_callback(
            [&](const Execution& execution)
            {
                own.executions.fetch_add(1, std::memory_order_relaxed);
                publish_execution(lane, execution, 1, hhmmss_now);
            });
        own.engine.run_auction(symbol_id);
        own.engine.set_execution_callback(nullptr);

        if (options_.publish_order_book)
        {
            publish_order_book(lane, symbol_id, hhmmss_now);
        }

        return;
    }

    const IncomingOrder order = to_incoming_order(record, symbol_id);

    if (command == WireCommand::ACCUMULATE)
    {
        if (!own.engine.accumulate(order))
        {
            own.rejected.fetch_add(1, std::memory_order_relaxed);
        }

        return;
    }

    const int direction = order.side == OrderSide::SELL ? 5 : 1;
    own.engine.set_execution_callback(
        [&](const Execution& execution)
        {
            own.executions.fetch_add(1, std::memory_order_relaxed);
            publish_execution(lane, execution, direction, hhmmss_now);
        });

    const int64_t matched = own.engine.match(order);
    own.engine.set_execution_callback(nullptr);

    if (matched > 0 && options_.publish_order_book)
    {
        publish_order_book(lane, symbol_id, hhmmss_now);
    }
}

void ZmqOrderFeed::process_batch(uint32_t lane, const OrderWireBatch& batch)
{
    Lane& own = *lanes_[lane];
    own.batches.fetch_add(1, std::memory_order_relaxed);
    own.records.fetch_add(batch.count, std::memory_order_relaxed);

    // 시각은 통마다 한 번만 읽는다 — 건마다 읽으면 그것이 곧 부하가 된다.
    const int32_t hhmmss_now = market_hhmmss();

    for (size_t index = 0; index < batch.count; ++index)
    {
        process_record(lane, batch.records[index], hhmmss_now);
    }
}

void ZmqOrderFeed::drain_submit_queue(uint32_t lane, int32_t hhmmss_now)
{
    Lane& own = *lanes_[lane];

    while (true)
    {
        std::optional<IncomingOrder> order = own.submit_queue.pop();

        if (!order.has_value())
        {
            return;
        }

        const int direction = order->side == OrderSide::SELL ? 5 : 1;
        own.engine.set_execution_callback(
            [&](const Execution& execution)
            {
                own.executions.fetch_add(1, std::memory_order_relaxed);
                publish_execution(lane, execution, direction, hhmmss_now);
            });

        const int64_t matched = own.engine.match(*order);
        own.engine.set_execution_callback(nullptr);

        if (matched > 0 && options_.publish_order_book)
        {
            publish_order_book(lane, order->symbol_id, hhmmss_now);
        }
    }
}

void ZmqOrderFeed::ingest(uint32_t lane, const void* data, size_t size)
{
    if (lane >= lanes_.size())
    {
        return;
    }

    // 수신 스레드가 도는 차례와 같게 둔다 — 전략이 넣어 둔 주문을 먼저 비우고 전문을 처리한다.
    drain_submit_queue(lane, market_hhmmss());

    OrderWireBatch batch;

    if (!decode_order_wire(data, size, batch))
    {
        return;
    }

    process_batch(lane, batch);
}

ZmqOrderFeed::Statistics ZmqOrderFeed::statistics() const
{
    Statistics total;

    for (const std::unique_ptr<Lane>& lane : lanes_)
    {
        total.batches += lane->batches.load(std::memory_order_relaxed);
        total.records += lane->records.load(std::memory_order_relaxed);
        total.executions += lane->executions.load(std::memory_order_relaxed);
        total.rejected += lane->rejected.load(std::memory_order_relaxed);
        total.submitted += lane->submitted.load(std::memory_order_relaxed);
        total.submit_dropped += lane->submit_dropped.load(std::memory_order_relaxed);
    }

    return total;
}

// 장 시각. ZMQ 없이 빌드해도 ingest()가 부르므로 가드 밖에 둔다.
int32_t ZmqOrderFeed::market_hhmmss() const
{
    if (options_.session_start_hhmmss == 0)
    {
        return kst::hhmmss_int(std::time(nullptr));
    }

    const int64_t elapsed_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - session_start_).count();

    return second_of_day_to_hhmmss(hhmmss_to_second_of_day(options_.session_start_hhmmss) +
                                   static_cast<int32_t>(elapsed_seconds));
}

#ifdef HAS_ZMQ

void ZmqOrderFeed::report_throughput(uint32_t lane, std::chrono::steady_clock::time_point& last_report,
                                     uint64_t& last_records, uint64_t& last_executions)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const long long elapsed_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();

    if (elapsed_milliseconds < kReportIntervalMilliseconds)
    {
        return;
    }

    const Lane&    counters      = *lanes_[lane];
    const uint64_t records_now   = counters.records.load(std::memory_order_relaxed);
    const uint64_t executions_now = counters.executions.load(std::memory_order_relaxed);

    // 조용한 초는 안 적는다 — 시험 전후로 로그가 한없이 길어진다.
    if (records_now != last_records)
    {
        const long long divisor = elapsed_milliseconds;
        LOG_INFO("[부하시험 수신 " + std::to_string(lane) + "] 초당 주문 " +
                 std::to_string(static_cast<long long>(records_now - last_records) * kMillisecondsPerSecond / divisor) +
                 "건, 초당 체결 " +
                 std::to_string(static_cast<long long>(executions_now - last_executions) * kMillisecondsPerSecond / divisor) +
                 "건 (누적 주문 " + std::to_string(records_now) + "건, 체결 " + std::to_string(executions_now) +
                 "건, 버린 것 " + std::to_string(counters.rejected.load(std::memory_order_relaxed)) + "건)");
    }

    last_report     = now;
    last_records    = records_now;
    last_executions = executions_now;
}

void ZmqOrderFeed::receive_loop(uint32_t lane)
{
    const std::string endpoint = options_.bind_address + ":" + std::to_string(options_.base_port + static_cast<int>(lane));

    zmq::context_t context{1};
    zmq::socket_t  socket{context, zmq::socket_type::pull};

    try
    {
        socket.set(zmq::sockopt::rcvhwm, kReceiveHighWaterMark);
        socket.bind(endpoint);
    }
    catch (const zmq::error_t&)
    {
        return;
    }

    zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::message_t  message;

    std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
    uint64_t                              last_records    = 0;
    uint64_t                              last_executions = 0;

    while (running_.load(std::memory_order_acquire))
    {
        const int32_t hhmmss_now = market_hhmmss();
        drain_submit_queue(lane, hhmmss_now);
        report_throughput(lane, last_report, last_records, last_executions);

        try
        {
            zmq::poll(items, 1, std::chrono::milliseconds{kPollTimeoutMilliseconds});
        }
        catch (const zmq::error_t&)
        {
            return;
        }

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        for (int taken = 0; taken < kMaxBatchesPerPoll; ++taken)
        {
            bool received = false;

            try
            {
                received = socket.recv(message, zmq::recv_flags::dontwait).has_value();
            }
            catch (const zmq::error_t&)
            {
                return;
            }

            if (!received)
            {
                break;
            }

            OrderWireBatch batch;

            if (decode_order_wire(message.data(), message.size(), batch))
            {
                process_batch(lane, batch);
            }
        }
    }
}

#else

void ZmqOrderFeed::receive_loop(uint32_t /*lane*/)
{
    // ZMQ 없이 빌드하면 소켓이 없다 — ingest()로 바이트를 직접 먹이는 길만 남는다.
}

#endif // HAS_ZMQ

} // namespace exchange
