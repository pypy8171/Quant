// 신호 하나가 파이프라인을 지나는 구간 시각을 CSV 한 줄로 남긴다 — 틱 수신→신호→주문 큐 pop→라우터 반환.
// 주문 스레드 전용(단일 작성자). 측정이 먼저라는 원칙의 도구이고, 주문 건수가 하루 수십 건이라 줄마다 바로 쓴다. [why D-071]
#pragma once

#include "core/Types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace trace
{

// 한 신호의 네 시각(steady_clock nanoseconds). 0은 "그 지점을 안 지났다" — 예: REST 봉에서 난 신호는 tick_ns가 0.
struct Marks
{
    int64_t tick_ns   = 0; // 수신 스레드가 틱을 받은 시각
    int64_t signal_ns = 0; // 전략 스레드가 신호를 만든 시각(sequence stamp 지점)
    int64_t pop_ns    = 0; // 주문 스레드가 order_queue_에서 꺼낸 시각
    int64_t done_ns   = 0; // OrderRouter::submit이 돌아온 시각(게이트+HTTP)
};

// 구간을 us로. 앞 지점이 0이면 -1(측정 불가)로 둬 평균에 섞이지 않게 한다.
inline int64_t segment_us(int64_t from_ns, int64_t to_ns)
{
    if (from_ns == 0 || to_ns == 0)
    {
        return -1;
    }

    return (to_ns - from_ns) / 1000;
}

inline std::string_view csv_header()
{
    return "utc_ms,seq,ticker,strategy,side,action,tick_to_signal_us,signal_to_pop_us,pop_to_done_us,total_us,kis_called,"
           "accepted\n";
}

// utc_ms는 줄을 쓴 시각(system_clock). 문자열 필드에 쉼표가 들어올 일은 없다(종목코드·전략 id·enum 이름).
inline std::string csv_row(const OrderSignal& signal, const Marks& marks, bool kis_called, bool accepted, int64_t utc_ms)
{
    const int64_t first = marks.tick_ns != 0 ? marks.tick_ns : marks.signal_ns;
    std::string   text;
    text.reserve(160);
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
    text += kis_called ? '1' : '0';
    text += ',';
    text += accepted ? '1' : '0';
    text += '\n';
    return text;
}

// 구간 지연의 분포를 원자 버킷으로 모은다 — 주문 스레드가 넣고 데이터 스레드가 HEALTH를 만들 때 읽는다(락 없음).
// 옥타브(2배 구간)마다 8칸이라 분위수 오차는 칸 너비(12% 안쪽)이고, 기동 후 누적 분포다 — 구간 분포가
// 필요하면 읽는 쪽이 직전 값과 뺀다. 표본 하나가 원자 덧셈 두 번이라 주문 경로에 얹어도 잰 값이 흔들리지 않는다. [why D-071]
class LatencyHistogram
{
public:
    static constexpr int kSubBucketBits  = 3;                     // 옥타브당 2^3 = 8칸
    static constexpr int kSubBucketCount = 1 << kSubBucketBits;
    static constexpr int kOctaveCount    = 40;                    // 2^40us 약 13일까지. 넘치면 마지막 칸에 쌓인다
    static constexpr int kBucketCount    = kOctaveCount * kSubBucketCount;

    // 음수는 버린다 — segment_us가 "그 지점을 안 지났다"를 -1로 주기 때문이다.
    void add(int64_t microseconds) noexcept
    {
        if (microseconds < 0)
        {
            return;
        }

        buckets_[bucket_of(microseconds)].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t count() const noexcept
    {
        return count_.load(std::memory_order_relaxed);
    }

    // 그 분위수가 든 칸의 상한(us). 실제 값은 그 칸의 하한과 상한 사이다. 표본이 없으면 -1.
    [[nodiscard]] int64_t percentile(double ratio) const noexcept
    {
        const uint64_t total = count();

        if (total == 0)
        {
            return -1;
        }

        const uint64_t target = std::min<uint64_t>(static_cast<uint64_t>(ratio * static_cast<double>(total)) + 1, total);
        uint64_t       seen   = 0;

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

    // [formula] 칸 = (옥타브 - 2) x 8 + 최상위 비트 바로 아래 3비트. 8us 미만은 값 하나가 칸 하나다.
    [[nodiscard]] static int bucket_of(int64_t microseconds) noexcept
    {
        if (microseconds < kSubBucketCount)
        {
            return static_cast<int>(microseconds);
        }

        const int octave = 63 - std::countl_zero(static_cast<uint64_t>(microseconds));
        const int shift  = octave - kSubBucketBits;
        const int index  = (shift + 1) * kSubBucketCount + static_cast<int>((microseconds >> shift) & (kSubBucketCount - 1));
        return index < kBucketCount ? index : kBucketCount - 1;
    }

    [[nodiscard]] static int64_t upper_bound_of(int index) noexcept
    {
        if (index < kSubBucketCount)
        {
            return index;
        }

        const int     shift = index / kSubBucketCount - 1;
        const int64_t lower = static_cast<int64_t>(kSubBucketCount + index % kSubBucketCount) << shift;
        return lower + (static_cast<int64_t>(1) << shift) - 1;
    }

private:
    std::array<std::atomic<uint32_t>, kBucketCount> buckets_{};
    std::atomic<uint64_t>                           count_{0};
};

// 파이프라인 구간별 분포 한 벌. 엔진이 하나 들고, 주문 스레드가 LatencyTrace::record와 같은 자리에서 채운다.
struct PipelineLatency
{
    LatencyHistogram tick_to_signal;
    LatencyHistogram signal_to_pop;
    LatencyHistogram pop_to_done;
    LatencyHistogram total;

    void add(const Marks& marks) noexcept
    {
        const int64_t first = marks.tick_ns != 0 ? marks.tick_ns : marks.signal_ns;
        tick_to_signal.add(segment_us(marks.tick_ns, marks.signal_ns));
        signal_to_pop.add(segment_us(marks.signal_ns, marks.pop_ns));
        pop_to_done.add(segment_us(marks.pop_ns, marks.done_ns));
        total.add(segment_us(first, marks.done_ns));
    }
};

// 파일은 첫 줄을 쓸 때 연다(기동 때 주문이 없으면 파일도 없다). 열기에 실패하면 조용히 버린다 — 측정이 발주를 막지 않는다.
class LatencyTrace
{
public:
    explicit LatencyTrace(std::filesystem::path path) : path_(std::move(path)) {}

    void record(const OrderSignal& signal, const Marks& marks, bool kis_called, bool accepted)
    {
        if (!opened_)
        {
            open();
        }

        if (!out_.is_open())
        {
            return;
        }

        const auto utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        out_ << csv_row(signal, marks, kis_called, accepted, utc_ms);
        out_.flush();
        ++rows_;
    }

    [[nodiscard]] uint64_t rows() const noexcept
    {
        return rows_;
    }

private:
    void open()
    {
        opened_ = true;
        std::error_code error_code;
        const bool existed = std::filesystem::exists(path_, error_code) && std::filesystem::file_size(path_, error_code) > 0;
        out_.open(path_, std::ios::app);

        if (out_.is_open() && !existed)
        {
            out_ << csv_header();
        }
    }

    std::filesystem::path path_;
    std::ofstream         out_;
    bool                  opened_ = false;
    uint64_t              rows_   = 0;
};

inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace trace
