// test_order_gate — 킬 스위치·한도·초당 횟수·중복·정상 통과와 시장가 명목 백스톱.
//  공용 준비는 test_order_gate_support.h, 실행 순서는 test_order_gate.cpp의 main.
#include "test_order_gate_support.h"

#include <string>

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
    gate.ledger().on_accept("005930", OrderSide::BUY, 3, 100000);

    // 추가 3주 시도 → 합산 6 > 5, 차단
    assert(!gate.check(sig3, reason));
    assert(reason.find("한도") != std::string::npos);
    PASS("position_limit");
}

// ─── 테스트 3: 일일 손실 한도 ────────────────────────────────────────────
void test_daily_loss_limit()
{
    OrderGate::Config config;
    config.daily_loss_limit   = -100'000;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    OrderGate gate(config);

    gate.ledger().add_realized_pnl(-100001.0); // 한도 초과

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

// ─── 테스트 8: 중복 신호는 rate slot 소모 안 함 ─────────────────
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
    config.max_notional_per_order = 50'000'000;
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

void run_basic_tests()
{
    test_kill_switch();
    test_position_limit();
    test_daily_loss_limit();
    test_rate_limit_per_sec();
    test_deduplicate();
    test_normal_pass();
    test_sell_bypasses_position_check();
    test_deduplicate_does_not_consume_rate_slot();
    test_market_sell_reference_price_notional();
    test_market_sell_without_reference_price_bypasses_notional();
}
