#include "risk/GateReasons.h"

namespace gate_reason
{
std::string rate_limit(bool per_minute, int limit)
{
    return std::string(kRateLimit) + " (" + (per_minute ? kPerMinute : kPerSecond) + " " + std::to_string(limit) +
           "건)";
}

} // namespace gate_reason
