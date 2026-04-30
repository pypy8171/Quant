#pragma once
#include "core/Types.h"
#include <string>
#include <vector>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// StrategyBase  —  모든 전략이 구현해야 하는 인터페이스
//   - 플러그인 구조: 엔진은 StrategyBase* 만 알면 됨
//   - 신규 전략 추가 시 엔진 수정 불필요
// ─────────────────────────────────────────────────────────────────────────────
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    // 전략 식별자 (로그·주문 추적용)
    virtual std::string id() const = 0;

    // 시세 수신 시 엔진이 호출 → 신호 있으면 OrderSignal 반환
    virtual std::optional<OrderSignal> on_data(const MarketData& data) = 0;

    // 엔진 시작 시 초기화 (파라미터 세팅 등)
    virtual void on_start() {}

    // 엔진 종료 시 정리
    virtual void on_stop() {}

    // 전략별 파라미터 출력 (로깅용)
    virtual std::string describe() const = 0;
};
