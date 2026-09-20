// 신호 하나가 파이프라인을 지나는 구간 시각을 CSV 한 줄로 남긴다 — 틱 수신→신호→주문 큐 pop→라우터 반환.
// 주문 스레드 전용(단일 작성자). 측정이 먼저라는 원칙의 도구이고, 주문 건수가 하루 수십 건이라 줄마다 바로 쓴다. [why D-071]
#pragma once

#include "core/Types.h"

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
