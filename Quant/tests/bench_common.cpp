#include "bench_common.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bench
{

PercentileSummary percentiles(std::vector<int64_t>& values)
{
    PercentileSummary summary;
    summary.count = values.size();

    if (values.empty())
    {
        return summary;
    }

    std::sort(values.begin(), values.end());
    auto at = [&](double fraction)
    {
        return values[static_cast<size_t>(fraction * (values.size() - 1))];
    };
    summary.p50       = at(0.50);
    summary.p99       = at(0.99);
    summary.p999      = at(0.999);
    summary.max_value = values.back();
    return summary;
}

std::string format_ns(int64_t nanoseconds)
{
    char text[32];

    if (nanoseconds < 1000)
    {
        std::snprintf(text, sizeof(text), "%lld ns", static_cast<long long>(nanoseconds));
    }
    else if (nanoseconds < 1'000'000)
    {
        std::snprintf(text, sizeof(text), "%.2f us", nanoseconds / 1000.0);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%.2f ms", nanoseconds / 1'000'000.0);
    }

    return std::string(text);
}

const char* build_type()
{
#ifdef NDEBUG
    return "Release (NDEBUG)";
#else
    return "Debug (⚠ 측정 무의미 — Release로 재빌드)";
#endif
}

std::string argument_string(int argc, char** argv, const char* key, const std::string& default_value)
{
    for (int index = 2; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], key) == 0)
        {
            return argv[index + 1];
        }
    }

    return default_value;
}

int64_t argument_int64(int argc, char** argv, const char* key, int64_t default_value)
{
    const std::string text = argument_string(argc, argv, key, "");
    return text.empty() ? default_value : std::atoll(text.c_str());
}

double argument_double(int argc, char** argv, const char* key, double default_value)
{
    const std::string text = argument_string(argc, argv, key, "");
    return text.empty() ? default_value : std::atof(text.c_str());
}

} // namespace bench
