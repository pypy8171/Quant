#include "utils/ThreadName.h"

namespace thread_name
{
void set_current(const std::string& name)
{
#ifdef _WIN32
    std::wstring wide(name.begin(), name.end()); // 이름은 ASCII만 쓴다
    SetThreadDescription(GetCurrentThread(), wide.c_str());
#else
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

} // namespace thread_name
