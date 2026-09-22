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
    MomentumStrategy(std::string ticker, int period, int quantity)
        : ticker_(std::move(ticker)), period_(period), quantity_(quantity)
    {
        id_ = "MOMENTUM_" + ticker_;
    }

    const std::string& id() const override { return id_; }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{ticker_, Market::KR, ""}};
    }

    std::string describe() const override;

    void on_start() override
    {
        symbol_id_ = symbol_of(ticker_);
        highs_.clear();
        lows_.clear();
        in_position_ = false;
    }

    bool wants_daily_bars() const override { return true; }

    std::optional<OrderSignal> on_data(const MarketData& data) override;

private:
    OrderSignal make_signal(const MarketData& market_data, OrderSide side);

    std::string ticker_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    symbol::SymbolId symbol_id_ = symbol::kNone; // ticker_의 id — on_start에서 한 번(미주입=kNone, 문자열 비교로 폴백)
    int period_;
    int quantity_;
    std::deque<double> highs_;
    std::deque<double> lows_;
    bool in_position_ = false;
};
