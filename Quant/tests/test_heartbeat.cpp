// tests/test_heartbeat.cpp
// 심장박동 전이 검증 (D-114 단계 2) — 상대가 살아 있는지 박동 공백만으로 판정하는가.
//
//   시계도 스레드도 쓰지 않는다. 시각을 손으로 넣어 전이를 전수로 본다 — feed::Supervisor 시험과 같은 방식이다.
//
//   ① 기동 직후(아직 한 번도 안 뛴 상태)를 사망으로 읽지 않는가
//   ② 정상 → 의심 → 사망 문턱을 시각대로 넘는가
//   ③ 사망 판정은 들어갈 때 한 번만 가져가지는가(마무리를 두 번 타지 않게)
//   ④ 박동이 돌아오면 정상으로 내려오고 사망이 다시 무장되는가
//   ⑤ 본 가장 긴 공백을 쌓는가(문턱을 정하는 근거)
//   ⑥ 뛰는 쪽과 보는 쪽이 값 하나로 이어지는가
//   ⑦ 주문 쪽 문턱이 증권사 왕복 상한 위에 있는가(멀쩡히 왕복 중인 주문 스레드를 죽었다고 읽지 않게)
//
//   사용법: test_heartbeat

#include "ipc/Heartbeat.h"

#include <cassert>
#include <iostream>
#include <string>

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cout << "[FAIL] " << name << "\n";
        std::abort();
    }

    std::cout << "[PASS] " << name << "\n";
}

constexpr int64_t kNanosecondsPerMillisecond = 1'000'000;

// 밀리초를 나노초로 — 시험을 읽기 좋게.
constexpr int64_t milliseconds(int64_t count)
{
    return count * kNanosecondsPerMillisecond;
}

using Step = ipc::HeartbeatMonitor::Step;

ipc::HeartbeatConfig test_config()
{
    ipc::HeartbeatConfig config;
    config.period_ms  = 50;
    config.suspect_ms = 250;
    config.dead_ms    = 1000;
    return config;
}
} // namespace

int main()
{
    std::cout << "=== 심장박동 전이 시험 (D-114 단계 2) ===\n";

    // ── ① 기동 직후 ──────────────────────────────────────────────────────
    {
        ipc::HeartbeatMonitor monitor(test_config());

        // 마지막 박동이 0 = 아직 한 번도 안 뛰었다. 기동 직후 전략 스레드가 첫 바퀴를 돌기 전이다.
        check(monitor.observe(milliseconds(10'000), 0) == Step::kHealthy, "한 번도 안 뛴 상대를 사망으로 읽지 않는다");
        check(monitor.dead_count() == 0, "그때 사망 수는 늘지 않는다");
        check(monitor.max_gap_ns() == 0, "박동이 없으면 공백도 세지 않는다");
    }

    // ── ② 문턱을 시각대로 넘는가 ─────────────────────────────────────────
    {
        ipc::HeartbeatMonitor monitor(test_config());
        const int64_t         beat_at = milliseconds(1'000);

        check(monitor.observe(beat_at + milliseconds(0), beat_at) == Step::kHealthy, "막 뛰었으면 정상");
        check(monitor.observe(beat_at + milliseconds(249), beat_at) == Step::kHealthy, "의심 문턱 직전은 정상");
        check(monitor.observe(beat_at + milliseconds(250), beat_at) == Step::kSuspect, "의심 문턱에 닿으면 의심");
        check(monitor.observe(beat_at + milliseconds(999), beat_at) == Step::kSuspect, "사망 문턱 직전은 의심");
        check(monitor.observe(beat_at + milliseconds(1'000), beat_at) == Step::kDead, "사망 문턱에 닿으면 사망");
        check(monitor.suspect_count() == 1, "의심은 들어갈 때 한 번만 센다");
        check(monitor.dead_count() == 1, "사망도 들어갈 때 한 번만 센다");
    }

    // ── ③ 사망은 한 번만 가져가진다 ──────────────────────────────────────
    {
        ipc::HeartbeatMonitor monitor(test_config());
        const int64_t         beat_at = milliseconds(1'000);

        monitor.observe(beat_at + milliseconds(1'500), beat_at);
        check(monitor.take_dead_once(), "사망 전이 뒤 처음 한 번은 가져간다");
        check(!monitor.take_dead_once(), "두 번째는 안 가져간다 — 마무리를 두 번 타지 않는다");

        // 죽은 채로 계속 봐도 다시 무장되지 않는다.
        monitor.observe(beat_at + milliseconds(3'000), beat_at);
        check(!monitor.take_dead_once(), "사망 상태가 이어지는 동안에도 다시 안 가져간다");
        check(monitor.dead_count() == 1, "사망 수도 그대로다");
    }

    // ── ④ 박동이 돌아오면 ────────────────────────────────────────────────
    {
        ipc::HeartbeatMonitor monitor(test_config());
        const int64_t         first_beat = milliseconds(1'000);

        monitor.observe(first_beat + milliseconds(1'500), first_beat);
        check(monitor.take_dead_once(), "먼저 한 번 죽는다");

        // 감시견이 전략을 다시 띄웠다 — 새 박동이 찍힌다.
        const int64_t second_beat = milliseconds(3'000);
        check(monitor.observe(second_beat + milliseconds(10), second_beat) == Step::kHealthy, "박동이 돌아오면 정상으로 내려온다");
        check(!monitor.take_dead_once(), "정상으로 내려온 것만으로는 가져갈 사망이 없다");

        // 그다음 사망은 다시 마무리를 타야 한다.
        monitor.observe(second_beat + milliseconds(1'500), second_beat);
        check(monitor.take_dead_once(), "다시 죽으면 다시 한 번 가져간다");
        check(monitor.dead_count() == 2, "두 번째 사망도 센다");
    }

    // ── ⑤ 가장 긴 공백 ───────────────────────────────────────────────────
    {
        ipc::HeartbeatMonitor monitor(test_config());
        const int64_t         beat_at = milliseconds(1'000);

        monitor.observe(beat_at + milliseconds(30), beat_at);
        monitor.observe(beat_at + milliseconds(180), beat_at);
        monitor.observe(beat_at + milliseconds(90), beat_at);

        check(monitor.max_gap_ns() == milliseconds(180), "지금까지 본 가장 긴 공백을 들고 있다");

        // 시각이 거꾸로 와도(스레드 사이 읽기 순서) 음수 공백을 쌓지 않는다.
        monitor.observe(beat_at - milliseconds(5), beat_at);
        check(monitor.max_gap_ns() == milliseconds(180), "박동이 지금보다 앞서 보여도 공백은 0으로 본다");
    }

    // ── ⑥ 뛰는 쪽과 보는 쪽 ──────────────────────────────────────────────
    {
        ipc::Heartbeat heartbeat;
        check(heartbeat.last_ns() == 0, "아직 안 뛰었으면 0");

        heartbeat.beat(milliseconds(500));
        check(heartbeat.last_ns() == milliseconds(500), "찍은 시각이 그대로 읽힌다");

        heartbeat.beat(milliseconds(700));
        check(heartbeat.last_ns() == milliseconds(700), "다시 찍으면 갱신된다");

        ipc::HeartbeatMonitor monitor(test_config());
        check(monitor.observe(milliseconds(720), heartbeat.last_ns()) == Step::kHealthy, "보는 쪽이 그 값으로 판정한다");
    }

    // ── ⑦ 주문 쪽 문턱 ──────────────────────────────────────────────────
    //  전략 쪽과 달리 주문 스레드의 공백에는 증권사 왕복이 그대로 들어온다 — 한 번 부르는 데 윈도는
    //  전송 10초·수신 15초(KisTransport.cpp 의 WinHttpSetTimeouts), 리눅스는 10초(CURLOPT_TIMEOUT)
    //  까지 간다. 문턱을 그 아래로 좁히면 멀쩡히 답을 기다리는 주문 스레드가 사망으로 읽힌다.
    //  Engine::strategy_thread_fn 이 쓰는 값을 여기 박아 둔다. [why D-114]
    {
        constexpr ipc::HeartbeatConfig kOrderSideConfig{50, 30'000, 60'000};
        ipc::HeartbeatMonitor         monitor(kOrderSideConfig);

        // 마지막 박동을 실제 값으로 둔다 — 0은 "아직 한 번도 안 뛰었다"라 문턱을 안 탄다(①).
        const int64_t     beat_at             = milliseconds(1'000);
        constexpr int64_t kWindowsRoundTripMs = 10'000 + 15'000; // 전송 + 수신 상한

        check(monitor.observe(beat_at + milliseconds(kWindowsRoundTripMs), beat_at) == Step::kHealthy,
              "증권사 왕복 상한만큼 비어도 아직 정상이다");

        check(monitor.observe(beat_at + milliseconds(kOrderSideConfig.suspect_ms), beat_at) == Step::kSuspect,
              "그 위에서 의심으로 넘어간다");
        check(monitor.observe(beat_at + milliseconds(kOrderSideConfig.dead_ms), beat_at) == Step::kDead,
              "사망 문턱은 왕복 상한의 두 배 넘게 떨어져 있다");

        check(kOrderSideConfig.suspect_ms > kWindowsRoundTripMs,
              "의심 문턱이 왕복 상한 위에 있다 — 좁히려면 실측부터");
    }

    std::cout << "test_heartbeat: " << g_checks << " checks passed\n";
    return 0;
}
