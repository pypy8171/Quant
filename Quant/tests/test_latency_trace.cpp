// LatencyTrace 단위 테스트 — 구간 계산의 0 처리, CSV 행 형식, 파일 머리글이 한 번만 쓰이는지.
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

    const auto row = trace::csv_row(signal, marks, true, true, 1'700'000'000'123LL);
    CHECK(row.back() == '\n');
    const auto fields = split(row.substr(0, row.size() - 1));
    CHECK(fields.size() == 12);
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
    CHECK(fields[10] == "1");
    CHECK(fields[11] == "1");

    // 3. REST 봉 신호(tick_ns=0): 첫 구간 -1, total은 signal부터.
    marks.tick_ns = 0;
    const auto row2 = trace::csv_row(signal, marks, false, false, 0);
    const auto fields_two   = split(row2.substr(0, row2.size() - 1));
    CHECK(fields_two[6] == "-1");
    CHECK(fields_two[9] == "15010");
    CHECK(fields_two[10] == "0");
    CHECK(fields_two[11] == "0");

    // 4. 파일: 머리글은 새 파일에만, 두 번째 인스턴스가 이어 써도 머리글이 다시 안 붙는다.
    const auto path = std::filesystem::temp_directory_path() / "quant_test_latency_trace.csv";
    std::filesystem::remove(path);
    {
        trace::LatencyTrace latency_trace(path);
        CHECK(latency_trace.rows() == 0);
        latency_trace.record(signal, marks, true, true);
        latency_trace.record(signal, marks, false, false);
        CHECK(latency_trace.rows() == 2);
    }

    {
        trace::LatencyTrace latency_trace(path);
        latency_trace.record(signal, marks, true, false);
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
    CHECK(split(lines[1]).size() == 12);
    CHECK(split(lines[3])[10] == "1");
    CHECK(split(lines[3])[11] == "0");
    in.close(); // 열린 채로 지우면 Windows가 공유 위반을 내고 filesystem_error가 잡히지 않는다
    std::filesystem::remove(path);

    std::cout << "test_latency_trace: " << g_checks << " checks passed\n";
    return 0;
}
