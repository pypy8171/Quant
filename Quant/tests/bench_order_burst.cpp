// 주문 몰림 재현 — 한꺼번에 든 주문 N건이 진짜 Engine 주문 스레드를 지나 접수되기까지 걸린 시간을 잰다.
//  09-14 청산 41건이 한 스레드의 동기 왕복 뒤에 줄을 섰다. 전송 스레드(order_transport_threads)를 두기 전후를
//  같은 입력으로 비교하는 것이 목적이다. [why D-151]
//
//  쓰는 법(저장소 루트에서):
//    bench_order_burst --threads 0 --orders 41 --interval 500 --rtt-file C:/tmp/ot_rtt_ms.txt
//    bench_order_burst --threads 4 --orders 41 --interval 500 --rtt-file C:/tmp/ot_rtt_ms.txt
//
//    bench_order_burst --threads 4 --orders 41 --interval 500 --cancels 1   (매수 41건 접수 뒤 취소 41건 몰림)
//
//  왕복은 모의 체결기의 접수·취소 답을 늦춰 흉내 낸다. --rtt-file은 한 줄에 ms 하나 — 실제 접수 로그의 RTT에서
//  버킷 대기를 뺀 값을 순서대로 쓴다. 파일이 없으면 --rtt-ms 고정값을 쓴다.
//  [inv] 신규 주문은 운영단말 수동주문 입구(accept_manual_order)로 넣는다 — 전략 없이 주문 스레드 고리를 그대로 탄다.
//  취소는 그 입구가 받지 않아 전략(CancelBurst)이 낸다 — 체결 한 건을 흘려 전략을 깨운다.

#include "core/Engine.h"
#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

// 스스로는 시세를 내지 않는 피드 — 이 벤치는 주문 쪽만 본다. 취소 구간만 전략을 깨울 체결을 흘린다.
class SilentFeed : public feed::IFeedSource
{
public:
    void set_callbacks(OrderBookCb, TradeCb on_trade) override
    {
        on_trade_ = std::move(on_trade);
    }

    void emit_trade(const std::string& ticker, double price)
    {
        TradeData trade;
        trade.ticker.assign(ticker);
        trade.price     = price;
        trade.quantity  = 1;
        trade.direction = 1;
        trade.hhmmss    = 100000;

        if (on_trade_)
        {
            on_trade_(trade);
        }
    }

    bool connect(const std::vector<WatchSpec>&) override
    {
        connected_.store(true);
        return true;
    }

    void disconnect() override
    {
        connected_.store(false);
    }

    bool subscribe_incremental(const WatchSpec&) override
    {
        return true;
    }

    bool has_specification(const WatchSpec&) const override
    {
        return false;
    }

    std::vector<WatchSpec> take_overflow_specifications() override
    {
        return {};
    }

    bool is_connected() const override
    {
        return connected_.load();
    }

    bool is_stale(int) const override
    {
        return false;
    }

private:
    std::atomic<bool> connected_{false};
    TradeCb           on_trade_;
};

// 신호가 들어오면 한 번에 취소 N건을 낸다. 대상은 앞 구간 매수의 내부 주문번호(ORD-000001부터 차례)다 —
//  라우터가 번호를 1부터 차례로 매기고, 이 벤치는 그 앞에 다른 주문을 내지 않는다.
class CancelBurst : public StrategyBase
{
public:
    CancelBurst(std::vector<std::string> tickers, const std::atomic<bool>& armed)
        : tickers_(std::move(tickers)), armed_(armed)
    {
    }

    const std::string& id() const override
    {
        return identifier_;
    }

    std::string describe() const override
    {
        return "주문 몰림 벤치 — 취소 몰림";
    }

    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
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

    void on_trade_batch(const TradeData&, std::vector<OrderSignal>& out) override
    {
        if (fired_ || !armed_.load())
        {
            return;
        }

        fired_ = true;

        for (size_t index = 0; index < tickers_.size(); ++index)
        {
            char order_id[16];
            std::snprintf(order_id, sizeof(order_id), "ORD-%06zu", index + 1);
            OrderSignal signal;
            signal.ticker                       = tickers_[index];
            signal.symbol_id                    = symbol_of(tickers_[index]);
            signal.side                         = OrderSide::BUY;
            signal.type                         = OrderType::LIMIT;
            signal.quantity                     = 0;
            signal.strategy_id                  = identifier_;
            signal.action                       = OrderAction::CANCEL;
            signal.original_client_order_id     = order_id;
            signal.original_client_order_number = index + 1;
            signal.timestamp                    = std::chrono::system_clock::now();
            out.push_back(std::move(signal));
        }
    }

private:
    std::string              identifier_ = "bench_cancel";
    std::vector<std::string> tickers_;
    const std::atomic<bool>& armed_;
    bool                     fired_ = false; // 샤드 스레드만 만진다
};

std::vector<int> load_rtt(const std::string& path)
{
    std::vector<int> values;
    std::ifstream    file(path);

    for (int value = 0; file >> value;)
    {
        values.push_back(value);
    }

    return values;
}

int64_t percentile(std::vector<int64_t> values, double fraction)
{
    if (values.empty())
    {
        return -1;
    }

    std::sort(values.begin(), values.end());
    const auto index = std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<double>(values.size())));
    return values[index];
}

} // namespace

int main(int argc, char** argv)
{
    int         threads     = 0;
    int         orders      = 41;
    int         interval_ms = 500;
    int         fixed_rtt   = 1500;
    std::string rtt_file;
    bool        cancels     = false;

    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string argument = argv[index];
        const std::string value    = argv[index + 1];

        if (argument == "--threads")
        {
            threads = std::stoi(value);
        }
        else if (argument == "--orders")
        {
            orders = std::stoi(value);
        }
        else if (argument == "--interval")
        {
            interval_ms = std::stoi(value);
        }
        else if (argument == "--rtt-ms")
        {
            fixed_rtt = std::stoi(value);
        }
        else if (argument == "--rtt-file")
        {
            rtt_file = value;
        }
        else if (argument == "--cancels")
        {
            cancels = value != "0";
        }
    }

    // 운영 로그 폴더를 건드리지 않게 부하 하네스와 같은 곳에 쓴다.
    Logger::instance().set_base_directory(Logger::executable_directory() / "logs_bench");
    Logger::instance().initialize(Logger::instance().path_for("bench_order_burst.log"), LogLevel::ERROR);

    std::vector<int> rtt = rtt_file.empty() ? std::vector<int>{} : load_rtt(rtt_file);

    if (rtt.empty())
    {
        rtt.push_back(fixed_rtt);
    }

    Engine engine(KisConfig{});
    // 한도는 풀어 둔다 — 여기서 재는 것은 줄서기이지 리스크 규칙이 아니다. 간격만 운영값으로 둔다.
    OrderGate::Config risk;
    risk.max_quantity_per_ticker  = 1'000'000'000;
    risk.max_quantity_per_order   = 1'000'000;
    risk.max_notional_per_order   = 1e15;
    risk.max_notional_per_ticker  = 0.0;
    risk.max_concurrent_positions = 0;
    risk.max_orders_per_sec       = 1'000'000;
    risk.max_orders_per_min       = 60'000'000;
    risk.deduplicate_window_sec   = 0.0;
    risk.daily_loss_limit         = -1e15;
    engine.set_risk_config(risk);
    engine.set_order_interval(interval_ms, 0);
    engine.set_order_transport_threads(threads);
    engine.set_zmq_enabled(false);
    std::vector<std::string> tickers;

    for (int index = 0; index < orders; ++index)
    {
        char ticker[7];
        std::snprintf(ticker, sizeof(ticker), "%06d", 100 + index);
        tickers.emplace_back(ticker);
    }

    std::atomic<bool> armed{false};
    auto              feed_owned = std::make_unique<SilentFeed>();
    SilentFeed*       feed       = feed_owned.get();

    if (cancels)
    {
        engine.add_strategy(std::make_unique<CancelBurst>(tickers, armed));
    }

    engine.set_feed_source(std::move(feed_owned), 1e15);
    engine.start();

    // 취소 구간의 왕복 끝 시각 — 모의 체결기가 답을 늦추기 시작한 때 + 그 지연. 증권사가 답한 때라
    //  신규의 접수 시각과 같은 뜻이다.
    std::mutex                            cancel_mutex;
    std::vector<int64_t>                  cancel_done_ms;
    std::chrono::steady_clock::time_point cancel_started{};
    std::atomic<size_t>                   next_rtt{0};
    engine.set_paper_acknowledgement_delay([&]
    {
        const auto delay = std::chrono::milliseconds(rtt[next_rtt.fetch_add(1) % rtt.size()]);

        if (armed.load())
        {
            std::lock_guard<std::mutex> lock(cancel_mutex);
            cancel_done_ms.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() + delay - cancel_started)
                                         .count());
        }

        return delay;
    });

    const auto started = std::chrono::steady_clock::now();

    for (const std::string& ticker : tickers)
    {
        OpsOrderReq request;
        request.client_id = "burst" + ticker;
        request.ticker    = ticker;
        request.side      = "BUY";
        request.quantity  = 1;
        request.price     = 10000.0;
        const std::string refusal = engine.accept_manual_order(request);

        if (!refusal.empty())
        {
            std::cerr << "입구 거부 " << ticker << ": " << refusal << "\n";
        }
    }

    // 접수 수가 늘 때마다 그 시각을 적는다.
    std::vector<int64_t> accepted_ms;
    uint64_t             seen     = engine.order_count();
    const auto           deadline = started + std::chrono::minutes(3);

    while (accepted_ms.size() < static_cast<size_t>(orders) && std::chrono::steady_clock::now() < deadline)
    {
        const uint64_t count = engine.order_count();

        for (; seen < count; ++seen)
        {
            accepted_ms.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool cancel_complete = true;

    if (cancels && accepted_ms.size() == static_cast<size_t>(orders))
    {
        cancel_started = std::chrono::steady_clock::now();
        armed.store(true);
        const auto cancel_deadline = cancel_started + std::chrono::minutes(3);

        // 전략이 체결을 받아 취소를 낼 때까지 체결을 흘린다. 매수 지정가 10000원보다 높은 값이라 체결되지 않는다.
        while (true)
        {
            {
                std::lock_guard<std::mutex> lock(cancel_mutex);

                if (cancel_done_ms.size() >= static_cast<size_t>(orders) ||
                    std::chrono::steady_clock::now() > cancel_deadline)
                {
                    break;
                }
            }

            feed->emit_trade(tickers.front(), 20000.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::vector<int64_t> done;
        {
            std::lock_guard<std::mutex> lock(cancel_mutex);
            done = cancel_done_ms;
        }

        std::sort(done.begin(), done.end());
        const int64_t last_ms = done.empty() ? 0 : done.back();
        // 마지막 취소의 답이 돌아올 때까지 둔다.
        std::this_thread::sleep_until(cancel_started + std::chrono::milliseconds(last_ms + 200));
        cancel_complete = done.size() == static_cast<size_t>(orders);
        std::cout << "취소 답까지 p50=" << percentile(done, 0.5) << "ms p90=" << percentile(done, 0.9)
                  << "ms 마지막=" << (done.empty() ? -1 : done.back()) << "ms (취소 " << done.size() << "건)\n";
    }

    engine.stop();

    std::cout << "threads=" << threads << " orders=" << orders << " interval=" << interval_ms << "ms rtt값="
              << rtt.size() << "개 접수=" << accepted_ms.size() << "\n";
    std::cout << "접수까지 p50=" << percentile(accepted_ms, 0.5) << "ms p90=" << percentile(accepted_ms, 0.9)
              << "ms 마지막=" << (accepted_ms.empty() ? -1 : accepted_ms.back()) << "ms\n";
    return accepted_ms.size() == static_cast<size_t>(orders) && cancel_complete ? 0 : 1;
}
