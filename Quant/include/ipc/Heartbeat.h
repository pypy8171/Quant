// 심장박동 — 상대가 살아 있는지 박동 공백으로만 판정한다. 시계도 스레드도 로그도 여기 없다.
//  관찰(지금 시각·마지막 박동 시각)을 받아 "정상/의심/사망" 중 하나만 답한다 — 문턱 전이를 시계 없이
//  전수 시험하려고 뗐다. feed::Supervisor가 같은 꼴이다. [why D-114]
//  OS 종료 알림은 보조다. 실측으로 crash 감지가 1,385ms 늦고 멈춤(hang)은 아예 못 잡는데, 박동 문턱은 셋 다 잡았다
//  (docs/reports/FEED_MEASURE.md). 그래서 주 수단이 이쪽이다.
#pragma once

#include <atomic>
#include <cstdint>

namespace ipc
{

struct HeartbeatConfig
{
    // 뛰는 쪽이 적어도 이 간격으로 찍는다. 관찰 쪽은 이 값을 쓰지 않고 아래 둘로만 판정한다 — 배선 확인용.
    int64_t period_ms = 50;
    // 아래 두 문턱의 근거(2026-09-23 실측, bench_engine_load run --tickers 2700 --lanes 4 --shards 4 --seconds 20):
    //  전략 스레드가 한가할 때 최대 10ms 자고, 봉투 몰림 중에도 봉투 256건마다 찍으므로 가장 긴 공백이 24ms였다.
    //  D-114 본문의 5ms는 자식이 놀 때 잰 값이라 못 쓴다 — 그 값이면 이 부하에서 멀쩡한 전략을 죽었다고 본다
    //  (봉투 몰림 중 박동을 안 찍던 판에서는 3,702ms까지 벌어져 실제로 사망 판정이 났다).
    // 박동이 이만큼 없으면 의심. 아직 아무것도 하지 않고 세기만 한다. 실측 최대의 10배.
    int64_t suspect_ms = 250;
    // 박동이 이만큼 없으면 죽은 것으로 본다. 마무리 순서는 여기서 시작한다. 실측 최대의 40배 —
    //  오판 한 번이 그날 신규 매수를 막으므로, 늦게 잡는 쪽으로 기울여 둔다.
    int64_t dead_ms = 1000;
};

// 뛰는 쪽. 자기 스레드가 주기마다 시각을 찍고, 관찰 쪽이 읽는다. 값 하나뿐이라 잠금은 없다.
class Heartbeat
{
public:
    void beat(int64_t now_ns) noexcept;

    [[nodiscard]] int64_t last_ns() const noexcept;

private:
    std::atomic<int64_t> last_ns_{0};
};

// 보는 쪽. 상태를 들고 있다가 전이가 일어난 순간을 알려 준다.
//  [inv] 관찰 스레드 하나만 부른다 — 동기화는 Heartbeat의 원자 변수 하나뿐이다.
class HeartbeatMonitor
{
public:
    enum class Step
    {
        kHealthy, // 박동이 문턱 안에 있다
        kSuspect, // 의심 문턱은 넘었고 사망 문턱은 아직이다
        kDead     // 사망 문턱을 넘었다
    };

    explicit HeartbeatMonitor(HeartbeatConfig config = {});

    [[nodiscard]] const HeartbeatConfig& config() const noexcept
    {
        return config_;
    }

    // 한 번 판정한다. 둘 다 steady_clock 나노초다. 마지막 박동이 0이면(아직 한 번도 안 뛰었으면) 정상으로 본다 —
    //  기동 직후를 사망으로 읽지 않게. 첫 박동이 찍힌 뒤부터 공백을 센다.
    Step observe(int64_t now_ns, int64_t last_beat_ns);

    // 사망 판정이 새로 난 그 한 번만 참. 마무리 순서를 두 번 타지 않게 한다. 박동이 돌아오면 다시 무장된다.
    bool take_dead_once() noexcept;

    // 지금까지 본 가장 긴 박동 공백(나노초). 문턱을 정하는 근거 — 부하 아래 흔들림이 여기 쌓인다. [why D-114]
    [[nodiscard]] int64_t max_gap_ns() const noexcept
    {
        return max_gap_ns_;
    }

    [[nodiscard]] Step state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] uint64_t suspect_count() const noexcept
    {
        return suspect_count_;
    }

    [[nodiscard]] uint64_t dead_count() const noexcept
    {
        return dead_count_;
    }

private:
    HeartbeatConfig config_;
    Step            state_         = Step::kHealthy;
    bool            dead_pending_  = false; // 사망 전이가 났고 아직 아무도 가져가지 않았다
    int64_t         max_gap_ns_    = 0;
    uint64_t        suspect_count_ = 0;
    uint64_t        dead_count_    = 0;
};

} // namespace ipc
