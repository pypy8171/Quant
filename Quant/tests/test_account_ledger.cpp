// tests/test_account_ledger.cpp
// 계좌별 원장 파티셔닝 검증 (D3) — DMA 미니원장 독립성
//
//   OrderGate의 positions/reserved/avg_price를 (account_id:ticker)로 파티셔닝한 뒤,
//   ① 같은 종목을 여러 계좌가 독립적으로 보유하는지
//   ② 한 계좌의 한도 초과가 다른 계좌 주문을 막지 않는지
//   ③ account="" 하위호환(단일 계좌)이 유지되는지
//   를 검증한다. 이것이 "법인/DMA 다계좌 인테이크"의 원장 격리 증거다.
//
//   사용법: test_account_ledger

#include "risk/OrderGate.h"
#include <cassert>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

static OrderSignal sig(const std::string& account, const std::string& ticker, OrderSide side, int qty)
{
    OrderSignal s;
    s.account_id  = account;
    s.ticker      = ticker;
    s.side        = side;
    s.quantity    = qty;
    s.price       = 1000.0;
    s.strategy_id = "T";
    s.market      = Market::KR;
    return s;
}

static OrderGate::Config relaxed_cfg()
{
    OrderGate::Config c;
    c.max_qty_per_ticker = 100;      // 한도 검증용
    c.max_orders_per_min = 1'000'000; // 벤치 아님 — rate로 막히지 않게
    c.max_orders_per_sec = 1'000'000;
    c.dedup_window_sec   = 0.0;       // 중복 제거 끄기
    return c;
}

static void PASS(const std::string& name) { std::cout << "[PASS] " << name << "\n"; }

// ─── 테스트 1: 같은 종목을 두 계좌가 독립 보유 ───────────────────────────────
void test_independent_holdings()
{
    OrderGate gate(relaxed_cfg());
    gate.on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 40, 1000.0);
    gate.on_fill_confirmed("ACC2", "005930", OrderSide::BUY, 70, 1000.0);

    assert(gate.position("ACC1", "005930") == 40);
    assert(gate.position("ACC2", "005930") == 70); // 같은 종목, 독립 보유
    assert(gate.position("", "005930") == 0);       // 기본(빈) 계좌엔 없음
    PASS("independent_holdings");
}

// ─── 테스트 2: 한 계좌 한도초과가 다른 계좌를 막지 않음 (핵심) ────────────────
void test_limit_isolation()
{
    OrderGate gate(relaxed_cfg()); // max_qty_per_ticker = 100

    // ACC1이 005930을 한도(100)까지 선점
    for (int i = 0; i < 100; ++i)
    {
        std::string r;
        assert(gate.check(sig("ACC1", "005930", OrderSide::BUY, 1), r));
        gate.on_accept("ACC1", "005930", OrderSide::BUY, 1, 1000.0);
    }
    assert(gate.reserved("ACC1", "005930") == 100);

    // ACC1의 101번째 → 한도 초과 거부
    std::string r1;
    assert(!gate.check(sig("ACC1", "005930", OrderSide::BUY, 1), r1));
    assert(r1.find("한도") != std::string::npos);

    // ACC2의 같은 종목 주문 → ACC1 한도와 무관하게 통과 (격리 증명)
    std::string r2;
    assert(gate.check(sig("ACC2", "005930", OrderSide::BUY, 1), r2));
    assert(gate.reserved("ACC2", "005930") == 0); // ACC2는 아직 선점 0
    PASS("limit_isolation");
}

// ─── 테스트 3: 체결·청산이 계좌별로 격리 ─────────────────────────────────────
void test_fill_and_close_isolation()
{
    OrderGate gate(relaxed_cfg());
    // 두 계좌가 같은 종목 매수 후, ACC1만 전량 매도(청산)
    gate.on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 10, 1000.0);
    gate.on_fill_confirmed("ACC2", "005930", OrderSide::BUY, 10, 2000.0);
    assert(gate.avg_price("ACC1", "005930") == 1000.0);
    assert(gate.avg_price("ACC2", "005930") == 2000.0); // 평단도 계좌별 독립

    gate.on_fill_confirmed("ACC1", "005930", OrderSide::SELL, 10, 1500.0); // ACC1 청산
    assert(gate.position("ACC1", "005930") == 0);
    assert(gate.position("ACC2", "005930") == 10);      // ACC2는 영향 없음
    assert(gate.avg_price("ACC2", "005930") == 2000.0);
    PASS("fill_and_close_isolation");
}

// ─── 테스트 4: account="" 하위호환 (단일 계좌 경로) ──────────────────────────
void test_default_account_backcompat()
{
    OrderGate gate(relaxed_cfg());
    gate.on_fill_confirmed("005930", OrderSide::BUY, 10, 1000.0); // 4-arg → account=""
    assert(gate.position("005930") == 10);        // 하위호환 조회
    assert(gate.position("", "005930") == 10);    // 명시적 빈 계좌 == 하위호환
    assert(gate.position("ACC1", "005930") == 0); // 다른 계좌엔 안 섞임
    PASS("default_account_backcompat");
}

// ─── 테스트 5: 중복신호 제거가 계좌별로 격리 (dedup 키에 account 포함) ────────
void test_dedup_account_isolation()
{
    OrderGate::Config c = relaxed_cfg();
    c.dedup_window_sec = 5.0; // dedup 켜기
    OrderGate gate(c);

    std::string r;
    assert(gate.check(sig("ACC1", "005930", OrderSide::BUY, 1), r));   // ACC1 최초 → 통과
    assert(!gate.check(sig("ACC1", "005930", OrderSide::BUY, 1), r));  // ACC1 즉시 재신호 → 중복 거부
    assert(r.find("중복") != std::string::npos);
    // 동일 strategy+ticker라도 ACC2는 별개 → 통과 (계좌별 dedup 격리)
    assert(gate.check(sig("ACC2", "005930", OrderSide::BUY, 1), r));
    PASS("dedup_account_isolation");
}

// ─── 테스트 6: reset_daily가 계좌별 선점만 만료, 포지션/평단은 양쪽 보존 ──────
void test_reset_daily_isolation()
{
    OrderGate gate(relaxed_cfg());
    gate.on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 10, 1000.0);
    gate.on_fill_confirmed("ACC2", "005930", OrderSide::BUY, 20, 2000.0);
    gate.on_accept("ACC1", "005930", OrderSide::BUY, 5, 1000.0); // 미체결 선점
    gate.on_accept("ACC2", "005930", OrderSide::BUY, 7, 2000.0);
    assert(gate.reserved("ACC1", "005930") == 5);
    assert(gate.reserved("ACC2", "005930") == 7);

    gate.reset_daily(); // 미체결 선점 만료, 포지션/평단은 영속

    assert(gate.reserved("ACC1", "005930") == 0);
    assert(gate.reserved("ACC2", "005930") == 0);
    assert(gate.position("ACC1", "005930") == 10); // 양쪽 포지션 보존
    assert(gate.position("ACC2", "005930") == 20);
    assert(gate.avg_price("ACC1", "005930") == 1000.0);
    assert(gate.avg_price("ACC2", "005930") == 2000.0);
    PASS("reset_daily_isolation");
}

// ─── 테스트 7: make_key 구분자 충돌 안전 (W-1 회귀) ──────────────────────────
//   나이브 'account:ticker'면 ("A","B:C")와 ("A:B","C")가 "A:B:C"로 충돌한다.
//   길이접두 키는 두 파티션을 분리해야 한다.
void test_key_collision_safety()
{
    OrderGate gate(relaxed_cfg());
    gate.on_fill_confirmed("A", "B:C", OrderSide::BUY, 3, 1000.0);
    gate.on_fill_confirmed("A:B", "C", OrderSide::BUY, 5, 1000.0);
    assert(gate.position("A", "B:C") == 3); // 충돌 없이 각각 독립
    assert(gate.position("A:B", "C") == 5);
    PASS("key_collision_safety");
}

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== Account Ledger Partition Tests ===\n";
    test_independent_holdings();
    test_limit_isolation();
    test_fill_and_close_isolation();
    test_default_account_backcompat();
    test_dedup_account_isolation();
    test_reset_daily_isolation();
    test_key_collision_safety();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
