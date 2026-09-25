#include "ipc/Heartbeat.h"

#include <algorithm>

namespace ipc
{

namespace
{
// 밀리초를 나노초로. 설정은 사람이 읽기 좋게 밀리초로 두고, 비교는 박동 시각과 같은 단위로 한다.
constexpr int64_t kNanosecondsPerMillisecond = 1'000'000;
} // namespace

void Heartbeat::beat(int64_t now_ns) noexcept
{
    last_ns_.store(now_ns, std::memory_order_release);
}

int64_t Heartbeat::last_ns() const noexcept
{
    return last_ns_.load(std::memory_order_acquire);
}

HeartbeatMonitor::HeartbeatMonitor(HeartbeatConfig config) : config_(config)
{
}

HeartbeatMonitor::Step HeartbeatMonitor::observe(int64_t now_ns, int64_t last_beat_ns)
{
    if (last_beat_ns == 0)
    {
        // 아직 한 번도 안 뛰었다. 기동 직후 찍는 쪽(전략·주문·시세)이 첫 바퀴를 돌기 전이라 공백을 세지 않는다.
        state_ = Step::kHealthy;
        return state_;
    }

    const int64_t gap_ns = std::max<int64_t>(now_ns - last_beat_ns, 0);
    max_gap_ns_          = std::max(max_gap_ns_, gap_ns);

    const Step previous = state_;

    if (gap_ns >= config_.dead_ms * kNanosecondsPerMillisecond)
    {
        state_ = Step::kDead;
    }
    else if (gap_ns >= config_.suspect_ms * kNanosecondsPerMillisecond)
    {
        state_ = Step::kSuspect;
    }
    else
    {
        state_ = Step::kHealthy;
    }

    if (state_ == previous)
    {
        return state_;
    }

    // 전이가 났다. 사망은 들어갈 때 한 번만 무장하고, 정상으로 돌아오면 해제한다 —
    //  감시견이 전략을 다시 띄우면 박동이 돌아오고, 그다음 사망은 다시 마무리를 타야 한다.
    if (state_ == Step::kSuspect)
    {
        ++suspect_count_;
    }
    else if (state_ == Step::kDead)
    {
        ++dead_count_;
        dead_pending_ = true;
    }
    else
    {
        dead_pending_ = false;
    }

    return state_;
}

bool HeartbeatMonitor::take_dead_once() noexcept
{
    if (!dead_pending_)
    {
        return false;
    }

    dead_pending_ = false;
    return true;
}

} // namespace ipc
