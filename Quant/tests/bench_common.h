#pragma once
// 벤치 공용 도우미 — 지연 분위수 요약·나노초 표기·빌드 종류·`--key value` 인자 읽기.
//  bench_feed_ingest·bench_market_firehose·Quant/tools/feed_latency_measure.cpp가 같은 몸통을 따로 들고 있던 것을
//  한 벌로 모았다. 분위수 정의(정렬 뒤 fraction × (n−1) 자리를 내림)는 옮기기 전과 같다 — 바꾸면 이전 측정과
//  숫자를 비교할 수 없게 된다.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bench
{

struct PercentileSummary
{
    int64_t p50 = 0, p99 = 0, p999 = 0, max_value = 0;
    size_t  count = 0;
};

// values를 제자리에서 정렬한 뒤 p50·p99·p999·최댓값을 낸다. 비어 있으면 count만 0인 요약.
PercentileSummary percentiles(std::vector<int64_t>& values);

// 나노초 → "123 ns" / "1.23 us" / "1.23 ms".
std::string format_ns(int64_t nanoseconds);

// NDEBUG 여부로 "Release (NDEBUG)" 또는 Debug 경고 문구.
const char* build_type();

// argv[2]부터(argv[1]은 실행 모드) `key value` 쌍을 찾는다. 없으면 default_value.
std::string argument_string(int argc, char** argv, const char* key, const std::string& default_value);
int64_t     argument_int64(int argc, char** argv, const char* key, int64_t default_value);
double      argument_double(int argc, char** argv, const char* key, double default_value);

} // namespace bench
