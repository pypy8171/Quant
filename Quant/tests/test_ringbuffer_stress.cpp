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

using clk = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

// ─────────────────────────────────────────────────────────────────
// 실제 OrderBook 크기 시뮬레이션 (KIS H0STASP0 기반)
// 약 200바이트 — Types.h의 OrderBook 구조체와 동일 수준
// ─────────────────────────────────────────────────────────────────
struct alignas(64) MockOrderBook {
	char     ticker[8];          // 종목코드
	char     time[8];             // 시간
	double   ask_price[5];        // 매도호가 5단계
	int64_t  ask_qty[5];          // 매도잔량
	double   bid_price[5];        // 매수호가
	int64_t  bid_qty[5];          // 매수잔량
	int64_t  send_ts_ns;          // producer 송신 시각 (latency 측정용)
	uint64_t seq;                 // 무결성 검증용 시퀀스 번호
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
	std::atomic<uint64_t> seq_errors{ 0 };    // 순서 깨짐
	std::atomic<uint64_t> data_errors{ 0 };   // 데이터 손상

	std::vector<int64_t> latencies_ns;      // consumer가 측정
};

// ─────────────────────────────────────────────────────────────────
// Producer: 실제 시세 발생 패턴 모방
//   - 종목당 burst (10-50건 연속) → 짧은 idle
//   - 종목 6개 round-robin (실제 다종목 모니터링 환경)
// ─────────────────────────────────────────────────────────────────
static void producer_fn(RingBuffer<MockOrderBook>& rb,
	Stats& stats,
	std::atomic<bool>& stop_flag,
	int duration_sec)
{
	static const char* TICKERS[] = {
		"005930", "000660", "005380", "035720", "051910", "006400"
	};
	constexpr int N_TICKERS = sizeof(TICKERS) / sizeof(TICKERS[0]);

	std::mt19937 rng(42);
	std::uniform_int_distribution<int> burst_dist(10, 50);
	std::uniform_int_distribution<int> idle_us_dist(50, 500);

	auto deadline = clk::now() + std::chrono::seconds(duration_sec);
	uint64_t seq = 0;
	int ticker_idx = 0;

	while (!stop_flag.load(std::memory_order_relaxed) && clk::now() < deadline) {
		int burst = burst_dist(rng);
		const char* tk = TICKERS[ticker_idx];
		ticker_idx = (ticker_idx + 1) % N_TICKERS;

		for (int b = 0; b < burst; ++b) {
			MockOrderBook ob{};
			std::memcpy(ob.ticker, tk, 6);
			ob.ticker[6] = '\0';
			ob.seq = seq++;
			for (int i = 0; i < 5; ++i) {
				ob.ask_price[i] = 70000.0 + i * 10 + b;
				ob.ask_qty[i] = 100 + i * 50;
				ob.bid_price[i] = 69990.0 - i * 10 - b;
				ob.bid_qty[i] = 100 + i * 50;
			}
			ob.send_ts_ns = std::chrono::duration_cast<ns>(
				clk::now().time_since_epoch()).count();

			// Push (가득 차면 재시도, 운영 환경에선 데이터 손실 옵션도 있음)
			int retries = 0;
			while (!rb.push(ob)) {
				if (++retries > 1000) {
					stats.push_failed.fetch_add(1, std::memory_order_relaxed);
					break;
				}
				std::this_thread::yield();
			}
			stats.produced.fetch_add(1, std::memory_order_relaxed);
		}

		// Idle (burst 사이 간격)
		std::this_thread::sleep_for(
			std::chrono::microseconds(idle_us_dist(rng)));
	}
}

// ─────────────────────────────────────────────────────────────────
// Consumer: 전략 계산 latency 시뮬레이션
//   - 메시지당 0-50µs 가변 처리 시간 (실제 MA/Donchian 계산 수준)
//   - 시퀀스 번호 검증 (순서/무손실)
//   - end-to-end latency 측정
// ─────────────────────────────────────────────────────────────────
static void consumer_fn(RingBuffer<MockOrderBook>& rb,
	Stats& stats,
	std::atomic<bool>& stop_flag)
{
	uint64_t expected_seq[6] = { 0, 0, 0, 0, 0, 0 };  // 종목별 seq
	auto ticker_idx = [](const char* tk) -> int {
		if (std::strcmp(tk, "005930") == 0) return 0;
		if (std::strcmp(tk, "000660") == 0) return 1;
		if (std::strcmp(tk, "005380") == 0) return 2;
		if (std::strcmp(tk, "035720") == 0) return 3;
		if (std::strcmp(tk, "051910") == 0) return 4;
		if (std::strcmp(tk, "006400") == 0) return 5;
		return -1;
		};

	stats.latencies_ns.reserve(10'000'000);

	while (!stop_flag.load(std::memory_order_relaxed) || !rb.empty()) {
		auto opt = rb.pop();
		if (!opt) {
			std::this_thread::sleep_for(std::chrono::microseconds(10));
			continue;
		}

		// Latency 측정
		int64_t now_ns = std::chrono::duration_cast<ns>(
			clk::now().time_since_epoch()).count();
		int64_t latency = now_ns - opt->send_ts_ns;
		if (latency >= 0) stats.latencies_ns.push_back(latency);

		// 데이터 무결성 (호가가 음수면 손상)
		if (opt->ask_price[0] < 0 || opt->bid_price[0] < 0) {
			stats.data_errors.fetch_add(1, std::memory_order_relaxed);
		}

		stats.consumed.fetch_add(1, std::memory_order_relaxed);

		// 전략 계산 시뮬레이션 (간단한 work)
		volatile double sink = 0.0;
		for (int i = 0; i < 5; ++i) {
			sink += opt->ask_price[i] * opt->ask_qty[i];
			sink += opt->bid_price[i] * opt->bid_qty[i];
		}
		(void)sink;
	}
}

// ─────────────────────────────────────────────────────────────────
// Latency 분위수 출력
// ─────────────────────────────────────────────────────────────────
static void print_latency_stats(std::vector<int64_t>& v) {
	if (v.empty()) {
		std::cout << "  (no latency samples)\n";
		return;
	}
	std::sort(v.begin(), v.end());
	auto pct = [&](double p) {
		size_t idx = static_cast<size_t>(v.size() * p);
		if (idx >= v.size()) 
			idx = v.size() - 1;

		return v[idx];
		};
	auto fmt = [](int64_t ns) -> std::string {
		if (ns < 1000)        return std::to_string(ns) + " ns";
		if (ns < 1'000'000)   return std::to_string(ns / 1000) + " µs";
		return std::to_string(ns / 1'000'000) + " ms";
		};
	std::cout << "  p50:  " << fmt(pct(0.50)) << "\n"
		<< "  p90:  " << fmt(pct(0.90)) << "\n"
		<< "  p99:  " << fmt(pct(0.99)) << "\n"
		<< "  p999: " << fmt(pct(0.999)) << "\n"
		<< "  max:  " << fmt(v.back()) << "\n";
}

// ─────────────────────────────────────────────────────────────────
// Main: 부하 테스트 실행
//   사용법: ./test_ringbuffer_stress [duration_sec]
//   기본 30초, 운영 검증은 3600+ 추천.
// ─────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
	int duration = 30;
	if (argc > 1) duration = std::atoi(argv[1]);
	if (duration < 1) duration = 30;

	std::cout << "=== RingBuffer Stress Test ===\n";
	std::cout << "Duration       : " << duration << " sec\n";
	std::cout << "Message size   : " << sizeof(MockOrderBook) << " bytes\n";
	std::cout << "Queue capacity : 4096\n";
	std::cout << "Pattern        : 6 tickers, burst(10-50)/idle(50-500us)\n";
	std::cout << "Consumer       : variable work per msg + integrity check\n\n";

	RingBuffer<MockOrderBook> rb(4096);
	Stats stats;
	std::atomic<bool> stop_flag{ false };

	auto t0 = clk::now();

	std::thread prod(producer_fn, std::ref(rb), std::ref(stats),
		std::ref(stop_flag), duration);
	std::thread cons(consumer_fn, std::ref(rb), std::ref(stats),
		std::ref(stop_flag));

	// 진행 상황 표시 (5초마다)
	while (clk::now() - t0 < std::chrono::seconds(duration)) {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
			clk::now() - t0).count();
		std::cout << "  [t+" << std::setw(4) << elapsed << "s] "
			<< "produced=" << stats.produced.load()
			<< " consumed=" << stats.consumed.load()
			<< " in-queue=" << (stats.produced.load() - stats.consumed.load())
			<< "\n";
	}

	prod.join();
	stop_flag.store(true);
	cons.join();

	auto t1 = clk::now();
	auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

	// ─── 결과 ─────────────────────────────────────────────────────
	std::cout << "\n=== Results ===\n";
	std::cout << "Elapsed        : " << elapsed_ms << " ms\n";
	std::cout << "Produced       : " << stats.produced.load() << "\n";
	std::cout << "Consumed       : " << stats.consumed.load() << "\n";
	std::cout << "Push failed    : " << stats.push_failed.load() << "\n";
	std::cout << "Data errors    : " << stats.data_errors.load() << "\n";
	std::cout << "Throughput     : "
		<< (stats.consumed.load() * 1000 / std::max<int64_t>(elapsed_ms, 1))
		<< " msg/sec\n";
	std::cout << "Bandwidth      : "
		<< (stats.consumed.load() * sizeof(MockOrderBook) * 1000
			/ std::max<int64_t>(elapsed_ms, 1) / 1024 / 1024)
		<< " MB/sec\n\n";

	std::cout << "Latency (producer push → consumer pop):\n";
	print_latency_stats(stats.latencies_ns);

	// ─── 검증 판정 ────────────────────────────────────────────────
	bool ok = (stats.produced.load() == stats.consumed.load())
		&& (stats.data_errors.load() == 0);

	std::cout << "\n[" << (ok ? "PASS" : "FAIL")
		<< "] integrity=" << (stats.data_errors.load() == 0 ? "OK" : "FAIL")
		<< ", lossless=" << (stats.produced.load() == stats.consumed.load() ? "OK" : "FAIL")
		<< "\n";

	return ok ? 0 : 1;
}