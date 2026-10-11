// test_order_gate — 슬롯 제외·장부 사본·거부 코드·하루 리셋·총노출·매수매도 선점.
//  공용 준비는 test_order_gate_support.h, 실행 순서는 test_order_gate.cpp의 main.
#include "test_order_gate_support.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

// ─── 바스켓 슬롯 제외(D-109) ────────────────────────────────────────────
//  바스켓 슬리브가 든 종목은 장중 전략의 슬롯을 먹지 않는다. 상한 2에서 제외 종목 둘을 채워도 새 BUY가 통과하고,
//  open_slot_count·plan_displacement 후보·snapshot_positions 표시가 같은 집합을 본다.
void test_slot_exempt()
{
    OrderGate gate(displace_config()); // 상한 2, 교체 켜짐
    gate.ledger().set_slot_exempt(gate.ledger().strategy_index_of("BASKET_MAIN"), {"BK1", "BK2"});
    gate.ledger().seed_position("", "BK1", 10, 1000.0);
    gate.ledger().seed_position("", "BK2", 10, 1000.0);
    assert(!gate.slots_full());
    assert(gate.ledger().open_slot_count() == 0); // 열린 슬롯(차지한 자리) 수 — 제외 종목은 세지 않는다
    // 자리 세는 곳이 다섯이다. 하나라도 면제를 빼먹으면 전략과 게이트가 딴 판단을 한다 —
    //  entry_snapshot이 빼먹던 때는 교체를 켠 날 멀쩡한 보유를 팔아 이미 있던 자리를 만들었다.
    assert(!gate.entry_snapshot("", "Z").slots_full);
    assert(gate.entry_snapshot("", "Z").slots_full == gate.slots_full());

    std::string reason;
    auto        buy_a = make_signal("A", OrderSide::BUY, 10);
    assert(gate.check(buy_a, reason)); // 제외 종목 둘은 상한 계산에 없다
    gate.ledger().on_fill_confirmed("", "A", OrderSide::BUY, 10, 1000.0);
    auto buy_b = make_signal("B", OrderSide::BUY, 10);
    assert(gate.check(buy_b, reason));
    gate.ledger().on_fill_confirmed("", "B", OrderSide::BUY, 10, 1000.0);
    assert(gate.slots_full());
    auto buy_c = make_signal("C", OrderSide::BUY, 10);
    assert(!gate.check(buy_c, reason)); // 장중 종목 둘로 꽉 참

    // 제외 종목은 상한이 꽉 차도 추가 매수가 된다(바스켓 채우기).
    auto buy_bk1 = make_signal("BK1", OrderSide::BUY, 5);
    assert(gate.check(buy_bk1, reason));

    // 교체 후보에도 안 든다 — 점수가 가장 낮은 BK1 대신 B가 뽑힌다.
    gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 0.9}, {gate.ledger().intern_symbol("B"), 2, -0.8}, {gate.ledger().intern_symbol("C"), 3, 1.5}, {gate.ledger().intern_symbol("BK1"), 4, -5.0}}, 4);
    auto plan = gate.plan_displacement("", gate.ledger().intern_symbol("C"));
    assert(plan.ok && plan.ticker == "B");

    // snapshot 표시와 티커 목록.
    size_t exempt_count = 0;

    for (const auto& held : gate.ledger().snapshot_positions())
    {
        exempt_count += held.slot_exempt ? 1 : 0;
    }

    assert(exempt_count == 2);
    const auto exempt_symbols = gate.ledger().slot_exempt_symbols(); // 오름차순 id
    assert(exempt_symbols.size() == 2 && exempt_symbols[0] == gate.ledger().symbol_id_of("BK1") && exempt_symbols[1] == gate.ledger().symbol_id_of("BK2"));

    // 집합을 갈아 끼우면 이전 것은 풀린다.
    gate.ledger().set_slot_exempt(gate.ledger().strategy_index_of("BASKET_MAIN"), {"BK1"});
    assert(gate.ledger().slot_exempt_symbols().size() == 1 && gate.ledger().slot_exempt_symbols()[0] == gate.ledger().symbol_id_of("BK1"));
    PASS("slot_exempt");
}

// ─── 슬롯 제외 소유자별 합집합(D-157) ──────────────────────────────────────
//  바스켓과 SURGE가 각자 목록을 올린다. 한쪽이 갱신해도 다른 쪽 종목이 풀리면 안 된다 — 덮어쓰던 때는
//  나중에 올린 쪽만 남아 먼저 올린 쪽 보유가 슬롯을 먹고 강제청산 대상이 됐다.
void test_slot_exempt_owner_union()
{
    OrderGate  gate(displace_config());
    const auto basket = gate.ledger().strategy_index_of("BASKET_MAIN");
    const auto surge  = gate.ledger().strategy_index_of("SURGE_MAIN");
    gate.ledger().set_slot_exempt(basket, {"BK1", "BK2", "BK3"});
    gate.ledger().set_slot_exempt(surge, {"SG1", "SG2"});
    assert(gate.ledger().slot_exempt_symbols().size() == 5);

    // SURGE가 목록을 바꿔도 바스켓 셋은 남는다.
    gate.ledger().set_slot_exempt(surge, {"SG2"});
    assert(gate.ledger().slot_exempt_symbols().size() == 4);
    assert(gate.ledger().is_slot_exempt(gate.ledger().symbol_id_of("BK1")));
    assert(!gate.ledger().is_slot_exempt(gate.ledger().symbol_id_of("SG1")));

    // 두 소유자가 같은 종목을 올렸다면 한쪽이 빼도 다른 쪽 몫으로 남는다.
    gate.ledger().set_slot_exempt(surge, {"SG2", "BK1"});
    gate.ledger().set_slot_exempt(basket, {"BK2", "BK3"});
    assert(gate.ledger().is_slot_exempt(gate.ledger().symbol_id_of("BK1")));

    // 빈 목록은 그 소유자 몫만 비운다.
    gate.ledger().set_slot_exempt(surge, {});
    assert(gate.ledger().slot_exempt_symbols().size() == 2);
    assert(!gate.ledger().is_slot_exempt(gate.ledger().symbol_id_of("BK1")));
    PASS("slot_exempt_owner_union");
}

void test_publish_ledger_matches_gate()
{
    auto config = displace_config();
    config.max_concurrent_positions = 3;
    OrderGate gate(config);

    gate.ledger().set_slot_exempt(gate.ledger().strategy_index_of("BASKET_MAIN"), {"BK1"});
    gate.ledger().seed_position("", "A", 10, 1000.0);
    gate.ledger().seed_position("", "B", 5, 2000.0);
    gate.ledger().seed_position("", "BK1", 7, 500.0);   // 슬롯 면제 — 자리를 안 먹는다
    gate.ledger().refresh_sellable("", "A", 4);          // 매도가능이 보유보다 적은 경우
    gate.ledger().on_accept("", "A", OrderSide::SELL, 2, 1000.0);  // 미체결 매도 — 선점은 음수
    gate.ledger().on_accept("", "C", OrderSide::BUY, 3, 700.0);    // 보유 없이 매수 선점만

    ipc::LedgerSnapshot snapshot;
    gate.publish_ledger(snapshot);

    assert(snapshot.generation() == 1);
    assert(gate.ledger().ledger_foreign_account_rows() == 0);

    // ① 보유 전체가 그대로 실렸는가 — 원본 snapshot_positions()와 한 줄씩 맞춰 본다.
    for (const auto& held : gate.ledger().snapshot_positions())
    {
        const ipc::LedgerRow row = snapshot.row(held.symbol);
        assert(row.position == held.quantity);
        assert(row.average_price == held.average_price);
        assert((row.slot_exempt != 0) == held.slot_exempt);
    }

    // ② 매도가능 — 원본 sellable_view()의 셈(상한 - 미체결 매도, 음수면 0)과 같아야 한다.
    for (const char* ticker : {"A", "B", "BK1"})
    {
        const auto view     = gate.ledger().sellable_view("", ticker);
        const int  expected = (view.possible_quantity_cap > view.pending) ? (view.possible_quantity_cap - view.pending) : 0;
        assert(snapshot.row(gate.ledger().symbol_id_of(ticker)).sellable == expected);
    }

    // ③ 진입 판단 셋 — 원본 entry_snapshot()과 보유·선점이 같아야 한다.
    for (const char* ticker : {"A", "B", "C", "Z"})
    {
        const auto original = gate.entry_snapshot("", ticker);
        const auto copy     = snapshot.entry(gate.ledger().symbol_id_of(ticker));
        assert(copy.position == original.position);
        assert(copy.reserved == original.reserved);
    }

    // ④ 미체결 선점의 부호가 살아 있는가 — 매도는 음수, 매수는 양수.
    assert(snapshot.row(gate.ledger().symbol_id_of("A")).reserved == -2);
    assert(snapshot.row(gate.ledger().symbol_id_of("C")).reserved == 3);
    assert(snapshot.row(gate.ledger().symbol_id_of("C")).position == 0); // 보유 없이 선점만

    // ⑤ 전역값 — 열린 자리 수와 여력은 원본과 같은 답이어야 한다.
    //  A·B·C 셋이 자리를 먹고 BK1은 면제라 3/3이다.
    const ipc::LedgerGlobals globals = snapshot.globals();
    assert(globals.open_slot_count == static_cast<int32_t>(gate.ledger().open_slot_count()));
    assert(globals.open_slot_count == 3);
    assert((globals.capacity_full != 0) == gate.capacity_full());
    assert(globals.max_concurrent_positions == config.max_concurrent_positions);
    assert(globals.displace_enabled == (config.displace_enabled ? 1 : 0));

    // ⑥ 진입 정지는 세 원천의 OR이다 [why D-091]
    gate.set_strategy_down_halt(true);
    gate.publish_ledger(snapshot);
    assert(snapshot.globals().entry_halted == 1);
    gate.set_strategy_down_halt(false);

    // ⑦ 판이 바뀌면 사라진 종목은 사본에서도 사라진다 — 낡은 보유가 남으면 이중 발주가 된다.
    gate.ledger().on_fill_confirmed("", "B", OrderSide::SELL, 5, 2000.0); // 전량 매도 체결 — 장부에서 빠진다
    gate.publish_ledger(snapshot);
    assert(snapshot.row(gate.ledger().symbol_id_of("B")).position == 0);
    assert(snapshot.row(gate.ledger().symbol_id_of("A")).position == 10); // 남은 종목은 그대로

    // ⑧ 다른 계좌 줄은 싣지 않고 센다 — 한 프로세스는 한 계좌만 다룬다.
    const uint64_t before = gate.ledger().ledger_foreign_account_rows();
    gate.ledger().seed_position("OTHER", "A", 99, 1234.0);
    gate.publish_ledger(snapshot);
    assert(gate.ledger().ledger_foreign_account_rows() > before);
    assert(snapshot.row(gate.ledger().symbol_id_of("A")).position == 10); // 남의 계좌 값이 덮지 않았다

    // ⑨ 이 판이 담은 계좌 이름 — 전략 쪽이 강제청산·한도 정리 주문에 적을 값이다. 단일 계좌의
    //  원장 키는 빈 이름이라 여기서는 빈 문자열이 나온다.
    assert(std::string(snapshot.globals().account).empty());

    {
        // 이름이 있는 계좌면 그 이름이 실린다.
        OrderGate named_gate(config);
        named_gate.ledger().seed_position("TEST-ACCOUNT", "A", 1, 100.0);
        named_gate.publish_ledger(snapshot);
        assert(std::string(snapshot.globals().account) == "TEST-ACCOUNT");

        // 32칸을 넘는 이름은 잘리되 끝은 0으로 끊긴다 — 끊기지 않으면 읽는 쪽이 옆칸까지 읽는다.
        OrderGate         long_name_gate(config);
        const std::string long_name(60, 'X');
        long_name_gate.ledger().seed_position(long_name, "A", 1, 100.0);
        long_name_gate.publish_ledger(snapshot);
        const std::string copied = snapshot.globals().account;
        assert(copied.size() == sizeof(ipc::LedgerGlobals::account) - 1);
        assert(copied == long_name.substr(0, copied.size()));
    }

    PASS("publish_ledger_matches_gate");
}

// ─── 테스트 23: 장부 사본이 원본과 같은 값을 싣는다 (D-114 단계 2.5) ──────────
//   전략 쪽이 OrderGate 대신 읽을 사본이다. 사본이 원본과 한 자리라도 다르면 전략이 딴 장부를 보고
//   매매하게 되므로, 사본의 모든 칸을 원본 접근자와 맞춰 본다.
// ─── 판정은 코드만, 문장은 describe가 ────────────────────
void test_verdict_codes_and_describe()
{
    OrderGate::Config config;
    config.deduplicate_window_sec = 2.0;
    config.max_orders_per_min     = 100;
    config.max_orders_per_sec     = 100;
    OrderGate gate(config);

    const auto signal = make_signal("005930", OrderSide::BUY);
    assert(gate.evaluate(signal).passed());

    const GateVerdict duplicate = gate.evaluate(signal);
    assert(duplicate.code == GateReject::Duplicate);
    assert(gate.describe(duplicate).find("중복 신호") != std::string::npos);

    gate.set_kill_switch(true);
    const GateVerdict killed = gate.evaluate(signal);
    assert(killed.code == GateReject::KillSwitch);
    assert(gate.describe(killed) == "KILL_SWITCH 활성");

    GateVerdict over_ticker;
    over_ticker.code   = GateReject::TickerQuantityLimit;
    over_ticker.amount = 7;
    over_ticker.base   = 95;
    assert(gate.describe(over_ticker) == std::format("포지션 한도 초과 (95+7 > {})", config.max_quantity_per_ticker));
    PASS("verdict_codes_and_describe");
}

// ─── 테스트 25: 하루 리셋은 거래일당 한 번 (전수조사 A-4) ──────────────────────
//  장 시작 감지는 "이 스레드가 본 닫힘→열림"이라 장중 재기동 직후 첫 회차에도 불린다. 저널의 RESET_DAY로
//  그날 리셋을 이미 했음을 알고 건너뛰어야, 리플레이가 되살린 선점과 당일 손익이 남는다.
void test_reset_daily_once_per_trading_date()
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "quant_gate_test_reset_day";
    std::error_code             error_code;
    std::filesystem::remove_all(directory, error_code);
    std::filesystem::create_directories(directory, error_code);
    const std::string  date = "20260925";
    constexpr uint32_t kTradingDate = 20260925;
    constexpr uint32_t kNextTradingDate = 20260928;

    {
        OrderGate gate(OrderGate::Config{});
        assert(gate.ledger().set_journal(directory, date, false));
        // 개장 전(NXT 프리마켓)에 낸 주문 — 개장 리셋이 비운다
        assert(gate.ledger().on_intent("", "000660", OrderSide::BUY, 3, 200000.0,
                                       PositionLedger::OrderRef{1, 0, OrderType::LIMIT}));
        assert(gate.reset_daily(kTradingDate));
        assert(gate.ledger().reserved("", "000660") == 0);

        assert(gate.ledger().on_intent("", "005930", OrderSide::BUY, 5, 70000.0,
                                       PositionLedger::OrderRef{2, 0, OrderType::LIMIT}));
        gate.ledger().set_daily_pnl(-3000.0);
        assert(!gate.reset_daily(kTradingDate)); // 같은 프로세스의 US 22:30 개장 — 건너뜀
        assert(gate.ledger().reserved("", "005930") == 5);
    }

    // 장중 재기동 — 새 프로세스가 저널을 리플레이한 뒤 첫 회차에 또 부른다
    OrderGate restarted(OrderGate::Config{});
    assert(restarted.ledger().set_journal(directory, date, false));
    assert(restarted.ledger().last_daily_reset_date() == kTradingDate);
    assert(restarted.ledger().reserved("", "000660") == 0); // 리플레이도 RESET_DAY에서 비운다
    assert(restarted.ledger().reserved("", "005930") == 5);
    assert(!restarted.reset_daily(kTradingDate));
    assert(restarted.ledger().reserved("", "005930") == 5);
    assert(restarted.ledger().daily_pnl() < -2999.0 && restarted.ledger().daily_pnl() > -3001.0);

    // 다음 거래일에는 한다
    assert(restarted.reset_daily(kNextTradingDate));
    assert(restarted.ledger().reserved("", "005930") == 0);
    assert(restarted.ledger().daily_pnl() == 0.0);

    std::filesystem::remove_all(directory, error_code);
    PASS("reset_daily_once_per_trading_date");
}

// ─── 테스트 26: 총노출은 현재가로 잰다 ───────────────────────────────────
//  자본 100만, 한도 100%. 005930 10주를 평단 5만에 들고 있고 현재가는 9만이다.
//  000660 2주(20만)를 더 사면 원가로는 70만이라 통과지만 시가로는 110만이라 막혀야 한다.
void test_gross_exposure_uses_mark_price()
{
    OrderGate::Config config;
    config.max_gross_exposure_percent = 1.0;
    config.max_orders_per_min         = 100;
    config.max_orders_per_sec         = 100;
    OrderGate gate(config);
    gate.ledger().seed_position("005930", 10, 50000.0);
    gate.ledger().set_equity(1'000'000.0);
    gate.ledger().replace_mark_prices("", {{"005930", 90000.0}, {"035420", 0.0}});

    const OrderSignal buy = make_signal("000660", OrderSide::BUY, 2);
    const GateVerdict blocked = gate.evaluate(buy);
    assert(blocked.code == GateReject::GrossExposure);
    assert(blocked.amount == 1'100'000);
    assert(blocked.ceiling == 1'000'000);
    assert(gate.clamp_buy_quantity(buy) == 1); // 남은 여유 10만 = 1주

    // 잔고에서 빠진 종목의 현재가는 지워진다 — 평단 5만으로 돌아가 70만이라 통과
    gate.ledger().replace_mark_prices("", {});
    assert(gate.evaluate(buy).passed());
    PASS("gross_exposure_uses_mark_price");
}

// ─── 테스트 27: 매수·매도 선점은 따로 센다 ─────────────────────
//  부호 있는 합 하나로 두던 때는 매수 10·매도 3이 +7로 뭉쳐, 매수 10 체결이 10을 풀면 0에서 멈추며
//  살아 있는 매도 3까지 지웠다. 그 뒤 매도 가능 수량을 3주 부풀려 봐 이미 판 주식을 다시 팔 수 있었다.
void test_buy_and_sell_reservations_kept_apart()
{
    OrderGate::Config config;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    OrderGate gate(config);
    gate.ledger().on_fill_confirmed("", "005930", OrderSide::BUY, 10, 50000.0);
    gate.ledger().on_accept("", "005930", OrderSide::BUY, 10, 50000.0);
    gate.ledger().on_accept("", "005930", OrderSide::SELL, 3, 51000.0);
    assert(gate.ledger().reserved("", "005930") == 7);
    assert(gate.ledger().sellable_view("", "005930").pending == 3);

    // 매수 체결 — 매도 선점 3은 남는다
    gate.ledger().on_fill_confirmed("", "005930", OrderSide::BUY, 10, 50000.0);
    assert(gate.ledger().position("", "005930") == 20);
    assert(gate.ledger().reserved("", "005930") == -3);
    assert(gate.ledger().sellable_view("", "005930").pending == 3);
    assert(gate.clamp_buy_quantity(make_signal("005930", OrderSide::SELL, 20)) == 17);

    // 매수 취소도 매도 선점을 건드리지 않는다
    gate.ledger().on_accept("", "005930", OrderSide::BUY, 10, 50000.0);
    gate.ledger().on_cancel("", "005930", OrderSide::BUY, 10);
    assert(gate.ledger().sellable_view("", "005930").pending == 3);

    // 매도 체결로 매도 선점이 풀리면 비어 있다
    gate.ledger().on_fill_confirmed("", "005930", OrderSide::SELL, 3, 51000.0);
    assert(gate.ledger().reserved("", "005930") == 0);
    assert(gate.ledger().sellable_view("", "005930").pending == 0);
    PASS("buy_and_sell_reservations_kept_apart");
}

// ─── 테스트 28: 잔고 대조 기록(ADJUST)이 매수·매도 선점을 둘 다 되살린다 ─────────
//  순값만 적던 때는 매수 5·매도 3이 +2로 적혀, 재기동 뒤 매도 3이 사라지고 매수 2만 남았다.
void test_adjust_replay_keeps_both_reservations()
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "quant_gate_test_adjust_split";
    std::error_code             error_code;
    std::filesystem::remove_all(directory, error_code);
    std::filesystem::create_directories(directory, error_code);
    const std::string date = "20260926";

    {
        OrderGate gate(OrderGate::Config{});
        assert(gate.ledger().set_journal(directory, date, false));
        gate.ledger().on_fill_confirmed("", "005930", OrderSide::BUY, 10, 50000.0);
        assert(gate.ledger().on_intent("", "005930", OrderSide::BUY, 5, 50000.0,
                                       PositionLedger::OrderRef{1, 0, OrderType::LIMIT}));
        assert(gate.ledger().on_intent("", "005930", OrderSide::SELL, 3, 51000.0,
                                       PositionLedger::OrderRef{2, 0, OrderType::LIMIT}));
        gate.ledger().refresh_sellable("", "005930", 7); // ADJUST 한 줄을 남긴다
    }

    OrderGate restarted(OrderGate::Config{});
    assert(restarted.ledger().set_journal(directory, date, false));
    assert(restarted.ledger().reserved("", "005930") == 2);
    assert(restarted.ledger().sellable_view("", "005930").pending == 3);
    std::filesystem::remove_all(directory, error_code);
    PASS("adjust_replay_keeps_both_reservations");
}

void run_ledger_tests()
{
    test_slot_exempt();
    test_slot_exempt_owner_union();
    test_publish_ledger_matches_gate();
    test_verdict_codes_and_describe();
    test_reset_daily_once_per_trading_date();
    test_gross_exposure_uses_mark_price();
    test_buy_and_sell_reservations_kept_apart();
    test_adjust_replay_keeps_both_reservations();
}
