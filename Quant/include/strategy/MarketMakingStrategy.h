#pragma once
#include "core/TickSize.h"
#include "strategy/StrategyBase.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// MarketMakingStrategy (MM-1) — 미니 시장조성기
//
//  매 호가(H0STASP0) 틱에서 mid_price = (best_bid + best_ask)/2 를 계산하고,
//  mid_price ± half_spread_ticks 위치에 양방향 지정가(매수/매도)를 건다.
//  시장이 requote_move_ticks 이상 이동하면 기존 견적을 취소(CANCEL)하고 재호가(NEW).
//
//  발주는 on_order_book_batch로 틱당 최대 4건(취소2+신규2)을 낸다. 실제 KIS 취소/신규는
//  주문 쪽(OrderRouter)에서만 실행한다. 전략은 out에 의도만 담고, 샤드가 봉투로 디스패치 스레드에 넘긴다.
//
//  ── 첫 컷 한계 (Phase 1) ──
//   • 체결 피드백 미수신: 자기 견적이 live인지 낙관 가정. 체결된 견적의 후속 취소는
//     KIS 에러로 자가치유(로그 + 인덱스 drop, reserved는 체결 경로가 이미 해제).
//   • 재고(inventory) 미인지: 한쪽만 체결돼도 포지션 편중을 모름 → 스큐 없음. Phase 2에서 닫음.
//   • REPLACE(정정) 대신 CANCEL+NEW 사용(단순). ODNO 유지 정정은 Phase 2.
//
//  ── 재호가 폭주 방지 ──
//   • min_requote_ms 최소 간격  AND  mid_price 이동 ≥ requote_move_ticks 일 때만 재호가.
//   • 백스톱: OrderGate 초당 5건 한도. NEW 2건/재호가라 min_requote_ms ≥ 1000 권장(초당 4건 이내).
//     (CANCEL/REPLACE는 첫 컷에서 OrderGate.check를 우회 → rate 카운트 안 됨. NEW만 카운트.)
// ─────────────────────────────────────────────────────────────────────────────
class MarketMakingStrategy : public StrategyBase
{
public:
    MarketMakingStrategy(std::string ticker, int quantity, int half_spread_ticks,
                         int requote_move_ticks, int min_requote_ms)
        : ticker_(std::move(ticker)),
          quantity_(quantity),
          half_spread_ticks_(half_spread_ticks < 1 ? 1 : half_spread_ticks),
          requote_move_ticks_(requote_move_ticks < 1 ? 1 : requote_move_ticks),
          min_requote_(std::chrono::milliseconds(min_requote_ms < 0 ? 0 : min_requote_ms))
    {
        id_ = "MM_" + ticker_;
    }

    const std::string& id() const override
    {
        return id_;
    }

    std::string describe() const override;

    // 호가 필수 → trade_only=false (H0STASP0 구독). MM은 반드시 호가를 받아야 한다.
    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{ticker_, Market::KR, "", /*trade_only=*/false}};
    }

    // 일봉 이벤트 미사용 — 인터페이스 요구(순수가상) 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    void on_start() override;

    void on_order_book_batch(const OrderBook& order_book, std::vector<OrderSignal>& out) override;

private:
    // KRX 호가단위/격자 절사는 core/TickSize.h(krx::)로 일원화. 얇은 위임만 유지.
    static double tick_size(double price)
    {
        return krx::tick_size(price);
    }

    static double round_to_tick(double price, OrderSide side)
    {
        return krx::round_to_tick(price, side);
    }

    std::string next_order_id(const char* tag)
    {
        return id() + ":" + tag + ":" + std::to_string(++sequence_);
    }

    OrderSignal make_new(const std::string& order_id, uint64_t order_number, OrderSide side, double price);

    OrderSignal make_cancel(std::string original_order_id, uint64_t original_order_number, OrderSide side); // sink: 취소할 주문 id를 신호로 옮긴다

    std::string ticker_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    symbol::SymbolId symbol_id_ = symbol::kNone; // ticker_의 id — on_start에서 한 번(미주입=kNone, 문자열 비교로 폴백)
    int quantity_;
    int half_spread_ticks_;
    int requote_move_ticks_;
    std::chrono::milliseconds min_requote_;

    // 상태 — 소유 샤드 스레드에서만 접근한다(on_order_book_batch 단일 호출자).
    std::string bid_order_id_;  // 현재 live 매수 견적 client_order_id ("" = 없음/낙관)
    std::string ask_order_id_;  // 현재 live 매도 견적 client_order_id
    uint64_t    bid_order_number_ = 0; // 같은 견적의 주문 번호 — 취소 키
    uint64_t    ask_order_number_ = 0;
    double last_mid_ = 0.0;
    std::chrono::steady_clock::time_point last_requote_{};
    uint64_t sequence_ = 0;
};
