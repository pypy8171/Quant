#pragma once
#include "core/Types.h" // WatchSpec
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket 구독 칸 배정의 순수 부분. 칸(세션당 41건, 체결통보 1건 선점)은 앱키가 계좌당 하나라 늘릴 수 없다.
//  그래서 칸을 먼저 받을 종목을 우선순위로 고르고, 칸 밖 종목은 REST 현재가 폴링으로 받는다.
//  우선순위: ① 보유 ② 주문 대기(선점) ③ 재스캔 점수 순위 ④ 그 밖.
//
//  스레드: 상태가 없다. 전략 쪽(Engine::publish_watch_priorities)이 priority_of로 값을 매겨 보내고,
//  시세 쪽(Engine::rebalance_websocket_slots)이 plan으로 해제·등록할 종목을 고른다. test_websocket_slot_plan이 KIS 없이 시험한다.
//  관련 결정: D-132.
// ─────────────────────────────────────────────────────────────────────────────
namespace websocket_slot
{
// 우선순위 값 — 작을수록 칸을 먼저 받는다.
constexpr int32_t kHeld       = 0;    // 보유 중
constexpr int32_t kReserved   = 1;    // 주문을 내고 체결을 기다리는 중(선점 수량이 있다)
constexpr int32_t kScoreBase  = 1000; // 재스캔 점수 순위 r(0부터)은 kScoreBase + r
constexpr int32_t kUnranked   = std::numeric_limits<int32_t>::max(); // 보유·선점·점수 순위가 모두 없다
constexpr int32_t kProtected  = kReserved; // 이 값 이하는 다른 종목에 칸을 내주지 않는다

// 칸 배정을 받는 종목인가 — 국내 현물만 나눈다. 선물·미국은 기동 때 건 그대로 둔다. [why D-132]
bool is_managed(const WatchSpec& specification);

// 이 종목이 쓰는 칸 수 — KisWebSocket::specification_channel_count와 같은 규칙(국내 현물만 온다).
int channels_of(const WatchSpec& specification);

// scan_rank < 0 이면 점수 순위가 없다.
int32_t priority_of(bool held, bool reserved, int32_t scan_rank);

struct Entry
{
    int       channels  = 1;          // 이 종목이 쓰는 칸 수(호가+체결 2, 체결만 1)
    int32_t   priority  = kUnranked;
    bool      on_socket = false;      // 지금 칸을 쥐고 있나
    long long held_sec  = 0;          // 칸을 쥔 지 몇 초 — on_socket일 때만 뜻이 있다
};

// 잦은 교체를 막는 문턱. 매 교체는 해제·등록 프레임을 보내고 그 종목의 틱 흐름을 끊는다.
struct Rules
{
    int       capacity     = 0;  // 이 계획이 나눠 줄 칸 수(쥔 칸 + 빈 칸)
    long long min_hold_sec = 60; // 칸을 받은 지 이만큼 지나야 내준다
    int32_t   rank_gap     = 10; // 들어올 종목이 내줄 종목보다 우선순위가 이만큼 앞서야 바꾼다
    int       max_swaps    = 4;  // 한 번에 내주는 종목 수 상한
};

struct Plan
{
    std::vector<size_t> release; // 칸을 내줄 entries 번호
    std::vector<size_t> take;    // 칸을 받을 entries 번호
};

// entries 중 칸을 내줄 것과 받을 것을 고른다. 빈 칸은 우선순위 순으로 먼저 채우고, 칸이 모자라면
//  문턱(보호·유지 시간·순위 차)을 넘는 것만 바꾼다. [inv] release의 칸 합 + 빈 칸 ≥ take의 칸 합.
Plan plan(const std::vector<Entry>& entries, const Rules& rules);

} // namespace websocket_slot
