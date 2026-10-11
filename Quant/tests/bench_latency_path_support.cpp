// bench_latency_path 공용 준비의 몸통 — 분위수·표 출력·종목 코드·H0STCNT0 페이로드 생성.
#include "bench_latency_path_support.h"

namespace latency_path_bench
{

volatile int64_t g_sink = 0;

PercentileSummary percentiles(std::vector<double>& values)
{
    PercentileSummary percent;

    if (values.empty())
    {
        return percent;
    }

    std::sort(values.begin(), values.end());
    auto at = [&](double quantity)
    {
        return values[std::min(values.size() - 1, static_cast<size_t>(quantity * static_cast<double>(values.size())))];
    };
    percent.p50  = at(0.50);
    percent.p99  = at(0.99);
    percent.p999 = at(0.999);
    percent.max  = values.back();
    return percent;
}

void row(const char* name, double old_ns, double new_ns)
{
    const double ratio = new_ns > 0 ? old_ns / new_ns : 0.0;
    std::printf("  %-44s %9.1f  %9.1f  %6.2fx\n", name, old_ns, new_ns, ratio);
}

void print_row(const char* name, double nanoseconds)
{
    std::printf("  %-44s %9.1f\n", name, nanoseconds);
}

void head(const char* title)
{
    std::printf("\n== %s\n", title);
}

std::vector<std::string> make_tickers()
{
    std::vector<std::string> parts;
    parts.reserve(kSymbols);

    for (size_t symbol_index = 0; symbol_index < kSymbols; ++symbol_index)
    {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%06zu", 5930 + symbol_index * 7);
        parts.emplace_back(buffer);
    }

    return parts;
}

// [wire] H0STCNT0 페이로드 — 46필드를 '^'로 잇는다. 디코더가 읽는 자리(0·1·2·12·13·18·21)만 실값, 나머지는 0.
std::string make_kr_trade_payload(const std::string& ticker, int price)
{
    std::string text;

    for (int index = 0; index < 46; ++index)
    {
        if (index > 0)
        {
            text += '^';
        }

        switch (index)
        {
            case 0: text += ticker; break;
            case 1: text += "093001"; break;
            case 2: text += std::to_string(price); break;
            case 12: text += "150"; break;
            case 13: text += "1234567"; break;
            case 18: text += "120.55"; break;
            case 21: text += "1"; break;
            default: text += '0'; break;
        }
    }

    return text;
}

} // namespace latency_path_bench
