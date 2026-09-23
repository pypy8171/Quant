// tests/test_shared_layout.cpp
// 공유 쪽지 위 자리표 검증 (D-114 단계 4) — 면 여덟이 겹치지 않고, 양쪽이 같은 자리를 보는가.
//
//   ① 자리 셈이 면 여덟의 합과 맞는가(면마다 캐시라인까지 올린 크기)
//   ② 놓은 쪽이 쓴 것을 붙은 쪽이 면마다 그대로 보는가 — 이게 자리표를 만든 이유다
//   ③ 면이 서로를 덮지 않는가 — 여덟 면에 다른 값을 쓰고 전부 다시 읽는다(자리가 어긋나면 여기서 깨진다)
//   ④ 붙는 쪽이 값을 지우지 않는가(장부 사본·박동은 머리가 없어 붙는 쪽이 지으면 실린 값이 날아간다)
//   ⑤ 값이 다르거나 구역이 작으면 안 붙는가(옛 exe·다른 설정이 새 배치에 붙는 길이 없는가)
//
//   사용법: test_shared_layout

#include "ipc/SharedLayout.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cout << "[FAIL] " << name << "\n";
        std::abort();
    }

    std::cout << "[PASS] " << name << "\n";
}

// 시험용 작은 자리표 — 면 여덟이 다 들어가되 구역이 2MB를 넘지 않게. 장부 사본은 크기가 고정이라
//  (종목 8,192줄) 이 구역의 대부분을 그것이 쓴다.
constexpr size_t kStorageBytes      = 2 * 1024 * 1024;
constexpr size_t kSmallStorageBytes = 4096;
constexpr size_t kSymbolCapacity    = 64;

alignas(ipc::kSharedCacheLine) std::byte g_storage[kStorageBytes];
alignas(ipc::kSharedCacheLine) std::byte g_small_storage[kSmallStorageBytes];

ipc::SharedLayoutConfig test_config()
{
    ipc::SharedLayoutConfig config;
    config.feed_lanes               = 2;
    config.symbol_capacity          = kSymbolCapacity;
    config.strategy_capacity        = 8;
    config.request_capacity         = 8;
    config.response_capacity        = 8;
    config.control_capacity         = 8;
    config.feed_trade_capacity      = 16;
    config.feed_order_book_capacity = 8;
    return config;
}

TradeData trade_of(symbol::SymbolId symbol_id, int64_t quantity)
{
    TradeData trade;
    trade.ticker      = "005930";
    trade.symbol_id   = symbol_id;
    trade.hhmmss      = 93001;
    trade.price       = 70000.0;
    trade.quantity    = quantity;
    trade.direction   = 1;
    trade.market      = Market::KR;
    trade.received_ns = 12345;
    return trade;
}

ipc::MarketLimits feed_limits()
{
    ipc::MarketLimits limits;
    limits.symbol_count = kSymbolCapacity;
    return limits;
}

size_t align_up(size_t bytes)
{
    return (bytes + ipc::kSharedCacheLine - 1) / ipc::kSharedCacheLine * ipc::kSharedCacheLine;
}

void test_size_math()
{
    const ipc::SharedLayoutConfig config = test_config();

    size_t expected = align_up(sizeof(ipc::SharedLayoutHead));
    expected += align_up(ipc::SharedSpscRing<ipc::OrderRequest>::bytes_for(config.request_capacity));
    expected += align_up(ipc::SharedSpscRing<ipc::OrderResponse>::bytes_for(config.response_capacity));
    expected += align_up(ipc::SharedSpscRing<ipc::ControlRequest>::bytes_for(config.control_capacity));
    expected += align_up(sizeof(ipc::SharedHeartbeats));
    expected += align_up(ipc::MarketFeedChannel::bytes_for(config.feed_lanes, config.feed_trade_capacity,
                                                           config.feed_order_book_capacity));
    expected += align_up(ipc::SharedSymbolDictionary::bytes_for(config.symbol_capacity));
    expected += align_up(ipc::SharedStrategyDictionary::bytes_for(config.strategy_capacity));
    expected += align_up(sizeof(ipc::LedgerSnapshot));

    check(ipc::SharedLayout::bytes_for(config) == expected, "자리 셈: 머리 하나와 면 여덟의 합");
    check(ipc::SharedLayout::bytes_for(config) % ipc::kSharedCacheLine == 0, "자리표 전체가 캐시라인 배수");
    check(ipc::SharedLayout::bytes_for(config) < kStorageBytes, "시험 자리표가 시험 구역에 든다");

    // 실제로 쓸 값으로도 한 번 — 줄 하나에 약 3.1MB라 구역이 4MB를 넘는다. 기동 때 잡을 크기를 여기서 못 박는다.
    const size_t live_bytes = ipc::SharedLayout::bytes_for(ipc::SharedLayoutConfig{});
    check(live_bytes > 3 * 1024 * 1024, "기본 설정 자리표는 3MB를 넘는다");
    check(live_bytes < 16 * 1024 * 1024, "기본 설정 자리표는 16MB를 넘지 않는다");
}

// 면 여덟에 서로 다른 값을 심는다. 자리가 겹치면 뒤에 심은 면이 앞 면을 덮어 ③에서 깨진다.
void fill_all_faces(ipc::SharedLayout& layout)
{
    ipc::OrderRequest request;
    request.sequence  = 7;
    request.quantity  = 111;
    request.symbol_id = 1;
    check(layout.requests().push(request), "요청 심기");

    ipc::OrderResponse response;
    response.sequence         = 9;
    response.kis_order_number = 222;
    check(layout.responses().push(response), "응답 심기");

    ipc::ControlRequest control;
    control.sequence  = 11;
    control.kind      = ipc::ControlKind::kArmProtective;
    control.symbol_id = 3;
    check(layout.controls().push(control), "제어 요청 심기");

    layout.heartbeats()->strategy.beat(1234);
    layout.heartbeats()->order.beat(5678);

    check(layout.feed().push_trade(0, trade_of(1, 10)), "시세 0번 줄 심기");
    check(layout.feed().push_trade(1, trade_of(2, 20)), "시세 1번 줄 심기");

    check(layout.symbols().intern("005930") == 1, "종목 표 심기");
    check(layout.strategies().intern("DEVSCALE_005930") == 1, "전략 이름표 심기");

    layout.ledger()->begin_publish();
    layout.ledger()->row_for_write(1).position      = 33;
    layout.ledger()->row_for_write(1).average_price = 70000.0;
    layout.ledger()->globals_for_write().open_slot_count = 25;
    layout.ledger()->end_publish();
}

// 심은 값 여덟을 그대로 보는가. 놓은 손잡이로도, 붙은 손잡이로도 부른다.
void read_all_faces(ipc::SharedLayout& layout, const std::string& who)
{
    ipc::OrderRequest request;
    check(layout.requests().pop(request), who + ": 요청이 나온다");
    check(request.sequence == 7 && request.quantity == 111, who + ": 요청 값이 그대로");

    ipc::OrderResponse response;
    check(layout.responses().pop(response), who + ": 응답이 나온다");
    check(response.sequence == 9 && response.kis_order_number == 222, who + ": 응답 값이 그대로");

    ipc::ControlRequest control;
    check(layout.controls().pop(control), who + ": 제어 요청이 나온다");
    check(control.sequence == 11 && control.kind == ipc::ControlKind::kArmProtective, who + ": 제어 값이 그대로");

    check(layout.heartbeats()->strategy.last_ns() == 1234, who + ": 전략 박동이 그대로");
    check(layout.heartbeats()->order.last_ns() == 5678, who + ": 주문 박동이 그대로");

    TradeData trade;
    check(layout.feed().pop_trade(0, feed_limits(), trade), who + ": 시세 0번 줄이 나온다");
    check(trade.quantity == 10, who + ": 0번 줄 값이 그대로");
    check(layout.feed().pop_trade(1, feed_limits(), trade), who + ": 시세 1번 줄이 나온다");
    check(trade.quantity == 20, who + ": 1번 줄 값이 그대로");

    check(layout.symbols().lookup("005930") == 1, who + ": 종목 번호가 그대로");
    check(layout.strategies().lookup("DEVSCALE_005930") == 1, who + ": 전략 번호가 그대로");

    const ipc::LedgerRow row = layout.ledger()->row(1);
    check(row.position == 33 && row.average_price == 70000.0, who + ": 장부 줄이 그대로");
    check(layout.ledger()->globals().open_slot_count == 25, who + ": 장부 전역값이 그대로");
}

void test_both_sides_see_the_same()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout order_side;
    check(order_side.create(g_storage, kStorageBytes, config), "주문 쪽이 놓는다");
    check(order_side.is_bound(), "놓은 뒤에는 붙은 상태");

    fill_all_faces(order_side);

    ipc::SharedLayout strategy_side;
    check(strategy_side.attach(g_storage, kStorageBytes, config), "전략 쪽이 붙는다");

    read_all_faces(strategy_side, "전략 쪽");
}

void test_faces_do_not_overlap()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout layout;
    check(layout.create(g_storage, kStorageBytes, config), "놓기(겹침 시험)");

    // 큐 셋을 가득 채운다 — 자리가 겹치면 넘친 칸이 이웃 면의 머리를 덮는다.
    for (size_t index = 0; index < config.request_capacity; ++index)
    {
        ipc::OrderRequest request;
        request.sequence = index + 1;
        (void)layout.requests().push(request);
    }

    for (size_t index = 0; index < config.control_capacity; ++index)
    {
        ipc::ControlRequest control;
        control.sequence = index + 1;
        control.kind     = ipc::ControlKind::kSlotExemptEntry;
        (void)layout.controls().push(control);
    }

    for (size_t index = 0; index < config.feed_trade_capacity; ++index)
    {
        (void)layout.feed().push_trade(0, trade_of(1, 10));
        (void)layout.feed().push_trade(1, trade_of(2, 20));
    }

    // 종목 표·전략 이름표도 가득 채운다(번호는 1부터라 자리 하나가 비는 것이 정상이다).
    for (size_t index = 1; index < config.symbol_capacity; ++index)
    {
        (void)layout.symbols().intern(std::to_string(100000 + index));
    }

    for (size_t index = 1; index < config.strategy_capacity; ++index)
    {
        (void)layout.strategies().intern("DEVSCALE_" + std::to_string(100000 + index));
    }

    // 장부 사본은 마지막 면이다 — 앞 일곱이 넘쳤으면 여기가 먼저 깨진다.
    layout.ledger()->begin_publish();

    for (symbol::SymbolId id = 1; id < static_cast<symbol::SymbolId>(config.symbol_capacity); ++id)
    {
        layout.ledger()->row_for_write(id).position = static_cast<int32_t>(id) * 2;
    }

    layout.ledger()->end_publish();

    check(layout.symbols().lookup("100001") == 1, "가득 찬 뒤에도 종목 번호가 맞는다");
    check(layout.strategies().lookup("DEVSCALE_100001") == 1, "가득 찬 뒤에도 전략 번호가 맞는다");
    check(layout.ledger()->row(1).position == 2, "가득 찬 뒤에도 장부 첫 줄이 맞는다");
    check(layout.ledger()->row(config.symbol_capacity - 1).position ==
              static_cast<int32_t>(config.symbol_capacity - 1) * 2,
          "가득 찬 뒤에도 장부 마지막 줄이 맞는다");

    ipc::OrderRequest request;
    check(layout.requests().pop(request), "가득 찬 요청 큐에서 나온다");
    check(request.sequence == 1, "요청 첫 칸이 안 덮였다");

    ipc::ControlRequest control;
    check(layout.controls().pop(control), "가득 찬 제어 큐에서 나온다");
    check(control.sequence == 1, "제어 첫 칸이 안 덮였다");

    TradeData trade;
    check(layout.feed().pop_trade(0, feed_limits(), trade), "가득 찬 시세 줄에서 나온다");
    check(trade.quantity == 10, "시세 첫 칸이 안 덮였다");
}

void test_attach_keeps_values()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout order_side;
    check(order_side.create(g_storage, kStorageBytes, config), "놓기(값 보존 시험)");

    order_side.heartbeats()->order.beat(777);
    order_side.ledger()->begin_publish();
    order_side.ledger()->row_for_write(2).position = 44;
    order_side.ledger()->end_publish();

    const uint64_t generation = order_side.ledger()->generation();

    // 전략이 죽었다 다시 뜨는 자리다 — 붙는 쪽이 장부·박동을 새로 지으면 주문 쪽이 실어 둔 보유가
    //  0으로 지워지고, 전략은 "보유 0"으로 읽어 같은 종목을 또 산다(이중 발주, A등급).
    ipc::SharedLayout strategy_side;
    check(strategy_side.attach(g_storage, kStorageBytes, config), "붙기(값 보존 시험)");

    check(strategy_side.ledger()->row(2).position == 44, "붙어도 장부 줄이 남는다");
    check(strategy_side.ledger()->generation() == generation, "붙어도 판 번호가 그대로");
    check(strategy_side.heartbeats()->order.last_ns() == 777, "붙어도 박동이 남는다");

    strategy_side.unbind();
    check(!strategy_side.is_bound(), "뗀 뒤에는 안 붙은 상태");
    check(strategy_side.ledger() == nullptr, "뗀 뒤 장부는 없다");
    check(strategy_side.heartbeats() == nullptr, "뗀 뒤 박동은 없다");
    check(order_side.ledger()->row(2).position == 44, "한쪽이 떼어도 건너편 값은 그대로");
}

void test_refusals()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout layout;
    check(layout.create(g_storage, kStorageBytes, config), "놓기(거절 시험)");

    ipc::SharedLayout other;

    check(!other.attach(nullptr, kStorageBytes, config), "자리가 없으면 안 붙는다");
    check(!other.attach(g_storage + 8, kStorageBytes - 8, config), "경계가 어긋나면 안 붙는다");
    check(!other.attach(g_small_storage, kSmallStorageBytes, config), "구역이 작으면 안 붙는다");
    check(!other.last_error().empty(), "거절 사유가 남는다");

    ipc::SharedLayoutConfig wider_lanes = config;
    wider_lanes.feed_lanes              = config.feed_lanes + 1;
    check(!other.attach(g_storage, kStorageBytes, wider_lanes), "시세 줄 수가 다르면 안 붙는다");

    ipc::SharedLayoutConfig wider_symbols = config;
    wider_symbols.symbol_capacity        = config.symbol_capacity * 2;
    check(!other.attach(g_storage, kStorageBytes, wider_symbols), "종목 수가 다르면 안 붙는다");

    ipc::SharedLayoutConfig wider_requests = config;
    wider_requests.request_capacity        = config.request_capacity * 2;
    check(!other.attach(g_storage, kStorageBytes, wider_requests), "요청 칸 수가 다르면 안 붙는다");

    // 값 자체가 말이 안 되는 것들 — 붙기 전에 멈춘다
    ipc::SharedLayoutConfig no_lanes = config;
    no_lanes.feed_lanes              = 0;
    check(!other.create(g_storage, kStorageBytes, no_lanes), "시세 줄 0은 못 놓는다");

    ipc::SharedLayoutConfig too_many_symbols = config;
    too_many_symbols.symbol_capacity         = ipc::LedgerSnapshot::kMaxSymbols + 1;
    check(!other.create(g_storage, kStorageBytes, too_many_symbols), "종목 수가 장부 상한을 넘으면 못 놓는다");

    ipc::SharedLayoutConfig one_strategy = config;
    one_strategy.strategy_capacity       = 1;
    check(!other.create(g_storage, kStorageBytes, one_strategy), "전략 수 1은 못 놓는다");

    ipc::SharedLayoutConfig no_control = config;
    no_control.control_capacity         = 0;
    check(!other.create(g_storage, kStorageBytes, no_control), "칸 수 0인 큐는 못 놓는다");

    check(!other.is_bound(), "거절당한 손잡이는 안 붙은 상태로 남는다");
    check(other.attach(g_storage, kStorageBytes, config), "같은 값이면 붙는다");
}

// 자리표 머리가 지키는 것 — 면의 머리가 우연히 맞는 날에도 여기서 먼저 걸려야 한다.
void test_head_guards()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout layout;
    check(layout.create(g_storage, kStorageBytes, config), "놓기(머리 시험)");

    ipc::SharedLayoutHead* head = reinterpret_cast<ipc::SharedLayoutHead*>(g_storage);
    check(head->magic == ipc::kSharedLayoutMagic, "놓는 쪽이 표식을 적는다");
    check(head->layout_version == ipc::kSharedLayoutVersion, "놓는 쪽이 판 번호를 적는다");
    check(head->symbol_capacity == config.symbol_capacity, "놓는 쪽이 종목 수를 적는다");

    ipc::SharedLayout other;

    // 시세 줄이 **적을** 때 — 뒤따르는 면이 통째로 앞당겨진다. 구역 크기로는 안 걸린다(더 작게 든다).
    ipc::SharedLayoutConfig fewer_lanes = config;
    fewer_lanes.feed_lanes              = config.feed_lanes - 1;
    check(!other.attach(g_storage, kStorageBytes, fewer_lanes), "시세 줄이 적어도 안 붙는다");

    ipc::SharedLayoutConfig smaller_trades = config;
    smaller_trades.feed_trade_capacity    = config.feed_trade_capacity / 2;
    check(!other.attach(g_storage, kStorageBytes, smaller_trades), "체결 칸 수가 적어도 안 붙는다");

    const uint32_t saved_version = head->layout_version;
    head->layout_version         = saved_version + 1;
    check(!other.attach(g_storage, kStorageBytes, config), "판 번호가 다르면 안 붙는다");
    head->layout_version = saved_version;

    const uint32_t saved_magic = head->magic;
    head->magic                = 0;
    check(!other.attach(g_storage, kStorageBytes, config), "표식이 깨지면 안 붙는다");
    check(!other.is_bound(), "표식이 깨졌으면 반쪽으로도 안 붙는다");
    head->magic = saved_magic;

    check(other.attach(g_storage, kStorageBytes, config), "머리를 되돌리면 붙는다");
}

} // namespace

int main()
{
    std::cout << "=== 공유 쪽지 위 자리표 (D-114 단계 4) ===\n";
    test_size_math();
    test_both_sides_see_the_same();
    test_faces_do_not_overlap();
    test_attach_keeps_values();
    test_refusals();
    test_head_guards();
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
