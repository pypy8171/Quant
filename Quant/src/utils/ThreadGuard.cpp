#include "utils/ThreadGuard.h"

#include "utils/Logger.h"

#include <string>

namespace thread_guard
{

void log_exception(std::string_view where, std::string_view what) noexcept
{
    try
    {
        std::string message = "[스레드 예외] 루프에서 예외를 잡았다 - where(";
        message.append(where).append(") what(").append(what).append(")");
        LOG_ERROR(message);
    }
    catch (...)
    {
        // 로그를 만들다 실패하면(할당 실패 등) 남길 방법이 없다. 여기서 다시 던지면 감싼 의미가 없어 그대로 넘긴다.
    }
}

} // namespace thread_guard
