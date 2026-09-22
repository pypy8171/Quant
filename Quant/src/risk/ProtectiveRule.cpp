#include "risk/ProtectiveRule.h"

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

const char* protective_mode_name(ProtectiveMode mode)
{
    switch (mode)
    {
    case ProtectiveMode::Owner:
        return "owner";

    case ProtectiveMode::Shadow:
        return "shadow";

    default:
        return "off";
    }
}

} // namespace risk
