#include "utils/ThreadName.h"

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace thread_name
{
namespace
{
thread_local char current_name[kMaxLength] = {};

void remember(const std::string& name)
{
    const std::size_t length = name.size() < kMaxLength - 1 ? name.size() : kMaxLength - 1;
    std::memcpy(current_name, name.data(), length);
    current_name[length] = '\0';
}

unsigned long os_thread_number()
{
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<unsigned long>(syscall(SYS_gettid));
#else
    return static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(pthread_self()));
#endif
}

} // namespace

void set_current(const std::string& name)
{
    remember(name);
#ifdef _WIN32
    std::wstring wide(name.begin(), name.end()); // 이름은 ASCII만 쓴다
    SetThreadDescription(GetCurrentThread(), wide.c_str());
#else
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

const char* current()
{
    if (current_name[0] == '\0')
    {
        remember("T" + std::to_string(os_thread_number()));
    }

    return current_name;
}

} // namespace thread_name
