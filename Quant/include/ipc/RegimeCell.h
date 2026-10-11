// 국면 칸 — 지금 어떤 국면인가를 전략 쪽이 적고 나머지 역할이 읽는 값 하나. 링이 아니다.
//  국면은 흐름이 아니라 "지금 무엇인가"다. 낱말로 흘려보내면 받는 쪽이 다음 전환까지 빈 채로 돌고,
//  장중에 주문 프로세스만 다시 뜬 날은 그날 남은 시간 내내 빈다 — 그 사이 체결의 regime 열이 통째로
//  빈 칸이 되어 국면별로 되짚을 수 없다. 값 한 칸이면 붙는 순간 지금 값을 그대로 읽는다. [why D-129]
//  [inv] 적는 쪽은 전략 역할 하나(Engine::apply_regime_selection), 읽는 쪽은 여럿이다.
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace ipc
{

// 아직 판정이 없음. 놓는 쪽이 이 값으로 민다 — 0으로 밀면 그것이 실제 국면 하나와 겹쳐,
//  판정 전 체결이 "RISK_ON 에서 난 체결"로 적힌다.
constexpr int32_t kRegimeNone = -1;

struct RegimeCell
{
    // Regime::Value 를 정수로 담는다. ipc 는 core 의 타입을 모르고, 읽는 쪽이 제 타입으로 되돌린다.
    std::atomic<int32_t> code{kRegimeNone};
};

// 공유 메모리 칸에 그대로 놓인다. MSVC x64 계산: atomic<int32_t> 하나 = 4. 원자 필드라 trivially copyable 은 아니고,
//  표준 배치·자명한 소멸·잠금 없는 원자만 본다.
static_assert(sizeof(RegimeCell) == 4, "RegimeCell 크기가 바뀌었다 — 공유 메모리 자리표를 다시 본다");
static_assert(std::is_standard_layout_v<RegimeCell> && std::is_trivially_destructible_v<RegimeCell>,
              "RegimeCell 은 공유 메모리에 놓인다");
static_assert(std::atomic<int32_t>::is_always_lock_free, "공유 메모리 단어는 잠금 없는 원자여야 한다");

} // namespace ipc
