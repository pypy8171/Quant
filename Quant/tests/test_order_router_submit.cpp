// test_order_router — 게이트 거부·KIS 접수 성공과 실패·통계·이력·주문번호 순번.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

// ─── 테스트 1: OrderGate 거부 → KIS 호출 없이 REJECTED ───────────────────────
void test_gate_rejected()
{
    OrderGate gate(relaxed_config());
    gate.set_kill_switch(true);

    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    auto managed_order = router.submit(make_signal("005930", OrderSide::BUY));

    assert(managed_order.status == OrderStatus::REJECTED);
    assert(managed_order.reject_reason.find("KILL") != std::string::npos);
    assert(stub.call_count == 0); // KIS 호출 없어야 함

    auto statistics = router.statistics();
    assert(statistics.total == 1 && statistics.accepted == 0 && statistics.rejected == 1);
    PASS("gate_rejected");
}

// ─── 테스트 2: KIS 성공 → ACCEPTED, ODNO 저장 ────────────────────────────────
void test_kis_accepted()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000012345");
    OrderRouter       router(gate, stub);

    auto managed_order = router.submit(make_signal("005930", OrderSide::BUY, 1));

    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(managed_order.kis_order_no == "0000012345");
    assert(stub.call_count == 1);

    auto statistics = router.statistics();
    assert(statistics.total == 1 && statistics.accepted == 1 && statistics.rejected == 0);
    PASS("kis_accepted");
}

// ─── 테스트 3: KIS 실패 (빈 ODNO) → REJECTED with "KIS" ──────────────────────
void test_kis_failed()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(false); // 빈 문자열 반환
    OrderRouter       router(gate, stub);

    auto managed_order = router.submit(make_signal("005930", OrderSide::BUY, 1));

    assert(managed_order.status == OrderStatus::REJECTED);
    assert(managed_order.reject_reason.find("KIS") != std::string::npos);
    assert(stub.call_count == 1); // 게이트 통과 후 KIS 호출은 됨

    auto statistics = router.statistics();
    assert(statistics.total == 1 && statistics.accepted == 0 && statistics.rejected == 1);
    PASS("kis_failed");
}

// ─── 테스트 4: 혼합 제출 후 Stats 검증 ───────────────────────────────────────
void test_statistics_mixed()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    // 3건 성공
    for (int index = 0; index < 3; ++index)
    {
        (void)router.submit(make_signal("00593" + std::to_string(index), OrderSide::BUY));
    }

    // kill switch 이후 1건 거부
    gate.set_kill_switch(true);
    (void)router.submit(make_signal("005934", OrderSide::BUY));

    auto statistics = router.statistics();
    assert(statistics.total == 4 && statistics.accepted == 3 && statistics.rejected == 1);
    PASS("stats_mixed");
}

// ─── 테스트 5: recent() 이력 N건 반환 ────────────────────────────────────────
void test_history_recent()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    for (int index = 0; index < 5; ++index)
    {
        (void)router.submit(make_signal("00593" + std::to_string(index), OrderSide::BUY));
    }

    auto history = router.recent(3);
    assert(history.size() == 3);
    assert(history[2].signal.ticker == "005934"); // 마지막이 5번째 종목
    PASS("history_recent");
}

// ─── 테스트 6: order_id 순번 "ORD-000001" 포맷 ───────────────────────────────
void test_order_id_sequence()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    auto mo1 = router.submit(make_signal("005930", OrderSide::BUY));
    auto mo2 = router.submit(make_signal("000660", OrderSide::BUY));

    assert(mo1.order_id == "ORD-000001");
    assert(mo2.order_id == "ORD-000002");
    PASS("order_id_sequence");
}

void run_submit_tests()
{
    test_gate_rejected();
    test_kis_accepted();
    test_kis_failed();
    test_statistics_mixed();
    test_history_recent();
    test_order_id_sequence();
}
