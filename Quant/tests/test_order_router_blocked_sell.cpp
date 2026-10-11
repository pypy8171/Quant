// test_order_router — 청산 차단 해소 — 이번 세션 예약매도 취소.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

// ─── C-2: 청산차단 해소 — 이번 세션 예약매도를 취소하면 CANCELLED로 닫고 선점을 푼다 ───────
//   미체결 조회가 실패한 경로: 미체결을 KIS가 아니라 history_에서 찾는다. 취소 뒤 재매도가
//   접수되면 그 선점만 남아야 한다(취소분 8 + 재매도 8 = 16이 아니라 8).
void test_blocked_sell_releases_reservation()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000301");
    OrderRouter       router(gate, stub);
    stub.paper            = true;
    stub.open_orders_fail = true; // 조회 실패 → 이력으로 물러난다

    // 원장에 포지션을 심지 않는다 — 심으면 게이트 SELL 클램프가 미체결매도를 빼고 0주로 깎아
    //  KIS까지 가지 않는다. 이 경로는 원장이 종목을 모르는(재기동 직후·WS 모드) 상황이 대상이다.

    OrderSignal reserved = make_signal("005930", OrderSide::SELL, 8);
    reserved.type        = OrderType::LIMIT;
    reserved.price       = 80000.0;
    reserved.client_order_id  = "RESV:1";
    auto acknowledgement_a          = router.submit(reserved);
    assert(acknowledgement_a.status == OrderStatus::ACCEPTED);
    assert(gate.ledger().reserved("005930") == -8);       // 매도 선점(부호는 게이트 규약)

    // 시장가 청산이 40240000으로 막힘 → 예약매도 취소 → 재매도 접수
    stub.kis_order_no      = "0000000302";
    stub.fail_next = 1;
    stub.error_code  = "40240000";
    OrderSignal liquidation = make_signal("005930", OrderSide::SELL, 8);
    liquidation.type        = OrderType::MARKET;
    liquidation.price       = 0.0;
    liquidation.reference_price   = 75000.0;
    auto acknowledgement_b         = router.submit(liquidation);
    assert(acknowledgement_b.status == OrderStatus::ACCEPTED);
    assert(acknowledgement_b.kis_order_no == "0000000302");
    assert(stub.cancel_calls == 1 && stub.last_cancel_quantity == 8);

    // 원주문은 CANCELLED, 선점은 재매도분만
    bool original_cancelled = false;

    for (const auto& history_entry : router.recent(10))
    {
        if (history_entry.kis_order_no == "0000000301")
        {
            original_cancelled = (history_entry.status == OrderStatus::CANCELLED);
        }
    }

    assert(original_cancelled);
    assert(gate.ledger().reserved("005930") == -8);

    // 원장에 CANCELLED 행이 남는다(재매도 ACCEPTED 행 앞).
    router.flush_file_writes();
    auto rows = tail_trade_rows(2);
    assert(split_csv(rows[0])[1] == "CANCELLED" && split_csv(rows[0])[3] == "0000000301");
    assert(split_csv(rows[1])[1] == "ACCEPTED" && split_csv(rows[1])[3] == "0000000302");
    PASS("blocked_sell_releases_reservation");
}

// ─── 실계좌: 통보 없이 사라진 예약매도 — 브로커 미체결 목록에 없으면 닫고 선점을 푼다 ───────
//   10-06 237690: SOR 익절 지정가가 브로커에서 사라졌는데 이력이 미체결로 세어 재매도가 "매도가능 0"으로 막혔다.
//   취소는 보내지 않는다(브로커에 없는 주문이라 "정정취소 가능수량 없음"으로 거부된다).
void test_blocked_sell_closes_vanished_order()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000401");
    OrderRouter       router(gate, stub);
    stub.paper = false; // 실계좌 경로 — 미체결은 브로커 조회로 안다

    OrderSignal reserved     = make_signal("005930", OrderSide::SELL, 1);
    reserved.type            = OrderType::LIMIT;
    reserved.price           = 101000.0;
    reserved.client_order_id = "RESV:2";
    auto acknowledgement_a   = router.submit(reserved);
    assert(acknowledgement_a.status == OrderStatus::ACCEPTED);
    assert(gate.ledger().reserved("005930") == -1);

    // 브로커 미체결 목록은 비었다 → 시장가 재매도가 40240000으로 막히면 사라진 주문을 닫고 다시 낸다.
    stub.open_orders.clear();
    stub.kis_order_no = "0000000402";
    stub.fail_next    = 1;
    stub.error_code   = "40240000";
    OrderSignal liquidation     = make_signal("005930", OrderSide::SELL, 1);
    liquidation.type            = OrderType::MARKET;
    liquidation.price           = 0.0;
    liquidation.reference_price = 96000.0;
    auto acknowledgement_b      = router.submit(liquidation);
    assert(acknowledgement_b.status == OrderStatus::ACCEPTED);
    assert(acknowledgement_b.kis_order_no == "0000000402");
    assert(stub.cancel_calls == 0);
    assert(stub.open_order_calls == 1);

    bool original_cancelled = false;

    for (const auto& history_entry : router.recent(10))
    {
        if (history_entry.kis_order_no == "0000000401")
        {
            original_cancelled = (history_entry.status == OrderStatus::CANCELLED);
        }
    }

    assert(original_cancelled);
    assert(gate.ledger().reserved("005930") == -1); // 재매도분만
    PASS("blocked_sell_closes_vanished_order");
}

// ─── 실계좌: 브로커 미체결 목록에 있는 예약매도는 사라진 것으로 보지 않는다 ───────
void test_blocked_sell_keeps_order_open_at_broker()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000501");
    OrderRouter       router(gate, stub);
    stub.paper = false;

    OrderSignal reserved     = make_signal("005930", OrderSide::SELL, 1);
    reserved.type            = OrderType::LIMIT;
    reserved.price           = 101000.0;
    reserved.client_order_id = "RESV:3";
    auto acknowledgement_a   = router.submit(reserved);
    assert(acknowledgement_a.status == OrderStatus::ACCEPTED);

    // 브로커에 살아 있다 → 취소를 보내 풀고 다시 낸다(기존 경로).
    OpenOrder open_at_broker;
    open_at_broker.ticker                = "005930";
    open_at_broker.side                  = OrderSide::SELL;
    open_at_broker.psbl_qty              = 1;
    open_at_broker.ord_unpr              = 101000.0;
    open_at_broker.kis_order_no          = "0000000501";
    open_at_broker.krx_forwarding_org_no = "ORG000777";
    stub.open_orders  = {open_at_broker};
    stub.kis_order_no = "0000000502";
    stub.fail_next    = 1;
    stub.error_code   = "40240000";
    OrderSignal liquidation     = make_signal("005930", OrderSide::SELL, 1);
    liquidation.type            = OrderType::MARKET;
    liquidation.price           = 0.0;
    liquidation.reference_price = 96000.0;
    auto acknowledgement_b      = router.submit(liquidation);
    assert(acknowledgement_b.status == OrderStatus::ACCEPTED);
    assert(stub.cancel_calls == 1 && stub.last_cancel_quantity == 1);
    assert(gate.ledger().reserved("005930") == -1);
    PASS("blocked_sell_keeps_order_open_at_broker");
}

// ─── 모의: 전송 시간초과로 거부 처리됐지만 KIS에 접수된 예약매도 — 브로커 조회로 찾아 취소한다 ───────
//   10-07 071320: 09:00 익절 지정가 15주가 E_TRANSPORT(RTT 18.8초)로 거부 처리돼 이력에 없었는데 KIS에는
//   접수돼 수량을 묶었다. 모의가 이력만 보던 때는 "취소할 예약매도 없음"으로 하루 종일 막혔다.
void test_paper_blocked_sell_cancels_order_unknown_to_history()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000602");
    OrderRouter       router(gate, stub);
    stub.paper = true;

    OpenOrder accepted_at_broker;
    accepted_at_broker.ticker                = "071320";
    accepted_at_broker.side                  = OrderSide::SELL;
    accepted_at_broker.psbl_qty              = 15;
    accepted_at_broker.ord_unpr              = 81400.0;
    accepted_at_broker.kis_order_no          = "0000000601"; // 라우터 이력에 없는 번호
    accepted_at_broker.krx_forwarding_org_no = "ORG000777";
    stub.open_orders = {accepted_at_broker};
    stub.fail_next   = 1;
    stub.error_code  = "40240000";

    OrderSignal liquidation     = make_signal("071320", OrderSide::SELL, 15);
    liquidation.type            = OrderType::MARKET;
    liquidation.price           = 0.0;
    liquidation.reference_price = 78300.0;
    auto acknowledgement        = router.submit(liquidation);
    assert(acknowledgement.status == OrderStatus::ACCEPTED);
    assert(acknowledgement.kis_order_no == "0000000602");
    assert(stub.open_order_calls == 1);
    assert(stub.cancel_calls == 1 && stub.last_cancel_quantity == 15);
    assert(gate.ledger().reserved("071320") == -15); // 재매도분만
    PASS("paper_blocked_sell_cancels_order_unknown_to_history");
}

void run_blocked_sell_tests()
{
    test_blocked_sell_releases_reservation();
    test_blocked_sell_closes_vanished_order();
    test_blocked_sell_keeps_order_open_at_broker();
    test_paper_blocked_sell_cancels_order_unknown_to_history();
}
