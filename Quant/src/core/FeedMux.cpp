#include "core/FeedMux.h"

namespace feed
{
FeedMux::FeedMux(std::vector<std::unique_ptr<IFeedSource>> sources, size_t ring_capacity) : sources_(std::move(sources))
{
    lanes_.reserve(sources_.size());

    for (size_t source_index = 0; source_index < sources_.size(); ++source_index)
    {
        lanes_.push_back(std::make_unique<Lane>(ring_capacity));
    }
}

void FeedMux::set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade)
{
    lane_order_book_ = std::move(on_order_book);
    lane_trade_ = std::move(on_trade);
    lane_mode_ = true;

    for (size_t source_index = 0; source_index < sources_.size(); ++source_index)
    {
        const uint32_t lane = static_cast<uint32_t>(source_index);
        sources_[source_index]->set_callbacks([this, lane](const OrderBook& order_book)
                                              { lane_order_book_(lane, order_book); },
                                              [this, lane](const TradeData& trade) { lane_trade_(lane, trade); });
    }
}

void FeedMux::set_callbacks(OrderBookCb on_order_book, TradeCb on_trade)
{
    on_order_book_ = std::move(on_order_book);
    on_trade_ = std::move(on_trade);
    lane_mode_ = false;

    for (size_t source_index = 0; source_index < sources_.size(); ++source_index)
    {
        Lane* lane = lanes_[source_index].get();
        sources_[source_index]->set_callbacks([this, lane](const OrderBook& order_book)
                                              { enqueue(*lane, Event{order_book}); },
                                              [this, lane](const TradeData& trade) { enqueue(*lane, Event{trade}); });
    }

    if (!multiplexer_thread_.joinable())
    {
        multiplexer_thread_ = std::jthread([this](std::stop_token stop_token) { multiplexer_loop(stop_token); });
    }
}

void FeedMux::set_fill_callback(FillCb callback)
{
    on_fill_ = std::move(callback);

    if (!sources_.empty())
    {
        // 모드는 부르는 시점에 본다 — set_callbacks/set_lane_callbacks와 등록 순서에 매이지 않게.
        Lane* lane = lanes_[0].get();
        sources_[0]->set_fill_callback(
            [this, lane](const FillNotification& fill_notification)
            {
                if (lane_mode_)
                {
                    on_fill_(fill_notification);
                    return;
                }

                enqueue(*lane, Event{fill_notification});
            });
    }
}

bool FeedMux::connect(const std::vector<WatchSpec>& specifications)
{
    if (sources_.empty())
    {
        return false;
    }

    std::vector<std::vector<WatchSpec>> per_source(sources_.size());
    {
        std::lock_guard<std::mutex> lock(assign_mutex_);
        size_t next = 0;

        for (const auto& specification : specifications)
        {
            std::string specification_key = key(specification);
            const auto iterator = assign_.find(specification_key);
            size_t index;

            if (iterator != assign_.end())
            {
                index = iterator->second;
            }
            else
            {
                index = next++ % sources_.size();
                assign_.emplace(std::move(specification_key), index);
            }

            per_source[index].push_back(specification);
        }
    }

    for (size_t source_index = 0; source_index < sources_.size(); ++source_index)
    {
        if (!sources_[source_index]->connect(per_source[source_index]))
        {
            for (size_t inner_index = 0; inner_index < source_index; ++inner_index)
            {
                sources_[inner_index]->disconnect();
            }

            return false;
        }
    }

    return true;
}

void FeedMux::disconnect()
{
    for (auto& source : sources_)
    {
        source->disconnect();
    }
}

bool FeedMux::subscribe_incremental(const WatchSpec& specification)
{
    size_t index;
    {
        std::lock_guard<std::mutex> lock(assign_mutex_);
        std::string specification_key = key(specification);
        const auto iterator = assign_.find(specification_key);

        if (iterator != assign_.end())
        {
            index = iterator->second;
        }
        else
        {
            index = least_loaded_locked();
            assign_.emplace(std::move(specification_key), index);
        }
    }

    const bool sent = sources_[index]->subscribe_incremental(specification);

    if (!sent && !sources_[index]->has_specification(specification))
    {
        std::lock_guard<std::mutex> lock(assign_mutex_);
        assign_.erase(key(specification));
    }

    return sent;
}

bool FeedMux::has_specification(const WatchSpec& specification) const
{
    std::optional<size_t> index;
    {
        std::lock_guard<std::mutex> lock(assign_mutex_);
        const auto iterator = assign_.find(key(specification));

        if (iterator != assign_.end())
        {
            index = iterator->second;
        }
    }

    return index && sources_[*index]->has_specification(specification);
}

std::vector<WatchSpec> FeedMux::take_overflow_specifications()
{
    std::vector<WatchSpec> out;

    for (auto& source : sources_)
    {
        auto part = source->take_overflow_specifications();
        out.insert(out.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }

    if (!out.empty())
    {
        std::lock_guard<std::mutex> lock(assign_mutex_);

        for (const auto& out_event : out)
        {
            assign_.erase(key(out_event));
        }
    }

    return out;
}

bool FeedMux::is_connected() const
{
    for (const auto& source : sources_)
    {
        if (source->is_connected())
        {
            return true;
        }
    }

    return false;
}

bool FeedMux::is_stale(int threshold_sec) const
{
    for (const auto& source : sources_)
    {
        if (!source->is_connected() || source->is_stale(threshold_sec))
        {
            return true;
        }
    }

    return false;
}

bool FeedMux::reconnect_stale(const std::vector<WatchSpec>& specifications, int threshold_sec)
{
    std::vector<bool> dead(sources_.size(), false);
    size_t dead_n = 0;

    for (size_t source_index = 0; source_index < sources_.size(); ++source_index)
    {
        if (!sources_[source_index]->is_connected() || sources_[source_index]->is_stale(threshold_sec))
        {
            dead[source_index] = true;
            ++dead_n;
        }
    }

    if (dead_n == 0 || dead_n == sources_.size())
    {
        disconnect();
        return connect(specifications);
    }

    std::vector<std::vector<WatchSpec>> per_source(sources_.size());
    {
        std::lock_guard<std::mutex> lock(assign_mutex_);
        size_t next = 0;

        for (const auto& specification : specifications)
        {
            std::string specification_key = key(specification);
            const auto iterator = assign_.find(specification_key);
            size_t index;

            if (iterator != assign_.end())
            {
                index = iterator->second;
            }
            else
            {
                index = next++ % sources_.size();

                while (!dead[index])
                {
                    index = (index + 1) % sources_.size();
                }

                assign_.emplace(std::move(specification_key), index);
            }

            if (dead[index])
            {
                per_source[index].push_back(specification);
            }
        }
    }

    bool ok = true;

    for (size_t source_index = 0; source_index < sources_.size(); ++source_index)
    {
        if (!dead[source_index])
        {
            continue;
        }

        sources_[source_index]->disconnect();
        ok = sources_[source_index]->connect(per_source[source_index]) && ok;
    }

    return ok;
}

std::optional<size_t> FeedMux::source_of(const WatchSpec& specification) const
{
    std::lock_guard<std::mutex> lock(assign_mutex_);
    const auto iterator = assign_.find(key(specification));
    return iterator == assign_.end() ? std::nullopt : std::optional<size_t>(iterator->second);
}

void FeedMux::enqueue(Lane& lane, Event&& event)
{
    if (!lane.ring.push(std::move(event)))
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    wake_.notify();
}

size_t FeedMux::least_loaded_locked() const
{
    std::vector<size_t> load(sources_.size(), 0);

    for (const auto& [assignment, index] : assign_)
    {
        ++load[index];
    }

    size_t best = 0;

    for (size_t load_index = 1; load_index < load.size(); ++load_index)
    {
        if (load[load_index] < load[best])
        {
            best = load_index;
        }
    }

    return best;
}

bool FeedMux::all_empty() const
{
    for (const auto& lane : lanes_)
    {
        if (!lane->ring.empty())
        {
            return false;
        }
    }

    return true;
}

void FeedMux::multiplexer_loop(std::stop_token stop_token)
{
    static constexpr size_t kBurst = 256;

    while (!stop_token.stop_requested())
    {
        bool any = false;

        for (auto& lane : lanes_)
        {
            for (size_t burst_index = 0; burst_index < kBurst; ++burst_index)
            {
                auto event = lane->ring.pop();

                if (!event)
                {
                    break;
                }

                any = true;
                dispatch(*event);
            }
        }

        if (!any)
        {
            wake_.wait_for(std::chrono::milliseconds(5), stop_token, [this] { return all_empty(); });
        }
    }
}

void FeedMux::dispatch(Event& event)
{
    if (auto* trade = std::get_if<TradeData>(&event))
    {
        if (on_trade_)
        {
            on_trade_(*trade);
        }
    }
    else if (auto* order_book = std::get_if<OrderBook>(&event))
    {
        if (on_order_book_)
        {
            on_order_book_(*order_book);
        }
    }
    else if (auto* fill_notification = std::get_if<FillNotification>(&event))
    {
        if (on_fill_)
        {
            on_fill_(*fill_notification);
        }
    }
}

} // namespace feed
