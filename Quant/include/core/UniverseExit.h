#pragma once
#include "core/SymbolTable.h"

#include <algorithm>
#include <string>
#include <vector>

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
int clamp_block(int block_after_sec, int drop_after_sec);

enum class Absent
{
    KEEP,    // 아직 임계 전
    BLOCK,   // 신규매수 차단으로 전환(in_universe true→false)
    DROP     // 전략 해제
};

// 연속 부재 absent_sec로 판정. 이미 차단된 종목(in_universe=false)에는 BLOCK을 다시 내지 않는다.
Absent judge_absent(long long absent_sec, const Thresholds& thread, bool in_universe);

// 차단 상태에서 present 스캔이 return_confirm회 연속이면 true(차단 해제). return_confirm≤1은 1회로 본다.
inline bool judge_return(int present_streak, const Thresholds& thread, bool in_universe)
{
    return !in_universe && present_streak >= std::max(1, thread.return_confirm);
}

// 등록 상한이 찼을 때 자리를 내줄 후보 — 오늘 스캔 top-N에 없고(점수 밀림) 미보유·미선점인 등록 종목 중
//  부재가 가장 오래된 것(absent_sec 최댓값). 종목은 id로 말한다 — in_scan·held는 id 인덱스 비트.
//  owned의 순서는 등록·해제 이력에 달리므로, 부재 시간이 같을 때는(부재 추적이 없어 전부 0일 때 포함)
//  id가 작은 쪽(먼저 번호를 받은 종목)으로 마저 정해 입력이 같으면 항상 같은 종목을 고른다 [why D-087].
//  없으면 symbol::kNone.
template <typename ReservedFn, typename AbsentSecFn>
inline symbol::SymbolId pick_evict_candidate(const std::vector<symbol::SymbolId>& owned, const std::vector<bool>& in_scan,
                                             const std::vector<bool>& held, ReservedFn&& reserved, AbsentSecFn&& absent_sec)
{
    symbol::SymbolId best            = symbol::kNone;
    long long        best_absent_sec = -1;

    for (symbol::SymbolId symbol : owned)
    {
        if ((symbol < in_scan.size() && in_scan[symbol]) || (symbol < held.size() && held[symbol]) || reserved(symbol) != 0)
        {
            continue;
        }

        const long long seconds = absent_sec(symbol);

        if (seconds > best_absent_sec || (seconds == best_absent_sec && symbol < best))
        {
            best            = symbol;
            best_absent_sec = seconds;
        }
    }

    return best;
}
} // namespace universe_exit
