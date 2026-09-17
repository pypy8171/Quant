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

    std::string describe() const override
    {
        return id() + " | BUY=" + std::to_string(buy_quantity_) +
               " SELL=" + std::to_string(sell_quantity_) +
               " every " + std::to_string(interval_sec_) + "s";
    }

    // 고정 종목 구독
    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{ticker_, Market::KR, ""}};
    }

    void on_start() override
    {
        symbol_id_ = symbol_of(ticker_);
        last_signal_ = std::chrono::steady_clock::now() -
                       std::chrono::seconds(interval_sec_); // 즉시 첫 신호 허용
        phase_ = Phase::BUY;
        LOG_INFO("[FixedInterval] 시작: " + ticker_ +
                 " BUY=" + std::to_string(buy_quantity_) +
                 " SELL=" + std::to_string(sell_quantity_) +
                 " 주기=" + std::to_string(interval_sec_) + "초");
    }

    // 일봉 데이터 불필요
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 체결 이벤트마다 시간 체크
    std::optional<OrderSignal> on_trade(const TradeData& trade) override
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
        signal.ticker      = ticker_;
        signal.symbol_id         = symbol_id_;
        signal.market      = Market::KR;
        signal.type        = OrderType::MARKET;
        signal.reference_price   = trade.price; // 시장가는 price=0 — 이 값이 없으면 1주문 명목 상한이 비어 버린다
        signal.strategy_id = id();
        signal.timestamp   = std::chrono::system_clock::now();

        if (phase_ == Phase::BUY)
        {
            signal.side     = OrderSide::BUY;
            signal.quantity = buy_quantity_;
            phase_       = (sell_quantity_ > 0) ? Phase::SELL : Phase::BUY; // sell_quantity=0이면 BUY만 반복
            LOG_INFO("[FixedInterval] BUY " + ticker_ + " " + std::to_string(buy_quantity_) + "주");
        }
        else
        {
            signal.side     = OrderSide::SELL;
            signal.quantity = sell_quantity_;
            phase_       = Phase::BUY;
            LOG_INFO("[FixedInterval] SELL " + ticker_ + " " + std::to_string(sell_quantity_) + "주");
        }

        last_signal_ = now;
        return signal;
    }

    void on_stop() override
    {
        LOG_INFO("[FixedInterval] 종료: " + ticker_);
    }

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
