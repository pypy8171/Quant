#include "ipc/MarketFeedChannel.h"

namespace ipc
{

namespace
{

// 큐 하나가 끝나는 자리를 캐시라인 경계까지 올린다 — 다음 큐의 머리가 경계에서 시작해야 붙는다.
[[nodiscard]] size_t align_up(size_t bytes) noexcept
{
    return (bytes + kSharedCacheLine - 1) / kSharedCacheLine * kSharedCacheLine;
}

[[nodiscard]] size_t trade_span(size_t capacity) noexcept
{
    return align_up(SharedSpscRing<TradeData>::bytes_for(capacity));
}

[[nodiscard]] size_t order_book_span(size_t capacity) noexcept
{
    return align_up(SharedSpscRing<OrderBook>::bytes_for(capacity));
}

// 종목 코드 칸이 칸 안에서 끝나는가. 길이가 칸을 넘으면 view()가 칸 밖을 읽는다.
[[nodiscard]] bool ticker_fits(const symbol::Ticker& ticker) noexcept
{
    return ticker.length <= symbol::Ticker::kMax;
}

// 하루의 마지막 시각(23시 59분 59초)을 시각 정수로 적은 것.
constexpr int32_t kLastTimeOfDay = 235959;

// KST 시각 정수(093001 → 93001). 0은 "모름"이라 받는다.
[[nodiscard]] bool hhmmss_fits(int32_t hhmmss) noexcept
{
    return hhmmss >= 0 && hhmmss <= kLastTimeOfDay;
}

[[nodiscard]] bool market_fits(Market market) noexcept
{
    return market == Market::KR || market == Market::US;
}

// 호가 다섯 단계가 말이 되는가. 빈 자리는 0이다 — 상한가 잔량 없음·시간외처럼 한쪽이 비는 때가 있다.
[[nodiscard]] bool levels_fit(const OrderBookLevel (&levels)[5], const MarketLimits& limits) noexcept
{
    for (const OrderBookLevel& level : levels)
    {
        if (!(level.price >= 0.0) || level.price > limits.price_max)
        {
            return false;
        }

        if (level.quantity < 0 || level.quantity > limits.quantity_max)
        {
            return false;
        }
    }

    return true;
}

} // namespace

bool is_plausible(const TradeData& trade, const MarketLimits& limits) noexcept
{
    if (trade.symbol_id == symbol::kNone || trade.symbol_id > limits.symbol_count)
    {
        return false;
    }

    if (!ticker_fits(trade.ticker) || !hhmmss_fits(trade.hhmmss) || !market_fits(trade.market))
    {
        return false;
    }

    // 체결가는 0이 아니다. 음수·NaN·무한대는 비교가 전부 거짓이라 뒤집어 본다.
    if (!(trade.price > 0.0) || trade.price > limits.price_max)
    {
        return false;
    }

    if (trade.quantity <= 0 || trade.quantity > limits.quantity_max)
    {
        return false;
    }

    // 방향은 매수 1·매도 5뿐이고, 0은 "안 찍음"(REST 대체 틱·미국 틱)이라 받는다.
    if (trade.direction != 0 && trade.direction != 1 && trade.direction != 5)
    {
        return false;
    }

    if (!(trade.strength >= 0.0) || trade.accumulated_volume < 0 || trade.received_ns < 0)
    {
        return false;
    }

    return true;
}

bool is_plausible(const OrderBook& order_book, const MarketLimits& limits) noexcept
{
    if (order_book.symbol_id == symbol::kNone || order_book.symbol_id > limits.symbol_count)
    {
        return false;
    }

    if (!ticker_fits(order_book.ticker) || !hhmmss_fits(order_book.hhmmss) || order_book.received_ns < 0)
    {
        return false;
    }

    return levels_fit(order_book.asks, limits) && levels_fit(order_book.bids, limits);
}

size_t MarketFeedChannel::bytes_for(uint32_t lanes, size_t trade_capacity, size_t order_book_capacity)
{
    return lanes * (trade_span(trade_capacity) + order_book_span(order_book_capacity));
}

bool MarketFeedChannel::check_arguments(const std::byte* base, size_t bytes, uint32_t lanes, size_t trade_capacity,
                                        size_t order_book_capacity)
{
    last_error_.clear();

    if (base == nullptr)
    {
        last_error_ = "통로를 놓을 자리가 없다";
        return false;
    }

    if (lanes == 0 || lanes > kMaxFeedLanes)
    {
        last_error_ = "줄 수가 1과 " + std::to_string(kMaxFeedLanes) + " 사이가 아니다 — 받은 것=" +
                      std::to_string(lanes);
        return false;
    }

    const size_t needed = bytes_for(lanes, trade_capacity, order_book_capacity);

    if (bytes < needed)
    {
        last_error_ = "구역이 통로보다 작다 — 필요=" + std::to_string(needed) + " 받은 것=" + std::to_string(bytes);
        return false;
    }

    if (reinterpret_cast<uintptr_t>(base) % kSharedCacheLine != 0)
    {
        last_error_ = "통로가 캐시라인 경계에서 시작하지 않는다";
        return false;
    }

    return true;
}

bool MarketFeedChannel::bind(std::byte* base, uint32_t lanes, size_t trade_capacity, size_t order_book_capacity,
                             bool as_owner)
{
    std::vector<SharedSpscRing<TradeData>> trades(lanes);
    std::vector<SharedSpscRing<OrderBook>> order_books(lanes);

    const size_t trade_bytes = trade_span(trade_capacity);
    const size_t book_bytes  = order_book_span(order_book_capacity);
    std::byte*   cursor      = base;

    for (uint32_t lane = 0; lane < lanes; ++lane)
    {
        const bool trade_ok = as_owner ? trades[lane].create(cursor, trade_bytes, trade_capacity)
                                       : trades[lane].attach(cursor, trade_bytes, trade_capacity);

        if (!trade_ok)
        {
            last_error_ = "체결 큐 " + std::to_string(lane) + ": " + std::string(trades[lane].last_error());
            return false;
        }

        cursor += trade_bytes;

        const bool book_ok = as_owner ? order_books[lane].create(cursor, book_bytes, order_book_capacity)
                                      : order_books[lane].attach(cursor, book_bytes, order_book_capacity);

        if (!book_ok)
        {
            last_error_ = "호가 큐 " + std::to_string(lane) + ": " + std::string(order_books[lane].last_error());
            return false;
        }

        cursor += book_bytes;
    }

    // 줄 전부가 선 뒤에 바꿔 단다 — 중간에 실패한 통로를 반쪽으로 들고 있지 않는다.
    trades_      = std::move(trades);
    order_books_ = std::move(order_books);
    lanes_       = lanes;
    return true;
}

bool MarketFeedChannel::create(std::byte* base, size_t bytes, uint32_t lanes, size_t trade_capacity,
                               size_t order_book_capacity)
{
    if (!check_arguments(base, bytes, lanes, trade_capacity, order_book_capacity))
    {
        return false;
    }

    return bind(base, lanes, trade_capacity, order_book_capacity, true);
}

bool MarketFeedChannel::attach(std::byte* base, size_t bytes, uint32_t lanes, size_t trade_capacity,
                               size_t order_book_capacity)
{
    if (!check_arguments(base, bytes, lanes, trade_capacity, order_book_capacity))
    {
        return false;
    }

    return bind(base, lanes, trade_capacity, order_book_capacity, false);
}

void MarketFeedChannel::unbind() noexcept
{
    trades_.clear();
    order_books_.clear();
    lanes_ = 0;
}

bool MarketFeedChannel::push_trade(uint32_t lane, const TradeData& trade) noexcept
{
    if (lane >= lanes_)
    {
        return false;
    }

    if (!trades_[lane].push(trade))
    {
        overflow_trades_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

bool MarketFeedChannel::push_order_book(uint32_t lane, const OrderBook& order_book) noexcept
{
    if (lane >= lanes_)
    {
        return false;
    }

    if (!order_books_[lane].push(order_book))
    {
        overflow_order_books_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

bool MarketFeedChannel::pop_trade(uint32_t lane, const MarketLimits& limits, TradeData& out) noexcept
{
    if (lane >= lanes_)
    {
        return false;
    }

    // 말이 안 되는 칸은 버리고 다음 칸을 본다 — 한 건 때문에 그 줄이 멈추지 않는다.
    while (trades_[lane].pop(out))
    {
        if (is_plausible(out, limits))
        {
            return true;
        }

        discarded_.fetch_add(1, std::memory_order_relaxed);
    }

    return false;
}

bool MarketFeedChannel::pop_order_book(uint32_t lane, const MarketLimits& limits, OrderBook& out) noexcept
{
    if (lane >= lanes_)
    {
        return false;
    }

    while (order_books_[lane].pop(out))
    {
        if (is_plausible(out, limits))
        {
            return true;
        }

        discarded_.fetch_add(1, std::memory_order_relaxed);
    }

    return false;
}

uint64_t MarketFeedChannel::stamp_out_of_turn() const
{
    uint64_t total = 0;

    for (const SharedSpscRing<TradeData>& ring : trades_)
    {
        total += ring.stamp_out_of_turn();
    }

    for (const SharedSpscRing<OrderBook>& ring : order_books_)
    {
        total += ring.stamp_out_of_turn();
    }

    return total;
}

size_t MarketFeedChannel::pending_trades(uint32_t lane) const
{
    return lane < lanes_ ? trades_[lane].pending() : 0;
}

size_t MarketFeedChannel::pending_order_books(uint32_t lane) const
{
    return lane < lanes_ ? order_books_[lane].pending() : 0;
}

} // namespace ipc
