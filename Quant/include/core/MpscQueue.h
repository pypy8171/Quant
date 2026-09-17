#pragma once
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// MpscQueue<T>  —  Vyukov Bounded MPSC Lock-Free Queue
//   - Multi Producer / Single Consumer. RingBuffer<T>와 동일한 API(드롭인 교체).
//   - 다중 생산자 상황(N 클라이언트가 동시에 주문 push)에서 SPSC RingBuffer는
//     같은 슬롯을 덮어써 유실이 발생한다. 이를 슬롯별 sequence 번호로 해결한다.
//   - 생산자는 enqueue_position_(티켓 발급기)를 CAS로 다투어 "자리"만 예약하고,
//     데이터 쓰기는 각자 다른 칸에서 경합 없이 병렬 수행한다.
//   - 소비자는 하나뿐이므로 dequeue 경로에는 CAS가 필요 없다.
//
//   참조: Dmitry Vyukov, "Bounded MPMC queue"
//         (원형은 다중 생산자/다중 소비자(MPMC, Multi Producer Multi Consumer).
//          여기서는 소비자 1개 계약으로 사용 — dequeue를 store로 단순화)
// ─────────────────────────────────────────────────────────────────────────────

namespace mpsc_detail
{
#ifdef __cpp_lib_hardware_interference_size
inline constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr size_t kCacheLine = 64;
#endif

// 용량을 2의 거듭제곱으로 올림 → position & mask 로 modulo 대체 (최소 2)
inline constexpr size_t round_up_pow2(size_t count) noexcept
{
    return std::bit_ceil(count < 2 ? size_t{2} : count);
}
} // namespace mpsc_detail

template <typename T> class MpscQueue
{
public:
    explicit MpscQueue(size_t capacity)
        : buffer_(mpsc_detail::round_up_pow2(capacity)), mask_(buffer_.size() - 1), enqueue_position_(0),
          dequeue_position_(0)
    {
        // 초기 상태: i번 칸은 i번째 티켓을 기다린다 (sequence == 그 칸을 채울 position 값)
        for (size_t buffer_index = 0; buffer_index < buffer_.size(); ++buffer_index)
        {
            buffer_[buffer_index].sequence.store(buffer_index, std::memory_order_relaxed);
        }
    }

    MpscQueue(const MpscQueue&)            = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // 여러 생산자 스레드에서 동시 호출 가능
    [[nodiscard]] bool push(const T& item)
    {
        return emplace(item);
    }

    [[nodiscard]] bool push(T&& item)
    {
        return emplace(std::move(item));
    }

    // 단일 소비자 스레드에서만 호출
    [[nodiscard]] std::optional<T> pop()
    {
        const size_t position = dequeue_position_.load(std::memory_order_relaxed);
        Cell& cell = buffer_[position & mask_];
        const size_t sequence = cell.sequence.load(std::memory_order_acquire);
        const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position + 1);

        if (difference < 0)
        {
            return std::nullopt; // 이 칸이 아직 안 채워짐 = 큐 비어 있음
        }

        // difference == 0 : 채워진 칸. (소비자 1개이므로 difference > 0 은 발생하지 않음)

        T item = std::move(cell.data);
        // 데이터를 뺀 뒤에 칸을 "다음 바퀴용 빈 칸"으로 표시 → 생산자가 재사용 가능
        cell.sequence.store(position + mask_ + 1, std::memory_order_release);
        dequeue_position_.store(position + 1, std::memory_order_relaxed);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return enqueue_position_.load(std::memory_order_acquire) ==
               dequeue_position_.load(std::memory_order_acquire);
    }

    // enqueue/dequeue를 별도로 읽으므로 근사치 (실사용 무방)
    [[nodiscard]] size_t size() const noexcept
    {
        const size_t enq = enqueue_position_.load(std::memory_order_acquire);
        const size_t deq = dequeue_position_.load(std::memory_order_acquire);
        return enq - deq;
    }

    size_t capacity() const
    {
        return buffer_.size();
    }

private:
    struct Cell
    {
        std::atomic<size_t> sequence;
        T data;
    };

    template <typename U> bool emplace(U&& item)
    {
        size_t position = enqueue_position_.load(std::memory_order_relaxed);
        Cell* cell;

        while (true)
        {
            cell = &buffer_[position & mask_];
            const size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);

            if (difference == 0)
            {
                // 이 칸이 비었고 정확히 내 차례 — 티켓(enqueue_position_) 확정 시도
                if (enqueue_position_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
                {
                    break; // 성공: 이 칸의 임자 확정. (실패 시 position 최신값으로 갱신되어 재시도)
                }
            }
            else if (difference < 0)
            {
                return false; // 지난 바퀴 데이터가 아직 소비 안 됨 = 큐 가득 참(backpressure)
            }
            else
            {
                position = enqueue_position_.load(std::memory_order_relaxed); // 남이 예약함 → 재로드
            }
        }

        cell->data = std::forward<U>(item);
        // release: 앞의 data 쓰기를 가둔 뒤 "채웠다"를 공개 → 소비자 acquire와 happens-before
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    std::vector<Cell> buffer_;
    const size_t mask_;

    // enqueue_position_: 생산자들이 CAS로 공유, dequeue_position_: 소비자 전용.
    // 같은 cache line에 있으면 false sharing → 각각 독립 라인으로 분리
    alignas(mpsc_detail::kCacheLine) std::atomic<size_t> enqueue_position_;
    alignas(mpsc_detail::kCacheLine) std::atomic<size_t> dequeue_position_;
};
