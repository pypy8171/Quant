// LatencyTrace 단위 테스트 — 구간 계산의 0 처리, CSV 행 형식, 파일 머리글이 한 번만 쓰이는지,
// 그리고 HEALTH가 싣는 LatencyHistogram의 칸 나눔·분위수.
// 빌드: cmake --build <directory> --target test_latency_trace
#include "core/LatencyTrace.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

std::vector<std::string> split(const std::string& line)
{
    std::vector<std::string> out;
    std::stringstream        stream(line);
    std::string              field;

    while (std::getline(stream, field, ','))
    {
        out.push_back(field);
    }

    return out;
}

} // namespace

int main()
{
    // 1. 구간: 앞 지점이 0이면 -1, 아니면 us.
    CHECK(trace::segment_us(0, 5000) == -1);
    CHECK(trace::segment_us(5000, 0) == -1);
    CHECK(trace::segment_us(1000, 4000) == 3);
    CHECK(trace::segment_us(1'000'000, 3'500'000) == 2500);

    // 2. 행 형식 — 열 12개, 값이 자리에 맞게 들어간다.
    OrderSignal signal;
    signal.sequence         = 42;
    signal.ticker      = "005930";
    signal.strategy_id = "devscale";
    signal.side        = OrderSide::BUY;
    signal.action      = OrderAction::NEW;

    trace::Marks marks;
    marks.tick_ns   = 10'000'000;
    marks.signal_ns = 10'050'000; // +50us
    marks.pop_ns    = 10'060'000; // +10us
    marks.done_ns   = 25'060'000; // +15,000us (HTTP)

    const OrderStageTiming stages{.gate_us          = 40,
                                  .history_guard_us = 70,
                                  .journal_us       = 900,
                                  .bucket_wait_us   = 150000,
                                  .transport_us     = 210000,
                                  .record_us        = 1800};

    const auto row = trace::csv_row(signal, marks, stages, true, true, 1'700'000'000'123LL);
    CHECK(row.back() == '\n');
    const auto fields = split(row.substr(0, row.size() - 1));
    CHECK(fields.size() == 18);
    CHECK(fields[0] == "1700000000123");
    CHECK(fields[1] == "42");
    CHECK(fields[2] == "005930");
    CHECK(fields[3] == "devscale");
    CHECK(fields[4] == "BUY");
    CHECK(fields[5] == "NEW");
    CHECK(fields[6] == "50");
    CHECK(fields[7] == "10");
    CHECK(fields[8] == "15000");
    CHECK(fields[9] == "15060");
    CHECK(fields[10] == "40");      // gate
    CHECK(fields[11] == "70");      // history_guard
    CHECK(fields[12] == "900");     // journal
    CHECK(fields[13] == "150000");  // bucket_wait
    CHECK(fields[14] == "210000");  // transport
    CHECK(fields[15] == "1800");    // record
    CHECK(fields[16] == "1");
    CHECK(fields[17] == "1");

    // 3. REST 봉 신호(tick_ns=0): 첫 구간 -1, total은 signal부터.
    marks.tick_ns = 0;
    const auto row2 = trace::csv_row(signal, marks, OrderStageTiming{}, false, false, 0);
    const auto fields_two   = split(row2.substr(0, row2.size() - 1));
    CHECK(fields_two[6] == "-1");
    CHECK(fields_two[9] == "15010");
    CHECK(fields_two[10] == "-1"); // 게이트 앞에서 끝난 주문은 라우터 구간이 전부 -1이다
    CHECK(fields_two[15] == "-1");
    CHECK(fields_two[16] == "0");
    CHECK(fields_two[17] == "0");

    // 4. 파일: 머리글은 새 파일에만, 두 번째 인스턴스가 이어 써도 머리글이 다시 안 붙는다.
    const auto path = std::filesystem::temp_directory_path() / "quant_test_latency_trace.csv";
    std::filesystem::remove(path);
    {
        trace::LatencyTrace latency_trace(path);
        CHECK(latency_trace.rows() == 0);
        latency_trace.record(signal, marks, stages, true, true);
        latency_trace.record(signal, marks, OrderStageTiming{}, false, false);
        CHECK(latency_trace.rows() == 2);
    }

    {
        trace::LatencyTrace latency_trace(path);
        latency_trace.record(signal, marks, stages, true, false);
    }

    std::ifstream            in(path);
    std::vector<std::string> lines;
    std::string              line;

    while (std::getline(in, line))
    {
        lines.push_back(line);
    }

    CHECK(lines.size() == 4);
    CHECK(lines[0].rfind("utc_ms,seq,", 0) == 0);
    CHECK(split(lines[1]).size() == 18);
    CHECK(split(lines[3])[16] == "1");
    CHECK(split(lines[3])[17] == "0");
    in.close(); // 열린 채로 지우면 Windows가 공유 위반을 내고 filesystem_error가 잡히지 않는다
    std::filesystem::remove(path);

    // 5. 히스토그램: 작은 값은 칸 하나가 값 하나, 큰 값은 칸 너비가 12% 안쪽.
    CHECK(trace::LatencyHistogram::bucket_of(0) == 0);
    CHECK(trace::LatencyHistogram::bucket_of(7) == 7);
    CHECK(trace::LatencyHistogram::upper_bound_of(trace::LatencyHistogram::bucket_of(7)) == 7);
    CHECK(trace::LatencyHistogram::upper_bound_of(trace::LatencyHistogram::bucket_of(1000)) >= 1000);
    CHECK(trace::LatencyHistogram::upper_bound_of(trace::LatencyHistogram::bucket_of(1000)) < 1000 * 113 / 100);

    trace::LatencyHistogram histogram;
    CHECK(histogram.count() == 0);
    CHECK(histogram.percentile(0.50) == -1);   // 표본이 없으면 -1
    histogram.add(-1);                         // 측정 불가는 버린다
    CHECK(histogram.count() == 0);

    for (int64_t microseconds = 1; microseconds <= 1000; ++microseconds)
    {
        histogram.add(microseconds);
    }

    CHECK(histogram.count() == 1000);
    // 칸 상한을 돌려주므로 참값 이상이되 칸 너비(12%) 안쪽이어야 한다.
    CHECK(histogram.percentile(0.50) >= 500 && histogram.percentile(0.50) < 500 * 113 / 100);
    CHECK(histogram.percentile(0.99) >= 990 && histogram.percentile(0.99) < 990 * 113 / 100);

    // 구간을 한 번에 채운다 — tick_ns가 0인 신호는 tick_to_signal만 표본이 안 잡힌다.
    trace::PipelineLatency pipeline_latency;
    pipeline_latency.add(trace::Marks{0, 1000, 3000, 5000, 9000}, OrderStageTiming{});
    CHECK(pipeline_latency.tick_to_signal.count() == 0);
    CHECK(pipeline_latency.signal_to_pop.count() == 1);
    CHECK(pipeline_latency.pop_to_send.count() == 1);      // 3000→5000ns = 호출 간격 조절 대기
    CHECK(pipeline_latency.pop_to_done.count() == 1);
    CHECK(pipeline_latency.total.count() == 1);
    CHECK(pipeline_latency.total.percentile(0.50) >= 8);   // 8000ns = 8us
    // 라우터 구간은 기본값 -1이라 표본이 안 잡힌다 — 게이트 앞에서 끝난 주문이 분포를 끌어내리지 않게.
    CHECK(pipeline_latency.gate.count() == 0);
    CHECK(pipeline_latency.transport.count() == 0);

    // 라우터가 값을 실어 오면 그 구간만 표본이 는다.
    pipeline_latency.add(trace::Marks{0, 1000, 3000, 5000, 9000},
                         OrderStageTiming{.gate_us          = 40,
                                          .history_guard_us = 70,
                                          .journal_us       = 900,
                                          .bucket_wait_us   = 150000,
                                          .transport_us     = 210000,
                                          .record_us        = 1800});
    CHECK(pipeline_latency.gate.count() == 1);
    CHECK(pipeline_latency.history_guard.count() == 1);
    CHECK(pipeline_latency.journal.count() == 1);
    CHECK(pipeline_latency.bucket_wait.count() == 1);
    CHECK(pipeline_latency.transport.count() == 1);
    CHECK(pipeline_latency.record.count() == 1);
    CHECK(pipeline_latency.transport.percentile(0.50) >= 210000);
    CHECK(pipeline_latency.record.percentile(0.50) >= 1800);

    // 이름 배열과 사본 배열은 같은 첨자로 짝지어 읽는다 — 구간을 늘릴 때 둘이 어긋나면 그라파나가
    //  다른 열에 값을 넣으면서도 아무 오류도 안 난다. 그래서 자리를 고정한다.
    const auto names = trace::PipelineLatency::segment_names();
    CHECK(names.size() == static_cast<size_t>(trace::PipelineLatency::kSegmentCount));
    CHECK(names[3] == "gate");
    CHECK(names[4] == "history_guard");
    CHECK(names[8] == "record");
    CHECK(names[9] == "pop_to_done");

    // 구간 분포: 사본을 뜬 뒤 들어온 표본만 잡힌다 — 누적 분위수와 달리 지난 값이 안 남는다. [why D-071]
    trace::PipelineSnapshot before;
    before.capture(pipeline_latency);
    const int transport_index = 7; // segment_names()의 "transport" 자리
    CHECK(trace::PipelineLatency::segment_names()[transport_index] == "transport");
    CHECK(trace::percentile_of_difference(before.segments[transport_index], before.segments[transport_index],
                                          0.50) == -1);

    pipeline_latency.add(trace::Marks{0, 1000, 3000, 5000, 9000}, OrderStageTiming{.transport_us = 3000});
    trace::PipelineSnapshot after;
    after.capture(pipeline_latency);
    // 누적은 210ms가 섞여 p99를 끌어올리지만, 구간은 방금 들어온 3ms만 본다.
    CHECK(pipeline_latency.transport.percentile(0.99) >= 210000);
    const int64_t interval_p99 =
        trace::percentile_of_difference(before.segments[transport_index], after.segments[transport_index], 0.99);
    CHECK(interval_p99 >= 3000 && interval_p99 < 3000 * 113 / 100);

    std::cout << "test_latency_trace: " << g_checks << " checks passed\n";
    return 0;
}
