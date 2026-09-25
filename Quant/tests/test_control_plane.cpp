// 제어 요청 통로(ControlPlane) 단위 테스트. 뒤 토막 두 줄을 시험 버퍼 위에 놓고 Engine 없이
//  순번 찍기·낱말별 줄 가르기·주문 쪽 적용(종목·전략 등록, 스위치, 표 모으기, 보호 주문)·가득 참 셈을 고정한다.
//  관련 결정: D-114.
#include "core/ControlPlane.h"
#include "risk/OrderGate.h"
#include "risk/ProtectiveOrders.h"

#include <cassert>
#include <cstddef>
#include <iostream>
#include <string>

namespace
{
constexpr size_t kLaneCapacity = 64;
constexpr size_t kLaneBytes    = ipc::SharedSpscRing<ipc::ControlRequest>::bytes_for(kLaneCapacity);

alignas(ipc::kSharedCacheLine) std::byte g_order_storage[kLaneBytes];
alignas(ipc::kSharedCacheLine) std::byte g_feed_storage[kLaneBytes];

// Engine 대신 통로가 닿는 것들을 쥐는 자리.
struct Harness
{
    OrderGate                               gate;
    risk::ProtectiveOrderBook               book{risk::ProtectiveMode::Owner}; // 기본 Off는 owns가 늘 거짓이다
    symbol::SymbolTable                     table{64};
    wake::WakeGate                          strategy_wake;
    wake::WakeGate                          order_wake;
    int                                     reset_count = 0;
    ipc::SharedSpscRing<ipc::ControlRequest> order_lane;
    ipc::SharedSpscRing<ipc::ControlRequest> feed_lane;
    ControlPlane plane{gate, book, table, strategy_wake, order_wake, [this] { ++reset_count; }};

    Harness()
    {
        assert(order_lane.create(g_order_storage, kLaneBytes, kLaneCapacity));
        assert(feed_lane.create(g_feed_storage, kLaneBytes, kLaneCapacity));
        plane.bind(&order_lane, &feed_lane);
    }

    // 보내고 옮기고 적용까지 한 번에 — 주문 스레드 한 바퀴와 같다.
    void send_and_apply(ipc::ControlRequest& request)
    {
        assert(plane.send(request));
        plane.relay();
        plane.apply();
    }
};

ipc::ControlRequest request_of(ipc::ControlKind kind)
{
    ipc::ControlRequest request;
    request.kind = kind;
    return request;
}

// 순번은 1부터 오르고, 구독 낱말은 시세 쪽 줄로, 나머지는 주문 쪽 줄로 간다.
void check_sequence_and_routing()
{
    Harness harness;

    ipc::ControlRequest symbol_request = request_of(ipc::ControlKind::kRegisterSymbol);
    symbol_request.ticker              = std::string_view("005930");
    ipc::ControlRequest watch_request  = request_of(ipc::ControlKind::kWatchSubscribe);

    assert(harness.plane.send(symbol_request));
    assert(harness.plane.send(watch_request));
    assert(symbol_request.sequence == 1 && watch_request.sequence == 2);

    harness.plane.relay();
    assert(!harness.plane.order_lane_empty());

    ipc::ControlRequest taken;
    assert(harness.plane.pop_feed(taken));
    assert(taken.kind == ipc::ControlKind::kWatchSubscribe);
    assert(!harness.plane.pop_feed(taken));

    harness.plane.apply();
    assert(harness.plane.order_lane_empty());
    assert(harness.table.lookup("005930") != symbol::kNone);
}

// 스위치 다섯과 하루치 새로 열기는 주문 쪽 적용에서 게이트·콜백에 닿는다.
void check_switches()
{
    Harness harness;

    ipc::ControlRequest kill = request_of(ipc::ControlKind::kKillSwitch);
    kill.toggle_on           = 1;
    harness.send_and_apply(kill);
    assert(harness.gate.is_killed());

    ipc::ControlRequest halt = request_of(ipc::ControlKind::kEntryHalt);
    halt.toggle_on           = 1;
    harness.send_and_apply(halt);
    assert(harness.gate.is_entry_halted());

    ipc::ControlRequest scale = request_of(ipc::ControlKind::kEntryScale);
    scale.entry_scale         = 0.5;
    harness.send_and_apply(scale);
    assert(harness.gate.entry_scale() == 0.5);

    ipc::ControlRequest manual = request_of(ipc::ControlKind::kManualHalt);
    manual.halt_side           = static_cast<uint8_t>(static_cast<OrderSide::Value>(OrderSide::SELL));
    manual.toggle_on           = 1;
    harness.send_and_apply(manual);
    assert(harness.gate.is_manual_sell_halted() && !harness.gate.is_manual_buy_halted());

    ipc::ControlRequest reset = request_of(ipc::ControlKind::kResetDaily);
    harness.send_and_apply(reset);
    assert(harness.reset_count == 1);
}

// 표는 commit의 줄 수가 맞을 때만 걸리고, 어긋나면 버린 줄 수를 센다.
void check_slot_exempt_table()
{
    Harness harness;

    ipc::ControlRequest open = request_of(ipc::ControlKind::kSlotExemptBegin);
    assert(harness.plane.send(open));

    for (const symbol::SymbolId symbol : {3u, 5u})
    {
        ipc::ControlRequest row = request_of(ipc::ControlKind::kSlotExemptEntry);
        row.batch               = open.sequence;
        row.symbol_id           = symbol;
        assert(harness.plane.send(row));
    }

    ipc::ControlRequest close = request_of(ipc::ControlKind::kSlotExemptCommit);
    close.batch               = open.sequence;
    close.row_count           = 2;
    harness.send_and_apply(close);

    const auto& ledger = harness.gate.ledger();
    assert(ledger.is_slot_exempt(3) && ledger.is_slot_exempt(5) && !ledger.is_slot_exempt(4));
    assert(harness.plane.discarded() == 0);

    // 줄 하나를 잃은 표 — commit이 3줄이라는데 1줄만 왔다. 걸지 않고 직전 표를 둔다.
    ipc::ControlRequest open_again = request_of(ipc::ControlKind::kSlotExemptBegin);
    assert(harness.plane.send(open_again));
    ipc::ControlRequest lone = request_of(ipc::ControlKind::kSlotExemptEntry);
    lone.batch               = open_again.sequence;
    lone.symbol_id           = 7;
    assert(harness.plane.send(lone));
    ipc::ControlRequest short_close = request_of(ipc::ControlKind::kSlotExemptCommit);
    short_close.batch               = open_again.sequence;
    short_close.row_count           = 3;
    harness.send_and_apply(short_close);

    assert(!ledger.is_slot_exempt(7) && ledger.is_slot_exempt(3));
    assert(harness.plane.discarded() > 0);
}

// 보호 주문 창구 — 등록은 요청이 되어 주문 쪽 적용에서 표에 오르고, 해제도 같은 길로 내려간다.
void check_protective_registry()
{
    Harness harness;

    ipc::ControlRequest strategy_request = request_of(ipc::ControlKind::kRegisterStrategy);
    strategy_request.strategy_name.assign("fake");
    ipc::ControlRequest symbol_request = request_of(ipc::ControlKind::kRegisterSymbol);
    symbol_request.ticker              = std::string_view("000660");
    harness.send_and_apply(strategy_request);
    harness.send_and_apply(symbol_request);

    risk::ProtectiveOrderRegistry& registry = harness.plane.protective_registry();

    // 번호가 없는 규칙은 요청도 안 나간다.
    risk::ProtectiveRule nameless;
    nameless.symbol = symbol::kNone;
    registry.arm(nameless);
    harness.plane.relay();
    assert(harness.plane.order_lane_empty());

    risk::ProtectiveRule rule;
    rule.account           = "acct";
    rule.ticker            = "000660";
    rule.symbol            = harness.table.lookup("000660");
    rule.owner_index       = harness.gate.ledger().strategy_table().lookup("fake");
    rule.stop_loss_percent = 5.0;
    registry.arm(rule);
    harness.plane.relay();
    assert(!registry.owns("acct", rule.symbol)); // 적용 전에는 표에 없다
    harness.plane.apply();
    assert(registry.owns("acct", rule.symbol));

    registry.disarm("acct", rule.symbol);
    harness.plane.relay();
    harness.plane.apply();
    assert(!registry.owns("acct", rule.symbol));
}

// 뒤 토막이 가득 차면 옮기지 못한 줄을 세고, 앞 토막이 가득 차면 보내기가 거짓을 돌려준다.
void check_full_lanes()
{
    Harness harness;

    for (size_t index = 0; index < kLaneCapacity + 6; ++index)
    {
        ipc::ControlRequest halt = request_of(ipc::ControlKind::kEntryHalt);
        assert(harness.plane.send(halt));
    }

    harness.plane.relay();
    assert(harness.plane.relay_dropped() >= 6);
    harness.plane.apply();

    size_t refused = 0;

    for (size_t index = 0; index <= ControlPlane::kQueueCapacity; ++index)
    {
        ipc::ControlRequest halt = request_of(ipc::ControlKind::kEntryHalt);
        refused += harness.plane.send(halt) ? 0 : 1;
    }

    assert(refused >= 1 && harness.plane.dropped() == refused);
}
} // namespace

int main()
{
    check_sequence_and_routing();
    check_switches();
    check_slot_exempt_table();
    check_protective_registry();
    check_full_lanes();

    std::cout << "test_control_plane: 전부 통과\n";
    return 0;
}
