// tests/test_account_ledger.cpp
// 계좌별 원장 파티셔닝 검증 (D3) — DMA 미니원장 독립성
//
//   OrderGate의 positions/reserved/average_price를 (account_id:ticker)로 파티셔닝한 뒤,
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

static OrderSignal signal(const std::string& account, const std::string& ticker, OrderSide side, int quantity)
{
    OrderSignal signal;
    signal.account_id  = account;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.quantity    = quantity;
    signal.price       = 1000.0;
    signal.strategy_id = "T";
    signal.market      = Market::KR;
    return signal;
}

static OrderGate::Config relaxed_config()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker = 100;      // 한도 검증용
    config.max_orders_per_min = 1'000'000; // 벤치 아님 — rate로 막히지 않게
    config.max_orders_per_sec = 1'000'000;
    config.deduplicate_window_sec   = 0.0;       // 중복 제거 끄기
    return config;
}

static void PASS(const std::string& name) { std::cout << "[PASS] " << name << "\n"; }

// ─── 테스트 1: 같은 종목을 두 계좌가 독립 보유 ───────────────────────────────
void test_independent_holdings()
{
    OrderGate gate(relaxed_config());
    gate.ledger().on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 40, 1000.0);
    gate.ledger().on_fill_confirmed("ACC2", "005930", OrderSide::BUY, 70, 1000.0);

    assert(gate.ledger().position("ACC1", "005930") == 40);
    assert(gate.ledger().position("ACC2", "005930") == 70); // 같은 종목, 독립 보유
    assert(gate.ledger().position("", "005930") == 0);       // 기본(빈) 계좌엔 없음
    PASS("independent_holdings");
}

// ─── 테스트 2: 한 계좌 한도초과가 다른 계좌를 막지 않음 (핵심) ────────────────
void test_limit_isolation()
{
    OrderGate gate(relaxed_config()); // max_quantity_per_ticker = 100

    // ACC1이 005930을 한도(100)까지 선점
    for (int index = 0; index < 100; ++index)
    {
        std::string raw;
        assert(gate.check(signal("ACC1", "005930", OrderSide::BUY, 1), raw));
        gate.ledger().on_accept("ACC1", "005930", OrderSide::BUY, 1, 1000.0);
    }

    assert(gate.ledger().reserved("ACC1", "005930") == 100);

    // ACC1의 101번째 → 한도 초과 거부
    std::string row_a;
    assert(!gate.check(signal("ACC1", "005930", OrderSide::BUY, 1), row_a));
    assert(row_a.find("한도") != std::string::npos);

    // ACC2의 같은 종목 주문 → ACC1 한도와 무관하게 통과 (격리 증명)
    std::string row_b;
    assert(gate.check(signal("ACC2", "005930", OrderSide::BUY, 1), row_b));
    assert(gate.ledger().reserved("ACC2", "005930") == 0); // ACC2는 아직 선점 0
    PASS("limit_isolation");
}

// ─── 테스트 3: 체결·청산이 계좌별로 격리 ─────────────────────────────────────
void test_fill_and_close_isolation()
{
    OrderGate gate(relaxed_config());
    // 두 계좌가 같은 종목 매수 후, ACC1만 전량 매도(청산)
    gate.ledger().on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 10, 1000.0);
    gate.ledger().on_fill_confirmed("ACC2", "005930", OrderSide::BUY, 10, 2000.0);
    assert(gate.ledger().average_price("ACC1", "005930") == 1000.0);
    assert(gate.ledger().average_price("ACC2", "005930") == 2000.0); // 평단도 계좌별 독립

    gate.ledger().on_fill_confirmed("ACC1", "005930", OrderSide::SELL, 10, 1500.0); // ACC1 청산
    assert(gate.ledger().position("ACC1", "005930") == 0);
    assert(gate.ledger().position("ACC2", "005930") == 10);      // ACC2는 영향 없음
    assert(gate.ledger().average_price("ACC2", "005930") == 2000.0);
    PASS("fill_and_close_isolation");
}

// ─── 테스트 4: account="" 하위호환 (단일 계좌 경로) ──────────────────────────
void test_default_account_backcompat()
{
    OrderGate gate(relaxed_config());
    gate.ledger().on_fill_confirmed("005930", OrderSide::BUY, 10, 1000.0); // 4-argument → account=""
    assert(gate.ledger().position("005930") == 10);        // 하위호환 조회
    assert(gate.ledger().position("", "005930") == 10);    // 명시적 빈 계좌 == 하위호환
    assert(gate.ledger().position("ACC1", "005930") == 0); // 다른 계좌엔 안 섞임
    PASS("default_account_backcompat");
}

// ─── 테스트 5: 중복신호 제거가 계좌별로 격리 (deduplicate 키에 account 포함) ────────
void test_deduplicate_account_isolation()
{
    OrderGate::Config config = relaxed_config();
    config.deduplicate_window_sec = 5.0; // deduplicate 켜기
    OrderGate gate(config);

    std::string raw;
    assert(gate.check(signal("ACC1", "005930", OrderSide::BUY, 1), raw));   // ACC1 최초 → 통과
    assert(!gate.check(signal("ACC1", "005930", OrderSide::BUY, 1), raw));  // ACC1 즉시 재신호 → 중복 거부
    assert(raw.find("중복") != std::string::npos);
    // 동일 strategy+ticker라도 ACC2는 별개 → 통과 (계좌별 deduplicate 격리)
    assert(gate.check(signal("ACC2", "005930", OrderSide::BUY, 1), raw));
    PASS("dedup_account_isolation");
}

// ─── 테스트 6: reset_daily가 계좌별 선점만 만료, 포지션/평단은 양쪽 보존 ──────
void test_reset_daily_isolation()
{
    OrderGate gate(relaxed_config());
    gate.ledger().on_fill_confirmed("ACC1", "005930", OrderSide::BUY, 10, 1000.0);
    gate.ledger().on_fill_confirmed("ACC2", "005930", OrderSide::BUY, 20, 2000.0);
    gate.ledger().on_accept("ACC1", "005930", OrderSide::BUY, 5, 1000.0); // 미체결 선점
    gate.ledger().on_accept("ACC2", "005930", OrderSide::BUY, 7, 2000.0);
    assert(gate.ledger().reserved("ACC1", "005930") == 5);
    assert(gate.ledger().reserved("ACC2", "005930") == 7);

    gate.reset_daily(); // 미체결 선점 만료, 포지션/평단은 영속

    assert(gate.ledger().reserved("ACC1", "005930") == 0);
    assert(gate.ledger().reserved("ACC2", "005930") == 0);
    assert(gate.ledger().position("ACC1", "005930") == 10); // 양쪽 포지션 보존
    assert(gate.ledger().position("ACC2", "005930") == 20);
    assert(gate.ledger().average_price("ACC1", "005930") == 1000.0);
    assert(gate.ledger().average_price("ACC2", "005930") == 2000.0);
    PASS("reset_daily_isolation");
}

// ─── 테스트 7: make_key 구분자 충돌 안전 (W-1 회귀) ──────────────────────────
//   나이브 'account:ticker'면 ("A","B:C")와 ("A:B","C")가 "A:B:C"로 충돌한다.
//   두 필드를 따로 든 PosKey(D-057, 그 전엔 길이접두 문자열)는 두 파티션을 분리해야 한다.
void test_key_collision_safety()
{
    OrderGate gate(relaxed_config());
    gate.ledger().on_fill_confirmed("A", "B:C", OrderSide::BUY, 3, 1000.0);
    gate.ledger().on_fill_confirmed("A:B", "C", OrderSide::BUY, 5, 1000.0);
    assert(gate.ledger().position("A", "B:C") == 3); // 충돌 없이 각각 독립
    assert(gate.ledger().position("A:B", "C") == 5);
    PASS("key_collision_safety");
}

// ─── 테스트 8: 평단 미상 SELL은 가짜 실현이익을 만들지 않는다 (C-1 회귀) ─────────
//   원장이 종목을 모르면(재기동 후 미시드·미연결 체결) current_average=0이라 (price-0)*quantity가 이익으로
//   잡히고 daily_pnl이 부풀어 일일 손실컷이 무력화된다. 평단을 모르면 0 + basis_unknown.
void test_sell_unknown_basis_no_fake_profit()
{
    OrderGate gate(relaxed_config());
    auto on_fill_confirmed = gate.ledger().on_fill_confirmed("ACC1", "047050", OrderSide::SELL, 91, 54700.0);
    assert(on_fill_confirmed.basis_unknown);
    assert(on_fill_confirmed.realized_pnl == 0.0);
    assert(gate.ledger().daily_pnl() == 0.0);             // 4,977,700원이 이익으로 적립되지 않음
    assert(gate.ledger().position("ACC1", "047050") == 0); // 보유 초과 매도는 0으로 클램프(기존 동작)

    // 평단을 아는 계좌는 그대로 계산된다 — 격리 확인
    gate.ledger().on_fill_confirmed("ACC2", "047050", OrderSide::BUY, 10, 50000.0);
    auto row_b = gate.ledger().on_fill_confirmed("ACC2", "047050", OrderSide::SELL, 10, 54700.0);
    assert(!row_b.basis_unknown);
    assert(row_b.realized_pnl > 0.0 && row_b.realized_pnl < 47000.0); // 47,000원에서 수수료·세금 차감

    // 매수 수수료(10*50000*0.00015=75원)도 발생 즉시 daily_pnl_에서 빠진다.
    const double buy_commission = 10 * 50000.0 * 0.00015;
    assert(gate.ledger().daily_pnl() == row_b.realized_pnl - buy_commission);
    PASS("sell_unknown_basis_no_fake_profit");
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
    test_deduplicate_account_isolation();
    test_reset_daily_isolation();
    test_key_collision_safety();
    test_sell_unknown_basis_no_fake_profit();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
