// test_order_router — 이번 세션 주문 닫기의 저널 번호와 이력 잠금 밖 장부 기록.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

// ─── 이번 세션 주문을 닫는 취소가 저널에 실제 주문번호로 남는다 ─────────────────────
//  close_session_order(청산차단 취소·기동 취소)는 CANCEL 저널의 주문번호를 digits_to_number("ORD-000001")로 적어
//  늘 0이었다. 리플레이는 번호 0 레코드를 미결 대조에 쓰지 않아, 재기동 때 이미 취소한 주문이 미결로 되살아났다.
void test_session_cancel_journals_order_number()
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "quant_router_test_session_cancel";
    const std::string           date      = "20261010";
    std::error_code             error_code;
    std::filesystem::remove_all(directory, error_code);
    std::filesystem::create_directories(directory, error_code);

    {
        OrderGate gate(relaxed_config());
        assert(gate.ledger().set_journal(directory, date, false));
        StubOrderExecutor stub(true, "0000000701");
        OrderRouter       router(gate, stub);
        stub.paper            = true;
        stub.open_orders_fail = true; // 조회 실패 → 이력으로 물러난다(test_blocked_sell_releases_reservation과 같은 장면)

        OrderSignal reserved      = make_signal("005930", OrderSide::SELL, 8);
        reserved.type             = OrderType::LIMIT;
        reserved.price            = 80000.0;
        reserved.client_order_id  = "RESV:1";
        assert(router.submit(reserved).status == OrderStatus::ACCEPTED);

        stub.kis_order_no = "0000000702";
        stub.fail_next    = 1;
        stub.error_code   = "40240000";
        OrderSignal liquidation     = make_signal("005930", OrderSide::SELL, 8);
        liquidation.type            = OrderType::MARKET;
        liquidation.price           = 0.0;
        liquidation.reference_price = 75000.0;
        assert(router.submit(liquidation).status == OrderStatus::ACCEPTED);
        assert(stub.cancel_calls == 1);
    }

    // 재기동 — 취소된 예약매도(ODNO 701)는 미결로 남지 않고, 재매도(ODNO 702)만 남는다.
    OrderGate restarted(relaxed_config());
    assert(restarted.ledger().set_journal(directory, date, false));
    const auto intents          = restarted.ledger().open_intents();
    int        cancelled_left   = 0;
    int        resell_remaining = 0;

    for (const auto& intent : intents)
    {
        if (intent.kis_order_number == 701)
        {
            ++cancelled_left;
        }

        if (intent.kis_order_number == 702)
        {
            resell_remaining = intent.remaining;
        }
    }

    assert(cancelled_left == 0);
    assert(resell_remaining == 8);
    std::filesystem::remove_all(directory, error_code);
    PASS("session_cancel_journals_order_number");
}

// ─── 장부 기록을 history_mutex_ 밖으로 옮긴 뒤에도 선점이 맞는다 ─────────────────────
//  체결·취소의 장부 기록(저널 디스크 쓰기)은 이력 락을 푼 뒤에 한다. 다른 스레드가 그 사이 이력 락을 잡는 조회와
//  선점 정리를 쉬지 않고 불러도 (1) 멈추지 않고 (2) 살아 있는 주문의 선점을 풀지 않고 (3) 끝난 뒤 선점이 0이어야 한다.
//  정리가 "이력은 닫혔고 장부 해제는 아직"인 틈을 보면 선점을 먼저 지운 뒤 늦은 해제가 다음 주문의 선점을 깎는다 —
//  (2)·(3)의 단언이 그것을 잡는다.
void test_ledger_calls_outside_history_lock()
{
    OrderGate::Config config      = relaxed_config();
    config.max_orders_per_min     = 100000;
    config.max_orders_per_sec     = 100000;
    config.max_quantity_per_ticker = 100000;
    OrderGate         gate(config);
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    // 이력이 비면 정리가 아무 것도 안 하므로 다른 종목의 접수 한 건을 둔다.
    stub.kis_order_no = "0000008000";
    assert(router.submit(make_signal("000660", OrderSide::BUY, 5)).status == OrderStatus::ACCEPTED);

    std::atomic<bool> stop{false};
    std::atomic<int>  rounds{0};
    std::thread       reader([&]
    {
        while (!stop.load())
        {
            (void)router.recent(5);                 // history_mutex_
            (void)router.sweep_stale_reservations(); // in_flight_mutex_ → history_mutex_, 그다음 장부
            (void)gate.ledger().reserved("005930"); // 장부 positions_mutex_
            ++rounds;
        }
    });

    constexpr int kCycles = 200;

    for (int cycle = 1; cycle <= kCycles; ++cycle)
    {
        const std::string digits       = std::to_string(1000 + cycle);
        const std::string kis_order_no = std::string(10 - digits.size(), '0') + digits; // 순환마다 다른 10자리 ODNO
        stub.kis_order_no              = kis_order_no;

        OrderSignal buy         = make_signal("005930", OrderSide::BUY, 10);
        buy.client_order_id     = "LOCK:B:" + std::to_string(cycle);
        buy.client_order_number = static_cast<uint64_t>(cycle);
        assert(router.submit(buy).status == OrderStatus::ACCEPTED);

        FillNotification fill_notification;
        fill_notification.kis_order_no    = kis_order_no;
        fill_notification.ticker          = "005930";
        fill_notification.side            = OrderSide::BUY;
        fill_notification.filled_quantity = 4;
        fill_notification.filled_price    = 75000.0;
        fill_notification.fill_time       = std::to_string(100000 + cycle);
        router.on_fill(fill_notification);
        assert(gate.ledger().reserved("005930") == 6); // 살아 있는 주문의 잔량 — 정리가 풀면 0이 된다

        OrderSignal cancel                  = make_signal("005930", OrderSide::BUY, 0);
        cancel.action                       = OrderAction::CANCEL;
        cancel.original_client_order_id     = buy.client_order_id;
        cancel.original_client_order_number = buy.client_order_number;
        assert(router.submit(cancel).status == OrderStatus::CANCELLED);
        assert(gate.ledger().reserved("005930") == 0);
    }

    stop = true;
    reader.join();
    assert(rounds.load() > 0);
    assert(gate.ledger().position("005930") == 4 * kCycles);
    assert(gate.ledger().reserved("005930") == 0);
    assert(gate.ledger().reserved("000660") == 5); // 살아 있는 다른 종목 주문의 선점은 그대로
    PASS("ledger_calls_outside_history_lock");
}

void run_session_tests()
{
    test_session_cancel_journals_order_number();
    test_ledger_calls_outside_history_lock();
}
