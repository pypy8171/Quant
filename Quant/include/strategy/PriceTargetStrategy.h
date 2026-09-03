#pragma once
#include "core/MarketSession.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <chrono>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PriceTargetStrategy  —  가격 도달 시 주문 + 예약 지정가 주문
//
//  [price_targets]
//    호가/체결 이벤트에서 가격 조건 충족 시 시장가 주문
//    동일 방향 cooldown_sec 안에 중복 주문 차단
//
//  [limit_orders]
//    장 시작 후 첫 이벤트에서 지정가 주문 1회 제출
// ─────────────────────────────────────────────────────────────────────────────
class PriceTargetStrategy : public StrategyBase
{
public:
    struct PriceTarget
    {
        std::string ticker;
        double      buy_price    = 0;  // 이 가격 이하면 매수 (0=비활성)
        double      sell_price   = 0;  // 이 가격 이상이면 매도 (0=비활성)
        int         quantity     = 1;
        int         cooldown_sec = 60; // 동일 방향 재주문 최소 간격
    };

    struct LimitOrder
    {
        std::string ticker;
        OrderSide   side     = OrderSide::BUY;
        double      price    = 0;
        int         quantity = 1;
        bool        placed   = false;
    };

    PriceTargetStrategy(std::vector<PriceTarget> targets,
                        std::vector<LimitOrder>  limit_orders)
        : targets_(std::move(targets)),
          limit_orders_(std::move(limit_orders))
    {}

    std::string id() const override { return "PRICE_TARGET"; }

    std::string describe() const override
    {
        std::string s = "PRICE_TARGET";
        for (const auto& t : targets_)
            s += " | " + t.ticker + " B=" + std::to_string(static_cast<int>(t.buy_price)) +
                 " S=" + std::to_string(static_cast<int>(t.sell_price)) +
                 " qty=" + std::to_string(t.quantity);
        for (const auto& lo : limit_orders_)
            s += " | " + lo.ticker + " LMT@" + std::to_string(static_cast<int>(lo.price)) +
                 " x" + std::to_string(lo.quantity);
        return s;
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        std::vector<WatchSpec> specs;
        for (const auto& t : targets_)
            specs.push_back({t.ticker, Market::KR, ""});
        for (const auto& lo : limit_orders_)
        {
            bool dup = false;
            for (const auto& t : targets_)
                if (t.ticker == lo.ticker) { dup = true; break; }
            if (!dup) specs.push_back({lo.ticker, Market::KR, ""});
        }
        return specs;
    }

    void on_start() override
    {
        // last_buy/sell 타임포인트 초기화
        last_buy_.assign(targets_.size(), std::chrono::steady_clock::time_point{});
        last_sell_.assign(targets_.size(), std::chrono::steady_clock::time_point{});

        LOG_INFO("[PriceTarget] 시작");
        for (const auto& t : targets_)
            LOG_INFO("  가격목표: " + t.ticker +
                     " BUY≤" + std::to_string(static_cast<int>(t.buy_price)) +
                     " SELL≥" + std::to_string(static_cast<int>(t.sell_price)) +
                     " qty=" + std::to_string(t.quantity) +
                     " cooldown=" + std::to_string(t.cooldown_sec) + "s");
        for (const auto& lo : limit_orders_)
            LOG_INFO("  예약지정가: " + lo.ticker +
                     (lo.side == OrderSide::BUY ? " BUY" : " SELL") +
                     " @" + std::to_string(static_cast<int>(lo.price)) +
                     " x" + std::to_string(lo.quantity));
    }

    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 호가 이벤트 — 매도호가[0]을 현재가 대리로 삼아 가격 체크(없으면 매수호가[0]로 폴백)
    std::optional<OrderSignal> on_order_book(const OrderBook& ob) override
    {
        // 지정가 예약 주문 먼저
        auto lo = check_limit_order(ob.ticker);
        if (lo) return lo;

        // 가격 목표 — 매도호가[0]을 현재가 대리로 사용
        double price = ob.asks[0].price > 0 ? ob.asks[0].price : ob.bids[0].price;
        return check_price_target(ob.ticker, price, ob.time);
    }

    // 체결 이벤트 — 체결가 기준 가격 체크
    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        if (td.market != Market::KR) return std::nullopt;

        // 지정가 예약 주문 먼저
        auto lo = check_limit_order(td.ticker);
        if (lo) return lo;

        return check_price_target(td.ticker, td.price, td.time);
    }

    void on_stop() override
    {
        LOG_INFO("[PriceTarget] 종료");
    }

private:
    // ── 예약 지정가 주문 (1회) ────────────────────────────────────────────
    std::optional<OrderSignal> check_limit_order(const std::string& ticker)
    {
        for (auto& lo : limit_orders_)
        {
            if (lo.placed || lo.ticker != ticker) continue;
            if (lo.side == OrderSide::BUY && !is_active()) continue;  // BUY 예약은 국면 게이트

            lo.placed = true;
            OrderSignal sig;
            sig.ticker      = lo.ticker;
            sig.side        = lo.side;
            sig.type        = OrderType::LIMIT;
            sig.price       = lo.price;
            sig.quantity    = lo.quantity;
            sig.market      = Market::KR;
            sig.strategy_id = id();
            sig.timestamp   = std::chrono::system_clock::now();

            LOG_INFO("[PriceTarget] 예약지정가 제출: " + lo.ticker +
                     (lo.side == OrderSide::BUY ? " BUY" : " SELL") +
                     " @" + std::to_string(static_cast<int>(lo.price)) +
                     " x" + std::to_string(lo.quantity));
            return sig;
        }
        return std::nullopt;
    }

    // ── 가격 목표 도달 시 시장가 주문 ────────────────────────────────────
    std::optional<OrderSignal> check_price_target(const std::string& ticker,
                                                   double price,
                                                   const std::string& time_str)
    {
        if (price <= 0) return std::nullopt;

        int hhmm = parse_hhmm(time_str);
        if (!krx::in_session(hhmm)) return std::nullopt; // 09:00~15:30 정규장만(core/MarketSession.h)

        auto now = std::chrono::steady_clock::now();

        for (size_t i = 0; i < targets_.size(); ++i)
        {
            auto& t = targets_[i];
            if (t.ticker != ticker) continue;

            // 매수 조건: 가격 ≤ buy_price (진입 — 국면 게이트)
            if (is_active() && t.buy_price > 0 && price <= t.buy_price)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - last_buy_[i]).count();
                if (elapsed >= t.cooldown_sec)
                {
                    last_buy_[i] = now;
                    LOG_INFO("[PriceTarget] BUY 조건 충족: " + ticker +
                             " @" + std::to_string(static_cast<int>(price)) +
                             " (목표≤" + std::to_string(static_cast<int>(t.buy_price)) + ")");
                    return make_signal(t, OrderSide::BUY, price);
                }
            }

            // 매도 조건: 가격 ≥ sell_price
            if (t.sell_price > 0 && price >= t.sell_price)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - last_sell_[i]).count();
                if (elapsed >= t.cooldown_sec)
                {
                    last_sell_[i] = now;
                    LOG_INFO("[PriceTarget] SELL 조건 충족: " + ticker +
                             " @" + std::to_string(static_cast<int>(price)) +
                             " (목표≥" + std::to_string(static_cast<int>(t.sell_price)) + ")");
                    return make_signal(t, OrderSide::SELL, price);
                }
            }
        }
        return std::nullopt;
    }

    static OrderSignal make_signal(const PriceTarget& t, OrderSide side, double trigger_price)
    {
        OrderSignal sig;
        sig.ticker      = t.ticker;
        sig.side        = side;
        sig.type        = OrderType::MARKET;
        sig.quantity    = t.quantity;
        sig.ref_price   = trigger_price;  // 시장가 명목 백스톱 기준가(트리거 현재가)
        sig.market      = Market::KR;
        sig.strategy_id = "PRICE_TARGET";
        sig.timestamp   = std::chrono::system_clock::now();
        return sig;
    }

    static int parse_hhmm(const std::string& t)
    {
        if (t.size() < 4) return 0;
        try { return std::stoi(t.substr(0, 2)) * 100 + std::stoi(t.substr(2, 2)); }
        catch (...) { return 0; }
    }

    std::vector<PriceTarget>                              targets_;
    std::vector<LimitOrder>                               limit_orders_;
    std::vector<std::chrono::steady_clock::time_point>    last_buy_;
    std::vector<std::chrono::steady_clock::time_point>    last_sell_;
};
