#pragma once
#include "strategy/StrategyBase.h"
#include <deque>
#include <numeric>

// ─────────────────────────────────────────────────────────────────────────────
// MACrossStrategy  —  골든크로스 / 데드크로스 전략
//   - 단기 MA가 장기 MA를 상향 돌파 → 매수 (골든크로스)
//   - 단기 MA가 장기 MA를 하향 돌파 → 매도 (데드크로스)
// ─────────────────────────────────────────────────────────────────────────────
class MACrossStrategy : public StrategyBase
{
public:
    MACrossStrategy(std::string ticker, int short_period, int long_period, int qty)
        : ticker_(std::move(ticker)), short_period_(short_period), long_period_(long_period), quantity_(qty)
    {
    }

    std::string id() const override
    {
        return "MA_CROSS_" + ticker_;
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        return {{ticker_, Market::KR, ""}};
    }

    std::string describe() const override
    {
        return "MACross | " + ticker_ + " | short=" + std::to_string(short_period_) +
               " long=" + std::to_string(long_period_) + " qty=" + std::to_string(quantity_);
    }

    void on_start() override
    {
        prices_.clear();
        prev_short_ma_ = 0.0;
        prev_long_ma_ = 0.0;
        in_position_ = false;
    }

    std::optional<OrderSignal> on_data(const MarketData& data) override
    {
        if (data.ticker != ticker_)
            return std::nullopt;

        prices_.push_back(data.close);
        if ((int)prices_.size() > long_period_)
            prices_.pop_front();

        if ((int)prices_.size() < long_period_)
            return std::nullopt;

        double short_ma = calc_ma(short_period_);
        double long_ma = calc_ma(long_period_);

        std::optional<OrderSignal> signal;

        // 골든크로스: 단기가 장기를 상향 돌파
        if (!in_position_ && prev_short_ma_ <= prev_long_ma_ && short_ma > long_ma)
        {
            signal = make_signal(data, OrderSide::BUY);
            in_position_ = true;
        }
        // 데드크로스: 단기가 장기를 하향 돌파
        else if (in_position_ && prev_short_ma_ >= prev_long_ma_ && short_ma < long_ma)
        {
            signal = make_signal(data, OrderSide::SELL);
            in_position_ = false;
        }

        prev_short_ma_ = short_ma;
        prev_long_ma_ = long_ma;
        return signal;
    }

private:
    double calc_ma(int period) const
    {
        auto it = prices_.end();
        double sum = 0.0;
        for (int i = 0; i < period; ++i)
            sum += *(--it);
        return sum / period;
    }

    OrderSignal make_signal(const MarketData& d, OrderSide side)
    {
        OrderSignal s;
        s.ticker = ticker_;
        s.side = side;
        s.type = OrderType::MARKET;
        s.quantity = quantity_;
        s.strategy_id = id();
        s.timestamp = d.timestamp;
        return s;
    }

    std::string ticker_;
    int short_period_;
    int long_period_;
    int quantity_;
    std::deque<double> prices_;
    double prev_short_ma_ = 0.0;
    double prev_long_ma_ = 0.0;
    bool in_position_ = false;
};
