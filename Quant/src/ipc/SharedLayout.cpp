#include "ipc/SharedLayout.h"

#include <new>
#include <type_traits>

namespace ipc
{

// 장부 사본을 공유 바이트 위에 그대로 놓는다 — 배열이 고정이고 값 칸만 있어야 건너편이 같은 자리를 본다.
//  포인터나 힙을 들고 있으면 여기서 걸린다(그 주소는 건너편에서 남의 자리다).
static_assert(std::is_standard_layout_v<LedgerSnapshot>, "장부 사본은 표준 배치여야 공유 쪽지에 얹힌다");
static_assert(std::is_trivially_destructible_v<LedgerSnapshot>, "장부 사본은 소멸자 없이 사라져야 한다");
static_assert(alignof(LedgerSnapshot) <= kSharedCacheLine, "장부 사본 정렬이 캐시라인보다 크다");
static_assert(alignof(SharedHeartbeats) <= kSharedCacheLine, "박동 정렬이 캐시라인보다 크다");
static_assert(alignof(RegimeCell) <= kSharedCacheLine, "국면 칸 정렬이 캐시라인보다 크다");

namespace
{

// 면 하나가 끝나는 자리를 캐시라인 경계까지 올린다 — 다음 면의 머리가 경계에서 시작해야 붙는다.
[[nodiscard]] size_t align_up(size_t bytes) noexcept
{
    return (bytes + kSharedCacheLine - 1) / kSharedCacheLine * kSharedCacheLine;
}

[[nodiscard]] size_t feed_span(const SharedLayoutConfig& config) noexcept
{
    return align_up(
        MarketFeedChannel::bytes_for(config.feed_lanes, config.feed_trade_capacity, config.feed_order_book_capacity));
}

// 면 하나의 양끝이 누구인가. 주인(주문)까지 들어가야 방향을 제대로 적는다 — 붙는 쪽만으로 적으면
//  "전략이 아닌 쪽"이 주문인지 시세인지 가려지지 않는다.
enum class FaceParty : uint8_t
{
    kOrder    = 0,
    kStrategy = 1,
    kFeed     = 2,
};

// 붙는 역할이 그 면에서 맡는 끝. 제 끝이 아닌 면은 구경만 한다 — 붙기만 하고 공유 칸에는 아무것도 안 적는다.
//  아래 부르는 자리들이 통로 방향의 정본이다(설계 문서의 면 표와 같아야 한다). [why D-114]
[[nodiscard]] RingEndpoint endpoint_for(SharedAttachRole role, FaceParty producer, FaceParty consumer) noexcept
{
    const FaceParty self = role == SharedAttachRole::kFeed ? FaceParty::kFeed : FaceParty::kStrategy;

    if (producer == self)
    {
        return RingEndpoint::kProducer;
    }

    if (consumer == self)
    {
        return RingEndpoint::kConsumer;
    }

    return RingEndpoint::kObserver;
}

} // namespace

size_t SharedLayout::bytes_for(const SharedLayoutConfig& config)
{
    size_t total = align_up(sizeof(SharedLayoutHead));

    total += align_up(SharedSpscRing<OrderRequest>::bytes_for(config.request_capacity));
    total += align_up(SharedSpscRing<OrderResponse>::bytes_for(config.response_capacity));
    total += align_up(SharedSpscRing<ControlRequest>::bytes_for(config.control_capacity));
    total += align_up(SharedSpscRing<ControlRequest>::bytes_for(config.feed_control_capacity));
    total += align_up(FillChannel::bytes_for(config.fill_capacity));
    total += align_up(sizeof(SharedHeartbeats));
    total += align_up(sizeof(RegimeCell));
    total += feed_span(config);
    total += align_up(SharedSymbolDictionary::bytes_for(config.symbol_capacity));
    total += align_up(SharedStrategyDictionary::bytes_for(config.strategy_capacity));
    total += align_up(sizeof(LedgerSnapshot));

    return total;
}

bool SharedLayout::check_config(const std::byte* base, size_t bytes, const SharedLayoutConfig& config)
{
    last_error_.clear();

    if (base == nullptr)
    {
        last_error_ = "자리표를 놓을 자리가 없다";
        return false;
    }

    if (reinterpret_cast<uintptr_t>(base) % kSharedCacheLine != 0)
    {
        last_error_ = "자리표가 캐시라인 경계에서 시작하지 않는다";
        return false;
    }

    if (config.feed_lanes == 0 || config.feed_lanes > kMaxFeedLanes)
    {
        last_error_ = "시세 줄 수가 1과 " + std::to_string(kMaxFeedLanes) + " 사이가 아니다 — 받은 것=" +
                      std::to_string(config.feed_lanes);
        return false;
    }

    // 종목 번호가 장부 사본의 줄 수를 넘으면 그 종목의 보유가 사본에 안 실린다 — 전략이 "보유 0"으로 읽고
    //  같은 종목을 또 산다(이중 발주, A등급). 기동 때 여기서 멈추는 쪽이 낫다.
    if (config.symbol_capacity == 0 || config.symbol_capacity > LedgerSnapshot::kMaxSymbols)
    {
        last_error_ = "종목 수가 1과 장부 사본 상한 " + std::to_string(LedgerSnapshot::kMaxSymbols) +
                      " 사이가 아니다 — 받은 것=" + std::to_string(config.symbol_capacity);
        return false;
    }

    if (config.strategy_capacity < 2)
    {
        last_error_ = "전략 수가 2보다 작다";
        return false;
    }

    if (config.request_capacity == 0 || config.response_capacity == 0 || config.control_capacity == 0 ||
        config.feed_control_capacity == 0 || config.fill_capacity == 0)
    {
        last_error_ = "칸 수가 0인 큐가 있다";
        return false;
    }

    const size_t needed = bytes_for(config);

    if (bytes < needed)
    {
        last_error_ = "구역이 자리표보다 작다 — 필요=" + std::to_string(needed) + " 받은 것=" + std::to_string(bytes);
        return false;
    }

    return true;
}

bool SharedLayout::bind_head(std::byte* base, const SharedLayoutConfig& config, bool as_owner)
{
    if (as_owner)
    {
        SharedLayoutHead* head         = new (base) SharedLayoutHead();
        head->layout_version           = kSharedLayoutVersion;
        head->feed_lanes               = config.feed_lanes;
        head->symbol_capacity          = config.symbol_capacity;
        head->strategy_capacity        = config.strategy_capacity;
        head->request_capacity         = config.request_capacity;
        head->response_capacity        = config.response_capacity;
        head->control_capacity         = config.control_capacity;
        head->feed_trade_capacity      = config.feed_trade_capacity;
        head->feed_order_book_capacity = config.feed_order_book_capacity;
        head->feed_control_capacity    = config.feed_control_capacity;
        head->fill_capacity            = config.fill_capacity;
        head->magic                    = kSharedLayoutMagic; // 표식은 마지막에 — 붙는 쪽은 이걸 보고 들어온다
        head_                          = head;
        return true;
    }

    SharedLayoutHead* head = reinterpret_cast<SharedLayoutHead*>(base);

    if (head->magic != kSharedLayoutMagic || head->layout_version != kSharedLayoutVersion)
    {
        last_error_ = "자리표 머리가 다르다 — 옛 exe가 새 배치에 붙었는지 본다";
        return false;
    }

    // 설정이 한 칸이라도 다르면 뒤따르는 면의 자리가 통째로 밀린다. 그 자리에서 멈춘다.
    if (head->feed_lanes != config.feed_lanes || head->symbol_capacity != config.symbol_capacity ||
        head->strategy_capacity != config.strategy_capacity || head->request_capacity != config.request_capacity ||
        head->response_capacity != config.response_capacity || head->control_capacity != config.control_capacity ||
        head->feed_trade_capacity != config.feed_trade_capacity ||
        head->feed_order_book_capacity != config.feed_order_book_capacity ||
        head->feed_control_capacity != config.feed_control_capacity || head->fill_capacity != config.fill_capacity)
    {
        last_error_ = "자리표 설정이 건너편과 다르다 — 양쪽 설정 파일이 같은지 본다";
        return false;
    }

    head_ = head;
    return true;
}

bool SharedLayout::bind(std::byte* base, const SharedLayoutConfig& config, bool as_owner, SharedAttachRole role)
{
    if (!bind_head(base, config, as_owner))
    {
        unbind();
        return false;
    }

    std::byte* cursor = base + align_up(sizeof(SharedLayoutHead));

    // 큐 셋 — 놓기와 붙기는 같은 자리를 같은 차례로 짚고, 다른 것은 머리를 적느냐 대조하느냐뿐이다.
    const size_t request_bytes = align_up(SharedSpscRing<OrderRequest>::bytes_for(config.request_capacity));

    // 요청 큐: 전략이 보내고 주문이 받는다 — 시세는 구경만 한다.
    if (!(as_owner ? requests_.create(cursor, request_bytes, config.request_capacity)
                   : requests_.attach(cursor, request_bytes, config.request_capacity,
                                      endpoint_for(role, FaceParty::kStrategy, FaceParty::kOrder))))
    {
        last_error_ = "요청 큐: " + std::string(requests_.last_error());
        unbind();
        return false;
    }

    cursor += request_bytes;

    const size_t response_bytes = align_up(SharedSpscRing<OrderResponse>::bytes_for(config.response_capacity));

    // 응답 큐: 주문이 보내고 전략이 받는다.
    if (!(as_owner ? responses_.create(cursor, response_bytes, config.response_capacity)
                   : responses_.attach(cursor, response_bytes, config.response_capacity,
                                       endpoint_for(role, FaceParty::kOrder, FaceParty::kStrategy))))
    {
        last_error_ = "응답 큐: " + std::string(responses_.last_error());
        unbind();
        return false;
    }

    cursor += response_bytes;

    const size_t control_bytes = align_up(SharedSpscRing<ControlRequest>::bytes_for(config.control_capacity));

    // 제어 큐(주문): 전략이 보내고 주문이 받는다.
    if (!(as_owner ? controls_.create(cursor, control_bytes, config.control_capacity)
                   : controls_.attach(cursor, control_bytes, config.control_capacity,
                                      endpoint_for(role, FaceParty::kStrategy, FaceParty::kOrder))))
    {
        last_error_ = "제어 큐: " + std::string(controls_.last_error());
        unbind();
        return false;
    }

    cursor += control_bytes;

    const size_t feed_control_bytes = align_up(SharedSpscRing<ControlRequest>::bytes_for(config.feed_control_capacity));

    // 제어 큐(시세): 전략이 보내고 시세가 받는다. 보내는 쪽이 주문 줄과 같은 전략 하나라 SPSC 그대로다.
    if (!(as_owner ? feed_controls_.create(cursor, feed_control_bytes, config.feed_control_capacity)
                   : feed_controls_.attach(cursor, feed_control_bytes, config.feed_control_capacity,
                                           endpoint_for(role, FaceParty::kStrategy, FaceParty::kFeed))))
    {
        last_error_ = "제어 큐(시세): " + std::string(feed_controls_.last_error());
        unbind();
        return false;
    }

    cursor += feed_control_bytes;

    const size_t fill_bytes = align_up(FillChannel::bytes_for(config.fill_capacity));

    // 체결 통로: 시세가 보내고 주문이 받는다 — 전략은 구경만 한다.
    if (!(as_owner ? fills_.create(cursor, fill_bytes, config.fill_capacity)
                   : fills_.attach(cursor, fill_bytes, endpoint_for(role, FaceParty::kFeed, FaceParty::kOrder),
                                   config.fill_capacity)))
    {
        last_error_ = "체결 통로: " + std::string(fills_.last_error());
        unbind();
        return false;
    }

    cursor += fill_bytes;

    // 박동 — 값 둘뿐이라 머리가 없다. 놓는 쪽만 0으로 민다(붙는 쪽이 밀면 건너편이 찍어 둔 박동이 사라진다).
    const size_t heartbeat_bytes = align_up(sizeof(SharedHeartbeats));

    if (as_owner)
    {
        heartbeats_ = new (cursor) SharedHeartbeats();
    }
    else
    {
        heartbeats_ = reinterpret_cast<SharedHeartbeats*>(cursor);
    }

    cursor += heartbeat_bytes;

    // 국면 칸 — 박동과 같이 머리가 없는 값 한 칸이다. 놓는 쪽만 "아직 판정 없음"으로 민다.
    //  붙는 쪽이 밀면 장중에 시세·전략이 다시 뜨는 순간 전략이 적어 둔 국면이 지워진다. [why D-129]
    const size_t regime_bytes = align_up(sizeof(RegimeCell));

    if (as_owner)
    {
        regime_cell_ = new (cursor) RegimeCell();
    }
    else
    {
        regime_cell_ = reinterpret_cast<RegimeCell*>(cursor);
    }

    cursor += regime_bytes;

    const size_t feed_bytes = feed_span(config);

    // 시세 통로: 시세가 보내고 전략이 받는다.
    if (!(as_owner ? feed_.create(cursor, feed_bytes, config.feed_lanes, config.feed_trade_capacity,
                                  config.feed_order_book_capacity)
                   : feed_.attach(cursor, feed_bytes, config.feed_lanes,
                                  endpoint_for(role, FaceParty::kFeed, FaceParty::kStrategy),
                                  config.feed_trade_capacity, config.feed_order_book_capacity)))
    {
        last_error_ = "시세 통로: " + std::string(feed_.last_error());
        unbind();
        return false;
    }

    cursor += feed_bytes;

    const size_t symbol_bytes = align_up(SharedSymbolDictionary::bytes_for(config.symbol_capacity));

    if (!(as_owner ? symbols_.create(cursor, symbol_bytes, config.symbol_capacity)
                   : symbols_.attach(cursor, symbol_bytes, config.symbol_capacity)))
    {
        last_error_ = "종목 표: " + std::string(symbols_.last_error());
        unbind();
        return false;
    }

    cursor += symbol_bytes;

    const size_t strategy_bytes = align_up(SharedStrategyDictionary::bytes_for(config.strategy_capacity));

    if (!(as_owner ? strategies_.create(cursor, strategy_bytes, config.strategy_capacity)
                   : strategies_.attach(cursor, strategy_bytes, config.strategy_capacity)))
    {
        last_error_ = "전략 이름표: " + std::string(strategies_.last_error());
        unbind();
        return false;
    }

    cursor += strategy_bytes;

    // 장부 사본 — 머리가 없어 대조할 것이 없다. 앞선 면이 다 맞았으면 이 자리도 맞는다.
    //  붙는 쪽은 짓지 않는다(지으면 주문 쪽이 이미 실어 둔 보유가 0으로 지워진다).
    if (as_owner)
    {
        ledger_ = new (cursor) LedgerSnapshot();
    }
    else
    {
        ledger_ = reinterpret_cast<LedgerSnapshot*>(cursor);
    }

    return true;
}

bool SharedLayout::create(std::byte* base, size_t bytes, const SharedLayoutConfig& config)
{
    if (!check_config(base, bytes, config))
    {
        return false;
    }

    return bind(base, config, true, SharedAttachRole::kStrategy);
}

bool SharedLayout::attach(std::byte* base, size_t bytes, const SharedLayoutConfig& config, SharedAttachRole role)
{
    if (!check_config(base, bytes, config))
    {
        return false;
    }

    return bind(base, config, false, role);
}

void SharedLayout::unbind() noexcept
{
    requests_.unbind();
    responses_.unbind();
    controls_.unbind();
    feed_controls_.unbind();
    fills_.unbind();
    feed_.unbind();
    symbols_.unbind();
    strategies_.unbind();
    head_        = nullptr;
    heartbeats_  = nullptr;
    regime_cell_ = nullptr;
    ledger_      = nullptr;
}

} // namespace ipc
