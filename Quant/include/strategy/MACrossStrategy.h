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
        id_ = "MA_CROSS_" + ticker_;
    }

    const std::string& id() const override { return id_; }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        // MACross는 REST 폴링(get_daily_ohlcv)으로만 동작 — WS 호가/체결 불필요.
        // trade_only=true → H0STCNT0만 구독(호가 제외)해 구독 한도(≈41건) 절약.
        return {{ticker_, Market::KR, "", true}};
    }

    std::string describe() const override;

    void on_start() override;

    bool wants_daily_bars() const override { return true; }

    std::optional<OrderSignal> on_data(const MarketData& data) override;

private:
    double calculate_moving_average(int period) const;

    OrderSignal make_signal(const MarketData& market_data, OrderSide side);

    std::string ticker_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    symbol::SymbolId symbol_id_ = symbol::kNone; // ticker_의 id — on_start에서 한 번(미주입=kNone, 문자열 비교로 폴백)
    int short_period_;
    int long_period_;
    int quantity_;
    bool start_in_position_ = false;
    std::deque<double> prices_;
    double previous_short_moving_average_ = 0.0;
    double previous_long_moving_average_ = 0.0;
    bool have_previous_ = false;
    bool in_position_ = false;
};
