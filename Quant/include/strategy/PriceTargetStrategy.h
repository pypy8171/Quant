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
        std::string   ticker;
        symbol::SymbolId symbol_id = symbol::kNone; // on_start에서 ticker로 한 번 채운다 — 틱 비교는 이 값
        double      buy_price    = 0;  // 이 가격 이하면 매수 (0=비활성)
        double      sell_price   = 0;  // 이 가격 이상이면 매도 (0=비활성)
        int         quantity     = 1;
        int         cooldown_sec = 60; // 동일 방향 재주문 최소 간격
    };

    struct LimitOrder
    {
        std::string   ticker;
        symbol::SymbolId symbol_id = symbol::kNone; // 위와 같다
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
        std::string text = "PRICE_TARGET";

        for (const auto& target : targets_)
        {
            text += " | " + target.ticker + " B=" + std::to_string(static_cast<int>(target.buy_price)) +
                 " S=" + std::to_string(static_cast<int>(target.sell_price)) +
                 " qty=" + std::to_string(target.quantity);
        }

        for (const auto& low : limit_orders_)
        {
            text += " | " + low.ticker + " LMT@" + std::to_string(static_cast<int>(low.price)) +
                 " x" + std::to_string(low.quantity);
        }

        return text;
    }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        std::vector<WatchSpec> specifications;

        for (const auto& target : targets_)
        {
            specifications.push_back({target.ticker, Market::KR, ""});
        }

        for (const auto& low : limit_orders_)
        {
            bool duplicate = false;

            for (const auto& target : targets_)
            {
                if (target.ticker == low.ticker) { duplicate = true; break; }
            }

            if (!duplicate)
            {
                specifications.push_back({low.ticker, Market::KR, ""});
            }
        }

        return specifications;
    }

    void on_start() override
    {
        for (auto& target : targets_)
        {
            target.symbol_id = symbol_of(target.ticker);
        }

        for (auto& low : limit_orders_)
        {
            low.symbol_id = symbol_of(low.ticker);
        }

        // last_buy/sell 타임포인트 초기화
        last_buy_.assign(targets_.size(), std::chrono::steady_clock::time_point{});
        last_sell_.assign(targets_.size(), std::chrono::steady_clock::time_point{});

        LOG_INFO("[PriceTarget] 시작");

        for (const auto& target : targets_)
        {
            LOG_INFO("  가격목표: " + target.ticker +
                     " BUY≤" + std::to_string(static_cast<int>(target.buy_price)) +
                     " SELL≥" + std::to_string(static_cast<int>(target.sell_price)) +
                     " qty=" + std::to_string(target.quantity) +
                     " cooldown=" + std::to_string(target.cooldown_sec) + "s");
        }

        for (const auto& low : limit_orders_)
        {
            LOG_INFO("  예약지정가: " + low.ticker +
                     (low.side == OrderSide::BUY ? " BUY" : " SELL") +
                     " @" + std::to_string(static_cast<int>(low.price)) +
                     " x" + std::to_string(low.quantity));
        }
    }

    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 호가 이벤트 — 매도호가[0]을 현재가 대리로 삼아 가격 체크(없으면 매수호가[0]로 폴백)
    std::optional<OrderSignal> on_order_book(const OrderBook& order_book) override
    {
        // 지정가 예약 주문 먼저
        auto limit_order = check_limit_order(order_book.symbol_id, order_book.ticker);

        if (limit_order)
        {
            return limit_order;
        }

        // 가격 목표 — 매도호가[0]을 현재가 대리로 사용
        double price = order_book.asks[0].price > 0 ? order_book.asks[0].price : order_book.bids[0].price;
        return check_price_target(order_book.symbol_id, order_book.ticker, price, order_book.hhmmss);
    }

    // 체결 이벤트 — 체결가 기준 가격 체크
    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        if (trade.market != Market::KR)
        {
            return std::nullopt;
        }

        // 지정가 예약 주문 먼저
        auto limit_order = check_limit_order(trade.symbol_id, trade.ticker);

        if (limit_order)
        {
            return limit_order;
        }

        return check_price_target(trade.symbol_id, trade.ticker, trade.price, trade.hhmmss);
    }

    void on_stop() override
    {
        LOG_INFO("[PriceTarget] 종료");
    }

private:
    // ── 예약 지정가 주문 (1회) ────────────────────────────────────────────
    std::optional<OrderSignal> check_limit_order(symbol::SymbolId symbol_id, std::string_view ticker)
    {
        for (auto& low : limit_orders_)
        {
            if (low.placed || !same_symbol(low.symbol_id, low.ticker, symbol_id, ticker))
            {
                continue;
            }

            if (low.side == OrderSide::BUY && !is_active())
            {
                continue;  // BUY 예약은 국면 게이트
            }

            low.placed = true;
            OrderSignal signal;
            signal.ticker      = low.ticker;
            signal.symbol_id         = low.symbol_id;
            signal.side        = low.side;
            signal.type        = OrderType::LIMIT;
            signal.price       = low.price;
            signal.quantity    = low.quantity;
            signal.market      = Market::KR;
            signal.strategy_id = id();
            signal.timestamp   = std::chrono::system_clock::now();

            LOG_INFO("[PriceTarget] 예약지정가 제출: " + low.ticker +
                     (low.side == OrderSide::BUY ? " BUY" : " SELL") +
                     " @" + std::to_string(static_cast<int>(low.price)) +
                     " x" + std::to_string(low.quantity));
            return signal;
        }

        return std::nullopt;
    }

    // ── 가격 목표 도달 시 시장가 주문 ────────────────────────────────────
    std::optional<OrderSignal> check_price_target(symbol::SymbolId symbol_id,
                                                   std::string_view ticker,
                                                   double price,
                                                   int32_t hhmmss)
    {
        if (price <= 0)
        {
            return std::nullopt;
        }

        int hhmm = hhmmss / 100;

        if (!krx::in_session(hhmm))
        {
            return std::nullopt;  // 09:00~15:30 정규장만(core/MarketSession.h)
        }

        auto now = std::chrono::steady_clock::now();

        for (size_t target_index = 0; target_index < targets_.size(); ++target_index)
        {
            auto& target = targets_[target_index];

            if (!same_symbol(target.symbol_id, target.ticker, symbol_id, ticker))
            {
                continue;
            }

            // 매수 조건: 가격 ≤ buy_price (진입 — 국면 게이트)
            if (is_active() && target.buy_price > 0 && price <= target.buy_price)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - last_buy_[target_index]).count();

                if (elapsed >= target.cooldown_sec)
                {
                    last_buy_[target_index] = now;
                    LOG_INFO("[PriceTarget] BUY 조건 충족: " + std::string(ticker) +
                             " @" + std::to_string(static_cast<int>(price)) +
                             " (목표≤" + std::to_string(static_cast<int>(target.buy_price)) + ")");
                    return make_signal(target, OrderSide::BUY, price);
                }
            }

            // 매도 조건: 가격 ≥ sell_price
            if (target.sell_price > 0 && price >= target.sell_price)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - last_sell_[target_index]).count();

                if (elapsed >= target.cooldown_sec)
                {
                    last_sell_[target_index] = now;
                    LOG_INFO("[PriceTarget] SELL 조건 충족: " + std::string(ticker) +
                             " @" + std::to_string(static_cast<int>(price)) +
                             " (목표≥" + std::to_string(static_cast<int>(target.sell_price)) + ")");
                    return make_signal(target, OrderSide::SELL, price);
                }
            }
        }

        return std::nullopt;
    }

    static OrderSignal make_signal(const PriceTarget& price_target, OrderSide side, double trigger_price)
    {
        OrderSignal signal;
        signal.ticker      = price_target.ticker;
        signal.symbol_id         = price_target.symbol_id;
        signal.side        = side;
        signal.type        = OrderType::MARKET;
        signal.quantity    = price_target.quantity;
        signal.reference_price   = trigger_price;  // 시장가 명목 백스톱 기준가(트리거 현재가)
        signal.market      = Market::KR;
        signal.strategy_id = "PRICE_TARGET";
        signal.timestamp   = std::chrono::system_clock::now();
        return signal;
    }

    std::vector<PriceTarget>                              targets_;
    std::vector<LimitOrder>                               limit_orders_;
    std::vector<std::chrono::steady_clock::time_point>    last_buy_;
    std::vector<std::chrono::steady_clock::time_point>    last_sell_;
};
