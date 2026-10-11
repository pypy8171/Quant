// test_order_gate — 매매 시간 창·시가 단일가 창·진입 정지 원천.
//  공용 준비는 test_order_gate_support.h, 실행 순서는 test_order_gate.cpp의 main.
#include "test_order_gate_support.h"

#include <ctime>
#include <iostream>
#include <string>

void test_session_window()
{
    // 지금 KST 분 — 창을 "지금을 품는 구간"과 "지금을 피하는 구간"으로 만들어 시계에 기대지 않는다.
    const auto time_of_day_now = kst::time_of_day(std::time(nullptr));
    const int  now_min = static_cast<int>(time_of_day_now.hours().count() * 60 + time_of_day_now.minutes().count());
    std::string reason;

    {
        OrderGate::Config config;
        config.session_open_min  = (std::max)(0, now_min - 30);       // <windows.h>의 max/min 매크로 회피
        config.session_close_min = (std::min)(24 * 60, now_min + 30);
        OrderGate gate(config);
        auto      signal = make_signal("005930", OrderSide::BUY);
        assert(gate.check(signal, reason));
    }

    {
        OrderGate::Config config;
        // 지금이 정오 전이면 오후 창, 아니면 오전 창 — 어느 쪽이든 지금은 밖.
        config.session_open_min  = now_min < 12 * 60 ? 13 * 60 : 1 * 60;
        config.session_close_min = now_min < 12 * 60 ? 14 * 60 : 2 * 60;
        OrderGate gate(config);
        auto      signal = make_signal("005930", OrderSide::BUY);
        assert(!gate.check(signal, reason));
        assert(reason.find("세션 창 밖") != std::string::npos);

        auto cancel   = make_signal("005930", OrderSide::SELL);
        cancel.action = OrderAction::CANCEL;
        cancel.original_client_order_id = "TEST-1";
        reason.clear();
        gate.check(cancel, reason);
        assert(reason.find("세션 창 밖") == std::string::npos);
    }

    {
        // 정규장 창은 지금을 비켜 가고 애프터마켓 창이 지금을 덮으면 통과 — 두 창의 합집합이다. [why D-097]
        OrderGate::Config config;
        config.session_open_min  = now_min < 12 * 60 ? 13 * 60 : 1 * 60;
        config.session_close_min = now_min < 12 * 60 ? 14 * 60 : 2 * 60;
        config.after_open_min    = (std::max)(0, now_min - 30);
        config.after_close_min   = (std::min)(24 * 60, now_min + 30);
        OrderGate gate(config);
        auto      signal = make_signal("005930", OrderSide::BUY);
        reason.clear();
        assert(gate.check(signal, reason));

        // 애프터마켓 창도 지금을 비켜 가면 거부 사유에 두 창이 다 적힌다.
        config.after_open_min  = now_min < 12 * 60 ? 15 * 60 : 3 * 60;
        config.after_close_min = now_min < 12 * 60 ? 16 * 60 : 4 * 60;
        OrderGate gate_closed(config);
        reason.clear();
        assert(!gate_closed.check(signal, reason));
        assert(reason.find("세션 창 밖") != std::string::npos);
        assert(reason.find(" 및 ") != std::string::npos);
    }

    {
        OrderGate gate; // 기본 0/0 = 검사 없음(테스트·리플레이)
        auto      signal = make_signal("005930", OrderSide::BUY);
        assert(gate.check(signal, reason));
    }

    PASS("session_window");
}

// ─── 장 시작 동시호가 창(D-157) ──────────────────────────────────────────
//  opening_auction 표시 주문은 정규장 시작 10분 전부터 받는다 — 신규 매수만. 25분 전(08:35 같은 자리)과 매도는 거절한다.
void test_opening_auction_window()
{
    const auto time_of_day_now = kst::time_of_day(std::time(nullptr));
    const int  now_min = static_cast<int>(time_of_day_now.hours().count() * 60 + time_of_day_now.minutes().count());
    std::string reason;

    if (now_min + 25 >= 24 * 60)
    {
        PASS("opening_auction_window (자정 직전이라 건너뜀)");
        return;
    }

    OrderGate::Config config;
    config.session_open_min  = now_min + 5; // 지금은 시작 5분 전 — 08:55 자리
    config.session_close_min = now_min + 60;
    OrderGate gate(config);

    auto buy            = make_signal("005930", OrderSide::BUY);
    buy.opening_auction = true;
    assert(gate.check(buy, reason));

    auto plain = make_signal("005930", OrderSide::BUY);
    reason.clear();
    assert(!gate.check(plain, reason)); // 표시 없는 주문은 그대로 막힌다

    auto sell            = make_signal("005930", OrderSide::SELL);
    sell.opening_auction = true;
    reason.clear();
    assert(!gate.check(sell, reason)); // 매도는 동시호가 창을 받지 않는다
    assert(reason.find("세션 창 밖") != std::string::npos);

    OrderGate::Config early_config;
    early_config.session_open_min  = now_min + 25; // 지금은 시작 25분 전 — 08:35 자리
    early_config.session_close_min = now_min + 60;
    OrderGate early(early_config);
    reason.clear();
    assert(!early.check(buy, reason));

    PASS("opening_auction_window");
}

// ─── 테스트: 매매 세션 창(D-096) — 창 밖 NEW는 거부, 창 안·창 없음(0/0)·CANCEL은 통과 ──────────
// 진입 정지는 원천이 셋이다 — 국면(RegimeFileJudge가 자동으로 켜고 끈다)·사람(운영단말)·전략 사망(주문 스레드).
//  한 원천의 해제가 다른 원천이 켠 정지를 지우면 급락장에 신규 매수가 되살아난다. OR로만 합치는지 본다. [why D-114]
void test_halt_sources_are_independent()
{
    OrderGate::Config config;
    OrderGate         gate(config);

    assert(!gate.is_entry_halted());

    // 전략이 죽어 주문 스레드가 켠다.
    gate.set_strategy_down_halt(true);
    assert(gate.is_entry_halted());

    // 국면이 "위험 없음"으로 자동 해제해도 전략 사망 정지는 남는다.
    gate.set_entry_halt(false);
    assert(gate.is_entry_halted());

    // 사람이 수동 정지를 풀어도 남는다.
    gate.set_manual_halt(OrderSide::BUY, false);
    assert(gate.is_entry_halted());

    // 박동이 돌아와 주문 스레드가 끄면 비로소 풀린다.
    gate.set_strategy_down_halt(false);
    assert(!gate.is_entry_halted());

    // 거꾸로도 같다 — 전략 사망 해제가 국면·사람이 켠 정지를 못 지운다.
    gate.set_entry_halt(true);
    gate.set_manual_halt(OrderSide::BUY, true);
    gate.set_strategy_down_halt(true);
    gate.set_strategy_down_halt(false);
    assert(gate.is_entry_halted());
    assert(gate.is_manual_buy_halted());

    gate.set_entry_halt(false);
    assert(gate.is_entry_halted()); // 사람이 켠 것이 남아 있다
    gate.set_manual_halt(OrderSide::BUY, false);
    assert(!gate.is_entry_halted());

    std::cout << "[PASS] 진입 정지 원천 셋이 서로를 안 지운다\n";
}

void run_session_tests()
{
    test_session_window();
    test_opening_auction_window();
    test_halt_sources_are_independent();
}
