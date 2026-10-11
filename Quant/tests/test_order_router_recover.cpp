// test_order_router — 놓친 체결 되찾기.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

// ─── 놓친 체결 되찾기 (D-149) ───────────────────────────────────────────────
//  체결통보 소켓이 끊긴 사이 난 체결은 통보가 다시 오지 않을 수 있다. 재구독 뒤 당일 체결 조회의 누적 수량·금액과
//  라우터가 받은 누적을 비교해 모자란 만큼을 체결로 넣는다. 단가는 누적 금액 차이 ÷ 모자란 수량이다.
static DailyOrderFill daily_fill_row(const std::string& kis_order_no, const std::string& ticker, int order_quantity,
                                     int filled_quantity, int64_t filled_amount)
{
    DailyOrderFill row;
    row.kis_order_no    = kis_order_no;
    row.ticker          = ticker;
    row.side            = OrderSide::BUY;
    row.order_quantity  = order_quantity;
    row.filled_quantity = filled_quantity;
    row.filled_amount   = filled_amount;
    return row;
}

void test_recover_missed_fill_by_amount_difference()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000301");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 10));

    FillNotification fill_notification;
    fill_notification.kis_order_no    = "0000000301";
    fill_notification.ticker          = "005930";
    fill_notification.side            = OrderSide::BUY;
    fill_notification.filled_quantity = 3;
    fill_notification.filled_price    = 75000.0;
    fill_notification.fill_time       = "000001";
    router.on_fill(fill_notification);

    // 끊긴 사이 3주가 76000원에 더 체결됐다 — 조회 누적은 6주, 3×75000 + 3×76000.
    stub.daily_fills = {daily_fill_row("0000000301", "005930", 10, 6, 3 * 75000 + 3 * 76000)};
    assert(router.recover_missed_fills() == 1);
    assert(stub.daily_fill_calls == 1);
    assert(router.recent(1)[0].confirmed_quantity == 6);
    assert(router.recent(1)[0].confirmed_amount == 3 * 75000 + 3 * 76000);
    assert(router.recent(1)[0].status == OrderStatus::ACCEPTED);
    assert(gate.ledger().position("005930") == 6);

    // 같은 조회를 다시 해도 차이가 없으면 넣지 않는다.
    assert(router.recover_missed_fills() == 0);
    assert(gate.ledger().position("005930") == 6);

    // 조회 시각 이전 체결의 늦은 통보는 되찾은 몫에서 빠지고 원장에 다시 들어가지 않는다.
    fill_notification.filled_price = 76000.0;
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 6);
    assert(router.recent(1)[0].recovered_credit_quantity == 0);
    assert(gate.ledger().position("005930") == 6);

    // 몫을 다 쓴 뒤의 통보, 조회 시각 뒤의 통보는 새 체결이다.
    fill_notification.filled_quantity = 4;
    fill_notification.fill_time       = "235959";
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 10);
    assert(router.recent(1)[0].status == OrderStatus::FILLED);
    assert(gate.ledger().position("005930") == 10);

    // 원장 CSV는 FILL 행으로 남고 사유 칸에 되찾은 체결임을 적는다.
    bool recovered_row = false;

    for (const auto& row : tail_trade_rows(8))
    {
        if (row.find("0000000301") != std::string::npos && row.find("조회로 되찾음") != std::string::npos &&
            row.find(",FILL,") != std::string::npos)
        {
            recovered_row = true;
        }
    }

    assert(recovered_row);
    PASS("recover_missed_fill_by_amount_difference");
}

// 되찾은 몫보다 큰 늦은 통보는 몫을 뺀 나머지만 넣는다. 조회 시각 뒤 통보는 몫이 있어도 깎지 않는다.
void test_recover_credit_partial_and_after_query_time()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000302");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 10));
    stub.daily_fills = {daily_fill_row("0000000302", "005930", 10, 2, 2 * 75000)};
    assert(router.recover_missed_fills() == 1);
    assert(router.recent(1)[0].recovered_credit_quantity == 2);

    FillNotification fill_notification;
    fill_notification.kis_order_no    = "0000000302";
    fill_notification.ticker          = "005930";
    fill_notification.side            = OrderSide::BUY;
    fill_notification.filled_quantity = 3;
    fill_notification.filled_price    = 75000.0;
    fill_notification.fill_time       = "235959"; // 조회 뒤 체결 — 몫과 무관
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 5);
    assert(router.recent(1)[0].recovered_credit_quantity == 2);

    fill_notification.fill_time = "000001"; // 조회 전 체결 3주 중 2주는 이미 넣었다
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 6);
    assert(router.recent(1)[0].recovered_credit_quantity == 0);
    assert(gate.ledger().position("005930") == 6);
    PASS("recover_credit_partial_and_after_query_time");
}

// 재기동으로 되살린 주문은 잔고 시드와 겹쳐 두 번 셀 수 있어 되찾지 않는다. 이 프로세스가 낸 미결 주문이 없으면 조회도 안 한다.
void test_recover_skips_restored_orders()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000303");
    OrderRouter       router(gate, stub);
    stub.open_orders_fail = true;

    (void)router.adopt_open_intents({reserve_intent(gate, 21, 571, 10)});
    stub.daily_fills = {daily_fill_row("0000000571", "005930", 10, 10, 10 * 75000),
                        daily_fill_row("0000000303", "000660", 5, 5, 5 * 120000)};
    assert(router.recover_missed_fills() == 0);
    assert(stub.daily_fill_calls == 0);
    assert(gate.ledger().position("005930") == 0);

    (void)router.submit(make_signal("000660", OrderSide::BUY, 5));
    assert(router.recover_missed_fills() == 1);
    assert(stub.daily_fill_calls == 1);
    assert(gate.ledger().position("000660") == 5);
    assert(gate.ledger().position("005930") == 0);

    // 조회 실패는 아무것도 바꾸지 않는다 — 잔고 대조가 맡는다.
    (void)router.submit(make_signal("035420", OrderSide::BUY, 2));
    stub.daily_fills_fail = true;
    assert(router.recover_missed_fills() == 0);
    assert(gate.ledger().position("035420") == 0);
    PASS("recover_skips_restored_orders");
}

void run_recover_tests()
{
    test_recover_missed_fill_by_amount_difference();
    test_recover_credit_partial_and_after_query_time();
    test_recover_skips_restored_orders();
}
