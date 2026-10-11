// test_engine 공용 준비의 몸통 — 단언 수와 시험용 피드·전략의 메서드.
#include "test_engine_support.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine_test
{

int g_checks = 0;

// ─── FakeFeed ───
FakeFeed::FakeFeed(uint32_t lanes)
    : lanes_(lanes)
{
}

uint32_t FakeFeed::lanes() const
{
    return lanes_;
}

void FakeFeed::set_callbacks(OrderBookCb on_order_book, TradeCb on_trade)
{
    on_order_book_    = std::move(on_order_book);
    on_trade_ = std::move(on_trade);
}

void FakeFeed::set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade)
{
    lane_order_book_    = std::move(on_order_book);
    lane_trade_ = std::move(on_trade);
}

bool FakeFeed::connect(const std::vector<WatchSpec>& specifications)
{
    std::lock_guard<std::mutex> lock(mutex_);
    specifications_     = specifications;
    connected_ = true;
    return true;
}

void FakeFeed::disconnect()
{
    connected_ = false;
}

bool FakeFeed::subscribe_incremental(const WatchSpec& specification)
{
    std::lock_guard<std::mutex> lock(mutex_);
    specifications_.push_back(specification);
    return true;
}

bool FakeFeed::has_specification(const WatchSpec& specification) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& watch : specifications_)
    {
        if (watch.ticker == specification.ticker)
        {
            return true;
        }
    }

    return false;
}

std::vector<WatchSpec> FakeFeed::take_overflow_specifications()
{
    return {};
}

bool FakeFeed::is_connected() const
{
    return connected_.load();
}

bool FakeFeed::is_stale(int) const
{
    return false;
}

void FakeFeed::emit_trade(uint32_t lane, const std::string& ticker, double price, int32_t hhmmss)
{
    TradeData trade;
    trade.ticker.assign(ticker);
    trade.price     = price;
    trade.quantity  = 10;
    trade.direction = 1;
    trade.hhmmss    = hhmmss;

    if (lane_trade_)
    {
        lane_trade_(lane, trade);
    }
    else if (on_trade_)
    {
        on_trade_(trade);
    }
}

std::vector<WatchSpec> FakeFeed::specifications() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return specifications_;
}

// ─── BuyOnce ───
BuyOnce::BuyOnce(std::string ticker)
    : ticker_(std::move(ticker)), id_("buy_once_" + ticker_)
{
}

const std::string& BuyOnce::id() const
{
    return id_;
}

std::string BuyOnce::describe() const
{
    return "첫 틱에 1주 매수";
}

std::optional<OrderSignal> BuyOnce::on_data(const MarketData&)
{
    return std::nullopt;
}

void BuyOnce::on_start()
{
    symbol_id_ = symbol_of(ticker_);
}

std::vector<WatchSpec> BuyOnce::get_watch_specifications() const
{
    WatchSpec specification;
    specification.ticker = ticker_;
    return {specification};
}

std::optional<OrderSignal> BuyOnce::on_trade(const TradeData& trade)
{
    ++ticks_seen;

    if (fired_ || !same_symbol(symbol_id_, ticker_, trade.symbol_id, trade.ticker.string()))
    {
        return std::nullopt;
    }

    fired_ = true;
    OrderSignal signal;
    signal.ticker      = ticker_;
    signal.symbol_id         = trade.symbol_id;
    signal.side        = OrderSide::BUY;
    signal.type        = OrderType::MARKET;
    signal.quantity    = 1;
    signal.reference_price   = trade.price;
    signal.strategy_id = id();
    return signal;
}

symbol::SymbolId BuyOnce::symbol_id() const
{
    return symbol_id_;
}

// ─── BuyEachOnce ───
BuyEachOnce::BuyEachOnce(std::vector<std::string> tickers)
    : tickers_(std::move(tickers)), id_("BuyEachOnce")
{
}

const std::string& BuyEachOnce::id() const
{
    return id_;
}

std::string BuyEachOnce::describe() const
{
    return "종목마다 첫 틱에 1주 매수";
}

std::optional<OrderSignal> BuyEachOnce::on_data(const MarketData&)
{
    return std::nullopt;
}

void BuyEachOnce::on_start()
{
    for (const auto& ticker : tickers_)
    {
        symbol_ids_.push_back(symbol_of(ticker));
    }

    fired_.assign(tickers_.size(), 0);
}

std::vector<WatchSpec> BuyEachOnce::get_watch_specifications() const
{
    std::vector<WatchSpec> specifications;

    for (const auto& ticker : tickers_)
    {
        WatchSpec specification;
        specification.ticker = ticker;
        specifications.push_back(specification);
    }

    return specifications;
}

std::optional<OrderSignal> BuyEachOnce::on_trade(const TradeData& trade)
{
    for (size_t index = 0; index < tickers_.size(); ++index)
    {
        if (fired_[index] || !same_symbol(symbol_ids_[index], tickers_[index], trade.symbol_id, trade.ticker.string()))
        {
            continue;
        }

        fired_[index] = 1;
        OrderSignal signal;
        signal.ticker          = tickers_[index];
        signal.symbol_id       = trade.symbol_id;
        signal.side            = OrderSide::BUY;
        signal.type            = OrderType::MARKET;
        signal.quantity        = 1;
        signal.reference_price = trade.price;
        signal.strategy_id     = id();
        return signal;
    }

    return std::nullopt;
}

const std::vector<symbol::SymbolId>& BuyEachOnce::symbol_ids() const
{
    return symbol_ids_;
}

} // namespace engine_test
