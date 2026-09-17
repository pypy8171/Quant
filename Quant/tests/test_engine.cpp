// Engine 한 바퀴 단위 테스트. 시험용 피드 소스를 주입해 KIS·소켓 없이 틱→샤드→전략→디스패치→주문 스레드→모의 체결→
// 체결 소비 스레드→원장(보유)까지 도는지, 주입 모드가 브로커 없이 기동·종료하는지 고정한다. 케이스는 둘 — 레인 1×샤드 1과
// 레인 2×샤드 2(종목 둘이 서로 다른 레인에서 들어와 서로 다른 열에서 판단된다). 관련 결정: D-071(Phase 3·Phase 4 앞단계).
// 스레드: 테스트 스레드가 피드 소스의 수신 스레드 역할(레인 0..N-1)을 하고 나머지는 Engine이 띄운다.
// 빌드: cmake --build <directory> --target test_engine
#include "core/Engine.h"
#include "core/IFeedSource.h"
#include "core/ShardMatrix.h"
#include "core/TickCapture.h"
#include "strategy/StrategyBase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
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

// 콜백을 받아 두고 테스트가 부르는 대로 틱을 내보내는 피드 소스. 연결·구독은 기록만 하고 성공이라 답한다.
//  lanes()를 N으로 답해 Engine이 행렬 행을 N개 잡게 한다 — 실제 소켓 N개(FeedMux 레인 모드)와 같은 자리.
class FakeFeed : public feed::IFeedSource
{
public:
    explicit FakeFeed(uint32_t lanes) : lanes_(lanes) {}

    uint32_t lanes() const override { return lanes_; }

    void set_callbacks(OrderBookCb on_order_book, TradeCb on_trade) override
    {
        on_order_book_    = std::move(on_order_book);
        on_trade_ = std::move(on_trade);
    }

    void set_lane_callbacks(LaneOrderBookCb on_order_book, LaneTradeCb on_trade) override
    {
        lane_order_book_    = std::move(on_order_book);
        lane_trade_ = std::move(on_trade);
    }

    bool connect(const std::vector<WatchSpec>& specifications) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        specifications_     = specifications;
        connected_ = true;
        return true;
    }

    void disconnect() override { connected_ = false; }

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

    // 수신 스레드가 디코드 직후 부르는 자리 — 여기서는 테스트 스레드가 레인 lane의 역할을 한다.
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

    std::vector<WatchSpec> specifications() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return specifications_;
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

// 자기 종목의 첫 틱에 시장가 1주 매수 한 번. 이후는 침묵. 종목 비교는 id로(원칙 6).
class BuyOnce : public StrategyBase
{
public:
    explicit BuyOnce(std::string ticker) : ticker_(std::move(ticker)) {}

    std::string id() const override { return "buy_once_" + ticker_; }
    std::string describe() const override { return "첫 틱에 1주 매수"; }
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    void on_start() override { symbol_id_ = symbol_of(ticker_); }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        WatchSpec specification;
        specification.ticker = ticker_;
        return {specification};
    }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        ++ticks_seen;

        if (fired_ || !same_symbol(symbol_id_, ticker_, trade.symbol_id, trade.ticker.string()))
        {
            return std::nullopt;
        }

        fired_ = true;
        OrderSignal signal;
        signal.ticker      = ticker_;
        signal.symbol_id         = trade.symbol_id;
        signal.side        = OrderSide::BUY;
        signal.type        = OrderType::MARKET;
        signal.quantity    = 1;
        signal.reference_price   = trade.price;
        signal.strategy_id = id();
        return signal;
    }

    symbol::SymbolId    symbol_id() const { return symbol_id_; }
    std::atomic<int> ticks_seen{0};

private:
    std::string   ticker_;
    symbol::SymbolId symbol_id_{};
    bool          fired_ = false;
};

// 종목 i는 레인 i % lanes에서 들어온다. 종목마다 전략 하나. 종목 수만큼 주문·체결·보유가 잡히면 통과.
int run_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers)
{
    using namespace std::chrono_literals;
    std::cout << "case lanes=" << lanes << " shards=" << shards << " tickers=" << tickers.size() << "\n";

    auto  feed_owned = std::make_unique<FakeFeed>(lanes);
    auto* feed       = feed_owned.get();
    std::vector<BuyOnce*> strategies;

    Engine engine(KisConfig{});
    engine.set_strategy_shards(shards);

    for (const auto& ticker : tickers)
    {
        auto stop_token = std::make_unique<BuyOnce>(ticker);
        strategies.push_back(stop_token.get());
        engine.add_strategy(std::move(stop_token));
    }

    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
    engine.start();

    // 1. 브로커 없이 떴고, 소스는 전략들의 구독 종목으로 연결됐고, 행렬은 요청한 레인×샤드 그대로다(샤드 1 폴백 없음).
    CHECK(engine.is_running());
    CHECK(feed->is_connected());
    CHECK(feed->specifications().size() == tickers.size());
    CHECK(engine.websocket_lanes() == lanes);
    CHECK(engine.shard_count() == shards);

    // 2. 종목들이 실제로 서로 다른 열에 떨어진다(샤드가 둘 이상일 때) — 같은 열이면 N×M을 시험한 것이 아니다.
    {
        std::set<uint32_t> cols;

        for (auto* stop_token : strategies)
        {
            cols.insert(shard::shard_of(stop_token->symbol_id(), shards));
        }

        CHECK(cols.size() == std::min<size_t>(shards, tickers.size()));
    }

    // 3. 첫 틱이 각 전략까지 닿고 매수 신호가 주문 큐로 간다. 접수는 모의 체결기 — 체결은 다음 틱에서.
    for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
    {
        feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70000.0, 93001);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;

    while (engine.order_count() < tickers.size() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }

    CHECK(engine.order_count() == tickers.size());

    for (auto* stop_token : strategies)
    {
        CHECK(stop_token->ticks_seen.load() >= 1);
    }

    // 4. 다음 틱이 시장가를 그 가격에 체결시키고 체결 소비 스레드가 원장에 반영한다 — 종목마다 보유 1주, 평단 70100.
    std::vector<OrderGate::HeldPos> held;

    for (int index = 0; index < 400; ++index)
    {
        for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
        {
            feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70100.0, 93002 + index);
        }

        held = engine.held_positions();

        if (held.size() >= tickers.size())
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(held.size() == tickers.size());

    for (const auto& holding : held)
    {
        CHECK(std::find(tickers.begin(), tickers.end(), holding.ticker) != tickers.end());
        CHECK(holding.quantity == 1);
        CHECK(holding.average_price > 70099.0 && holding.average_price < 70101.0);
    }

    CHECK(engine.signal_count() == tickers.size());

    // 5. 정지가 장 외 대기(60초)를 기다리지 않는다.
    const auto start_time = std::chrono::steady_clock::now();
    engine.stop();
    CHECK(!engine.is_running());
    CHECK(std::chrono::steady_clock::now() - start_time < 10s);
    return 0;
}

// 캡처 파일을 틀어 같은 한 바퀴를 돈다 — set_replay만으로 KIS 없이 뜨는지. 첫 틱 70000이 매수 신호를 내고 뒤따르는
//  70100 틱 중 하나가 시장가를 체결시킨다. 캡처 간격 20ms·speed 1이라 주문이 큐를 지나는 사이에도 틱이 계속 온다.
int run_replay_case()
{
    using namespace std::chrono_literals;
    std::cout << "case replay\n";
    const auto path = std::filesystem::temp_directory_path() / "quant_test_engine_replay.bin";
    std::filesystem::remove(path);
    {
        feed::TickCapture capture(path);
        CHECK(capture.ok());

        for (int index = 0; index < 100; ++index)
        {
            TradeData trade;
            trade.ticker.assign("005930");
            trade.price     = index == 0 ? 70000.0 : 70100.0;
            trade.quantity  = 10;
            trade.direction = 1;
            trade.hhmmss    = 93001 + index;
            trade.received_ns   = 1'000'000'000LL + static_cast<int64_t>(index) * 20'000'000LL;
            capture.on_trade(trade);
        }

        capture.flush();
        CHECK(capture.written() == 100);
    }

    Engine engine(KisConfig{});
    auto   st_owned = std::make_unique<BuyOnce>("005930");
    auto*  stop_token       = st_owned.get();
    engine.add_strategy(std::move(st_owned));
    engine.set_replay(path.string(), 1.0, 1'000'000.0);
    engine.start();

    // 1. KIS 없이 떴고 레인은 하나다.
    CHECK(engine.is_running());
    CHECK(engine.websocket_lanes() == 1);

    // 2. 캡처가 흐르는 동안 주문 1건이 접수되고 다음 틱에 체결돼 보유 1주·평단 70100이 잡힌다.
    std::vector<OrderGate::HeldPos> held;
    const auto                      deadline = std::chrono::steady_clock::now() + 8s;

    while (std::chrono::steady_clock::now() < deadline)
    {
        held = engine.held_positions();

        if (!held.empty())
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(engine.order_count() == 1);
    CHECK(stop_token->ticks_seen.load() >= 2);
    CHECK(held.size() == 1);

    if (held.size() == 1)
    {
        CHECK(held[0].ticker == "005930");
        CHECK(held[0].quantity == 1);
        CHECK(held[0].average_price > 70099.0 && held[0].average_price < 70101.0);
    }

    CHECK(engine.signal_count() == 1);
    engine.stop();
    CHECK(!engine.is_running());
    std::filesystem::remove(path);
    return 0;
}
} // namespace

int main()
{
    if (const int result_code = run_case(1, 1, {"005930"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_case(2, 2, {"005930", "000660"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_replay_case(); result_code != 0)
    {
        return result_code;
    }

    std::cout << "test_engine OK (" << g_checks << " checks)\n";
    return 0;
}
