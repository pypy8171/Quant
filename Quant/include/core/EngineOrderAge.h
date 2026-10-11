#pragma once
#include "core/Types.h" // OrderSignal
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// 주문 나이 판정 — 요청 면에서 꺼낸 신규 매수가 너무 오래 기다렸는지, 같은 종목 줄 뒤에 다시 세울지 정한다.
//  상태가 없는 constexpr 함수라 주문 스레드(Engine::order_thread_fn)와 test_engine이 같이 쓴다.
//  Engine.h에서 떼어 낸 것이다. 관련 결정: D-127·D-151·D-155.
// ─────────────────────────────────────────────────────────────────────────────

// 주문 큐에서 이 시간을 넘게 기다린 신규 매수는 꺼낼 때 버린다. 큐가 찬 동안 증권사에 낼 수 있는 건수는
//  초당한도로 고정이라 늘릴 수 없고, 남은 예산을 2초 전 판단에 쓰면 그만큼 지금 판단이 못 나간다.
//  취소·정정과 매도(손절·청산)는 나이를 안 본다 — 늦어도 보내야 하는 주문이다. [why D-127]
inline constexpr int64_t kOrderSignalMaxAgeNs   = 1'000'000'000; // 1초
inline constexpr int64_t kNanosecondsPerMillisecond = 1'000'000; // 로그에 ms로 적을 때 쓰는 나눗수

// 지금 꺼내 보내기엔 너무 오래된 신호인가. 시각을 안 찍은 신호(signal_at_ns=0)는 나이를 모르니 보낸다.
//  나이 제한 면제 표시(exempt_from_age_limit)가 켜진 신호도 보낸다. [why D-155]
constexpr bool is_stale_entry(const OrderSignal& signal, int64_t popped_at_ns)
{
    return signal.action == OrderAction::NEW && signal.side == OrderSide::BUY && !signal.exempt_from_age_limit
           && signal.signal_at_ns != 0 && popped_at_ns - signal.signal_at_ns > kOrderSignalMaxAgeNs;
}

// 같은 종목 앞 주문(취소·정정·신규)의 답을 기다린 신규 매수는 이 시간을 넘길 때만 버린다. 그 대기는 우리 쪽 순서
//  보장 때문에 생긴 것이라 1초 기준(kOrderSignalMaxAgeNs)으로 재면 취소 뒤 다시 까는 매수가 거의 다 버려진다
//  (10-01 실계좌 082270: 취소 왕복 1.1초 뒤 BUY 32 소실). KIS 주문 왕복 p90 약 4.5초를 넉넉히 덮는 값. [why D-151]
inline constexpr int64_t kParkedOrderMaxWaitNs = 10'000'000'000; // 10초

// 앞 주문 답을 기다리다 너무 오래 선 신규 매수인가. 큐에서 꺼낸 시각(parked_at_ns)부터 잰다 — 큐에서 꺼낼 때
//  이미 1초 기준을 통과했다. 시각이 없으면(0)·나이 제한 면제 표시가 켜졌으면 보낸다. [why D-155]
constexpr bool is_stale_parked_entry(const OrderSignal& signal, int64_t parked_at_ns, int64_t now_ns)
{
    return signal.action == OrderAction::NEW && signal.side == OrderSide::BUY && !signal.exempt_from_age_limit
           && parked_at_ns != 0 && now_ns - parked_at_ns > kParkedOrderMaxWaitNs;
}

// 고른 주문을 같은 종목 줄 뒤에 다시 세워야 하는가. 같은 종목이 답을 기다리면 세운다. 줄에 같은 종목이 서 있으면
//  새로 온 주문만 세운다 — 줄에서 꺼낸 주문은 이미 그 종목의 맨 앞이라, 다시 세우면 남은 같은 종목 건과 서로
//  뒤로 미루며 끝없이 돈다(10-01 모의 306200 취소·매도 두 건이 10:00~15:22 주문 스레드를 묶어 다른 신호까지 잃었다). [why D-151]
constexpr bool should_park(bool symbol_sending, bool symbol_parked, bool taken_from_parked)
{
    return symbol_sending || (symbol_parked && !taken_from_parked);
}
