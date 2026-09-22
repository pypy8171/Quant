// OrderGate 단위 테스트
// 빌드: cmake --build <directory> --target test_order_gate
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
//  18. 바스켓 슬롯 제외 — 제외 종목은 동시보유 상한·빈 슬롯 수·교체 후보에 안 들고 snapshot에 표시된다 (D-109)
//  17. 시장가 1주문 명목 백스톱 — BUY는 ref_price로 거부, SELL은 경고만 하고 통과,
//      reference_price 없으면 검사 자체가 없음(게이트가 못 잡는 현행을 기록)
//  19. 원장 저널 — 재기동 리플레이가 보유·평단·선점·매도가능·당일손익·현금을 되살린다 (D-113)
//  20. 원장 저널 — 쓰다 만 꼬리(전원 장애)는 리플레이가 거기서 멈추고 다음 기동이 잘라 낸다
//  21. 원장 저널 — 한 레코드가 깨지면(CRC 불일치) 그 앞까지만 적용하고 뒤는 버린다
//  22. 진입 정지 원천 셋(국면·사람·전략 사망)이 서로를 안 지운다 (D-114)
//  23. 장부 사본이 원본과 같은 값을 싣는다 — 보유·선점·매도가능·평단·면제·전역값 전부 (D-114)

#include "risk/OrderGate.h"
#include "ipc/LedgerSnapshot.h"
#include "core/KstTime.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

static OrderSignal make_signal(const std::string& ticker, OrderSide side, int quantity = 1)
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.quantity    = quantity;
    signal.price       = 100000.0;
    signal.strategy_id = "TEST";
    signal.market      = Market::KR;
    return signal;
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
    auto signal = make_signal("005930", OrderSide::BUY);
    assert(!gate.check(signal, reason));
    assert(reason.find("KILL") != std::string::npos);
    PASS("kill_switch");
}

// ─── 테스트 2: 포지션 한도 ───────────────────────────────────────────────
void test_position_limit()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker = 5;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    OrderGate gate(config);

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
    OrderGate::Config config;
    config.daily_loss_limit   = -100000.0;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    OrderGate gate(config);

    gate.add_realized_pnl(-100001.0); // 한도 초과

    std::string reason;
    auto signal = make_signal("005930", OrderSide::BUY);
    assert(!gate.check(signal, reason));
    assert(reason.find("손실") != std::string::npos);
    PASS("daily_loss_limit");
}

// ─── 테스트 4: 초당 Rate limit ────────────────────────────────────────────
void test_rate_limit_per_sec()
{
    OrderGate::Config config;
    config.max_orders_per_sec = 3;
    config.max_orders_per_min = 100;
    config.deduplicate_window_sec   = 0.0; // deduplicate 비활성
    OrderGate gate(config);

    std::string reason;

    // 3건 연속 통과
    for (int index = 0; index < 3; ++index)
    {
        auto signal = make_signal("00593" + std::to_string(index), OrderSide::BUY);
        assert(gate.check(signal, reason));
    }

    // 4번째 → 초당 한도 초과
    auto sig4 = make_signal("005934", OrderSide::BUY);
    assert(!gate.check(sig4, reason));
    assert(reason.find("초당") != std::string::npos);
    PASS("rate_limit_per_sec");
}

// ─── 테스트 5: 중복 신호 ─────────────────────────────────────────────────
void test_deduplicate()
{
    OrderGate::Config config;
    config.deduplicate_window_sec   = 2.0;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    OrderGate gate(config);

    std::string reason;
    auto signal = make_signal("005930", OrderSide::BUY);
    assert(gate.check(signal, reason));  // 첫 번째: 통과
    assert(!gate.check(signal, reason)); // 즉시 재시도: 차단
    assert(reason.find("중복") != std::string::npos);
    PASS("dedup");
}

// ─── 테스트 6: 정상 통과 ─────────────────────────────────────────────────
void test_normal_pass()
{
    OrderGate::Config config;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    config.deduplicate_window_sec   = 0.0;
    OrderGate gate(config);

    std::string reason;
    auto signal = make_signal("005930", OrderSide::BUY, 1);
    assert(gate.check(signal, reason));
    PASS("normal_pass");
}

// ─── 테스트 7: SELL은 포지션 한도 미적용 ─────────────────────────────────
void test_sell_bypasses_position_check()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker = 0; // BUY 완전 차단
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    config.deduplicate_window_sec   = 0.0;
    OrderGate gate(config);

    std::string reason;
    auto sell = make_signal("005930", OrderSide::SELL, 1);
    assert(gate.check(sell, reason)); // SELL은 포지션 한도 무관
    PASS("sell_bypasses_position_check");
}

// ─── 테스트 8: 중복 신호는 rate slot 소모 안 함 (C5 fix) ─────────────────
void test_deduplicate_does_not_consume_rate_slot()
{
    OrderGate::Config config;
    config.max_orders_per_sec = 2;   // 초당 2건 허용
    config.max_orders_per_min = 100;
    config.deduplicate_window_sec   = 10.0; // 10초 deduplicate
    OrderGate gate(config);

    std::string reason;
    auto signal = make_signal("005930", OrderSide::BUY);

    // 첫 번째 통과 (rate slot 1 소모)
    assert(gate.check(signal, reason));

    // 두 번째: deduplicate 차단 — rate slot 소모 없어야 함
    assert(!gate.check(signal, reason));
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
    OrderGate::Config config;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    config.deduplicate_window_sec   = 0.0;
    OrderGate gate(config);

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
void test_partial_fill_average_price()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker = 100;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    config.deduplicate_window_sec   = 0.0;
    OrderGate gate(config);

    gate.on_accept("005930", OrderSide::BUY, 10, 1000.0); // 선점 10
    assert(gate.reserved("005930") == 10);

    // 5주 부분체결 @1000 → 평단 1000 (구버그라면 분모=선점10 → 500)
    auto fill_a = gate.on_fill_confirmed("005930", OrderSide::BUY, 5, 1000.0);
    assert(fill_a.net_quantity == 5);
    assert(fill_a.average_price > 999.9 && fill_a.average_price < 1000.1);
    assert(gate.reserved("005930") == 5); // 체결분만큼 선점 해제

    // 나머지 5주 체결 @1100 → 평단 (5*1000 + 5*1100)/10 = 1050
    auto fill_b = gate.on_fill_confirmed("005930", OrderSide::BUY, 5, 1100.0);
    assert(fill_b.net_quantity == 10);
    assert(fill_b.average_price > 1049.9 && fill_b.average_price < 1050.1);
    assert(gate.position("005930") == 10);
    assert(gate.reserved("005930") == 0); // 선점 전부 해제
    PASS("partial_fill_avg_price");
}

// ─── 교체 진입 ───────────────────────────────────────────────────────────
//  슬롯이 꽉 찬 뒤에도 더 높은 점수가 오면 최약체를 비운다. 비우지 못하는 조건들(격차 부족,
//  최소 보유 미달, 점수 미상)이 실제로 막는지도 같이 본다.
static OrderGate::Config displace_config()
{
    OrderGate::Config config;
    config.max_concurrent_positions = 2;
    config.max_quantity_per_ticker       = 1000;
    config.max_orders_per_min       = 1000;
    config.max_orders_per_sec       = 1000;
    config.displace_enabled         = true;
    config.displace_min_z_gap       = 0.5;
    config.displace_min_hold_sec    = 0; // 테스트에선 보유시간 조건을 끈다(별도 케이스에서 검증)
    return config;
}

void test_displace_picks_weakest()
{
    OrderGate gate(displace_config());
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 0.9}, {gate.intern_symbol("B"), 2, -0.8}, {gate.intern_symbol("C"), 3, 1.5}}, 3);

    assert(gate.slots_full());
    auto plan = gate.plan_displacement("", gate.intern_symbol("C"));
    assert(plan.ok);
    assert(plan.ticker == "B");   // z가 더 낮은 쪽
    assert(plan.quantity == 10);
    PASS("displace_picks_weakest");
}

void test_displace_needs_score_gap()
{
    OrderGate gate(displace_config());
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    // C가 B보다 0.3σ 높을 뿐 — 임계 0.5σ 미달이라 교체하지 않는다.
    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 0.9}, {gate.intern_symbol("B"), 2, 0.1}, {gate.intern_symbol("C"), 3, 0.4}}, 3);
    assert(!gate.plan_displacement("", gate.intern_symbol("C")).ok);
    PASS("displace_needs_score_gap");
}

void test_displace_skips_unscored_holdings()
{
    OrderGate gate(displace_config());
    gate.seed_position("", "A", 10, 1000.0);  // 점수 있음
    gate.seed_position("", "Z", 10, 1000.0);  // 점수 없음(청산 관리 보유분)
    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 1.2}, {gate.intern_symbol("C"), 2, 1.9}}, 2);
    auto plan = gate.plan_displacement("", gate.intern_symbol("C"));
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
    gate.on_fill_confirmed("", "A", OrderSide::BUY, 10, 1000.0);
    auto b2 = make_signal("B", OrderSide::BUY, 10);
    assert(gate.check(b2, reason));
    gate.on_fill_confirmed("", "B", OrderSide::BUY, 10, 1000.0);

    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 0.9}, {gate.intern_symbol("B"), 2, -0.8}, {gate.intern_symbol("C"), 3, 1.5}}, 3);
    assert(gate.slots_full());
    assert(!gate.plan_displacement("", gate.intern_symbol("C")).ok); // 최소 보유 시간 미달
    PASS("displace_min_hold_blocks");
}

void test_displace_reserves_slot_and_cooldown()
{
    OrderGate gate(displace_config());
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 0.9}, {gate.intern_symbol("B"), 2, -0.8}, {gate.intern_symbol("C"), 3, 1.5}, {gate.intern_symbol("D"), 4, 1.4}}, 4);

    auto plan = gate.plan_displacement("", gate.intern_symbol("C"));
    assert(plan.ok && plan.ticker == "B");
    gate.note_displacement(plan, gate.intern_symbol("C"));

    // B 전량 매도가 체결돼 슬롯이 하나 비었다.
    gate.on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
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
    gate.on_fill_confirmed("", "C", OrderSide::BUY, 1, 1000.0);
    PASS("displace_reserves_slot_and_cooldown");
}

void test_displace_daily_cap()
{
    auto config = displace_config();
    config.displace_max_per_day = 1;
    config.displace_slot_hold_sec = 0; // 슬롯 예약이 아니라 횟수 상한이 막는지를 본다
    OrderGate gate(config);
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 0.9}, {gate.intern_symbol("B"), 2, -0.8}, {gate.intern_symbol("C"), 3, 1.5}}, 3);
    auto plan_a = gate.plan_displacement("", gate.intern_symbol("C"));
    assert(plan_a.ok);
    gate.note_displacement(plan_a, gate.intern_symbol("C"));
    assert(!gate.plan_displacement("", gate.intern_symbol("C")).ok); // 하루 1회 소진
    PASS("displace_daily_cap");
}

void test_entry_snapshot_matches_separate_calls()
{
    auto config = displace_config();
    config.max_concurrent_positions = 2;
    OrderGate gate(config);
    gate.seed_position("", "A", 10, 1000.0);

    auto snap_new = gate.entry_snapshot("", "Z"); // 미보유·미선점·슬롯 여유
    assert(snap_new.position == 0 && snap_new.reserved == 0 && !snap_new.slots_full);

    auto snap_held = gate.entry_snapshot("", "A");
    assert(snap_held.position == 10 && snap_held.reserved == 0);

    gate.seed_position("", "B", 10, 1000.0); // 2/2 슬롯 — 새 종목은 이제 가득 참
    auto snap_full = gate.entry_snapshot("", "Z");
    assert(snap_full.position == 0 && snap_full.reserved == 0 && snap_full.slots_full);
    assert(snap_full.slots_full == gate.slots_full());
    PASS("entry_snapshot_matches_separate_calls");
}

// ─── 테스트 17: 시장가 명목 백스톱과 reference_price ────────────────────────────
//   시장가는 price=0이라 eval_px가 ref_price로 떨어진다. 전략이 ref_price를 안 찍으면
//   명목 검사 자체가 건너뛰어진다(현 설계). SELL은 청산 계열이라 초과해도 통과시킨다.
static OrderSignal make_market(OrderSide side, int quantity, double reference_price)
{
    auto signal = make_signal("005930", side, quantity);
    signal.type      = OrderType::MARKET;
    signal.price     = 0.0;
    signal.reference_price = reference_price;
    return signal;
}

static OrderGate::Config notional_config()
{
    OrderGate::Config config;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    config.deduplicate_window_sec   = 0.0;
    config.max_quantity_per_ticker = 100'000;
    config.max_quantity_per_order  = 10'000;
    config.max_notional_per_order = 50'000'000.0;
    return config;
}

void test_market_sell_reference_price_notional()
{
    OrderGate gate(notional_config());
    std::string reason;
    // 1,000주 × 100,000원 = 1억 > 5천만. SELL(청산)은 통과, BUY는 거부.
    assert(gate.check(make_market(OrderSide::SELL, 1000, 100000.0), reason));
    assert(!gate.check(make_market(OrderSide::BUY, 1000, 100000.0), reason));
    assert(reason.find("명목") != std::string::npos);
    // 참조가가 낮아 명목이 상한 안이면 BUY도 통과
    assert(gate.check(make_market(OrderSide::BUY, 1000, 10000.0), reason));
    PASS("market_sell_ref_price_notional");
}

void test_market_sell_without_reference_price_bypasses_notional()
{
    OrderGate gate(notional_config());
    std::string reason;
    // price=0, reference_price=0 → 평가가가 없어 명목 검사를 건너뛴다(수량 한도만). BUY도 통과가 현행이다.
    assert(gate.check(make_market(OrderSide::SELL, 1000, 0.0), reason));
    assert(gate.check(make_market(OrderSide::BUY, 1000, 0.0), reason));
    // 수량 한도는 여전히 산다
    assert(!gate.check(make_market(OrderSide::SELL, 10'001, 0.0), reason));
    assert(reason.find("수량") != std::string::npos);
    PASS("market_sell_without_ref_price_bypasses_notional");
}

// ─── 테스트: 매매 세션 창(D-096) — 창 밖 NEW는 거부, 창 안·창 없음(0/0)·CANCEL은 통과 ──────────
// 진입 정지는 원천이 셋이다 — 국면(RegimeFileJudge가 자동으로 켜고 끈다)·사람(운영단말)·전략 사망(주문 스레드).
//  한 원천의 해제가 다른 원천이 켠 정지를 지우면 급락장에 신규 매수가 되살아난다. OR로만 합치는지 본다. [why D-114]
void test_halt_sources_are_independent()
{
    OrderGate::Config config;
    OrderGate         gate(config);

    assert(!gate.is_entry_halted());

    // 전략이 죽어 주문 스레드가 켠다.
    gate.set_strategy_down_halt(true);
    assert(gate.is_entry_halted());

    // 국면이 "위험 없음"으로 자동 해제해도 전략 사망 정지는 남는다.
    gate.set_entry_halt(false);
    assert(gate.is_entry_halted());

    // 사람이 수동 정지를 풀어도 남는다.
    gate.set_manual_halt(OrderSide::BUY, false);
    assert(gate.is_entry_halted());

    // 박동이 돌아와 주문 스레드가 끄면 비로소 풀린다.
    gate.set_strategy_down_halt(false);
    assert(!gate.is_entry_halted());

    // 거꾸로도 같다 — 전략 사망 해제가 국면·사람이 켠 정지를 못 지운다.
    gate.set_entry_halt(true);
    gate.set_manual_halt(OrderSide::BUY, true);
    gate.set_strategy_down_halt(true);
    gate.set_strategy_down_halt(false);
    assert(gate.is_entry_halted());
    assert(gate.is_manual_buy_halted());

    gate.set_entry_halt(false);
    assert(gate.is_entry_halted()); // 사람이 켠 것이 남아 있다
    gate.set_manual_halt(OrderSide::BUY, false);
    assert(!gate.is_entry_halted());

    std::cout << "[PASS] 진입 정지 원천 셋이 서로를 안 지운다\n";
}

void test_session_window()
{
    // 지금 KST 분 — 창을 "지금을 품는 구간"과 "지금을 피하는 구간"으로 만들어 시계에 기대지 않는다.
    const auto time_of_day_now = kst::time_of_day(std::time(nullptr));
    const int  now_min = static_cast<int>(time_of_day_now.hours().count() * 60 + time_of_day_now.minutes().count());
    std::string reason;

    {
        OrderGate::Config config;
        config.session_open_min  = (std::max)(0, now_min - 30);       // <windows.h>의 max/min 매크로 회피
        config.session_close_min = (std::min)(24 * 60, now_min + 30);
        OrderGate gate(config);
        auto      signal = make_signal("005930", OrderSide::BUY);
        assert(gate.check(signal, reason));
    }

    {
        OrderGate::Config config;
        // 지금이 정오 전이면 오후 창, 아니면 오전 창 — 어느 쪽이든 지금은 밖.
        config.session_open_min  = now_min < 12 * 60 ? 13 * 60 : 1 * 60;
        config.session_close_min = now_min < 12 * 60 ? 14 * 60 : 2 * 60;
        OrderGate gate(config);
        auto      signal = make_signal("005930", OrderSide::BUY);
        assert(!gate.check(signal, reason));
        assert(reason.find("세션 창 밖") != std::string::npos);

        auto cancel   = make_signal("005930", OrderSide::SELL);
        cancel.action = OrderAction::CANCEL;
        cancel.original_client_order_id = "TEST-1";
        reason.clear();
        gate.check(cancel, reason);
        assert(reason.find("세션 창 밖") == std::string::npos);
    }

    {
        // 정규장 창은 지금을 비켜 가고 애프터마켓 창이 지금을 덮으면 통과 — 두 창의 합집합이다. [why D-097]
        OrderGate::Config config;
        config.session_open_min  = now_min < 12 * 60 ? 13 * 60 : 1 * 60;
        config.session_close_min = now_min < 12 * 60 ? 14 * 60 : 2 * 60;
        config.after_open_min    = (std::max)(0, now_min - 30);
        config.after_close_min   = (std::min)(24 * 60, now_min + 30);
        OrderGate gate(config);
        auto      signal = make_signal("005930", OrderSide::BUY);
        reason.clear();
        assert(gate.check(signal, reason));

        // 애프터마켓 창도 지금을 비켜 가면 거부 사유에 두 창이 다 적힌다.
        config.after_open_min  = now_min < 12 * 60 ? 15 * 60 : 3 * 60;
        config.after_close_min = now_min < 12 * 60 ? 16 * 60 : 4 * 60;
        OrderGate gate_closed(config);
        reason.clear();
        assert(!gate_closed.check(signal, reason));
        assert(reason.find("세션 창 밖") != std::string::npos);
        assert(reason.find(" 및 ") != std::string::npos);
    }

    {
        OrderGate gate; // 기본 0/0 = 검사 없음(테스트·리플레이)
        auto      signal = make_signal("005930", OrderSide::BUY);
        assert(gate.check(signal, reason));
    }

    PASS("session_window");
}

// ─── 바스켓 슬롯 제외(D-109) ────────────────────────────────────────────
//  바스켓 슬리브가 든 종목은 장중 전략의 슬롯을 먹지 않는다. 상한 2에서 제외 종목 둘을 채워도 새 BUY가 통과하고,
//  open_slot_count·plan_displacement 후보·snapshot_positions 표시가 같은 집합을 본다.
void test_slot_exempt()
{
    OrderGate gate(displace_config()); // 상한 2, 교체 켜짐
    gate.set_slot_exempt({"BK1", "BK2"});
    gate.seed_position("", "BK1", 10, 1000.0);
    gate.seed_position("", "BK2", 10, 1000.0);
    assert(!gate.slots_full());
    assert(gate.open_slot_count() == 0); // 열린 슬롯(차지한 자리) 수 — 제외 종목은 세지 않는다
    // 자리 세는 곳이 다섯이다. 하나라도 면제를 빼먹으면 전략과 게이트가 딴 판단을 한다 —
    //  entry_snapshot이 빼먹던 때는 교체를 켠 날 멀쩡한 보유를 팔아 이미 있던 자리를 만들었다.
    assert(!gate.entry_snapshot("", "Z").slots_full);
    assert(gate.entry_snapshot("", "Z").slots_full == gate.slots_full());

    std::string reason;
    auto        buy_a = make_signal("A", OrderSide::BUY, 10);
    assert(gate.check(buy_a, reason)); // 제외 종목 둘은 상한 계산에 없다
    gate.on_fill_confirmed("", "A", OrderSide::BUY, 10, 1000.0);
    auto buy_b = make_signal("B", OrderSide::BUY, 10);
    assert(gate.check(buy_b, reason));
    gate.on_fill_confirmed("", "B", OrderSide::BUY, 10, 1000.0);
    assert(gate.slots_full());
    auto buy_c = make_signal("C", OrderSide::BUY, 10);
    assert(!gate.check(buy_c, reason)); // 장중 종목 둘로 꽉 참

    // 제외 종목은 상한이 꽉 차도 추가 매수가 된다(바스켓 채우기).
    auto buy_bk1 = make_signal("BK1", OrderSide::BUY, 5);
    assert(gate.check(buy_bk1, reason));

    // 교체 후보에도 안 든다 — 점수가 가장 낮은 BK1 대신 B가 뽑힌다.
    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 0.9}, {gate.intern_symbol("B"), 2, -0.8}, {gate.intern_symbol("C"), 3, 1.5}, {gate.intern_symbol("BK1"), 4, -5.0}}, 4);
    auto plan = gate.plan_displacement("", gate.intern_symbol("C"));
    assert(plan.ok && plan.ticker == "B");

    // snapshot 표시와 티커 목록.
    size_t exempt_count = 0;

    for (const auto& held : gate.snapshot_positions())
    {
        exempt_count += held.slot_exempt ? 1 : 0;
    }

    assert(exempt_count == 2);
    const auto exempt_symbols = gate.slot_exempt_symbols(); // 오름차순 id
    assert(exempt_symbols.size() == 2 && exempt_symbols[0] == gate.symbol_id_of("BK1") && exempt_symbols[1] == gate.symbol_id_of("BK2"));

    // 집합을 갈아 끼우면 이전 것은 풀린다.
    gate.set_slot_exempt({"BK1"});
    assert(gate.slot_exempt_symbols().size() == 1 && gate.slot_exempt_symbols()[0] == gate.symbol_id_of("BK1"));
    PASS("slot_exempt");
}


// ─── 테스트 23: 장부 사본이 원본과 같은 값을 싣는다 (D-114 단계 2.5) ──────────
//   전략 쪽이 OrderGate 대신 읽을 사본이다. 사본이 원본과 한 자리라도 다르면 전략이 딴 장부를 보고
//   매매하게 되므로, 사본의 모든 칸을 원본 접근자와 맞춰 본다.
void test_publish_ledger_matches_gate()
{
    auto config = displace_config();
    config.max_concurrent_positions = 3;
    OrderGate gate(config);

    gate.set_slot_exempt({"BK1"});
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 5, 2000.0);
    gate.seed_position("", "BK1", 7, 500.0);   // 슬롯 면제 — 자리를 안 먹는다
    gate.refresh_sellable("", "A", 4);          // 매도가능이 보유보다 적은 경우
    gate.on_accept("", "A", OrderSide::SELL, 2, 1000.0);  // 미체결 매도 — 선점은 음수
    gate.on_accept("", "C", OrderSide::BUY, 3, 700.0);    // 보유 없이 매수 선점만

    ipc::LedgerSnapshot snapshot;
    gate.publish_ledger(snapshot);

    assert(snapshot.generation() == 1);
    assert(gate.ledger_foreign_account_rows() == 0);

    // ① 보유 전체가 그대로 실렸는가 — 원본 snapshot_positions()와 한 줄씩 맞춰 본다.
    for (const auto& held : gate.snapshot_positions())
    {
        const ipc::LedgerRow row = snapshot.row(held.symbol);
        assert(row.position == held.quantity);
        assert(row.average_price == held.average_price);
        assert((row.slot_exempt != 0) == held.slot_exempt);
    }

    // ② 매도가능 — 원본 sellable_view()의 셈(상한 - 미체결 매도, 음수면 0)과 같아야 한다.
    for (const char* ticker : {"A", "B", "BK1"})
    {
        const auto view     = gate.sellable_view("", ticker);
        const int  expected = (view.possible_quantity_cap > view.pending) ? (view.possible_quantity_cap - view.pending) : 0;
        assert(snapshot.row(gate.symbol_id_of(ticker)).sellable == expected);
    }

    // ③ 진입 판단 셋 — 원본 entry_snapshot()과 보유·선점이 같아야 한다.
    for (const char* ticker : {"A", "B", "C", "Z"})
    {
        const auto original = gate.entry_snapshot("", ticker);
        const auto copy     = snapshot.entry(gate.symbol_id_of(ticker));
        assert(copy.position == original.position);
        assert(copy.reserved == original.reserved);
    }

    // ④ 미체결 선점의 부호가 살아 있는가 — 매도는 음수, 매수는 양수.
    assert(snapshot.row(gate.symbol_id_of("A")).reserved == -2);
    assert(snapshot.row(gate.symbol_id_of("C")).reserved == 3);
    assert(snapshot.row(gate.symbol_id_of("C")).position == 0); // 보유 없이 선점만

    // ⑤ 전역값 — 열린 자리 수와 여력은 원본과 같은 답이어야 한다.
    //  A·B·C 셋이 자리를 먹고 BK1은 면제라 3/3이다.
    const ipc::LedgerGlobals globals = snapshot.globals();
    assert(globals.open_slot_count == static_cast<int32_t>(gate.open_slot_count()));
    assert(globals.open_slot_count == 3);
    assert((globals.capacity_full != 0) == gate.capacity_full());
    assert(globals.max_concurrent_positions == config.max_concurrent_positions);
    assert(globals.displace_enabled == (config.displace_enabled ? 1 : 0));

    // ⑥ 진입 정지는 세 원천의 OR이다 [why D-091]
    gate.set_strategy_down_halt(true);
    gate.publish_ledger(snapshot);
    assert(snapshot.globals().entry_halted == 1);
    gate.set_strategy_down_halt(false);

    // ⑦ 판이 바뀌면 사라진 종목은 사본에서도 사라진다 — 낡은 보유가 남으면 이중 발주가 된다.
    gate.on_fill_confirmed("", "B", OrderSide::SELL, 5, 2000.0); // 전량 매도 체결 — 장부에서 빠진다
    gate.publish_ledger(snapshot);
    assert(snapshot.row(gate.symbol_id_of("B")).position == 0);
    assert(snapshot.row(gate.symbol_id_of("A")).position == 10); // 남은 종목은 그대로

    // ⑧ 다른 계좌 줄은 싣지 않고 센다 — 한 프로세스는 한 계좌만 다룬다.
    const uint64_t before = gate.ledger_foreign_account_rows();
    gate.seed_position("OTHER", "A", 99, 1234.0);
    gate.publish_ledger(snapshot);
    assert(gate.ledger_foreign_account_rows() > before);
    assert(snapshot.row(gate.symbol_id_of("A")).position == 10); // 남의 계좌 값이 덮지 않았다

    // ⑨ 이 판이 담은 계좌 이름 — 전략 쪽이 강제청산·한도 정리 주문에 적을 값이다. 단일 계좌의
    //  원장 키는 빈 이름이라 여기서는 빈 문자열이 나온다.
    assert(std::string(snapshot.globals().account).empty());

    {
        // 이름이 있는 계좌면 그 이름이 실린다.
        OrderGate named_gate(config);
        named_gate.seed_position("TEST-ACCOUNT", "A", 1, 100.0);
        named_gate.publish_ledger(snapshot);
        assert(std::string(snapshot.globals().account) == "TEST-ACCOUNT");

        // 32칸을 넘는 이름은 잘리되 끝은 0으로 끊긴다 — 끊기지 않으면 읽는 쪽이 옆칸까지 읽는다.
        OrderGate         long_name_gate(config);
        const std::string long_name(60, 'X');
        long_name_gate.seed_position(long_name, "A", 1, 100.0);
        long_name_gate.publish_ledger(snapshot);
        const std::string copied = snapshot.globals().account;
        assert(copied.size() == sizeof(ipc::LedgerGlobals::account) - 1);
        assert(copied == long_name.substr(0, copied.size()));
    }

    PASS("publish_ledger_matches_gate");
}

// ─── 원장 저널 공통 ────────────────────────────────────────────────────────
//  테스트마다 빈 폴더 하나 — 남아 있던 파일을 리플레이해 앞 테스트가 뒤 테스트를 오염시키지 않게.
static std::filesystem::path make_journal_directory(const std::string& name)
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("quant_ledger_test_" + name);
    std::error_code             error_code;
    std::filesystem::remove_all(directory, error_code);
    std::filesystem::create_directories(directory, error_code);
    return directory;
}

static OrderGate::Config journal_config()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker = 1000;
    config.max_orders_per_min      = 1000;
    config.max_orders_per_sec      = 1000;
    config.deduplicate_window_sec  = 0.0;
    return config;
}

// 한 거래일치 사건을 저널에 적고 그 파일 경로를 돌려준다 — 리플레이 테스트 3개가 같은 장면을 쓴다.
static std::filesystem::path write_sample_day(const std::filesystem::path& directory, const std::string& date)
{
    OrderGate gate(journal_config());
    assert(gate.set_journal(directory, date, false));
    assert(gate.journal_open());

    gate.seed_position("ACC1", "005930", 10, 70000.0, 10);                                                 // 1 SEED
    assert(gate.on_intent("ACC1", "005930", OrderSide::BUY, 5, 71000.0,
                          OrderGate::OrderRef{11, 0, OrderType::LIMIT}));                                  // 2 INTENT
    gate.on_accepted("ACC1", "005930", OrderSide::BUY, 5, OrderGate::OrderRef{11, 991, OrderType::LIMIT}); // 3 ACCEPT
    gate.on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 3, 71000.0, strategy_table::kNone,
                           OrderGate::OrderRef{11, 991, OrderType::LIMIT});                                // 4 FILL
    // 보내고 거부당한 주문 — 선점이 잡혔다 풀린다
    assert(gate.on_intent("ACC1", "000660", OrderSide::BUY, 7, 200000.0,
                          OrderGate::OrderRef{12, 0, OrderType::MARKET}));                                 // 5 INTENT
    gate.on_reject("ACC1", "000660", OrderSide::BUY, 7, OrderGate::OrderRef{12, 0, OrderType::MARKET},
                   "KIS 거부");                                                                            // 6 REJECT
    gate.set_daily_pnl(-12345.0);                                                                          // 7 DAILY_PNL
    gate.set_available_cash(4500000.0);                                                                    // 8 CASH
    gate.set_equity(9900000.0);                                                                            // 9 CASH

    assert(gate.position("ACC1", "005930") == 13);
    assert(gate.reserved("ACC1", "005930") == 2);
    assert(gate.reserved("ACC1", "000660") == 0);
    assert(gate.journal_failures() == 0);
    return gate.journal_path();
}

// 저널 파일 끝에서 몇 바이트를 잘라 낸다 — 쓰다 만 마지막 레코드(전원 장애) 흉내.
static void truncate_tail_bytes(const std::filesystem::path& file, uintmax_t bytes)
{
    std::error_code error_code;
    const uintmax_t size = std::filesystem::file_size(file, error_code);
    assert(size > bytes);
    std::filesystem::resize_file(file, size - bytes, error_code);
    assert(!error_code);
}

// ─── 테스트 19: 재기동 리플레이가 원장을 되살린다 ──────────────────────────
void test_journal_replay_rebuilds_ledger()
{
    const std::filesystem::path directory = make_journal_directory("replay");
    const std::string           date      = "20260922";
    write_sample_day(directory, date);

    OrderGate restarted(journal_config());
    assert(restarted.set_journal(directory, date, false));

    const auto& replayed = restarted.journal_replay();
    assert(replayed.header_ok);
    assert(replayed.applied == 9);
    assert(!replayed.truncated_tail);
    assert(replayed.last_sequence == 9);

    assert(restarted.position("ACC1", "005930") == 13);
    assert(restarted.reserved("ACC1", "005930") == 2);   // 5주 중 3주 체결 → 잔량 2주 선점
    assert(restarted.reserved("ACC1", "000660") == 0);   // 거부된 주문의 선점은 안 남는다
    const double average = restarted.average_price("ACC1", "005930");
    assert(average > 70229.0 && average < 70232.0);      // (10*70000 + 3*71000)/13 = 70230.77
    assert(restarted.sellable_view("ACC1", "005930").possible_quantity_cap == 13); // 시드 10 + 매수체결 3
    assert(restarted.daily_pnl() < -12344.0 && restarted.daily_pnl() > -12346.0);
    assert(restarted.available_cash() > 4499999.0 && restarted.available_cash() < 4500001.0);
    assert(restarted.equity() > 9899999.0 && restarted.equity() < 9900001.0);
    assert(restarted.journal_failures() == 0);

    // 미결 주문 — 5주 중 3주만 체결돼 결말을 못 본 주문 하나가 남는다(거부된 000660은 닫혔다).
    const auto intents = restarted.open_intents();
    assert(intents.size() == 1);
    assert(intents[0].order_id == 11 && intents[0].kis_order_number == 991);
    assert(intents[0].ticker == "005930" && intents[0].remaining == 2 && intents[0].accepted);

    // 이어 적기 — 리플레이한 만큼 건너뛴 번호부터 붙는다(같은 번호를 두 번 쓰면 뒤 기동이 헷갈린다).
    assert(restarted.on_intent("ACC1", "005930", OrderSide::SELL, 4, 72000.0,
                               OrderGate::OrderRef{13, 0, OrderType::LIMIT}));
    assert(restarted.reserved("ACC1", "005930") == -2); // 매수 잔량 2 - 매도 선점 4

    std::error_code error_code;
    std::filesystem::remove_all(directory, error_code);
    PASS("journal_replay_rebuilds_ledger");
}

// ─── 테스트 20: 쓰다 만 꼬리 ───────────────────────────────────────────────
void test_journal_truncates_broken_tail()
{
    const std::filesystem::path directory = make_journal_directory("tail");
    const std::string           date      = "20260922";
    const std::filesystem::path file      = write_sample_day(directory, date);

    // 마지막 CASH 레코드를 절반만 남긴다 — fwrite 도중 전원이 나간 모습.
    truncate_tail_bytes(file, sizeof(ledger_journal::Record) / 2);

    OrderGate restarted(journal_config());
    assert(restarted.set_journal(directory, date, false));

    const auto& replayed = restarted.journal_replay();
    assert(replayed.truncated_tail);
    assert(replayed.applied == 8);                        // 온전한 8건까지만
    assert(restarted.position("ACC1", "005930") == 13);   // 보유는 그대로
    assert(restarted.equity() == 0.0);                    // 잘린 CASH는 안 적용된다

    // 잘린 꼬리는 파일에서도 떨어져 나가 다음 레코드가 경계에 맞게 붙는다 — 파이썬 판독기가 같은 자리를 본다.
    std::error_code error_code;
    assert(std::filesystem::file_size(restarted.journal_path(), error_code) ==
           sizeof(ledger_journal::FileHeader) + 8 * sizeof(ledger_journal::Record));

    std::filesystem::remove_all(directory, error_code);
    PASS("journal_truncates_broken_tail");
}

// ─── 테스트 21: 깨진 레코드 ────────────────────────────────────────────────
void test_journal_stops_at_corrupt_record()
{
    const std::filesystem::path directory = make_journal_directory("crc");
    const std::string           date      = "20260922";
    const std::filesystem::path file      = write_sample_day(directory, date);

    // 4번째 레코드(FILL) 한 바이트를 뒤집는다 — 디스크가 조용히 썩은 경우.
    const long offset = static_cast<long>(sizeof(ledger_journal::FileHeader) + 3 * sizeof(ledger_journal::Record)) + 32;
    std::FILE* handle = std::fopen(file.string().c_str(), "r+b");
    assert(handle != nullptr);
    assert(std::fseek(handle, offset, SEEK_SET) == 0);
    unsigned char byte = 0;
    assert(std::fread(&byte, 1, 1, handle) == 1);
    byte ^= 0xFFu;
    assert(std::fseek(handle, offset, SEEK_SET) == 0);
    assert(std::fwrite(&byte, 1, 1, handle) == 1);
    std::fclose(handle);

    OrderGate restarted(journal_config());
    assert(restarted.set_journal(directory, date, false));

    const auto& replayed = restarted.journal_replay();
    assert(replayed.truncated_tail);
    assert(replayed.applied == 3);                       // SEED·INTENT·ACCEPT까지
    assert(restarted.position("ACC1", "005930") == 10);  // 체결 전 보유
    assert(restarted.reserved("ACC1", "005930") == 5);   // 선점은 아직 안 풀렸다
    assert(restarted.daily_pnl() == 0.0);

    std::error_code error_code;
    std::filesystem::remove_all(directory, error_code);
    PASS("journal_stops_at_corrupt_record");
}

// ─── 테스트 22: 코드페이지에 없는 글자가 든 폴더 ──────────────────────────
void test_journal_opens_on_non_codepage_path()
{
    // U+3400은 Windows 한국어 코드페이지(CP949)에 매핑이 없다. 경로를 좁은 문자열로 되돌려 열면
    //  여기서 예외가 나고, 저널을 못 열면 엔진이 기동을 거부한다 — 실제로 리플레이 기동이 그렇게 죽었다.
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::wstring(L"quant_ledger_test_㐀");
    std::error_code error_code;
    std::filesystem::remove_all(directory, error_code);
    std::filesystem::create_directories(directory, error_code);

    const std::string date = "20260922";
    OrderGate         gate(journal_config());
    assert(gate.set_journal(directory, date, false));
    gate.seed_position("ACC1", "005930", 10, 70000.0);
    assert(gate.journal_failures() == 0);

    OrderGate restarted(journal_config());
    assert(restarted.set_journal(directory, date, false));
    assert(restarted.journal_replay().header_ok);
    assert(restarted.position("ACC1", "005930") == 10);

    std::filesystem::remove_all(directory, error_code);
    PASS("journal_opens_on_non_codepage_path");
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
    test_deduplicate();
    test_normal_pass();
    test_sell_bypasses_position_check();
    test_deduplicate_does_not_consume_rate_slot();
    test_sell_clamps_position_at_zero();
    test_partial_fill_average_price();
    test_market_sell_reference_price_notional();
    test_market_sell_without_reference_price_bypasses_notional();
    test_displace_picks_weakest();
    test_displace_needs_score_gap();
    test_displace_skips_unscored_holdings();
    test_displace_min_hold_blocks();
    test_displace_reserves_slot_and_cooldown();
    test_displace_daily_cap();
    test_entry_snapshot_matches_separate_calls();
    test_session_window();
    test_halt_sources_are_independent();
    test_slot_exempt();
    test_publish_ledger_matches_gate();
    test_journal_replay_rebuilds_ledger();
    test_journal_truncates_broken_tail();
    test_journal_stops_at_corrupt_record();
    test_journal_opens_on_non_codepage_path();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
