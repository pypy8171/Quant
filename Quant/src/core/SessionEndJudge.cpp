#include "core/SessionEndJudge.h"

namespace session_end
{
Judge::Step Judge::observe(int now_sec_of_day, bool orders_pending)
{
    if (config_.close_min <= 0 || done_)
    {
        return Step::kNone;
    }

    const int since_close = now_sec_of_day - config_.close_min * 60;

    if (since_close < 0 || since_close > config_.grace_sec + config_.drain_limit_sec)
    {
        return Step::kNone;
    }

    if (!closed_seen_)
    {
        closed_seen_ = true;
        return Step::kClosed;
    }

    if (since_close < config_.grace_sec)
    {
        return Step::kNone;
    }

    if (!orders_pending)
    {
        done_ = true;
        return Step::kShutdown;
    }

    if (since_close >= config_.grace_sec + config_.drain_limit_sec)
    {
        done_ = true;
        return Step::kShutdownForced;
    }

    return Step::kNone;
}

} // namespace session_end
