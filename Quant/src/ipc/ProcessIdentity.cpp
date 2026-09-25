// Quant/include/ipc/ProcessIdentity.h 구현 — 프로세스 표를 읽고, 그 프로세스가 아직 사는지 운영체제에 묻는다.
//  플랫폼 갈래는 Quant/src/ipc/SharedRegion.cpp 와 같은 방식으로 이 파일 안 #ifdef 하나로 둔다.
#include "ipc/ProcessIdentity.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#endif

namespace ipc
{
namespace
{

#ifdef _WIN32

// 그 손잡이가 가리키는 프로세스가 만들어진 시각(FILETIME 100ns). 못 읽으면 0이다.
uint64_t creation_time_of(HANDLE handle) noexcept
{
    FILETIME creation{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};

    if (GetProcessTimes(handle, &creation, &exited, &kernel, &user) == 0)
    {
        return 0;
    }

    ULARGE_INTEGER packed{};
    packed.LowPart  = creation.dwLowDateTime;
    packed.HighPart = creation.dwHighDateTime;
    return packed.QuadPart;
}

#else

// /proc/<번호>/stat 의 22번째 칸(부팅 뒤 클럭 틱)과 3번째 칸(상태)을 읽는다. 못 읽으면 start_time 0이다.
//  두 번째 칸(실행 파일 이름)에 괄호와 빈칸이 들어갈 수 있어 **마지막 ')' 뒤부터** 센다 — 그 자리가 3번째 칸이다.
bool read_proc_stat(uint32_t process_id, uint64_t& start_time, char& state) noexcept
{
    start_time = 0;
    state      = '?';

    std::ifstream file("/proc/" + std::to_string(process_id) + "/stat");

    if (!file.is_open())
    {
        return false;
    }

    std::string line;
    std::getline(file, line);
    const size_t tail = line.rfind(')');

    if (tail == std::string::npos)
    {
        return false;
    }

    std::istringstream rest(line.substr(tail + 1));
    std::string        field;
    // 마지막 ')' 뒤 첫 칸이 3번째 칸이므로, 22번째 칸은 여기서 스무 번째다.
    constexpr int      kStartTimeIndex = 20;

    for (int index = 1; index <= kStartTimeIndex; ++index)
    {
        if (!(rest >> field))
        {
            return false;
        }

        if (index == 1 && !field.empty())
        {
            state = field.front();
        }

        if (index == kStartTimeIndex)
        {
            // strtoull 을 쓴다 — stoull 은 칸이 숫자가 아니면 던지고, 이 함수는 noexcept 다.
            start_time = std::strtoull(field.c_str(), nullptr, 10);
        }
    }

    return true;
}

#endif

} // namespace

ProcessIdentity current_process_identity() noexcept
{
    ProcessIdentity identity;

#ifdef _WIN32
    identity.process_id = static_cast<uint32_t>(GetCurrentProcessId());
    identity.start_time = creation_time_of(GetCurrentProcess());
#else
    identity.process_id = static_cast<uint32_t>(getpid());
    char state          = '?';
    (void)read_proc_stat(identity.process_id, identity.start_time, state);
#endif

    return identity;
}

bool process_is_alive(const ProcessIdentity& identity) noexcept
{
    if (!identity.is_set())
    {
        return false;
    }

#ifdef _WIN32
    const HANDLE handle =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(identity.process_id));

    if (handle == nullptr)
    {
        // 권한이 없다는 것은 그 번호를 쥔 프로세스가 있다는 뜻이다 — 살아 있는 쪽으로 읽는다.
        //  산 주인을 죽었다고 보고 쪽지를 덮어쓰는 것이 반대 오독보다 훨씬 나쁘다.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    bool alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;

    if (alive && identity.start_time != 0)
    {
        const uint64_t creation = creation_time_of(handle);
        alive                   = creation == 0 || creation == identity.start_time;
    }

    CloseHandle(handle);
    return alive;
#else
    uint64_t start_time = 0;
    char     state      = '?';

    if (!read_proc_stat(identity.process_id, start_time, state))
    {
        return false;
    }

    if (state == 'Z')
    {
        // 좀비는 거둬지기를 기다릴 뿐 제 코드는 이미 멎었다 — 쪽지의 주인으로 세지 않는다.
        return false;
    }

    return identity.start_time == 0 || start_time == 0 || start_time == identity.start_time;
#endif
}

} // namespace ipc
