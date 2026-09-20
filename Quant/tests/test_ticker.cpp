// 티커 조회 방식별 비용 벤치 — 문자열 키 맵과 정수 id 배열 인덱스, 그리고 실제 입구인 SymbolTable::intern을
//  같은 입력으로 잰다. 원칙 6(hot path에 문자열 없음)의 근거 수치이고, 원칙 7(고치기 전에 잰다)의 도구다. [why D-071]
//  결과 검산: 모든 케이스가 같은 체크섬을 찍어야 한다. 체크섬을 출력하지 않으면 컴파일러가 배열 인덱스 루프를
//  통째로 지워 0 ns가 나온다(이 파일의 첫 판이 그렇게 "100~200배"를 적었다).
//
//  실측 2026-09-20, MSVC /O2, 2,700종목, 1천만 회, 무작위 순서:
//    [1] std::map<string>::find                  ~ 80 ns
//    [2] unordered_map<string>::find             ~ 12 ns
//    [3] SymbolTable::intern (shared_lock+sv해시) ~ 28 ns   (공유 락이 [2]의 두 배를 만든다)
//    [4] vector[symbol_id]                       ~ 0.3 ns
//    [5] 6자리 파싱 + 희소 배열[1e6]              ~ 3 ns   (알파벳 코드를 못 담아 채택하지 않음)
//    [6] intern 1회 + 배열 조회 4곳               ~ 20 ns/틱
//    [7] 문자열 맵 조회 4곳                       ~ 38 ns/틱
//  문자열 맵 하나가 12 ns라 틱당 몇 번을 없애도 절약은 수십 ns다 — 바꿀 가치가 있는 자리는 "틱마다" 도는 조회뿐이다.
#include "core/SymbolTable.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

constexpr size_t kTickerCount = 2700;       // 국내 상장 종목 수 수준
constexpr size_t kIterations  = 10'000'000; // 무작위 순서 조회 횟수

struct MarketData
{
    uint32_t price  = 0;
    uint32_t volume = 0;
};

using Clock = std::chrono::high_resolution_clock;

double nanoseconds_per_lookup(Clock::time_point start)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    return static_cast<double>(elapsed) / static_cast<double>(kIterations);
}

void report(const char* label, double nanoseconds, uint64_t checksum)
{
    std::printf("%-44s: %7.1f ns  (checksum %llu)\n", label, nanoseconds, static_cast<unsigned long long>(checksum));
}

// 6자리 코드를 흩어 놓는다 — 연속 번호면 해시 충돌·캐시가 실제보다 유리하게 나온다.
std::vector<std::string> make_tickers()
{
    std::vector<std::string> tickers;
    tickers.reserve(kTickerCount);

    for (size_t index = 0; index < kTickerCount; ++index)
    {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%06zu", index * 37 % 1'000'000);
        tickers.emplace_back(buffer);
    }

    return tickers;
}

} // namespace

int main()
{
    const auto tickers = make_tickers();

    std::vector<std::string> input_string;  // 수신 문자열 그대로
    std::vector<uint32_t>    input_id;      // 이미 id가 찍힌 경우
    std::vector<uint32_t>    input_numeric; // 이미 숫자로 파싱된 경우
    input_string.reserve(kIterations);
    input_id.reserve(kIterations);
    input_numeric.reserve(kIterations);

    std::mt19937                          random_engine(42);
    std::uniform_int_distribution<size_t> pick(0, kTickerCount - 1);

    for (size_t iteration = 0; iteration < kIterations; ++iteration)
    {
        const size_t index = pick(random_engine);
        input_string.push_back(tickers[index]);
        input_id.push_back(static_cast<uint32_t>(index));
        input_numeric.push_back(static_cast<uint32_t>(std::stoul(tickers[index])));
    }

    std::printf("=== ticker lookup: %zu tickers, %zu lookups ===\n", kTickerCount, kIterations);

    // [1] 정렬 맵 — 문자열 비교 log2(2700)≈12회
    {
        std::map<std::string, MarketData> table;

        for (size_t index = 0; index < kTickerCount; ++index)
        {
            table[tickers[index]] = {static_cast<uint32_t>(index), 1};
        }

        uint64_t   checksum = 0;
        const auto start    = Clock::now();

        for (size_t iteration = 0; iteration < kIterations; ++iteration)
        {
            checksum += table.find(input_string[iteration])->second.price;
        }

        report("[1] std::map<string>::find", nanoseconds_per_lookup(start), checksum);
    }

    // [2] 해시 맵 — 지금 콜드 패스(OrderGate·PaperExecutor 등)가 쓰는 방식
    {
        std::unordered_map<std::string, MarketData> table;

        for (size_t index = 0; index < kTickerCount; ++index)
        {
            table[tickers[index]] = {static_cast<uint32_t>(index), 1};
        }

        uint64_t   checksum = 0;
        const auto start    = Clock::now();

        for (size_t iteration = 0; iteration < kIterations; ++iteration)
        {
            checksum += table.find(input_string[iteration])->second.price;
        }

        report("[2] unordered_map<string>::find", nanoseconds_per_lookup(start), checksum);
    }

    // [3] 실제 입구 — 수신 스레드가 틱마다 한 번 부르는 SymbolTable::intern(공유 락 + string_view 해시)
    {
        symbol::SymbolTable     table;
        std::vector<MarketData> by_id(kTickerCount + 1);

        for (size_t index = 0; index < kTickerCount; ++index)
        {
            by_id[table.intern(tickers[index])] = {static_cast<uint32_t>(index), 1};
        }

        uint64_t   checksum = 0;
        const auto start    = Clock::now();

        for (size_t iteration = 0; iteration < kIterations; ++iteration)
        {
            checksum += by_id[table.intern(input_string[iteration])].price;
        }

        report("[3] SymbolTable::intern + vector[id]", nanoseconds_per_lookup(start), checksum);
    }

    // [4] 정수 id 배열 인덱스 — 샤드·전략·봉 집계가 쓰는 방식
    {
        std::vector<MarketData> by_id(kTickerCount);

        for (size_t index = 0; index < kTickerCount; ++index)
        {
            by_id[index] = {static_cast<uint32_t>(index), 1};
        }

        uint64_t   checksum = 0;
        const auto start    = Clock::now();

        for (size_t iteration = 0; iteration < kIterations; ++iteration)
        {
            checksum += by_id[input_id[iteration]].price;
        }

        report("[4] vector[symbol_id]", nanoseconds_per_lookup(start), checksum);
    }

    // [5] 문자열을 숫자로 파싱해 100만 칸 희소 배열 — 빠르지만 알파벳 섞인 코드를 못 담아 갈래가 둘이 된다
    {
        std::vector<MarketData> sparse(1'000'000);

        for (size_t index = 0; index < kTickerCount; ++index)
        {
            sparse[std::stoul(tickers[index])] = {static_cast<uint32_t>(index), 1};
        }

        uint64_t   checksum = 0;
        const auto start    = Clock::now();

        for (size_t iteration = 0; iteration < kIterations; ++iteration)
        {
            uint32_t value = 0;

            for (const char character : input_string[iteration])
            {
                value = value * 10 + static_cast<uint32_t>(character - '0');
            }

            checksum += sparse[value].price;
        }

        report("[5] parse 6 digits + sparse[1e6]", nanoseconds_per_lookup(start), checksum);
    }

    // [6] 틱 하나의 실제 경로 모델 — 입구에서 intern 한 번, 소비자 4곳은 id 배열
    {
        symbol::SymbolTable     table;
        std::vector<MarketData> consumer_a(kTickerCount + 1), consumer_b(kTickerCount + 1), consumer_c(kTickerCount + 1),
            consumer_d(kTickerCount + 1);

        for (size_t index = 0; index < kTickerCount; ++index)
        {
            const auto id = table.intern(tickers[index]);
            consumer_a[id] = consumer_b[id] = consumer_c[id] = consumer_d[id] = {static_cast<uint32_t>(index), 1};
        }

        uint64_t   checksum = 0;
        const auto start    = Clock::now();

        for (size_t iteration = 0; iteration < kIterations; ++iteration)
        {
            const auto id = table.intern(input_string[iteration]);
            checksum += consumer_a[id].price + consumer_b[id].price + consumer_c[id].price + consumer_d[id].price;
        }

        report("[6] intern once + 4 consumers by id (per tick)", nanoseconds_per_lookup(start), checksum);
    }

    // [7] 비교 — 소비자 4곳이 각자 문자열 맵을 뒤지는 경우
    {
        std::unordered_map<std::string, MarketData> consumer_a, consumer_b, consumer_c, consumer_d;

        for (size_t index = 0; index < kTickerCount; ++index)
        {
            consumer_a[tickers[index]] = consumer_b[tickers[index]] = consumer_c[tickers[index]] =
                consumer_d[tickers[index]] = {static_cast<uint32_t>(index), 1};
        }

        uint64_t   checksum = 0;
        const auto start    = Clock::now();

        for (size_t iteration = 0; iteration < kIterations; ++iteration)
        {
            const std::string& text = input_string[iteration];
            checksum += consumer_a.find(text)->second.price + consumer_b.find(text)->second.price +
                        consumer_c.find(text)->second.price + consumer_d.find(text)->second.price;
        }

        report("[7] 4 consumers by string map (per tick)", nanoseconds_per_lookup(start), checksum);
    }

    return 0;
}
