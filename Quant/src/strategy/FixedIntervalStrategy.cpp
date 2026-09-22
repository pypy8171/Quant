#include "strategy/FixedIntervalStrategy.h"

std::string FixedIntervalStrategy::describe() const
{
    return id() + " | BUY=" + std::to_string(buy_quantity_) + " SELL=" + std::to_string(sell_quantity_) + " every " +
           std::to_string(interval_sec_) + "s";
}

void FixedIntervalStrategy::on_start()
{
    symbol_id_ = symbol_of(ticker_);
    last_signal_ = std::chrono::steady_clock::now() - std::chrono::seconds(interval_sec_); // 즉시 첫 신호 허용
    phase_ = Phase::BUY;
    LOG_INFO("[FixedInterval] 시작: " + ticker_ + " BUY=" + std::to_string(buy_quantity_) +
             " SELL=" + std::to_string(sell_quantity_) + " 주기=" + std::to_string(interval_sec_) + "초");
}

std::optional<OrderSignal> FixedIntervalStrategy::on_trade(const TradeData& trade)
{
    if (!same_symbol(symbol_id_, ticker_, trade.symbol_id, trade.ticker))
    {
        return std::nullopt;
    }

    if (!is_in_session(trade.hhmmss))
    {
        return std::nullopt;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_signal_).count();

    if (elapsed < interval_sec_)
    {
        return std::nullopt;
    }

    OrderSignal signal;
    signal.ticker = ticker_;
    signal.symbol_id = symbol_id_;
    signal.market = Market::KR;
    signal.type = OrderType::MARKET;
    signal.reference_price = trade.price; // 시장가는 price=0 — 이 값이 없으면 1주문 명목 상한이 비어 버린다
    signal.strategy_id = id();
    signal.timestamp = std::chrono::system_clock::now();

    if (phase_ == Phase::BUY)
    {
        signal.side = OrderSide::BUY;
        signal.quantity = buy_quantity_;
        phase_ = (sell_quantity_ > 0) ? Phase::SELL : Phase::BUY; // sell_quantity=0이면 BUY만 반복
        LOG_INFO("[FixedInterval] BUY " + ticker_ + " " + std::to_string(buy_quantity_) + "주");
    }
    else
    {
        signal.side = OrderSide::SELL;
        signal.quantity = sell_quantity_;
        phase_ = Phase::BUY;
        LOG_INFO("[FixedInterval] SELL " + ticker_ + " " + std::to_string(sell_quantity_) + "주");
    }

    last_signal_ = now;
    return signal;
}

void FixedIntervalStrategy::on_stop()
{
    LOG_INFO("[FixedInterval] 종료: " + ticker_);
}
