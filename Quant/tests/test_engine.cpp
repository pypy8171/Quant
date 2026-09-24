// Engine 한 바퀴 단위 테스트. 시험용 피드 소스를 주입해 KIS·소켓 없이 틱→샤드→전략→디스패치→주문 스레드→모의 체결→
// 체결 소비 스레드→원장(보유)까지 도는지, 주입 모드가 브로커 없이 기동·종료하는지 고정한다. 케이스는 둘 — 수신 스레드 1×샤드 1과
// 수신 스레드 2×샤드 2(종목 둘이 서로 다른 수신 스레드에서 들어와 서로 다른 열에서 판단된다). 관련 결정: D-071(Phase 3·Phase 4 앞단계).
// 스레드: 테스트 스레드가 피드 소스의 수신 스레드 역할(수신 스레드 0..N-1)을 하고 나머지는 Engine이 띄운다.
// 케이스 하나 더 — 보호 주문 한 주기를 두 스레드가 같이 잡지 못하는지(D-114 전략 사망 마무리).
// 케이스 하나 더 — 티커가 종목 번호가 되는 자리 둘(D-114 단계 4): 넣는 쪽은 주문 프로세스 하나고, 잦은 자리는 없는 티커를 만들지 않는다.
// 케이스 하나 더 — 주문 쪽 스위치 다섯(D-114 단계 4): 전략 역할이면 제어 요청을 거쳐 주문 스레드가 고친다.
// 케이스 하나 더 — 역할대로 제 스레드만 띄우는지(D-114 단계 4): 전략 역할은 주문 스레드가 없고, 주문 역할은 전략을 올리지 않는다.
// 케이스 셋 더 — 유니버스 점수 쪽 순수 함수: 비중 배수의 spread 상한, 동점 순서, 시세 표를 다시 채울 때 옛 값 비우기.
// 빌드: cmake --build <directory> --target test_engine
#include "core/Engine.h"
#include "core/IFeedSource.h"
#include "core/ShardMatrix.h"
#include "core/TickCapture.h"
#include "strategy/StrategyBase.h"
#include "universe/ScoreWeight.h"
#include "universe/UniverseScanner.h"
#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
//  lanes()를 N으로 답해 Engine이 행렬 행을 N개 잡게 한다 — 실제 소켓 N개(FeedMux 직접 호출 모드)와 같은 자리.
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

    // 수신 스레드가 디코드 직후 부르는 자리 — 여기서는 테스트 스레드가 수신 스레드 lane의 역할을 한다.
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
    explicit BuyOnce(std::string ticker) : ticker_(std::move(ticker)), id_("buy_once_" + ticker_) {}

    const std::string& id() const override { return id_; }
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
    std::string   id_;
    symbol::SymbolId symbol_id_{};
    bool          fired_ = false;
};

// 전략 하나가 종목 여럿을 본다 — 종목마다 첫 틱에 1주 매수. 샤드가 둘 이상일 때 이 전략의 종목이 여러 열로 흩어지면
//  옛 설계는 샤드를 1로 내렸다. 지금은 전략이 샤드 하나를 갖고 종목 틱이 그 샤드로 온다. [why D-110]
class BuyEachOnce : public StrategyBase
{
public:
    explicit BuyEachOnce(std::vector<std::string> tickers) : tickers_(std::move(tickers)), id_("BuyEachOnce") {}

    const std::string& id() const override { return id_; }
    std::string describe() const override { return "종목마다 첫 틱에 1주 매수"; }
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    void on_start() override
    {
        for (const auto& ticker : tickers_)
        {
            symbol_ids_.push_back(symbol_of(ticker));
        }

        fired_.assign(tickers_.size(), false);
    }

    std::vector<WatchSpec> get_watch_specifications() const override
    {
        std::vector<WatchSpec> specifications;

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
        for (size_t index = 0; index < tickers_.size(); ++index)
        {
            if (fired_[index] || !same_symbol(symbol_ids_[index], tickers_[index], trade.symbol_id, trade.ticker.string()))
            {
                continue;
            }

            fired_[index] = true;
            OrderSignal signal;
            signal.ticker          = tickers_[index];
            signal.symbol_id       = trade.symbol_id;
            signal.side            = OrderSide::BUY;
            signal.type            = OrderType::MARKET;
            signal.quantity        = 1;
            signal.reference_price = trade.price;
            signal.strategy_id     = id();
            return signal;
        }

        return std::nullopt;
    }

    const std::vector<symbol::SymbolId>& symbol_ids() const { return symbol_ids_; }

private:
    std::vector<std::string>      tickers_;
    std::string                   id_;
    std::vector<symbol::SymbolId> symbol_ids_;
    std::vector<bool>             fired_;
};

// 다종목 전략 하나 + 샤드 여럿. 샤드 수가 config 그대로 서고(1 폴백 없음), 종목 전부의 마스크가 그 전략의 샤드 하나이며,
//  종목 수만큼 주문·보유가 잡히면 통과.
int run_spanning_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers)
{
    using namespace std::chrono_literals;
    std::cout << "case spanning lanes=" << lanes << " shards=" << shards << " tickers=" << tickers.size() << "\n";

    auto  feed_owned = std::make_unique<FakeFeed>(lanes);
    auto* feed       = feed_owned.get();
    auto  strategy_owned = std::make_unique<BuyEachOnce>(tickers);
    auto* strategy       = strategy_owned.get();

    Engine engine(KisConfig{});
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트의 합성 주문·체결을 물어 실거래 DB에 넣는다(09-22 실제로 났다).
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(shards);
    engine.add_strategy(std::move(strategy_owned));
    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
    engine.start();

    CHECK(engine.is_running());
    CHECK(engine.shard_count() == shards);
    CHECK(strategy->shard_index() < shards);

    for (const auto symbol_id : strategy->symbol_ids())
    {
        CHECK(engine.route_mask(symbol_id) == shard::mask_of(strategy->shard_index()));
    }

    for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
    {
        feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70000.0, 93001);
    }

    std::vector<OrderGate::HeldPos> held;
    const auto                      deadline = std::chrono::steady_clock::now() + 8s;

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
        {
            feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70100.0, 93002);
        }

        held = engine.held_positions();

        if (held.size() >= tickers.size())
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(held.size() == tickers.size());
    CHECK(engine.signal_count() == tickers.size());

    // 한 프로세스로 돌면 시세는 통로를 지나지 않는다 — 수신 스레드가 곧바로 샤드에 넣는다. [why D-114]
    for (uint32_t lane = 0; lane < lanes; ++lane)
    {
        CHECK(engine.feed_channel_pending_trades(lane) == 0);
    }

    engine.stop();
    CHECK(!engine.is_running());
    return 0;
}

// 종목 i는 수신 스레드 i % lanes에서 들어온다. 종목마다 전략 하나. 종목 수만큼 주문·체결·보유가 잡히면 통과.
int run_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers)
{
    using namespace std::chrono_literals;
    std::cout << "case lanes=" << lanes << " shards=" << shards << " tickers=" << tickers.size() << "\n";

    auto  feed_owned = std::make_unique<FakeFeed>(lanes);
    auto* feed       = feed_owned.get();
    std::vector<BuyOnce*> strategies;

    Engine engine(KisConfig{});
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트의 합성 주문·체결을 물어 실거래 DB에 넣는다(09-22 실제로 났다).
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(shards);

    for (const auto& ticker : tickers)
    {
        auto stop_token = std::make_unique<BuyOnce>(ticker);
        strategies.push_back(stop_token.get());
        engine.add_strategy(std::move(stop_token));
    }

    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
    engine.start();

    // 1. 브로커 없이 떴고, 소스는 전략들의 구독 종목으로 연결됐고, 행렬은 요청한 수신 스레드×샤드 그대로다(샤드 1 폴백 없음).
    CHECK(engine.is_running());
    CHECK(feed->is_connected());
    CHECK(feed->specifications().size() == tickers.size());
    CHECK(engine.websocket_lanes() == lanes);
    CHECK(engine.shard_count() == shards);

    // 2. 전략들이 실제로 서로 다른 샤드에 배정됐고(등록 순 라운드로빈), 각 종목 틱은 그 전략의 샤드로만 간다 —
    //  같은 샤드면 N×M을 시험한 것이 아니다. [why D-110]
    {
        std::set<uint32_t> cols;

        for (auto* stop_token : strategies)
        {
            cols.insert(stop_token->shard_index());
            CHECK(engine.route_mask(stop_token->symbol_id()) == shard::mask_of(stop_token->shard_index()));
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
// 보호 주문 한 주기는 한 스레드만 잡는다. 평소 주인은 전략 스레드고 전략이 죽으면 주문 스레드가 이어받는데,
//  멈췄던 전략이 깨어나면 둘 다 같은 주기를 보게 된다 — 둘 다 잡으면 같은 청산이 두 번 나간다(A등급).
//  스레드 넷이 같은 시각으로 한꺼번에 달려들어도 참이 하나뿐인지 본다. [why D-114]
// ─── 큐에서 오래 기다린 신규 매수만 버린다 (D-127) ──────────────────────────
int run_stale_entry_case()
{
    const int64_t emitted_ns = 1'000'000'000;                        // 신호를 만든 시각
    const int64_t fresh_ns   = emitted_ns + kOrderSignalMaxAgeNs / 2; // 문턱의 절반만 기다렸다
    const int64_t stale_ns   = emitted_ns + kOrderSignalMaxAgeNs + 1; // 문턱을 막 넘겼다

    OrderSignal buy;
    buy.action       = OrderAction::NEW;
    buy.side         = OrderSide::BUY;
    buy.signal_at_ns = emitted_ns;

    CHECK(!is_stale_entry(buy, fresh_ns));
    CHECK(is_stale_entry(buy, stale_ns));

    // 매도는 손절·청산이라 늦어도 보낸다.
    OrderSignal sell = buy;
    sell.side        = OrderSide::SELL;
    CHECK(!is_stale_entry(sell, stale_ns));

    // 취소·정정도 늦어도 보낸다 — 원주문이 살아 있다.
    OrderSignal cancel = buy;
    cancel.action      = OrderAction::CANCEL;
    CHECK(!is_stale_entry(cancel, stale_ns));

    OrderSignal replace = buy;
    replace.action      = OrderAction::REPLACE;
    CHECK(!is_stale_entry(replace, stale_ns));

    // 시각을 안 찍은 신호는 나이를 모르니 버리지 않는다.
    OrderSignal unstamped = buy;
    unstamped.signal_at_ns = 0;
    CHECK(!is_stale_entry(unstamped, stale_ns));

    std::cout << "  stale_entry OK" << std::endl;
    return 0;
}

int run_protective_claim_case()
{
    std::cout << "case protective claim\n";

    Engine engine(KisConfig{});
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트를 물어 실거래 DB에 넣는다.
    engine.set_zmq_enabled(false);

    constexpr int  kThreads    = 4;
    constexpr int  kAttempts   = 2000;
    const auto     fixed_now   = std::chrono::steady_clock::now();
    std::atomic<int> claimed{0};
    std::atomic<int> ready{0};

    {
        std::vector<std::thread> racers;

        for (int index = 0; index < kThreads; ++index)
        {
            racers.emplace_back(
                [&]
                {
                    // 넷이 같은 자리에서 출발해야 경합이 실제로 겹친다.
                    ready.fetch_add(1);

                    while (ready.load() < kThreads)
                    {
                        std::this_thread::yield();
                    }

                    for (int attempt = 0; attempt < kAttempts; ++attempt)
                    {
                        if (engine.claim_protective_cycle(fixed_now))
                        {
                            claimed.fetch_add(1);
                        }
                    }
                });
        }

        for (auto& racer : racers)
        {
            racer.join();
        }
    }

    // 같은 시각으로 8,000번 달려들어도 참은 딱 한 번이다.
    CHECK(claimed.load() == 1);

    // 간격(기본 200ms)이 아직 안 찼으면 계속 거짓이다.
    CHECK(!engine.claim_protective_cycle(fixed_now + std::chrono::milliseconds(199)));

    // 간격이 차면 다시 한 번만 참이다.
    const auto next_now = fixed_now + std::chrono::milliseconds(200);
    CHECK(engine.claim_protective_cycle(next_now));
    CHECK(!engine.claim_protective_cycle(next_now));

    std::cout << "  보호 주기 잡기 OK (경합 " << (kThreads * kAttempts) << "회에 참 1회)\n";
    return 0;
}

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
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트의 합성 주문·체결을 물어 실거래 DB에 넣는다(09-22 실제로 났다).
    engine.set_zmq_enabled(false);
    auto   st_owned = std::make_unique<BuyOnce>("005930");
    auto*  stop_token       = st_owned.get();
    engine.add_strategy(std::move(st_owned));
    engine.set_replay(path.string(), 1.0, 1'000'000.0);
    engine.start();

    // 1. KIS 없이 떴고 수신 스레드은 하나다.
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

// D-114 단계 4 — 티커가 종목 번호가 되는 자리를 둘로 가른 것을 고정한다. 넣는 쪽(register_symbol)은
//  주문 프로세스 하나고, 잦은 자리(lookup_symbol)는 표에 없는 티커를 만들지 않는다.
int run_symbol_role_case()
{
    using namespace std::chrono_literals;
    std::cout << "case symbol role\n";

    // 1. 역할을 주기 전은 지금까지와 같다 — 넣는 쪽이라 그 자리에서 번호가 나고, 같은 티커는 같은 번호다.
    //  여기서는 스레드를 띄우지 않는다. 역할은 뜨기 전에 정해지고 뜬 뒤에는 바꾸지 않는다 — 어느 스레드를
    //  띄울지가 그 값으로 갈리고, 돌고 있는 스레드가 같은 값을 읽는다. [why D-114 단계 5]
    {
        Engine engine(KisConfig{});
        engine.set_zmq_enabled(false);
        engine.set_strategy_shards(1);
        engine.add_strategy(std::make_unique<BuyOnce>("005930"));
        engine.set_feed_source(std::make_unique<FakeFeed>(1), 1'000'000.0);

        const symbol::SymbolId samsung = engine.register_symbol("005930");
        CHECK(samsung != symbol::kNone);
        CHECK(engine.register_symbol("005930") == samsung);

        // 2. 잦은 자리는 없는 티커를 만들지 않는다 — kNone을 주고 센다(등록 경로가 빠진 것을 드러낸다).
        CHECK(engine.lookup_symbol("000660") == symbol::kNone);
        CHECK(engine.symbol_lookup_misses() == 1);
        CHECK(engine.lookup_symbol("000660") == symbol::kNone);
        CHECK(engine.symbol_lookup_misses() == 2);
    }

    // 3. 전략 역할은 표에 직접 넣지 않는다 — 제어 요청을 보내고, 주문 쪽이 넣은 번호를 같은 표에서 읽어
    //  온다. 답할 쪽이 있어야 하니 주문 역할을 따로 띄운다.
    Engine order_engine(KisConfig{});
    order_engine.set_zmq_enabled(false);
    order_engine.set_strategy_shards(1);
    order_engine.set_feed_source(std::make_unique<FakeFeed>(1), 1'000'000.0);
    order_engine.set_role(ProcessRole::Order);
    order_engine.start();
    CHECK(order_engine.is_running());

    Engine strategy_engine(KisConfig{});
    strategy_engine.set_zmq_enabled(false);
    strategy_engine.set_strategy_shards(1);
    strategy_engine.set_feed_source(std::make_unique<FakeFeed>(1), 1'000'000.0);
    strategy_engine.set_role(ProcessRole::Strategy);
    strategy_engine.start();
    CHECK(strategy_engine.is_running());

    const symbol::SymbolId samsung = order_engine.register_symbol("005930");
    CHECK(samsung != symbol::kNone);

    const symbol::SymbolId hynix = strategy_engine.register_symbol("000660");
    CHECK(hynix != symbol::kNone);
    CHECK(hynix != samsung);
    CHECK(strategy_engine.lookup_symbol("000660") == hynix);
    CHECK(strategy_engine.symbol_register_timeouts() == 0);

    // 4. 이미 표에 있으면 요청을 보내지 않고 바로 답한다.
    CHECK(strategy_engine.register_symbol("000660") == hynix);
    CHECK(strategy_engine.symbol_register_timeouts() == 0);

    // 5. 집어 갈 쪽이 멎으면 번호가 안 뜬다 — 기다리다 kNone을 주고 세고, 표에도 안 들어간다.
    //  부른 쪽이 그 줄을 접게 하려는 것이다(전략 쪽이 제 번호를 찍어 버리는 쪽이 훨씬 나쁘다).
    order_engine.stop();
    CHECK(!order_engine.is_running());

    CHECK(strategy_engine.register_symbol("373220") == symbol::kNone);
    CHECK(strategy_engine.symbol_register_timeouts() == 1);
    CHECK(strategy_engine.lookup_symbol("373220") == symbol::kNone);

    strategy_engine.stop();
    CHECK(!strategy_engine.is_running());

    return 0;
}

// D-114 단계 4 — 주문 쪽 스위치를 고치는 자리가 주문 스레드 하나인 것을 고정한다. Engine::request_* 는
//  제어 요청 한 줄로 바뀌고, 값은 주문 스레드가 그 줄을 집은 뒤에 바뀐다. Both 로 돌 때도 같은 통로다 —
//  전략 프로세스 안 여러 생산자가 앞 토막에 모이고, 전략 스레드가 경계 너머 제어 면으로 옮긴다.
int run_switch_role_case()
{
    using namespace std::chrono_literals;
    std::cout << "case switch role\n";

    auto feed_owned = std::make_unique<FakeFeed>(1);

    Engine engine(KisConfig{});
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(1);
    engine.add_strategy(std::make_unique<BuyOnce>("005930"));
    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);

    // 1. 역할을 주기 전에도 통로를 탄다 — 부른 그 자리에서는 바뀌지 않는다. Both 만 질러가게 두면
    //  그 통로가 갈라 띄운 날 처음 돈다.
    engine.request_entry_halt(true);
    CHECK(!engine.is_entry_halted());

    engine.start();
    CHECK(engine.is_running());

    // 주문 스레드가 제어 큐를 집어 갈 때까지 짧게 본다. 값이 바뀌는 시점이 부른 자리가 아니라는 것이 요점이다.
    const auto settled = [](auto&& reached) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (reached())
            {
                return true;
            }

            std::this_thread::sleep_for(200us);
        }

        return false;
    };

    // 2. 뜨기 전에 넣은 줄도 잃지 않는다 — 전략 스레드가 옮기고 주문 스레드가 건다.
    CHECK(settled([&engine] { return engine.is_entry_halted(); }));

    // 3. 신규 진입 정지 — 끄고 다시 켠다. 뜬 뒤에는 역할을 바꾸지 않는다(스레드 구성이 그 값으로
    //  갈린다) — 전략 역할에서 같은 통로를 타는 것은 run_split_start_case 가 실제 두 프로세스로 본다.
    engine.request_entry_halt(false);
    CHECK(settled([&engine] { return !engine.is_entry_halted(); }));
    engine.request_entry_halt(true);
    CHECK(settled([&engine] { return engine.is_entry_halted(); }));

    // 4. 매수 비율 — 스위치가 아니라 값이라 칸을 따로 둔다.
    constexpr double kScaleUnderTest = 0.4;
    engine.request_entry_scale(kScaleUnderTest);
    CHECK(settled([&engine] { return std::fabs(engine.entry_scale() - kScaleUnderTest) < 1e-9; }));

    // 5. 수동 정지 — 방향 칸이 같이 건너간다(매도만 걸고 매수는 그대로).
    engine.request_manual_halt(OrderSide::SELL, true);
    CHECK(settled([&engine] { return engine.is_manual_sell_halted(); }));

    // 6. 전방향 차단 — 마지막에 건다.
    engine.request_kill_switch(true);
    CHECK(settled([&engine] { return engine.is_killed(); }));

    engine.stop();
    CHECK(!engine.is_running());

    // 7. 집어 갈 쪽이 멎으면 값이 안 바뀐다 — 요청은 통로에 남고, 여기서 제 손으로 고치지 않는다.
    engine.request_entry_halt(false);
    CHECK(engine.is_entry_halted());

    return 0;
}

// D-114 단계 4 — 역할대로 제 몫만 띄운다. 전략 역할은 샤드·전략 스레드만, 주문 역할은 주문·체결 스레드만이다.
//  둘 다 브로커 없이 뜨고 멎는다. 여기서 드러나는 빈자리가 --role 을 아직 막아 둔 이유다 — 전략 쪽은 종목
//  번호를 주는 주문 쪽이 없어 못 받고, 주문 쪽은 구독 목록이 전략 쪽에 있어 소켓을 열 게 없다.
int run_manual_order_case()
{
    using namespace std::chrono_literals;
    std::cout << "case manual order\n";

    // 전략을 하나도 올리지 않는다 — 수동주문이 전략 쪽을 안 거치고 주문 스레드에서 나가는지만 본다. [why D-114]
    auto feed_owned = std::make_unique<FakeFeed>(1);

    Engine engine(KisConfig{});
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(1);
    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
    engine.set_role(ProcessRole::Order);
    engine.start();

    CHECK(engine.is_running());

    // 값이 틀린 것은 인테이크에서 되돌린다 — 단말이 그 자리에서 사유를 본다.
    OpsOrderReq bad_ticker;
    bad_ticker.client_id = "manual-bad-1";
    bad_ticker.ticker    = "삼성전자";
    bad_ticker.side      = "BUY";
    bad_ticker.quantity  = 1;
    CHECK(!engine.accept_manual_order(bad_ticker).empty());

    OpsOrderReq bad_side  = bad_ticker;
    bad_side.client_id    = "manual-bad-2";
    bad_side.ticker       = "005930";
    bad_side.side         = "LONG";
    CHECK(!engine.accept_manual_order(bad_side).empty());

    // 보유가 없는 매도는 꺼내는 자리(주문 스레드)에서 끊긴다 — 인테이크는 값만 보므로 여기선 통과다.
    OpsOrderReq sell_without_position = bad_side;
    sell_without_position.client_id   = "manual-sell-1";
    sell_without_position.side        = "SELL";
    CHECK(engine.accept_manual_order(sell_without_position).empty());

    OpsOrderReq buy;
    buy.client_id       = "manual-buy-1";
    buy.ticker          = "005930";
    buy.side            = "BUY";
    buy.quantity        = 1;
    buy.price           = 70000.0;
    buy.reference_price = 70000.0;
    CHECK(engine.accept_manual_order(buy).empty());

    // 같은 cid 재전송은 한 번만 받는다.
    CHECK(!engine.accept_manual_order(buy).empty());

    // 지정가라 체결은 시세가 닿아야 난다 — 여기서 보는 것은 주문이 나갔는가까지다.
    const auto deadline = std::chrono::steady_clock::now() + 8s;

    while (engine.order_count() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }

    CHECK(engine.order_count() == 1);   // 매수 한 건만 나갔다 — 보유 없는 매도는 꺼내는 자리에서 끊겼다
    CHECK(engine.signal_count() == 0);  // 전략 쪽 순번은 찍히지 않았다 — 통로를 안 지났다

    engine.stop();
    CHECK(!engine.is_running());
    return 0;
}

int run_split_start_case()
{
    using namespace std::chrono_literals;
    std::cout << "case split start\n";

    // 세 프로세스를 한 프로세스 안에서 흉내낸다 — 주문 역할이 공유 쪽지를 만들고, 시세·전략 역할이 같은
    //  이름으로 붙는다. 붙는 쪽이 늦게 뜨는 순서까지 그대로다. 단계 5부터 소켓은 시세가 쥔다 — 앱키 하나에
    //  실시간 세션 하나라 소켓도 하나뿐이고 체결통보가 같은 세션에 실린다. 가르면 셋이 다 뜬다. [why D-114 단계 5]
    auto  order_feed_owned = std::make_unique<FakeFeed>(1);
    auto* order_feed       = order_feed_owned.get();
    auto  order_strategy   = std::make_unique<BuyOnce>("005930");
    auto* order_side_view  = order_strategy.get();

    Engine order_engine(KisConfig{});
    order_engine.set_zmq_enabled(false);
    order_engine.set_strategy_shards(1);
    order_engine.add_strategy(std::move(order_strategy));
    order_engine.set_feed_source(std::move(order_feed_owned), 1'000'000.0);
    order_engine.set_role(ProcessRole::Order);
    order_engine.start();

    // 1. 주문 역할 — 전략을 올리지 않는다. on_start 를 부르지 않으니 종목 번호를 달라는 요청도 없다.
    //  소켓도 열지 않는다(단계 4까지는 여기가 열었다). [why D-114 단계 5]
    CHECK(order_engine.is_running());
    CHECK(order_side_view->symbol_id() == symbol::kNone); // 전략을 올리지 않았다
    CHECK(order_engine.symbol_register_timeouts() == 0);
    CHECK(!order_feed->is_connected());                   // 소켓은 시세 쪽이 쥔다
    CHECK(order_engine.order_count() == 0);
    CHECK(order_engine.feed_channel_lanes() == order_engine.websocket_lanes() + 1);
    CHECK(order_engine.fill_channel_overflows() == 0); // 체결 통로는 비어 있다
    CHECK(order_engine.fill_channel_discarded() == 0);

    // 2. 시세 역할 — 소켓을 쥐고, 꺼낸 시세를 샤드가 아니라 통로에 넣는다. 전략도 샤드도 저쪽 프로세스에 있다.
    auto  feed_side_owned = std::make_unique<FakeFeed>(1);
    auto* feed_side       = feed_side_owned.get();

    Engine feed_engine(KisConfig{});
    feed_engine.set_zmq_enabled(false);
    feed_engine.set_strategy_shards(1);
    feed_engine.set_feed_source(std::move(feed_side_owned), 1'000'000.0);
    feed_engine.set_role(ProcessRole::Feed);
    feed_engine.start();

    CHECK(feed_engine.is_running());
    CHECK(feed_side->is_connected());    // 구독 목록이 비어도 연다 — 체결통보를 이 소켓이 듣는다
    CHECK(feed_engine.order_count() == 0);
    CHECK(feed_engine.feed_channel_lanes() == feed_engine.websocket_lanes() + 1);

    // 번호를 다는 쪽은 주문 하나다 — 시세는 표에 없는 티커가 오면 버리고 센다. 여기서 청하면 제어 줄의
    //  보내는 쪽이 둘이 되어 한줄 큐가 깨진다. 아무도 청하지 않은 종목이 세션에 실려 온 자리다. [why D-114 단계 5]
    feed_side->emit_trade(0, "000660", 70000.0, 93001);
    CHECK(feed_engine.unknown_ticker_dropped() == 1);
    CHECK(feed_engine.feed_channel_pending_trades(0) == 0);
    CHECK(feed_engine.feed_channel_overflows() == 0);

    // 3. 전략 역할 — 전략은 올라가지만 주문·소켓은 이 프로세스에 없다. 자리표는 주문 쪽이 만든 쪽지에 붙는다.
    {
        auto  feed_owned     = std::make_unique<FakeFeed>(1);
        auto* feed           = feed_owned.get();
        auto  strategy_owned = std::make_unique<BuyOnce>("005930");
        auto* strategy       = strategy_owned.get();

        Engine engine(KisConfig{});
        engine.set_zmq_enabled(false);
        engine.set_strategy_shards(1);
        engine.add_strategy(std::move(strategy_owned));
        engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
        engine.set_role(ProcessRole::Strategy);
        engine.start();

        CHECK(engine.is_running());
        CHECK(engine.shard_count() == 1);              // 틱 파이프라인 자리는 전략 쪽에 남는다
        CHECK(!feed->is_connected());                  // 시세 소켓은 시세 쪽이 쥔다
        CHECK(engine.order_count() == 0);              // 주문 스레드가 없다
        CHECK(engine.feed_channel_lanes() == engine.websocket_lanes() + 1); // 꺼내는 쪽도 같은 줄 수를 본다

        // 종목 표는 이제 한 장이다 — 전략이 on_start 에서 부탁한 번호를 주문 쪽이 그 표에 넣고, 전략은
        //  같은 표에서 읽는다. 셋이 같은 종목에 같은 번호를 본다. [why D-114]
        CHECK(engine.symbol_register_timeouts() == 0);
        const symbol::SymbolId shared_id = strategy->symbol_id();
        CHECK(shared_id != symbol::kNone);
        CHECK(order_engine.symbols().lookup("005930") == shared_id);

        // 전략이 보낸 구독 요청은 주문이 아니라 시세 쪽 제어 줄로 간다(ipc::routes_to_feed) — 소켓을 쥔
        //  쪽이 받아 소켓에 건다. 감시 스레드 바퀴가 5초라 그동안 기다린다. [why D-114 단계 5]
        WatchSpec expected;
        expected.ticker = "005930";

        const auto watch_deadline = std::chrono::steady_clock::now() + 15s;

        while (!feed_side->has_specification(expected) && std::chrono::steady_clock::now() < watch_deadline)
        {
            std::this_thread::sleep_for(50ms);
        }

        CHECK(feed_side->has_specification(expected));

        // 표에 번호가 생긴 뒤의 체결은 통로를 그대로 건넌다 — 번호가 한 표에서 나오니 꺼낸 쪽이 알아본다.
        feed_side->emit_trade(0, "005930", 70100.0, 93002);

        const auto deadline = std::chrono::steady_clock::now() + 5s;

        while (engine.data_count() == 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(10ms);
        }

        CHECK(engine.data_count() == 1);
        CHECK(engine.feed_channel_discarded() == 0);
        CHECK(feed_engine.unknown_ticker_dropped() == 1); // 더 늘지 않았다 — 표에서 번호를 찾았다

        // 전략 역할이 낸 스위치 요청은 제어 줄을 타고 주문 쪽에서 바뀐다 — 부른 자리에서는 안 바뀐다.
        //  값을 쥔 쪽이 주문 하나라는 것이 요점이다. [why D-114 단계 4]
        engine.request_entry_halt(true);
        CHECK(!engine.is_entry_halted());

        const auto halt_deadline = std::chrono::steady_clock::now() + 5s;

        while (!order_engine.is_entry_halted() && std::chrono::steady_clock::now() < halt_deadline)
        {
            std::this_thread::sleep_for(10ms);
        }

        CHECK(order_engine.is_entry_halted());

        engine.stop();
        CHECK(!engine.is_running());
    }

    feed_engine.stop();
    CHECK(!feed_engine.is_running());

    order_engine.stop();
    CHECK(!order_engine.is_running());

    return 0;
}

// spread가 1을 넘어도 배수가 음수가 되지 않는다. 음수가 나오면 팩토리가 0 이하를 버리고 기본 배수 1.0으로 되돌아가
//  점수 최하위 종목을 도리어 크게 산다. 1로 자른 결과와 같아야 한다.
int run_score_spread_case()
{
    symbol::SymbolTable symbols(64);
    universe::ScoreList scores;

    for (int index = 0; index < 6; ++index)
    {
        scores.push_back({symbols.intern("00000" + std::to_string(index)), static_cast<double>(index)});
    }

    scores.push_back({symbols.intern("000009"), -40.0}); // 하위로 크게 떨어진 종목 — z가 −2에 걸린다

    std::vector<double> multiplier;
    std::vector<double> sorted_raw;
    universe::score_to_mult(scores, 3.0, 0.8, 0.05, 5, multiplier, sorted_raw);
    CHECK(multiplier.size() == scores.size());

    for (const double value : multiplier)
    {
        CHECK(value >= 0.0);
    }

    std::vector<double> clamped;
    universe::score_to_mult(scores, 1.0, 0.8, 0.05, 5, clamped, sorted_raw);

    for (size_t index = 0; index < multiplier.size(); ++index)
    {
        CHECK(std::abs(multiplier[index] - clamped[index]) < 1e-12);
    }

    // 같은 버퍼를 두 번째 호출에 넘겨도 앞 결과가 섞이지 않는다.
    universe::score_to_mult(universe::ScoreList{}, 0.6, 0.8, 0.05, 5, multiplier, sorted_raw);
    CHECK(multiplier.empty());
    return 0;
}

// 동점은 티커 사전순이다. 순위 계산과 등록 상한 자르기가 같은 비교(ranks_before)를 쓰므로 여기서 한 번 고정한다.
int run_score_tie_order_case()
{
    symbol::SymbolTable symbols(64);
    const symbol::SymbolId later   = symbols.intern("035720"); // 먼저 들어가 번호는 작지만 사전순은 뒤다
    const symbol::SymbolId earlier = symbols.intern("005930");
    const symbol::SymbolId top     = symbols.intern("000660");

    CHECK(universe::ranks_before(1.0, earlier, 1.0, later, symbols));
    CHECK(!universe::ranks_before(1.0, later, 1.0, earlier, symbols));
    CHECK(!universe::ranks_before(1.0, earlier, 1.0, earlier, symbols));
    CHECK(universe::ranks_before(2.0, later, 1.0, earlier, symbols));

    const universe::ScoreList scores = {{later, 1.0}, {earlier, 1.0}, {top, 2.0}};
    std::vector<int>    rank;
    std::vector<size_t> order;
    universe::score_to_rank(scores, symbols, rank, order);
    CHECK(rank.size() == 3);
    CHECK(rank[2] == 1);
    CHECK(rank[1] == 2);
    CHECK(rank[0] == 3);
    return 0;
}

// 시세 표를 재스캔 사이에 이어 쓸 때, 이번 파일에 없는 종목 칸은 비워진다(옛 가격·이름이 남지 않는다).
int run_quote_table_reload_case()
{
    const std::filesystem::path directory = Logger::executable_directory() / "logs_test";
    std::filesystem::create_directories(directory);
    const std::string path = (directory / "quote_table_case.json").string();
    auto write_file = [&path](const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    };

    symbol::SymbolTable  symbols(64);
    universe::QuoteTable quotes;
    write_file(R"({"ts": 0, "prices": {"005930": {"px": 70000, "val": 1000, "vol": 10, "nm": "삼성전자"},)"  // [wire] prices.json 키
               R"( "000660": {"px": 200000, "val": 2000, "vol": 20, "nm": "SK하이닉스"}}})");  // [wire] prices.json 키
    universe::load_quote_table(path, quotes, symbols);
    const symbol::SymbolId samsung = symbols.lookup("005930");
    const symbol::SymbolId hynix   = symbols.lookup("000660");
    CHECK(samsung != symbol::kNone);
    CHECK(hynix != symbol::kNone);
    CHECK(quotes.size() == symbols.capacity());
    CHECK(quotes[hynix].price == 200000.0);
    CHECK(quotes[hynix].name == "SK하이닉스");

    write_file(R"({"ts": 0, "prices": {"005930": {"px": 71000, "val": 1500, "vol": 15, "nm": "삼성전자"}}})");  // [wire] prices.json 키
    universe::load_quote_table(path, quotes, symbols);
    CHECK(quotes[samsung].price == 71000.0);
    CHECK(quotes[samsung].value == 1500.0);
    CHECK(quotes[samsung].name == "삼성전자");
    CHECK(quotes[hynix].price == 0.0);
    CHECK(quotes[hynix].value == 0.0);
    CHECK(quotes[hynix].volume == 0.0);
    CHECK(quotes[hynix].name.empty());

    // 파일이 없으면 전 칸이 비워진 채로 돌아온다.
    std::filesystem::remove(path);
    universe::load_quote_table(path, quotes, symbols);
    CHECK(quotes[samsung].price == 0.0);
    CHECK(quotes[samsung].name.empty());
    return 0;
}

} // namespace

int main()
{
    // 산출물은 실행파일 옆 logs_test/ — 기본 logs/는 트레이더·부하 하네스와 같은 폴더라, 거기 남은 open_orders.txt를
    //  OrderRouter가 이전 세션 미체결로 읽어 수만 건 취소에 매달리고 이 테스트의 주문이 5초 안에 안 나온다(09-22 실제로 났다).
    Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");

    if (const int result_code = run_case(1, 1, {"005930"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_case(2, 2, {"005930", "000660"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_spanning_case(2, 4, {"005930", "000660", "005380"}); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_replay_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_protective_claim_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_stale_entry_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_symbol_role_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_switch_role_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_split_start_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_manual_order_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_score_spread_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_score_tie_order_case(); result_code != 0)
    {
        return result_code;
    }

    if (const int result_code = run_quote_table_reload_case(); result_code != 0)
    {
        return result_code;
    }

    std::cout << "test_engine OK (" << g_checks << " checks)\n";
    return 0;
}
