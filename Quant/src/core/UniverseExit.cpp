#include "core/UniverseExit.h"

namespace universe_exit
{
int clamp_block(int block_after_sec, int drop_after_sec)
{
    if (drop_after_sec > 0 && block_after_sec > drop_after_sec)
    {
        return drop_after_sec;
    }

    return block_after_sec;
}

Absent judge_absent(long long absent_sec, const Thresholds& thread, bool in_universe)
{
    if (thread.drop_after_sec > 0 && absent_sec >= thread.drop_after_sec)
    {
        return Absent::DROP;
    }

    if (in_universe && thread.block_after_sec > 0 && absent_sec >= thread.block_after_sec)
    {
        return Absent::BLOCK;
    }

    return Absent::KEEP;
}

} // namespace universe_exit
