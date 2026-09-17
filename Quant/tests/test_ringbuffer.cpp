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
	RingBuffer<int> odd_queue(1000);
	assert(odd_queue.capacity() == 1024);
	RingBuffer<int> large_queue(1024);
	assert(large_queue.capacity() == 1024);
	RingBuffer<int> single_queue(1);
	assert(single_queue.capacity() == 1);
	assert(single_queue.push(7));
	assert(!single_queue.push(8));
	assert(single_queue.size() == 1);
	auto popped_v = single_queue.pop();
	assert(popped_v && *popped_v == 7);
	assert(!single_queue.pop());
	assert(single_queue.empty());

	RingBuffer<int> small_queue(4);

	for (int index = 0; index < 4; ++index) {
		assert(small_queue.push(index));
	}

	assert(!small_queue.push(99));
	assert(small_queue.size() == 4);

	// 한 바퀴 넘겨 마스크 경계를 지난 뒤에도 순서가 유지되는지
	for (int round = 0; round < 3; ++round) {
		for (int index = 0; index < 4; ++index) {
			auto popped_x = small_queue.pop();
			assert(popped_x && *popped_x == round * 4 + index);
			assert(small_queue.push(round * 4 + index + 4));
		}
	}

	std::cout << "[PASS] pow2 capacity / boundary\n";
}

// 고수위는 가득 찼던 4에서 더 오르지 않고, 비운 뒤에도 내려가지 않는다(기동 뒤 최댓값).
static void test_high_water() {
	RingBuffer<int> queue(4);
	assert(queue.high_water() == 0);
	assert(queue.push(1));
	assert(queue.high_water() == 1);
	assert(queue.pop());
	assert(queue.push(2));
	assert(queue.high_water() == 1);

	for (int index = 0; index < 3; ++index) {
		assert(queue.push(index));
	}

	assert(queue.high_water() == 4);

	while (queue.pop()) {
	}

	assert(queue.high_water() == 4);
	std::cout << "[PASS] high water\n";
}

static void test_spsc_correctness() {
	RingBuffer<int> rb(1024);
	constexpr int kItemCount = 1'000'000;

	std::thread prod([&] {
		for (int index = 0; index < kItemCount; ++index)
		{
			while (!rb.push(index))
			{
				std::this_thread::yield();
			}
		}
		});

	std::thread cons([&] {
		int expected = 0;

		while (expected < kItemCount) {
			auto popped = rb.pop();

			if (!popped)
			{
				std::this_thread::yield();
				continue;
			}

			assert(*popped == expected);
			++expected;
		}
		});

	prod.join();
	cons.join();
	std::cout << "[OK] SPSC ordering preserved over " << kItemCount << " items\n";
}

static void test_throughput() {
	RingBuffer<int> rb(4096);
	constexpr int kItemCount = 10'000'000;

	auto start_time = std::chrono::steady_clock::now();

	std::thread prod([&] {
		for (int index = 0; index < kItemCount; ++index)
		{
			while (!rb.push(index))
			{
				std::this_thread::yield();
			}
		}
		});

	std::thread cons([&] {
		int got = 0;

		while (got < kItemCount)
		{
			auto popped = rb.pop();

			if (!popped)
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
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - start_time).count();
	double mops = (ms > 0) ? static_cast<double>(kItemCount) / ms / 1000.0 : 0.0;

	std::cout << "[OK] Throughput: " << kItemCount << " items in "
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

