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
    std::cout << "[PASS] " << name << "\n";
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
    std::cout << "=== All tests passed ===\n";
    return 0;
}
