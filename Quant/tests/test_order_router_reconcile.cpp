// test_order_router — 내부 주문번호 읽기와 전송 시간초과 재확인.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

// 내부 주문번호 "ORD-000123"을 저널 정수로 — 통째로 읽으면 'O'에서 멈춰 늘 0이었다(09-28).
void test_order_number_of_reads_digits_after_prefix()
{
    assert(OrderRouter::order_number_of("ORD-000123") == 123);
    assert(OrderRouter::order_number_of("ORD-1234567") == 1234567);
    assert(OrderRouter::order_number_of("000123") == 0);
    assert(OrderRouter::order_number_of("ORD-12A") == 0);
    assert(OrderRouter::order_number_of("") == 0);
    PASS("order_number_of_reads_digits_after_prefix");
}

// ─── 전송 타임아웃 재확인: 몰린 종목을 모아 조회하고, 조회가 실패하면 성공할 때까지 다시 조회한다 ───
//  2026-10-07 09:00 모의: 시간초과 9건이 몰렸는데 앞 건이 도는 동안 온 종목은 버려졌고, 첫 조회 실패에 포기했다.
void test_transport_timeout_reconcile_retries_batch()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouterConfig config;
    config.reconcile_settle = std::chrono::milliseconds(5);
    config.reconcile_retry  = {std::chrono::milliseconds(5), std::chrono::milliseconds(5), std::chrono::milliseconds(5)};
#ifdef HAS_ZMQ
    OrderRouter router(gate, stub, nullptr, config);
#else
    OrderRouter router(gate, stub, config);
#endif

    const auto wait_until = [](const auto& condition)
    {
        for (int round = 0; round < 400 && !condition(); ++round)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return condition();
    };

    // 응답은 못 받았지만 둘 다 KIS에 접수돼 있다.
    OpenOrder first;
    first.ticker       = "005930";
    first.kis_order_no = "0000009001";
    first.psbl_qty     = 3;
    first.side         = OrderSide::SELL;
    OpenOrder second   = first;
    second.ticker       = "000660";
    second.kis_order_no = "0000009002";
    second.psbl_qty     = 2;
    stub.open_orders      = {first, second};
    stub.open_orders_fail = true;

    stub.fail_next  = 2;
    stub.error_code = kis_error::kTransport;
    assert(router.submit(make_signal("005930", OrderSide::SELL, 3)).status == OrderStatus::REJECTED);
    assert(router.submit(make_signal("000660", OrderSide::SELL, 2)).status == OrderStatus::REJECTED);

    // 조회가 계속 실패해도 포기하지 않는다.
    assert(wait_until([&stub]
    {
        return stub.open_order_calls.load() >= 3;
    }));
    assert(stub.cancel_calls == 0);

    // 조회가 살아나면 두 종목을 한 번에 지우고 스레드가 내려간다.
    stub.open_orders_fail = false;
    assert(wait_until([&stub]
    {
        return stub.cancel_calls.load() == 2;
    }));
    const int calls_after = stub.open_order_calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(stub.open_order_calls.load() == calls_after);
    assert(stub.cancel_calls == 2);
    PASS("transport_timeout_reconcile_retries_batch");
}

// ─── 기동 취소 보충: 방향을 모르는 브로커 미체결은 줄에 넣지 않는다 ───
//  예전에는 매수·매도 구분 코드가 01·02 밖이면 "BUY"로 적어 취소했다. 사유 기록은 같은 경우를 "SELL"로 적어
//  두 곳이 달랐다(4단계에서 둘 다 건너뛰기로 맞췄다). 부속 파일 줄 하나와 방향이 분명한 보충 한 건만 취소한다.
void test_broker_open_order_unknown_side_skipped()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    stub.open_orders.push_back(make_open_order("000660", OrderSide::NONE, 4, 75000.0, "0000009102"));
    stub.open_orders.push_back(make_open_order("035720", OrderSide::BUY, 2, 40000.0, "0000009103"));

    const std::filesystem::path path = Logger::instance().path_for("open_orders.txt");
    {
        std::ofstream out(path, std::ios::trunc);
        out << "0000009101|ORG000001|005930|BUY|3\n";
    }

    router.cancel_stale_orders_async();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (stub.cancel_calls.load() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(stub.cancel_calls.load() == 2); // 파일 줄 0000009101과 보충 0000009103만. NONE인 0000009102는 빠진다
    std::error_code error_code;
    std::filesystem::remove(path, error_code);
    PASS("broker_open_order_unknown_side_skipped");
}

void run_reconcile_tests()
{
    test_order_number_of_reads_digits_after_prefix();
    test_transport_timeout_reconcile_retries_batch();
    test_broker_open_order_unknown_side_skipped();
}
