// test_order_router — 취소·정정 — 선점 해제, 모르는 주문, 부분 체결 뒤 취소, 정정 재선점.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

// ─── 테스트 9: CANCEL 경로 — reserved 해제 + orgno 캡처 (MM-1) ────────────────
void test_cancel_releases_reserved()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000111");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_order_id     = "MM:B:1";
    buy.client_order_number = 1;
    auto managed_order = router.submit(buy);
    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(managed_order.krx_forwarding_org_no == "ORG000001");       // submit_order_ack가 조직번호 캡처
    assert(gate.ledger().reserved("005930") == 10);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.strategy_id     = "MM";
    cancel.action          = OrderAction::CANCEL;
    cancel.original_client_order_id     = "MM:B:1";
    cancel.original_client_order_number = 1;
    auto cancel_acknowledgement = router.submit(cancel);

    assert(cancel_acknowledgement.status == OrderStatus::CANCELLED);
    assert(stub.cancel_calls == 1);
    assert(stub.last_cancel_quantity == 10);         // 미체결 전량
    assert(gate.ledger().reserved("005930") == 0);       // 선점 해제
    PASS("cancel_releases_reserved");
}

// ─── 테스트 10: 존재하지 않는 order_id 취소 → CANCELLED(취소할 것 없음), KIS 미호출 ─────
void test_cancel_unknown_order_id()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000222");
    OrderRouter       router(gate, stub);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.action          = OrderAction::CANCEL;
    cancel.original_client_order_id     = "NOPE";
    cancel.original_client_order_number = 999; // 이력에 없는 번호
    auto cancel_acknowledgement = router.submit(cancel);

    assert(cancel_acknowledgement.status == OrderStatus::CANCELLED); // 거부가 아니라 끝난 상태 — 거부 통계에 안 들어간다
    assert(router.statistics().rejected == 0);
    assert(stub.cancel_calls == 0);
    PASS("cancel_unknown_oid");
}

// ─── 테스트 11: 부분체결 중 취소 → 잔량만 취소·해제 ──────────────────────────
void test_partial_fill_then_cancel()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000333");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_order_id     = "MM:B:1";
    buy.client_order_number = 1;
    (void)router.submit(buy);
    assert(gate.ledger().reserved("005930") == 10);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000000333"; fill_notification.ticker = "005930"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 4; fill_notification.filled_price = 75000.0; fill_notification.fill_time = "100000";
    router.on_fill(fill_notification);
    assert(gate.ledger().reserved("005930") == 6);   // 10 - 4
    assert(gate.ledger().position("005930") == 4);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.action          = OrderAction::CANCEL;
    cancel.original_client_order_id     = "MM:B:1";
    cancel.original_client_order_number = 1;
    auto cancel_acknowledgement = router.submit(cancel);

    assert(cancel_acknowledgement.status == OrderStatus::CANCELLED);
    assert(stub.last_cancel_quantity == 6);      // 미체결 잔량만
    assert(gate.ledger().reserved("005930") == 0);   // 잔량 6 해제
    assert(gate.ledger().position("005930") == 4);   // 체결분은 불변
    PASS("partial_fill_then_cancel");
}

// ─── 테스트 12: 전량체결 후 취소 → 자가치유(REJECTED, 이중해제 없음) ─────────
void test_cancel_after_full_fill_selfheal()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000444");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_order_id     = "MM:B:1";
    buy.client_order_number = 1;
    (void)router.submit(buy);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000000444"; fill_notification.ticker = "005930"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 10; fill_notification.filled_price = 75000.0; fill_notification.fill_time = "100000";
    router.on_fill(fill_notification);
    assert(gate.ledger().reserved("005930") == 0);
    assert(gate.ledger().position("005930") == 10);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.action          = OrderAction::CANCEL;
    cancel.original_client_order_id     = "MM:B:1";
    cancel.original_client_order_number = 1;
    auto cancel_acknowledgement = router.submit(cancel);

    assert(cancel_acknowledgement.status == OrderStatus::CANCELLED); // 이미 FILLED → 취소 대상 없음(거부가 아니라 끝난 상태)
    assert(cancel_acknowledgement.reject_reason.find("이미 체결") != std::string::npos);
    assert(stub.cancel_calls == 0);
    assert(gate.ledger().reserved("005930") == 0);       // 이중해제 없음
    assert(gate.ledger().position("005930") == 10);
    PASS("cancel_after_full_fill_selfheal");
}

// ─── 테스트 13: REPLACE(정정) — 잔량 해제 후 new_quantity 재선점 ──────────────────
void test_replace_reserves_new_quantity()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000555");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_order_id     = "MM:B:1";
    buy.client_order_number = 1;
    (void)router.submit(buy);
    assert(gate.ledger().reserved("005930") == 10);

    OrderSignal rep;
    rep.ticker          = "005930";
    rep.side            = OrderSide::BUY;
    rep.type            = OrderType::LIMIT;
    rep.quantity        = 8;
    rep.price           = 74000.0;
    rep.action          = OrderAction::REPLACE;
    rep.original_client_order_id     = "MM:B:1";
    rep.original_client_order_number = 1;
    rep.client_order_id              = "MM:B:2";
    rep.client_order_number          = 2;
    auto rm = router.submit(rep);

    assert(rm.status == OrderStatus::ACCEPTED);
    assert(rm.kis_order_no == "0000000001");
    assert(stub.revise_calls == 1);
    assert(gate.ledger().reserved("005930") == 8);   // 10 해제 후 8 재선점
    PASS("replace_reserves_new_qty");
}

void run_cancel_tests()
{
    test_cancel_releases_reserved();
    test_cancel_unknown_order_id();
    test_partial_fill_then_cancel();
    test_cancel_after_full_fill_selfheal();
    test_replace_reserves_new_quantity();
}
