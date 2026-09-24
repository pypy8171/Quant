// tests/test_shared_layout.cpp
// 공유 쪽지 위 자리표 검증 (D-114 단계 4·5) — 면 열이 겹치지 않고, 셋이 같은 자리를 보는가.
//
//   ① 자리 셈이 면 열의 합과 맞는가(면마다 캐시라인까지 올린 크기)
//   ② 한 쪽이 쓴 것을 건너편이 면마다 그대로 보는가 — 이게 자리표를 만든 이유다
//   ③ 면이 서로를 덮지 않는가 — 열 면에 다른 값을 쓰고 전부 다시 읽는다(자리가 어긋나면 여기서 깨진다)
//   ④ 붙는 쪽이 값을 지우지 않는가(장부 사본·박동은 머리가 없어 붙는 쪽이 지으면 실린 값이 날아간다)
//   ⑤ 값이 다르거나 구역이 작으면 안 붙는가(옛 exe·다른 설정이 새 배치에 붙는 길이 없는가)
//   ⑥ 붙는 쪽이 셋이 되어도 제 끝만 맡는가 — 제 줄이 아닌 면에서는 넣지도 꺼내지도 못한다
//
//   사용법: test_shared_layout

#include "ipc/SharedLayout.h"

#include <cstdlib>
#include <cstring>
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

// 시험용 작은 자리표 — 면 열이 다 들어가되 구역이 2MB를 넘지 않게. 장부 사본은 크기가 고정이라
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
    config.feed_control_capacity    = 8;
    config.fill_capacity            = 8;
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

ipc::FillNotice notice_of(uint64_t sequence, int32_t quantity)
{
    ipc::FillNotice notice;
    notice.sequence        = sequence;
    notice.sent_at_ns      = 1'700'000'000;
    notice.filled_price    = 70000.0;
    notice.filled_quantity = quantity;
    notice.order_quantity  = quantity;
    notice.side            = static_cast<uint8_t>(OrderSide::BUY);
    std::memcpy(notice.ticker, "005930", 7);
    std::memcpy(notice.kis_order_no, "0000012345", 11);
    return notice;
}

ipc::ControlRequest watch_of(uint64_t sequence, symbol::SymbolId symbol_id)
{
    ipc::ControlRequest control;
    control.sequence  = sequence;
    control.kind      = ipc::ControlKind::kWatchSubscribe;
    control.symbol_id = symbol_id;
    return control;
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
    expected += align_up(ipc::SharedSpscRing<ipc::ControlRequest>::bytes_for(config.feed_control_capacity));
    expected += align_up(ipc::FillChannel::bytes_for(config.fill_capacity));
    expected += align_up(sizeof(ipc::SharedHeartbeats));
    expected += align_up(sizeof(ipc::RegimeCell));
    expected += align_up(ipc::MarketFeedChannel::bytes_for(config.feed_lanes, config.feed_trade_capacity,
                                                           config.feed_order_book_capacity));
    expected += align_up(ipc::SharedSymbolDictionary::bytes_for(config.symbol_capacity));
    expected += align_up(ipc::SharedStrategyDictionary::bytes_for(config.strategy_capacity));
    expected += align_up(sizeof(ipc::LedgerSnapshot));

    check(ipc::SharedLayout::bytes_for(config) == expected, "자리 셈: 머리 하나와 면 열의 합");
    check(ipc::SharedLayout::bytes_for(config) % ipc::kSharedCacheLine == 0, "자리표 전체가 캐시라인 배수");
    check(ipc::SharedLayout::bytes_for(config) < kStorageBytes, "시험 자리표가 시험 구역에 든다");

    // 실제로 쓸 값으로도 한 번 — 줄 하나에 약 3.1MB라 구역이 4MB를 넘는다. 기동 때 잡을 크기를 여기서 못 박는다.
    const size_t live_bytes = ipc::SharedLayout::bytes_for(ipc::SharedLayoutConfig{});
    check(live_bytes > 3 * 1024 * 1024, "기본 설정 자리표는 3MB를 넘는다");
    check(live_bytes < 16 * 1024 * 1024, "기본 설정 자리표는 16MB를 넘지 않는다");
}

// 셋이 한 쪽지에 붙어 제 끝으로만 주고받는다. 면이 겹치면 여기서 값이 서로를 덮는다.
//  줄마다 보내는 쪽이 정해져 있으므로(주문·전략·시세) 값을 심는 자리도 줄마다 다르다 —
//  한 손잡이로 다 심고 다 읽던 옛 시험은 붙는 쪽이 둘일 때만 성립했다. [why D-114]
void test_three_sides_see_the_same()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout order_side;
    check(order_side.create(g_storage, kStorageBytes, config), "주문 쪽이 놓는다");
    check(order_side.is_bound(), "놓은 뒤에는 붙은 상태");

    ipc::SharedLayout strategy_side;
    check(strategy_side.attach(g_storage, kStorageBytes, config, ipc::SharedAttachRole::kStrategy),
          "전략 쪽이 붙는다");

    ipc::SharedLayout feed_side;
    check(feed_side.attach(g_storage, kStorageBytes, config, ipc::SharedAttachRole::kFeed), "시세 쪽이 붙는다");

    // 종목 표·전략 이름표·장부는 주문 쪽만 쓴다(원칙 4: 번호를 다는 쪽은 하나).
    check(order_side.symbols().intern("005930") == 1, "종목 표 심기");
    check(order_side.strategies().intern("DEVSCALE_005930") == 1, "전략 이름표 심기");
    order_side.ledger()->begin_publish();
    order_side.ledger()->row_for_write(1).position          = 33;
    order_side.ledger()->row_for_write(1).average_price     = 70000.0;
    order_side.ledger()->globals_for_write().open_slot_count = 25;
    order_side.ledger()->end_publish();

    // 국면 칸 — 놓는 쪽이 "아직 판정 없음"으로 밀어야 판정 전 체결이 국면 0(RISK_ON)으로 적히지 않는다.
    check(order_side.regime_cell()->code.load() == ipc::kRegimeNone, "놓는 쪽이 국면을 판정 없음으로 민다");

    // 국면은 전략만 적는다. 값 하나라 순서 맞출 것이 없다 — 적은 뒤 나머지가 그 값을 본다. [why D-129]
    strategy_side.regime_cell()->code.store(2);

    // 박동은 셋이 제 칸에만 찍는다.
    order_side.heartbeats()->order.beat(5678);
    strategy_side.heartbeats()->strategy.beat(1234);
    feed_side.heartbeats()->feed.beat(9012);

    // 전략 → 주문: 주문 요청·제어 요청
    ipc::OrderRequest request;
    request.sequence  = 7;
    request.quantity  = 111;
    request.symbol_id = 1;
    check(strategy_side.requests().push(request), "전략이 주문 요청을 넣는다");

    ipc::ControlRequest control;
    control.sequence  = 11;
    control.kind      = ipc::ControlKind::kArmProtective;
    control.symbol_id = 3;
    check(strategy_side.controls().push(control), "전략이 제어 요청을 넣는다");

    // 전략 → 시세: 구독 요청(낱말로 가른 줄)
    check(strategy_side.feed_controls().push(watch_of(13, 5)), "전략이 구독 요청을 넣는다");

    // 주문 → 전략: 주문 응답
    ipc::OrderResponse response;
    response.sequence         = 9;
    response.kis_order_number = 222;
    check(order_side.responses().push(response), "주문이 응답을 넣는다");

    // 시세 → 전략: 체결·호가 / 시세 → 주문: 체결통보
    check(feed_side.feed().push_trade(0, trade_of(1, 10)), "시세가 0번 줄에 체결을 넣는다");
    check(feed_side.feed().push_trade(1, trade_of(2, 20)), "시세가 1번 줄에 체결을 넣는다");
    check(feed_side.fills().push(notice_of(21, 5)), "시세가 체결통보를 넣는다");

    // 받는 쪽에서 그대로 나오는가.
    ipc::OrderRequest taken_request;
    check(order_side.requests().pop(taken_request), "주문 쪽에 요청이 나온다");
    check(taken_request.sequence == 7 && taken_request.quantity == 111, "요청 값이 그대로");

    ipc::ControlRequest taken_control;
    check(order_side.controls().pop(taken_control), "주문 쪽에 제어 요청이 나온다");
    check(taken_control.sequence == 11 && taken_control.kind == ipc::ControlKind::kArmProtective,
          "제어 값이 그대로");

    ipc::FillNotice taken_notice;
    check(order_side.fills().pop(ipc::FillLimits{}, taken_notice), "주문 쪽에 체결통보가 나온다");
    check(taken_notice.sequence == 21 && taken_notice.filled_quantity == 5, "체결통보 값이 그대로");
    check(std::string(taken_notice.ticker) == "005930", "체결통보 종목 코드가 그대로");

    ipc::ControlRequest taken_watch;
    check(feed_side.feed_controls().pop(taken_watch), "시세 쪽에 구독 요청이 나온다");
    check(taken_watch.sequence == 13 && taken_watch.kind == ipc::ControlKind::kWatchSubscribe,
          "구독 요청 값이 그대로");
    check(taken_watch.symbol_id == 5, "구독 요청 종목 번호가 그대로");

    ipc::OrderResponse taken_response;
    check(strategy_side.responses().pop(taken_response), "전략 쪽에 응답이 나온다");
    check(taken_response.sequence == 9 && taken_response.kis_order_number == 222, "응답 값이 그대로");

    TradeData trade;
    check(strategy_side.feed().pop_trade(0, feed_limits(), trade), "전략 쪽에 0번 줄 체결이 나온다");
    check(trade.quantity == 10, "0번 줄 값이 그대로");
    check(strategy_side.feed().pop_trade(1, feed_limits(), trade), "전략 쪽에 1번 줄 체결이 나온다");
    check(trade.quantity == 20, "1번 줄 값이 그대로");

    // 머리 없는 면(박동·표·장부)은 셋 다 같은 값을 본다.
    check(strategy_side.heartbeats()->order.last_ns() == 5678, "전략 쪽이 주문 박동을 본다");
    check(feed_side.heartbeats()->strategy.last_ns() == 1234, "시세 쪽이 전략 박동을 본다");
    check(order_side.heartbeats()->feed.last_ns() == 9012, "주문 쪽이 시세 박동을 본다");

    check(strategy_side.symbols().lookup("005930") == 1, "전략 쪽 종목 번호가 그대로");
    check(feed_side.symbols().lookup("005930") == 1, "시세 쪽 종목 번호가 그대로");
    check(strategy_side.strategies().lookup("DEVSCALE_005930") == 1, "전략 쪽 전략 번호가 그대로");

    const ipc::LedgerRow row = strategy_side.ledger()->row(1);
    check(row.position == 33 && row.average_price == 70000.0, "전략 쪽 장부 줄이 그대로");
    check(feed_side.ledger()->globals().open_slot_count == 25, "시세 쪽 장부 전역값이 그대로");

    // 여기가 뚫리면 갈라 띄운 날 체결의 regime 열이 통째로 빈다 — 국면별로 되짚을 수 없다. [why D-129]
    check(order_side.regime_cell()->code.load() == 2, "주문 쪽이 전략이 고른 국면을 본다");
    check(feed_side.regime_cell()->code.load() == 2, "시세 쪽도 같은 국면을 본다");
}

// ⑥ 제 줄이 아닌 면에서는 넣지도 꺼내지도 못한다. 여기가 뚫리면 붙는 쪽 하나가 남의 줄의
//  받은 자리를 적어 이미 읽은 것을 다시 읽힌다(체결통보 이중 반영, A등급). [why D-114]
void test_roles_keep_their_end()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout order_side;
    check(order_side.create(g_storage, kStorageBytes, config), "놓기(끝 지키기 시험)");

    ipc::SharedLayout strategy_side;
    check(strategy_side.attach(g_storage, kStorageBytes, config, ipc::SharedAttachRole::kStrategy),
          "붙기(전략, 끝 지키기 시험)");

    ipc::SharedLayout feed_side;
    check(feed_side.attach(g_storage, kStorageBytes, config, ipc::SharedAttachRole::kFeed),
          "붙기(시세, 끝 지키기 시험)");

    // 주문 요청 줄은 전략이 넣고 주문이 꺼낸다 — 전략이 꺼낼 수 없고, 시세는 양쪽 다 못 한다.
    ipc::OrderRequest request;
    request.sequence = 31;
    check(strategy_side.requests().push(request), "전략은 요청 줄에 넣는다");

    ipc::OrderRequest taken_request;
    check(!strategy_side.requests().pop(taken_request), "전략은 제가 넣은 요청을 못 꺼낸다");
    check(!feed_side.requests().pop(taken_request), "시세는 요청 줄에서 못 꺼낸다");
    check(!feed_side.requests().push(request), "시세는 요청 줄에 못 넣는다");
    check(order_side.requests().pop(taken_request), "주문은 요청을 꺼낸다");
    check(taken_request.sequence == 31, "끼어든 손잡이가 자리를 밀지 않았다");

    // 체결통보 줄은 시세가 넣고 주문이 꺼낸다 — 전략은 양쪽 다 못 한다.
    check(feed_side.fills().push(notice_of(41, 3)), "시세는 체결통보를 넣는다");

    ipc::FillNotice taken_notice;
    check(!strategy_side.fills().pop(ipc::FillLimits{}, taken_notice), "전략은 체결통보를 못 꺼낸다");
    check(!strategy_side.fills().push(notice_of(42, 3)), "전략은 체결통보를 못 넣는다");
    check(order_side.fills().pop(ipc::FillLimits{}, taken_notice), "주문은 체결통보를 꺼낸다");
    check(taken_notice.sequence == 41, "끼어든 손잡이가 체결통보 자리를 밀지 않았다");

    // 구독 줄은 전략이 넣고 시세가 꺼낸다 — 주문 쪽은 만든 쪽이라 양끝을 겸하므로 여기서는 안 본다.
    check(strategy_side.feed_controls().push(watch_of(51, 9)), "전략은 구독 요청을 넣는다");

    ipc::ControlRequest taken_watch;
    check(!strategy_side.feed_controls().pop(taken_watch), "전략은 제가 넣은 구독 요청을 못 꺼낸다");
    check(feed_side.feed_controls().pop(taken_watch), "시세는 구독 요청을 꺼낸다");
    check(taken_watch.sequence == 51, "구독 요청 자리가 안 밀렸다");
}

void test_faces_do_not_overlap()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout layout;
    check(layout.create(g_storage, kStorageBytes, config), "놓기(겹침 시험)");

    // 큐 다섯을 가득 채운다 — 자리가 겹치면 넘친 칸이 이웃 면의 머리를 덮는다.
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

    for (size_t index = 0; index < config.feed_control_capacity; ++index)
    {
        (void)layout.feed_controls().push(watch_of(index + 1, 2));
    }

    for (size_t index = 0; index < config.fill_capacity; ++index)
    {
        (void)layout.fills().push(notice_of(index + 1, 7));
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

    // 장부 사본은 마지막 면이다 — 앞 아홉이 넘쳤으면 여기가 먼저 깨진다.
    layout.ledger()->begin_publish();

    for (symbol::SymbolId id = 1; id < static_cast<symbol::SymbolId>(config.symbol_capacity); ++id)
    {
        layout.ledger()->row_for_write(id).position = static_cast<int32_t>(id) * 2;
    }

    layout.ledger()->end_publish();

    check(layout.symbols().lookup("100001") == 1, "가득 찬 뒤에도 종목 번호가 맞는다");
    check(layout.strategies().lookup("DEVSCALE_100001") == 1, "가득 찬 뒤에도 전략 번호가 맞는다");
    check(layout.ledger()->row(1).position == 2, "가득 찬 뒤에도 장부 첫 줄이 맞는다");
    const auto last_symbol = static_cast<symbol::SymbolId>(config.symbol_capacity - 1);

    check(layout.ledger()->row(last_symbol).position == static_cast<int32_t>(last_symbol) * 2,
          "가득 찬 뒤에도 장부 마지막 줄이 맞는다");

    ipc::OrderRequest request;
    check(layout.requests().pop(request), "가득 찬 요청 큐에서 나온다");
    check(request.sequence == 1, "요청 첫 칸이 안 덮였다");

    ipc::ControlRequest control;
    check(layout.controls().pop(control), "가득 찬 제어 큐에서 나온다");
    check(control.sequence == 1, "제어 첫 칸이 안 덮였다");

    ipc::ControlRequest watch;
    check(layout.feed_controls().pop(watch), "가득 찬 구독 큐에서 나온다");
    check(watch.sequence == 1 && watch.kind == ipc::ControlKind::kWatchSubscribe, "구독 첫 칸이 안 덮였다");

    ipc::FillNotice notice;
    check(layout.fills().pop(ipc::FillLimits{}, notice), "가득 찬 체결통보 큐에서 나온다");
    check(notice.sequence == 1, "체결통보 첫 칸이 안 덮였다");

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
    check(strategy_side.attach(g_storage, kStorageBytes, config, ipc::SharedAttachRole::kStrategy),
          "붙기(값 보존 시험)");

    check(strategy_side.ledger()->row(2).position == 44, "붙어도 장부 줄이 남는다");
    check(strategy_side.ledger()->generation() == generation, "붙어도 판 번호가 그대로");
    check(strategy_side.heartbeats()->order.last_ns() == 777, "붙어도 박동이 남는다");

    // 시세가 뒤늦게 더 붙어도 같다 — 붙는 쪽이 하나 더 늘어난 것이 이 단계의 달라진 점이다.
    ipc::SharedLayout feed_side;
    check(feed_side.attach(g_storage, kStorageBytes, config, ipc::SharedAttachRole::kFeed), "붙기(시세, 값 보존)");
    check(feed_side.ledger()->row(2).position == 44, "시세가 붙어도 장부 줄이 남는다");
    check(feed_side.heartbeats()->order.last_ns() == 777, "시세가 붙어도 박동이 남는다");

    strategy_side.unbind();
    check(!strategy_side.is_bound(), "뗀 뒤에는 안 붙은 상태");
    check(strategy_side.ledger() == nullptr, "뗀 뒤 장부는 없다");
    check(strategy_side.heartbeats() == nullptr, "뗀 뒤 박동은 없다");
    check(order_side.ledger()->row(2).position == 44, "한쪽이 떼어도 건너편 값은 그대로");
    check(feed_side.ledger()->row(2).position == 44, "한쪽이 떼어도 셋째 쪽 값도 그대로");
}

void test_refusals()
{
    const ipc::SharedLayoutConfig config = test_config();

    ipc::SharedLayout layout;
    check(layout.create(g_storage, kStorageBytes, config), "놓기(거절 시험)");

    ipc::SharedLayout       other;
    const ipc::SharedAttachRole strategy = ipc::SharedAttachRole::kStrategy;

    check(!other.attach(nullptr, kStorageBytes, config, strategy), "자리가 없으면 안 붙는다");
    check(!other.attach(g_storage + 8, kStorageBytes - 8, config, strategy), "경계가 어긋나면 안 붙는다");
    check(!other.attach(g_small_storage, kSmallStorageBytes, config, strategy), "구역이 작으면 안 붙는다");
    check(!other.last_error().empty(), "거절 사유가 남는다");

    ipc::SharedLayoutConfig wider_lanes = config;
    wider_lanes.feed_lanes              = config.feed_lanes + 1;
    check(!other.attach(g_storage, kStorageBytes, wider_lanes, strategy), "시세 줄 수가 다르면 안 붙는다");

    ipc::SharedLayoutConfig wider_symbols = config;
    wider_symbols.symbol_capacity        = config.symbol_capacity * 2;
    check(!other.attach(g_storage, kStorageBytes, wider_symbols, strategy), "종목 수가 다르면 안 붙는다");

    ipc::SharedLayoutConfig wider_requests = config;
    wider_requests.request_capacity        = config.request_capacity * 2;
    check(!other.attach(g_storage, kStorageBytes, wider_requests, strategy), "요청 칸 수가 다르면 안 붙는다");

    ipc::SharedLayoutConfig wider_watches = config;
    wider_watches.feed_control_capacity   = config.feed_control_capacity * 2;
    check(!other.attach(g_storage, kStorageBytes, wider_watches, strategy), "구독 칸 수가 다르면 안 붙는다");

    ipc::SharedLayoutConfig wider_fills = config;
    wider_fills.fill_capacity           = config.fill_capacity * 2;
    check(!other.attach(g_storage, kStorageBytes, wider_fills, strategy), "체결통보 칸 수가 다르면 안 붙는다");

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

    ipc::SharedLayoutConfig no_fill = config;
    no_fill.fill_capacity           = 0;
    check(!other.create(g_storage, kStorageBytes, no_fill), "체결통보 칸 0은 못 놓는다");

    check(!other.is_bound(), "거절당한 손잡이는 안 붙은 상태로 남는다");
    check(other.attach(g_storage, kStorageBytes, config, strategy), "같은 값이면 붙는다");
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
    check(ipc::kSharedLayoutVersion == 6, "이 단계의 판 번호는 6이다 — 올릴 때 이 줄도 같이 본다");
    check(head->symbol_capacity == config.symbol_capacity, "놓는 쪽이 종목 수를 적는다");
    check(head->feed_control_capacity == config.feed_control_capacity, "놓는 쪽이 구독 칸 수를 적는다");
    check(head->fill_capacity == config.fill_capacity, "놓는 쪽이 체결통보 칸 수를 적는다");

    ipc::SharedLayout       other;
    const ipc::SharedAttachRole strategy = ipc::SharedAttachRole::kStrategy;

    // 시세 줄이 **적을** 때 — 뒤따르는 면이 통째로 앞당겨진다. 구역 크기로는 안 걸린다(더 작게 든다).
    ipc::SharedLayoutConfig fewer_lanes = config;
    fewer_lanes.feed_lanes              = config.feed_lanes - 1;
    check(!other.attach(g_storage, kStorageBytes, fewer_lanes, strategy), "시세 줄이 적어도 안 붙는다");

    ipc::SharedLayoutConfig smaller_trades = config;
    smaller_trades.feed_trade_capacity    = config.feed_trade_capacity / 2;
    check(!other.attach(g_storage, kStorageBytes, smaller_trades, strategy), "체결 칸 수가 적어도 안 붙는다");

    const uint32_t saved_version = head->layout_version;
    head->layout_version         = saved_version + 1;
    check(!other.attach(g_storage, kStorageBytes, config, strategy), "판 번호가 크면 안 붙는다");

    // 판 3짜리 구역(단계 4의 exe가 놓아 둔 것)에 판 4가 붙으려 하면 거절해야 한다. 면이 둘 늘어
    //  박동부터 뒤가 통째로 밀려 있어, 붙으면 장부 사본을 엉뚱한 바이트에서 읽는다. [why D-114]
    head->layout_version = 3;
    check(!other.attach(g_storage, kStorageBytes, config, strategy), "판 3 구역에는 판 4가 안 붙는다");
    check(!other.is_bound(), "판이 다르면 반쪽으로도 안 붙는다");
    check(!other.last_error().empty(), "판이 다르면 사유가 남는다");
    head->layout_version = saved_version;

    const uint32_t saved_magic = head->magic;
    head->magic                = 0;
    check(!other.attach(g_storage, kStorageBytes, config, strategy), "표식이 깨지면 안 붙는다");
    check(!other.is_bound(), "표식이 깨졌으면 반쪽으로도 안 붙는다");
    head->magic = saved_magic;

    check(other.attach(g_storage, kStorageBytes, config, strategy), "머리를 되돌리면 붙는다");
}

} // namespace

int main()
{
    std::cout << "=== 공유 쪽지 위 자리표 (D-114 단계 4·5) ===\n";
    test_size_math();
    test_three_sides_see_the_same();
    test_roles_keep_their_end();
    test_faces_do_not_overlap();
    test_attach_keeps_values();
    test_refusals();
    test_head_guards();
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
