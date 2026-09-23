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
    int64_t pop_ns        = 0; // 주문 스레드가 pipeline_.requests에서 꺼낸 시각
    int64_t send_ready_ns = 0; // 호출 간격 조절(OrderRateLimiter) sleep이 끝난 시각 — 우리가 스스로 줄 세운 몫의 끝
    int64_t done_ns       = 0; // OrderRouter::submit이 돌아온 시각(게이트+원장+HTTP)
};

// 라우터 안 구간은 OrderStageTiming(core/Types.h) — OrderRouter가 채워 ManagedOrder로 돌려주고 여기서 분포에 넣는다.

// 구간을 us로. 앞 지점이 0이면 -1(측정 불가)로 둬 평균에 섞이지 않게 한다.
int64_t segment_us(int64_t from_ns, int64_t to_ns);

std::string_view csv_header();

// utc_ms는 줄을 쓴 시각(system_clock). 문자열 필드에 쉼표가 들어올 일은 없다(종목코드·전략 id·enum 이름).
std::string csv_row(const OrderSignal& signal, const Marks& marks, const OrderStageTiming& stages, bool kis_called,
                    bool accepted, int64_t utc_ms);

// 구간 지연의 분포를 원자 버킷으로 모은다 — 주문 스레드가 넣고 데이터 스레드가 HEALTH를 만들 때 읽는다(락 없음).
// 옥타브(2배 구간)마다 8칸이라 분위수 오차는 칸 너비(12.5% 이내)이고, 기동 후 누적 분포다 — 구간 분포가
// 필요하면 읽는 쪽이 직전 값과 뺀다. 표본 하나가 원자 덧셈 두 번이라 주문 경로에 얹어도 잰 값이 흔들리지 않는다. [why D-071]
struct HistogramSnapshot;

class LatencyHistogram
{
public:
    static constexpr int kSubBucketBits  = 3;                     // 옥타브당 2^3 = 8칸
    static constexpr int kSubBucketCount = 1 << kSubBucketBits;
    static constexpr int kOctaveCount    = 40;                    // 행 = 옥타브-2라 마지막 행은 2^42us(약 51일)까지. 넘치면 마지막 칸에 쌓인다
    static constexpr int kBucketCount    = kOctaveCount * kSubBucketCount;

    // 음수는 버린다 — segment_us가 "그 지점을 안 지났다"를 -1로 주기 때문이다.
    void add(int64_t microseconds) noexcept;

    [[nodiscard]] uint64_t count() const noexcept
    {
        return count_.load(std::memory_order_relaxed);
    }

    // 그 분위수가 든 칸의 상한(us). 실제 값은 그 칸의 하한과 상한 사이다. 표본이 없으면 -1.
    [[nodiscard]] int64_t percentile(double ratio) const noexcept;

    // 버킷을 통째로 베낀다. 베끼는 중에 들어온 표본은 이번 사본과 다음 사본 중 한쪽에만 들어간다(칸마다 원자 읽기).
    //  [inv] 사본의 count는 칸 합이지 count_가 아니다 — count_를 따로 읽으면 칸 합과 어긋나 분위수가 빗나간다.
    void capture(HistogramSnapshot& out) const noexcept;

    // [formula] 칸 = (옥타브 - 2) x 8 + 최상위 비트 바로 아래 3비트. 8us 미만은 값 하나가 칸 하나다.
    [[nodiscard]] static int bucket_of(int64_t microseconds) noexcept;

    [[nodiscard]] static int64_t upper_bound_of(int index) noexcept;

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

// 두 사본 사이에 들어온 표본만의 분위수(칸 상한, us). 그 사이에 표본이 없으면 -1.
// 기동 후 누적 분위수는 한 번 튀면 내려오지 않아 "언제 느려졌나"를 못 본다 — 그래서 뺀다. [why D-071]
int64_t percentile_of_difference(const HistogramSnapshot& older, const HistogramSnapshot& newer,
                                        double ratio) noexcept;

// 파이프라인 구간별 분포 한 벌. 엔진이 하나 들고, 주문 스레드가 LatencyTrace::record와 같은 자리에서 채운다.
// 구간 이름은 health 테이블 열 이름의 앞머리와 같다 — 이름을 한 군데서만 고치게. [wire] PYQuant/db/client.py
struct PipelineLatency
{
    LatencyHistogram tick_to_signal; // 틱 수신 → 신호
    LatencyHistogram signal_to_pop;  // 신호 → 주문 큐에서 꺼냄
    LatencyHistogram pop_to_send;    // 꺼냄 → 호출 간격 조절 끝(우리가 스스로 줄 세운 시간)
    LatencyHistogram gate;           // 주문 게이트 판정(아래 이력 가드 몰을 벙 것)
    LatencyHistogram history_guard;  // 주문 이력 잠금·중복 가드 훑기(선형 탐색)
    LatencyHistogram journal;        // 원장 선기록(디스크)
    LatencyHistogram bucket_wait;    // 증권사 초당한도 버킷 줄서기
    LatencyHistogram transport;      // 증권사 REST 왕복
    LatencyHistogram record;         // 전송 뒤 마무리 — 접수 확정·발행·이력 저장·원장 CSV·미결주문 파일
    LatencyHistogram open_orders;    // 그중 미결주문 파일 다시쓰기 — record 안에 든 몫이라 합산에서 뺀다
    LatencyHistogram pop_to_done;    // 꺼냄 → 라우터 반환(위 여섯 + 발주 간격 대기를 품은 한 덩이)
    LatencyHistogram total;          // 틱 수신 → 라우터 반환

    static constexpr int kSegmentCount = 12;

    void add(const Marks& marks, const OrderStageTiming& stages) noexcept;

    // [inv] 반환 배열의 순서·이름은 PipelineSnapshot::capture와 같다 — 둘을 같은 첨자로 짝지어 읽는다.
    [[nodiscard]] static std::array<std::string_view, kSegmentCount> segment_names() noexcept;
};

// 한 시점의 열두 구간 사본. 데이터 스레드가 HEALTH를 만들 때 뜨고, 직전 것과 빼 구간 분위수를 낸다.
struct PipelineSnapshot
{
    std::array<HistogramSnapshot, PipelineLatency::kSegmentCount> segments{};

    void capture(const PipelineLatency& source) noexcept;
};

// 파일은 첫 줄을 쓸 때 연다(기동 때 주문이 없으면 파일도 없다). 열기에 실패하면 조용히 버린다 — 측정이 발주를 막지 않는다.
class LatencyTrace
{
public:
    explicit LatencyTrace(std::filesystem::path path) : path_(std::move(path)) {}

    void record(const OrderSignal& signal, const Marks& marks, const OrderStageTiming& stages, bool kis_called,
                bool accepted);

    [[nodiscard]] uint64_t rows() const noexcept
    {
        return rows_;
    }

private:
    void open();

    std::filesystem::path path_;
    std::ofstream         out_;
    bool                  opened_ = false;
    uint64_t              rows_   = 0;
};

int64_t now_ns();

} // namespace trace
