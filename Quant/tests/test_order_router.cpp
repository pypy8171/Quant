// OrderRouter 통합 테스트
// 빌드: cmake --build <dir> --target test_order_router
// 실행: ./test_order_router
//
// 테스트 항목:
//   1. OrderGate 거부 → REJECTED, KIS 호출 없음
//   2. KIS 성공 (ODNO 반환) → ACCEPTED, ODNO 저장
//   3. KIS 실패 (빈 ODNO) → REJECTED with "KIS API 오류"
//   4. 혼합 제출 후 Stats(total/accepted/rejected) 검증
//   5. recent() 이력 N건 반환
//   6. order_id 순번 "ORD-000001" 포맷 검증

#include "api/IOrderExecutor.h"
#include "ipc/OrderRouter.h"
#include "risk/OrderGate.h"
#include <cassert>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

// ─── KIS 응답 시뮬레이션 Stub ─────────────────────────────────────────────────
struct StubOrderExecutor : IOrderExecutor
{
    bool        succeed;
    std::string odno;
    int         call_count = 0;

    explicit StubOrderExecutor(bool s, std::string o = "A000000042")
        : succeed(s), odno(std::move(o))
    {
    }

    std::string submit_order(const OrderSignal&) override
    {
        ++call_count;
        return succeed ? odno : "";
    }
};

// ─── 헬퍼 ─────────────────────────────────────────────────────────────────────
static OrderSignal make_signal(const std::string& ticker, OrderSide side, int qty = 1)
{
    OrderSignal s;
    s.ticker      = ticker;
    s.side        = side;
    s.quantity    = qty;
    s.price       = 75000.0;
    s.strategy_id = "TEST";
    s.market      = Market::KR;
    return s;
}

static OrderGate::Config relaxed_cfg()
{
    OrderGate::Config c;
    c.max_orders_per_min = 100;
    c.max_orders_per_sec = 100;
    c.dedup_window_sec   = 0.0;
    return c;
}

static void PASS(const std::string& name)
{
    std::cout << "[PASS] " << name << "\n";
}

// ─── 테스트 1: OrderGate 거부 → KIS 호출 없이 REJECTED ───────────────────────
void test_gate_rejected()
{
    OrderGate gate(relaxed_cfg());
    gate.set_kill_switch(true);

    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    auto mo = router.submit(make_signal("005930", OrderSide::BUY));

    assert(mo.status == OrderStatus::REJECTED);
    assert(mo.reject_reason.find("KILL") != std::string::npos);
    assert(stub.call_count == 0); // KIS 호출 없어야 함

    auto s = router.stats();
    assert(s.total == 1 && s.accepted == 0 && s.rejected == 1);
    PASS("gate_rejected");
}

// ─── 테스트 2: KIS 성공 → ACCEPTED, ODNO 저장 ────────────────────────────────
void test_kis_accepted()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000012345");
    OrderRouter       router(gate, stub);

    auto mo = router.submit(make_signal("005930", OrderSide::BUY, 1));

    assert(mo.status == OrderStatus::ACCEPTED);
    assert(mo.kis_order_no == "K000012345");
    assert(stub.call_count == 1);

    auto s = router.stats();
    assert(s.total == 1 && s.accepted == 1 && s.rejected == 0);
    PASS("kis_accepted");
}

// ─── 테스트 3: KIS 실패 (빈 ODNO) → REJECTED with "KIS" ──────────────────────
void test_kis_failed()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(false); // 빈 문자열 반환
    OrderRouter       router(gate, stub);

    auto mo = router.submit(make_signal("005930", OrderSide::BUY, 1));

    assert(mo.status == OrderStatus::REJECTED);
    assert(mo.reject_reason.find("KIS") != std::string::npos);
    assert(stub.call_count == 1); // 게이트 통과 후 KIS 호출은 됨

    auto s = router.stats();
    assert(s.total == 1 && s.accepted == 0 && s.rejected == 1);
    PASS("kis_failed");
}

// ─── 테스트 4: 혼합 제출 후 Stats 검증 ───────────────────────────────────────
void test_stats_mixed()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    // 3건 성공
    for (int i = 0; i < 3; ++i)
        router.submit(make_signal("00593" + std::to_string(i), OrderSide::BUY));

    // kill switch 이후 1건 거부
    gate.set_kill_switch(true);
    router.submit(make_signal("005934", OrderSide::BUY));

    auto s = router.stats();
    assert(s.total == 4 && s.accepted == 3 && s.rejected == 1);
    PASS("stats_mixed");
}

// ─── 테스트 5: recent() 이력 N건 반환 ────────────────────────────────────────
void test_history_recent()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    for (int i = 0; i < 5; ++i)
        router.submit(make_signal("00593" + std::to_string(i), OrderSide::BUY));

    auto h = router.recent(3);
    assert(h.size() == 3);
    assert(h[2].signal.ticker == "005934"); // 마지막이 5번째 종목
    PASS("history_recent");
}

// ─── 테스트 6: order_id 순번 "ORD-000001" 포맷 ───────────────────────────────
void test_order_id_sequence()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    auto mo1 = router.submit(make_signal("005930", OrderSide::BUY));
    auto mo2 = router.submit(make_signal("000660", OrderSide::BUY));

    assert(mo1.order_id == "ORD-000001");
    assert(mo2.order_id == "ORD-000002");
    PASS("order_id_sequence");
}

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== OrderRouter Unit Tests ===\n";
    test_gate_rejected();
    test_kis_accepted();
    test_kis_failed();
    test_stats_mixed();
    test_history_recent();
    test_order_id_sequence();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
