#pragma once
// bench_engine_load 공용 준비 — 실행 옵션·결과 구조체와 입력 준비·한 구성 실행 함수 선언.
//  본문은 bench_engine_load_<주제>.cpp에 있고, bench_engine_load.cpp의 main이 옵션을 읽어 구성마다 부른다.

#include "core/Engine.h"
#include "strategy/StrategyBase.h"
#include "strategy/IntradayBreakoutStrategy.h"
#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <random>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>

#include "core/HttpQuoteFeed.h"
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace engine_load_bench
{

// ── 실행 옵션 ────────────────────────────────────────────────────────────────
struct Options
{
    size_t                universe_size = 2700;
    std::vector<uint32_t> lane_list{4};
    std::vector<uint32_t> shard_list{4};
    int                   seconds       = 20;
    uint64_t              ticks_per_sec = 0;   // 0이면 최대 속도
    std::vector<uint64_t> rate_list{0};        // 초당 몇 건을 밀어 넣을지. 원소마다 한 번씩 돈다
    int                   order_every   = 1;   // 0이면 주문 없음(시세 → 전략 구간만)
    double                zipf          = 1.0; // 0이면 종목 균등
    std::string           universe_path = "Quant/config/universe_full.json";
    std::string           out_path;
    std::string           profile_path;              // 실측 유량 프로파일 JSON. 주면 zipf 대신 이 몫을 쓴다
    std::string           strategy_kind = "counter"; // counter(카운터) | itb(장중 돌파 — 체결마다 분봉·채널 계산)
    std::string           zmq_bind;                  // 비면 발행 안 함. 주소만으로는 안 갈린다 — 아래 포트도 같이 본다
    int                   zmq_port_base = 5605;      // 발행·제어 포트 넷(base, +1, +2, +3). 운영 블록 밖 기본값
    double                clock_speed   = 1.0;       // 합성 장시계 배속 — 지표 전략이 분봉을 쌓으려면 시각이 흘러야 한다
    int                   channel_minutes = 10;      // itb 전략의 채널 길이(분). 짧은 구간을 잴 때 줄인다
    std::string           source        = "synthetic"; // synthetic(합성 생성기) | http(실제 장 시세를 주기마다 통째로)
    int                   sweep_milliseconds = 1000;   // http 한 바퀴 주기. 1000과 7000 두 값으로 재다
    size_t                codes_per_call     = 900;    // http 한 요청에 묶을 종목 수(900까지 전부 돌아오는 것을 쟀다)
};

struct LatencySummary
{
    uint64_t samples = 0;
    double   p50_us  = 0.0;
    double   p99_us  = 0.0;
    double   max_us  = 0.0;
};

struct RunResult
{
    uint32_t       lanes           = 0;
    uint32_t       shards          = 0;
    double         elapsed_sec     = 0.0;
    uint64_t       ticks_emitted   = 0;
    uint64_t       signals         = 0;
    uint64_t       orders          = 0;
    Engine::QueueStatistics queues;
    LatencySummary latency;
    std::string    started_at; // 벽시계 HH:MM:SS — 바깥 자원 수집기(procwatch) 표본과 이 행을 시각으로 맞추는 열쇠
    // 아래는 --source http 일 때만 찬다. 합성 생성기에서는 전부 0이다.
    uint64_t       feed_sweeps        = 0; // 돈 바퀴 수(레인별 합)
    uint64_t       feed_calls         = 0;
    uint64_t       feed_failures      = 0; // 빈 응답·종목 0건
    uint64_t       feed_quotes        = 0; // 응답에 들어 있던 종목 수 합
    uint64_t       feed_megabytes     = 0;
    double         feed_sweep_average_ms  = 0.0;
    double         feed_sweep_max_ms  = 0.0;
    uint64_t       feed_overruns      = 0; // 한 바퀴가 주기를 넘긴 횟수
};

// ─── 함수 선언 — 정의가 있는 파일 이름을 묶음 위에 적었다 ───
// bench_engine_load_inputs.cpp
std::vector<std::string> load_universe(const std::string& path, size_t wanted);
std::vector<double> load_rank_shares(const std::string& path);
uint64_t load_profile_rate(const std::string& path, const std::string& key);
std::vector<uint32_t> build_pick_table_from_weights(const std::vector<double>& weights, size_t table_size);
std::vector<uint32_t> build_pick_table(size_t symbol_count, double skew, size_t table_size);
std::vector<uint32_t> parse_number_list(const std::string& text);
// bench_engine_load_run.cpp
std::uintmax_t file_size_or_zero(const std::string& path);
RunResult run_once(const Options& options, const std::vector<std::string>& universe, uint32_t lanes, uint32_t shards);
extern const char* kCsvHeader;
std::string to_csv_row(const Options& options, const RunResult& result);

} // namespace engine_load_bench
