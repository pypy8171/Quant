// test_order_router — 사유 기록 재기동 복원·대조 행·순번 열.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

// ─── 테스트 15: 재기동 복원 — 사유 기록으로 ODNO 귀속과 잔량 클램프 회복 ─────
//   이전 세션이 낸 주문이 재기동 뒤에 체결되면, 예전엔 전략도 사유도 모르는 미매핑
//   체결로 들어가고 주문수량을 몰라 잔량 클램프도 없었다. 접수 시점에 남긴 기록을
//   읽어 주문을 되살리면 둘 다 복구된다.
void test_reason_journal_restart_recovery()
{
    OrderSignal buy = make_signal("047050", OrderSide::BUY, 100);
    buy.strategy_id = "DEVSCALE";
    buy.reason      = "정배열 눌림 진입";

    // 1차 세션 — 접수까지만 하고 끝난다(체결 전 재기동).
    {
        OrderGate         gate(relaxed_config());
        StubOrderExecutor stub(true, "0000000777");
        OrderRouter       router(gate, stub);
        auto managed_order = router.submit(buy);
        assert(managed_order.status == OrderStatus::ACCEPTED);
        assert(gate.ledger().reserved("047050") == 100);
    }

    // 2차 세션 — 메모리 이력이 빈 상태에서 같은 ODNO의 체결이 들어온다.
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000888");
    OrderRouter       router(gate, stub);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000000777"; fill_notification.ticker = "047050"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 60; fill_notification.filled_price = 54700.0; fill_notification.fill_time = "110707";
    router.on_fill(fill_notification);

    assert(gate.ledger().position("047050") == 60);
    assert(gate.ledger().reserved("047050") == 40);   // 주문수량 100을 되살리고 60만 해제

    // 전략 귀속이 미연결이 아니라 원래 전략으로 남는다.
    auto history = router.recent(5);
    bool found = false;

    for (const auto& history_entry : history)
    {
        if (history_entry.kis_order_no == "0000000777")
        {
            found = true;
            assert(history_entry.signal.strategy_id == "DEVSCALE");
            assert(history_entry.signal.quantity == 100);
        }
    }

    assert(found);

    // 잔량 클램프 회복 — 남은 40주보다 많은 통보가 와도 100을 넘지 않는다.
    fill_notification.fill_time = "110709"; fill_notification.filled_quantity = 60;
    router.on_fill(fill_notification);
    assert(gate.ledger().position("047050") == 100);

    PASS("reason_journal_restart_recovery");
}


// ─── C-2: 잔고 대조 행 — RECONCILE 행이 원장·브로커 수량과 살아있는 주문 수를 남긴다 ──────
void test_reconcile_row_written()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000201");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 10)); // ACCEPTED, 미체결 → live_orders=1

    OrderRouter::ReconcileNote reconcile_note;
    reconcile_note.ticker     = "005930";
    reconcile_note.ledger_quantity = 10;
    reconcile_note.broker_quantity = 7;
    reconcile_note.ledger_average = 75000.0;
    reconcile_note.broker_average = 74900.0;
    reconcile_note.action     = "OVERWRITE";
    reconcile_note.note       = "mode=REST";
    router.record_reconcile(reconcile_note);

    router.flush_file_writes();   // 원장 행은 전담 스레드가 쓴다 — 읽기 전에 내린다(D-124)
    auto rows = tail_trade_rows(1);
    assert(rows.size() == 1);
    auto other_split_csv = split_csv(rows[0]);
    assert(other_split_csv.size() == 18);              // 헤더 열 수와 같다(strategy_realized_pnl까지)
    assert(other_split_csv[1] == "RECONCILE");
    assert(other_split_csv[5] == "005930");
    assert(other_split_csv[8] == "10" && other_split_csv[10] == "7"); // order_quantity=원장, fill_quantity=브로커
    assert(other_split_csv[12] == "OVERWRITE");
    assert(other_split_csv[13] == "live_orders=1 diff_qty=-3 mode=REST");
    assert(other_split_csv[16].empty() && other_split_csv[17].empty()); // sequence·strategy_realized_pnl 빈 칸
    PASS("reconcile_row_written");
}

// ─── C-2: sequence 전파 — 전략이 stamp한 순번이 접수 행과 체결 행에 그대로 남는다 ────────────
void test_sequence_propagates_to_rows()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000202");
    OrderRouter       router(gate, stub);

    OrderSignal signal = make_signal("005930", OrderSide::BUY, 3);
    signal.sequence         = 77;
    auto managed_order         = router.submit(signal);
    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(managed_order.signal.sequence == 77);

    FillNotification fill_notification;
    fill_notification.kis_order_no         = "0000000202";
    fill_notification.ticker       = "005930";
    fill_notification.side         = OrderSide::BUY;
    fill_notification.filled_quantity   = 3;
    fill_notification.filled_price = 75000.0;
    fill_notification.fill_time    = "100100";
    router.on_fill(fill_notification);

    router.flush_file_writes();
    auto rows = tail_trade_rows(2);
    assert(rows.size() == 2);
    auto accepted_row  = split_csv(rows[0]);
    auto fill = split_csv(rows[1]);
    assert(accepted_row[1] == "ACCEPTED" && accepted_row[16] == "77");
    assert(fill[1] == "FILL" && fill[16] == "77");

    // 미부여(0)는 빈 칸으로 남는다 — 0이 진짜 순번으로 읽히지 않게.
    (void)router.submit(make_signal("005930", OrderSide::BUY, 1));
    router.flush_file_writes();
    auto last = split_csv(tail_trade_rows(1)[0]);
    assert(last[1] == "ACCEPTED" && last[16].empty());
    PASS("seq_propagates_to_rows");
}

void run_journal_tests()
{
    test_reason_journal_restart_recovery();
    test_reconcile_row_written();
    test_sequence_propagates_to_rows();
}
