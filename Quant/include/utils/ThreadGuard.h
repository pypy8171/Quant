#pragma once
// 스레드 루프 가장 바깥의 예외 방어(CODE_CONVENTIONS 9.2). 스레드 밖으로 예외가 새면 std::terminate로 프로세스
//  전체가 내려가므로, 루프를 이 함수로 감싸 잡고 로그를 남긴다. 잡은 뒤 루프를 다시 돌릴지(run_restarting) 그 스레드만
//  끝낼지(run_and_log)는 부르는 자리가 정한다 — 자리마다 고른 이유는 그 자리 주석에 적는다.
//  스레드 소유: 어느 스레드에서 불러도 된다. 상태를 갖지 않는다.
//  관련: CONSOLIDATED 2절 #22–#26.
#include <chrono>
#include <exception>
#include <string_view>
#include <thread>

namespace thread_guard
{

// 잡은 예외를 `[스레드 예외] 설명 - where(...) what(...)` 한 줄로 남긴다. 로그가 실패해도 던지지 않는다.
void log_exception(std::string_view where, std::string_view what) noexcept;

// body를 한 번 돌린다. 예외가 나면 잡아 로그를 남기고 거짓을 돌려준다. 정상으로 끝나면 참.
template <typename Body>
bool run_and_log(std::string_view where, Body&& body) noexcept
{
    try
    {
        body();
        return true;
    }
    catch (const std::exception& exception)
    {
        log_exception(where, exception.what());
    }
    catch (...)
    {
        log_exception(where, "std::exception이 아닌 예외");
    }

    return false;
}

// body가 예외로 끝나면 pause만큼 쉰 뒤 keep_running()이 참인 동안 다시 부른다. body가 정상으로 돌아오면 끝낸다.
//  쉬는 이유: 같은 원인으로 바로 다시 던지면 쉬지 않고 로그만 쌓인다.
template <typename KeepRunning, typename Body>
void run_restarting(std::string_view where, KeepRunning&& keep_running, std::chrono::milliseconds pause, Body&& body)
{
    while (keep_running())
    {
        if (run_and_log(where, body))
        {
            return;
        }

        if (!keep_running())
        {
            return;
        }

        std::this_thread::sleep_for(pause);
    }
}

} // namespace thread_guard
