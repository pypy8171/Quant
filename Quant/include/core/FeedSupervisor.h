// WS 피드 감독기 — 시세 미수신(stale)→재연결→백오프→REST 폴백 전이의 판정만. 소켓 호출·로그·폴백 적용은
// Engine(control_thread)이 하고, 여기는 관찰(장중·stale·재연결 결과·시각)을 받아 "이번 주기에 무엇을 할지"만 답한다 —
// 백오프 누적·폴백 문턱을 시계·소켓 없이 전수 시험하려고 뗐다. control_thread 전용이라 동기화는 없다. [why D-071]
#pragma once

#include <algorithm>
#include <chrono>

namespace feed
{

struct SupervisorConfig
{
    int stale_sec           = 30;  // 이 초 넘게 틱이 없으면 죽은 것으로 본다(장중만)
    int backoff_step_sec    = 30;  // 실패 n회째 재시도 간격 = step × n
    int backoff_max_sec     = 300; // 간격 상한 — KIS 연결 한도 소진·스팸 방지
    int fallback_after_fails = 3;  // 연속 실패가 이 수에 닿는 순간 한 번만 폴백을 요구한다
};

class Supervisor
{
public:
    using clock = std::chrono::steady_clock;

    // 한 주기의 답. kIdle·kWaitBackoff는 할 일 없음, kHealthy는 폴백을 해제할 자리, kReconnect는 재연결을 시도하고
    //  결과를 on_reconnect로 돌려줄 자리.
    enum class Step
    {
        kIdle,        // 장 외 — stale이 정상이라 판정하지 않는다
        kHealthy,     // 정상 수신 — 실패 누적을 지운다
        kWaitBackoff, // stale이지만 직전 실패의 재시도 시각 전
        kReconnect
    };

    // 재연결 결과에 따르는 요구. 폴백은 연속 실패가 문턱에 닿는 첫 번째에만 — 그 뒤 실패에서 다시 요구하지 않는다.
    //  폴백을 해제(kHealthy)했다가 다시 문턱에 닿으면 다시 요구한다.
    enum class After
    {
        kNone,
        kFallback
    };

    explicit Supervisor(SupervisorConfig config = {}) : config_(config) {}

    const SupervisorConfig& config() const { return config_; }

    Step observe(bool market_open, bool stale, clock::time_point now);

    After on_reconnect(bool ok, clock::time_point now);

    int fail_streak() const { return fail_streak_; }
    int last_backoff_sec() const { return last_backoff_sec_; } // 직전 실패가 정한 대기(초), 로그용
    clock::time_point next_try() const { return next_try_; }

private:
    SupervisorConfig  config_;
    int               fail_streak_      = 0;
    int               last_backoff_sec_ = 0;
    clock::time_point next_try_{}; // [inv] 기본값(epoch)은 어떤 now보다 앞이라 첫 stale은 바로 재연결한다
};

} // namespace feed
