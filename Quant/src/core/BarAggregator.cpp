// N분봉 집계기 — 전략 스레드 소유, 락 없음. 확정은 다음 버킷 첫 틱이 하고, 틱이 없으면 호출자가 시계로
//  close_stale을 부른다(집계기 안에 타이머는 없다). [why D-068] [why D-074]
#include "core/BarAggregator.h"

#include "core/KstTime.h"

#include <algorithm>

namespace bars
{
namespace
{
int64_t day_key(const struct tm& time_parts)
{
    return static_cast<int64_t>(time_parts.tm_year) * 400 + time_parts.tm_yday;
}
} // namespace

BarSlot slot_of(int32_t hhmmss, std::time_t recv_utc, int interval_min, int open_hhmm, int close_hhmm)
{
    BarSlot out;

    if (interval_min < 1)
    {
        return out;
    }

    const struct tm local_time = kst::to_tm(recv_utc);
    int             hour = local_time.tm_hour;
    int             minute = local_time.tm_min;

    if (hhmmss > 0)
    {
        hour = hhmmss / 10000;
        minute = hhmmss / 100 % 100;
    }

    const int hhmm = hour * 100 + minute;

    if (hour > 23 || minute > 59 || hhmm < open_hhmm || hhmm > close_hhmm)
    {
        return out;
    }

    out.day    = day_key(local_time);
    out.bucket = (hour * 60 + minute) / interval_min;
    return out;
}

std::chrono::system_clock::time_point slot_start(const BarSlot& slot, std::time_t recv_utc, int interval_min)
{
    const struct tm   local_time          = kst::to_tm(recv_utc);
    const std::time_t day_start  = recv_utc - (local_time.tm_hour * 3600 + local_time.tm_min * 60 + local_time.tm_sec);
    const std::time_t bar_start  = day_start + static_cast<std::time_t>(slot.bucket) * interval_min * 60;
    return std::chrono::system_clock::from_time_t(bar_start);
}

std::vector<MarketData> resample(const std::vector<MarketData>& bars_1m, int interval_min, int max_count)
{
    std::vector<MarketData> out;

    if (bars_1m.empty())
    {
        return out;
    }

    const size_t wanted_count = max_count > 0 ? static_cast<size_t>(max_count) : bars_1m.size();

    if (interval_min <= 1)
    {
        out.assign(bars_1m.begin(), bars_1m.begin() + static_cast<std::ptrdiff_t>((std::min)(wanted_count, bars_1m.size())));

        for (size_t out_index = 0; out_index < out.size(); ++out_index)
        {
            out[out_index].bar_index = static_cast<int>(out_index);
        }

        return out;
    }

    // 과거→최신으로 걸으며 같은 자리를 접는다. 자리는 봉 timestamp의 KST 분에서 온다 — 집계기 1분봉(자리 시작
    //  시각)과 REST 1분봉(그 분의 라벨) 모두 같은 분을 가리킨다.
    std::vector<MarketData> ascending;
    BarSlot                 current;

    for (auto iterator = bars_1m.rbegin(); iterator != bars_1m.rend(); ++iterator)
    {
        const std::time_t timestamp = std::chrono::system_clock::to_time_t(iterator->timestamp);
        const struct tm   local_time  = kst::to_tm(timestamp);
        BarSlot           slot;
        slot.day    = day_key(local_time);
        slot.bucket = (local_time.tm_hour * 60 + local_time.tm_min) / interval_min;

        if (ascending.empty() || slot != current)
        {
            ascending.push_back(*iterator);
            current = slot;
            continue;
        }

        MarketData& market_data = ascending.back();
        market_data.high        = (std::max)(market_data.high, iterator->high); // (): windows.h max 매크로 회피
        market_data.low         = (std::min)(market_data.low, iterator->low);
        market_data.close       = iterator->close;
        market_data.volume += iterator->volume;
        market_data.timestamp = iterator->timestamp;
    }

    out.reserve((std::min)(wanted_count, ascending.size()));

    for (auto iterator = ascending.rbegin(); iterator != ascending.rend() && out.size() < wanted_count; ++iterator)
    {
        out.push_back(*iterator);
        out.back().bar_index = static_cast<int>(out.size() - 1);
    }

    return out;
}

BarAggregator::BarAggregator(Config config) : config_(config)
{
    if (config_.interval_min < 1)
    {
        config_.interval_min = 1;
    }

    if (config_.keep < 1)
    {
        config_.keep = 1;
    }
}

void BarAggregator::close_live(Series& series)
{
    if (!series.live.active)
    {
        return;
    }

    series.closed.push_front(series.live.market_data);
    series.slots.push_front(series.live.slot);
    series.live.active    = false;
    series.live.accumulated_base = -1;
    trim(series);

    if (sink_)
    {
        sink_(series.closed.front());
    }
}

int BarAggregator::close_stale(std::time_t now_utc)
{
    int count = 0;

    for (auto& entry : series_)
    {
        count += close_stale(entry.first, now_utc);
    }

    return count;
}

int BarAggregator::close_stale(symbol::SymbolId symbol_id, std::time_t now_utc)
{
    auto iterator = series_.find(symbol_id);

    if (iterator == series_.end() || !iterator->second.live.active)
    {
        return 0;
    }

    // 장 시간 필터는 걸지 않는다 — 15:31의 시계가 15:30 봉을 닫아야 한다. hhmmss가 비어 있으니 now의 KST 분이 자리다.
    const BarSlot now_slot = slot_of(0, now_utc, config_.interval_min, 0, 2359);

    if (!now_slot.valid() || !(iterator->second.live.slot < now_slot))
    {
        return 0;
    }

    close_live(iterator->second);
    return 1;
}

void BarAggregator::trim(Series& series)
{
    while (static_cast<int>(series.closed.size()) > config_.keep)
    {
        series.closed.pop_back();
        series.slots.pop_back();
    }
}

bool BarAggregator::on_tick(const TradeData& trade)
{
    if (trade.price <= 0.0 || trade.symbol_id == symbol::kNone)
    {
        return false;
    }

    const std::time_t recv = std::chrono::system_clock::to_time_t(trade.timestamp);
    const BarSlot     slot = slot_of(trade.hhmmss, recv, config_.interval_min, config_.session_open, config_.session_close);

    if (!slot.valid())
    {
        return false;
    }

    Series& series = series_[trade.symbol_id];

    if (series.ticker.empty())
    {
        series.ticker = trade.ticker;
    }

    if (series.live.active && slot < series.live.slot)
    {
        return false; // 늦게 온 과거 틱 — 닫힌 봉의 종가를 고칠 순 없다
    }

    if (!series.slots.empty() && slot < series.slots.front())
    {
        return false; // 시드가 이미 닫아 둔 자리보다 오래된 틱
    }

    if (series.live.active && slot != series.live.slot)
    {
        close_live(series);
    }

    if (!series.live.active)
    {
        // [inv] 거래량은 누적차다 — 첫 틱의 acml − quantity가 버킷 시작값. acml이 없으면(REST 대체 틱) quantity 합산.
        series.live.active    = true;
        series.live.slot      = slot;
        series.live.accumulated_base = trade.accumulated_volume > 0 ? trade.accumulated_volume - trade.quantity : -1;
        MarketData& market_data   = series.live.market_data;

        if (!series.slots.empty() && series.slots.front() == slot)
        {
            // 시드가 같은 자리를 닫힌 봉으로 넣어 뒀다(REST가 진행 중 봉을 돌려준 경우). 그 위에 이어 붙인다.
            market_data = series.closed.front();
            series.closed.pop_front();
            series.slots.pop_front();
            market_data.high  = (std::max)(market_data.high, trade.price);
            market_data.low   = (std::min)(market_data.low, trade.price);
            market_data.close = trade.price;
            market_data.volume += trade.quantity;

            if (series.live.accumulated_base >= 0)
            {
                series.live.accumulated_base = trade.accumulated_volume - market_data.volume;
            }

            return true;
        }

        market_data               = MarketData{};
        market_data.ticker        = series.ticker;
        market_data.symbol_id           = trade.symbol_id;
        market_data.market        = trade.market;
        market_data.open = market_data.high = market_data.low = market_data.close = trade.price;
        market_data.volume        = trade.quantity;
        market_data.timestamp     = slot_start(slot, recv, config_.interval_min);
        market_data.bar_index     = 0;
        return true;
    }

    MarketData& market_data = series.live.market_data;
    market_data.high        = (std::max)(market_data.high, trade.price); // (): windows.h max 매크로 회피
    market_data.low         = (std::min)(market_data.low, trade.price);
    market_data.close       = trade.price;

    if (series.live.accumulated_base >= 0 && trade.accumulated_volume > 0)
    {
        market_data.volume = (std::max)(market_data.volume, trade.accumulated_volume - series.live.accumulated_base);
    }
    else
    {
        market_data.volume += trade.quantity;
    }

    return true;
}

int BarAggregator::seed(symbol::SymbolId symbol_id, const std::vector<MarketData>& rest_bars)
{
    if (symbol_id == symbol::kNone || rest_bars.empty())
    {
        return 0;
    }

    Series& series     = series_[symbol_id];
    int     added = 0;

    if (series.ticker.empty())
    {
        series.ticker = rest_bars.front().ticker;
    }

    for (const MarketData& rest_bar : rest_bars)
    {
        const std::time_t timestamp = std::chrono::system_clock::to_time_t(rest_bar.timestamp);

        if (timestamp <= 0)
        {
            continue;
        }

        // REST 봉의 timestamp는 버킷의 마지막 1분 시각이라 같은 버킷에 든다. 장 시간 필터는 시드에 걸지 않는다.
        const BarSlot slot = slot_of(kst::hhmmss_int(timestamp), timestamp, config_.interval_min, 0, 2359);

        if (!slot.valid())
        {
            continue;
        }

        if (series.live.active && slot == series.live.slot)
        {
            MarketData&   market_data      = series.live.market_data;
            const int64_t old_volume = market_data.volume;
            market_data.open               = rest_bar.open;
            market_data.high               = (std::max)(market_data.high, rest_bar.high);
            market_data.low                = (std::min)(market_data.low, rest_bar.low);
            market_data.volume             = (std::max)(old_volume, rest_bar.volume);

            // 누적차 기준을 옮겨 다음 틱부터 합친 거래량 위에 쌓이게 한다(마지막 acml = base + old_volume).
            if (series.live.accumulated_base >= 0)
            {
                series.live.accumulated_base += old_volume - market_data.volume;
            }

            continue;
        }

        if (series.live.active && series.live.slot < slot)
        {
            // 시드가 진행 중 봉보다 새 자리를 가졌다 — 로컬이 그 사이 틱을 못 본 것이다. 진행 봉을 닫고 이어 붙인다.
            close_live(series);
        }

        auto iterator = std::lower_bound(series.slots.begin(), series.slots.end(), slot,
                                   [](const BarSlot& slot_a, const BarSlot& slot_b) { return slot_b < slot_a; }); // 내림차순

        const size_t index = static_cast<size_t>(iterator - series.slots.begin());

        if (iterator != series.slots.end() && *iterator == slot)
        {
            series.closed[index] = rest_bar; // 닫힌 자리는 REST가 이긴다
        }
        else
        {
            series.slots.insert(iterator, slot);
            series.closed.insert(series.closed.begin() + static_cast<std::ptrdiff_t>(index), rest_bar);
            ++added;
        }
    }

    for (size_t closed_index = 0; closed_index < series.closed.size(); ++closed_index)
    {
        series.closed[closed_index].ticker    = series.ticker;
        series.closed[closed_index].symbol_id       = symbol_id;
        series.closed[closed_index].bar_index = 0; // snapshot이 다시 매긴다
    }

    trim(series);
    return added;
}

std::vector<MarketData> BarAggregator::snapshot(symbol::SymbolId symbol_id, int max_count) const
{
    std::vector<MarketData> out;
    const auto              iterator = series_.find(symbol_id);

    if (iterator == series_.end())
    {
        return out;
    }

    const Series& series = iterator->second;
    const size_t  wanted_count = max_count > 0 ? static_cast<size_t>(max_count) : series.closed.size() + 1;
    out.reserve((std::min)(wanted_count, series.closed.size() + 1));

    if (series.live.active)
    {
        out.push_back(series.live.market_data);
    }

    for (const MarketData& market_data : series.closed)
    {
        if (out.size() >= wanted_count)
        {
            break;
        }

        out.push_back(market_data);
    }

    for (size_t out_index = 0; out_index < out.size(); ++out_index)
    {
        out[out_index].bar_index = static_cast<int>(out_index);
    }

    return out;
}

int BarAggregator::closed_count(symbol::SymbolId symbol_id) const
{
    const auto iterator = series_.find(symbol_id);
    return iterator == series_.end() ? 0 : static_cast<int>(iterator->second.closed.size());
}

BarSlot BarAggregator::current_slot(symbol::SymbolId symbol_id) const
{
    const auto iterator = series_.find(symbol_id);

    if (iterator == series_.end() || !iterator->second.live.active)
    {
        return BarSlot{};
    }

    return iterator->second.live.slot;
}

void BarAggregator::clear(symbol::SymbolId symbol_id)
{
    series_.erase(symbol_id);
}
} // namespace bars
