#pragma once
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
// MutexQueue<T>  —  락-프리 MPSC(MpscQueue)의 대조군(control group)
//   - std::mutex + std::deque. 자명하게 correct하고 구현이 단순하다.
//   - 다중 생산자가 같은 락을 다투므로 경합 시 지연 꼬리(p99/p999)가 커진다.
//   - MpscQueue와 동일 API + bounded 동작을 맞춰, 동일 부하에서 처리량/지연을
//     직접 비교하기 위한 벤치 기준선으로 쓴다.
//     ("락-프리가 뮤텍스 대비 p99를 X% 낮췄다"의 대조군)
// ─────────────────────────────────────────────────────────────────────────────

template <typename T> class MutexQueue
{
public:
    explicit MutexQueue(size_t capacity) : capacity_(capacity)
    {
    }

    bool push(const T& item)
    {
        return emplace(item);
    }
    bool push(T&& item)
    {
        return emplace(std::move(item));
    }

    std::optional<T> pop()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty())
            return std::nullopt;
        T item = std::move(q_.front());
        q_.pop_front();
        return item;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.empty();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

    size_t capacity() const
    {
        return capacity_;
    }

private:
    template <typename U> bool emplace(U&& item)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.size() >= capacity_)
            return false; // bounded — MpscQueue의 가득 참과 동일 계약
        q_.push_back(std::forward<U>(item));
        return true;
    }

    const size_t capacity_;
    mutable std::mutex mtx_;
    std::deque<T> q_;
};
