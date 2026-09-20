// 주문 경로의 문자열 키 비용 — OrderGate::check()가 신호마다 만드는 중복 키(last_signal_), BUY NEW가 도는
//  진입 우선순위 표(entry_rank_ 전체 순회), 체결마다 두 번 묻는 전략 서브원장(strategy_positions_).
//  틱 경로는 종목 정수 id로 끝났고(D-105) 주문·체결 경로에 문자열 키가 남아 있어, 정수 키로 바꾸기 전후를
//  같은 하네스로 잰다. 세 장면을 따로 재서 어느 키가 얼마를 먹는지 보이게 한다.
//   1) SELL NEW 300종목 순환 — 통과 경로. 중복 키 생성 + last_signal_ find/insert가 문자열 조회의 전부.
//   2) BUY NEW 신규 종목, entry_priority 300종목·보유 20/25 — entry_rank_ find + 표 전체 순회(항목마다 원장 키 조회 2회).
//   3) on_fill_confirmed BUY, 전략 id 40개 순환 — strategy_positions_/strategy_average_prices_ 각 2회.
//   4) 체결통보 키 — 옛 방식(날짜·ODNO·시각·수량·단가를 문자열로 이어 붙여 unordered_map<string,int>에 ++)과
//      새 방식(정수 다섯 개 구조체 FillKey에 ++)을 같은 통보 열로 잰다. 라우터 seen_fills_가 통보마다 하는 일.
//  체크섬을 찍어 컴파일러가 루프를 못 지운다. 단일 스레드라 뮤텍스는 경합 없이 잡힌다.
//  측정(2026-09-21, x64 Release, 같은 기계·같은 하네스, D-112 전 → 후):
//   1) SELL NEW 161.3 → 148.0 ns   2) BUY NEW 588.8 → 455.6 ns   3) on_fill_confirmed 72.5 → 65.8 ns
//   4) 체결통보 키 296.4(문자열) → 31.2(FillKey) ns — 통보마다 문자열을 만들던 비용이 가장 컸다.
#include "ipc/FillKey.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

constexpr int    kUniverse   = 300;
constexpr int    kSlots      = 25;
constexpr int    kHeld       = 20; // 슬롯을 다 채우면 동시보유 단에서 먼저 거부돼 우선순위 표까지 못 간다
constexpr int    kStrategies = 40;
constexpr size_t kRounds     = 2'000'000;

OrderGate::Config bench_config()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker  = 1'000'000;
    config.max_orders_per_min       = 1'000'000'000;
    config.max_orders_per_sec       = 1'000'000'000;
    config.deduplicate_window_sec   = 0.0; // 창 0 — find는 하되 거부는 안 한다
    config.max_concurrent_positions = kSlots;
    config.entry_priority_enabled   = true;
    return config;
}

std::string ticker_of(int index)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%06d", index + 1);
    return buffer;
}

template <typename Function>
double measure_ns(size_t count, Function&& function)
{
    const auto start = std::chrono::high_resolution_clock::now();
    function();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - start).count();
    return static_cast<double>(elapsed) / static_cast<double>(count);
}

} // namespace

int main()
{
    OrderGate gate;
    gate.set_config(bench_config());

    std::vector<std::string>              tickers;
    std::vector<OrderGate::PriorityEntry> priority;

    for (int index = 0; index < kUniverse; ++index)
    {
        tickers.push_back(ticker_of(index));
        priority.push_back({gate.intern_symbol(tickers.back()), index + 1, 1.0 - index * 0.01});
    }

    gate.set_entry_priority(priority, kUniverse);

    // 슬롯 20/25 — BUY NEW 신규 종목이 동시보유 단을 지나 우선순위 표를 끝까지 돌게 된다.
    for (int index = 0; index < kHeld; ++index)
    {
        gate.seed_position(std::string(), tickers[static_cast<size_t>(index)], 10, 10000.0);
    }

    std::string reject_reason;
    long long   checksum = 0;

    // 1) SELL NEW — 보유 종목만 순환(보유 없으면 매도가 거부돼 중복 키를 찍지 않는다)
    OrderSignal sell;
    sell.side        = OrderSide::SELL;
    sell.type        = OrderType::MARKET;
    sell.quantity    = 1;
    sell.strategy_id    = "DEVSCALE_A";
    sell.strategy_index = gate.strategy_index_of(sell.strategy_id);
    sell.symbol_id      = symbol::kNone;

    const double sell_ns = measure_ns(kRounds, [&] {
        for (size_t round = 0; round < kRounds; ++round)
        {
            sell.ticker    = tickers[round % kHeld];
            sell.symbol_id = gate.symbol_id_of(sell.ticker);
            checksum += gate.check(sell, reject_reason) ? 1 : 0;
        }
    });

    // 2) BUY NEW 신규 종목 — 우선순위 표를 전부 돈 뒤 유효 랭크로 통과/거부가 갈린다
    OrderSignal buy;
    buy.side        = OrderSide::BUY;
    buy.type        = OrderType::LIMIT;
    buy.quantity    = 1;
    buy.price       = 10000.0;
    buy.strategy_id    = "DEVSCALE_A";
    buy.strategy_index = gate.strategy_index_of(buy.strategy_id);

    const double buy_ns = measure_ns(kRounds, [&] {
        for (size_t round = 0; round < kRounds; ++round)
        {
            buy.ticker    = tickers[kHeld + round % (kUniverse - kHeld)];
            buy.symbol_id = gate.symbol_id_of(buy.ticker);
            checksum += gate.check(buy, reject_reason) ? 1 : 0;
        }
    });

    // 3) 체결 확인 — 전략 서브원장 문자열 키
    std::vector<strategy_table::StrategyId> strategies;

    for (int index = 0; index < kStrategies; ++index)
    {
        strategies.push_back(gate.strategy_index_of("DEVSCALE_" + ticker_of(index)));
    }

    const double fill_ns = measure_ns(kRounds, [&] {
        for (size_t round = 0; round < kRounds; ++round)
        {
            const auto result = gate.on_fill_confirmed(std::string(), tickers[round % kHeld], OrderSide::BUY, 1, 10000.0,
                                                       strategies[round % kStrategies]);
            checksum += result.net_quantity;
        }
    });

    // 4) 체결통보 키 — 문자열 vs 정수 구조체. 주문 1,000건이 각각 여러 번 체결되는 열(ODNO·시각·수량이 돈다).
    constexpr size_t kOrders = 1'000;
    const size_t     kFills  = kRounds / 4;
    std::unordered_map<std::string, int>                              seen_by_string;
    std::unordered_map<fill_key::FillKey, int, fill_key::FillKeyHash> seen_by_key;

    const double fill_key_string_ns = measure_ns(kFills, [&] {
        for (size_t round = 0; round < kFills; ++round)
        {
            const std::string key = std::format("{}:{:010}:{:06}:{}:{}", "20260921", round % kOrders, 90000 + round % 3600,
                                                1 + static_cast<int>(round % 7), 1000000 + static_cast<long long>(round % 50) * 100);
            checksum += ++seen_by_string[key];
        }
    });

    const double fill_key_struct_ns = measure_ns(kFills, [&] {
        for (size_t round = 0; round < kFills; ++round)
        {
            const fill_key::FillKey key{20260921u, static_cast<uint64_t>(round % kOrders), static_cast<uint32_t>(90000 + round % 3600),
                                        1 + static_cast<int32_t>(round % 7), 1000000 + static_cast<int64_t>(round % 50) * 100};
            checksum += ++seen_by_key[key];
        }
    });

    std::printf("check SELL NEW (last_signal_ key)            : %.1f ns/signal\n", sell_ns);
    std::printf("check BUY NEW  (entry_rank_ find + traverse) : %.1f ns/signal\n", buy_ns);
    std::printf("on_fill_confirmed (strategy sub-ledger)      : %.1f ns/fill\n", fill_ns);
    std::printf("fill key, string format + map<string>        : %.1f ns/fill\n", fill_key_string_ns);
    std::printf("fill key, FillKey struct + map<FillKey>      : %.1f ns/fill\n", fill_key_struct_ns);
    std::printf("checksum %lld\n", checksum);
    return 0;
}
