// tests/test_market_feed_channel.cpp
// 주문 → 전략 시세 통로 검증 (D-114 단계 4) — 소켓을 쥔 쪽이 디코드한 체결·호가가 건너편에 그대로 닿는가.
//
//   ① 자리 셈이 실제 배치와 맞는가(줄마다 큐 둘, 캐시라인 올림 포함)
//   ② 보낸 순서 그대로 나오는가 — 한 종목의 순서가 지켜지는 것이 이 통로의 조건이다(원칙 2)
//   ③ 줄이 갈리는가(소켓 하나의 시세가 다른 줄로 새지 않는가)
//   ④ 머리가 다르면 붙지 않는가(옛 exe가 새 배치에 붙는 길이 없는가)
//   ⑤ 큐가 차면 **기다리지 않고** 버리고 세는가(원칙 3), 받는 쪽이 비우면 다시 들어가는가
//   ⑥ 칸이 망가졌을 때 그 값으로 전략이 판단하지 않는가 — 버리고 세고 다음 칸을 보는가
//   ⑦ 보내는 스레드·받는 스레드가 같이 돌 때 건수와 순서가 맞는가
//
//   사용법: test_market_feed_channel

#include "ipc/MarketFeedChannel.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        // abort 는 cout 버퍼를 비우지 않는다 — 파이프·파일로 받으면 실패 줄이 통째로 사라져 "아무것도
        //  못 찍고 죽은" 것처럼 보인다(09-23 이 시험을 쫓다 반나절 헛짚을 뻔했다). 흘려보내고 죽는다.
        std::cout << "[FAIL] " << name << std::endl;
        std::abort();
    }

    std::cout << "[PASS] " << name << "\n";
}

constexpr size_t   kStorageBytes    = 256 * 1024;
constexpr size_t   kTradeCapacity   = 16;
constexpr size_t   kBookCapacity    = 8;
constexpr uint32_t kLanes           = 2;
constexpr uint32_t kSymbolCount     = 64;

alignas(ipc::kSharedCacheLine) std::byte g_storage[kStorageBytes];

ipc::MarketLimits limits()
{
    ipc::MarketLimits value;
    value.symbol_count = kSymbolCount;
    return value;
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

OrderBook order_book_of(symbol::SymbolId symbol_id, double best_ask)
{
    OrderBook order_book;
    order_book.ticker    = "005930";
    order_book.symbol_id = symbol_id;
    order_book.hhmmss    = 93001;

    for (int level = 0; level < 5; ++level)
    {
        order_book.asks[level].price    = best_ask + level * 100.0;
        order_book.asks[level].quantity = 10 + level;
        order_book.bids[level].price    = best_ask - (level + 1) * 100.0;
        order_book.bids[level].quantity = 20 + level;
    }

    order_book.received_ns = 12345;
    return order_book;
}

// 공유 바이트 위 체결 칸 하나를 직접 짚는다 — 건너편이 망가졌을 때를 흉내 낸다.
TradeData& raw_trade_slot(uint32_t lane, size_t index)
{
    using Ring = ipc::SharedSpscRing<TradeData>;
    using Book = ipc::SharedSpscRing<OrderBook>;

    const size_t trade_span = (Ring::bytes_for(kTradeCapacity) + ipc::kSharedCacheLine - 1) /
                              ipc::kSharedCacheLine * ipc::kSharedCacheLine;
    const size_t book_span = (Book::bytes_for(kBookCapacity) + ipc::kSharedCacheLine - 1) /
                             ipc::kSharedCacheLine * ipc::kSharedCacheLine;

    std::byte* lane_base = g_storage + lane * (trade_span + book_span);
    auto*      slots     = reinterpret_cast<Ring::Slot*>(lane_base + sizeof(ipc::SharedRingControl));
    return slots[index].record;
}

void test_size_math()
{
    const size_t one_lane = ipc::MarketFeedChannel::bytes_for(1, kTradeCapacity, kBookCapacity);
    const size_t two_lane = ipc::MarketFeedChannel::bytes_for(2, kTradeCapacity, kBookCapacity);

    check(two_lane == one_lane * 2, "줄 수에 비례한다");
    check(one_lane % ipc::kSharedCacheLine == 0, "줄 하나가 캐시라인 배수다");
    check(one_lane >= ipc::SharedSpscRing<TradeData>::bytes_for(kTradeCapacity) +
                          ipc::SharedSpscRing<OrderBook>::bytes_for(kBookCapacity),
          "큐 둘이 들어간다");
    check(ipc::MarketFeedChannel::bytes_for(kLanes, kTradeCapacity, kBookCapacity) < kStorageBytes,
          "시험 구역에 든다");
}

void test_round_trip()
{
    ipc::MarketFeedChannel sender;
    ipc::MarketFeedChannel receiver;

    check(sender.create(g_storage, kStorageBytes, kLanes, kTradeCapacity, kBookCapacity), "놓기");
    check(receiver.attach(g_storage, kStorageBytes, kLanes,
                          ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "붙기");
    check(sender.lanes() == kLanes && receiver.is_bound(), "줄 수");

    for (int64_t quantity = 1; quantity <= 5; ++quantity)
    {
        check(sender.push_trade(0, trade_of(7, quantity)), "체결 보내기");
    }

    check(sender.push_order_book(0, order_book_of(7, 70100.0)), "호가 보내기");

    TradeData received;

    for (int64_t quantity = 1; quantity <= 5; ++quantity)
    {
        check(receiver.pop_trade(0, limits(), received), "체결 받기");
        check(received.quantity == quantity, "보낸 순서 그대로 나온다");
        check(received.symbol_id == 7 && received.ticker.view() == "005930", "값이 그대로다");
    }

    check(!receiver.pop_trade(0, limits(), received), "다 읽으면 빈 큐");

    OrderBook book;
    check(receiver.pop_order_book(0, limits(), book), "호가 받기");
    check(book.asks[0].price == 70100.0 && book.bids[0].quantity == 20, "호가 다섯 단계가 그대로다");
    check(!receiver.pop_order_book(0, limits(), book), "호가도 다 읽으면 빈 큐");
    check(receiver.discarded() == 0, "버린 칸이 없다");

    // 넘어간 건수는 링 순번을 그대로 읽고, 그 순번은 공유 칸에 있다 — 어느 손잡이로 물어도 같은 답이다.
    //  한쪽 프로세스만 보고도 통로가 도는지 알 수 있다. [why D-114 단계 5]
    check(sender.sent_trades() == 5 && sender.sent_order_books() == 1, "보낸 수를 센다");
    check(receiver.received_trades() == 5 && receiver.received_order_books() == 1, "꺼낸 수를 센다");
    check(sender.received_trades() == 5 && sender.received_order_books() == 1, "보낸 쪽에서도 꺼낸 수가 보인다");
    check(receiver.sent_trades() == 5 && receiver.sent_order_books() == 1, "받은 쪽에서도 보낸 수가 보인다");
}

void test_lanes_are_separate()
{
    ipc::MarketFeedChannel sender;
    ipc::MarketFeedChannel receiver;

    check(sender.create(g_storage, kStorageBytes, kLanes, kTradeCapacity, kBookCapacity), "놓기(줄 가르기)");
    check(receiver.attach(g_storage, kStorageBytes, kLanes,
                          ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "붙기(줄 가르기)");

    check(sender.push_trade(1, trade_of(9, 33)), "둘째 줄에 보내기");

    TradeData received;
    check(!receiver.pop_trade(0, limits(), received), "첫째 줄은 비어 있다");
    check(receiver.pop_trade(1, limits(), received), "둘째 줄에서 나온다");
    check(received.quantity == 33, "둘째 줄 값");

    check(!sender.push_trade(kLanes, trade_of(9, 1)), "없는 줄에는 못 보낸다");
    check(!receiver.pop_trade(kLanes, limits(), received), "없는 줄에서는 못 꺼낸다");
}

void test_attach_refusals()
{
    ipc::MarketFeedChannel sender;

    check(sender.create(g_storage, kStorageBytes, kLanes, kTradeCapacity, kBookCapacity), "놓기(붙기 거절 시험)");

    ipc::MarketFeedChannel other;

    check(!other.attach(g_storage, kStorageBytes, kLanes,
                        ipc::RingEndpoint::kConsumer, kTradeCapacity * 2, kBookCapacity),
          "체결 칸 수가 다르면 안 붙는다");
    check(!other.attach(g_storage, kStorageBytes, kLanes,
                        ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity * 2),
          "호가 칸 수가 다르면 안 붙는다");
    check(!other.attach(g_storage, 64, kLanes,
                        ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "구역이 작으면 안 붙는다");
    check(!other.attach(g_storage + 8, kStorageBytes - 8, kLanes,
                        ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity),
          "경계가 어긋나면 안 붙는다");
    check(!other.attach(nullptr, kStorageBytes, kLanes,
                        ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "자리가 없으면 안 붙는다");
    check(!other.attach(g_storage, kStorageBytes, 0,
                        ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "줄 0은 안 붙는다");
    check(!other.attach(g_storage, kStorageBytes, ipc::kMaxFeedLanes + 1,
                        ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity),
          "줄 상한을 넘으면 안 붙는다");
    check(!other.last_error().empty(), "거절 사유가 남는다");
    check(other.attach(g_storage, kStorageBytes, kLanes,
                       ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "머리가 같으면 붙는다");

    // 둘째 줄 머리를 망가뜨리면 그 줄에서 걸린다 — 첫째 줄만 보고 통과시키지 않는다.
    auto* second = reinterpret_cast<ipc::SharedRingControl*>(
        g_storage + ipc::MarketFeedChannel::bytes_for(1, kTradeCapacity, kBookCapacity));
    second->magic = 0;

    ipc::MarketFeedChannel late;
    check(!late.attach(g_storage, kStorageBytes, kLanes,
                       ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "표식이 깨지면 안 붙는다");
    check(!late.is_bound(), "반쪽으로 붙어 있지 않다");

    late.unbind();
    check(!late.push_trade(0, trade_of(7, 1)), "안 붙었으면 못 보낸다");
}

void test_overflow_is_counted()
{
    ipc::MarketFeedChannel sender;
    ipc::MarketFeedChannel receiver;

    check(sender.create(g_storage, kStorageBytes, kLanes, kTradeCapacity, kBookCapacity), "놓기(넘침 시험)");
    check(receiver.attach(g_storage, kStorageBytes, kLanes,
                          ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "붙기(넘침 시험)");

    for (size_t index = 0; index < kTradeCapacity; ++index)
    {
        check(sender.push_trade(0, trade_of(7, 1)), "칸이 있으면 들어간다");
    }

    check(!sender.push_trade(0, trade_of(7, 1)), "가득 차면 거짓 — 기다리지 않는다");
    check(sender.overflow_trades() == 1, "버린 수를 센다");

    TradeData received;
    check(receiver.pop_trade(0, limits(), received), "받는 쪽이 한 칸 비운다");
    check(sender.push_trade(0, trade_of(7, 2)), "비운 만큼 다시 들어간다");
    check(sender.overflow_trades() == 1, "성공한 건은 넘침으로 세지 않는다");
}

void test_broken_slots_are_discarded()
{
    ipc::MarketFeedChannel sender;
    ipc::MarketFeedChannel receiver;

    check(sender.create(g_storage, kStorageBytes, kLanes, kTradeCapacity, kBookCapacity), "놓기(망가진 칸 시험)");
    check(receiver.attach(g_storage, kStorageBytes, kLanes,
                          ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "붙기(망가진 칸 시험)");

    // 네 건을 보낸 뒤 앞의 셋을 서로 다른 방식으로 망가뜨린다. 넷째는 성한 것으로 둔다.
    for (int64_t quantity = 1; quantity <= 4; ++quantity)
    {
        check(sender.push_trade(0, trade_of(7, quantity)), "보내기(망가진 칸 시험)");
    }

    raw_trade_slot(0, 0).symbol_id = kSymbolCount + 1;                          // 표 밖 번호
    raw_trade_slot(0, 1).ticker.length = symbol::Ticker::kMax + 3;              // 칸을 넘는 종목 코드 길이
    raw_trade_slot(0, 2).price = std::numeric_limits<double>::quiet_NaN();      // 숫자가 아닌 가격

    TradeData received;
    check(receiver.pop_trade(0, limits(), received), "성한 칸이 나온다");
    check(received.quantity == 4, "망가진 셋을 건너뛰고 넷째가 나온다");
    check(receiver.discarded() == 3, "버린 칸을 센다");

    // 값 검사 자체도 따로 본다 — 부르는 쪽이 통로 없이 쓸 수 있어야 한다.
    TradeData zero_quantity = trade_of(7, 0);
    check(!ipc::is_plausible(zero_quantity, limits()), "수량 0은 체결이 아니다");

    TradeData bad_direction = trade_of(7, 1);
    bad_direction.direction = 3;
    check(!ipc::is_plausible(bad_direction, limits()), "방향은 0·1·5뿐이다");

    TradeData no_direction = trade_of(7, 1);
    no_direction.direction = 0;
    check(ipc::is_plausible(no_direction, limits()), "방향 0(안 찍음)은 받는다");

    OrderBook negative_quantity = order_book_of(7, 70100.0);
    negative_quantity.bids[2].quantity = -1;
    check(!ipc::is_plausible(negative_quantity, limits()), "잔량 음수는 호가가 아니다");

    OrderBook empty_level = order_book_of(7, 70100.0);
    empty_level.asks[4].price    = 0.0;
    empty_level.asks[4].quantity = 0;
    check(ipc::is_plausible(empty_level, limits()), "빈 단계(0)는 받는다");
}

void test_two_threads()
{
    ipc::MarketFeedChannel sender;
    ipc::MarketFeedChannel receiver;

    check(sender.create(g_storage, kStorageBytes, kLanes, kTradeCapacity, kBookCapacity), "놓기(두 스레드)");
    check(receiver.attach(g_storage, kStorageBytes, kLanes,
                          ipc::RingEndpoint::kConsumer, kTradeCapacity, kBookCapacity), "붙기(두 스레드)");

    constexpr int64_t kTotal = 20000;

    // 스레드를 띄우기 전에 큐를 꽉 채운다 — 받는 쪽이 늘 앞서 비우면 한 번도 안 차서, 빈 큐만 오가는
    //  시험이 된다. 예전엔 그것을 "dropped > 0"으로 잡으려 했는데 그 수 자체가 타이밍에 달려 있어
    //  스무 번에 두어 번 그 자리에서 죽었다(09-23). 가득 찬 상태는 여기서 손으로 만든다.
    bool prefilled = true;

    for (int64_t quantity = 1; quantity <= static_cast<int64_t>(kTradeCapacity); ++quantity)
    {
        prefilled = prefilled && sender.push_trade(0, trade_of(7, quantity));
    }

    check(prefilled, "큐를 칸 수만큼 채운다");
    check(!sender.push_trade(0, trade_of(7, kTradeCapacity + 1)), "꽉 찬 큐는 더 받지 않는다(그 건은 버려진다)");

    std::atomic<int64_t> dropped{0};

    std::thread producer([&sender, &dropped] {
        // 미리 채운 칸 뒤부터 — 버려진 건은 큐에 안 들어갔으니 순번은 이어진다.
        for (int64_t quantity = static_cast<int64_t>(kTradeCapacity) + 1; quantity <= kTotal; ++quantity)
        {
            // 큐가 차면 버리는 것이 이 통로의 규칙이지만, 건수를 맞춰 보려고 여기서는 빌 때까지 다시 낸다.
            while (!sender.push_trade(0, trade_of(7, quantity)))
            {
                dropped.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    });

    int64_t   seen        = 0;
    int64_t   out_of_turn = 0;
    TradeData received;

    // 보내는 쪽이 "끝났다"고 알린 순간에 큐가 빈 것은 아니다 — 마지막 칸의 도장이 이 스레드에 아직
    //  안 보였을 수 있고, 거기서 멈추면 몇 건을 놓고 끝난다(09-23 스물다섯 번에 네 번 그랬다).
    //  보낸 수를 다 받을 때까지 돌되, 안 오면 기한에서 끊어 판정으로 넘긴다.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);

    while (seen < kTotal && std::chrono::steady_clock::now() < deadline)
    {
        if (receiver.pop_trade(0, limits(), received))
        {
            ++seen;

            if (received.quantity != seen)
            {
                ++out_of_turn;
            }

            continue;
        }

        std::this_thread::yield();
    }

    producer.join();

    check(seen == kTotal, "보낸 건수만큼 받는다");
    check(out_of_turn == 0, "순서가 뒤집히지 않았다");
    check(receiver.discarded() == 0, "반쪽 레코드를 본 적이 없다");
    check(receiver.stamp_out_of_turn() == 0, "덮인 칸이 없다");

    // 가득 찬 큐를 지나왔다는 것은 위에서 손으로 확인했다. 여기서는 그 뒤 얼마나 더 막혔는지만 남긴다 —
    //  수 자체는 기계 사정에 따라 0일 수 있어 판정에 쓰지 않는다.
    std::cout << "[INFO] 보내는 쪽이 큐가 차서 다시 낸 횟수: " << dropped.load() << "\n";
}

} // namespace

int main()
{
    std::cout << "=== 주문 → 전략 시세 통로 (D-114 단계 4) ===\n";
    test_size_math();
    test_round_trip();
    test_lanes_are_separate();
    test_attach_refusals();
    test_overflow_is_counted();
    test_broken_slots_are_discarded();
    test_two_threads();
    std::cout << "=== 전부 통과 (" << g_checks << " checks) ===\n";
    return 0;
}
