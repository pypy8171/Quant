#pragma once
// 현재 스레드에 이름을 붙인다 — 리눅스는 /proc/<pid>/task/<tid>/comm(procwatch가 스레드별 CPU를 이 이름으로 적재),
// Windows는 디버거·프로세스 탐색기가 보는 스레드 설명. 리눅스 이름은 15바이트까지라 그 안에서 짓는다.
#include <string>

#ifdef _WIN32
// <windows.h>를 헤더에 들이면 ERROR 매크로가 LogLevel::ERROR와 부딪히므로 필요한 두 함수만 선언한다.
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void* thread, const wchar_t* description);
#else
#include <pthread.h>
#endif

namespace thread_name
{

void set_current(const std::string& name);

} // namespace thread_name
