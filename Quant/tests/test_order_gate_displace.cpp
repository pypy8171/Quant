// test_order_gate — 교체 진입과 진입 판정 묶음 조회.
//  공용 준비는 test_order_gate_support.h, 실행 순서는 test_order_gate.cpp의 main.
#include "test_order_gate_support.h"

#include <string>

// ─── 교체 진입 ───────────────────────────────────────────────────────────
//  슬롯이 꽉 찬 뒤에도 더 높은 점수가 오면 최약체를 비운다. 비우지 못하는 조건들(격차 부족,
//  최소 보유 미달, 점수 미상)이 실제로 막는지도 같이 본다.

void test_displace_picks_weakest()
{
    OrderGate gate(displace_config());
    gate.ledger().seed_position("", "A", 10, 1000.0);
    gate.ledger().seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 0.9}, {gate.ledger().intern_symbol("B"), 2, -0.8}, {gate.ledger().intern_symbol("C"), 3, 1.5}}, 3);

    assert(gate.slots_full());
    auto plan = gate.plan_displacement("", gate.ledger().intern_symbol("C"));
    assert(plan.ok);
    assert(plan.ticker == "B");   // z가 더 낮은 쪽
    assert(plan.quantity == 10);
    PASS("displace_picks_weakest");
}

void test_displace_needs_score_gap()
{
    OrderGate gate(displace_config());
    gate.ledger().seed_position("", "A", 10, 1000.0);
    gate.ledger().seed_position("", "B", 10, 1000.0);
    // C가 B보다 0.3σ 높을 뿐 — 임계 0.5σ 미달이라 교체하지 않는다.
    gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 0.9}, {gate.ledger().intern_symbol("B"), 2, 0.1}, {gate.ledger().intern_symbol("C"), 3, 0.4}}, 3);
    assert(!gate.plan_displacement("", gate.ledger().intern_symbol("C")).ok);
    PASS("displace_needs_score_gap");
}

void test_displace_skips_unscored_holdings()
{
    OrderGate gate(displace_config());
    gate.ledger().seed_position("", "A", 10, 1000.0);  // 점수 있음
    gate.ledger().seed_position("", "Z", 10, 1000.0);  // 점수 없음(청산 관리 보유분)
    gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 1.2}, {gate.ledger().intern_symbol("C"), 2, 1.9}}, 2);
    auto plan = gate.plan_displacement("", gate.ledger().intern_symbol("C"));
    // Z는 후보가 아니고 A는 격차(0.7σ)가 임계를 넘으므로 A가 뽑혀야 한다.
    assert(plan.ok);
    assert(plan.ticker == "A");
    PASS("displace_skips_unscored_holdings");
}

void test_displace_min_hold_blocks()
{
    auto config = displace_config();
    config.displace_min_hold_sec = 3600; // 방금 산 종목은 못 뺀다
    OrderGate gate(config);
    std::string reason;
    // 실제 매수 체결로 열어 opened_at_을 "지금"으로 만든다.
    auto b1 = make_signal("A", OrderSide::BUY, 10);
    assert(gate.check(b1, reason));
    gate.ledger().on_fill_confirmed("", "A", OrderSide::BUY, 10, 1000.0);
    auto b2 = make_signal("B", OrderSide::BUY, 10);
    assert(gate.check(b2, reason));
    gate.ledger().on_fill_confirmed("", "B", OrderSide::BUY, 10, 1000.0);

    gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 0.9}, {gate.ledger().intern_symbol("B"), 2, -0.8}, {gate.ledger().intern_symbol("C"), 3, 1.5}}, 3);
    assert(gate.slots_full());
    assert(!gate.plan_displacement("", gate.ledger().intern_symbol("C")).ok); // 최소 보유 시간 미달
    PASS("displace_min_hold_blocks");
}

void test_displace_reserves_slot_and_cooldown()
{
    OrderGate gate(displace_config());
    gate.ledger().seed_position("", "A", 10, 1000.0);
    gate.ledger().seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 0.9}, {gate.ledger().intern_symbol("B"), 2, -0.8}, {gate.ledger().intern_symbol("C"), 3, 1.5}, {gate.ledger().intern_symbol("D"), 4, 1.4}}, 4);

    auto plan = gate.plan_displacement("", gate.ledger().intern_symbol("C"));
    assert(plan.ok && plan.ticker == "B");
    gate.note_displacement(plan, gate.ledger().intern_symbol("C"));

    // B 전량 매도가 체결돼 슬롯이 하나 비었다.
    gate.ledger().on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
    assert(!gate.slots_full());

    std::string reason;
    // 밀려난 B는 쿨다운으로 재진입 불가.
    auto buy_sell_code = make_signal("B", OrderSide::BUY, 1);
    assert(!gate.check(buy_sell_code, reason));
    assert(reason.find("쿨다운") != std::string::npos);

    // 비운 자리는 D가 아니라 C의 것이다.
    auto signal_d = make_signal("D", OrderSide::BUY, 1);
    assert(!gate.check(signal_d, reason));
    assert(reason.find("예약") != std::string::npos);

    auto signal_c = make_signal("C", OrderSide::BUY, 1);
    assert(gate.check(signal_c, reason));

    // C가 자리를 가져갔으므로 예약은 풀린다.
    gate.ledger().on_fill_confirmed("", "C", OrderSide::BUY, 1, 1000.0);
    PASS("displace_reserves_slot_and_cooldown");
}

void test_displace_daily_cap()
{
    auto config = displace_config();
    config.displace_max_per_day = 1;
    config.displace_slot_hold_sec = 0; // 슬롯 예약이 아니라 횟수 상한이 막는지를 본다
    OrderGate gate(config);
    gate.ledger().seed_position("", "A", 10, 1000.0);
    gate.ledger().seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 0.9}, {gate.ledger().intern_symbol("B"), 2, -0.8}, {gate.ledger().intern_symbol("C"), 3, 1.5}}, 3);
    auto plan_a = gate.plan_displacement("", gate.ledger().intern_symbol("C"));
    assert(plan_a.ok);
    gate.note_displacement(plan_a, gate.ledger().intern_symbol("C"));
    assert(!gate.plan_displacement("", gate.ledger().intern_symbol("C")).ok); // 하루 1회 소진
    PASS("displace_daily_cap");
}

void test_entry_snapshot_matches_separate_calls()
{
    auto config = displace_config();
    config.max_concurrent_positions = 2;
    OrderGate gate(config);
    gate.ledger().seed_position("", "A", 10, 1000.0);

    auto snap_new = gate.entry_snapshot("", "Z"); // 미보유·미선점·슬롯 여유
    assert(snap_new.position == 0 && snap_new.reserved == 0 && !snap_new.slots_full);

    auto snap_held = gate.entry_snapshot("", "A");
    assert(snap_held.position == 10 && snap_held.reserved == 0);

    gate.ledger().seed_position("", "B", 10, 1000.0); // 2/2 슬롯 — 새 종목은 이제 가득 참
    auto snap_full = gate.entry_snapshot("", "Z");
    assert(snap_full.position == 0 && snap_full.reserved == 0 && snap_full.slots_full);
    assert(snap_full.slots_full == gate.slots_full());
    PASS("entry_snapshot_matches_separate_calls");
}

void run_displace_tests()
{
    test_displace_picks_weakest();
    test_displace_needs_score_gap();
    test_displace_skips_unscored_holdings();
    test_displace_min_hold_blocks();
    test_displace_reserves_slot_and_cooldown();
    test_displace_daily_cap();
    test_entry_snapshot_matches_separate_calls();
}
