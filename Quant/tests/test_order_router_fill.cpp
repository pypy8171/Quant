// test_order_router — 체결통보 반영 — 같은 키 통보, 재연결 뒤 재전송, 미연결 체결, 원주문번호 연결, 날짜 넘김.
//  공용 준비는 test_order_router_support.h, 실행 순서는 test_order_router.cpp의 main.
#include "test_order_router_support.h"

#include <chrono>
#include <ctime>

// ─── 테스트 7: 과체결 방어는 주문잔량 상한이 담당 ────────────────────────
//   (kis_order_no,체결시각,수량,단가)는 유일하지 않다 — 같은 초에 같은 수량·단가로 나뉘어
//   체결되면 서로 다른 실체결이 같은 키를 갖는다. 그래서 같은 키의 통보도 각각 반영하고,
//   대신 누적 체결이 주문수량을 넘지 못하게 클램프해 과체결을 막는다.
void test_duplicate_fill_ignored()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000077");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 10)); // ACCEPTED, ODNO=K000077

    // 부분체결 5주 통보
    FillNotification fill_notification;
    fill_notification.kis_order_no         = "0000000077";
    fill_notification.ticker       = "005930";
    fill_notification.side         = OrderSide::BUY;
    fill_notification.filled_quantity   = 5;
    fill_notification.filled_price = 75000.0;
    fill_notification.fill_time    = "100000";
    router.on_fill(fill_notification);

    auto recent_a = router.recent(1);
    assert(recent_a[0].confirmed_quantity == 5);
    assert(recent_a[0].status == OrderStatus::ACCEPTED);

    // 같은 키의 두 번째 통보 = 같은 초의 또 다른 5주 분할체결 → 반영되어 전량 체결
    router.on_fill(fill_notification);
    auto recent_b = router.recent(1);
    assert(recent_b[0].confirmed_quantity == 10);
    assert(recent_b[0].status == OrderStatus::FILLED);

    // 주문수량(10주)을 이미 채웠으므로 그 이상은 반영되지 않는다.
    //  ODNO는 아는 주문이므로 미매핑(미연결) 경로로 새어 포지션이 부풀어도 안 된다.
    fill_notification.fill_time = "100005";
    router.on_fill(fill_notification);
    auto h3 = router.recent(1);
    assert(h3[0].confirmed_quantity == 10);
    assert(h3[0].status == OrderStatus::FILLED);
    assert(gate.ledger().position("005930") == 10);  // 15주로 부풀지 않음
    PASS("duplicate_fill_ignored");
}

// ─── 테스트 7a: 재연결 뒤 재전송은 실체결로 쌓지 않는다 (CODE_REVIEW C-1) ─────
//   같은 세션 안의 같은 키는 분할체결(테스트 7), 새 세션에서 앞 세션까지 받은 횟수 이하로 다시 온
//   같은 키는 재전송이다. 잔량 상한만으로는 100주 주문의 10주 재전송이 20주로 기록되고 뒤의 실체결
//   10주가 "충족 후 통보"로 버려져 체결가 귀속이 틀어진다.
void test_replayed_fill_after_reconnect_ignored()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000078");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 100));

    FillNotification fill_notification;
    fill_notification.kis_order_no       = "0000000078";
    fill_notification.ticker             = "005930";
    fill_notification.side               = OrderSide::BUY;
    fill_notification.filled_quantity    = 10;
    fill_notification.filled_price       = 75000.0;
    fill_notification.fill_time          = "100000";
    fill_notification.session_generation = 1;

    // (a) 세션 1에서 같은 초·같은 수량·단가 실체결 두 건 → 둘 다 반영
    router.on_fill(fill_notification);
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 20);

    // (b) 재연결(세션 2) 뒤 같은 통보 두 건이 다시 온다 → 반영 안 함, 센다
    fill_notification.session_generation = 2;
    router.on_fill(fill_notification);
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 20);
    assert(router.replayed_fills() == 2);
    assert(gate.ledger().position("005930") == 20);

    // 세션 2에서 앞 세션 횟수를 넘은 같은 키는 새 실체결이다
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 30);

    // 키가 다른 체결은 세션과 무관하게 반영된다
    fill_notification.fill_time = "100007";
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 40);
    assert(router.replayed_fills() == 2);
    assert(gate.ledger().position("005930") == 40);
    PASS("replayed_fill_after_reconnect_ignored");
}

// ─── 테스트 7b: 미매핑 체결도 원장·포지션에 반영 ────────────────────────
//   장중 재시작하면 이전 세션의 미체결 주문이 history_에서 사라진다. 거래소 호가창에는
//   그대로 살아있으므로 나중에 체결통보가 들어오는데, 예전에는 통째로 버려져 원장이
//   어긋났다(2026-09-07 047050 91주). 이제는 포지션에 반영하고, 선점이 없던 상태라
//   reserved_가 음수로 내려가지 않아야 한다.
void test_unmapped_fill_applied()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000555");
    OrderRouter       router(gate, stub);

    // 이 라우터가 낸 적 없는 ODNO의 체결통보
    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000009001"; fill_notification.ticker = "047050"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 91; fill_notification.filled_price = 54700.0; fill_notification.fill_time = "110707";
    router.on_fill(fill_notification);

    assert(gate.ledger().position("047050") == 91);  // 원장에 반영
    assert(gate.ledger().reserved("047050") == 0);   // 없던 선점을 깎아 음수로 만들지 않음
    PASS("unmapped_fill_applied");
}

// ─── 테스트 7c: 미매핑 체결의 재전송은 한 번만 반영 (W-6 회귀) ──────────────
//   미연결은 history_에 없어 exhausted 판정이 못 잡고, 주문수량도 몰라 잔량 클램프도 없다.
//   같은 키(거래일:kis_order_no:시각:수량:단가)의 2회차는 무시하고, 키가 다른 후속 분할체결은 반영한다.
void test_unmapped_fill_duplicate_ignored()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000556");
    OrderRouter       router(gate, stub);

    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000009001"; fill_notification.ticker = "047050"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 91; fill_notification.filled_price = 54700.0; fill_notification.fill_time = "110707";
    router.on_fill(fill_notification);
    router.on_fill(fill_notification);                    // WS 재구독 재전송
    assert(gate.ledger().position("047050") == 91); // 182로 부풀지 않음
    assert(gate.ledger().reserved("047050") == 0);

    fill_notification.fill_time = "110709"; fill_notification.filled_quantity = 9; // 같은 주문의 다음 분할체결(키 다름)
    router.on_fill(fill_notification);
    assert(gate.ledger().position("047050") == 100);

    // 평단 미상 미연결 SELL — 실현이익을 만들지 않는다
    FillNotification unlinked_sell_fill;
    unlinked_sell_fill.kis_order_no = "0000009002"; unlinked_sell_fill.ticker = "316140"; unlinked_sell_fill.side = OrderSide::SELL;
    unlinked_sell_fill.filled_quantity = 75; unlinked_sell_fill.filled_price = 34050.0; unlinked_sell_fill.fill_time = "093000";
    router.on_fill(unlinked_sell_fill);

    // 위 BUY 100주(91+9, 평단 54700)의 매수 수수료가 발생 즉시 차감돼 있다.
    //  SELL(316140)은 평단 미상이라 0을 더할 뿐 — 실현이익은 안 생긴다.
    //  수수료는 체결 한 건마다 원 단위로 반올림한다 — 91주 746.655 → 747원, 9주 73.845 → 74원, 합 821원(옛 double 셈 820.5원).
    const money::Won buy_commission = money::apply_rate(91 * 54700, 15, 100000) + money::apply_rate(9 * 54700, 15, 100000);
    assert(gate.ledger().daily_pnl() == -buy_commission);
    PASS("unmapped_fill_duplicate_ignored");
}

// ─── 테스트 7d: 주문수량(ODER_QTY)을 받은 미매핑 체결은 잔량으로 묶는다 ─────
//   전문 [16]이 그 주문의 총수량이라, 오면 상한이 "키가 겹치지 않을 것"이 아니라 "주문수량"이 된다.
//   연결된 주문(테스트 7)과 같은 규칙이 되므로 같은 초·같은 수량·단가로 갈라진 분할체결도 잃지 않는다.
void test_unmapped_fill_clamped_by_order_quantity()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000557");
    OrderRouter       router(gate, stub);

    // 6주 주문이 같은 초에 2+2+2로 갈라졌다 — 세 통보의 키가 모두 같다.
    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000014893"; fill_notification.ticker = "047050"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 2; fill_notification.filled_price = 54700.0; fill_notification.fill_time = "110707";
    fill_notification.order_quantity = 6;
    router.on_fill(fill_notification);
    router.on_fill(fill_notification);
    router.on_fill(fill_notification);
    assert(gate.ledger().position("047050") == 6);   // 종전 키 중복 제거로는 2주만 남았다
    assert(gate.ledger().reserved("047050") == 0);   // 없던 선점을 깎아 음수로 만들지 않음

    router.on_fill(fill_notification);      // 주문수량을 채운 뒤의 재전송
    assert(gate.ledger().position("047050") == 6);   // 8로 부풀지 않는다

    // 통보 수량이 잔량을 넘으면 잔량까지만 반영한다(연결된 주문의 클램프와 같다).
    FillNotification partial;
    partial.kis_order_no = "0000014894"; partial.ticker = "005930"; partial.side = OrderSide::BUY;
    partial.filled_price = 71500.0; partial.order_quantity = 6; partial.filled_quantity = 4;
    partial.fill_time = "093512";
    router.on_fill(partial);
    partial.fill_time = "093513";
    router.on_fill(partial);
    assert(gate.ledger().position("005930") == 6);   // 8이 아니라 주문수량에서 멈춘다

    // 주문수량이 통보 수량보다 작은 전문은 믿지 않는다 — 체결을 깎는 대신 종전 키 중복 제거로 떨어진다.
    FillNotification inconsistent;
    inconsistent.kis_order_no = "0000014895"; inconsistent.ticker = "000660"; inconsistent.side = OrderSide::BUY;
    inconsistent.filled_quantity = 5; inconsistent.filled_price = 20000.0; inconsistent.fill_time = "100000";
    inconsistent.order_quantity = 3;
    router.on_fill(inconsistent);
    assert(gate.ledger().position("000660") == 5);   // 3주로 깎이지 않는다
    router.on_fill(inconsistent);
    assert(gate.ledger().position("000660") == 5);   // 재전송은 종전대로 키가 막는다
    PASS("unmapped_fill_clamped_by_order_quantity");
}

// ─── 테스트 7e: 정정 응답을 못 받아도 원주문번호로 체결을 연결한다 ──────────
//   정정이 나가면 KIS가 새 ODNO를 준다. 그 응답을 못 받으면 이력에는 옛 ODNO가 남고, 체결통보는
//   새 ODNO로 오므로 전략 귀속을 잃은 채 미연결로 떨어진다. 전문 [3]OODER_NO가 옛 ODNO라 되찾는다.
void test_fill_linked_by_original_order_number()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000301");
    OrderRouter       router(gate, stub);

    (void)router.submit(make_signal("005930", OrderSide::BUY, 10)); // 이력의 ODNO = 0000000301

    // 정정 뒤 새 ODNO로 오는 체결통보. 원주문번호가 이력의 ODNO다.
    FillNotification fill_notification;
    fill_notification.kis_order_no      = "0000000302";
    fill_notification.original_order_no = "0000000301";
    fill_notification.ticker            = "005930";
    fill_notification.side              = OrderSide::BUY;
    fill_notification.filled_quantity   = 10;
    fill_notification.filled_price      = 75000.0;
    fill_notification.fill_time         = "100000";
    router.on_fill(fill_notification);

    auto recent = router.recent(1);
    assert(recent[0].confirmed_quantity == 10);
    assert(recent[0].status == OrderStatus::FILLED);
    assert(recent[0].signal.strategy_id != "UNLINKED");  // 전략 귀속을 잃지 않았다
    assert(gate.ledger().position("005930") == 10);
    PASS("fill_linked_by_original_order_number");
}

// ─── 테스트 8: cross-day 중복방지 키 (V-4 fix) ────────────────────────────────────
//   ODNO는 영업일 단위 재사용 + fill_time은 HHMMSS(날짜 없음). 다른 거래일의 동일
//   (kis_order_no,fill_time,quantity,price) 통보가 전일 체결로 오인돼 drop되면 실체결 누락 사고.
//   거래일 prefix로 차단 — 둘 다 정상 반영되어야 한다.
void test_cross_day_fill_not_deduped()
{
    OrderGate         gate(relaxed_config());
    StubOrderExecutor stub(true, "0000000077");
    OrderRouter       router(gate, stub);
    (void)router.submit(make_signal("005930", OrderSide::BUY, 10));

    auto make_timestamp = [](int y_value, int managed_order, int days) {
        std::tm time_parts{}; time_parts.tm_year = y_value - 1900; time_parts.tm_mon = managed_order - 1; time_parts.tm_mday = days;
        time_parts.tm_hour = 10; time_parts.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&time_parts));
    };

    FillNotification fill_notification;
    fill_notification.kis_order_no = "0000000077"; fill_notification.ticker = "005930"; fill_notification.side = OrderSide::BUY;
    fill_notification.filled_quantity = 5; fill_notification.filled_price = 75000.0; fill_notification.fill_time = "100000";

    fill_notification.timestamp = make_timestamp(2024, 1, 10);   // 거래일 1
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 5);

    // 동일 (kis_order_no,fill_time,quantity,price) + 다른 거래일 → 별개 체결로 처리(중복 아님)
    fill_notification.timestamp = make_timestamp(2024, 1, 11);   // 거래일 2
    router.on_fill(fill_notification);
    assert(router.recent(1)[0].confirmed_quantity == 10);   // 누락 없이 누적
    assert(router.recent(1)[0].status == OrderStatus::FILLED);
    PASS("cross_day_fill_not_deduped");
}

void run_fill_tests()
{
    test_duplicate_fill_ignored();
    test_replayed_fill_after_reconnect_ignored();
    test_unmapped_fill_applied();
    test_unmapped_fill_duplicate_ignored();
    test_unmapped_fill_clamped_by_order_quantity();
    test_fill_linked_by_original_order_number();
    test_cross_day_fill_not_deduped();
}
