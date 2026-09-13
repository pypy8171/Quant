// Engine 한 바퀴 단위 테스트. 시험용 피드 소스를 주입해 KIS·소켓 없이 틱→샤드→전략→디스패치→주문 스레드→모의 체결→
// 체결 소비 스레드→원장(보유)까지 도는지, 주입 모드가 브로커 없이 기동·종료하는지 고정한다. 케이스는 둘 — 레인 1×샤드 1과
// 레인 2×샤드 2(종목 둘이 서로 다른 레인에서 들어와 서로 다른 열에서 판단된다). 관련 결정: D-071(Phase 3·Phase 4 앞단계).
// 스레드: 테스트 스레드가 피드 소스의 수신 스레드 역할(레인 0..N-1)을 하고 나머지는 Engine이 띄운다.
// 빌드: cmake --build <dir> --target test_engine
#include "core/Engine.h"
#include "core/IFeedSource.h"
#include "core/ShardMatrix.h"
#include "strategy/StrategyBase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

// 콜백을 받아 두고 테스트가 부르는 대로 틱을 내보내는 피드 소스. 연결·구독은 기록만 하고 성공이라 답한다.
//  lanes()를 N으로 답해 Engine이 행렬 행을 N개 잡게 한다 — 실제 소켓 N개(FeedMux 레인 모드)와 같은 자리.
class FakeFeed : public feed::IFeedSource
{
public:
    explicit FakeFeed(uint32_t lanes) : lanes_(lanes) {}

    uint32_t lanes() const override { return lanes_; }

    void set_callbacks(OrderBookCb on_ob, TradeCb on_trade) override
    {
        on_ob_    = std::move(on_ob);
        on_trade_ = std::move(on_trade);
    }

    void set_lane_callbacks(LaneOrderBookCb on_ob, LaneTradeCb on_trade) override
    {
        lane_ob_    = std::move(on_ob);
        lane_trade_ = std::move(on_trade);
    }

    bool connect(const std::vector<WatchSpec>& specs) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        specs_     = specs;
        connected_ = true;
        return true;
    }

    void disconnect() override { connected_ = false; }

    bool subscribe_incremental(const WatchSpec& spec) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        specs_.push_back(spec);
        return true;
    }

    bool has_spec(const WatchSpec& spec) const override
    {
        std::lock_guard<std::mutex> lk(mtx_);

        for (const auto& s : specs_)
        {
            if (s.ticker == spec.ticker)
            {
                return true;
            }
        }

        return false;
    }

    std::vector<WatchSpec> take_overflow_specs() override { return {}; }
    bool                   is_connected() const override { return connected_.load(); }
    bool                   is_stale(int) const override { return false; }

    // 수신 스레드가 디코드 직후 부르는 자리 — 여기서는 테스트 스레드가 레인 lane의 역할을 한다.
    void emit_trade(uint32_t lane, const std::string& ticker, double price, int32_t hhmmss)
    {
        TradeData td;
        td.ticker.assign(ticker);
        td.price     = price;
        td.quantity  = 10;
        td.direction = 1;
        td.hhmmss    = hhmmss;

        if (lane_trade_)
        {
            lane_trade_(lane, td);
        }
        else if (on_trade_)
        {
            on_trade_(td);
        }
    }

    std::vector<WatchSpec> specs() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return specs_;
    }

private:
    uint32_t               lanes_;
    mutable std::mutex     mtx_;
    std::vector<WatchSpec> specs_;
    std::atomic<bool>      connected_{false};
    OrderBookCb            on_ob_;
    TradeCb                on_trade_;
    LaneOrderBookCb        lane_ob_;
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

    void on_start() override { sym_ = symbol_of(ticker_); }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        WatchSpec w;
        w.ticker = ticker_;
        return {w};
    }

    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        ++ticks_seen;

        if (fired_ || !same_symbol(sym_, ticker_, td.sym, td.ticker.str()))
        {
            return std::nullopt;
        }

        fired_ = true;
        OrderSignal sig;
        sig.ticker      = ticker_;
        sig.sym         = td.sym;
        sig.side        = OrderSide::BUY;
        sig.type        = OrderType::MARKET;
        sig.quantity    = 1;
        sig.ref_price   = td.price;
        sig.strategy_id = id();
        return sig;
    }

    sym::SymbolId    sym() const { return sym_; }
    std::atomic<int> ticks_seen{0};

private:
    std::string   ticker_;
    sym::SymbolId sym_{};
    bool          fired_ = false;
};

// 종목 i는 레인 i % lanes에서 들어온다. 종목마다 전략 하나. 종목 수만큼 주문·체결·보유가 잡히면 통과.
int run_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers)
{
    using namespace std::chrono_literals;
    std::cout << "case lanes=" << lanes << " shards=" << shards << " tickers=" << tickers.size() << "\n";

    auto  feed_owned = std::make_unique<FakeFeed>(lanes);
    auto* feed       = feed_owned.get();
    std::vector<BuyOnce*> strats;

    Engine eng(KisConfig{});
    eng.set_strategy_shards(shards);

    for (const auto& t : tickers)
    {
        auto st = std::make_unique<BuyOnce>(t);
        strats.push_back(st.get());
        eng.add_strategy(std::move(st));
    }

    eng.set_feed_source(std::move(feed_owned), 1'000'000.0);
    eng.start();

    // 1. 브로커 없이 떴고, 소스는 전략들의 구독 종목으로 연결됐고, 행렬은 요청한 레인×샤드 그대로다(샤드 1 폴백 없음).
    CHECK(eng.is_running());
    CHECK(feed->is_connected());
    CHECK(feed->specs().size() == tickers.size());
    CHECK(eng.ws_lanes() == lanes);
    CHECK(eng.shard_count() == shards);

    // 2. 종목들이 실제로 서로 다른 열에 떨어진다(샤드가 둘 이상일 때) — 같은 열이면 N×M을 시험한 것이 아니다.
    {
        std::set<uint32_t> cols;

        for (auto* st : strats)
        {
            cols.insert(shard::shard_of(st->sym(), shards));
        }

        CHECK(cols.size() == std::min<size_t>(shards, tickers.size()));
    }

    // 3. 첫 틱이 각 전략까지 닿고 매수 신호가 주문 큐로 간다. 접수는 모의 체결기 — 체결은 다음 틱에서.
    for (size_t i = 0; i < tickers.size(); ++i)
    {
        feed->emit_trade(static_cast<uint32_t>(i) % lanes, tickers[i], 70000.0, 93001);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;

    while (eng.order_count() < tickers.size() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }

    CHECK(eng.order_count() == tickers.size());

    for (auto* st : strats)
    {
        CHECK(st->ticks_seen.load() >= 1);
    }

    // 4. 다음 틱이 시장가를 그 가격에 체결시키고 체결 소비 스레드가 원장에 반영한다 — 종목마다 보유 1주, 평단 70100.
    std::vector<OrderGate::HeldPos> held;

    for (int i = 0; i < 400; ++i)
    {
        for (size_t k = 0; k < tickers.size(); ++k)
        {
            feed->emit_trade(static_cast<uint32_t>(k) % lanes, tickers[k], 70100.0, 93002 + i);
        }

        held = eng.held_positions();

        if (held.size() >= tickers.size())
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(held.size() == tickers.size());

    for (const auto& h : held)
    {
        CHECK(std::find(tickers.begin(), tickers.end(), h.ticker) != tickers.end());
        CHECK(h.qty == 1);
        CHECK(h.avg_price > 70099.0 && h.avg_price < 70101.0);
    }

    CHECK(eng.signal_count() == tickers.size());

    // 5. 정지가 장 외 대기(60초)를 기다리지 않는다.
    const auto t0 = std::chrono::steady_clock::now();
    eng.stop();
    CHECK(!eng.is_running());
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);
    return 0;
}
} // namespace

int main()
{
    if (const int rc = run_case(1, 1, {"005930"}); rc != 0)
    {
        return rc;
    }

    if (const int rc = run_case(2, 2, {"005930", "000660"}); rc != 0)
    {
        return rc;
    }

    std::cout << "test_engine OK (" << g_checks << " checks)\n";
    return 0;
}
