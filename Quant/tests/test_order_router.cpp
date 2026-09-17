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
#include "utils/Logger.h"
#include <filesystem>
#include <cassert>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// ─── KIS 응답 시뮬레이션 Stub ─────────────────────────────────────────────────
struct StubOrderExecutor : IOrderExecutor
{
    bool        succeed;
    std::string kis_order_no;
    int         call_count = 0;
    // MM-1 확장 — 취소/정정 경로 추적
    std::string orgno        = "ORG000001"; // submit_order_ack가 반환할 조직번호
    bool        cancel_ok    = true;          // cancel_order 성공 여부
    bool        revise_ok    = true;          // revise_order 성공 여부
    int         cancel_calls = 0;
    int         revise_calls = 0;
    int         last_cancel_qty = -1;         // 마지막 취소에 전달된 quantity(잔량 재계산 검증)
    // C-2 청산차단 경로 — 다음 fail_next건은 err_code로 실패, 그 뒤 성공
    bool        paper     = false;
    int         fail_next = 0;
    std::string err_code;

    explicit StubOrderExecutor(bool flag, std::string output = "A000000042")
        : succeed(flag), kis_order_no(std::move(output))
    {
    }

    bool is_paper() const noexcept override { return paper; }

    OrderAck submit_order_ack(const OrderSignal&) override
    {
        ++call_count;

        if (fail_next > 0)
        {
            --fail_next;
            return OrderAck::fail(err_code.empty() ? std::string("E_TEST") : err_code);
        }

        return succeed ? OrderAck{kis_order_no, orgno, std::string()} : OrderAck::fail("E_TEST");
    }

    OrderAck cancel_order(const std::string&, const std::string&, const std::string&,
                          int quantity, bool) override
    {
        ++cancel_calls;
        last_cancel_qty = quantity;
        return cancel_ok ? OrderAck{"C000000001", std::string(), std::string()} : OrderAck::fail("E_TEST");
    }

    OrderAck revise_order(const std::string&, const std::string&, const std::string&,
                          int, double) override
    {
        ++revise_calls;
        return revise_ok ? OrderAck{"R000000001", std::string(), std::string()} : OrderAck::fail("E_TEST");
    }
};

// ─── 헬퍼 ─────────────────────────────────────────────────────────────────────
static OrderSignal make_signal(const std::string& ticker, OrderSide side, int quantity = 1)
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.quantity    = quantity;
    signal.price       = 75000.0;
    signal.strategy_id = "TEST";
    signal.market      = Market::KR;
    return signal;
}

static OrderGate::Config relaxed_cfg()
{
    OrderGate::Config config;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    config.dedup_window_sec   = 0.0;
    return config;
}

// 오늘 원장 CSV의 마지막 n줄(헤더 제외). 행 검증용.
static std::vector<std::string> tail_trade_rows(size_t count)
{
    std::time_t tt = std::time(nullptr);
    std::tm     lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char buffer[9];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &lt);
    std::ifstream in(Logger::instance().path_for(std::string("trades_") + buffer + ".csv"));
    std::vector<std::string> rows;

    for (std::string ln; std::getline(in, ln); )
    {
        if (!ln.empty() && ln.back() == '\r')
        {
            ln.pop_back();
        }

        if (!ln.empty() && ln.rfind("ts_kst,", 0) != 0)
        {
            rows.push_back(ln);
        }
    }

    if (rows.size() > count)
    {
        rows.erase(rows.begin(), rows.end() - static_cast<long>(count));
    }

    return rows;
}

static std::vector<std::string> split_csv(const std::string& ln)
{
    std::vector<std::string> out;
    std::string cur;

    for (char character : ln)
    {
        if (character == ',')
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += character;
        }
    }

    out.push_back(cur);
    return out;
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

    auto managed_order = router.submit(make_signal("005930", OrderSide::BUY));

    assert(managed_order.status == OrderStatus::REJECTED);
    assert(managed_order.reject_reason.find("KILL") != std::string::npos);
    assert(stub.call_count == 0); // KIS 호출 없어야 함

    auto stats = router.stats();
    assert(stats.total == 1 && stats.accepted == 0 && stats.rejected == 1);
    PASS("gate_rejected");
}

// ─── 테스트 2: KIS 성공 → ACCEPTED, ODNO 저장 ────────────────────────────────
void test_kis_accepted()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000012345");
    OrderRouter       router(gate, stub);

    auto managed_order = router.submit(make_signal("005930", OrderSide::BUY, 1));

    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(managed_order.kis_order_no == "K000012345");
    assert(stub.call_count == 1);

    auto stats = router.stats();
    assert(stats.total == 1 && stats.accepted == 1 && stats.rejected == 0);
    PASS("kis_accepted");
}

// ─── 테스트 3: KIS 실패 (빈 ODNO) → REJECTED with "KIS" ──────────────────────
void test_kis_failed()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(false); // 빈 문자열 반환
    OrderRouter       router(gate, stub);

    auto managed_order = router.submit(make_signal("005930", OrderSide::BUY, 1));

    assert(managed_order.status == OrderStatus::REJECTED);
    assert(managed_order.reject_reason.find("KIS") != std::string::npos);
    assert(stub.call_count == 1); // 게이트 통과 후 KIS 호출은 됨

    auto stats = router.stats();
    assert(stats.total == 1 && stats.accepted == 0 && stats.rejected == 1);
    PASS("kis_failed");
}

// ─── 테스트 4: 혼합 제출 후 Stats 검증 ───────────────────────────────────────
void test_stats_mixed()
{
    OrderGate         gate(relaxed_cfg());
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

    auto stats = router.stats();
    assert(stats.total == 4 && stats.accepted == 3 && stats.rejected == 1);
    PASS("stats_mixed");
}

// ─── 테스트 5: recent() 이력 N건 반환 ────────────────────────────────────────
void test_history_recent()
{
    OrderGate         gate(relaxed_cfg());
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
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true);
    OrderRouter       router(gate, stub);

    auto mo1 = router.submit(make_signal("005930", OrderSide::BUY));
    auto mo2 = router.submit(make_signal("000660", OrderSide::BUY));

    assert(mo1.order_id == "ORD-000001");
    assert(mo2.order_id == "ORD-000002");
    PASS("order_id_sequence");
}

// ─── 테스트 7: 과체결 방어는 주문잔량 상한이 담당 ────────────────────────
//   (kis_order_no,체결시각,수량,단가)는 유일하지 않다 — 같은 초에 같은 수량·단가로 나뉘어
//   체결되면 서로 다른 실체결이 같은 키를 갖는다. 그래서 같은 키의 통보도 각각 반영하고,
//   대신 누적 체결이 주문수량을 넘지 못하게 클램프해 과체결을 막는다.
void test_duplicate_fill_ignored()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000077");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 10)); // ACCEPTED, ODNO=K000077

    // 부분체결 5주 통보
    FillNotification fill_notification;
    fill_notification.kis_order_no         = "K000077";
    fill_notification.ticker       = "005930";
    fill_notification.side         = OrderSide::BUY;
    fill_notification.filled_quantity   = 5;
    fill_notification.filled_price = 75000.0;
    fill_notification.fill_time    = "100000";
    router.on_fill(fill_notification);

    auto h1 = router.recent(1);
    assert(h1[0].confirmed_quantity == 5);
    assert(h1[0].status == OrderStatus::ACCEPTED);

    // 같은 키의 두 번째 통보 = 같은 초의 또 다른 5주 분할체결 → 반영되어 전량 체결
    router.on_fill(fill_notification);
    auto h2 = router.recent(1);
    assert(h2[0].confirmed_quantity == 10);
    assert(h2[0].status == OrderStatus::FILLED);

    // 주문수량(10주)을 이미 채웠으므로 그 이상은 반영되지 않는다.
    //  ODNO는 아는 주문이므로 미매핑(ORPHAN) 경로로 새어 포지션이 부풀어도 안 된다.
    fill_notification.fill_time = "100005";
    router.on_fill(fill_notification);
    auto h3 = router.recent(1);
    assert(h3[0].confirmed_quantity == 10);
    assert(h3[0].status == OrderStatus::FILLED);
    assert(gate.position("005930") == 10);  // 15주로 부풀지 않음
    PASS("duplicate_fill_ignored");
}

// ─── 테스트 7b: 미매핑 체결도 원장·포지션에 반영 ────────────────────────
//   장중 재시작하면 이전 세션의 미체결 주문이 history_에서 사라진다. 거래소 호가창에는
//   그대로 살아있으므로 나중에 체결통보가 들어오는데, 예전에는 통째로 버려져 원장이
//   어긋났다(2026-09-07 047050 91주). 이제는 포지션에 반영하고, 선점이 없던 상태라
//   reserved_가 음수로 내려가지 않아야 한다.
void test_unmapped_fill_applied()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000555");
    OrderRouter       router(gate, stub);

    // 이 라우터가 낸 적 없는 ODNO의 체결통보
    FillNotification fill_notification;
    fill_notification.kis_order_no = "PREV-SESSION"; fill_notification.ticker = "047050"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 91; fill_notification.filled_price = 54700.0; fill_notification.fill_time = "110707";
    router.on_fill(fill_notification);

    assert(gate.position("047050") == 91);  // 원장에 반영
    assert(gate.reserved("047050") == 0);   // 없던 선점을 깎아 음수로 만들지 않음
    PASS("unmapped_fill_applied");
}

// ─── 테스트 7c: 미매핑 체결의 재전송은 한 번만 반영 (W-6 회귀) ──────────────
//   미연결은 history_에 없어 exhausted 판정이 못 잡고, 주문수량도 몰라 잔량 클램프도 없다.
//   같은 키(거래일:kis_order_no:시각:수량:단가)의 2회차는 무시하고, 키가 다른 후속 분할체결은 반영한다.
void test_unmapped_fill_duplicate_ignored()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000556");
    OrderRouter       router(gate, stub);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "PREV-SESSION"; fill_notification.ticker = "047050"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 91; fill_notification.filled_price = 54700.0; fill_notification.fill_time = "110707";
    router.on_fill(fill_notification);
    router.on_fill(fill_notification);                    // WS 재구독 재전송
    assert(gate.position("047050") == 91); // 182로 부풀지 않음
    assert(gate.reserved("047050") == 0);

    fill_notification.fill_time = "110709"; fill_notification.filled_quantity = 9; // 같은 주문의 다음 분할체결(키 다름)
    router.on_fill(fill_notification);
    assert(gate.position("047050") == 100);

    // 평단 미상 미연결 SELL — 실현이익을 만들지 않는다(C-1)
    FillNotification orphan_sell_fill;
    orphan_sell_fill.kis_order_no = "PREV-SELL"; orphan_sell_fill.ticker = "316140"; orphan_sell_fill.side = OrderSide::SELL;
    orphan_sell_fill.filled_quantity = 75; orphan_sell_fill.filled_price = 34050.0; orphan_sell_fill.fill_time = "093000";
    router.on_fill(orphan_sell_fill);

    // 위 BUY 100주(91+9, 평단 54700)의 매수 수수료가 발생 즉시 차감돼 있다.
    //  SELL(316140)은 평단 미상이라 0을 더할 뿐 — 실현이익은 안 생긴다(C-1).
    const double buy_commission = 100 * 54700.0 * 0.00015;
    assert(std::abs(gate.daily_pnl() - (-buy_commission)) < 0.01); // 분할 누적 부동소수 오차 허용
    PASS("unmapped_fill_duplicate_ignored");
}

// ─── 테스트 8: cross-day 중복방지 키 (V-4 fix) ────────────────────────────────────
//   ODNO는 영업일 단위 재사용 + fill_time은 HHMMSS(날짜 없음). 다른 거래일의 동일
//   (kis_order_no,fill_time,quantity,price) 통보가 전일 체결로 오인돼 drop되면 실체결 누락 사고.
//   거래일 prefix로 차단 — 둘 다 정상 반영되어야 한다.
void test_cross_day_fill_not_deduped()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000077");
    OrderRouter       router(gate, stub);
    (void)router.submit(make_signal("005930", OrderSide::BUY, 10));

    auto mk_ts = [](int y_value, int managed_order, int days) {
        std::tm time_parts{}; time_parts.tm_year = y_value - 1900; time_parts.tm_mon = managed_order - 1; time_parts.tm_mday = days;
        time_parts.tm_hour = 10; time_parts.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&time_parts));
    };

    FillNotification fill_notification;
    fill_notification.kis_order_no = "K000077"; fill_notification.ticker = "005930"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 5; fill_notification.filled_price = 75000.0; fill_notification.fill_time = "100000";

    fill_notification.timestamp = mk_ts(2024, 1, 10);   // 거래일 1
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 5);

    // 동일 (kis_order_no,fill_time,quantity,price) + 다른 거래일 → 별개 체결로 처리(중복 아님)
    fill_notification.timestamp = mk_ts(2024, 1, 11);   // 거래일 2
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 10);   // 누락 없이 누적
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
    auto managed_order = router.submit(buy);
    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(managed_order.krx_forwarding_org_no == "ORG000001");       // submit_order_ack가 조직번호 캡처
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

// ─── 테스트 10: 존재하지 않는 oid 취소 → CANCELLED(취소할 것 없음), KIS 미호출 ─────
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

    assert(cm.status == OrderStatus::CANCELLED); // 거부가 아니라 끝난 상태 — 거부 통계에 안 들어간다
    assert(router.stats().rejected == 0);
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
    (void)router.submit(buy);
    assert(gate.reserved("005930") == 10);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "K000333"; fill_notification.ticker = "005930"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 4; fill_notification.filled_price = 75000.0; fill_notification.fill_time = "100000";
    router.on_fill(fill_notification);
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
    (void)router.submit(buy);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "K000444"; fill_notification.ticker = "005930"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 10; fill_notification.filled_price = 75000.0; fill_notification.fill_time = "100000";
    router.on_fill(fill_notification);
    assert(gate.reserved("005930") == 0);
    assert(gate.position("005930") == 10);

    OrderSignal cancel;
    cancel.ticker          = "005930";
    cancel.action          = OrderAction::CANCEL;
    cancel.orig_client_oid = "MM:B:1";
    auto cm = router.submit(cancel);

    assert(cm.status == OrderStatus::CANCELLED); // 이미 FILLED → 취소 대상 없음(거부가 아니라 끝난 상태)
    assert(cm.reject_reason.find("이미 체결") != std::string::npos);
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
    (void)router.submit(buy);
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
        OrderGate         gate(relaxed_cfg());
        StubOrderExecutor stub(true, "R000777");
        OrderRouter       router(gate, stub);
        auto managed_order = router.submit(buy);
        assert(managed_order.status == OrderStatus::ACCEPTED);
        assert(gate.reserved("047050") == 100);
    }

    // 2차 세션 — 메모리 이력이 빈 상태에서 같은 ODNO의 체결이 들어온다.
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "R000888");
    OrderRouter       router(gate, stub);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "R000777"; fill_notification.ticker = "047050"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 60; fill_notification.filled_price = 54700.0; fill_notification.fill_time = "110707";
    router.on_fill(fill_notification);

    assert(gate.position("047050") == 60);
    assert(gate.reserved("047050") == 40);   // 주문수량 100을 되살리고 60만 해제

    // 전략 귀속이 ORPHAN이 아니라 원래 전략으로 남는다.
    auto hist = router.recent(5);
    bool found = false;

    for (const auto& history_entry : hist)
    {
        if (history_entry.kis_order_no == "R000777")
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
    assert(gate.position("047050") == 100);

    PASS("reason_journal_restart_recovery");
}


// ─── C-2: 잔고 대조 행 — RECONCILE 행이 원장·브로커 수량과 살아있는 주문 수를 남긴다 ──────
void test_reconcile_row_written()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000201");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 10)); // ACCEPTED, 미체결 → live_orders=1

    OrderRouter::ReconcileNote reconcile_note;
    reconcile_note.ticker     = "005930";
    reconcile_note.ledger_qty = 10;
    reconcile_note.broker_qty = 7;
    reconcile_note.ledger_avg = 75000.0;
    reconcile_note.broker_avg = 74900.0;
    reconcile_note.action     = "OVERWRITE";
    reconcile_note.note       = "mode=REST";
    router.record_reconcile(reconcile_note);

    auto rows = tail_trade_rows(1);
    assert(rows.size() == 1);
    auto other_split_csv = split_csv(rows[0]);
    assert(other_split_csv.size() == 17);              // 헤더 열 수와 같다(sequence까지)
    assert(other_split_csv[1] == "RECONCILE");
    assert(other_split_csv[5] == "005930");
    assert(other_split_csv[8] == "10" && other_split_csv[10] == "7"); // order_qty=원장, fill_qty=브로커
    assert(other_split_csv[12] == "OVERWRITE");
    assert(other_split_csv[13] == "live_orders=1 diff_qty=-3 mode=REST");
    assert(other_split_csv[16].empty());               // sequence 빈 칸
    PASS("reconcile_row_written");
}

// ─── C-2: sequence 전파 — 전략이 stamp한 순번이 접수 행과 체결 행에 그대로 남는다 ────────────
void test_seq_propagates_to_rows()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000202");
    OrderRouter       router(gate, stub);

    OrderSignal signal = make_signal("005930", OrderSide::BUY, 3);
    signal.sequence         = 77;
    auto managed_order         = router.submit(signal);
    assert(managed_order.status == OrderStatus::ACCEPTED);
    assert(managed_order.signal.sequence == 77);

    FillNotification fill_notification;
    fill_notification.kis_order_no         = "K000202";
    fill_notification.ticker       = "005930";
    fill_notification.side         = OrderSide::BUY;
    fill_notification.filled_quantity   = 3;
    fill_notification.filled_price = 75000.0;
    fill_notification.fill_time    = "100100";
    router.on_fill(fill_notification);

    auto rows = tail_trade_rows(2);
    assert(rows.size() == 2);
    auto acc  = split_csv(rows[0]);
    auto fill = split_csv(rows[1]);
    assert(acc[1] == "ACCEPTED" && acc[16] == "77");
    assert(fill[1] == "FILL" && fill[16] == "77");

    // 미부여(0)는 빈 칸으로 남는다 — 0이 진짜 순번으로 읽히지 않게.
    (void)router.submit(make_signal("005930", OrderSide::BUY, 1));
    auto last = split_csv(tail_trade_rows(1)[0]);
    assert(last[1] == "ACCEPTED" && last[16].empty());
    PASS("seq_propagates_to_rows");
}

// ─── C-2: 청산차단 해소 — 이번 세션 예약매도를 취소하면 CANCELLED로 닫고 선점을 푼다 ───────
//   모의투자 경로(is_paper): 미체결을 KIS가 아니라 history_에서 찾는다. 취소 뒤 재매도가
//   접수되면 그 선점만 남아야 한다(취소분 8 + 재매도 8 = 16이 아니라 8).
void test_blocked_sell_releases_reservation()
{
    OrderGate         gate(relaxed_cfg());
    StubOrderExecutor stub(true, "K000301");
    OrderRouter       router(gate, stub);
    stub.paper = true;

    // 원장에 포지션을 심지 않는다 — 심으면 게이트 SELL 클램프가 미체결매도를 빼고 0주로 깎아
    //  KIS까지 가지 않는다. 이 경로는 원장이 종목을 모르는(재기동 직후·WS 모드) 상황이 대상이다.

    OrderSignal resv = make_signal("005930", OrderSide::SELL, 8);
    resv.type        = OrderType::LIMIT;
    resv.price       = 80000.0;
    resv.client_oid  = "RESV:1";
    auto r1          = router.submit(resv);
    assert(r1.status == OrderStatus::ACCEPTED);
    assert(gate.reserved("005930") == -8);       // 매도 선점(부호는 게이트 규약)

    // 시장가 청산이 40240000으로 막힘 → 예약매도 취소 → 재매도 접수
    stub.kis_order_no      = "K000302";
    stub.fail_next = 1;
    stub.err_code  = "40240000";
    OrderSignal liq = make_signal("005930", OrderSide::SELL, 8);
    liq.type        = OrderType::MARKET;
    liq.price       = 0.0;
    liq.ref_price   = 75000.0;
    auto r2         = router.submit(liq);
    assert(r2.status == OrderStatus::ACCEPTED);
    assert(r2.kis_order_no == "K000302");
    assert(stub.cancel_calls == 1 && stub.last_cancel_qty == 8);

    // 원주문은 CANCELLED, 선점은 재매도분만
    bool orig_cancelled = false;

    for (const auto& history_entry : router.recent(10))
    {
        if (history_entry.kis_order_no == "K000301")
        {
            orig_cancelled = (history_entry.status == OrderStatus::CANCELLED);
        }
    }

    assert(orig_cancelled);
    assert(gate.reserved("005930") == -8);

    // 원장에 CANCELLED 행이 남는다(재매도 ACCEPTED 행 앞).
    auto rows = tail_trade_rows(2);
    assert(split_csv(rows[0])[1] == "CANCELLED" && split_csv(rows[0])[3] == "K000301");
    assert(split_csv(rows[1])[1] == "ACCEPTED" && split_csv(rows[1])[3] == "K000302");
    PASS("blocked_sell_releases_reservation");
}

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== OrderRouter Unit Tests ===\n";

    // 산출물을 라이브 원장과 갈라 둔다. 이 바이너리는 quant_trader.exe와 같은 build_win/에
    //  놓이고 Logger 기준 폴더가 실행파일 옆 logs/라, 그대로 두면 같은 trades_YYYYMMDD.csv에
    //  쓴다. 2026-09-09 원장 1364행 중 256행이 이렇게 섞였다(TEST 208 + 047050 48).
    //  QUANT_LOG_DIR이 이미 있으면 그쪽을 존중한다 — 산출물을 모아 보는 쪽 뜻이 우선이다.
    if (const char* env = std::getenv("QUANT_LOG_DIR"); !env || !*env)
    {
        Logger::instance().set_base_dir(Logger::executable_dir() / "logs_test");
    }

    // 사유 기록 파일은 append 전용이라 지난 실행분이 남으면 결과가 달라진다. 먼저 지운다.
    {
        std::time_t tt = std::time(nullptr);
        std::tm     lt{};
#ifdef _WIN32
        localtime_s(&lt, &tt);
#else
        localtime_r(&tt, &lt);
#endif
        char buffer[9];
        std::strftime(buffer, sizeof(buffer), "%Y%m%d", &lt);
        std::error_code error_code;
        std::filesystem::remove(
            Logger::instance().path_for(std::string("order_reasons_") + buffer + ".txt"), error_code);
    }

    test_gate_rejected();
    test_kis_accepted();
    test_kis_failed();
    test_stats_mixed();
    test_history_recent();
    test_order_id_sequence();
    test_duplicate_fill_ignored();
    test_unmapped_fill_applied();
    test_unmapped_fill_duplicate_ignored();
    test_cross_day_fill_not_deduped();
    test_cancel_releases_reserved();
    test_cancel_unknown_oid();
    test_partial_fill_then_cancel();
    test_cancel_after_full_fill_selfheal();
    test_replace_reserves_new_qty();
    test_reason_journal_restart_recovery();
    test_reconcile_row_written();
    test_seq_propagates_to_rows();
    test_blocked_sell_releases_reservation();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
