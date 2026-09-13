// WS 피드 감독기(core/FeedSupervisor.h) 단위 테스트. 장 외 무시·정상 수신의 누적 초기화·첫 stale 즉시 재연결·
// 실패 n회 백오프(step×n, 상한)·재시도 시각 전 대기·폴백 요구는 문턱에 닿는 한 번만·복귀 뒤 재무장을 고정한다.
// 헤더 전용이라 소켓·시계 없이 돈다(시각은 인자로 넣는다). 관련 결정: D-071(Phase 3 감독기).
// 빌드: cmake --build <dir> --target test_feed_supervisor
#include "core/FeedSupervisor.h"

#include <iostream>

using feed::Supervisor;
using feed::SupervisorConfig;
using Step  = Supervisor::Step;
using After = Supervisor::After;

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

Supervisor::clock::time_point at(int sec)
{
    return Supervisor::clock::time_point{} + std::chrono::seconds(sec);
}
} // namespace

int main()
{
    // 1. 장 외에는 stale이어도 아무것도 하지 않고 누적도 건드리지 않는다.
    {
        Supervisor s;
        CHECK(s.observe(false, true, at(0)) == Step::kIdle);
        CHECK(s.observe(false, false, at(0)) == Step::kIdle);
        CHECK(s.fail_streak() == 0);
    }

    // 2. 첫 stale은 백오프 없이 바로 재연결. 성공하면 누적 0, 그다음 정상 수신은 kHealthy.
    {
        Supervisor s;
        CHECK(s.observe(true, true, at(100)) == Step::kReconnect);
        CHECK(s.on_reconnect(true, at(100)) == After::kNone);
        CHECK(s.fail_streak() == 0);
        CHECK(s.observe(true, false, at(105)) == Step::kHealthy);
    }

    // 3. 실패 n회째 간격은 step×n이고 상한에서 멈춘다. 재시도 시각 전에는 kWaitBackoff, 지나면 kReconnect.
    {
        Supervisor s; // 기본값 30/300/3
        int t = 100;
        const int expect[] = {30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 300, 300};

        for (int n = 1; n <= 12; ++n)
        {
            CHECK(s.observe(true, true, at(t)) == Step::kReconnect);
            s.on_reconnect(false, at(t));
            CHECK(s.fail_streak() == n);
            CHECK(s.last_backoff_sec() == expect[n - 1]);
            CHECK(s.next_try() == at(t + expect[n - 1]));
            CHECK(s.observe(true, true, at(t + expect[n - 1] - 1)) == Step::kWaitBackoff);
            t += expect[n - 1];
        }
    }

    // 4. 폴백 요구는 연속 실패가 문턱(3)에 닿는 그 한 번만. 4회째부터는 kNone.
    {
        Supervisor s;
        int t = 0;
        After seen[5];

        for (int n = 0; n < 5; ++n)
        {
            s.observe(true, true, at(t));
            seen[n] = s.on_reconnect(false, at(t));
            t += s.last_backoff_sec();
        }

        CHECK(seen[0] == After::kNone && seen[1] == After::kNone);
        CHECK(seen[2] == After::kFallback);
        CHECK(seen[3] == After::kNone && seen[4] == After::kNone);
    }

    // 5. 정상 수신(kHealthy)이 누적을 지우면 다음 stale은 백오프 없이 바로 재연결하고, 다시 3회 실패하면 폴백을 다시 요구한다.
    {
        SupervisorConfig c;
        c.backoff_step_sec = 1;
        Supervisor s(c);
        int t = 0;

        for (int n = 0; n < 3; ++n)
        {
            s.observe(true, true, at(t));
            s.on_reconnect(false, at(t));
            t += 10;
        }

        CHECK(s.fail_streak() == 3);
        CHECK(s.observe(true, false, at(t)) == Step::kHealthy);
        CHECK(s.fail_streak() == 0);
        CHECK(s.observe(true, true, at(t + 1)) == Step::kReconnect);
        s.on_reconnect(false, at(t + 1));
        s.observe(true, true, at(t + 10));
        s.on_reconnect(false, at(t + 10));
        s.observe(true, true, at(t + 20));
        CHECK(s.on_reconnect(false, at(t + 20)) == After::kFallback);
    }

    // 6. 재연결에 성공했는데 틱이 여전히 없으면(연결은 됐지만 stale) 다음 주기에 바로 다시 시도한다 — 성공은 재시도 시각을 미루지 않는다.
    {
        Supervisor s;
        s.observe(true, true, at(0));
        s.on_reconnect(false, at(0)); // next_try = 30
        CHECK(s.observe(true, true, at(30)) == Step::kReconnect);
        CHECK(s.on_reconnect(true, at(30)) == After::kNone);
        CHECK(s.observe(true, true, at(35)) == Step::kReconnect);
    }

    // 7. 장 외로 넘어가면 백오프 시각과 누적은 그대로 남고, 장이 다시 열리면 이어서 판정한다.
    {
        Supervisor s;
        s.observe(true, true, at(0));
        s.on_reconnect(false, at(0));
        CHECK(s.observe(false, true, at(10)) == Step::kIdle);
        CHECK(s.fail_streak() == 1);
        CHECK(s.observe(true, true, at(10)) == Step::kWaitBackoff);
        CHECK(s.observe(true, true, at(30)) == Step::kReconnect);
    }

    std::cout << "test_feed_supervisor OK (" << g_checks << " checks)\n";
    return 0;
}
