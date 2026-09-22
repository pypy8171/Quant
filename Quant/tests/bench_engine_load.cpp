// 전 종목 부하 하네스 — 합성 체결을 진짜 Engine에 밀어넣어 수신 → 샤드 → 전략 → 주문 → 체결 → 원장 한 바퀴를 돌린다.
//  bench_market_firehose는 큐만(Mock) 재고 test_engine은 종목 셋뿐이라, 그 사이가 비어 있었다.
//  수신 스레드 수 N과 전략 샤드 수 M을 바꿔 가며 처리량·지연·큐 수위를 표로 뽑는 것이 목적이다. [why D-071]
//
//  쓰는 법(저장소 루트에서):
//    bench_engine_load run   --tickers 2700 --lanes 4 --shards 4 --seconds 20
//    bench_engine_load sweep --tickers 2700 --lane-list 1,2,4,8 --shard-list 1,2,4,8 --seconds 10 \
//                            --out logs/bench_engine_load.csv
//
//  유량을 안 묶으면(--rate 0) 큐가 전 구간 포화라 "포화 상태"만 보인다. 천장은 유량을 올려 가며 드롭이
//  처음 생기는 지점으로 읽는다 — --rate-list 100000,200000,400000,800000 처럼 준다.
//
//  [inv] 한 종목은 수신 스레드 하나만 내보내고 전략(샤드) 하나만 본다 — 종목 안 순서 보장(원칙 1·2).
//  [inv] 주문 한도·발주 간격은 전부 풀어 둔다. 여기서 재는 것은 파이프라인의 천장이지 리스크 규칙이 아니다.

#include "core/Engine.h"
#include "strategy/StrategyBase.h"
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
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

// ── 종목 목록 ────────────────────────────────────────────────────────────────
// universe_full.json의 "codes" 배열에서 6자리 코드만 뽑는다. 파일이 없으면 합성 코드로 채운다.
//  JSON 파서를 끌어오지 않는 것은 bench_market_firehose와 같은 이유다(하네스는 가볍게).
std::vector<std::string> load_universe(const std::string& path, size_t wanted)
{
    std::vector<std::string> codes;
    std::ifstream            file(path);

    if (file)
    {
        const std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const size_t      codes_at = body.find("\"codes\"");
        const std::string scan     = codes_at == std::string::npos ? body : body.substr(codes_at);

        for (size_t index = 0; index + 1 < scan.size(); ++index)
        {
            if (scan[index] != '"')
            {
                continue;
            }

            size_t tail = index + 1;

            while (tail < scan.size() && scan[tail] >= '0' && scan[tail] <= '9')
            {
                ++tail;
            }

            if (tail - index - 1 == 6 && tail < scan.size() && scan[tail] == '"')
            {
                codes.push_back(scan.substr(index + 1, 6));
                index = tail;
            }
        }
    }

    // 실제 종목 수가 모자라면 합성 코드로 채운다 — 부하 규모가 목적이라 코드의 진짜 여부는 중요하지 않다.
    for (size_t index = codes.size(); index < wanted; ++index)
    {
        std::string synthetic = std::to_string(900000 + index);
        codes.push_back(synthetic.substr(synthetic.size() - 6));
    }

    codes.resize(wanted);
    return codes;
}

// ── 합성 시세 소스 ───────────────────────────────────────────────────────────
// 수신 스레드(lane) 역할을 생성기 스레드가 직접 한다 — 소켓·디코드를 뺀 나머지 경로는 라이브와 같다.
class LoadFeed : public feed::IFeedSource
{
public:
    explicit LoadFeed(uint32_t lanes) : lanes_(lanes)
    {
    }

    uint32_t lanes() const override { return lanes_; }

    void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) override
    {
        on_order_book_ = std::move(on_order_book);
        on_trade_      = std::move(on_trade);
    }

    void set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade) override
    {
        lane_order_book_ = std::move(on_order_book);
        lane_trade_      = std::move(on_trade);
    }

    bool connect(const std::vector<WatchSpec>& specifications) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        specifications_ = specifications;
        connected_.store(true);
        return true;
    }

    void disconnect() override { connected_.store(false); }

    bool subscribe_incremental(const WatchSpec& specification) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        specifications_.push_back(specification);
        return true;
    }

    bool has_specification(const WatchSpec& specification) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& watch : specifications_)
        {
            if (watch.ticker == specification.ticker)
            {
                return true;
            }
        }

        return false;
    }

    std::vector<WatchSpec> take_overflow_specifications() override { return {}; }
    bool                   is_connected() const override { return connected_.load(); }
    bool                   is_stale(int) const override { return false; }

    // 수신 스레드가 디코드 직후 부르는 자리. 종목 id는 Engine 쪽 콜백이 찾는다 — 그 조회도 재는 대상이다.
    void emit_trade(uint32_t lane, const std::string& ticker, double price, int32_t hhmmss)
    {
        TradeData trade;
        trade.ticker.assign(ticker);
        trade.price     = price;
        trade.quantity  = 10;
        trade.direction = 1;
        trade.hhmmss    = hhmmss;

        if (lane_trade_)
        {
            lane_trade_(lane, trade);
        }
        else if (on_trade_)
        {
            on_trade_(trade);
        }
    }

private:
    uint32_t               lanes_;
    mutable std::mutex     mutex_;
    std::vector<WatchSpec> specifications_;
    std::atomic<bool>      connected_{false};
    OrderBookCb            on_order_book_;
    TradeCb                on_trade_;
    LaneOrderBookCb        lane_order_book_;
    LaneTradeCb            lane_trade_;
};

// ── 부하용 전략 ──────────────────────────────────────────────────────────────
// 자기 몫 종목을 전부 보고, 보이는 체결마다 시장가 1주 매수를 낸다. 신호 판단을 최소로 둔 것은
//  전략 계산이 아니라 파이프라인(큐·샤드·주문 경로)의 천장을 재려는 것이기 때문이다.
class OrderSpam : public StrategyBase
{
public:
    OrderSpam(std::string identifier, std::vector<std::string> tickers, int order_every)
        : identifier_(std::move(identifier)), tickers_(std::move(tickers)), order_every_(order_every < 0 ? 0 : order_every)
    {
    }

    const std::string& id() const override { return identifier_; }
    std::string        describe() const override { return "부하 하네스 — 체결마다 시장가 1주 매수"; }
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    void on_start() override
    {
        symbol_ids_.reserve(tickers_.size());

        for (const auto& ticker : tickers_)
        {
            symbol_ids_.push_back(symbol_of(ticker));
        }
    }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        std::vector<WatchSpec> specifications;
        specifications.reserve(tickers_.size());

        for (const auto& ticker : tickers_)
        {
            WatchSpec specification;
            specification.ticker = ticker;
            specifications.push_back(specification);
        }

        return specifications;
    }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        ++ticks_seen_;

        // order_every 0은 주문을 아예 내지 않는다 — 시세 수신 → 전략 평가 구간만 발라내는 구성.
        //  주문 경로(단일 시퀀서 OrderGate·라우터 파일 쓰기)가 천장이면 스레드 수를 바꿔도 처리량이 안 움직이는데,
        //  그 두 경우를 가르려면 주문 없는 구성과 나란히 놓고 봐야 한다.
        if (order_every_ == 0)
        {
            return std::nullopt;
        }

        if (order_every_ > 1 && ticks_seen_ % static_cast<uint64_t>(order_every_) != 0)
        {
            return std::nullopt;
        }

        OrderSignal signal;
        signal.ticker          = trade.ticker.string();
        signal.symbol_id       = trade.symbol_id;
        signal.side            = OrderSide::BUY;
        signal.type            = OrderType::MARKET;
        signal.quantity        = 1;
        signal.reference_price = trade.price;
        signal.strategy_id     = identifier_;
        return signal;
    }

    uint64_t ticks_seen() const { return ticks_seen_; }

private:
    std::string                   identifier_;
    std::vector<std::string>      tickers_;
    int                           order_every_;
    std::vector<symbol::SymbolId> symbol_ids_;
    uint64_t                      ticks_seen_ = 0; // 자기 샤드 스레드만 만진다
};

// ── 거래량 쏠림(Zipf) ────────────────────────────────────────────────────────
// 실제 장은 대형주 몇 개가 체결 대부분을 차지한다. 종목을 균등하게 도는 루프로는 큐가 실제로 터지는
//  국면(한 종목·한 샤드에 몰리는 순간)을 재현하지 못한다. 뽑기표를 미리 만들고 한 번 섞어 두었다가
//  순서대로 읽는다 — hot loop에 난수와 나눗셈을 두지 않으려고. 씨앗 고정이라 구성 사이 비교가 된다.
std::vector<uint32_t> build_pick_table(size_t symbol_count, double skew, size_t table_size)
{
    std::vector<uint32_t> table;

    if (symbol_count == 0)
    {
        return table;
    }

    table.reserve(table_size);

    if (skew <= 0.0)
    {
        for (size_t slot = 0; slot < table_size; ++slot)
        {
            table.push_back(static_cast<uint32_t>(slot % symbol_count));
        }

        return table;
    }

    std::vector<double> cumulative(symbol_count);
    double              total = 0.0;

    for (size_t rank = 0; rank < symbol_count; ++rank)
    {
        total += 1.0 / std::pow(static_cast<double>(rank + 1), skew);
        cumulative[rank] = total;
    }

    for (size_t slot = 0; slot < table_size; ++slot)
    {
        const double target = total * (static_cast<double>(slot) + 0.5) / static_cast<double>(table_size);
        const auto   found  = std::lower_bound(cumulative.begin(), cumulative.end(), target);
        table.push_back(static_cast<uint32_t>(std::distance(cumulative.begin(), found)));
    }

    std::mt19937 generator(20260922u);
    std::shuffle(table.begin(), table.end(), generator);
    return table;
}

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
};

std::vector<uint32_t> parse_number_list(const std::string& text)
{
    std::vector<uint32_t> numbers;
    std::stringstream     stream(text);
    std::string           token;

    while (std::getline(stream, token, ','))
    {
        if (!token.empty())
        {
            numbers.push_back(static_cast<uint32_t>(std::stoul(token)));
        }
    }

    return numbers;
}

// ── 지연 — 엔진이 이미 쓰고 있는 latency_trace.csv의 이번 구간 줄만 읽는다 ──
// 주문 한 건이 게이트·라우터를 지날 때마다 한 줄이 붙는다. total_us는 10번째 열. [inv] 열 순서는 LatencyTrace.h가 정본
struct LatencySummary
{
    uint64_t samples = 0;
    double   p50_us  = 0.0;
    double   p99_us  = 0.0;
    double   max_us  = 0.0;
};

LatencySummary read_latency_since(const std::string& path, std::uintmax_t from_offset)
{
    LatencySummary      summary;
    std::vector<double> totals;
    std::ifstream       file(path, std::ios::binary);

    if (!file)
    {
        return summary;
    }

    file.seekg(static_cast<std::streamoff>(from_offset));
    std::string line;

    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == 'u') // 헤더 줄(utc_ms...)
        {
            continue;
        }

        std::stringstream stream(line);
        std::string       field;
        int               column = 0;

        while (std::getline(stream, field, ','))
        {
            if (column == 9 && !field.empty())
            {
                try
                {
                    totals.push_back(std::stod(field));
                }
                catch (const std::exception&)
                {
                }
            }

            ++column;
        }
    }

    if (totals.empty())
    {
        return summary;
    }

    std::sort(totals.begin(), totals.end());
    summary.samples = totals.size();
    summary.p50_us  = totals[totals.size() / 2];
    summary.p99_us  = totals[static_cast<size_t>(static_cast<double>(totals.size()) * 0.99)];
    summary.max_us  = totals.back();
    return summary;
}

std::uintmax_t file_size_or_zero(const std::string& path)
{
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    return error ? 0u : size;
}

// ── 한 구성(N × M) 한 번 ─────────────────────────────────────────────────────
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
};

std::string wall_clock_now()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream text;
    text << std::put_time(std::localtime(&now), "%H:%M:%S");
    return text.str();
}

RunResult run_once(const Options& options, const std::vector<std::string>& universe, uint32_t lanes, uint32_t shards)
{
    RunResult result;
    result.lanes  = lanes;
    result.shards = shards;

    // 구성마다 엔진을 새로 띄운다 — 직전 구성이 남긴 미체결 목록을 OrderRouter가 이전 세션 미체결로 읽어 수만 건 취소하며
    //  주문 스레드와 경쟁하면 구성 사이 비교가 깨진다(09-22 주문/초가 한 구성 안에서도 이 경쟁을 탔다).
    for (const char* leftover : {"open_orders.txt", "open_orders.tmp"})
    {
        std::error_code ignored;
        std::filesystem::remove(Logger::instance().path_for(leftover), ignored);
    }

    auto  feed_owned = std::make_unique<LoadFeed>(lanes);
    auto* feed       = feed_owned.get();

    Engine engine(KisConfig{});
    engine.set_strategy_shards(shards);

    // 샤드 하나가 전략 하나를 소유한다 — 샤드 M개를 쓰려면 전략도 M개다. 종목은 겹치지 않게 나눈다. [why D-110]
    for (uint32_t shard_index = 0; shard_index < shards; ++shard_index)
    {
        std::vector<std::string> slice;

        for (size_t index = shard_index; index < universe.size(); index += shards)
        {
            slice.push_back(universe[index]);
        }

        engine.add_strategy(std::make_unique<OrderSpam>("load_" + std::to_string(shard_index), std::move(slice), options.order_every));
    }

    // 한도는 전부 풀어 둔다 — 여기서 재는 것은 파이프라인의 천장이다.
    OrderGate::Config risk;
    risk.max_quantity_per_ticker  = 1'000'000'000;
    risk.max_quantity_per_order   = 1'000'000;
    risk.max_notional_per_order   = 1e15; // 0은 "무제한"이 아니라 "0원 초과 금지"다 — 여기만 규칙이 반대다
    risk.max_notional_per_ticker  = 0.0;  // 이쪽은 0이 무제한
    risk.max_concurrent_positions = 0;
    risk.max_orders_per_sec       = 1'000'000;
    risk.max_orders_per_min       = 60'000'000;
    risk.deduplicate_window_sec   = 0.0;
    risk.daily_loss_limit         = -1e15;
    engine.set_risk_config(risk);
    engine.set_order_interval(0, 0); // 주문마다 자는 350ms를 없앤다 — 안 풀면 초당 세 건이 천장이다
    // 구독자 없는 발행 채널은 아예 열지 않는다 — 주문마다 나는 drop 로그의 파일 I/O가 측정 대상을 덮는다.
    engine.set_zmq_enabled(false);
    engine.set_feed_source(std::move(feed_owned), 1e15);
    engine.start();

    const std::string    trace_path   = Logger::instance().path_for("latency_trace.csv").string();
    const std::uintmax_t trace_offset = file_size_or_zero(trace_path);

    std::atomic<uint64_t>    emitted{0};
    std::atomic<bool>        stop{false};
    std::vector<std::thread> generators;
    result.started_at = wall_clock_now();
    const auto               started_at = std::chrono::steady_clock::now();
    const auto               deadline   = started_at + std::chrono::seconds(options.seconds);
    // 수신 스레드 하나가 초당 낼 건수. 0이면 제한 없음.
    const uint64_t lane_rate = options.ticks_per_sec == 0 ? 0 : std::max<uint64_t>(1, options.ticks_per_sec / lanes);

    constexpr size_t kBatch = 256; // 시각 확인·유량 조절 주기. 건마다 steady_clock을 부르면 그게 측정 대상을 가린다

    for (uint32_t lane = 0; lane < lanes; ++lane)
    {
        generators.emplace_back([&, lane]() {
            // [inv] 종목 하나는 수신 스레드 하나만 내보낸다 — 종목 안 순서 보장(원칙 1·2).
            std::vector<size_t> slice;

            for (size_t index = lane; index < universe.size(); index += lanes)
            {
                slice.push_back(index);
            }

            const std::vector<uint32_t> pick_table = build_pick_table(slice.size(), options.zipf, 8192);
            uint64_t                    sent       = 0;
            size_t                      cursor     = 0;
            double                      price      = 70000.0;

            if (slice.empty() || pick_table.empty())
            {
                return;
            }

            while (!stop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
            {
                for (size_t step = 0; step < kBatch; ++step)
                {
                    price = price > 71000.0 ? 70000.0 : price + 1.0;
                    feed->emit_trade(lane, universe[slice[pick_table[cursor]]], price, 93000);
                    cursor = cursor + 1 == pick_table.size() ? 0 : cursor + 1;
                    ++sent;
                }

                if (lane_rate != 0)
                {
                    // 목표 시각까지 쉰다 — 건별 슬립은 Windows 타이머 격자(15.6ms)에 걸린다.
                    const auto target = started_at + std::chrono::nanoseconds(sent * 1'000'000'000ull / lane_rate);
                    std::this_thread::sleep_until(target);
                }
            }

            emitted.fetch_add(sent, std::memory_order_relaxed);
        });
    }

    for (auto& generator : generators)
    {
        generator.join();
    }

    // [inv] 유량은 "내보낸 구간"으로만 나눈다 — 아래 배수 시간까지 넣으면 초당 건수가 그만큼 깎인다.
    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();

    // 큐에 남은 것을 소화할 시간을 준다 — 천장을 재는 것이지 유실을 만드는 것이 목적이 아니다.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    result.ticks_emitted = emitted.load();
    result.signals       = engine.signal_count();
    result.orders        = engine.order_count();
    result.queues        = engine.queue_statistics();
    engine.stop();
    result.latency = read_latency_since(trace_path, trace_offset);
    return result;
}

// offered_per_sec = 내보낸 유량, accepted_per_sec = 샤드까지 들어간 유량(= goodput). 둘이 갈라지는 지점이 그 구성의 천장이다.
const char* kCsvHeader = "lanes,shards,tickers,seconds,order_every,zipf,rate,elapsed_sec,ticks_emitted,offered_per_sec,"
                         "accepted_per_sec,drop_pct,signals,orders,orders_per_sec,"
                         "shard_high_water,order_high_water,fill_high_water,trade_dropped,shard_dropped,order_dropped,fill_dropped,"
                         "latency_samples,p50_us,p99_us,max_us,started_at";

std::string to_csv_row(const Options& options, const RunResult& result)
{
    const double   seconds  = std::max(0.001, result.elapsed_sec);
    const uint64_t accepted = result.ticks_emitted > result.queues.trade_dropped
                                  ? result.ticks_emitted - result.queues.trade_dropped
                                  : 0;
    const double   drop_percent = result.ticks_emitted == 0
                                  ? 0.0
                                  : 100.0 * static_cast<double>(result.queues.trade_dropped) / static_cast<double>(result.ticks_emitted);

    std::ostringstream row;
    row.setf(std::ios::fixed);
    row.precision(1);
    row << result.lanes << ',' << result.shards << ',' << options.universe_size << ',' << options.seconds << ','
        << options.order_every << ',' << options.zipf << ',' << options.ticks_per_sec << ','
        << result.elapsed_sec << ',' << result.ticks_emitted << ','
        << static_cast<uint64_t>(static_cast<double>(result.ticks_emitted) / seconds) << ','
        << static_cast<uint64_t>(static_cast<double>(accepted) / seconds) << ',' << drop_percent << ','
        << result.signals << ',' << result.orders << ','
        << static_cast<uint64_t>(static_cast<double>(result.orders) / seconds) << ','
        << result.queues.shard_high_water << ',' << result.queues.order_high_water << ',' << result.queues.fill_high_water << ','
        << result.queues.trade_dropped << ',' << result.queues.shard_dropped << ',' << result.queues.order_dropped << ','
        << result.queues.fill_dropped << ','
        << result.latency.samples << ',' << result.latency.p50_us << ',' << result.latency.p99_us << ',' << result.latency.max_us << ','
        << result.started_at;
    return row.str();
}

} // namespace

int main(int argc, char** argv)
{
    // 산출물은 실행파일 옆 logs_bench/ — 기본 logs/는 트레이더·테스트와 같은 폴더다. 주문을 수만 건 내는 하네스가 거기
    //  open_orders.txt를 남기면 다음에 뜨는 트레이더·test_engine의 OrderRouter가 그걸 이전 세션 미체결로 읽어 전부 취소하러 간다.
    Logger::instance().set_base_directory(Logger::executable_directory() / "logs_bench");

    // 건마다 찍히는 INFO·WARN의 파일 I/O가 재려는 구간보다 길다 — 기본은 ERROR만 남기고 --log-level로 푼다.
    Logger::instance().initialize(Logger::instance().path_for("bench_engine_load.log"), LogLevel::ERROR);

    Options options;
    std::string mode = argc > 1 && argv[1][0] != '-' ? argv[1] : "run";

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const std::string value    = index + 1 < argc ? argv[index + 1] : "";

        if (argument == "--tickers")
        {
            options.universe_size = static_cast<size_t>(std::stoul(value));
        }
        else if (argument == "--lanes")
        {
            options.lane_list = {static_cast<uint32_t>(std::stoul(value))};
        }
        else if (argument == "--shards")
        {
            options.shard_list = {static_cast<uint32_t>(std::stoul(value))};
        }
        else if (argument == "--lane-list")
        {
            options.lane_list = parse_number_list(value);
        }
        else if (argument == "--shard-list")
        {
            options.shard_list = parse_number_list(value);
        }
        else if (argument == "--seconds")
        {
            options.seconds = std::stoi(value);
        }
        else if (argument == "--rate")
        {
            options.rate_list = {static_cast<uint64_t>(std::stoull(value))};
        }
        else if (argument == "--rate-list")
        {
            options.rate_list.clear();

            for (const uint32_t rate : parse_number_list(value))
            {
                options.rate_list.push_back(rate);
            }
        }
        else if (argument == "--order-every")
        {
            options.order_every = std::stoi(value);
        }
        else if (argument == "--no-orders")
        {
            options.order_every = 0;
        }
        else if (argument == "--zipf")
        {
            options.zipf = std::stod(value);
        }
        else if (argument == "--universe")
        {
            options.universe_path = value;
        }
        else if (argument == "--out")
        {
            options.out_path = value;
        }
        else if (argument == "--log-level")
        {
            // 측정이 아니라 무슨 일이 났는지 보려고 켤 때만. debug/info/warn/error.
            if (value == "debug")
            {
                Logger::instance().set_min_level(LogLevel::DEBUG);
            }
            else if (value == "info")
            {
                Logger::instance().set_min_level(LogLevel::INFO);
            }
            else if (value == "warn")
            {
                Logger::instance().set_min_level(LogLevel::WARN);
            }
            else
            {
                Logger::instance().set_min_level(LogLevel::ERROR);
            }
        }
    }

    const std::vector<std::string> universe = load_universe(options.universe_path, options.universe_size);
    std::cout << "[부하] 종목 " << universe.size() << "개, 구간 " << options.seconds << "초, 모드 " << mode << "\n";
    std::cout << kCsvHeader << "\n";

    std::ofstream out;

    if (!options.out_path.empty())
    {
        // 상위 폴더가 없으면 ofstream이 아무 말 없이 실패한다 — 한 시간짜리 스윕을 돌리고 빈손이 된다.
        const std::filesystem::path out_file = options.out_path;

        if (out_file.has_parent_path())
        {
            std::error_code ignored;
            std::filesystem::create_directories(out_file.parent_path(), ignored);
        }

        const std::uintmax_t append_offset = file_size_or_zero(out_file.string());
        out.open(out_file, std::ios::app);

        if (!out)
        {
            std::cerr << "[부하] CSV를 못 연다: " << options.out_path << "\n";
            return 1;
        }


        // 이어 붙이는 파일이 비었을 때만 헤더를 쓴다. ios::app 스트림의 tellp()는 첫 쓰기 전까지 0을 주므로
        //  그걸로 판정하면 실행할 때마다 헤더 줄이 끼어든다.
        if (append_offset == 0)
        {
            out << kCsvHeader << "\n";
        }
    }

    for (const uint64_t rate : options.rate_list)
    {
        options.ticks_per_sec = rate;

        for (const uint32_t lanes : options.lane_list)
        {
            for (const uint32_t shards : options.shard_list)
            {
                const RunResult   result = run_once(options, universe, lanes, shards);
                const std::string row    = to_csv_row(options, result);
                std::cout << row << std::endl;

                if (out)
                {
                    out << row << std::endl;
                }
            }
        }
    }

    return 0;
}
