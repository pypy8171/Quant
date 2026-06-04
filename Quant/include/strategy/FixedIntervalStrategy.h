#pragma once
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <chrono>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// FixedIntervalStrategy  —  고정 종목 주기적 매수/매도 (파이프라인 테스트용)
//
//  interval_sec마다 BUY(buy_qty) → SELL(sell_qty) 를 교대로 발행한다.
//  장 세션 외 시간에는 신호를 내지 않는다.
// ─────────────────────────────────────────────────────────────────────────────
class FixedIntervalStrategy : public StrategyBase
{
public:
    FixedIntervalStrategy(std::string ticker, int buy_qty, int sell_qty, int interval_sec)
        : ticker_(std::move(ticker)), buy_qty_(buy_qty), sell_qty_(sell_qty),
          interval_sec_(interval_sec)
    {}

    std::string id() const override { return "FIXED_INTERVAL_" + ticker_; }

    std::string describe() const override
    {
        return id() + " | BUY=" + std::to_string(buy_qty_) +
               " SELL=" + std::to_string(sell_qty_) +
               " every " + std::to_string(interval_sec_) + "s";
    }

    // 고정 종목 구독
    std::vector<WatchSpec> get_watch_specs() const override
    {
        return {{ticker_, Market::KR, ""}};
    }

    void on_start() override
    {
        last_signal_ = std::chrono::steady_clock::now() -
                       std::chrono::seconds(interval_sec_); // 즉시 첫 신호 허용
        phase_ = Phase::BUY;
        LOG_INFO("[FixedInterval] 시작: " + ticker_ +
                 " BUY=" + std::to_string(buy_qty_) +
                 " SELL=" + std::to_string(sell_qty_) +
                 " 주기=" + std::to_string(interval_sec_) + "초");
    }

    // 일봉 데이터 불필요
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 체결 이벤트마다 시간 체크
    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        if (td.ticker != ticker_) return std::nullopt;
        if (!is_in_session(td.time)) return std::nullopt;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_signal_).count();
        if (elapsed < interval_sec_) return std::nullopt;

        OrderSignal sig;
        sig.ticker      = ticker_;
        sig.market      = Market::KR;
        sig.type        = OrderType::MARKET;
        sig.strategy_id = id();
        sig.timestamp   = std::chrono::system_clock::now();

        if (phase_ == Phase::BUY)
        {
            sig.side     = OrderSide::BUY;
            sig.quantity = buy_qty_;
            phase_       = (sell_qty_ > 0) ? Phase::SELL : Phase::BUY; // sell_qty=0이면 BUY만 반복
            LOG_INFO("[FixedInterval] BUY " + ticker_ + " " + std::to_string(buy_qty_) + "주");
        }
        else
        {
            sig.side     = OrderSide::SELL;
            sig.quantity = sell_qty_;
            phase_       = Phase::BUY;
            LOG_INFO("[FixedInterval] SELL " + ticker_ + " " + std::to_string(sell_qty_) + "주");
        }

        last_signal_ = now;
        return sig;
    }

    void on_stop() override
    {
        LOG_INFO("[FixedInterval] 종료: " + ticker_);
    }

private:
    enum class Phase { BUY, SELL };

    std::string ticker_;
    int buy_qty_, sell_qty_, interval_sec_;
    Phase phase_ = Phase::BUY;
    std::chrono::steady_clock::time_point last_signal_{};

    // HHMMSS → HHMM 정수
    static int parse_hhmm(const std::string& t)
    {
        if (t.size() < 4) return 0;
        try { return std::stoi(t.substr(0, 2)) * 100 + std::stoi(t.substr(2, 2)); }
        catch (...) { return 0; }
    }

    bool is_in_session(const std::string& time_str) const
    {
        int hhmm = parse_hhmm(time_str);
        return hhmm >= 900 && hhmm < 1530;
    }
};
