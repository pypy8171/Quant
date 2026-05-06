#pragma once
#include "core/Types.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// OrderGate  —  주문 전 위험 검증 게이트
//
//  Engine::order_thread_fn이 KIS send_order를 호출하기 직전에
//  check()를 통과한 신호만 실제 주문으로 내보낸다.
//
//  현재: 항상 true 반환 (인터페이스 정의만).
//  향후 각 TODO 항목을 단계적으로 구현한다.
// ─────────────────────────────────────────────────────────────────────────────
class OrderGate {
public:
    // true  → 주문 허용
    // false → 주문 거부 (reject_reason에 사유 기록)
    bool check(const OrderSignal& sig, std::string& reject_reason);
};
