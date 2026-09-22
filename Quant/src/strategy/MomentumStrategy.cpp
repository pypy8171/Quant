#include "strategy/MomentumStrategy.h"

// placeholder

std::string MomentumStrategy::describe() const
{
    return "Momentum(채널 돌파) | " + ticker_ + " | period=" + std::to_string(period_) +
           " qty=" + std::to_string(quantity_);
}

std::optional<OrderSignal> MomentumStrategy::on_data(const MarketData& data)
{
    if (!same_symbol(symbol_id_, ticker_, data.symbol_id, data.ticker))
    {
        return std::nullopt;
    }

    std::optional<OrderSignal> signal;

    // 채널은 직전 N봉(당일 제외) 고저로 만든다. 당일 바를 채널에 먼저 넣으면
    // close>=channel_high가 close==당일고가일 때만 성립해 돌파 진입이 거의 발화하지
    // 않고, 저점 청산(close<=channel_low)도 같이 억제돼 손절이 조용히 멈춘다.
    // 그래서 판정을 먼저 하고 당일 바 반영(push)은 뒤에 둔다.
    if (static_cast<int>(highs_.size()) >= period_)
    {
        double channel_high = *std::max_element(highs_.begin(), highs_.end());
        double channel_low = *std::min_element(lows_.begin(), lows_.end());

        // 돌파 매수 (진입 — 국면 게이트 적용)
        if (is_active() && !in_position_ && data.close >= channel_high)
        {
            in_position_ = true;
            signal = make_signal(data, OrderSide::BUY);
        }

        // 저점 이탈 청산
        else if (in_position_ && data.close <= channel_low)
        {
            in_position_ = false;
            signal = make_signal(data, OrderSide::SELL);
        }
    }

    // 판정 후 당일 바를 채널에 반영(다음 사이클용)
    highs_.push_back(data.high);
    lows_.push_back(data.low);

    if (static_cast<int>(highs_.size()) > period_)
    {
        highs_.pop_front();
        lows_.pop_front();
    }

    return signal;
}

OrderSignal MomentumStrategy::make_signal(const MarketData& market_data, OrderSide side)
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
