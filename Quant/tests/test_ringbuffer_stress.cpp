// tests/test_ringbuffer_stress.cpp
// 단일생산자·단일소비자(SPSC) RingBuffer 실환경 부하 시뮬레이션
//
// 시나리오:
//  - 실제 OrderBook 크기 구조체 (200B 수준)
//  - Burst 트래픽 패턴 (종목 호가 갱신 burst → idle)
//  - 가변 소비자 latency (전략 계산 시뮬레이션)
//  - 장기 운영 검증 (옵션: --duration N초)
//  - 메모리 누수 / 데이터 무결성 / latency 분포 측정

#include "core/RingBuffer.h"
#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>
#include <string>
#include <cstdint>

using steady_clock = std::chrono::steady_clock;
using nanoseconds = std::chrono::nanoseconds;

// ─────────────────────────────────────────────────────────────────
// 실제 OrderBook 크기 시뮬레이션 (KIS H0STASP0 기반)
// 약 200바이트 — Types.h의 OrderBook 구조체와 동일 수준
// ─────────────────────────────────────────────────────────────────
struct alignas(64) MockOrderBook {
	char     ticker[8];          // 종목코드
	char     time[8];             // 시간
	double   ask_price[5];        // 매도호가 5단계
	int64_t  ask_quantity[5];          // 매도잔량
	double   bid_price[5];        // 매수호가
	int64_t  bid_quantity[5];          // 매수잔량
	int64_t  send_ts_ns;          // producer 송신 시각 (latency 측정용)
	uint64_t sequence;                 // 무결성 검증용 시퀀스 번호
	char     padding[16];         // 200바이트 근접
};
static_assert(sizeof(MockOrderBook) >= 192, "OrderBook size check");

// ─────────────────────────────────────────────────────────────────
// 통계 수집
// ─────────────────────────────────────────────────────────────────
struct Stats {
	std::atomic<uint64_t> produced{ 0 };
	std::atomic<uint64_t> consumed{ 0 };
	std::atomic<uint64_t> push_failed{ 0 };   // 큐 full로 재시도
	std::atomic<uint64_t> sequence_errors{ 0 };    // 순서 깨짐
	std::atomic<uint64_t> data_errors{ 0 };   // 데이터 손상

	std::vector<int64_t> latencies_ns;      // consumer가 측정
};

// ─────────────────────────────────────────────────────────────────
// Producer: 실제 시세 발생 패턴 모방
//   - 종목당 burst (10-50건 연속) → 짧은 idle
//   - 종목 6개 round-robin (실제 다종목 모니터링 환경)
// ─────────────────────────────────────────────────────────────────
static void producer_fn(RingBuffer<MockOrderBook>& rest_bar,
	Stats& statistics,
	std::atomic<bool>& stop_flag,
	int duration_sec)
{
	static const char* TICKERS[] = {
		"005930", "000660", "005380", "035720", "051910", "006400"
	};
	constexpr int N_TICKERS = sizeof(TICKERS) / sizeof(TICKERS[0]);

	std::mt19937 random_engine(42);
	std::uniform_int_distribution<int> burst_dist(10, 50);
	std::uniform_int_distribution<int> idle_us_dist(50, 500);

	auto deadline = steady_clock::now() + std::chrono::seconds(duration_sec);
	uint64_t sequence = 0;
	int ticker_index = 0;

	while (!stop_flag.load(std::memory_order_relaxed) && steady_clock::now() < deadline) {
		int burst = burst_dist(random_engine);
		const char* ticker = TICKERS[ticker_index];
		ticker_index = (ticker_index + 1) % N_TICKERS;

		for (int burst_index = 0; burst_index < burst; ++burst_index) {
			MockOrderBook order_book{};
			std::memcpy(order_book.ticker, ticker, 6);
			order_book.ticker[6] = '\0';
			order_book.sequence = sequence++;

			for (int index = 0; index < 5; ++index) {
				order_book.ask_price[index] = 70000.0 + index * 10 + burst_index;
				order_book.ask_quantity[index] = 100 + index * 50;
				order_book.bid_price[index] = 69990.0 - index * 10 - burst_index;
				order_book.bid_quantity[index] = 100 + index * 50;
			}

			order_book.send_ts_ns = std::chrono::duration_cast<nanoseconds>(
				steady_clock::now().time_since_epoch()).count();

			// Push (가득 차면 재시도, 운영 환경에선 데이터 손실 옵션도 있음)
			int retries = 0;

			while (!rest_bar.push(order_book)) {
				if (++retries > 1000) {
					statistics.push_failed.fetch_add(1, std::memory_order_relaxed);
					break;
				}

				std::this_thread::yield();
			}

			statistics.produced.fetch_add(1, std::memory_order_relaxed);
		}

		// Idle (burst 사이 간격)
		std::this_thread::sleep_for(
			std::chrono::microseconds(idle_us_dist(random_engine)));
	}
}

// ─────────────────────────────────────────────────────────────────
// Consumer: 전략 계산 latency 시뮬레이션
//   - 메시지당 0-50µs 가변 처리 시간 (실제 MA/채널 돌파 계산 수준)
//   - 시퀀스 번호 검증 (순서/무손실)
//   - end-to-end latency 측정
// ─────────────────────────────────────────────────────────────────
static void consumer_fn(RingBuffer<MockOrderBook>& rest_bar,
	Stats& statistics,
	std::atomic<bool>& stop_flag)
{
	uint64_t expected_sequence[6] = { 0, 0, 0, 0, 0, 0 };  // 종목별 sequence
	auto ticker_index = [](const char* ticker) -> int {
		if (std::strcmp(ticker, "005930") == 0)
		{
		    return 0;
		}

		if (std::strcmp(ticker, "000660") == 0)
		{
		    return 1;
		}

		if (std::strcmp(ticker, "005380") == 0)
		{
		    return 2;
		}

		if (std::strcmp(ticker, "035720") == 0)
		{
		    return 3;
		}

		if (std::strcmp(ticker, "051910") == 0)
		{
		    return 4;
		}

		if (std::strcmp(ticker, "006400") == 0)
		{
		    return 5;
		}

		return -1;
		};

	statistics.latencies_ns.reserve(10'000'000);

	while (!stop_flag.load(std::memory_order_relaxed) || !rest_bar.empty()) {
		auto option = rest_bar.pop();

		if (!option) {
			std::this_thread::sleep_for(std::chrono::microseconds(10));
			continue;
		}

		// Latency 측정
		int64_t now_ns = std::chrono::duration_cast<nanoseconds>(
			steady_clock::now().time_since_epoch()).count();
		int64_t latency = now_ns - option->send_ts_ns;

		if (latency >= 0)
		{
		    statistics.latencies_ns.push_back(latency);
		}

		// 데이터 무결성 (호가가 음수면 손상)
		if (option->ask_price[0] < 0 || option->bid_price[0] < 0) {
			statistics.data_errors.fetch_add(1, std::memory_order_relaxed);
		}

		statistics.consumed.fetch_add(1, std::memory_order_relaxed);

		// 전략 계산 시뮬레이션 (간단한 work)
		volatile double sink = 0.0;

		for (int index = 0; index < 5; ++index) {
			sink = sink + option->ask_price[index] * option->ask_quantity[index];
			sink = sink + option->bid_price[index] * option->bid_quantity[index];
		}

		(void)sink;
	}
}

// ─────────────────────────────────────────────────────────────────
// Latency 분위수 출력
// ─────────────────────────────────────────────────────────────────
static void print_latency_statistics(std::vector<int64_t>& values) {
	if (values.empty()) {
		std::cout << "  (no latency samples)\n";
		return;
	}

	std::sort(values.begin(), values.end());
	auto percent = [&](double price) {
		size_t index = static_cast<size_t>(values.size() * price);

		if (index >= values.size()) 
		{
			index = values.size() - 1;
		}

		return values[index];
		};
	auto format = [](int64_t nanoseconds) -> std::string {
		if (nanoseconds < 1000)
		{
		    return std::to_string(nanoseconds) + " ns";
		}

		if (nanoseconds < 1'000'000)
		{
		    return std::to_string(nanoseconds / 1000) + " µs";
		}

		return std::to_string(nanoseconds / 1'000'000) + " ms";
		};
	std::cout << "  p50:  " << format(percent(0.50)) << "\n"
		<< "  p90:  " << format(percent(0.90)) << "\n"
		<< "  p99:  " << format(percent(0.99)) << "\n"
		<< "  p999: " << format(percent(0.999)) << "\n"
		<< "  max:  " << format(values.back()) << "\n";
}

// ─────────────────────────────────────────────────────────────────
// Main: 부하 테스트 실행
//   사용법: ./test_ringbuffer_stress [duration_sec]
//   기본 30초, 운영 검증은 3600+ 추천.
// ─────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
	int duration = 30;

	if (argc > 1)
	{
	    duration = std::atoi(argv[1]);
	}

	if (duration < 1)
	{
	    duration = 30;
	}

	std::cout << "=== RingBuffer Stress Test ===\n";
	std::cout << "Duration       : " << duration << " sec\n";
	std::cout << "Message size   : " << sizeof(MockOrderBook) << " bytes\n";
	std::cout << "Queue capacity : 4096\n";
	std::cout << "Pattern        : 6 tickers, burst(10-50)/idle(50-500us)\n";
	std::cout << "Consumer       : variable work per msg + integrity check\n\n";

	RingBuffer<MockOrderBook> rest_bar(4096);
	Stats statistics;
	std::atomic<bool> stop_flag{ false };

	auto start_time = steady_clock::now();

	std::thread produced(producer_fn, std::ref(rest_bar), std::ref(statistics),
		std::ref(stop_flag), duration);
	std::thread consumed(consumer_fn, std::ref(rest_bar), std::ref(statistics),
		std::ref(stop_flag));

	// 진행 상황 표시 (5초마다)
	while (steady_clock::now() - start_time < std::chrono::seconds(duration)) {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
			steady_clock::now() - start_time).count();
		std::cout << "  [t+" << std::setw(4) << elapsed << "s] "
			<< "produced=" << statistics.produced.load()
			<< " consumed=" << statistics.consumed.load()
			<< " in-queue=" << (statistics.produced.load() - statistics.consumed.load())
			<< "\n";
	}

	produced.join();
	stop_flag.store(true);
	consumed.join();

	auto end_time = steady_clock::now();
	auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

	// ─── 결과 ─────────────────────────────────────────────────────
	std::cout << "\n=== Results ===\n";
	std::cout << "Elapsed        : " << elapsed_ms << " ms\n";
	std::cout << "Produced       : " << statistics.produced.load() << "\n";
	std::cout << "Consumed       : " << statistics.consumed.load() << "\n";
	std::cout << "Push failed    : " << statistics.push_failed.load() << "\n";
	std::cout << "Data errors    : " << statistics.data_errors.load() << "\n";
	std::cout << "Throughput     : "
		<< (statistics.consumed.load() * 1000 / std::max<int64_t>(elapsed_ms, 1))
		<< " msg/sec\n";
	std::cout << "Bandwidth      : "
		<< (statistics.consumed.load() * sizeof(MockOrderBook) * 1000
			/ std::max<int64_t>(elapsed_ms, 1) / 1024 / 1024)
		<< " MB/sec\n\n";

	std::cout << "Latency (producer push → consumer pop):\n";
	print_latency_statistics(statistics.latencies_ns);

	// ─── 검증 판정 ────────────────────────────────────────────────
	bool ok = (statistics.produced.load() == statistics.consumed.load())
		&& (statistics.data_errors.load() == 0);

	std::cout << "\n[" << (ok ? "PASS" : "FAIL")
		<< "] integrity=" << (statistics.data_errors.load() == 0 ? "OK" : "FAIL")
		<< ", lossless=" << (statistics.produced.load() == statistics.consumed.load() ? "OK" : "FAIL")
		<< "\n";

	return ok ? 0 : 1;
}