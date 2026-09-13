// tests/test_ringbuffer.cpp
// 단일생산자·단일소비자(SPSC) RingBuffer 정확성 + 처리량 검증

#include "core/RingBuffer.h"
#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>

// 2의 거듭제곱 올림과 가득 참·비어 있음 경계. 카운터가 감싸지 않으므로 슬롯 전부를 쓴다(D-042).
static void test_pow2_capacity() {
	RingBuffer<int> a(1000);
	assert(a.capacity() == 1024);
	RingBuffer<int> b(1024);
	assert(b.capacity() == 1024);
	RingBuffer<int> c(1);
	assert(c.capacity() == 1);
	assert(c.push(7));
	assert(!c.push(8));
	assert(c.size() == 1);
	auto v = c.pop();
	assert(v && *v == 7);
	assert(!c.pop());
	assert(c.empty());

	RingBuffer<int> d(4);

	for (int i = 0; i < 4; ++i) {
		assert(d.push(i));
	}

	assert(!d.push(99));
	assert(d.size() == 4);

	// 한 바퀴 넘겨 마스크 경계를 지난 뒤에도 순서가 유지되는지
	for (int round = 0; round < 3; ++round) {
		for (int i = 0; i < 4; ++i) {
			auto x = d.pop();
			assert(x && *x == round * 4 + i);
			assert(d.push(round * 4 + i + 4));
		}
	}

	std::cout << "[PASS] pow2 capacity / boundary\n";
}

// 고수위는 가득 찼던 4에서 더 오르지 않고, 비운 뒤에도 내려가지 않는다(기동 뒤 최댓값).
static void test_high_water() {
	RingBuffer<int> q(4);
	assert(q.high_water() == 0);
	assert(q.push(1));
	assert(q.high_water() == 1);
	assert(q.pop());
	assert(q.push(2));
	assert(q.high_water() == 1);

	for (int i = 0; i < 3; ++i) {
		assert(q.push(i));
	}

	assert(q.high_water() == 4);

	while (q.pop()) {
	}

	assert(q.high_water() == 4);
	std::cout << "[PASS] high water\n";
}

static void test_spsc_correctness() {
	RingBuffer<int> rb(1024);
	constexpr int N = 1'000'000;

	std::thread prod([&] {
		for (int i = 0; i < N; ++i)
		{
			while (!rb.push(i))
			{
				std::this_thread::yield();
			}
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
			{
				std::this_thread::yield();
			}
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
	test_pow2_capacity();
	test_high_water();
	std::cout << "=== RingBuffer Tests ===\n";
	test_spsc_correctness();
	test_throughput();
	std::cout << "All tests passed.\n";
	return 0;
}

