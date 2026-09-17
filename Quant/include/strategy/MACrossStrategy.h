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
    //   → OrderGate 내부 원장에서 매도가 먼저 -quantity를 선점하므로, 뒤이은 재매수가 상쇄(net-zero)돼
    //     포지션 한도에 걸리지 않는다.
    MACrossStrategy(std::string ticker, int short_period, int long_period, int quantity,
                    bool start_in_position = false)
        : ticker_(std::move(ticker)), short_period_(short_period), long_period_(long_period),
          quantity_(quantity), start_in_position_(start_in_position)
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
        symbol_id_ = symbol_of(ticker_);
        prices_.clear();
        prev_short_ma_ = 0.0;
        prev_long_ma_ = 0.0;
        have_prev_ = false;
        in_position_ = start_in_position_;
    }

    bool wants_daily_bars() const override { return true; }

    std::optional<OrderSignal> on_data(const MarketData& data) override
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

        double short_ma = calc_ma(short_period_);
        double long_ma = calc_ma(long_period_);

        std::optional<OrderSignal> signal;

        // 첫 완전창은 previous만 시드하고 신호를 건너뛴다. previous가 0.0으로 시작하면
        // prev_short<=prev_long(0<=0)이 무조건 참이라, 실제 교차가 없어도 그 순간
        // short>long이기만 하면 허위 골든크로스로 매수해버린다.
        if (have_prev_)
        {
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
        }

        prev_short_ma_ = short_ma;
        prev_long_ma_ = long_ma;
        have_prev_ = true;
        return signal;
    }

private:
    double calc_ma(int period) const
    {
        auto iterator = prices_.end();
        double sum = 0.0;

        for (int period_index = 0; period_index < period; ++period_index)
        {
            sum += *(--iterator);
        }

        return sum / period;
    }

    OrderSignal make_signal(const MarketData& market_data, OrderSide side)
    {
        OrderSignal signal;
        signal.ticker = ticker_;
        signal.symbol_id    = symbol_id_;
        signal.side = side;
        signal.type = OrderType::MARKET;
        signal.quantity = quantity_;
        signal.ref_price = market_data.close;  // 시장가 명목 백스톱 평가 기준가(price=0이라 없으면 우회됨)
        signal.strategy_id = id();
        signal.timestamp = market_data.timestamp;
        return signal;
    }

    std::string ticker_;
    symbol::SymbolId symbol_id_ = symbol::kNone; // ticker_의 id — on_start에서 한 번(미주입=kNone, 문자열 비교로 폴백)
    int short_period_;
    int long_period_;
    int quantity_;
    bool start_in_position_ = false;
    std::deque<double> prices_;
    double prev_short_ma_ = 0.0;
    double prev_long_ma_ = 0.0;
    bool have_prev_ = false;
    bool in_position_ = false;
};
