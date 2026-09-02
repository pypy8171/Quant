// tests/test_ringbuffer.cpp
// 단일생산자·단일소비자(SPSC) RingBuffer 정확성 + 처리량 검증

#include "core/RingBuffer.h"
#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>

static void test_spsc_correctness() {
	RingBuffer<int> rb(1024);
	constexpr int N = 1'000'000;

	std::thread prod([&] {
		for (int i = 0; i < N; ++i)
		{
			while (!rb.push(i))
				std::this_thread::yield();
		}
		});

	std::thread cons([&] {
		int expected = 0;
		while (expected < N) {
			auto v = rb.pop();
			if (!v)
			{
				std::this_thread::yield();
				continue;
			}
			assert(*v == expected);
			++expected;
		}
		});

	prod.join();
	cons.join();
	std::cout << "[OK] SPSC ordering preserved over " << N << " items\n";
}

static void test_throughput() {
	RingBuffer<int> rb(4096);
	constexpr int N = 10'000'000;

	auto t0 = std::chrono::steady_clock::now();

	std::thread prod([&] {
		for (int i = 0; i < N; ++i)
		{
			while (!rb.push(i))
				std::this_thread::yield();
		}
		});

	std::thread cons([&] {
		int got = 0;
		while (got < N)
		{
			auto v = rb.pop();
			if (!v)
			{
				std::this_thread::yield();
				continue;
			}
			++got;
		}
		});

	prod.join();
	cons.join();

	auto t1 = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	double mops = (ms > 0) ? (double)N / ms / 1000.0 : 0.0;

	std::cout << "[OK] Throughput: " << N << " items in "
		<< ms << " ms (" << mops << " M ops/sec)\n";
}

int main() {
	std::cout << "=== RingBuffer Tests ===\n";
	test_spsc_correctness();
	test_throughput();
	std::cout << "All tests passed.\n";
	return 0;
}

