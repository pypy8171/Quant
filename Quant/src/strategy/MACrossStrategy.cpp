#include "strategy/MACrossStrategy.h"

// placeholder

std::string MACrossStrategy::describe() const
{
    return "MACross | " + ticker_ + " | short=" + std::to_string(short_period_) +
           " long=" + std::to_string(long_period_) + " qty=" + std::to_string(quantity_);
}

void MACrossStrategy::on_start()
{
    symbol_id_ = symbol_of(ticker_);
    prices_.clear();
    previous_short_moving_average_ = 0.0;
    previous_long_moving_average_ = 0.0;
    have_previous_ = false;
    in_position_ = start_in_position_;
}

std::optional<OrderSignal> MACrossStrategy::on_data(const MarketData& data)
{
    if (!same_symbol(symbol_id_, ticker_, data.symbol_id, data.ticker))
    {
        return std::nullopt;
    }

    prices_.push_back(data.close);

    if (static_cast<int>(prices_.size()) > long_period_)
    {
        prices_.pop_front();
    }

    if (static_cast<int>(prices_.size()) < long_period_)
    {
        return std::nullopt;
    }

    double short_moving_average = calculate_moving_average(short_period_);
    double long_moving_average = calculate_moving_average(long_period_);

    std::optional<OrderSignal> signal;

    // 첫 완전창은 previous만 시드하고 신호를 건너뛴다. previous가 0.0으로 시작하면
    // previous_short<=previous_long(0<=0)이 무조건 참이라, 실제 교차가 없어도 그 순간
    // short>long이기만 하면 허위 골든크로스로 매수해버린다.
    if (have_previous_)
    {
        // 골든크로스: 단기가 장기를 상향 돌파 (진입 — 국면 게이트 적용)
        if (is_active() && !in_position_ && previous_short_moving_average_ <= previous_long_moving_average_ &&
            short_moving_average > long_moving_average)
        {
            signal = make_signal(data, OrderSide::BUY);
            in_position_ = true;
        }

        // 데드크로스: 단기가 장기를 하향 돌파
        else if (in_position_ && previous_short_moving_average_ >= previous_long_moving_average_ &&
                 short_moving_average < long_moving_average)
        {
            signal = make_signal(data, OrderSide::SELL);
            in_position_ = false;
        }
    }

    previous_short_moving_average_ = short_moving_average;
    previous_long_moving_average_ = long_moving_average;
    have_previous_ = true;
    return signal;
}

double MACrossStrategy::calculate_moving_average(int period) const
{
    auto iterator = prices_.end();
    double sum = 0.0;

    for (int period_index = 0; period_index < period; ++period_index)
    {
        sum += *(--iterator);
    }

    return sum / period;
}

OrderSignal MACrossStrategy::make_signal(const MarketData& market_data, OrderSide side)
{
    OrderSignal signal;
    signal.ticker = ticker_;
    signal.symbol_id = symbol_id_;
    signal.side = side;
    signal.type = OrderType::MARKET;
    signal.quantity = quantity_;
    signal.reference_price = market_data.close; // 시장가 명목 백스톱 평가 기준가(price=0이라 없으면 우회됨)
    signal.strategy_id = id();
    signal.timestamp = market_data.timestamp;
    return signal;
}
