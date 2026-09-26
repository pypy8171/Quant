// 장부 사본 한 판을 내는 비용 — 보유 종목 수를 늘려 가며 잰다.
//  주문 스레드는 주문 한 건을 끝낼 때마다 OrderGate::publish_ledger 를 부른다(EngineOrderThread.cpp 끝줄).
//  그 안은 positions_ 와 reserved_ 를 통째로 훑는 고리가 여러 개라, 종목이 늘면 주문 한 건 값이 같이 는다.
//  09-26 부하시험에서 주문 스레드가 초당 1,000~1,400건에서 평평했는데 재고 있던 pop→반환 구간은 31µs뿐이라
//  주문 하나 몫 약 770µs 가운데 대부분이 재지 않은 자리에 있었다 — 그 자리가 여기인지 보려고 만들었다.
//
//  실측(MSVC /O2, 2026-09-26): 41종목 0.7µs · 300종목 7.1µs · 1,000종목 25.7µs · 2,700종목 88.5µs.
//  보유 종목 하나에 약 33ns로 곧게 는다(문턱 없음). 지금 라이브 41종목에서는 없는 값이지만 전 시장
//  2,700종목에서는 주문 하나 몫의 12%가 된다 — 종목이 늘면 그대로 같이 는다.
#include "ipc/LedgerSnapshot.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

// 41(지금 라이브) · 300(정합성 회차) · 2,700(전 시장 회차)에 그 사이를 채운 값들.
const std::vector<size_t> kSymbolCounts{41, 64, 80, 100, 128, 160, 200, 300, 600, 1000, 2700};
constexpr size_t          kPublishCount = 2000;

double publish_microseconds(size_t symbol_count)
{
    OrderGate               gate;
    const std::string       account = "00000000";
    std::vector<std::string> tickers;
    tickers.reserve(symbol_count);

    for (size_t index = 0; index < symbol_count; ++index)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%06zu", 1000 + index);
        tickers.emplace_back(buffer);
        gate.ledger().seed_position(account, tickers.back(), static_cast<int>(index % 100) + 1, 10000.0);
    }

    ipc::LedgerSnapshot snapshot;

    // 첫 판은 빈 배열을 처음 만지는 값이 섞이므로 재기 전에 한 번 버린다.
    gate.publish_ledger(snapshot);

    const auto start = std::chrono::high_resolution_clock::now();

    for (size_t round = 0; round < kPublishCount; ++round)
    {
        gate.publish_ledger(snapshot);
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - start).count();

    return static_cast<double>(elapsed) / static_cast<double>(kPublishCount) / 1000.0;
}

} // namespace

int main()
{
    std::printf("장부 사본 한 판 (보유 종목 수별, %zu판 평균)\n", kPublishCount);
    std::printf("%10s %14s %18s\n", "종목", "한 판(µs)", "이 값이 천장이면(건/초)");

    for (const size_t symbol_count : kSymbolCounts)
    {
        const double microseconds = publish_microseconds(symbol_count);
        std::printf("%10zu %14.1f %18.0f\n", symbol_count, microseconds,
                    microseconds > 0.0 ? 1'000'000.0 / microseconds : 0.0);
    }

    return 0;
}
