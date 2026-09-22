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

// 한 신호의 다섯 시각(steady_clock nanoseconds). 0은 "그 지점을 안 지났다" — 예: REST 봉에서 난 신호는 tick_ns가 0.
struct Marks
{
    int64_t tick_ns       = 0; // 수신 스레드가 틱을 받은 시각
    int64_t signal_ns     = 0; // 전략 스레드가 신호를 만든 시각(sequence stamp 지점)
    int64_t pop_ns        = 0; // 주문 스레드가 order_queue_에서 꺼낸 시각
    int64_t send_ready_ns = 0; // 호출 간격 조절(OrderRateLimiter) sleep이 끝난 시각 — 우리가 스스로 줄 세운 몫의 끝
    int64_t done_ns       = 0; // OrderRouter::submit이 돌아온 시각(게이트+원장+HTTP)
};

// 라우터 안 구간은 OrderStageTiming(core/Types.h) — OrderRouter가 채워 ManagedOrder로 돌려주고 여기서 분포에 넣는다.

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
struct HistogramSnapshot;

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

    // 버킷을 통째로 베낀다. 베끼는 중에 들어온 표본은 이번 사본과 다음 사본 중 한쪽에만 들어간다(칸마다 원자 읽기).
    //  [inv] 사본의 count는 칸 합이지 count_가 아니다 — count_를 따로 읽으면 칸 합과 어긋나 분위수가 빗나간다.
    void capture(HistogramSnapshot& out) const noexcept;

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

// 누적 버킷을 통째로 뜬 사본. 직전 사본과 빼면 그 사이에 들어온 표본만의 분포가 나온다 — 분위수끼리는 빼지지 않는다.
// 읽는 쪽(데이터 스레드)만 들고 있어 쓰는 쪽과 겹치지 않는다. [why D-071]
struct HistogramSnapshot
{
    std::array<uint32_t, LatencyHistogram::kBucketCount> buckets{};
    uint64_t                                            count = 0; // 칸 합 — LatencyHistogram::count()가 아니다
};

inline void LatencyHistogram::capture(HistogramSnapshot& out) const noexcept
{
    out.count = 0;

    for (int index = 0; index < kBucketCount; ++index)
    {
        out.buckets[index] = buckets_[index].load(std::memory_order_relaxed);
        out.count += out.buckets[index];
    }
}

// 두 사본 사이에 들어온 표본만의 분위수(칸 상한, us). 그 사이에 표본이 없으면 -1.
// 기동 후 누적 분위수는 한 번 튀면 내려오지 않아 "언제 느려졌나"를 못 본다 — 그래서 뺀다. [why D-071]
inline int64_t percentile_of_difference(const HistogramSnapshot& older, const HistogramSnapshot& newer,
                                        double ratio) noexcept
{
    if (newer.count <= older.count)
    {
        return -1;
    }

    const uint64_t total  = newer.count - older.count;
    const uint64_t target = std::min<uint64_t>(static_cast<uint64_t>(ratio * static_cast<double>(total)) + 1, total);
    uint64_t       seen   = 0;

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

// 파이프라인 구간별 분포 한 벌. 엔진이 하나 들고, 주문 스레드가 LatencyTrace::record와 같은 자리에서 채운다.
// 구간 이름은 health 테이블 열 이름의 앞머리와 같다 — 이름을 한 군데서만 고치게. [wire] PYQuant/db/client.py
struct PipelineLatency
{
    LatencyHistogram tick_to_signal; // 틱 수신 → 신호
    LatencyHistogram signal_to_pop;  // 신호 → 주문 큐에서 꺼냄
    LatencyHistogram pop_to_send;    // 꺼냄 → 호출 간격 조절 끝(우리가 스스로 줄 세운 시간)
    LatencyHistogram gate;           // 주문 게이트 판정
    LatencyHistogram journal;        // 원장 선기록(디스크)
    LatencyHistogram bucket_wait;    // 증권사 초당한도 버킷 줄서기
    LatencyHistogram transport;      // 증권사 REST 왕복
    LatencyHistogram pop_to_done;    // 꺼냄 → 라우터 반환(위 다섯을 다 품은 한 덩이)
    LatencyHistogram total;          // 틱 수신 → 라우터 반환

    static constexpr int kSegmentCount = 9;

    void add(const Marks& marks, const OrderStageTiming& stages) noexcept
    {
        const int64_t first = marks.tick_ns != 0 ? marks.tick_ns : marks.signal_ns;
        tick_to_signal.add(segment_us(marks.tick_ns, marks.signal_ns));
        signal_to_pop.add(segment_us(marks.signal_ns, marks.pop_ns));
        pop_to_send.add(segment_us(marks.pop_ns, marks.send_ready_ns));
        gate.add(stages.gate_us);
        journal.add(stages.journal_us);
        bucket_wait.add(stages.bucket_wait_us);
        transport.add(stages.transport_us);
        pop_to_done.add(segment_us(marks.pop_ns, marks.done_ns));
        total.add(segment_us(first, marks.done_ns));
    }

    // [inv] 반환 배열의 순서·이름은 PipelineSnapshot::capture와 같다 — 둘을 같은 첨자로 짝지어 읽는다.
    [[nodiscard]] static std::array<std::string_view, kSegmentCount> segment_names() noexcept
    {
        return {"tick_to_signal", "signal_to_pop", "pop_to_send", "gate",  "journal",
                "bucket_wait",    "transport",     "pop_to_done", "total"};
    }
};

// 한 시점의 아홉 구간 사본. 데이터 스레드가 HEALTH를 만들 때 뜨고, 직전 것과 빼 구간 분위수를 낸다.
struct PipelineSnapshot
{
    std::array<HistogramSnapshot, PipelineLatency::kSegmentCount> segments{};

    void capture(const PipelineLatency& source) noexcept
    {
        const std::array<const LatencyHistogram*, PipelineLatency::kSegmentCount> order{
            &source.tick_to_signal, &source.signal_to_pop, &source.pop_to_send, &source.gate,  &source.journal,
            &source.bucket_wait,    &source.transport,     &source.pop_to_done, &source.total};

        for (int index = 0; index < PipelineLatency::kSegmentCount; ++index)
        {
            order[index]->capture(segments[index]);
        }
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
