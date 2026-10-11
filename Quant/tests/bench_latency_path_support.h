#pragma once
// bench_latency_path 공용 준비 — 시계·분위수·표 출력·종목 코드·전문 생성과 항목별 측정 함수 선언.
//  측정 본문은 bench_latency_path_<주제>.cpp에 있고, bench_latency_path.cpp의 main이 원래 순서대로 부른다.

#include "api/KisWsDecode.h"
#include "core/FeedMux.h"
#include "core/MarketSession.h"
#include "core/RingBuffer.h"
#include "core/StrategyRouter.h"
#include "core/SymbolTable.h"
#include "core/TickCapture.h"
#include "core/Types.h"
#include "strategy/StrategyBase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace latency_path_bench
{

using Clock = std::chrono::steady_clock;

// 최적화가 측정 대상 계산을 지우지 못하게 결과를 모아 두는 곳. 정의는 bench_latency_path_support.cpp.
extern volatile int64_t g_sink;

// now_ns·spin_until·next_index는 측정 루프 안에서 부른다. 다른 번역 단위로 옮기면 호출 비용이 수치에
//  섞이므로 헤더에 inline으로 두어 원래처럼 호출 자리에 펼쳐지게 한다.
inline int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// t(steady nanoseconds)까지 바쁘게 기다린다 — 생산자 투입률 맞춤용. sleep은 2ms 격자라 못 쓴다.
inline void spin_until(int64_t time_value)
{
    while (now_ns() < time_value)
    {
    }
}

// f를 iters번 돌린 평균 nanoseconds/op. 첫 1/16은 예열로 버린다.
template <class F>
double ns_per_op(size_t iters, F&& item)
{
    const size_t warm = iters / 16;

    for (size_t warm_index = 0; warm_index < warm; ++warm_index)
    {
        item(warm_index);
    }

    const int64_t start_time = now_ns();

    for (size_t index = 0; index < iters; ++index)
    {
        item(index);
    }

    return static_cast<double>(now_ns() - start_time) / static_cast<double>(iters);
}

struct PercentileSummary
{
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

// 종목 코드 2,600개 — 여섯 자리 숫자열. 실 코스피·코스닥 규모.
constexpr size_t kSymbols = 2600;

// 종목 접근 순서 — 선형이면 캐시가 다 먹으므로 LCG로 섞는다.
inline size_t next_index(size_t& size)
{
    size = size * 6364136223846793005ULL + 1442695040888963407ULL;
    return (size >> 33) % kSymbols;
}

// 3(라우터)과 7(연쇄)이 같이 쓴다. on_trade를 측정 루프에서 부르므로 몸통은 클래스 안에 원래대로 둔다.
// 구독 종목 하나를 밝히는 가짜 전략. on_trade는 오늘 전략들처럼 symbol_id이 다르면 바로 돌아간다.
class OneSymStrategy final : public StrategyBase
{
public:
    explicit OneSymStrategy(std::string ticker) : ticker_(std::move(ticker)), id_("bench_" + ticker_) {}

    void bind(symbol::SymbolId id)
    {
        symbol_id_ = id;
    }

    const std::string& id() const override
    {
        return id_;
    }

    std::string describe() const override
    {
        return "bench";
    }

    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        if (trade.symbol_id != symbol_id_)
        {
            return std::nullopt;
        }

        ++hits;
        return std::nullopt;
    }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        WatchSpec specification;
        specification.ticker = ticker_;
        return {specification};
    }

    int64_t hits = 0;

private:
    std::string   ticker_;
    std::string   id_;
    symbol::SymbolId symbol_id_ = symbol::kNone;
};

// ─── 함수 선언 — 정의가 있는 파일 이름을 묶음 위에 적었다 ───
// bench_latency_path_support.cpp
PercentileSummary percentiles(std::vector<double>& values);
void row(const char* name, double old_ns, double new_ns);
void print_row(const char* name, double nanoseconds);
void head(const char* title);
std::vector<std::string> make_tickers();
std::string make_kr_trade_payload(const std::string& ticker, int price);
// bench_latency_path_decode.cpp
void bench_decode();
void bench_last_price(const std::vector<std::string>& tickers);
// bench_latency_path_router.cpp
void bench_router(const std::vector<std::string>& tickers);
void bench_state_key(const std::vector<std::string>& tickers);
// bench_latency_path_capture.cpp
void bench_capture(const std::vector<std::string>& tickers);
void bench_feed_mux(const std::vector<std::string>& tickers);
// bench_latency_path_chain.cpp
void bench_chain_at(const std::vector<std::string>& tickers, double rate_per_s);

} // namespace latency_path_bench
