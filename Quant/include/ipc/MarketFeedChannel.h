// 시세 → 전략 시세 통로 — 소켓 한 줄이 나르는 체결·호가 두 큐 한 벌.
//  실시간 세션이 하나라(앱키 하나) 그 소켓은 **시세 프로세스가 쥔다**. 전략은 제 소켓이 없으므로
//  수신 스레드가 디코드한 체결·호가를 이 통로로 받는다. REST 넘침 폴링도 시세 프로세스에 있어 그 줄의
//  보내는 쪽 또한 시세다. 일봉은 여기를 안 지난다 — 데이터 스레드 몸통이 전략 프로세스에 있다. [why D-114]
//
//  줄(lane) 하나가 소켓 하나다. 한 종목은 소켓 하나에만 있으므로(FeedMux) 그 종목의 체결은 큐 하나만
//  지나고 순서가 지켜진다(원칙 2). 체결과 호가를 따로 둔 것은 지금 행렬(shard::Matrix)이 그렇기 때문이고,
//  한 봉투에 담으면 80바이트짜리 체결이 200바이트 호가 칸을 쓰게 된다.
//  [inv] 한 줄의 보내는 쪽은 시세 프로세스의 그 소켓 수신 스레드 하나, 받는 쪽은 전략 프로세스의 그 줄 담당
//  스레드 하나다. 쪽지를 만드는 쪽(주문)은 이 통로의 양끝 어느 쪽도 아니다 — 자리만 놓고 지나간다.
//  보내는 쪽은 기다리지 않는다 — 큐가 차면 버리고 센다(원칙 3).
#pragma once

#include "core/Types.h" // TradeData, OrderBook, symbol::SymbolId
#include "ipc/SharedSpscRing.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ipc
{

// 줄 하나가 쓰는 칸 수. 체결이 호가보다 잦아 더 크게 잡는다. 받는 쪽은 꺼내서 행렬에 옮기기만 하는 얇은
//  고리라 오래 밀리지 않지만, 전략 프로세스가 한 박자 쉬는 동안(재스캔·GC 없는 C++라도 페이지 폴트) 오는
//  것을 받아 둘 만큼은 있어야 한다.
constexpr size_t kFeedTradeCapacity     = 16384;
constexpr size_t kFeedOrderBookCapacity = 8192;

// 줄 수 상한. 소켓 수와 같다 — 이 수만큼 큐 쌍이 공유 쪽지를 차지하므로 무한정 두지 않는다.
constexpr uint32_t kMaxFeedLanes = 16;

// 꺼낸 시세가 말이 되는지 보는 기준. 종목 표는 장중에도 늘어나므로 받는 쪽이 꺼내는 회차마다 지금 든 수로
//  다시 채운다.
struct MarketLimits
{
    uint32_t symbol_count = 0;             // 종목 표에 지금 든 수. id는 1부터 이 수까지다(0은 없음)
    int64_t  quantity_max = 1'000'000'000; // 한 건 최대 수량(체결·호가 잔량)
    double   price_max    = 100'000'000.0; // 한 주 최대 가격(원)
};

// 큐에서 꺼낸 체결이 전략에 들어가도 되는가. 건너편이 망가졌거나 칸이 덮였을 때 그 값으로 전략이 판단하지
//  않으려고 여기를 지나게 한다. 거짓이면 버리고 센다. [why D-114]
[[nodiscard]] bool is_plausible(const TradeData& trade, const MarketLimits& limits) noexcept;

// 큐에서 꺼낸 호가가 말이 되는가. 다섯 단계 값과 종목 코드 길이까지 본다 — 길이가 칸을 넘으면 읽다가 칸 밖을 짚는다.
[[nodiscard]] bool is_plausible(const OrderBook& order_book, const MarketLimits& limits) noexcept;

// 줄마다 체결 큐·호가 큐를 한 쌍씩 들고 있는 손잡이. 한 인스턴스가 한쪽 끝만 맡는다 —
//  create 는 쪽지를 만드는 쪽(주문 프로세스)이 자리를 놓느라 한 번 부르고, attach 는 보내는 쪽(시세)과
//  받는 쪽(전략)이 각각 제 끝을 적어 부른다. 한 프로세스로 돌 때는 create 하나가 양끝을 다 맡는다.
class MarketFeedChannel
{
public:
    MarketFeedChannel()                                    = default;
    MarketFeedChannel(const MarketFeedChannel&)            = delete;
    MarketFeedChannel& operator=(const MarketFeedChannel&) = delete;

    // 줄 lanes개가 차지하는 바이트. 큐마다 캐시라인 경계에서 시작하도록 올림한 값이다.
    [[nodiscard]] static size_t bytes_for(uint32_t lanes, size_t trade_capacity = kFeedTradeCapacity,
                                          size_t order_book_capacity = kFeedOrderBookCapacity);

    // 통로를 새로 놓는다(쪽지를 만드는 쪽이 한 번 부른다). base는 캐시라인 경계여야 한다.
    [[nodiscard]] bool create(std::byte* base, size_t bytes, uint32_t lanes,
                              size_t trade_capacity      = kFeedTradeCapacity,
                              size_t order_book_capacity = kFeedOrderBookCapacity);

    // 이미 놓인 통로에 붙는다. 큐 머리가 하나라도 다르면 붙지 않는다.
    //  endpoint 는 이 손잡이가 맡는 끝이다 — 시세는 RingEndpoint::kProducer, 전략은 kConsumer 로 붙는다.
    //  받는 끝으로 붙은 손잡이만 공유 칸의 받은 자리를 적는다(SharedSpscRing::attach). [why D-114]
    [[nodiscard]] bool attach(std::byte* base, size_t bytes, uint32_t lanes, RingEndpoint endpoint,
                              size_t trade_capacity      = kFeedTradeCapacity,
                              size_t order_book_capacity = kFeedOrderBookCapacity);

    void unbind() noexcept;

    [[nodiscard]] bool is_bound() const noexcept
    {
        return lanes_ != 0;
    }

    [[nodiscard]] uint32_t lanes() const noexcept
    {
        return lanes_;
    }

    // 보내는 쪽. 거짓이면 그 건은 버려진다 — 큐가 찼으면 overflow_trades()·overflow_order_books()가 하나 늘고,
    //  lane이 범위 밖이면 세지 않는다. 수신 스레드는 기다리지 않는다.
    [[nodiscard]] bool push_trade(uint32_t lane, const TradeData& trade) noexcept;
    [[nodiscard]] bool push_order_book(uint32_t lane, const OrderBook& order_book) noexcept;

    // 받는 쪽. 참이면 out에 한 건을 채운다. 말이 안 되는 칸은 안에서 버리고 세므로(discarded_*) 부르는 쪽이
    //  검사를 잊을 수 없다 — 이 통로를 꺼내는 자리는 여기 하나뿐이다.
    [[nodiscard]] bool pop_trade(uint32_t lane, const MarketLimits& limits, TradeData& out) noexcept;
    [[nodiscard]] bool pop_order_book(uint32_t lane, const MarketLimits& limits, OrderBook& out) noexcept;

    // 큐가 차서 버린 수. 0이 아니면 전략 프로세스가 못 따라오고 있다는 뜻이다.
    [[nodiscard]] uint64_t overflow_trades() const noexcept
    {
        return overflow_trades_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t overflow_order_books() const noexcept
    {
        return overflow_order_books_.load(std::memory_order_relaxed);
    }

    // 값이 말이 안 돼 버린 수. 0이 아니면 건너편 프로세스를 의심한다.
    [[nodiscard]] uint64_t discarded() const noexcept
    {
        return discarded_.load(std::memory_order_relaxed);
    }

    // 통로로 밀어 넣은 건수·통로에서 꺼낸 건수의 줄 합. 늘리는 계수기를 따로 두지 않는다 — 링이 순번으로
    //  이미 세고 있어 그 순번을 그대로 읽는다(수신 스레드를 얇게 두는 원칙 3). 순번은 공유 칸에 있어 어느
    //  프로세스에서 물어도 같은 답이 온다 — 한쪽만 보고도 통로가 도는지 알 수 있다. [why D-114 단계 5]
    [[nodiscard]] uint64_t sent_trades() const;
    [[nodiscard]] uint64_t sent_order_books() const;
    [[nodiscard]] uint64_t received_trades() const;
    [[nodiscard]] uint64_t received_order_books() const;

    // 도장이 제 차례보다 앞서 있던 횟수의 합 — 칸이 덮였다는 뜻이다. 건강 판정이 이 수를 본다.
    [[nodiscard]] uint64_t stamp_out_of_turn() const;

    // 보내는 쪽이 보는 대기 칸 수(어림값). 적체 판정이 줄마다 본다.
    [[nodiscard]] size_t pending_trades(uint32_t lane) const;
    [[nodiscard]] size_t pending_order_books(uint32_t lane) const;

    [[nodiscard]] std::string_view last_error() const noexcept
    {
        return last_error_;
    }

private:
    // lanes·칸 수·구역 크기가 말이 되는지 본다. 아니면 last_error_에 사유를 남긴다.
    [[nodiscard]] bool check_arguments(const std::byte* base, size_t bytes, uint32_t lanes, size_t trade_capacity,
                                       size_t order_book_capacity);

    // 줄마다 체결 큐·호가 큐를 차례로 놓는다. as_owner면 새로 놓고, 아니면 endpoint 끝으로 붙는다.
    [[nodiscard]] bool bind(std::byte* base, uint32_t lanes, size_t trade_capacity, size_t order_book_capacity,
                            bool as_owner, RingEndpoint endpoint);

    std::vector<SharedSpscRing<TradeData>> trades_;
    std::vector<SharedSpscRing<OrderBook>> order_books_;
    uint32_t                               lanes_                = 0;

    // 줄이 여럿이면 세는 스레드도 여럿이다(보내는 쪽은 소켓 수신 스레드, 받는 쪽은 줄 스레드) — 감시 스레드가
    //  같은 값을 읽어 로그에 싣는 자리라 원자로 센다. 실패했을 때만 오르므로 hot path 비용은 없다. [why D-114]
    std::atomic<uint64_t>                  overflow_trades_      = 0;
    std::atomic<uint64_t>                  overflow_order_books_ = 0;
    std::atomic<uint64_t>                  discarded_            = 0;
    std::string                            last_error_;
};

} // namespace ipc
