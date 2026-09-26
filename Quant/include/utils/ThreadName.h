#pragma once
// 현재 스레드에 이름을 붙인다 — 리눅스는 /proc/<pid>/task/<tid>/comm(procwatch가 스레드별 CPU를 이 이름으로 적재),
// Windows는 디버거·프로세스 탐색기가 보는 스레드 설명. 리눅스 이름은 15바이트까지라 그 안에서 짓는다.
// 같은 이름을 스레드별 저장소(thread_local)에도 남겨 로그 줄마다 어느 스레드가 찍었는지 적는다.
#include <cstddef>
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

// 이름 끝 NUL까지 16바이트. 리눅스 한도(15바이트+NUL)와 같게 둔다.
constexpr std::size_t kMaxLength = 16;

void set_current(const std::string& name);

// 이 스레드의 이름. set_current를 부른 적 없으면 "T" + 운영체제 스레드 번호를 한 번 만들어 둔다.
//  [inv] 돌려준 포인터는 이 스레드가 살아 있는 동안만 유효하다 — 다른 스레드로 넘길 때는 복사한다.
const char* current();

} // namespace thread_name
