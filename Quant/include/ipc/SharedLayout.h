// 공유 쪽지 한 장 위의 자리표 — 면 여덟이 어디서 시작해 몇 바이트를 쓰는지 이 파일 하나에서만 정한다.
//  두 프로세스가 같은 바이트를 서로 다른 자리로 읽는 것이 갈라진 뒤 제일 잡기 어려운 결함이다(양쪽 다
//  멀쩡히 돌면서 값만 어긋난다). 그래서 자리 셈을 주문 쪽·전략 쪽에 나눠 두지 않고 여기로 모았다. [why D-114]
//  [inv] 자리·차례·칸 수 기본값이 바뀌면 kSharedLayoutVersion을 올린다 — 옛 exe가 새 배치에 붙는 길을 막는다.
//  [inv] 놓는 쪽(create)은 주문 프로세스 하나, 붙는 쪽(attach)은 전략 프로세스다.
#pragma once

#include "ipc/ControlChannel.h"
#include "ipc/Heartbeat.h"
#include "ipc/LedgerSnapshot.h"
#include "ipc/MarketFeedChannel.h"
#include "ipc/OrderChannel.h"
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
constexpr uint32_t kSharedLayoutVersion = 1;

// 칸 수 기본값 — 한 프로세스로 돌던 때 쓰던 값과 같다(Engine::ShardPipeline). 여기서 바꾸면 양쪽이 같이 바뀐다.
constexpr size_t kLayoutRequestCapacity  = 1024; // 요청 하나에 답 하나라 응답과 같은 수다
constexpr size_t kLayoutResponseCapacity = 1024;
constexpr size_t kLayoutControlCapacity  = 8192; // 표 한 장이 줄 2,048개까지라 그 네 배를 둔다
constexpr size_t kLayoutStrategyCapacity = 256;

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
    uint64_t reserved1                = 0;
    uint64_t reserved2                = 0;
    uint64_t reserved3                = 0;
    uint64_t reserved4                = 0;
    uint64_t reserved5                = 0;
    uint64_t reserved6                = 0;
    uint64_t reserved7                = 0;
};

constexpr uint32_t kSharedLayoutMagic = 0x51'4C'41'59; // 'QLAY'

// 머리는 캐시라인 두 줄이다 — 뒤따르는 첫 면이 경계에서 시작한다.
static_assert(sizeof(SharedLayoutHead) == 2 * kSharedCacheLine, "자리표 머리는 캐시라인 두 줄이어야 한다");

// 박동 둘. 서로 다른 캐시라인에 둔다 — 한 줄에 같이 두면 한쪽이 찍을 때마다 건너편 줄이 무효가 되어,
//  박동을 보는 값싼 일이 둘을 계속 밀고 당기는 일이 된다.
struct SharedHeartbeats
{
    alignas(kSharedCacheLine) Heartbeat strategy; // 전략 프로세스가 찍고 주문 쪽이 본다
    alignas(kSharedCacheLine) Heartbeat order;    // 주문 프로세스가 찍고 전략 쪽이 본다
};

// 면 여덟의 크기를 정하는 값들. 양쪽이 **같은 값**을 넘겨야 같은 자리를 본다 — 다르면 붙기가 거절한다.
struct SharedLayoutConfig
{
    uint32_t feed_lanes               = 1; // 시세 소켓 수(줄 하나에 큐 둘)
    size_t   symbol_capacity          = symbol::kDefaultSymbolCapacity;
    size_t   strategy_capacity        = kLayoutStrategyCapacity;
    size_t   request_capacity         = kLayoutRequestCapacity;
    size_t   response_capacity        = kLayoutResponseCapacity;
    size_t   control_capacity         = kLayoutControlCapacity;
    size_t   feed_trade_capacity      = kFeedTradeCapacity;
    size_t   feed_order_book_capacity = kFeedOrderBookCapacity;
};

// 쪽지 위에 머리 하나와 면 여덟을 이 차례로 놓는다: 머리 → 요청 큐 → 응답 큐 → 제어 큐 → 박동 →
//  시세 통로 → 종목 표 → 전략 이름표 → 장부 사본. 면마다 캐시라인 경계에서 시작한다.
//  [inv] 제어 큐도 SPSC다 — 전략 프로세스 안에서 여러 스레드가 만든 제어 요청은 전략 스레드 하나가
//   모아서 이 줄에 옮긴다(원칙 5: 경계를 넘는 줄의 생산자는 하나로 만든다).
class SharedLayout
{
public:
    // 머리와 면 여덟을 담는 데 드는 바이트. SharedRegion 머리는 포함하지 않는다 — payload_bytes()와 견준다.
    [[nodiscard]] static size_t bytes_for(const SharedLayoutConfig& config);

    // 주문 프로세스가 부른다. 머리와 면마다 새 머리를 적고 칸을 0으로 민다.
    [[nodiscard]] bool create(std::byte* base, size_t bytes, const SharedLayoutConfig& config);

    // 전략 프로세스가 부른다. 자리표 머리의 설정을 한 칸씩 대조하고 안 맞으면 붙지 않는다.
    [[nodiscard]] bool attach(std::byte* base, size_t bytes, const SharedLayoutConfig& config);

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

    [[nodiscard]] SharedSpscRing<ControlRequest>& controls() noexcept
    {
        return controls_;
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

    // 면 여덟을 차례로 놓거나 붙인다. 중간에 하나라도 실패하면 전부 떼고 거짓을 준다 —
    //  반만 붙은 자리표를 들고 있지 않는다.
    [[nodiscard]] bool bind(std::byte* base, const SharedLayoutConfig& config, bool as_owner);

    SharedSpscRing<OrderRequest>   requests_;
    SharedSpscRing<OrderResponse>  responses_;
    SharedSpscRing<ControlRequest> controls_;
    MarketFeedChannel              feed_;
    SharedSymbolDictionary         symbols_;
    SharedStrategyDictionary       strategies_;
    SharedLayoutHead*              head_       = nullptr;
    SharedHeartbeats*              heartbeats_ = nullptr;
    LedgerSnapshot*                ledger_     = nullptr;
    std::string                    last_error_;
};

} // namespace ipc
