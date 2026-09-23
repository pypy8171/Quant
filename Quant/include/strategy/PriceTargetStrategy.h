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
//    해당 종목의 첫 호가·체결 이벤트에서 지정가 주문을 1회 낸다(세션 검사 없음, BUY는 국면 게이트)
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

    const std::string& id() const override;

    std::string describe() const override;

    std::vector<WatchSpec> get_watch_specifications() const override;

    void on_start() override;

    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 호가 이벤트 — 매도호가[0]을 현재가 대리로 삼아 가격 체크(없으면 매수호가[0]로 폴백)
    std::optional<OrderSignal> on_order_book(const OrderBook& order_book) override;

    // 체결 이벤트 — 체결가 기준 가격 체크
    std::optional<OrderSignal> on_trade(const TradeData& trade) override;

    void on_stop() override;

private:
    // ── 예약 지정가 주문 (1회) ────────────────────────────────────────────
    std::optional<OrderSignal> check_limit_order(symbol::SymbolId symbol_id, std::string_view ticker);

    // ── 가격 목표 도달 시 시장가 주문 ────────────────────────────────────
    std::optional<OrderSignal> check_price_target(symbol::SymbolId symbol_id,
                                                   std::string_view ticker,
                                                   double price,
                                                   int32_t hhmmss);

    static OrderSignal make_signal(const PriceTarget& price_target, OrderSide side, double trigger_price);

    std::vector<PriceTarget>                              targets_;
    std::vector<LimitOrder>                               limit_orders_;
    std::vector<std::chrono::steady_clock::time_point>    last_buy_;
    std::vector<std::chrono::steady_clock::time_point>    last_sell_;
};
