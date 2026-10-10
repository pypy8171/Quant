#pragma once
// 여러 스레드가 자물쇠를 잡고 바꾸는 목록을, 한 스레드가 자물쇠 없이 순회하도록 그 스레드 몫의 사본을 들고 있는다.
//  쓰는 쪽은 목록을 바꿀 때마다 자물쇠 안에서 판 번호를 올리고, 읽는 쪽은 refresh()로 판 번호가 바뀐 때만 사본을 다시 뜬다.
//  바뀌지 않은 동안은 원자 읽기 한 번으로 끝나서 매 주기 목록 전체를 복사하지 않는다.
//  스레드 소유: refresh()·copy()는 사본을 쥔 스레드 하나만 부른다. 판 번호와 원본은 원본 쪽 자물쇠가 지킨다.
//  관련: CONSOLIDATED 2절 #1(데이터 스레드가 watch_specifications_를 자물쇠 없이 순회하던 경쟁).
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

template <typename Container>
class GenerationCopy
{
public:
    // 원본이 바뀌었으면 자물쇠를 잡고 사본을 새로 뜬다. 돌려준 참조는 같은 스레드가 다음 refresh()를 부를 때까지 유효하다.
    //  [lock-order] 판 번호는 relaxed로 읽는다. 바뀐 것을 늦게 보면 한 주기 늦게 복사할 뿐이고, 복사 자체는 자물쇠 안이라
    //  원본 내용의 가시성은 mutex가 보장한다.
    const Container& refresh(const Container& source, std::mutex& source_mutex, const std::atomic<uint64_t>& source_generation)
    {
        if (source_generation.load(std::memory_order_relaxed) == copied_generation_)
        {
            return copy_;
        }

        std::lock_guard<std::mutex> source_lock(source_mutex);
        copy_              = source;
        copied_generation_ = source_generation.load(std::memory_order_relaxed);
        return copy_;
    }

    const Container& copy() const
    {
        return copy_;
    }

private:
    Container copy_;
    // 처음 refresh()가 반드시 복사하도록 원본이 가질 수 없는 값으로 시작한다(원본 판 번호는 0부터 올라간다).
    uint64_t copied_generation_ = std::numeric_limits<uint64_t>::max();
};
