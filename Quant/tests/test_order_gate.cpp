// OrderGate 단위 테스트
// 빌드: cmake --build <dir> --target test_order_gate
// 실행: ./test_order_gate
//
// 테스트 항목:
//   1. Kill switch 차단
//   2. 포지션 한도 초과 차단
//   3. 일일 손실 한도 차단
//   4. 초당 Rate limit 차단
//   5. 중복 신호 차단
//   6. 정상 주문 통과
//   7. SELL은 포지션 한도 미적용
//   8. 중복 신호는 rate slot 소모 안 함 (C5 fix)
//   9. SELL on_accept은 포지션 0 미만 방지 (C6 fix)
//  17. 시장가 1주문 명목 백스톱 — BUY는 ref_price로 거부, SELL은 경고만 하고 통과,
//      ref_price 없으면 검사 자체가 없음(게이트가 못 잡는 현행을 기록)

#include "risk/OrderGate.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

static OrderSignal make_signal(const std::string& ticker, OrderSide side, int qty = 1)
{
    OrderSignal s;
    s.ticker      = ticker;
    s.side        = side;
    s.quantity    = qty;
    s.price       = 100000.0;
    s.strategy_id = "TEST";
    s.market      = Market::KR;
    return s;
}

static void PASS(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl; // 뒤 테스트가 assert로 죽어도 여기까지는 남게 flush
}

// ─── 테스트 1: Kill switch ────────────────────────────────────────────────
void test_kill_switch()
{
    OrderGate gate;
    gate.set_kill_switch(true);

    std::string reason;
    auto sig = make_signal("005930", OrderSide::BUY);
    assert(!gate.check(sig, reason));
    assert(reason.find("KILL") != std::string::npos);
    PASS("kill_switch");
}

// ─── 테스트 2: 포지션 한도 ───────────────────────────────────────────────
void test_position_limit()
{
    OrderGate::Config cfg;
    cfg.max_qty_per_ticker = 5;
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    OrderGate gate(cfg);

    std::string reason;
    // 3주 매수 → 통과 후 포지션 등록
    auto sig3 = make_signal("005930", OrderSide::BUY, 3);
    assert(gate.check(sig3, reason));
    gate.on_accept("005930", OrderSide::BUY, 3, 100000);

    // 추가 3주 시도 → 합산 6 > 5, 차단
    assert(!gate.check(sig3, reason));
    assert(reason.find("한도") != std::string::npos);
    PASS("position_limit");
}

// ─── 테스트 3: 일일 손실 한도 ────────────────────────────────────────────
void test_daily_loss_limit()
{
    OrderGate::Config cfg;
    cfg.daily_loss_limit   = -100000.0;
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    OrderGate gate(cfg);

    gate.add_realized_pnl(-100001.0); // 한도 초과

    std::string reason;
    auto sig = make_signal("005930", OrderSide::BUY);
    assert(!gate.check(sig, reason));
    assert(reason.find("손실") != std::string::npos);
    PASS("daily_loss_limit");
}

// ─── 테스트 4: 초당 Rate limit ────────────────────────────────────────────
void test_rate_limit_per_sec()
{
    OrderGate::Config cfg;
    cfg.max_orders_per_sec = 3;
    cfg.max_orders_per_min = 100;
    cfg.dedup_window_sec   = 0.0; // dedup 비활성
    OrderGate gate(cfg);

    std::string reason;
    // 3건 연속 통과
    for (int i = 0; i < 3; ++i)
    {
        auto sig = make_signal("00593" + std::to_string(i), OrderSide::BUY);
        assert(gate.check(sig, reason));
    }
    // 4번째 → 초당 한도 초과
    auto sig4 = make_signal("005934", OrderSide::BUY);
    assert(!gate.check(sig4, reason));
    assert(reason.find("초당") != std::string::npos);
    PASS("rate_limit_per_sec");
}

// ─── 테스트 5: 중복 신호 ─────────────────────────────────────────────────
void test_dedup()
{
    OrderGate::Config cfg;
    cfg.dedup_window_sec   = 2.0;
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    OrderGate gate(cfg);

    std::string reason;
    auto sig = make_signal("005930", OrderSide::BUY);
    assert(gate.check(sig, reason));  // 첫 번째: 통과
    assert(!gate.check(sig, reason)); // 즉시 재시도: 차단
    assert(reason.find("중복") != std::string::npos);
    PASS("dedup");
}

// ─── 테스트 6: 정상 통과 ─────────────────────────────────────────────────
void test_normal_pass()
{
    OrderGate::Config cfg;
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    cfg.dedup_window_sec   = 0.0;
    OrderGate gate(cfg);

    std::string reason;
    auto sig = make_signal("005930", OrderSide::BUY, 1);
    assert(gate.check(sig, reason));
    PASS("normal_pass");
}

// ─── 테스트 7: SELL은 포지션 한도 미적용 ─────────────────────────────────
void test_sell_bypasses_position_check()
{
    OrderGate::Config cfg;
    cfg.max_qty_per_ticker = 0; // BUY 완전 차단
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    cfg.dedup_window_sec   = 0.0;
    OrderGate gate(cfg);

    std::string reason;
    auto sell = make_signal("005930", OrderSide::SELL, 1);
    assert(gate.check(sell, reason)); // SELL은 포지션 한도 무관
    PASS("sell_bypasses_position_check");
}

// ─── 테스트 8: 중복 신호는 rate slot 소모 안 함 (C5 fix) ─────────────────
void test_dedup_does_not_consume_rate_slot()
{
    OrderGate::Config cfg;
    cfg.max_orders_per_sec = 2;   // 초당 2건 허용
    cfg.max_orders_per_min = 100;
    cfg.dedup_window_sec   = 10.0; // 10초 dedup
    OrderGate gate(cfg);

    std::string reason;
    auto sig = make_signal("005930", OrderSide::BUY);

    // 첫 번째 통과 (rate slot 1 소모)
    assert(gate.check(sig, reason));

    // 두 번째: dedup 차단 — rate slot 소모 없어야 함
    assert(!gate.check(sig, reason));
    assert(reason.find("중복") != std::string::npos);

    // 다른 ticker로 두 번 더 시도 (rate slot 2번 소모 → 한도 도달)
    assert(gate.check(make_signal("005935", OrderSide::BUY), reason)); // slot 2
    // 이제 rate 한도 초과 (slot 3이므로 차단)
    assert(!gate.check(make_signal("005936", OrderSide::BUY), reason));
    assert(reason.find("초당") != std::string::npos);
    PASS("dedup_does_not_consume_rate_slot");
}

// ─── 테스트 9: SELL 체결이 실보유를 0 미만으로 내리지 않음 (C6 + C2/C4) ──
//   원장 분리 후: on_accept는 reserved_만, 실보유 positions_는 on_fill_confirmed가 갱신
void test_sell_clamps_position_at_zero()
{
    OrderGate::Config cfg;
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    cfg.dedup_window_sec   = 0.0;
    OrderGate gate(cfg);

    // BUY 2주 접수→체결 → 실보유 2, 선점 해제
    gate.on_accept("005930", OrderSide::BUY, 2, 100000);
    gate.on_fill_confirmed("005930", OrderSide::BUY, 2, 100000);
    assert(gate.position("005930") == 2);
    assert(gate.reserved("005930") == 0);

    // SELL 5주 접수→체결 (보유 2 초과) → 실보유 0으로 클램프 (공매도 미지원)
    gate.on_accept("005930", OrderSide::SELL, 5, 100000);
    gate.on_fill_confirmed("005930", OrderSide::SELL, 5, 100000);
    assert(gate.position("005930") == 0);
    PASS("sell_clamps_position_at_zero");
}

// ─── 테스트 10: 부분체결 평단 정확성 (C2/C4 fix) ───────────────────────────
//   on_accept 선점값이 아니라 실체결 수량으로 평단을 계산해야 함
void test_partial_fill_avg_price()
{
    OrderGate::Config cfg;
    cfg.max_qty_per_ticker = 100;
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    cfg.dedup_window_sec   = 0.0;
    OrderGate gate(cfg);

    gate.on_accept("005930", OrderSide::BUY, 10, 1000.0); // 선점 10
    assert(gate.reserved("005930") == 10);

    // 5주 부분체결 @1000 → 평단 1000 (구버그라면 분모=선점10 → 500)
    auto r1 = gate.on_fill_confirmed("005930", OrderSide::BUY, 5, 1000.0);
    assert(r1.net_qty == 5);
    assert(r1.avg_price > 999.9 && r1.avg_price < 1000.1);
    assert(gate.reserved("005930") == 5); // 체결분만큼 선점 해제

    // 나머지 5주 체결 @1100 → 평단 (5*1000 + 5*1100)/10 = 1050
    auto r2 = gate.on_fill_confirmed("005930", OrderSide::BUY, 5, 1100.0);
    assert(r2.net_qty == 10);
    assert(r2.avg_price > 1049.9 && r2.avg_price < 1050.1);
    assert(gate.position("005930") == 10);
    assert(gate.reserved("005930") == 0); // 선점 전부 해제
    PASS("partial_fill_avg_price");
}

// ─── 교체 진입 ───────────────────────────────────────────────────────────
//  슬롯이 꽉 찬 뒤에도 더 높은 점수가 오면 최약체를 비운다. 비우지 못하는 조건들(격차 부족,
//  최소 보유 미달, 점수 미상)이 실제로 막는지도 같이 본다.
static OrderGate::Config displace_cfg()
{
    OrderGate::Config cfg;
    cfg.max_concurrent_positions = 2;
    cfg.max_qty_per_ticker       = 1000;
    cfg.max_orders_per_min       = 1000;
    cfg.max_orders_per_sec       = 1000;
    cfg.displace_enabled         = true;
    cfg.displace_min_z_gap       = 0.5;
    cfg.displace_min_hold_sec    = 0; // 테스트에선 보유시간 조건을 끈다(별도 케이스에서 검증)
    return cfg;
}

void test_displace_picks_weakest()
{
    OrderGate gate(displace_cfg());
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{"A", 1}, {"B", 2}, {"C", 3}},
                            {{"A", 0.9}, {"B", -0.8}, {"C", 1.5}}, 3);

    assert(gate.slots_full());
    auto plan = gate.plan_displacement("", "C");
    assert(plan.ok);
    assert(plan.ticker == "B");   // z가 더 낮은 쪽
    assert(plan.qty == 10);
    PASS("displace_picks_weakest");
}

void test_displace_needs_score_gap()
{
    OrderGate gate(displace_cfg());
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    // C가 B보다 0.3σ 높을 뿐 — 임계 0.5σ 미달이라 교체하지 않는다.
    gate.set_entry_priority({{"A", 1}, {"B", 2}, {"C", 3}},
                            {{"A", 0.9}, {"B", 0.1}, {"C", 0.4}}, 3);
    assert(!gate.plan_displacement("", "C").ok);
    PASS("displace_needs_score_gap");
}

void test_displace_skips_unscored_holdings()
{
    OrderGate gate(displace_cfg());
    gate.seed_position("", "A", 10, 1000.0);  // 점수 있음
    gate.seed_position("", "Z", 10, 1000.0);  // 점수 없음(청산 관리 보유분)
    gate.set_entry_priority({{"A", 1}, {"C", 2}}, {{"A", 1.2}, {"C", 1.9}}, 2);
    auto plan = gate.plan_displacement("", "C");
    // Z는 후보가 아니고 A는 격차(0.7σ)가 임계를 넘으므로 A가 뽑혀야 한다.
    assert(plan.ok);
    assert(plan.ticker == "A");
    PASS("displace_skips_unscored_holdings");
}

void test_displace_min_hold_blocks()
{
    auto cfg = displace_cfg();
    cfg.displace_min_hold_sec = 3600; // 방금 산 종목은 못 뺀다
    OrderGate gate(cfg);
    std::string reason;
    // 실제 매수 체결로 열어 opened_at_을 "지금"으로 만든다.
    auto b1 = make_signal("A", OrderSide::BUY, 10);
    assert(gate.check(b1, reason));
    gate.on_fill_confirmed("", "A", OrderSide::BUY, 10, 1000.0);
    auto b2 = make_signal("B", OrderSide::BUY, 10);
    assert(gate.check(b2, reason));
    gate.on_fill_confirmed("", "B", OrderSide::BUY, 10, 1000.0);

    gate.set_entry_priority({{"A", 1}, {"B", 2}, {"C", 3}},
                            {{"A", 0.9}, {"B", -0.8}, {"C", 1.5}}, 3);
    assert(gate.slots_full());
    assert(!gate.plan_displacement("", "C").ok); // 최소 보유 시간 미달
    PASS("displace_min_hold_blocks");
}

void test_displace_reserves_slot_and_cooldown()
{
    OrderGate gate(displace_cfg());
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{"A", 1}, {"B", 2}, {"C", 3}, {"D", 4}},
                            {{"A", 0.9}, {"B", -0.8}, {"C", 1.5}, {"D", 1.4}}, 4);

    auto plan = gate.plan_displacement("", "C");
    assert(plan.ok && plan.ticker == "B");
    gate.note_displacement(plan, "C");

    // B 전량 매도가 체결돼 슬롯이 하나 비었다.
    gate.on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
    assert(!gate.slots_full());

    std::string reason;
    // 밀려난 B는 쿨다운으로 재진입 불가.
    auto sb = make_signal("B", OrderSide::BUY, 1);
    assert(!gate.check(sb, reason));
    assert(reason.find("쿨다운") != std::string::npos);

    // 비운 자리는 D가 아니라 C의 것이다.
    auto sd = make_signal("D", OrderSide::BUY, 1);
    assert(!gate.check(sd, reason));
    assert(reason.find("예약") != std::string::npos);

    auto sc = make_signal("C", OrderSide::BUY, 1);
    assert(gate.check(sc, reason));

    // C가 자리를 가져갔으므로 예약은 풀린다.
    gate.on_fill_confirmed("", "C", OrderSide::BUY, 1, 1000.0);
    PASS("displace_reserves_slot_and_cooldown");
}

void test_displace_daily_cap()
{
    auto cfg = displace_cfg();
    cfg.displace_max_per_day = 1;
    cfg.displace_slot_hold_sec = 0; // 슬롯 예약이 아니라 횟수 상한이 막는지를 본다
    OrderGate gate(cfg);
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{"A", 1}, {"B", 2}, {"C", 3}},
                            {{"A", 0.9}, {"B", -0.8}, {"C", 1.5}}, 3);
    auto p1 = gate.plan_displacement("", "C");
    assert(p1.ok);
    gate.note_displacement(p1, "C");
    assert(!gate.plan_displacement("", "C").ok); // 하루 1회 소진
    PASS("displace_daily_cap");
}

// ─── 테스트 17: 시장가 명목 백스톱과 ref_price ────────────────────────────
//   시장가는 price=0이라 eval_px가 ref_price로 떨어진다. 전략이 ref_price를 안 찍으면
//   명목 검사 자체가 건너뛰어진다(현 설계). SELL은 청산 계열이라 초과해도 통과시킨다.
static OrderSignal make_market(OrderSide side, int qty, double ref_price)
{
    auto s = make_signal("005930", side, qty);
    s.type      = OrderType::MARKET;
    s.price     = 0.0;
    s.ref_price = ref_price;
    return s;
}

static OrderGate::Config notional_cfg()
{
    OrderGate::Config cfg;
    cfg.max_orders_per_min = 100;
    cfg.max_orders_per_sec = 100;
    cfg.dedup_window_sec   = 0.0;
    cfg.max_qty_per_ticker = 100'000;
    cfg.max_qty_per_order  = 10'000;
    cfg.max_notional_per_order = 50'000'000.0;
    return cfg;
}

void test_market_sell_ref_price_notional()
{
    OrderGate gate(notional_cfg());
    std::string reason;
    // 1,000주 × 100,000원 = 1억 > 5천만. SELL(청산)은 통과, BUY는 거부.
    assert(gate.check(make_market(OrderSide::SELL, 1000, 100000.0), reason));
    assert(!gate.check(make_market(OrderSide::BUY, 1000, 100000.0), reason));
    assert(reason.find("명목") != std::string::npos);
    // 참조가가 낮아 명목이 상한 안이면 BUY도 통과
    assert(gate.check(make_market(OrderSide::BUY, 1000, 10000.0), reason));
    PASS("market_sell_ref_price_notional");
}

void test_market_sell_without_ref_price_bypasses_notional()
{
    OrderGate gate(notional_cfg());
    std::string reason;
    // price=0, ref_price=0 → 평가가가 없어 명목 검사를 건너뛴다(수량 한도만). BUY도 통과가 현행이다.
    assert(gate.check(make_market(OrderSide::SELL, 1000, 0.0), reason));
    assert(gate.check(make_market(OrderSide::BUY, 1000, 0.0), reason));
    // 수량 한도는 여전히 산다
    assert(!gate.check(make_market(OrderSide::SELL, 10'001, 0.0), reason));
    assert(reason.find("수량") != std::string::npos);
    PASS("market_sell_without_ref_price_bypasses_notional");
}

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== OrderGate Unit Tests ===\n";
    test_kill_switch();
    test_position_limit();
    test_daily_loss_limit();
    test_rate_limit_per_sec();
    test_dedup();
    test_normal_pass();
    test_sell_bypasses_position_check();
    test_dedup_does_not_consume_rate_slot();
    test_sell_clamps_position_at_zero();
    test_partial_fill_avg_price();
    test_market_sell_ref_price_notional();
    test_market_sell_without_ref_price_bypasses_notional();
    test_displace_picks_weakest();
    test_displace_needs_score_gap();
    test_displace_skips_unscored_holdings();
    test_displace_min_hold_blocks();
    test_displace_reserves_slot_and_cooldown();
    test_displace_daily_cap();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
