// test_order_router — 나눠 보내기 — 전송 중 선점 정리, 전송 풀, 로컬 거부, 전송 예외, 닫기 전 체결.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

// ─── 선점 정리가 KIS 답을 기다리는 주문의 선점을 풀지 않는다 ─────────────────────
//   선점은 전송 전 INTENT 때 생기고 이력에는 답이 온 뒤에 들어간다. 그 사이에 데이터 스레드의 정리가 돌면
//   이력만 보고 방금 잡은 선점을 풀었다. 스텁이 전송 도중에 정리를 불러 그 순간을 그대로 만든다.
struct SweepDuringSendExecutor : StubOrderExecutor
{
    OrderRouter*     router = nullptr;
    std::atomic<int> released_during_send{-1}; // 전송 풀 스레드가 쓰고 메인 스레드가 읽는다

    SweepDuringSendExecutor() : StubOrderExecutor(true, "0000000222") {}

    OrderAck submit_order_acknowledgement(const OrderSignal& signal) override
    {
        if (router && signal.ticker == "005930")
        {
            released_during_send = router->sweep_stale_reservations();
        }

        return StubOrderExecutor::submit_order_acknowledgement(signal);
    }
};

void test_sweep_keeps_in_flight_reservation()
{
    OrderGate               gate(relaxed_config());
    SweepDuringSendExecutor stub;
    OrderRouter             router(gate, stub);
    stub.router = &router;

    // 이력이 비면 정리는 아무 것도 안 하므로, 다른 종목의 접수 한 건을 먼저 둔다.
    assert(router.submit(make_signal("000660", OrderSide::BUY, 5)).status == OrderStatus::ACCEPTED);

    const auto managed_order = router.submit(make_signal("005930", OrderSide::BUY, 10));
    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(stub.released_during_send == 0);
    assert(gate.ledger().reserved("005930") == 10);
    assert(gate.ledger().reserved("000660") == 5);
    PASS("sweep_keeps_in_flight_reservation");
}

// ─── 신규 주문 세 토막 — 전송만 다른 스레드에서 해도 선점·이력이 같다 (D-151) ──────
//   주문 스레드는 open_new·close_new, 전송 스레드는 send_new만 부른다. 전송 도중 정리가 돌아도 종목 표시가
//   send 안에 살아 있어 선점을 안 푼다. 전송 스레드 풀을 그대로 써서 엔진과 같은 길로 돈다.
void test_split_send_on_transport_pool()
{
    OrderGate               gate(relaxed_config());
    SweepDuringSendExecutor stub;
    OrderRouter             router(gate, stub);
    stub.router = &router;
    assert(router.submit(make_signal("000660", OrderSide::BUY, 5)).status == OrderStatus::ACCEPTED);

    struct Job
    {
        OrderRouter::NewOrderSend send;
    };

    wake::WakeGate              done_wake;
    order_transport::Pool<Job>  pool(1, [&router](Job& job)
    {
        router.send_new(job.send);
    }, done_wake);

    const uint64_t calls_before = router.kis_calls();
    auto           opened       = router.open_new(make_signal("005930", OrderSide::BUY, 10));
    assert(std::holds_alternative<OrderRouter::NewOrderSend>(opened));
    assert(router.kis_calls() == calls_before + 1); // 호출 셈은 열 때 한다 — 주문 스레드가 이 수로 간격을 센다
    assert(gate.ledger().reserved("005930") == 10); // INTENT에서 선점

    pool.dispatch(Job{std::move(std::get<OrderRouter::NewOrderSend>(opened))});
    assert(pool.full());

    std::optional<Job> done;

    while (!(done = pool.take_done()))
    {
        done_wake.wait_for(std::chrono::milliseconds(100), [&pool]
        {
            return pool.done_empty();
        });
    }

    assert(pool.in_flight() == 0);
    assert(stub.released_during_send == 0);

    const auto managed_order = router.close_new(std::move(done->send));
    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(managed_order.kis_order_no == "0000000222");
    assert(gate.ledger().reserved("005930") == 10);
    assert(router.recent(1)[0].order_id == managed_order.order_id); // 이력은 닫을 때 들어간다
    PASS("split_send_on_transport_pool");
}

// 게이트가 막으면 보낼 것이 없다 — 끝난 주문이 바로 온다.
void test_open_new_local_reject()
{
    OrderGate gate(relaxed_config());
    gate.set_kill_switch(true);
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    auto opened = router.open_new(make_signal("005930", OrderSide::BUY));
    assert(std::holds_alternative<ManagedOrder>(opened));
    assert(std::get<ManagedOrder>(opened).status == OrderStatus::REJECTED);
    assert(router.kis_calls() == 0 && stub.call_count == 0);
    PASS("open_new_local_reject");
}

// 전송이 예외로 끝나면 send_new는 삼키고 close_new가 거부로 닫는다 — 선점을 푼다.
struct ThrowingExecutor : StubOrderExecutor
{
    ThrowingExecutor() : StubOrderExecutor(true) {}

    OrderAck submit_order_acknowledgement(const OrderSignal&) override
    {
        throw std::runtime_error("연결 끊김");
    }
};

void test_split_send_transport_exception()
{
    OrderGate        gate(relaxed_config());
    ThrowingExecutor stub;
    OrderRouter      router(gate, stub);

    auto opened = router.open_new(make_signal("005930", OrderSide::BUY, 10));
    auto& send  = std::get<OrderRouter::NewOrderSend>(opened);
    std::thread sender([&router, &send]
    {
        router.send_new(send);
    });
    sender.join();

    const auto managed_order = router.close_new(std::move(send));
    assert(managed_order.status == OrderStatus::REJECTED);
    assert(managed_order.reject_reason.find("KIS 예외: 연결 끊김") != std::string::npos);
    assert(gate.ledger().reserved("005930") == 0);
    PASS("split_send_transport_exception");
}

// 답을 받고 close_new가 ODNO를 적기 전에 체결통보가 먼저 오면 붙들었다가 닫을 때 연결한다 (D-151).
//  미연결로 넣으면 전략 귀속을 잃고 선점이 안 풀린다. 정말 모르는 ODNO는 닫은 뒤 미연결로 들어간다.
void test_fill_before_close_new_links()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000088");
    OrderRouter       router(gate, stub);

    auto  opened = router.open_new(make_signal("005930", OrderSide::BUY, 10));
    auto& send   = std::get<OrderRouter::NewOrderSend>(opened);
    router.send_new(send);

    FillNotification early;
    early.kis_order_no    = "0000000088";
    early.ticker          = "005930";
    early.side            = OrderSide::BUY;
    early.filled_quantity = 10;
    early.filled_price    = 75000.0;
    early.fill_time       = "100000";
    router.on_fill(early);

    FillNotification stranger = early;
    stranger.kis_order_no     = "0000000999";
    stranger.ticker           = "000660";
    stranger.filled_quantity  = 3;
    router.on_fill(stranger);

    // 닫기 전 — 둘 다 붙들려 원장에 안 들어갔다.
    assert(gate.ledger().position("005930") == 0);
    assert(gate.ledger().position("000660") == 0);
    assert(gate.ledger().reserved("005930") == 10);

    const auto managed_order = router.close_new(std::move(send));
    assert(managed_order.status == OrderStatus::ACCEPTED);

    const auto recent = router.recent(1);
    assert(recent[0].status == OrderStatus::FILLED);
    assert(recent[0].confirmed_quantity == 10);
    assert(gate.ledger().position("005930") == 10);
    assert(gate.ledger().reserved("005930") == 0);
    assert(gate.ledger().position("000660") == 3); // 보내는 중인 주문이 없어 미연결로 들어갔다
    PASS("fill_before_close_new_links");
}

void run_send_tests()
{
    test_sweep_keeps_in_flight_reservation();
    test_split_send_on_transport_pool();
    test_open_new_local_reject();
    test_split_send_transport_exception();
    test_fill_before_close_new_links();
}
