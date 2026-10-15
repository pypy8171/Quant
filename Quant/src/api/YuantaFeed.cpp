// YuantaFeed.h 구현 — 수신 스레드·전문 해석·구독 종목 거르기. [why D-163]
#include "api/YuantaFeed.h"

#include "core/LatencyTrace.h"
#include "utils/Logger.h"
#include "utils/ThreadName.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>

#ifdef HAS_ZMQ
#include <zmq.hpp>
#endif

namespace yuanta
{
namespace
{
// 소켓에 아무것도 없을 때 기다리는 시간. 이 주기로 멈춤 신호(running_)를 본다.
constexpr int kPollTimeoutMilliseconds = 100;

// 한 번에 연달아 꺼내는 전문 통 수(CODE_CONVENTIONS 4.3).
constexpr int kMaxMessagesPerPoll = 256;

// 받는 쪽 대기 한도. PUB-SUB는 넘으면 버린다 — 시세는 최신이 중요하고, 연결 프로그램을 막으면 유안타 DLL의
//  이벤트 처리가 밀린다.
constexpr int kReceiveHighWaterMark = 100000;

constexpr int64_t kNanosecondsPerSecond = 1'000'000'000;

// 종목코드 칸에서 실제 글자 수. 0으로 채운 뒷자리를 뺀다.
std::string_view code_view(const TradeWireRecord& record)
{
    size_t length = 0;

    while (length < kTradeWireCodeBytes && record.code[length] != '\0')
    {
        ++length;
    }

    return {record.code, length};
}

// 종목코드를 구독 집합의 키로 바꾼다. 전문의 코드 칸과 같은 모양(8바이트, 뒤는 0)으로 채워 정수로 읽는다.
//  여덟 글자를 넘는 코드는 국내 주식에 없다 — 넘으면 잘려 다른 키가 되므로 0(어느 것과도 안 맞음)을 준다.
uint64_t pack_code(std::string_view code)
{
    if (code.size() > kTradeWireCodeBytes)
    {
        return 0;
    }

    char bytes[kTradeWireCodeBytes] = {};
    std::copy(code.begin(), code.end(), bytes);

    uint64_t key = 0;
    std::memcpy(&key, bytes, sizeof(key));

    return key;
}

static_assert(sizeof(uint64_t) == kTradeWireCodeBytes);

} // namespace

YuantaFeed::YuantaFeed(Options options) : options_(std::move(options)), ticker_set_(std::make_shared<const TickerSet>())
{
}

YuantaFeed::~YuantaFeed()
{
    disconnect();
}

void YuantaFeed::set_callbacks(OrderBookCb /*on_order_book*/, TradeCb on_trade)
{
    set_lane_callbacks(nullptr,
                       [callback = std::move(on_trade)](uint32_t, TradeData& trade)
                       {
                           if (callback)
                           {
                               callback(trade);
                           }
                       });
}

void YuantaFeed::set_lane_callbacks(LaneOrderBookCb /*on_order_book*/, LaneTradeCb on_trade)
{
    // 호가는 아직 안 받는다(AUTO12 배선은 다음 단계). 체결만 올린다.
    on_lane_trade_ = std::move(on_trade);
}

bool YuantaFeed::connect(const std::vector<WatchSpec>& specifications)
{
    if (running_.load(std::memory_order_acquire))
    {
        return false;
    }

    {
        const std::lock_guard lock(specifications_mutex_);
        specifications_ = specifications;
        publish_ticker_set();
    }

    last_message_ns_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

#ifdef HAS_ZMQ
    if (options_.start_receive_thread)
    {
        thread_ = std::thread(&YuantaFeed::receive_loop, this);
    }
#endif

    LOG_INFO("[YuantaFeed] 연결 프로그램 구독 시작 - endpoint(" + options_.endpoint + ") 종목(" +
             std::to_string(specifications.size()) + ")");

    return true;
}

void YuantaFeed::disconnect()
{
    if (!running_.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    if (thread_.joinable())
    {
        thread_.join();
    }
}

bool YuantaFeed::subscribe_incremental(const WatchSpec& specification)
{
    const std::lock_guard lock(specifications_mutex_);

    for (const WatchSpec& known : specifications_)
    {
        if (same_watch(known, specification))
        {
            return true;
        }
    }

    specifications_.push_back(specification);
    publish_ticker_set();

    return true;
}

bool YuantaFeed::unsubscribe_incremental(const WatchSpec& specification)
{
    const std::lock_guard lock(specifications_mutex_);

    for (auto iterator = specifications_.begin(); iterator != specifications_.end(); ++iterator)
    {
        if (same_watch(*iterator, specification))
        {
            specifications_.erase(iterator);
            publish_ticker_set();

            return true;
        }
    }

    return false;
}

bool YuantaFeed::has_specification(const WatchSpec& specification) const
{
    const std::lock_guard lock(specifications_mutex_);

    for (const WatchSpec& known : specifications_)
    {
        if (same_watch(known, specification))
        {
            return true;
        }
    }

    return false;
}

std::vector<WatchSpec> YuantaFeed::take_overflow_specifications()
{
    return {};
}

bool YuantaFeed::is_connected() const
{
    return running_.load(std::memory_order_acquire);
}

bool YuantaFeed::is_stale(int threshold_sec) const
{
    const int64_t last_ns = last_message_ns_.load(std::memory_order_relaxed);

    // 아직 한 통도 안 왔으면 늙었다고 하지 않는다 — 장 시작 전에는 체결이 없다. 연결 프로그램이 죽은 채 띄운 것은
    //  기동 로그의 받은 통 수로 본다.
    if (last_ns == 0)
    {
        return false;
    }

    const int64_t threshold_ns = static_cast<int64_t>(threshold_sec) * kNanosecondsPerSecond;

    return trace::now_ns() - last_ns > threshold_ns;
}

void YuantaFeed::ingest(const void* data, size_t size)
{
    process_message(data, size);
}

YuantaFeed::Statistics YuantaFeed::statistics() const
{
    Statistics statistics;
    statistics.batches   = batches_.load(std::memory_order_relaxed);
    statistics.records   = records_.load(std::memory_order_relaxed);
    statistics.published = published_.load(std::memory_order_relaxed);
    statistics.malformed = malformed_.load(std::memory_order_relaxed);

    return statistics;
}

void YuantaFeed::publish_ticker_set()
{
    // 집합을 새로 만들어 통째로 바꾼다 — 수신 스레드가 쥔 옛 집합은 그쪽이 놓을 때 사라진다.
    auto tickers = std::make_shared<TickerSet>();
    tickers->reserve(specifications_.size());

    for (const WatchSpec& specification : specifications_)
    {
        if (specification.market == Market::KR)
        {
            tickers->insert(pack_code(specification.ticker));
        }
    }

    ticker_set_ = std::move(tickers);
}

void YuantaFeed::process_message(const void* data, size_t size)
{
    std::shared_ptr<const TickerSet> tickers;

    {
        const std::lock_guard lock(specifications_mutex_);
        tickers = ticker_set_;
    }

    const int64_t received_ns = trace::now_ns();
    uint64_t      record_count = 0;
    uint64_t      published    = 0;

    const bool decoded = decode_trade_wire(data, size,
                                           [&](const TradeWireHeader& /*header*/, const TradeWireRecord& record)
                                           {
                                               ++record_count;
                                               uint64_t key = 0;
                                               std::memcpy(&key, record.code, sizeof(key));

                                               // 연결 프로그램은 전 종목을 보낸다. 구독 목록 밖은 여기서 접는다.
                                               if (!on_lane_trade_ || !tickers->contains(key))
                                               {
                                                   return;
                                               }

                                               TradeData trade;
                                               trade.ticker             = code_view(record);
                                               trade.hhmmss             = record.hhmmss;
                                               trade.price              = static_cast<double>(record.price_krw);
                                               trade.quantity           = record.quantity;
                                               trade.direction          = record.direction;
                                               trade.market             = Market::KR;
                                               trade.timestamp          = std::chrono::system_clock::now();
                                               trade.strength           = record.strength;
                                               trade.accumulated_volume = record.accumulated_volume;
                                               trade.received_ns        = received_ns;

                                               on_lane_trade_(0, trade);
                                               ++published;
                                           });

    if (!decoded)
    {
        malformed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    last_message_ns_.store(received_ns, std::memory_order_relaxed);
    batches_.fetch_add(1, std::memory_order_relaxed);
    records_.fetch_add(record_count, std::memory_order_relaxed);
    published_.fetch_add(published, std::memory_order_relaxed);
}

#ifdef HAS_ZMQ

void YuantaFeed::receive_loop()
{
    thread_name::set_current("YuantaFeedIn");

    zmq::context_t context{1};
    zmq::socket_t  socket{context, zmq::socket_type::sub};

    try
    {
        socket.set(zmq::sockopt::rcvhwm, kReceiveHighWaterMark);
        socket.set(zmq::sockopt::subscribe, "");
        socket.connect(options_.endpoint);
    }
    catch (const zmq::error_t& error)
    {
        LOG_WARN(std::string("[YuantaFeed] 소켓을 열지 못했다 - endpoint(") + options_.endpoint + ") 오류(" + error.what() + ")");
        return;
    }

    zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::message_t  message;

    while (running_.load(std::memory_order_acquire))
    {
        try
        {
            zmq::poll(items, 1, std::chrono::milliseconds{kPollTimeoutMilliseconds});
        }
        catch (const zmq::error_t& error)
        {
            LOG_WARN(std::string("[YuantaFeed] 수신 대기 실패로 멈춘다 - 오류(") + error.what() + ")");
            return;
        }

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        for (int taken = 0; taken < kMaxMessagesPerPoll; ++taken)
        {
            bool received = false;

            try
            {
                received = socket.recv(message, zmq::recv_flags::dontwait).has_value();
            }
            catch (const zmq::error_t& error)
            {
                LOG_WARN(std::string("[YuantaFeed] 수신 실패로 멈춘다 - 오류(") + error.what() + ")");
                return;
            }

            if (!received)
            {
                break;
            }

            process_message(message.data(), message.size());
        }
    }
}

#else

void YuantaFeed::receive_loop()
{
    // ZMQ 없이 빌드하면 소켓이 없다 — ingest()로 바이트를 직접 먹이는 길만 남는다.
}

#endif // HAS_ZMQ

} // namespace yuanta
