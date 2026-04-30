#pragma once
#include <atomic>
#include <vector>
#include <optional>
#include <cstddef>

// ─────────────────────────────────────────────────────────────────────────────
// RingBuffer<T>  —  SPSC Lock-Free Ring Buffer
//   - Single Producer / Single Consumer 구조에서 mutex 없이 동작
//   - Windows/Linux 공통 사용 가능 (std::atomic 표준)
//   - 퀀트 엔진: 시세 수신 스레드(Producer) → 전략 처리 스레드(Consumer)
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity + 1)   // 슬롯 1개는 full/empty 구분용
        , buffer_(capacity + 1)
        , head_(0)
        , tail_(0)
    {}

    // 생산자 스레드에서 호출
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % capacity_;

        if (next == tail_.load(std::memory_order_acquire))
            return false;   // 버퍼 가득 참

        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % capacity_;

        if (next == tail_.load(std::memory_order_acquire))
            return false;

        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // 소비자 스레드에서 호출
    std::optional<T> pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;    // 버퍼 비어 있음

        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) % capacity_, std::memory_order_release);
        return item;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (capacity_ - tail + head);
    }

    size_t capacity() const { return capacity_ - 1; }

private:
    const size_t        capacity_;
    std::vector<T>      buffer_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
};
