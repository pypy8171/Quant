// OrderGate 원장 조회 비용 — 키가 (account, ticker) 문자열 두 개일 때와 (account id, symbol id) 정수 두 개일 때.
//  전략(ITB 청산 대기·DevScale 장 마감 블록)이 부르는 position()과, 주문 경로 check()가 매 신호마다 만드는 키가
//  같은 조회다. 25종목 보유 상태에서 무작위 종목을 1천만 번 묻는다. 체크섬을 찍어 컴파일러가 루프를 못 지운다.
//
//  실측(MSVC /O2, 2026-09-20, 두 번 실행): 문자열 두 개 키이던 때 position(account, ticker) 34.3·40.2 ns/조회.
//  정수 키로 바꾼 뒤 position(account, ticker) 28.3·29.0 ns(SymbolTable 조회 + 계좌 벡터 비교, 키 생성의 힙 할당 없음),
//  position(account, symbol_id) 13.4·12.8 ns. 남는 것은 positions_mutex_와 맵 한 번. docs/DECISIONS.md D-105 결정 3.
#include "risk/OrderGate.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace
{

constexpr size_t kHeldCount  = 25;
constexpr size_t kQueryCount = 10'000'000;

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
    OrderGate         gate;
    const std::string account = "ACCOUNT_A";
    std::vector<std::string> tickers;

    for (size_t index = 0; index < kHeldCount; ++index)
    {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%06zu", 5930 + index * 1010);
        tickers.emplace_back(buffer);
        gate.ledger().seed_position(account, tickers.back(), static_cast<int>(index + 1), 10000.0);
    }

    std::mt19937                          random_engine(7);
    std::uniform_int_distribution<size_t> pick(0, kHeldCount - 1);
    std::vector<size_t>                   order(kQueryCount);

    for (auto& index : order)
    {
        index = pick(random_engine);
    }

    long long checksum = 0;

    const double string_ns = measure_ns(kQueryCount, [&] {
        for (const size_t index : order)
        {
            checksum += gate.ledger().position(account, tickers[index]);
        }
    });
    std::printf("position(account, ticker)   : %.1f ns/조회 (checksum %lld)\n", string_ns, checksum);

    std::vector<symbol::SymbolId> ids;

    for (const auto& ticker : tickers)
    {
        ids.push_back(gate.ledger().symbol_id_of(ticker));
    }

    checksum = 0;
    const double id_ns = measure_ns(kQueryCount, [&] {
        for (const size_t index : order)
        {
            checksum += gate.ledger().position(account, ids[index]);
        }
    });
    std::printf("position(account, symbol_id): %.1f ns/조회 (checksum %lld)\n", id_ns, checksum);

    return 0;
}
