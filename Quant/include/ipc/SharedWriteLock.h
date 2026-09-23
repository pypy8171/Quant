// 공유 쪽지 위 표에 넣는 동안만 잡는 자물쇠. 표마다 따로 두면 한쪽만 고쳐지는 날 규약이 갈리므로 한 벌만 둔다.
//  [inv] 잡는 쪽은 한 프로세스의 스레드들뿐이다 — 건너편은 읽기만 하고 이 칸을 건드리지 않는다. 그래서
//  자물쇠를 쥔 채 죽어도 건너편이 멈추지 않는다. 양쪽이 잡게 바꾸려면 주인 생사 확인이 먼저다. [why D-114]
#pragma once

#include <atomic>
#include <cstdint>

namespace ipc
{

class SharedWriteLock
{
public:
    explicit SharedWriteLock(std::atomic<uint32_t>& flag);
    ~SharedWriteLock();

    SharedWriteLock(const SharedWriteLock&)            = delete;
    SharedWriteLock& operator=(const SharedWriteLock&) = delete;

private:
    std::atomic<uint32_t>& flag_;
};

} // namespace ipc
