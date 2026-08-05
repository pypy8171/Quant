#pragma once
#include "strategy/StrategyBase.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// MarketMakingStrategy (MM-1) — 미니 시장조성기
//
//  매 호가(H0STASP0) 틱에서 mid = (bid1 + ask1)/2 를 계산하고,
//  mid ± half_spread_ticks 위치에 양방향 지정가(매수/매도)를 건다.
//  시장이 requote_move_ticks 이상 이동하면 기존 견적을 취소(CANCEL)하고 재호가(NEW).
//
//  발주는 on_order_book_batch로 틱당 최대 4건(취소2+신규2)을 낸다. 실제 KIS 취소/신규는
//  order_thread(OrderRouter)에서만 실행 — 전략은 order_queue_에 의도만 push한다.
//
//  ── 첫 컷 한계 (Phase 1) ──
//   • 체결 피드백 미수신: 자기 견적이 live인지 낙관 가정. 체결된 견적의 후속 취소는
//     KIS 에러로 자가치유(로그 + 인덱스 drop, reserved는 체결 경로가 이미 해제).
//   • 재고(inventory) 미인지: 한쪽만 체결돼도 포지션 편중을 모름 → 스큐 없음. Phase 2에서 닫음.
//   • REPLACE(정정) 대신 CANCEL+NEW 사용(단순). ODNO 유지 정정은 Phase 2.
//
//  ── 재호가 폭주 방지 ──
//   • min_requote_ms 최소 간격  AND  mid 이동 ≥ requote_move_ticks 일 때만 재호가.
//   • 백스톱: OrderGate 초당 5건 한도. NEW 2건/재호가라 min_requote_ms ≥ 1000 권장(초당 4건 이내).
//     (CANCEL/REPLACE는 첫 컷에서 OrderGate.check를 우회 → rate 카운트 안 됨. NEW만 카운트.)
// ─────────────────────────────────────────────────────────────────────────────
class MarketMakingStrategy : public StrategyBase
{
public:
    MarketMakingStrategy(std::string ticker, int qty, int half_spread_ticks,
                         int requote_move_ticks, int min_requote_ms)
        : ticker_(std::move(ticker)),
          qty_(qty),
          half_spread_ticks_(half_spread_ticks < 1 ? 1 : half_spread_ticks),
          requote_move_ticks_(requote_move_ticks < 1 ? 1 : requote_move_ticks),
          min_requote_(std::chrono::milliseconds(min_requote_ms < 0 ? 0 : min_requote_ms))
    {
    }

    std::string id() const override
    {
        return "MM_" + ticker_;
    }

    std::string describe() const override
    {
        return "MarketMaking | " + ticker_ + " | qty=" + std::to_string(qty_) +
               " half_spread=" + std::to_string(half_spread_ticks_) + "tk" +
               " requote_move=" + std::to_string(requote_move_ticks_) + "tk" +
               " min_requote=" + std::to_string(min_requote_.count()) + "ms";
    }

    // 호가 필수 → trade_only=false (H0STASP0 구독). MM은 반드시 호가를 받아야 한다.
    std::vector<WatchSpec> get_watch_specs() const override
    {
        return {{ticker_, Market::KR, "", /*trade_only=*/false}};
    }

    // 일봉 이벤트 미사용 — 인터페이스 요구(순수가상) 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    void on_start() override
    {
        bid_oid_.clear();
        ask_oid_.clear();
        last_mid_ = 0.0;
        last_requote_ = std::chrono::steady_clock::time_point{};
        seq_ = 0;
    }

    void on_order_book_batch(const OrderBook& ob, std::vector<OrderSignal>& out) override
    {
        if (ob.ticker != ticker_ && !ob.ticker.empty())
            return;

        const double bid1 = ob.bids[0].price;
        const double ask1 = ob.asks[0].price;
        // 장전·비어있는 호가(구독 직후/휴장) → 견적 안 냄.
        // TODO(호가 소싱 폴백, 기본 비활성): bid1/ask1가 지속 0이면 kis_->get_current_price()로
        //   임시 mid 구성 가능하나 REST 저지연 아님 → 견적 억제가 원칙. config 플래그로만 노출 예정.
        if (bid1 <= 0.0 || ask1 <= 0.0 || ask1 < bid1)
            return;

        const double mid  = (bid1 + ask1) / 2.0;
        const double tick = tick_size(mid);

        const bool have_live = !bid_oid_.empty() || !ask_oid_.empty();
        const auto now = std::chrono::steady_clock::now();

        // ── 재호가 게이트 ──────────────────────────────────────────────────
        if (have_live)
        {
            // (a) 최소 간격 미달 → skip
            if (now - last_requote_ < min_requote_)
                return;
            // (b) mid 이동이 requote_move_ticks 미만 → skip (폭주 방지)
            if (std::fabs(mid - last_mid_) < requote_move_ticks_ * tick)
                return;
        }

        // ── 목표 견적가 (스프레드 보존: 매수 내림 / 매도 올림) ───────────────
        const double raw_bid = mid - half_spread_ticks_ * tick;
        const double raw_ask = mid + half_spread_ticks_ * tick;
        const double desired_bid = round_to_tick(raw_bid, OrderSide::BUY);
        const double desired_ask = round_to_tick(raw_ask, OrderSide::SELL);
        if (desired_bid <= 0.0 || desired_ask <= desired_bid)
            return; // 비정상 가격(교차/음수) → 이번 틱 skip

        // ── 기존 견적 취소 (있으면) ─────────────────────────────────────────
        if (!bid_oid_.empty())
        {
            out.push_back(make_cancel(bid_oid_, OrderSide::BUY));
            bid_oid_.clear();
        }
        if (!ask_oid_.empty())
        {
            out.push_back(make_cancel(ask_oid_, OrderSide::SELL));
            ask_oid_.clear();
        }

        // ── 신규 양방향 지정가 ──────────────────────────────────────────────
        bid_oid_ = next_oid("B");
        ask_oid_ = next_oid("A");
        out.push_back(make_new(bid_oid_, OrderSide::BUY, desired_bid));
        out.push_back(make_new(ask_oid_, OrderSide::SELL, desired_ask));

        last_mid_     = mid;
        last_requote_ = now;
    }

private:
    // KRX 호가단위 (2023 통합, 일반주식). 코스피/코스닥 동일 구간.
    static double tick_size(double price)
    {
        if (price < 2000)    return 1;
        if (price < 5000)    return 5;
        if (price < 20000)   return 10;
        if (price < 50000)   return 50;
        if (price < 200000)  return 100;
        if (price < 500000)  return 500;
        return 1000;
    }

    // 호가단위 격자로 절사. BUY=내림, SELL=올림 → 스프레드 보존.
    static double round_to_tick(double p, OrderSide side)
    {
        const double t = tick_size(p);
        if (t <= 0.0)
            return p;
        return (side == OrderSide::BUY) ? std::floor(p / t) * t
                                        : std::ceil(p / t) * t;
    }

    std::string next_oid(const char* tag)
    {
        return id() + ":" + tag + ":" + std::to_string(++seq_);
    }

    OrderSignal make_new(const std::string& oid, OrderSide side, double price)
    {
        OrderSignal s;
        s.ticker      = ticker_;
        s.side        = side;
        s.type        = OrderType::LIMIT;
        s.quantity    = qty_;
        s.price       = price;
        s.strategy_id = id();
        s.market      = Market::KR;
        s.action      = OrderAction::NEW;
        s.client_oid  = oid;
        s.timestamp   = std::chrono::system_clock::now();
        return s;
    }

    OrderSignal make_cancel(const std::string& orig_oid, OrderSide side)
    {
        OrderSignal s;
        s.ticker         = ticker_;
        s.side           = side; // 참고용(취소 라우팅은 원주문 정보 사용). Engine NONE 가드는 action으로 우회.
        s.type           = OrderType::LIMIT;
        s.quantity       = 0;
        s.strategy_id    = id();
        s.market         = Market::KR;
        s.action         = OrderAction::CANCEL;
        s.orig_client_oid = orig_oid;
        s.timestamp      = std::chrono::system_clock::now();
        return s;
    }

    std::string ticker_;
    int qty_;
    int half_spread_ticks_;
    int requote_move_ticks_;
    std::chrono::milliseconds min_requote_;

    // 상태 — order_thread가 아닌 strategy_thread에서만 접근(on_order_book_batch 단일 호출자).
    std::string bid_oid_;  // 현재 live 매수 견적 client_oid ("" = 없음/낙관)
    std::string ask_oid_;  // 현재 live 매도 견적 client_oid
    double last_mid_ = 0.0;
    std::chrono::steady_clock::time_point last_requote_{};
    uint64_t seq_ = 0;
};
