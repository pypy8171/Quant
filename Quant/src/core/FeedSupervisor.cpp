#include "core/FeedSupervisor.h"

namespace feed
{
Supervisor::Step Supervisor::observe(bool market_open, bool stale, clock::time_point now)
{
    if (!market_open)
    {
        return Step::kIdle;
    }

    if (!stale)
    {
        fail_streak_ = 0;
        return Step::kHealthy;
    }

    return now < next_try_ ? Step::kWaitBackoff : Step::kReconnect;
}

Supervisor::After Supervisor::on_reconnect(bool ok, clock::time_point now)
{
    if (ok)
    {
        fail_streak_ = 0;
        return After::kNone;
    }

    ++fail_streak_;
    last_backoff_sec_ = std::min(config_.backoff_step_sec * fail_streak_, config_.backoff_max_sec);
    next_try_ = now + std::chrono::seconds(last_backoff_sec_);
    return fail_streak_ == config_.fallback_after_fails ? After::kFallback : After::kNone;
}

} // namespace feed
