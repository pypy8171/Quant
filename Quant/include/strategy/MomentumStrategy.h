#pragma once
#include "strategy/StrategyBase.h"
#include <algorithm>
#include <deque>

// ─────────────────────────────────────────────────────────────────────────────
// MomentumStrategy  —  돈치안 채널 브레이크아웃
//   - 현재가가 N일 고점 돌파 → 매수
//   - 현재가가 N일 저점 하향 → 매도 (포지션 청산)
// ─────────────────────────────────────────────────────────────────────────────
class MomentumStrategy : public StrategyBase
{
public:
    MomentumStrategy(std::string ticker, int period, int qty)
        : ticker_(std::move(ticker)), period_(period), quantity_(qty)
    {
    }

    std::string id() const override
    {
        return "MOMENTUM_" + ticker_;
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        return {{ticker_, Market::KR, ""}};
    }

    std::string describe() const override
    {
        return "Momentum(Donchian) | " + ticker_ + " | period=" + std::to_string(period_) +
               " qty=" + std::to_string(quantity_);
    }

    void on_start() override
    {
        highs_.clear();
        lows_.clear();
        in_position_ = false;
    }

    std::optional<OrderSignal> on_data(const MarketData& data) override
    {
        if (data.ticker != ticker_)
            return std::nullopt;

        std::optional<OrderSignal> signal;

        // 채널은 직전 N봉(당일 제외) 고저로 만든다. 당일 바를 채널에 먼저 넣으면
        // close>=channel_high가 close==당일고가일 때만 성립해 돌파 진입이 거의 발화하지
        // 않고, 저점 청산(close<=channel_low)도 같이 억제돼 손절이 조용히 멈춘다.
        // 그래서 판정을 먼저 하고 당일 바 반영(push)은 뒤에 둔다.
        if ((int)highs_.size() >= period_)
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
        if ((int)highs_.size() > period_)
        {
            highs_.pop_front();
            lows_.pop_front();
        }

        return signal;
    }

private:
    OrderSignal make_signal(const MarketData& d, OrderSide side)
    {
        OrderSignal s;
        s.ticker = ticker_;
        s.side = side;
        s.type = OrderType::MARKET;
        s.quantity = quantity_;
        s.ref_price = d.close;  // 시장가 명목 백스톱 평가 기준가(price=0이라 없으면 우회됨)
        s.strategy_id = id();
        s.timestamp = d.timestamp;
        return s;
    }

    std::string ticker_;
    int period_;
    int quantity_;
    std::deque<double> highs_;
    std::deque<double> lows_;
    bool in_position_ = false;
};
