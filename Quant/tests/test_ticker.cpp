#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

//================================================================
// 테스트 결과 (티커를 string으로 저장하고 std::map, std::unordered_map, Flat Array에 데이터 넣은 뒤 조회 성능 비교)
// 
// DEBUG
//[1] std::map<string, Data>        : 73580 ms
//[2] std::unordered_map<string, D> : 9521 ms
//[3] Flat Array (Security ID)      : 444 ms
// 
// RELEASE
// [1] std::map<string, Data>           : 8104 ms (개당 약 8104 ns)
// [2] std::unordered_map<string, D>    : 1290 ms(개당 약 1290 ns)
// [3] Flat Array(Security ID)          : 0 ms(개당 약 0 ns)
//================================================================


// 1. 테스트 스펙 설정
constexpr size_t TICKER_COUNT = 2700;    // 국내 상장 종목 수 수준
constexpr size_t ITERATIONS = 100'000'0000; // 1억 번의 실시간 시세 조회 들이침 수

// 가상의 호가창/종목 데이터 구조체
struct MarketData
{
    uint32_t price = 0;
    uint32_t volume = 0;
};

// 6자리 KIS 스타일 가상 티커 문자열 생성 ("000000" ~ "002699")
std::vector<std::string> generate_tickers()
{
    std::vector<std::string> tickers;
    tickers.reserve(TICKER_COUNT);

    for (size_t ticker_index = 0; ticker_index < TICKER_COUNT; ++ticker_index)
    {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "%06zu", ticker_index);
        tickers.emplace_back(buffer);
    }

    return tickers;
}

int main()
{
    auto tickers = generate_tickers();

    // 장중에 랜덤하게 들이치는 100만 개의 무작위 웹소켓 입력 시뮬레이션 데이터 준비
    std::vector<std::string> input_stream_str;
    std::vector<uint32_t> input_stream_numeric; // 정수형 버전 (예: 5930)
    std::vector<uint32_t> input_stream_id;      // 순차 방 번호 ID 버전 (0~2699)

    input_stream_str.reserve(ITERATIONS);
    input_stream_numeric.reserve(ITERATIONS);
    input_stream_id.reserve(ITERATIONS);

    std::mt19937 rng(42); // 시드 고정
    std::uniform_int_distribution<size_t> dist(0, TICKER_COUNT - 1);

    for (size_t iteration_index = 0; iteration_index < ITERATIONS; ++iteration_index)
    {
        size_t index = dist(rng);
        input_stream_str.push_back(tickers[index]);
        input_stream_numeric.push_back(std::stoul(tickers[index]));
        input_stream_id.push_back(static_cast<uint32_t>(index));
    }

    std::cout << "=== HFT Ticker Lookup Benchmark (" << ITERATIONS << " lookups) ===\n\n";

    // -------------------------------------------------------------------------
    // TEST 1: std::map<std::string, MarketData>
    // -------------------------------------------------------------------------
    {
        std::map<std::string, MarketData> ticker_map;

        for (const auto& ticker : tickers)
        {
            ticker_map[ticker] = MarketData{1000, 100};
        }

        auto start = std::chrono::high_resolution_clock::now();
        uint64_t checksum = 0;

        for (size_t iteration_index = 0; iteration_index < ITERATIONS; ++iteration_index)
        {
            // Hot-Path: 문자열로 트리 탐색 후 데이터 접근
            const auto& data = ticker_map[input_stream_str[iteration_index]];
            checksum += data.price;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "[1] std::map<string, Data>        : " << duration << " ms (개당 약 "
                  << static_cast<double>(duration * 100'000'000) / ITERATIONS << " ns)\n";
    }

    // -------------------------------------------------------------------------
    // TEST 2: std::unordered_map<std::string, MarketData>
    // -------------------------------------------------------------------------
    {
        std::unordered_map<std::string, MarketData> ticker_unmap;

        for (const auto& ticker : tickers)
        {
            ticker_unmap[ticker] = MarketData{1000, 100};
        }

        auto start = std::chrono::high_resolution_clock::now();
        uint64_t checksum = 0;

        for (size_t iteration_index = 0; iteration_index < ITERATIONS; ++iteration_index)
        {
            // Hot-Path: 문자열 해시 연산 후 버킷 탐색
            const auto& data = ticker_unmap[input_stream_str[iteration_index]];
            checksum += data.price;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "[2] std::unordered_map<string, D> : " << duration << " ms (개당 약 "
                  << static_cast<double>(duration * 100'000'000) / ITERATIONS << " ns)\n";
    }

    // -------------------------------------------------------------------------
    // TEST 3: 정수형 Flat Array Indexing (방 번호 ID 매핑) - 최적화 구조
    // -------------------------------------------------------------------------
    {
        // 2700개 종목에 대한 연속된 고정 메모리 물리 배열 선언
        std::array<MarketData, TICKER_COUNT> flat_array;
        flat_array.fill(MarketData{1000, 100});

        auto start = std::chrono::high_resolution_clock::now();
        uint64_t checksum = 0;

        for (size_t iteration_index = 0; iteration_index < ITERATIONS; ++iteration_index)
        {
            // Hot-Path: 문자열 배제, 정수 ID로 다이렉트 주소 이동 (O(1))
            const auto& data = flat_array[input_stream_id[iteration_index]];
            checksum += data.price;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "[3] Flat Array (Security ID)      : " << duration << " ms (개당 약 "
                  << static_cast<double>(duration * 100'000'000) / ITERATIONS << " ns)\n";
    }

    // -------------------------------------------------------------------------
    // TEST 4: 정수형 Vector Indexing (방 번호 ID 매핑) 
    // -------------------------------------------------------------------------
    {
        // 2700개 종목에 대한 연속된 고정 메모리 물리 배열 선언
        std::vector<MarketData> vec;
        vec.resize(TICKER_COUNT);
        std::fill(vec.begin(), vec.end(), MarketData{1000, 100});

        auto start = std::chrono::high_resolution_clock::now();
        uint64_t checksum = 0;

        for (size_t iteration_index = 0; iteration_index < ITERATIONS; ++iteration_index)
        {
            // Hot-Path: 문자열 배제, 정수 ID로 다이렉트 주소 이동 (O(1))
            const auto& data = vec[input_stream_id[iteration_index]];
            checksum += data.price;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "[4] vector (Security ID)      : " << duration << " ms (개당 약 "
                  << static_cast<double>(duration * 100'000'000) / ITERATIONS << " ns)\n";
    }

    return 0;
}
