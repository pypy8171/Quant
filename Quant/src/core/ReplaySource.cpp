#include "core/ReplaySource.h"
#include "utils/ThreadName.h"

namespace feed
{
ReplaySource::~ReplaySource()
{
    disconnect();
}

bool ReplaySource::connect(const std::vector<WatchSpec>& specifications)
{
    disconnect();

    {
        std::lock_guard<std::mutex> lock(filter_mutex_);

        for (const auto& specification : specifications)
        {
            filter_.insert(specification.ticker);
        }
    }

    auto reader = std::make_unique<TickReader>(file_);

    if (!reader->ok())
    {
        return false;
    }

    finished_.store(false, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_release);
    thread_ = std::jthread([this, reader = std::move(reader)](std::stop_token stop_token) mutable
                           { run(stop_token, *reader); });
    return true;
}

void ReplaySource::disconnect()
{
    if (thread_.joinable())
    {
        thread_.request_stop();
        thread_.join();
    }

    connected_.store(false, std::memory_order_release);
}

void ReplaySource::run(std::stop_token stop_token, TickReader& reader)
{
    thread_name::set_current("Replay");

    Record record;
    int64_t previous_ns = 0;

    while (!stop_token.stop_requested() && reader.next(record))
    {
        // 봉 시드·유니버스는 재생할 시세가 아니다. 기동 재현에 쓰는 것은 큐 34 ③에서 붙인다. [why D-071]
        if (record.kind != kKindTrade && record.kind != kKindBook)
        {
            continue;
        }

        const Common& common = record.kind == kKindTrade ? record.trade.common : record.book.common;

        if (!pass(common.ticker))
        {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (speed_ > 0.0 && previous_ns != 0 && common.received_ns > previous_ns)
        {
            pace(stop_token, static_cast<int64_t>(static_cast<double>(common.received_ns - previous_ns) / speed_));
        }

        previous_ns = common.received_ns;

        if (stop_token.stop_requested())
        {
            break;
        }

        // 캡처 때 received_ns는 위 간격 계산에만 쓰고, 내보내는 틱에는 이 프로세스 시계를 찍는다 — 구간 지연 CSV가
        //  옛 시계와 지금 시계를 빼는 일이 없게. [why D-071]
        if (record.kind == kKindTrade)
        {
            if (on_trade_)
            {
                TradeData trade = to_trade(record.trade);
                trade.received_ns = now_ns();
                on_trade_(trade);
            }
        }
        else if (on_order_book_)
        {
            OrderBook order_book = to_book(record.book);
            order_book.received_ns = now_ns();
            on_order_book_(order_book);
        }

        played_.fetch_add(1, std::memory_order_relaxed);
    }

    // 정지 요청으로 나온 것은 "끝까지 재생"이 아니다.
    finished_.store(!stop_token.stop_requested(), std::memory_order_release);
    connected_.store(false, std::memory_order_release);
}

void ReplaySource::pace(const std::stop_token& stop_token, int64_t wait_ns)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + nanoseconds(wait_ns);

    while (!stop_token.stop_requested())
    {
        const auto left = deadline - steady_clock::now();

        if (left <= milliseconds(2))
        {
            break;
        }

        std::this_thread::sleep_for(left > milliseconds(50) ? milliseconds(50) : duration_cast<milliseconds>(left));
    }
}

int64_t ReplaySource::now_ns()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace feed
