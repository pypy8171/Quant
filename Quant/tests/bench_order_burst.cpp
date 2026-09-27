// 주문 몰림 재현 — 한꺼번에 든 주문 N건이 진짜 Engine 주문 스레드를 지나 접수되기까지 걸린 시간을 잰다.
//  09-14 청산 41건이 한 스레드의 동기 왕복 뒤에 줄을 섰다. 전송 스레드(order_transport_threads)를 두기 전후를
//  같은 입력으로 비교하는 것이 목적이다. [why D-151]
//
//  쓰는 법(저장소 루트에서):
//    bench_order_burst --threads 0 --orders 41 --interval 500 --rtt-file C:/tmp/ot_rtt_ms.txt
//    bench_order_burst --threads 4 --orders 41 --interval 500 --rtt-file C:/tmp/ot_rtt_ms.txt
//
//  왕복은 모의 체결기의 접수 답을 늦춰 흉내 낸다. --rtt-file은 한 줄에 ms 하나 — 실제 접수 로그의 RTT에서
//  버킷 대기를 뺀 값을 순서대로 쓴다. 파일이 없으면 --rtt-ms 고정값을 쓴다.
//  [inv] 주문은 운영단말 수동주문 입구(accept_manual_order)로 넣는다 — 전략 없이 주문 스레드 고리를 그대로 탄다.

#include "core/Engine.h"
#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

// 시세를 내지 않는 피드 — 이 벤치는 주문 쪽만 본다.
class SilentFeed : public feed::IFeedSource
{
public:
    void set_callbacks(OrderBookCb, TradeCb) override
    {
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
    engine.set_feed_source(std::make_unique<SilentFeed>(), 1e15);
    engine.start();

    std::atomic<size_t> next_rtt{0};
    engine.set_paper_acknowledgement_delay([&rtt, &next_rtt]
    {
        return std::chrono::milliseconds(rtt[next_rtt.fetch_add(1) % rtt.size()]);
    });

    const auto started = std::chrono::steady_clock::now();

    for (int index = 0; index < orders; ++index)
    {
        char ticker[7];
        std::snprintf(ticker, sizeof(ticker), "%06d", 100 + index);
        OpsOrderReq request;
        request.client_id = "burst" + std::to_string(index);
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

    engine.stop();

    std::cout << "threads=" << threads << " orders=" << orders << " interval=" << interval_ms << "ms rtt값="
              << rtt.size() << "개 접수=" << accepted_ms.size() << "\n";
    std::cout << "접수까지 p50=" << percentile(accepted_ms, 0.5) << "ms p90=" << percentile(accepted_ms, 0.9)
              << "ms 마지막=" << (accepted_ms.empty() ? -1 : accepted_ms.back()) << "ms\n";
    return accepted_ms.size() == static_cast<size_t>(orders) ? 0 : 1;
}
