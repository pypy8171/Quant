// 공유 쪽지 한 장 위의 자리표 — 면 열이 어디서 시작해 몇 바이트를 쓰는지 이 파일 하나에서만 정한다.
//  프로세스가 같은 바이트를 서로 다른 자리로 읽는 것이 갈라진 뒤 제일 잡기 어려운 결함이다(전부
//  멀쩡히 돌면서 값만 어긋난다). 그래서 자리 셈을 역할마다 나눠 두지 않고 여기로 모았다. [why D-114]
//  [inv] 자리·차례·칸 수 기본값이 바뀌면 kSharedLayoutVersion을 올린다 — 옛 exe가 새 배치에 붙는 길을 막는다.
//  [inv] 놓는 쪽(create)은 주문 프로세스 하나, 붙는 쪽(attach)은 전략과 시세 둘이다.
#pragma once

#include "ipc/ControlChannel.h"
#include "ipc/FillChannel.h"
#include "ipc/Heartbeat.h"
#include "ipc/LedgerSnapshot.h"
#include "ipc/MarketFeedChannel.h"
#include "ipc/OrderChannel.h"
#include "ipc/SharedRegion.h"
#include "ipc/SharedSpscRing.h"
#include "ipc/SharedStrategyDictionary.h"
#include "ipc/SharedSymbolDictionary.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ipc
{

// 자리표 판 번호. SharedRegion::create·attach 의 layout_version 으로 그대로 넘긴다.
//  판 2 — 제어 요청에 전략 이름 칸이 붙었고(kRegisterStrategy) 전략 이름표 칸 수가 힙 표와 같아졌다.
//  판 3 — 구역 머리에 주인 기동 시각·기동 번호·종료 사유가 붙었다(SharedRegionHeader, D-114 단계 4-b).
//  판 4 — 시세가 제 프로세스로 갈렸다(D-114 단계 5). 면 둘(전략→시세 제어 줄·시세→주문 체결 통로)이 늘고,
//   박동 칸이 셋이 되고, 구역 머리에 붙은 쪽 자리 둘이 붙었다.
//  판 5 — 체결 레코드의 빈 칸(reserved2)이 실시간 세션 번호가 됐다. 크기는 그대로지만 옛 판이 보낸 0을
//   "모두 같은 세션"으로 읽으면 재연결 뒤 재전송을 못 가른다(OrderRouter::on_fill).
constexpr uint32_t kSharedLayoutVersion = 5;

// 칸 수 기본값 — 한 프로세스로 돌던 때 쓰던 값과 같다(Engine::ShardPipeline). 여기서 바꾸면 양쪽이 같이 바뀐다.
constexpr size_t kLayoutRequestCapacity  = 1024; // 요청 하나에 답 하나라 응답과 같은 수다
constexpr size_t kLayoutResponseCapacity = 1024;
constexpr size_t kLayoutControlCapacity  = 8192; // 표 한 장이 줄 2,048개까지라 그 네 배를 둔다
// 전략 이름표 칸 수 — 힙 표(strategy_table::kDefaultCapacity)와 같은 수여야 한다. 갈라 띄우면 엔진이
//  힙 표 대신 이 표를 꽂는데, 칸 수가 다르면 상한을 보는 자리(ipc::RequestLimits)가 역할마다 달라진다.
constexpr size_t kLayoutStrategyCapacity = strategy_table::kDefaultCapacity;

// 전략 → 시세 제어 줄 칸 수. 여기에는 구독·해지만 실린다 — 세션 하나가 쥘 수 있는 종목이 마흔이라
//  주문 쪽 제어 줄(표 한 장이 줄 2,048개)만큼 잡을 까닭이 없다. 하루치 구독 갈이를 다 받고도 남는다.
constexpr size_t kLayoutFeedControlCapacity = 1024;

// 시세 → 주문 체결 통로 칸 수. FillChannel 이 정한 수를 그대로 쓴다(kFillCapacity).
constexpr size_t kLayoutFillCapacity = kFillCapacity;

// 자리표 머리 — 면 여덟 앞에 둔다. 양쪽이 넘긴 설정이 한 칸이라도 다르면 붙기를 여기서 거절한다.
//  머리가 없으면 어긋난 설정이 "면의 머리가 우연히 안 맞아서" 걸리는 데 기대게 된다 — 값 하나가 우연히
//  맞는 날 두 프로세스가 서로 다른 자리를 같은 자리로 알고 돈다. [why D-114]
//  [inv] 고정 크기 정수만 둔다. 칸을 더할 때는 reserved를 쓰고 kSharedLayoutVersion을 올린다.
struct SharedLayoutHead
{
    uint32_t magic                    = 0; // kSharedLayoutMagic
    uint32_t layout_version           = 0;
    uint32_t feed_lanes               = 0;
    uint32_t reserved0                = 0;
    uint64_t symbol_capacity          = 0;
    uint64_t strategy_capacity        = 0;
    uint64_t request_capacity         = 0;
    uint64_t response_capacity        = 0;
    uint64_t control_capacity         = 0;
    uint64_t feed_trade_capacity      = 0;
    uint64_t feed_order_book_capacity = 0;
    uint64_t feed_control_capacity    = 0; // 판 3까지 reserved1 이던 칸
    uint64_t fill_capacity            = 0; // 판 3까지 reserved2 이던 칸
    uint64_t reserved3                = 0;
    uint64_t reserved4                = 0;
    uint64_t reserved5                = 0;
    uint64_t reserved6                = 0;
    uint64_t reserved7                = 0;
};

constexpr uint32_t kSharedLayoutMagic = 0x51'4C'41'59; // 'QLAY'

// 머리는 캐시라인 두 줄이다 — 뒤따르는 첫 면이 경계에서 시작한다.
static_assert(sizeof(SharedLayoutHead) == 2 * kSharedCacheLine, "자리표 머리는 캐시라인 두 줄이어야 한다");

// 박동 셋. 서로 다른 캐시라인에 둔다 — 한 줄에 같이 두면 한쪽이 찍을 때마다 남의 줄이 무효가 되어,
//  박동을 보는 값싼 일이 셋을 계속 밀고 당기는 일이 된다.
struct SharedHeartbeats
{
    alignas(kSharedCacheLine) Heartbeat strategy; // 전략 프로세스가 찍고 나머지가 본다
    alignas(kSharedCacheLine) Heartbeat order;    // 주문 프로세스가 찍고 나머지가 본다
    alignas(kSharedCacheLine) Heartbeat feed;     // 시세 프로세스가 찍고 나머지가 본다 [why D-114]
};

// 면 열의 크기를 정하는 값들. 붙는 쪽 전부가 **같은 값**을 넘겨야 같은 자리를 본다 — 다르면 붙기가 거절한다.
struct SharedLayoutConfig
{
    uint32_t feed_lanes               = 1; // 시세 줄 수 = 소켓 수 + REST 대체 줄 하나(줄 하나에 큐 둘)
    size_t   symbol_capacity          = symbol::kDefaultSymbolCapacity;
    size_t   strategy_capacity        = kLayoutStrategyCapacity;
    size_t   request_capacity         = kLayoutRequestCapacity;
    size_t   response_capacity        = kLayoutResponseCapacity;
    size_t   control_capacity         = kLayoutControlCapacity;
    size_t   feed_trade_capacity      = kFeedTradeCapacity;
    size_t   feed_order_book_capacity = kFeedOrderBookCapacity;
    size_t   feed_control_capacity    = kLayoutFeedControlCapacity;
    size_t   fill_capacity            = kLayoutFillCapacity;
};

// 쪽지 위에 머리 하나와 면 열을 이 차례로 놓는다: 머리 → 요청 큐 → 응답 큐 → 제어 큐(주문) →
//  제어 큐(시세) → 체결 통로 → 박동 → 시세 통로 → 종목 표 → 전략 이름표 → 장부 사본.
//  면마다 캐시라인 경계에서 시작한다.
//  [inv] 제어 큐 둘 다 SPSC다 — 보내는 쪽은 둘 다 전략 하나이고 받는 쪽만 다르다. 전략 프로세스 안에서
//   여러 스레드가 만든 제어 요청은 전략 스레드 하나가 모아서 줄에 옮긴다(원칙 5: 경계를 넘는 줄의
//   생산자는 하나로 만든다). 어느 낱말이 어느 줄로 가는지는 ipc::routes_to_feed 하나가 정한다.
class SharedLayout
{
public:
    // 머리와 면 여덟을 담는 데 드는 바이트. SharedRegion 머리는 포함하지 않는다 — payload_bytes()와 견준다.
    [[nodiscard]] static size_t bytes_for(const SharedLayoutConfig& config);

    // 주문 프로세스가 부른다. 머리와 면마다 새 머리를 적고 칸을 0으로 민다.
    [[nodiscard]] bool create(std::byte* base, size_t bytes, const SharedLayoutConfig& config);

    // 붙는 쪽(전략·시세)이 부른다. 자리표 머리의 설정을 한 칸씩 대조하고 안 맞으면 붙지 않는다.
    //  role 로 면마다 제 끝을 고른다 — 제 끝이 아닌 면은 구경만 하고 공유 칸에 아무것도 안 적는다.
    [[nodiscard]] bool attach(std::byte* base, size_t bytes, const SharedLayoutConfig& config,
                              SharedAttachRole role);

    void unbind() noexcept;

    [[nodiscard]] bool is_bound() const noexcept
    {
        return ledger_ != nullptr;
    }

    [[nodiscard]] SharedSpscRing<OrderRequest>& requests() noexcept
    {
        return requests_;
    }

    [[nodiscard]] SharedSpscRing<OrderResponse>& responses() noexcept
    {
        return responses_;
    }

    // 전략 → 주문 제어 줄.
    [[nodiscard]] SharedSpscRing<ControlRequest>& controls() noexcept
    {
        return controls_;
    }

    // 전략 → 시세 제어 줄. 구독·해지만 여기로 간다(ipc::routes_to_feed).
    [[nodiscard]] SharedSpscRing<ControlRequest>& feed_controls() noexcept
    {
        return feed_controls_;
    }

    // 시세 → 주문 체결통보 통로.
    [[nodiscard]] FillChannel& fills() noexcept
    {
        return fills_;
    }

    [[nodiscard]] MarketFeedChannel& feed() noexcept
    {
        return feed_;
    }

    [[nodiscard]] SharedSymbolDictionary& symbols() noexcept
    {
        return symbols_;
    }

    [[nodiscard]] SharedStrategyDictionary& strategies() noexcept
    {
        return strategies_;
    }

    // 붙지 않았으면 nullptr이다. [inv] 돌려주는 참조의 수명은 unbind()까지다.
    [[nodiscard]] SharedHeartbeats* heartbeats() noexcept
    {
        return heartbeats_;
    }

    [[nodiscard]] LedgerSnapshot* ledger() noexcept
    {
        return ledger_;
    }

    // 마지막 실패 사유 — 로그에 그대로 싣는다. 성공하면 빈 문자열이다.
    [[nodiscard]] std::string_view last_error() const noexcept
    {
        return last_error_;
    }

private:
    // 자리표 머리에 지금 설정을 적는다(놓는 쪽) / 적힌 설정과 지금 설정을 대조한다(붙는 쪽).
    [[nodiscard]] bool bind_head(std::byte* base, const SharedLayoutConfig& config, bool as_owner);

    // 값이 말이 되는가(칸 수 0, 줄 수 상한, 종목 수가 장부 사본 상한을 넘는지). 아니면 사유를 남기고 거짓.
    [[nodiscard]] bool check_config(const std::byte* base, size_t bytes, const SharedLayoutConfig& config);

    // 면 열을 차례로 놓거나 붙인다. 중간에 하나라도 실패하면 전부 떼고 거짓을 준다 —
    //  반만 붙은 자리표를 들고 있지 않는다. as_owner면 role 은 안 본다.
    [[nodiscard]] bool bind(std::byte* base, const SharedLayoutConfig& config, bool as_owner, SharedAttachRole role);

    SharedSpscRing<OrderRequest>   requests_;
    SharedSpscRing<OrderResponse>  responses_;
    SharedSpscRing<ControlRequest> controls_;
    SharedSpscRing<ControlRequest> feed_controls_;
    FillChannel                    fills_;
    MarketFeedChannel              feed_;
    SharedSymbolDictionary         symbols_;
    SharedStrategyDictionary       strategies_;
    SharedLayoutHead*              head_       = nullptr;
    SharedHeartbeats*              heartbeats_ = nullptr;
    LedgerSnapshot*                ledger_     = nullptr;
    std::string                    last_error_;
};

} // namespace ipc
