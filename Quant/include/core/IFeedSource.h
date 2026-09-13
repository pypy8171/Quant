// 실시간 피드 소스 인터페이스 — Engine이 호가·체결·체결통보를 받는 창구. KIS WebSocket과 캡처 파일 리플레이가 구현한다.
// 스레드: 구현이 자기 수신 스레드에서 콜백을 부른다. Engine은 콜백 안에서 push만 한다(원칙 3). [why D-071]
#pragma once
#include "core/Types.h"

#include <functional>
#include <vector>

namespace feed
{

class IFeedSource
{
public:
    using OrderBookCb = std::function<void(const OrderBook&)>;
    using TradeCb     = std::function<void(const TradeData&)>;
    using FillCb      = std::function<void(const FillNotification&)>;

    virtual ~IFeedSource() = default;

    virtual void set_callbacks(OrderBookCb on_ob, TradeCb on_trade) = 0;

    // 체결통보가 없는 소스(리플레이)는 등록을 무시한다 — 주문은 어차피 REST 라우터가 낸다.
    virtual void set_fill_callback(FillCb) {}

    virtual bool connect(const std::vector<WatchSpec>& specs) = 0;
    virtual void disconnect()                                 = 0;

    // 연결을 유지한 채 종목을 더 구독한다. 반환·판정 규약은 KisWebSocket::subscribe_incremental 주석.
    virtual bool                   subscribe_incremental(const WatchSpec& spec) = 0;
    virtual bool                   has_spec(const WatchSpec& spec) const        = 0;
    virtual std::vector<WatchSpec> take_overflow_specs()                        = 0;

    virtual bool is_connected() const = 0;
    // threshold_sec 이상 메시지가 없으면 true. Engine 제어 스레드가 재연결 판단에 쓴다.
    virtual bool is_stale(int threshold_sec) const = 0;
};

} // namespace feed
