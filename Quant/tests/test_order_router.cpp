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
#include <chrono>
#include <ctime>
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
    // MM-1 확장 — 취소/정정 경로 추적
    std::string orgno        = "ORG000001"; // submit_order_ack가 반환할 조직번호
    bool        cancel_ok    = true;          // cancel_order 성공 여부
    bool        revise_ok    = true;          // revise_order 성공 여부
    int         cancel_calls = 0;
    int         revise_calls = 0;
    int         last_cancel_qty = -1;         // 마지막 취소에 전달된 qty(잔량 재계산 검증)

    explicit StubOrderExecutor(bool s, std::string o = "A000000042")
        : succeed(s), odno(std::move(o))
    {
    }

    std::string submit_order(const OrderSignal&) override
    {
        ++call_count;
        return succeed ? odno : "";
    }

    // 기존 call_count 계약 유지를 위해 submit_order를 내부 호출 + 조직번호만 덧붙임
    OrderAck submit_order_ack(const OrderSignal& sig) override
    {
        std::string o = submit_order(sig);
        return OrderAck{o, o.empty() ? std::string() : orgno};
    }

    std::string cancel_order(const std::string&, const std::string&, const std::string&,
                             int qty, bool) override
    {
        ++cancel_calls;
        last_cancel_qty = qty;
        return cancel_ok ? std::string("C000000001") : std::string();
    }

    std::string revise_order(const std::string&, const std::string&, const std::string&,
                             int, double) override
    {
        ++revise_calls;
        return revise_ok ? std::string("R000000001") : std::string();
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

// ─── 테스트 7: 중복 체결통보 멱등 처리 (C1 fix) ──────────────────────────────
//   부분체결 중 동일 통보가 재수신돼도 confirmed_qty가 이중 반영되면 안 됨
void test_duplicate_fill_ignored()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000077");
    OrderRouter       router(gate, stub);

    router.submit(make_signal("005930", OrderSide::BUY, 10)); // ACCEPTED, ODNO=K000077

    // 부분체결 5주 통보
    FillNotification fn;
    fn.odno         = "K000077";
    fn.ticker       = "005930";
    fn.side         = OrderSide::BUY;
    fn.filled_qty   = 5;
    fn.filled_price = 75000.0;
    fn.fill_time    = "100000";
    router.on_fill(fn);

    auto h1 = router.recent(1);
    assert(h1[0].confirmed_qty == 5);
    assert(h1[0].status == OrderStatus::ACCEPTED);

    // 동일 체결통보 중복 수신 → 무시되어야 함 (confirmed_qty 증가 X, FILLED 전환 X)
    router.on_fill(fn);
    auto h2 = router.recent(1);
    assert(h2[0].confirmed_qty == 5);
    assert(h2[0].status == OrderStatus::ACCEPTED);

    // 나머지 5주는 다른 체결(fill_time 상이) → 정상 처리되어 전량 체결
    fn.fill_time = "100005";
    router.on_fill(fn);
    auto h3 = router.recent(1);
    assert(h3[0].confirmed_qty == 10);
    assert(h3[0].status == OrderStatus::FILLED);
    PASS("duplicate_fill_ignored");
}

// ─── 테스트 8: cross-day 멱등키 (V-4 fix) ────────────────────────────────────
//   ODNO는 영업일 단위 재사용 + fill_time은 HHMMSS(날짜 없음). 다른 거래일의 동일
//   (odno,fill_time,qty,price) 통보가 전일 체결로 오인돼 drop되면 실체결 누락 사고.
//   거래일 prefix로 차단 — 둘 다 정상 반영되어야 한다.
void test_cross_day_fill_not_deduped()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000077");
    OrderRouter       router(gate, stub);
    router.submit(make_signal("005930", OrderSide::BUY, 10));

    auto mk_ts = [](int y, int mo, int d) {
        std::tm t{}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
        t.tm_hour = 10; t.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&t));
    };

    FillNotification fn;
    fn.odno = "K000077"; fn.ticker = "005930"; fn.side = OrderSide::BUY;
    fn.filled_qty = 5; fn.filled_price = 75000.0; fn.fill_time = "100000";

    fn.timestamp = mk_ts(2024, 1, 10);   // 거래일 1
    router.on_fill(fn);
    assert(router.recent(1)[0].confirmed_qty == 5);

    // 동일 (odno,fill_time,qty,price) + 다른 거래일 → 별개 체결로 처리(중복 아님)
    fn.timestamp = mk_ts(2024, 1, 11);   // 거래일 2
    router.on_fill(fn);
    assert(router.recent(1)[0].confirmed_qty == 10);   // 누락 없이 누적
    assert(router.recent(1)[0].status == OrderStatus::FILLED);
    PASS("cross_day_fill_not_deduped");
}

// ─── 테스트 9: CANCEL 경로 — reserved 해제 + orgno 캡처 (MM-1) ────────────────
void test_cancel_releases_reserved()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000111");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_oid  = "MM:B:1";
    auto mo = router.submit(buy);
    assert(mo.status == OrderStatus::ACCEPTED);
    assert(mo.krx_orgno == "ORG000001");       // submit_order_ack가 조직번호 캡처
    assert(gate.reserved("005930") == 10);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.strategy_id     = "MM";
    cancel.action          = OrderAction::CANCEL;
    cancel.orig_client_oid = "MM:B:1";
    auto cm = router.submit(cancel);

    assert(cm.status == OrderStatus::CANCELLED);
    assert(stub.cancel_calls == 1);
    assert(stub.last_cancel_qty == 10);         // 미체결 전량
    assert(gate.reserved("005930") == 0);       // 선점 해제
    PASS("cancel_releases_reserved");
}

// ─── 테스트 10: 존재하지 않는 oid 취소 → REJECTED, KIS 미호출 ────────────────
void test_cancel_unknown_oid()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000222");
    OrderRouter       router(gate, stub);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.action          = OrderAction::CANCEL;
    cancel.orig_client_oid = "NOPE";
    auto cm = router.submit(cancel);

    assert(cm.status == OrderStatus::REJECTED);
    assert(stub.cancel_calls == 0);
    PASS("cancel_unknown_oid");
}

// ─── 테스트 11: 부분체결 중 취소 → 잔량만 취소·해제 ──────────────────────────
void test_partial_fill_then_cancel()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000333");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_oid  = "MM:B:1";
    router.submit(buy);
    assert(gate.reserved("005930") == 10);

    FillNotification fn;
    fn.odno = "K000333"; fn.ticker = "005930"; fn.side = OrderSide::BUY;
    fn.filled_qty = 4; fn.filled_price = 75000.0; fn.fill_time = "100000";
    router.on_fill(fn);
    assert(gate.reserved("005930") == 6);   // 10 - 4
    assert(gate.position("005930") == 4);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.action          = OrderAction::CANCEL;
    cancel.orig_client_oid = "MM:B:1";
    auto cm = router.submit(cancel);

    assert(cm.status == OrderStatus::CANCELLED);
    assert(stub.last_cancel_qty == 6);      // 미체결 잔량만
    assert(gate.reserved("005930") == 0);   // 잔량 6 해제
    assert(gate.position("005930") == 4);   // 체결분은 불변
    PASS("partial_fill_then_cancel");
}

// ─── 테스트 12: 전량체결 후 취소 → 자가치유(REJECTED, 이중해제 없음) ─────────
void test_cancel_after_full_fill_selfheal()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000444");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_oid  = "MM:B:1";
    router.submit(buy);

    FillNotification fn;
    fn.odno = "K000444"; fn.ticker = "005930"; fn.side = OrderSide::BUY;
    fn.filled_qty = 10; fn.filled_price = 75000.0; fn.fill_time = "100000";
    router.on_fill(fn);
    assert(gate.reserved("005930") == 0);
    assert(gate.position("005930") == 10);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.action          = OrderAction::CANCEL;
    cancel.orig_client_oid = "MM:B:1";
    auto cm = router.submit(cancel);

    assert(cm.status == OrderStatus::REJECTED); // 이미 FILLED → 취소 대상 없음
    assert(stub.cancel_calls == 0);
    assert(gate.reserved("005930") == 0);       // 이중해제 없음
    assert(gate.position("005930") == 10);
    PASS("cancel_after_full_fill_selfheal");
}

// ─── 테스트 13: REPLACE(정정) — 잔량 해제 후 new_qty 재선점 ──────────────────
void test_replace_reserves_new_qty()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000555");
    OrderRouter       router(gate, stub);

    OrderSignal buy = make_signal("005930", OrderSide::BUY, 10);
    buy.client_oid  = "MM:B:1";
    router.submit(buy);
    assert(gate.reserved("005930") == 10);

    OrderSignal rep;
    rep.ticker          = "005930";
    rep.side            = OrderSide::BUY;
    rep.type            = OrderType::LIMIT;
    rep.quantity        = 8;
    rep.price           = 74000.0;
    rep.action          = OrderAction::REPLACE;
    rep.orig_client_oid = "MM:B:1";
    rep.client_oid      = "MM:B:2";
    auto rm = router.submit(rep);

    assert(rm.status == OrderStatus::ACCEPTED);
    assert(rm.kis_order_no == "R000000001");
    assert(stub.revise_calls == 1);
    assert(gate.reserved("005930") == 8);   // 10 해제 후 8 재선점
    PASS("replace_reserves_new_qty");
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
    test_duplicate_fill_ignored();
    test_cross_day_fill_not_deduped();
    test_cancel_releases_reserved();
    test_cancel_unknown_oid();
    test_partial_fill_then_cancel();
    test_cancel_after_full_fill_selfheal();
    test_replace_reserves_new_qty();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
