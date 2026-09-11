#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// RingBuffer<T>  —  단일생산자·단일소비자(SPSC) Lock-Free Ring Buffer
//   - mutex 없이 동작, Windows/Linux 공통(std::atomic 표준)
//   - 퀀트 엔진: 시세 수신 스레드(Producer) → 전략 처리 스레드(Consumer)
// ─────────────────────────────────────────────────────────────────────────────

// std::hardware_destructive_interference_size는 C++17이지만
// MSVC 19.26 미만과 일부 GCC에서 미지원 — 64로 fallback
#ifdef __cpp_lib_hardware_interference_size
inline constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr size_t kCacheLine = 64;
#endif

template <typename T> class RingBuffer
{
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity + 1) // 슬롯 1개는 full/empty 구분용
          ,
          buffer_(capacity + 1), head_(0), tail_(0)
    {
    }

    // 원자 인덱스 한 쌍이 한 큐를 뜻한다 — 사본은 다른 큐이면서 같은 이름을 갖게 된다.
    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 생산자 스레드에서 호출. false는 "가득 참"이라 버리면 메시지가 조용히 사라진다.
    //  T의 복사·이동이 던지지 않을 때만 noexcept — MarketData는 std::string을 품어 조건부다.
    [[nodiscard]] bool push(const T& item) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % capacity_;

        if (next == tail_.load(std::memory_order_acquire))
        {
            return false; // 버퍼 가득 참
        }

        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool push(T&& item) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % capacity_;

        if (next == tail_.load(std::memory_order_acquire))
        {
            return false;
        }

        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // 소비자 스레드에서 호출
    [[nodiscard]] std::optional<T> pop()
    {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire))
        {
            return std::nullopt; // 버퍼 비어 있음
        }

        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) % capacity_, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    // 두 atomic을 별도로 읽으므로 순간적인 근사치만 반환 (SPSC 특성상 실사용 무방)
    [[nodiscard]] size_t size() const noexcept
    {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (capacity_ - tail + head);
    }

    size_t capacity() const
    {
        return capacity_ - 1;
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;

    // head_: producer만 씀, tail_: consumer만 씀
    // 같은 cache line에 있으면 false sharing 발생 → 각각 독립 라인으로 분리
    alignas(kCacheLine) std::atomic<size_t> head_;
    alignas(kCacheLine) std::atomic<size_t> tail_;
};
