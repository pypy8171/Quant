// OrderRouter::submit(NEW) 한 건의 벽시계 시간 — 발주 스레드 hot path(new_route: 클램프 → 중복 검사 → 게이트 →
//  발주 → 마무리)를 KIS 없이 잰다. 발주는 즉시 접수를 돌려주는 가짜 실행기가 받는다. 로그는 WARN 이상만 남겨
//  접수 로그 한 줄의 파일 쓰기가 측정을 덮지 않게 한다. 원장 CSV·미체결 목록은 라우터가 평소대로 쓰기 스레드에 넘긴다.
//  종목 500개를 돌며 BUY 지정가 1주씩 kOrders건을 낸다. 접수된 주문은 살아 있는 채로 쌓이고 건마다 미체결 목록을
//  다시 만들므로 건당 비용이 살아 있는 주문 수에 비례한다 — 하루 규모(1,000건)로 끊는다. 라운드마다 라우터·게이트를 새로 만들고, 라운드별 건당 평균과
//  전체 표본의 p50·p99를 찍는다. 체크섬(접수 건수)을 찍어 결과를 버리지 않게 한다.
//  접수 주문의 구간별 평균(us)도 찍는다 — 어느 구간이 늘었는지 가른다.
//  부속 파일은 exe 옆 logs_bench/에 쓰인다. 두 빌드를 비교할 때는 exe를 같은 폴더에 두고 번갈아 돌린다 —
//  폴더가 다르면 쓰기 스레드 비용이 달라 p50이 10us 넘게 벌어졌다(2026-09-27 측정).
#include "api/IOrderExecutor.h"
#include "ipc/OrderRouter.h"
#include "risk/OrderGate.h"
#include "utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

constexpr int kUniverse = 500;
constexpr int kOrders   = 1'000;
constexpr int kRounds   = 20;

// 발주를 바로 접수로 돌려준다. 주문번호는 건마다 다르게 준다 — 라우터가 번호로 색인을 만든다.
struct InstantExecutor : IOrderExecutor
{
    long long order_number = 0;

    bool is_paper() const noexcept override
    {
        return true;
    }

    KisResult<std::vector<OpenOrder>> get_open_orders() override
    {
        return std::vector<OpenOrder>{};
    }

    KisResult<std::vector<DailyOrderFill>> get_daily_order_fills() override
    {
        return std::vector<DailyOrderFill>{};
    }

    OrderAck submit_order_acknowledgement(const OrderSignal&) override
    {
        return OrderAck{std::format("{:010}", ++order_number), "ORG000001", std::string()};
    }

    OrderAck cancel_order(const std::string&, const std::string&, const std::string&, int, bool) override
    {
        return OrderAck{"C000000001", std::string(), std::string()};
    }

    OrderAck revise_order(const std::string&, const std::string&, const std::string&, int, double) override
    {
        return OrderAck{"0000000001", std::string(), std::string()};
    }
};

OrderGate::Config open_config()
{
    OrderGate::Config config;
    config.max_orders_per_min     = 1'000'000'000;
    config.max_orders_per_sec     = 1'000'000'000;
    config.deduplicate_window_sec = 0.0;
    config.max_quantity_per_ticker = 1'000'000;
    config.daily_loss_limit       = -1e15;
    return config;
}

} // namespace

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    Logger::instance().set_base_directory(Logger::executable_directory() / "logs_bench");
    Logger::instance().set_min_level(LogLevel::WARN);

    std::vector<OrderSignal> signals;
    signals.reserve(kUniverse);

    for (int index = 0; index < kUniverse; ++index)
    {
        OrderSignal signal;
        signal.ticker      = std::format("{:06}", 100000 + index);
        signal.side        = OrderSide::BUY;
        signal.quantity    = 1;
        signal.price       = 10000.0;
        signal.strategy_id = "BENCH";
        signal.market      = Market::KR;
        signals.push_back(signal);
    }

    std::vector<double> samples_ns;
    samples_ns.reserve(static_cast<size_t>(kOrders) * kRounds);
    long long accepted = 0;
    // 접수된 주문의 구간별 시간 합(us) — 어느 구간이 늘었는지 가른다. 순서는 아래 kStageNames와 같다.
    constexpr const char* kStageNames[] = {"gate", "history_guard", "journal", "transport", "accept", "history_store",
                                           "open_orders"};
    long long stage_sum_us[std::size(kStageNames)] = {};

    for (int round = 0; round < kRounds; ++round)
    {
        OrderGate       gate(open_config());
        InstantExecutor executor;
        OrderRouter     router(gate, executor);
        double          round_total_ns = 0.0;

        for (int index = 0; index < kOrders; ++index)
        {
            const OrderSignal& signal = signals[static_cast<size_t>(index % kUniverse)];
            const auto started = std::chrono::steady_clock::now();
            const ManagedOrder managed_order = router.submit(signal);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const double elapsed_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            samples_ns.push_back(elapsed_ns);
            round_total_ns += elapsed_ns;

            if (managed_order.status == OrderStatus::ACCEPTED)
            {
                ++accepted;
                const OrderStageTiming& stages = managed_order.stages;
                stage_sum_us[0] += stages.gate_us;
                stage_sum_us[1] += stages.history_guard_us;
                stage_sum_us[2] += stages.journal_us;
                stage_sum_us[3] += stages.transport_us;
                stage_sum_us[4] += stages.accept_us;
                stage_sum_us[5] += stages.history_store_us;
                stage_sum_us[6] += stages.open_orders_us;
            }
        }

        std::printf("round %d: %.1f ns/submit\n", round, round_total_ns / kOrders);
    }

    std::sort(samples_ns.begin(), samples_ns.end());
    const double p50 = samples_ns[samples_ns.size() / 2];
    const double p99 = samples_ns[samples_ns.size() * 99 / 100];
    std::printf("stage mean us:");

    for (size_t stage = 0; stage < std::size(kStageNames); ++stage)
    {
        std::printf(" %s=%.2f", kStageNames[stage],
                    accepted > 0 ? static_cast<double>(stage_sum_us[stage]) / static_cast<double>(accepted) : 0.0);
    }

    std::printf("\n");
    std::printf("p50 %.0f ns  p99 %.0f ns  accepted %lld / %d\n", p50, p99, accepted, kOrders * kRounds);
    return accepted > 0 ? 0 : 1;
}
