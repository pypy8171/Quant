// LatencyTrace 단위 테스트 — 구간 계산의 0 처리, CSV 행 형식, 파일 머리글이 한 번만 쓰이는지.
// 빌드: cmake --build <dir> --target test_latency_trace
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

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

std::vector<std::string> split(const std::string& line)
{
    std::vector<std::string> out;
    std::stringstream        ss(line);
    std::string              f;

    while (std::getline(ss, f, ','))
    {
        out.push_back(f);
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
    OrderSignal sig;
    sig.seq         = 42;
    sig.ticker      = "005930";
    sig.strategy_id = "devscale";
    sig.side        = OrderSide::BUY;
    sig.action      = OrderAction::NEW;

    trace::Marks m;
    m.tick_ns   = 10'000'000;
    m.signal_ns = 10'050'000; // +50us
    m.pop_ns    = 10'060'000; // +10us
    m.done_ns   = 25'060'000; // +15,000us (HTTP)

    const auto row = trace::csv_row(sig, m, true, true, 1'700'000'000'123LL);
    CHECK(row.back() == '\n');
    const auto f = split(row.substr(0, row.size() - 1));
    CHECK(f.size() == 12);
    CHECK(f[0] == "1700000000123");
    CHECK(f[1] == "42");
    CHECK(f[2] == "005930");
    CHECK(f[3] == "devscale");
    CHECK(f[4] == "BUY");
    CHECK(f[5] == "NEW");
    CHECK(f[6] == "50");
    CHECK(f[7] == "10");
    CHECK(f[8] == "15000");
    CHECK(f[9] == "15060");
    CHECK(f[10] == "1");
    CHECK(f[11] == "1");

    // 3. REST 봉 신호(tick_ns=0): 첫 구간 -1, total은 signal부터.
    m.tick_ns = 0;
    const auto row2 = trace::csv_row(sig, m, false, false, 0);
    const auto f2   = split(row2.substr(0, row2.size() - 1));
    CHECK(f2[6] == "-1");
    CHECK(f2[9] == "15010");
    CHECK(f2[10] == "0");
    CHECK(f2[11] == "0");

    // 4. 파일: 머리글은 새 파일에만, 두 번째 인스턴스가 이어 써도 머리글이 다시 안 붙는다.
    const auto path = std::filesystem::temp_directory_path() / "quant_test_latency_trace.csv";
    std::filesystem::remove(path);
    {
        trace::LatencyTrace t(path);
        CHECK(t.rows() == 0);
        t.record(sig, m, true, true);
        t.record(sig, m, false, false);
        CHECK(t.rows() == 2);
    }

    {
        trace::LatencyTrace t(path);
        t.record(sig, m, true, false);
    }

    std::ifstream            in(path);
    std::vector<std::string> lines;
    std::string              l;

    while (std::getline(in, l))
    {
        lines.push_back(l);
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
