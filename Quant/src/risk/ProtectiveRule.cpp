#include "risk/ProtectiveRule.h"

#include <string>

namespace risk
{
ProtectiveMode protective_mode_from_string(const std::string& text)
{
    if (text == "owner")
    {
        return ProtectiveMode::Owner;
    }

    if (text == "shadow")
    {
        return ProtectiveMode::Shadow;
    }

    return ProtectiveMode::Off;
}

} // namespace risk
