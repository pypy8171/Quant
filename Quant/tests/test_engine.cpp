// Engine 한 바퀴 단위 테스트. 가짜 피드 소스를 주입해 KIS·소켓 없이 틱→샤드→전략→디스패치→주문 스레드→모의 체결→
// 체결 소비 스레드→원장(보유)까지 도는지, 주입 모드가 브로커 없이 기동·종료하는지 고정한다. 관련 결정: D-071(Phase 3).
// 스레드: 테스트 스레드가 피드 소스의 수신 스레드 역할(레인 0)을 하고 나머지는 Engine이 띄운다.
// 빌드: cmake --build <dir> --target test_engine
#include "core/Engine.h"
#include "core/IFeedSource.h"
#include "strategy/StrategyBase.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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

constexpr const char* kTicker = "005930";

// 콜백을 받아 두고 테스트가 부르는 대로 틱을 내보내는 피드 소스. 연결·구독은 기록만 하고 성공이라 답한다.
class FakeFeed : public feed::IFeedSource
{
public:
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

    // 수신 스레드가 디코드 직후 부르는 자리 — 여기서는 테스트 스레드가 그 역할을 한다.
    void emit_trade(double price, int32_t hhmmss)
    {
        TradeData td;
        td.ticker.assign(kTicker);
        td.price     = price;
        td.quantity  = 10;
        td.direction = 1;
        td.hhmmss    = hhmmss;

        if (lane_trade_)
        {
            lane_trade_(0, td);
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
    mutable std::mutex     mtx_;
    std::vector<WatchSpec> specs_;
    std::atomic<bool>      connected_{false};
    OrderBookCb            on_ob_;
    TradeCb                on_trade_;
    LaneOrderBookCb        lane_ob_;
    LaneTradeCb            lane_trade_;
};

// 첫 틱에 시장가 1주 매수 한 번. 이후는 침묵. 종목 비교는 id로(원칙 6).
class BuyOnce : public StrategyBase
{
public:
    std::string id() const override { return "buy_once"; }
    std::string describe() const override { return "첫 틱에 1주 매수"; }
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    void on_start() override { sym_ = symbol_of(kTicker); }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        WatchSpec w;
        w.ticker = kTicker;
        return {w};
    }

    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        ++ticks_seen;

        if (fired_ || !same_symbol(sym_, kTicker, td.sym, td.ticker.str()))
        {
            return std::nullopt;
        }

        fired_ = true;
        OrderSignal sig;
        sig.ticker      = kTicker;
        sig.sym         = td.sym;
        sig.side        = OrderSide::BUY;
        sig.type        = OrderType::MARKET;
        sig.quantity    = 1;
        sig.ref_price   = td.price;
        sig.strategy_id = id();
        return sig;
    }

    std::atomic<int> ticks_seen{0};

private:
    sym::SymbolId sym_{};
    bool          fired_ = false;
};
} // namespace

int main()
{
    using namespace std::chrono_literals;

    auto  feed_owned = std::make_unique<FakeFeed>();
    auto* feed       = feed_owned.get();
    auto  strat      = std::make_unique<BuyOnce>();
    auto* strat_raw  = strat.get();

    Engine eng(KisConfig{});
    eng.add_strategy(std::move(strat));
    eng.set_feed_source(std::move(feed_owned), 1'000'000.0);
    eng.start();

    // 1. 브로커 없이 떴고, 소스는 전략의 구독 종목으로 연결됐다.
    CHECK(eng.is_running());
    CHECK(feed->is_connected());
    {
        const auto specs = feed->specs();
        CHECK(specs.size() == 1 && specs[0].ticker == kTicker);
    }

    // 2. 첫 틱이 전략까지 닿고 매수 신호가 주문 큐로 간다. 접수는 모의 체결기 — 체결은 다음 틱에서.
    feed->emit_trade(70000.0, 93001);
    const auto deadline = std::chrono::steady_clock::now() + 5s;

    while (eng.order_count() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }

    CHECK(eng.order_count() == 1);
    CHECK(strat_raw->ticks_seen.load() >= 1);

    // 3. 다음 틱이 시장가를 그 가격에 체결시키고 체결 소비 스레드가 원장에 반영한다 — 보유 1주, 평단 70100.
    std::vector<OrderGate::HeldPos> held;

    for (int i = 0; i < 400; ++i)
    {
        feed->emit_trade(70100.0, 93002 + i);
        held = eng.held_positions();

        if (!held.empty())
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(held.size() == 1);
    CHECK(held[0].ticker == kTicker);
    CHECK(held[0].qty == 1);
    CHECK(held[0].avg_price > 70099.0 && held[0].avg_price < 70101.0);
    CHECK(eng.signal_count() == 1);

    // 4. 정지가 장 외 대기(60초)를 기다리지 않는다.
    const auto t0 = std::chrono::steady_clock::now();
    eng.stop();
    CHECK(!eng.is_running());
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);

    std::cout << "test_engine OK (" << g_checks << " checks)\n";
    return 0;
}
