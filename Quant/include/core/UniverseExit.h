#pragma once
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// 유니버스 이탈·복귀 판정의 순수 부분. Engine::maybe_rescan_universe(data_thread)가 시계·스캔 결과를 넣어
//  부르고, test_signal_dispatcher가 KIS 없이 시험한다 [why D-077].
//
//  시계는 하나(연속 부재 초), 임계값은 둘이다 — block_after_sec에 신규매수를 막고, drop_after_sec에 전략을 뗀다.
//  복귀는 present 스캔이 return_confirm회 연속일 때만 — 한 번 보이자마자 풀면 경계 종목이 사고팔기를 반복한다.
// ─────────────────────────────────────────────────────────────────────────────
namespace universe_exit
{
struct Thresholds
{
    int block_after_sec = 0;   // 이만큼 연속 부재면 신규매수 차단. ≤0이면 차단 안 함(떼기만)
    int drop_after_sec  = 0;   // 이만큼 연속 부재면 전략 해제. ≤0이면 안 뗌
    int return_confirm  = 2;   // 차단 해제에 필요한 연속 present 스캔 수. 임시 고정값 — 가변 규칙은 뒤로(D-077)
};

// 차단이 해제보다 늦으면 뜻이 없다 — 해제 시각에 맞춘다. 값이 바뀌었으면 호출자가 WARN을 남긴다.
inline int clamp_block(int block_after_sec, int drop_after_sec)
{
    if (drop_after_sec > 0 && block_after_sec > drop_after_sec)
    {
        return drop_after_sec;
    }

    return block_after_sec;
}

enum class Absent
{
    KEEP,    // 아직 임계 전
    BLOCK,   // 신규매수 차단으로 전환(in_universe true→false)
    DROP     // 전략 해제
};

// 연속 부재 absent_sec로 판정. 이미 차단된 종목(in_universe=false)에는 BLOCK을 다시 내지 않는다.
inline Absent judge_absent(long long absent_sec, const Thresholds& th, bool in_universe)
{
    if (th.drop_after_sec > 0 && absent_sec >= th.drop_after_sec)
    {
        return Absent::DROP;
    }

    if (in_universe && th.block_after_sec > 0 && absent_sec >= th.block_after_sec)
    {
        return Absent::BLOCK;
    }

    return Absent::KEEP;
}

// 차단 상태에서 present 스캔이 return_confirm회 연속이면 true(차단 해제). return_confirm≤1은 1회로 본다.
inline bool judge_return(int present_streak, const Thresholds& th, bool in_universe)
{
    return !in_universe && present_streak >= std::max(1, th.return_confirm);
}
} // namespace universe_exit
