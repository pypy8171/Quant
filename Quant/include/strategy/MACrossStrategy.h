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
    // start_in_position=true: 기동 시 이미 보유 중인 것으로 간주(모의계좌 보유분).
    //   → 첫 신호는 항상 데드크로스 매도(BUY는 무포지션에서만) → 기존 보유분을 지표로 청산 가능.
    //   → OrderGate 상대원장상 매도(선점 -qty) 후 재매수가 net-zero라 포지션 한도에 안 걸림.
    MACrossStrategy(std::string ticker, int short_period, int long_period, int qty,
                    bool start_in_position = false)
        : ticker_(std::move(ticker)), short_period_(short_period), long_period_(long_period),
          quantity_(qty), start_in_position_(start_in_position)
    {
    }

    std::string id() const override
    {
        return "MA_CROSS_" + ticker_;
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        // MACross는 REST 폴링(get_daily_ohlcv)으로만 동작 — WS 호가/체결 불필요.
        // trade_only=true → H0STCNT0만 구독(호가 제외)해 구독 한도(≈41건) 절약.
        return {{ticker_, Market::KR, "", true}};
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
        in_position_ = start_in_position_;
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

        // 골든크로스: 단기가 장기를 상향 돌파 (진입 — 국면 게이트 적용)
        if (is_active() && !in_position_ && prev_short_ma_ <= prev_long_ma_ && short_ma > long_ma)
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
    bool start_in_position_ = false;
    std::deque<double> prices_;
    double prev_short_ma_ = 0.0;
    double prev_long_ma_ = 0.0;
    bool in_position_ = false;
};
