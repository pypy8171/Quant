// OrderGate 단위 테스트
// 빌드: cmake --build <dir> --target test_order_gate
// 실행: ./test_order_gate
//
// 테스트 항목:
//   1. Kill switch 차단
//   2. 포지션 한도 초과 차단
//   3. 일일 손실 한도 차단
//   4. 초당 Rate limit 차단
//   5. 분당 Rate limit 차단
//   6. 중복 신호 차단
//   7. 정상 주문 통과

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
    gate.on_fill("005930", OrderSide::BUY, 3, 100000);

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
    std::cout << "=== All tests passed ===\n";
    return 0;
}
