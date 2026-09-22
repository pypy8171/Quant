#pragma once
#include "core/MarketSession.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <chrono>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// FixedIntervalStrategy  —  고정 종목 주기적 매수/매도 (파이프라인 테스트용)
//
//  interval_sec마다 BUY(buy_quantity) → SELL(sell_quantity) 를 교대로 발행한다.
//  장 세션 외 시간에는 신호를 내지 않는다.
// ─────────────────────────────────────────────────────────────────────────────
class FixedIntervalStrategy : public StrategyBase
{
public:
    FixedIntervalStrategy(std::string ticker, int buy_quantity, int sell_quantity, int interval_sec)
        : ticker_(std::move(ticker)), buy_quantity_(buy_quantity), sell_quantity_(sell_quantity),
          interval_sec_(interval_sec)
    {
        id_ = "FIXED_INTERVAL_" + ticker_;}

    const std::string& id() const override { return id_; }

    std::string describe() const override;

    // 고정 종목 구독
    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{ticker_, Market::KR, ""}};
    }

    void on_start() override;

    // 일봉 데이터 불필요
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 체결 이벤트마다 시간 체크
    std::optional<OrderSignal> on_trade(const TradeData& trade) override;

    void on_stop() override;

private:
    enum class Phase { BUY, SELL };

    std::string ticker_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    symbol::SymbolId symbol_id_ = symbol::kNone; // ticker_의 id — on_start에서 한 번(미주입=kNone, 문자열 비교로 폴백)
    int buy_quantity_, sell_quantity_, interval_sec_;
    Phase phase_ = Phase::BUY;
    std::chrono::steady_clock::time_point last_signal_{};

    bool is_in_session(int32_t hhmmss) const
    {
        return krx::in_session(hhmmss / 100); // 09:00~15:30 정규장 창(core/MarketSession.h)
    }
};
