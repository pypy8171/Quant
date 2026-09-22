// 실시간 피드 소스 인터페이스 — Engine이 호가·체결·체결통보를 받는 창구. KIS WebSocket과 캡처 파일 리플레이가 구현한다.
// 스레드: 구현이 자기 수신 스레드에서 콜백을 부른다 — 수신 스레드 i의 콜백은 스레드 하나만 부른다(lanes()). Engine은 콜백 안에서
//  push만 한다(원칙 3). [why D-071]
#pragma once
#include "core/Types.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace feed
{

class IFeedSource
{
public:
    using OrderBookCb = std::function<void(const OrderBook&)>;
    using TradeCb     = std::function<void(const TradeData&)>;
    using FillCb      = std::function<void(const FillNotification&)>;
    // 수신 스레드 번호가 붙은 콜백 — 첫 인자가 이 이벤트를 부르는 수신 스레드의 번호(0 ≤ lane < lanes()).
    using LaneOrderBookCb = std::function<void(uint32_t, const OrderBook&)>;
    using LaneTradeCb     = std::function<void(uint32_t, const TradeData&)>;

    virtual ~IFeedSource() = default;

    virtual void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) = 0;

    // 수신 스레드 수. Engine이 링 행렬의 행 수로 쓴다(원칙 5 — 수신 스레드마다 자기 행, 생산자 하나). 소켓 하나면 1.
    virtual uint32_t lanes() const
    {
        return 1;
    }

    // 수신 스레드 번호를 달아 부르는 콜백. 기본은 수신 스레드 0 하나로 set_callbacks에 얹는다 — 소켓 여럿을 묶는 구현만 덮어쓴다.
    virtual void set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade);

    // 체결통보가 없는 소스(리플레이)는 등록을 무시한다 — 주문은 어차피 REST 라우터가 낸다.
    virtual void set_fill_callback(FillCb) {}

    virtual bool connect(const std::vector<WatchSpec>& specifications) = 0;
    virtual void disconnect()                                 = 0;

    // 연결을 유지한 채 종목을 더 구독한다. 반환·판정 규약은 KisWebSocket::subscribe_incremental 주석.
    virtual bool                   subscribe_incremental(const WatchSpec& specification) = 0;
    virtual bool                   has_specification(const WatchSpec& specification) const        = 0;
    virtual std::vector<WatchSpec> take_overflow_specifications()                        = 0;

    virtual bool is_connected() const = 0;
    // threshold_sec 이상 메시지가 없으면 true. Engine 제어 스레드가 재연결 판단에 쓴다.
    virtual bool is_stale(int threshold_sec) const = 0;

    // 멈춘 연결을 다시 잇는다. specs는 지금 봐야 할 종목 전체(재스캔 추가분 포함). 소켓 하나면 끊고 specs로 다시 잇는
    //  것이고, 소켓 여럿을 묶은 소스는 멈춘 것만 자기 종목으로 다시 잇는다(FeedMux) — 살아 있는 소켓의 틱은 그 사이에도
    //  흐른다. 다시 이은 연결이 전부 성공하면 true. 제어 스레드만 부른다. [why D-071]
    virtual bool reconnect_stale(const std::vector<WatchSpec>& specifications, int /*threshold_sec*/);
};

} // namespace feed
