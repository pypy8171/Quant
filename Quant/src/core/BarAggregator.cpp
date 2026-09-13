// N분봉 집계기 — 전략 스레드 소유, 락 없음. 확정은 다음 버킷 첫 틱이 한다(타이머 없음). [why D-068]
#include "core/BarAggregator.h"

#include "core/KstTime.h"

#include <algorithm>
#include <cctype>

namespace bars
{
namespace
{
bool six_digits(const std::string& s)
{
    if (s.size() != 6)
    {
        return false;
    }

    for (char c : s)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    return true;
}

int64_t day_key(const struct tm& t)
{
    return static_cast<int64_t>(t.tm_year) * 400 + t.tm_yday;
}
} // namespace

BarSlot slot_of(const std::string& hhmmss, std::time_t recv_utc, int interval_min, int open_hhmm, int close_hhmm)
{
    BarSlot out;

    if (interval_min < 1)
    {
        return out;
    }

    const struct tm k = kst::to_tm(recv_utc);
    int             hh = k.tm_hour;
    int             mm = k.tm_min;

    if (six_digits(hhmmss))
    {
        hh = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
        mm = (hhmmss[2] - '0') * 10 + (hhmmss[3] - '0');
    }

    const int hhmm = hh * 100 + mm;

    if (hh > 23 || mm > 59 || hhmm < open_hhmm || hhmm > close_hhmm)
    {
        return out;
    }

    out.day    = day_key(k);
    out.bucket = (hh * 60 + mm) / interval_min;
    return out;
}

std::chrono::system_clock::time_point slot_start(const BarSlot& s, std::time_t recv_utc, int interval_min)
{
    const struct tm   k          = kst::to_tm(recv_utc);
    const std::time_t day_start  = recv_utc - (k.tm_hour * 3600 + k.tm_min * 60 + k.tm_sec);
    const std::time_t bar_start  = day_start + static_cast<std::time_t>(s.bucket) * interval_min * 60;
    return std::chrono::system_clock::from_time_t(bar_start);
}

std::vector<MarketData> resample(const std::vector<MarketData>& bars_1m, int interval_min, int max_count)
{
    std::vector<MarketData> out;

    if (bars_1m.empty())
    {
        return out;
    }

    const size_t want = max_count > 0 ? static_cast<size_t>(max_count) : bars_1m.size();

    if (interval_min <= 1)
    {
        out.assign(bars_1m.begin(), bars_1m.begin() + static_cast<std::ptrdiff_t>((std::min)(want, bars_1m.size())));

        for (size_t i = 0; i < out.size(); ++i)
        {
            out[i].bar_index = static_cast<int>(i);
        }

        return out;
    }

    // 과거→최신으로 걸으며 같은 자리를 접는다. 자리는 봉 timestamp의 KST 분에서 온다 — 집계기 1분봉(자리 시작
    //  시각)과 REST 1분봉(그 분의 라벨) 모두 같은 분을 가리킨다.
    std::vector<MarketData> asc;
    BarSlot                 cur;

    for (auto it = bars_1m.rbegin(); it != bars_1m.rend(); ++it)
    {
        const std::time_t ts = std::chrono::system_clock::to_time_t(it->timestamp);
        const struct tm   k  = kst::to_tm(ts);
        BarSlot           slot;
        slot.day    = day_key(k);
        slot.bucket = (k.tm_hour * 60 + k.tm_min) / interval_min;

        if (asc.empty() || slot != cur)
        {
            asc.push_back(*it);
            cur = slot;
            continue;
        }

        MarketData& md = asc.back();
        md.high        = (std::max)(md.high, it->high); // (): windows.h max 매크로 회피
        md.low         = (std::min)(md.low, it->low);
        md.close       = it->close;
        md.volume += it->volume;
        md.timestamp = it->timestamp;
    }

    out.reserve((std::min)(want, asc.size()));

    for (auto it = asc.rbegin(); it != asc.rend() && out.size() < want; ++it)
    {
        out.push_back(*it);
        out.back().bar_index = static_cast<int>(out.size() - 1);
    }

    return out;
}

BarAggregator::BarAggregator(Config cfg) : cfg_(cfg)
{
    if (cfg_.interval_min < 1)
    {
        cfg_.interval_min = 1;
    }

    if (cfg_.keep < 1)
    {
        cfg_.keep = 1;
    }
}

void BarAggregator::close_live(Series& s)
{
    if (!s.live.active)
    {
        return;
    }

    s.closed.push_front(s.live.md);
    s.slots.push_front(s.live.slot);
    s.live.active    = false;
    s.live.acml_base = -1;
    trim(s);

    if (sink_)
    {
        sink_(s.closed.front());
    }
}

void BarAggregator::trim(Series& s)
{
    while (static_cast<int>(s.closed.size()) > cfg_.keep)
    {
        s.closed.pop_back();
        s.slots.pop_back();
    }
}

bool BarAggregator::on_tick(const TradeData& td)
{
    if (td.price <= 0.0 || td.ticker.empty())
    {
        return false;
    }

    const std::time_t recv = std::chrono::system_clock::to_time_t(td.timestamp);
    const BarSlot     slot = slot_of(td.time, recv, cfg_.interval_min, cfg_.session_open, cfg_.session_close);

    if (!slot.valid())
    {
        return false;
    }

    Series& s = series_[td.ticker];

    if (s.live.active && slot < s.live.slot)
    {
        return false; // 늦게 온 과거 틱 — 닫힌 봉의 종가를 고칠 순 없다
    }

    if (!s.slots.empty() && slot < s.slots.front())
    {
        return false; // 시드가 이미 닫아 둔 자리보다 오래된 틱
    }

    if (s.live.active && slot != s.live.slot)
    {
        close_live(s);
    }

    if (!s.live.active)
    {
        // [inv] 거래량은 누적차다 — 첫 틱의 acml − qty가 버킷 시작값. acml이 없으면(REST 대체 틱) qty 합산.
        s.live.active    = true;
        s.live.slot      = slot;
        s.live.acml_base = td.acml_volume > 0 ? td.acml_volume - td.quantity : -1;
        MarketData& md   = s.live.md;

        if (!s.slots.empty() && s.slots.front() == slot)
        {
            // 시드가 같은 자리를 닫힌 봉으로 넣어 뒀다(REST가 진행 중 봉을 돌려준 경우). 그 위에 이어 붙인다.
            md = s.closed.front();
            s.closed.pop_front();
            s.slots.pop_front();
            md.high  = (std::max)(md.high, td.price);
            md.low   = (std::min)(md.low, td.price);
            md.close = td.price;
            md.volume += td.quantity;

            if (s.live.acml_base >= 0)
            {
                s.live.acml_base = td.acml_volume - md.volume;
            }

            return true;
        }

        md               = MarketData{};
        md.ticker        = td.ticker;
        md.market        = td.market;
        md.open = md.high = md.low = md.close = td.price;
        md.volume        = td.quantity;
        md.timestamp     = slot_start(slot, recv, cfg_.interval_min);
        md.bar_index     = 0;
        return true;
    }

    MarketData& md = s.live.md;
    md.high        = (std::max)(md.high, td.price); // (): windows.h max 매크로 회피
    md.low         = (std::min)(md.low, td.price);
    md.close       = td.price;

    if (s.live.acml_base >= 0 && td.acml_volume > 0)
    {
        md.volume = (std::max)(md.volume, td.acml_volume - s.live.acml_base);
    }
    else
    {
        md.volume += td.quantity;
    }

    return true;
}

int BarAggregator::seed(const std::string& ticker, const std::vector<MarketData>& rest_bars)
{
    if (ticker.empty() || rest_bars.empty())
    {
        return 0;
    }

    Series& s     = series_[ticker];
    int     added = 0;

    for (const MarketData& rb : rest_bars)
    {
        const std::time_t ts = std::chrono::system_clock::to_time_t(rb.timestamp);

        if (ts <= 0)
        {
            continue;
        }

        // REST 봉의 timestamp는 버킷의 마지막 1분 시각이라 같은 버킷에 든다. 장 시간 필터는 시드에 걸지 않는다.
        const BarSlot slot = slot_of(kst::hhmmss(ts), ts, cfg_.interval_min, 0, 2359);

        if (!slot.valid())
        {
            continue;
        }

        if (s.live.active && slot == s.live.slot)
        {
            MarketData&   md      = s.live.md;
            const int64_t old_vol = md.volume;
            md.open               = rb.open;
            md.high               = (std::max)(md.high, rb.high);
            md.low                = (std::min)(md.low, rb.low);
            md.volume             = (std::max)(old_vol, rb.volume);

            // 누적차 기준을 옮겨 다음 틱부터 합친 거래량 위에 쌓이게 한다(마지막 acml = base + old_vol).
            if (s.live.acml_base >= 0)
            {
                s.live.acml_base += old_vol - md.volume;
            }

            continue;
        }

        if (s.live.active && s.live.slot < slot)
        {
            // 시드가 진행 중 봉보다 새 자리를 가졌다 — 로컬이 그 사이 틱을 못 본 것이다. 진행 봉을 닫고 이어 붙인다.
            close_live(s);
        }

        auto it = std::lower_bound(s.slots.begin(), s.slots.end(), slot,
                                   [](const BarSlot& a, const BarSlot& b) { return b < a; }); // 내림차순

        const size_t idx = static_cast<size_t>(it - s.slots.begin());

        if (it != s.slots.end() && *it == slot)
        {
            s.closed[idx] = rb; // 닫힌 자리는 REST가 이긴다
        }
        else
        {
            s.slots.insert(it, slot);
            s.closed.insert(s.closed.begin() + static_cast<std::ptrdiff_t>(idx), rb);
            ++added;
        }
    }

    for (size_t i = 0; i < s.closed.size(); ++i)
    {
        s.closed[i].ticker    = ticker;
        s.closed[i].bar_index = 0; // snapshot이 다시 매긴다
    }

    trim(s);
    return added;
}

std::vector<MarketData> BarAggregator::snapshot(const std::string& ticker, int max_count) const
{
    std::vector<MarketData> out;
    const auto              it = series_.find(ticker);

    if (it == series_.end())
    {
        return out;
    }

    const Series& s = it->second;
    const size_t  want = max_count > 0 ? static_cast<size_t>(max_count) : s.closed.size() + 1;
    out.reserve((std::min)(want, s.closed.size() + 1));

    if (s.live.active)
    {
        out.push_back(s.live.md);
    }

    for (const MarketData& md : s.closed)
    {
        if (out.size() >= want)
        {
            break;
        }

        out.push_back(md);
    }

    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i].bar_index = static_cast<int>(i);
    }

    return out;
}

int BarAggregator::closed_count(const std::string& ticker) const
{
    const auto it = series_.find(ticker);
    return it == series_.end() ? 0 : static_cast<int>(it->second.closed.size());
}

BarSlot BarAggregator::current_slot(const std::string& ticker) const
{
    const auto it = series_.find(ticker);

    if (it == series_.end() || !it->second.live.active)
    {
        return BarSlot{};
    }

    return it->second.live.slot;
}

void BarAggregator::clear(const std::string& ticker)
{
    series_.erase(ticker);
}
} // namespace bars
