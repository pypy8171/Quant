// bench_engine_load — 한 구성 실행 — 합성 시세 소스·부하용 전략·지연 읽기·CSV 한 줄.
//  공용 준비는 bench_engine_load_support.h, 구성 순회는 bench_engine_load.cpp의 main.
#include "bench_engine_load_support.h"

namespace engine_load_bench
{

// ── 합성 시세 소스 ───────────────────────────────────────────────────────────
// 수신 스레드(lane) 역할을 생성기 스레드가 직접 한다 — 소켓·디코드를 뺀 나머지 경로는 라이브와 같다.
class LoadFeed : public feed::IFeedSource
{
public:
    explicit LoadFeed(uint32_t lanes) : lanes_(lanes)
    {
    }

    uint32_t lanes() const override
    {
        return lanes_;
    }

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

    void disconnect() override
    {
        connected_.store(false);
    }

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

    std::vector<WatchSpec> take_overflow_specifications() override
    {
        return {};
    }

    bool                   is_connected() const override
    {
        return connected_.load();
    }

    bool                   is_stale(int) const override
    {
        return false;
    }

    // 바깥 피드가 이미 만든 체결을 그대로 넘긴다 — 값은 시장이 정하고 하네스는 어느 레인이 낸 것인지만 고른다.
    void emit_trade_data(uint32_t lane, const TradeData& trade)
    {
        if (lane_trade_)
        {
            lane_trade_(lane, trade);
        }
        else if (on_trade_)
        {
            on_trade_(trade);
        }
    }

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

    const std::string& id() const override
    {
        return identifier_;
    }

    std::string        describe() const override
    {
        return "부하 하네스 — 체결마다 시장가 1주 매수";
    }

    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

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

    uint64_t ticks_seen() const
    {
        return ticks_seen_;
    }

private:
    std::string                   identifier_;
    std::vector<std::string>      tickers_;
    int                           order_every_;
    std::vector<symbol::SymbolId> symbol_ids_;
    uint64_t                      ticks_seen_ = 0; // 자기 샤드 스레드만 만진다
};

// ── 지연 — 엔진이 이미 쓰고 있는 latency_trace.csv의 이번 구간 줄만 읽는다 ──
// 주문 한 건이 게이트·라우터를 지날 때마다 한 줄이 붙는다. total_us는 10번째 열. [inv] 열 순서는 LatencyTrace.h가 정본
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

    // 프로파일은 구성마다 다시 읽는다(수 KB) — 측정 구간 밖이라 비용이 없다.
    const std::vector<double> rank_shares = options.profile_path.empty() ? std::vector<double>{} : load_rank_shares(options.profile_path);

    auto  feed_owned = std::make_unique<LoadFeed>(lanes);
    auto* feed       = feed_owned.get();

    Engine engine(KisConfig{});
    engine.set_strategy_shards(shards);

    if (options.strategy_kind == "itb")
    {
        // 라이브와 같은 배치 — 종목마다 전략 하나다(IntradayBreakoutStrategy는 종목 하나를 받는다).
        //  체결마다 1분 버킷을 쌓고 채널 최고가·트레일을 계산하므로, 카운터 전략이 안 재는 전략 CPU가 여기 들어온다.
        for (const std::string& ticker : universe)
        {
            engine.add_strategy(std::make_unique<IntradayBreakoutStrategy>(ticker, 1, 0, false, options.channel_minutes));
        }
    }
    else
    {
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
    //  --zmq-bind를 주면 켠다. 그때는 리코더가 붙어 ORDER·FILL을 DB까지 가져가는 구간까지 재는 것이고,
    //  운영 중인 엔진과 포트가 겹치면 bind가 실패해 다리가 조용히 선다 — 측정은 끝까지 돌고 숫자만 틀린다.
    //  주소를 바꿔도 127.0.0.1 이면 같은 포트를 무는 것이라, 포트를 운영 블록(모의 5555~5558 ·
    //  실계좌 5565~5568) 밖 5605~5608로 기본을 잡는다. --zmq-port로 옮긴다.
    engine.set_zmq_enabled(!options.zmq_bind.empty());

    if (!options.zmq_bind.empty())
    {
        engine.set_zmq_control(options.zmq_bind, "");
        engine.set_zmq_ports({.order_pub    = options.zmq_port_base,
                              .order_rep    = options.zmq_port_base + 1,
                              .feed_pub     = options.zmq_port_base + 2,
                              .strategy_pub = options.zmq_port_base + 3});
    }

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

    // 실제 장 시세를 쓰면 합성 생성기는 띄우지 않는다 — 유량을 시장이 정하므로 여기서 만들 것이 없다.
    const uint32_t generator_lanes = options.source == "http" ? 0 : lanes;

    for (uint32_t lane = 0; lane < generator_lanes; ++lane)
    {
        generators.emplace_back([&, lane]() {
            // [inv] 종목 하나는 수신 스레드 하나만 내보낸다 — 종목 안 순서 보장(원칙 1·2).
            std::vector<size_t> slice;

            for (size_t index = lane; index < universe.size(); index += lanes)
            {
                slice.push_back(index);
            }

            // 프로파일을 주면 이 레인이 맡은 종목의 순위별 몫으로 뽑기표를 만든다(실측 편중). 없으면 합성 zipf.
            std::vector<uint32_t> pick_table;

            if (!rank_shares.empty())
            {
                std::vector<double> weights;
                weights.reserve(slice.size());

                for (const size_t index : slice)
                {
                    weights.push_back(index < rank_shares.size() ? rank_shares[index] : 0.0);
                }

                pick_table = build_pick_table_from_weights(weights, 8192);
            }

            if (pick_table.empty())
            {
                pick_table = build_pick_table(slice.size(), options.zipf, 8192);
            }

            uint64_t                    sent          = 0;
            size_t                      cursor        = 0;
            double                      price         = 70000.0;
            uint64_t                    random_state  = 0x9E3779B97F4A7C15ull ^ (lane + 1);
            std::vector<double>         prices(slice.size(), 70000.0);

            if (slice.empty() || pick_table.empty())
            {
                return;
            }

            while (!stop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
            {
                // 배치마다 한 번만 시각을 만든다 — 건마다 now()를 부르면 그 호출이 재려는 구간을 가린다.
                //  09:00에서 시작해 clock_speed 배로 흐른다. 지표 전략은 이 hhmmss로 분봉을 자른다.
                const double  synthetic_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count() * options.clock_speed;
                const int32_t synthetic_second  = 9 * 3600 + static_cast<int32_t>(synthetic_elapsed);
                const int32_t hhmmss            = (synthetic_second / 3600) * 10000 + ((synthetic_second / 60) % 60) * 100 + synthetic_second % 60;

                for (size_t step = 0; step < kBatch; ++step)
                {
                    // 종목마다 제 가격을 걷게 한다 — 한 값을 모든 종목에 쓰면 지표 전략이 재현할 수 없는 계단을 본다.
                    const uint32_t pick = pick_table[cursor];
                    double&        last = prices[pick];
                    random_state ^= random_state << 13;
                    random_state ^= random_state >> 7;
                    random_state ^= random_state << 17;
                    last *= 1.0 + (static_cast<double>(static_cast<int32_t>(random_state & 0xFFFF) - 32768) / 32768.0) * 0.0008;
                    price = last;
                    feed->emit_trade(lane, universe[slice[pick]], price, hhmmss);
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

    std::unique_ptr<feed::HttpQuoteFeed> web_feed;

    if (options.source == "http")
    {
        feed::HttpQuoteFeed::Config feed_config;
        feed_config.codes              = universe;
        feed_config.lane_count         = lanes;
        feed_config.sweep_period       = std::chrono::milliseconds(options.sweep_milliseconds);
        feed_config.max_codes_per_call = options.codes_per_call;
        // [inv] 레인 배정은 피드가 정하고 여기서는 그대로 따른다 — 한 종목은 한 레인만 낸다(원칙 1·2).
        web_feed = std::make_unique<feed::HttpQuoteFeed>(std::move(feed_config), [&](size_t lane, TradeData trade)
        {
            feed->emit_trade_data(static_cast<uint32_t>(lane), trade);
            emitted.fetch_add(1, std::memory_order_relaxed);
        });
        web_feed->start();

        while (!stop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        web_feed->stop();
        const feed::HttpQuoteFeed::Counters& feed_counters = web_feed->counters();
        result.feed_sweeps    = feed_counters.sweeps.load();
        result.feed_calls     = feed_counters.calls.load();
        result.feed_failures  = feed_counters.call_failures.load();
        result.feed_quotes    = feed_counters.quotes_received.load();
        result.feed_overruns  = feed_counters.sweep_overruns.load();
        result.feed_megabytes = feed_counters.bytes_received.load() / (1024 * 1024);
        result.feed_sweep_max_ms = static_cast<double>(feed_counters.sweep_micros_max.load()) / 1000.0;

        if (result.feed_sweeps > 0)
        {
            result.feed_sweep_average_ms =
                static_cast<double>(feed_counters.sweep_micros_total.load()) / static_cast<double>(result.feed_sweeps) / 1000.0;
        }
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
const char* kCsvHeader = "lanes,shards,tickers,seconds,order_every,zipf,strategy,zmq,rate,elapsed_sec,ticks_emitted,offered_per_sec,"
                         "accepted_per_sec,drop_pct,signals,orders,orders_per_sec,"
                         "shard_high_water,order_high_water,fill_high_water,trade_dropped,shard_dropped,order_dropped,order_stale,fill_dropped,"
                         "latency_samples,p50_us,p99_us,max_us,started_at,"
                         "source,sweep_ms,feed_sweeps,feed_calls,feed_failures,feed_quotes,feed_megabytes,"
                         "feed_sweep_average_ms,feed_sweep_max_ms,feed_overruns,"
                         "beat_gap_max_ms";

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
        << options.order_every << ',' << options.zipf << ','
        << options.strategy_kind << ',' << (options.zmq_bind.empty() ? "off" : options.zmq_bind) << ','
        << options.ticks_per_sec << ','
        << result.elapsed_sec << ',' << result.ticks_emitted << ','
        << static_cast<uint64_t>(static_cast<double>(result.ticks_emitted) / seconds) << ','
        << static_cast<uint64_t>(static_cast<double>(accepted) / seconds) << ',' << drop_percent << ','
        << result.signals << ',' << result.orders << ','
        << static_cast<uint64_t>(static_cast<double>(result.orders) / seconds) << ','
        << result.queues.shard_high_water << ',' << result.queues.order_high_water << ',' << result.queues.fill_high_water << ','
        << result.queues.trade_dropped << ',' << result.queues.shard_dropped << ',' << result.queues.order_dropped << ','
        << result.queues.order_stale << ','
        << result.queues.fill_dropped << ','
        << result.latency.samples << ',' << result.latency.p50_us << ',' << result.latency.p99_us << ',' << result.latency.max_us << ','
        << result.started_at << ','
        << options.source << ',' << options.sweep_milliseconds << ','
        << result.feed_sweeps << ',' << result.feed_calls << ',' << result.feed_failures << ',' << result.feed_quotes << ','
        << result.feed_megabytes << ',' << result.feed_sweep_average_ms << ',' << result.feed_sweep_max_ms << ','
        << result.feed_overruns << ','
        // 전략 박동의 가장 긴 공백. 사망 문턱은 이 열의 최대값 위에 둔다 — 밑에 두면 멀쩡한 전략을 죽었다고 본다.
        << static_cast<double>(result.queues.strategy_beat_gap_max_ns) / 1'000'000.0;
    return row.str();
}

} // namespace engine_load_bench
