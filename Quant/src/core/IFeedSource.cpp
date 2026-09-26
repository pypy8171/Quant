#include "core/IFeedSource.h"

namespace feed
{
void IFeedSource::set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade)
{
    set_callbacks([callback = std::move(on_order_book)](const OrderBook& order_book)
    {
        callback(0, order_book);
    },
                  [callback = std::move(on_trade)](const TradeData& trade)
                  {
                      callback(0, trade);
                  });
}

bool IFeedSource::reconnect_stale(const std::vector<WatchSpec>& specifications, int /*threshold_sec*/)
{
    disconnect();
    return connect(specifications);
}

} // namespace feed
