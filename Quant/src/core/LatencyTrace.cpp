#include "core/LatencyTrace.h"

namespace
{
// CSV 한 줄 길이 어림 — 재할당을 한 번으로 줄이려는 값이다.
constexpr size_t kCsvRowReserve = 160;
} // namespace

namespace trace
{
int64_t segment_us(int64_t from_ns, int64_t to_ns)
{
    if (from_ns == 0 || to_ns == 0)
    {
        return -1;
    }

    return (to_ns - from_ns) / 1000;
}

std::string_view csv_header()
{
    // pop_to_done_us 뒤의 여섯 열이 그 한 덩이를 가른 몫이다 — 합이 pop_to_done_us에 거의 닿는다
    //  (남는 몫은 구간 사이 잔돈). 한 덩이만 있으면 느려진 자리를 못 짚는다. [why D-071]
    return "utc_ms,seq,ticker,strategy,side,action,tick_to_signal_us,signal_to_pop_us,pop_to_done_us,total_us,"
           "gate_us,history_guard_us,journal_us,bucket_wait_us,transport_us,record_us,open_orders_us,kis_"
           "called,accepted\n";
}

std::string csv_row(const OrderSignal& signal, const Marks& marks, const OrderStageTiming& stages, bool kis_called,
                    bool accepted, int64_t utc_ms)
{
    const int64_t first = marks.tick_ns != 0 ? marks.tick_ns : marks.signal_ns;
    std::string text;
    text.reserve(kCsvRowReserve);
    text += std::to_string(utc_ms);
    text += ',';
    text += std::to_string(signal.sequence);
    text += ',';
    text += signal.ticker;
    text += ',';
    text += signal.strategy_id;
    text += ',';
    text += signal.side == OrderSide::BUY ? "BUY" : (signal.side == OrderSide::SELL ? "SELL" : "NONE");
    text += ',';
    text += signal.action == OrderAction::NEW ? "NEW" : (signal.action == OrderAction::CANCEL ? "CANCEL" : "REPLACE");
    text += ',';
    text += std::to_string(segment_us(marks.tick_ns, marks.signal_ns));
    text += ',';
    text += std::to_string(segment_us(marks.signal_ns, marks.pop_ns));
    text += ',';
    text += std::to_string(segment_us(marks.pop_ns, marks.done_ns));
    text += ',';
    text += std::to_string(segment_us(first, marks.done_ns));
    text += ',';
    text += std::to_string(stages.gate_us);
    text += ',';
    text += std::to_string(stages.history_guard_us);
    text += ',';
    text += std::to_string(stages.journal_us);
    text += ',';
    text += std::to_string(stages.bucket_wait_us);
    text += ',';
    text += std::to_string(stages.transport_us);
    text += ',';
    text += std::to_string(stages.record_us);
    text += ',';
    text += std::to_string(stages.open_orders_us);
    text += ',';
    text += kis_called ? '1' : '0';
    text += ',';
    text += accepted ? '1' : '0';
    text += '\n';
    return text;
}

void LatencyHistogram::add(int64_t microseconds) noexcept
{
    if (microseconds < 0)
    {
        return;
    }

    buckets_[bucket_of(microseconds)].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
}

int64_t LatencyHistogram::percentile(double ratio) const noexcept
{
    const uint64_t total = count();

    if (total == 0)
    {
        return -1;
    }

    const uint64_t target = std::min<uint64_t>(static_cast<uint64_t>(ratio * static_cast<double>(total)) + 1, total);
    uint64_t seen = 0;

    for (int index = 0; index < kBucketCount; ++index)
    {
        seen += buckets_[index].load(std::memory_order_relaxed);

        if (seen >= target)
        {
            return upper_bound_of(index);
        }
    }

    return upper_bound_of(kBucketCount - 1);
}

int LatencyHistogram::bucket_of(int64_t microseconds) noexcept
{
    if (microseconds < kSubBucketCount)
    {
        return static_cast<int>(microseconds);
    }

    const int octave = 63 - std::countl_zero(static_cast<uint64_t>(microseconds));
    const int shift = octave - kSubBucketBits;
    const int index = (shift + 1) * kSubBucketCount + static_cast<int>((microseconds >> shift) & (kSubBucketCount - 1));
    return index < kBucketCount ? index : kBucketCount - 1;
}

int64_t LatencyHistogram::upper_bound_of(int index) noexcept
{
    if (index < kSubBucketCount)
    {
        return index;
    }

    const int shift = index / kSubBucketCount - 1;
    const int64_t lower = static_cast<int64_t>(kSubBucketCount + index % kSubBucketCount) << shift;
    return lower + (static_cast<int64_t>(1) << shift) - 1;
}

void LatencyHistogram::capture(HistogramSnapshot& out) const noexcept
{
    out.count = 0;

    for (int index = 0; index < kBucketCount; ++index)
    {
        out.buckets[index] = buckets_[index].load(std::memory_order_relaxed);
        out.count += out.buckets[index];
    }
}

int64_t percentile_of_difference(const HistogramSnapshot& older, const HistogramSnapshot& newer, double ratio) noexcept
{
    if (newer.count <= older.count)
    {
        return -1;
    }

    const uint64_t total = newer.count - older.count;
    const uint64_t target = std::min<uint64_t>(static_cast<uint64_t>(ratio * static_cast<double>(total)) + 1, total);
    uint64_t seen = 0;

    for (int index = 0; index < LatencyHistogram::kBucketCount; ++index)
    {
        // 사본을 뜨는 사이 버킷이 줄어들 일은 없다(덧셈만 한다) — 그래도 음수 방지로 크기를 확인한다.
        if (newer.buckets[index] > older.buckets[index])
        {
            seen += newer.buckets[index] - older.buckets[index];
        }

        if (seen >= target)
        {
            return LatencyHistogram::upper_bound_of(index);
        }
    }

    return LatencyHistogram::upper_bound_of(LatencyHistogram::kBucketCount - 1);
}

void PipelineLatency::add(const Marks& marks, const OrderStageTiming& stages) noexcept
{
    const int64_t first = marks.tick_ns != 0 ? marks.tick_ns : marks.signal_ns;
    tick_to_signal.add(segment_us(marks.tick_ns, marks.signal_ns));
    signal_to_pop.add(segment_us(marks.signal_ns, marks.pop_ns));
    pop_to_send.add(segment_us(marks.pop_ns, marks.send_ready_ns));
    gate.add(stages.gate_us);
    history_guard.add(stages.history_guard_us);
    journal.add(stages.journal_us);
    bucket_wait.add(stages.bucket_wait_us);
    transport.add(stages.transport_us);
    record.add(stages.record_us);
    open_orders.add(stages.open_orders_us);
    pop_to_done.add(segment_us(marks.pop_ns, marks.done_ns));
    total.add(segment_us(first, marks.done_ns));
}

std::array<std::string_view, PipelineLatency::kSegmentCount> PipelineLatency::segment_names() noexcept
{
    return {"tick_to_signal", "signal_to_pop", "pop_to_send", "gate",        "history_guard", "journal",
            "bucket_wait",    "transport",     "record",      "open_orders", "pop_to_done",   "total"};
}

void PipelineSnapshot::capture(const PipelineLatency& source) noexcept
{
    const std::array<const LatencyHistogram*, PipelineLatency::kSegmentCount> order{
        &source.tick_to_signal, &source.signal_to_pop, &source.pop_to_send, &source.gate,        &source.history_guard,
        &source.journal,        &source.bucket_wait,   &source.transport,   &source.record,      &source.open_orders,
        &source.pop_to_done,    &source.total};

    for (int index = 0; index < PipelineLatency::kSegmentCount; ++index)
    {
        order[index]->capture(segments[index]);
    }
}

void LatencyTrace::record(const OrderSignal& signal, const Marks& marks, const OrderStageTiming& stages,
                          bool kis_called, bool accepted)
{
    if (!opened_)
    {
        open();
    }

    if (!out_.is_open())
    {
        return;
    }

    const auto utc_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    out_ << csv_row(signal, marks, stages, kis_called, accepted, utc_ms);
    out_.flush();
    ++rows_;
}

void LatencyTrace::open()
{
    opened_ = true;
    std::error_code error_code;
    const bool existed =
        std::filesystem::exists(path_, error_code) && std::filesystem::file_size(path_, error_code) > 0;
    out_.open(path_, std::ios::app);

    if (out_.is_open() && !existed)
    {
        out_ << csv_header();
    }
}

int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace trace
