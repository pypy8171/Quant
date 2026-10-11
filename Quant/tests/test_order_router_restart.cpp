// test_order_router — 재기동 미결 주문 대조와 낡은 취소.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

// ─── 재기동 미결 주문 대조 (D-113) ─────────────────────────────────────────────
//   전송 뒤 접수 응답 전에 죽으면 저널에는 주문번호 없는 INTENT만 남는다. 예전에는 이를 무조건 풀어,
//   KIS에 살아 있는 주문을 잊고 같은 수량을 또 낼 수 있었다.

void test_adopt_unnumbered_intent_matched()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);
    stub.open_orders.push_back(make_open_order("000660", OrderSide::BUY, 10, 75000.0, "0000000554")); // 다른 종목
    stub.open_orders.push_back(make_open_order("005930", OrderSide::SELL, 10, 75000.0, "0000000553")); // 반대 방향
    stub.open_orders.push_back(make_open_order("005930", OrderSide::BUY, 10, 75000.0, "0000000555"));

    const auto adopted = router.adopt_open_intents({reserve_intent(gate, 5, 0, 10)});
    assert(adopted.restored == 1 && adopted.released == 0);
    assert(gate.ledger().reserved("005930") == 10);            // 살아 있는 주문의 선점은 그대로
    assert(router.recent(1)[0].kis_order_no == "0000000555");
    assert(router.recent(1)[0].krx_forwarding_org_no == "ORG000777"); // 취소에 쓸 조직번호도 미체결에서 받는다

    // 늦은 체결통보가 짝지은 번호로 주문에 붙는다.
    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000000555"; fill_notification.ticker = "005930"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 10; fill_notification.filled_price = 75000.0; fill_notification.fill_time = "093001";
    router.on_fill(fill_notification);
    assert(gate.ledger().position("005930") == 10);
    assert(gate.ledger().reserved("005930") == 0);
    assert(router.recent(1)[0].signal.strategy_id == "DEVSCALE");
    PASS("adopt_unnumbered_intent_matched");
}

void test_adopt_unnumbered_intent_not_sent()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);
    stub.open_orders.push_back(make_open_order("005930", OrderSide::BUY, 10, 76000.0, "0000000556")); // 가격이 다르다

    const auto adopted = router.adopt_open_intents({reserve_intent(gate, 6, 0, 10)});
    assert(adopted.restored == 0 && adopted.released == 1);    // 안 나간 주문 — 선점만 푼다
    assert(gate.ledger().reserved("005930") == 0);
    PASS("adopt_unnumbered_intent_not_sent");
}

void test_adopt_unnumbered_skips_claimed_order()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);
    stub.open_orders.push_back(make_open_order("005930", OrderSide::BUY, 10, 75000.0, "0000000557"));

    // 같은 모양의 두 주문 — 번호를 아는 쪽이 제 주문을 쥐고, 번호 없는 쪽은 남의 주문과 짝지어지지 않는다.
    const auto adopted = router.adopt_open_intents({reserve_intent(gate, 7, 557, 10), reserve_intent(gate, 8, 0, 10)});
    assert(adopted.restored == 1 && adopted.released == 1);
    assert(gate.ledger().reserved("005930") == 10);
    assert(router.recent(1)[0].kis_order_no == "0000000557");
    PASS("adopt_unnumbered_skips_claimed_order");
}

void test_adopt_paper_keeps_accepted_rule()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    stub.paper = true;
    OrderRouter router(gate, stub);

    // 번호 없는 INTENT가 없으면 모의는 미체결을 묻지 않고, 접수를 본 주문을 되살린다.
    auto adopted = router.adopt_open_intents({reserve_intent(gate, 9, 559, 10)});
    assert(stub.open_order_calls == 0);
    assert(adopted.restored == 1 && adopted.released == 0);

    // 번호 없는 INTENT가 있으면 묻는다 — 목록에 없는 접수 주문은 종전대로 살리고, 번호 없는 것은 짝을 찾아 살린다.
    stub.open_orders.push_back(make_open_order("005930", OrderSide::BUY, 4, 75000.0, "0000000561")); // 죽은 사이 6주 체결
    adopted = router.adopt_open_intents({reserve_intent(gate, 10, 560, 10), reserve_intent(gate, 11, 0, 10)});
    assert(stub.open_order_calls == 1);
    assert(adopted.restored == 2 && adopted.released == 0);
    PASS("adopt_paper_keeps_accepted_rule");
}

// 실계좌 미체결 조회가 실패하면 "브로커에 못 물어봤다"로 본다 — 빈 목록으로 읽어 접수된 주문의 선점을 풀면
//  같은 수량이 또 나간다. 접수 기록이 없는 번호 없는 INTENT만 푼다. (전수조사 B1-2)
void test_adopt_open_orders_failure_keeps_accepted()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);
    stub.open_orders_fail = true;

    const auto adopted = router.adopt_open_intents({reserve_intent(gate, 12, 562, 10), reserve_intent(gate, 13, 0, 7)});
    assert(stub.open_order_calls == 1);
    assert(adopted.restored == 1 && adopted.released == 1);
    assert(gate.ledger().reserved("005930") == 10);
    assert(router.recent(1)[0].kis_order_no == "0000000562");
    PASS("adopt_open_orders_failure_keeps_accepted");
}

// 재기동 대조가 되살린 매도와 이전 세션 미체결 줄이 같은 ODNO다. 기동 취소 스레드가 그 줄을 취소하면
//  되살린 주문도 닫혀 선점이 풀려야 한다 — 10-06 모의 13:42 재기동 뒤 7종목이 선점에 묶여 매도가능 0으로 굳었다.
void test_stale_cancel_closes_restored_sell()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    stub.paper = true;
    OrderRouter router(gate, stub);

    const OrderGate::OrderRef reference{21, 571, OrderType::LIMIT};
    const bool                written = gate.ledger().on_intent("", "005930", OrderSide::SELL, 8, 80000.0, reference);
    assert(written);
    OrderGate::OpenIntent intent;
    intent.order_id         = 21;
    intent.kis_order_number = 571;
    intent.ticker           = "005930";
    intent.strategy_name    = "DEVSCALE";
    intent.side             = OrderSide::SELL;
    intent.type             = OrderType::LIMIT;
    intent.remaining        = 8;
    intent.price            = 80000.0;
    intent.accepted         = true;

    const auto adopted = router.adopt_open_intents({intent});
    assert(adopted.restored == 1);
    assert(gate.ledger().reserved("005930") == -8);

    const std::filesystem::path path = Logger::instance().path_for("open_orders.txt");
    {
        std::ofstream out(path, std::ios::trunc);
        out << "0000000571|ORG000777|005930|SELL|8\n";
    }

    router.cancel_stale_orders_async();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool       closed   = false;

    while (!closed && std::chrono::steady_clock::now() < deadline)
    {
        closed = router.recent(1)[0].status == OrderStatus::CANCELLED;

        if (!closed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    assert(closed);
    assert(gate.ledger().reserved("005930") == 0);
    std::error_code error_code;
    std::filesystem::remove(path, error_code);
    PASS("stale_cancel_closes_restored_sell");
}

void run_restart_tests()
{
    test_adopt_unnumbered_intent_matched();
    test_adopt_unnumbered_intent_not_sent();
    test_adopt_unnumbered_skips_claimed_order();
    test_adopt_paper_keeps_accepted_rule();
    test_adopt_open_orders_failure_keeps_accepted();
    test_stale_cancel_closes_restored_sell();
}
