#pragma once
// 교체 진입 창구 — 자리가 꽉 찬 책에 새 종목 매수가 오면 최약체를 먼저 비우고, 그 매수를 자리가 날 때까지 들고 있다.
//  최약체 고르기(읽기)·매도 발주·쿨다운 기록(쓰기)이 한 덩어리라 쪼개지 않는다. 쪼개면 최약체를 고르는 시점과
//  자리를 예약하는 시점이 갈려 둘이 같은 종목을 두 번 판다. 그래서 전략 쪽이 아니라 주문 쪽에 둔다. [why D-114]
//  [inv] 주문 스레드 하나만 부른다(원칙 4 단일 시퀀서) — 보류 목록에 잠금이 없다.
#include "core/Types.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <string>

namespace risk
{

class DisplacementDesk
{
public:
    using Clock   = std::chrono::steady_clock;
    using LabelFn = std::function<std::string(const std::string&)>; // 로그용 종목 표기

    explicit DisplacementDesk(OrderGate& gate);

    void set_label(LabelFn label);

    // 낼 신호 하나에 대한 판정.
    enum class Verdict
    {
        kPass,      // 그대로 낸다
        kHold,      // 자리가 날 때까지 들고 있다 — 이번 회차에 낼 것이 없다
        kSellFirst, // 최약체 매도를 먼저 낸다. 매수는 창구가 들고 있다
    };

    // kSellFirst면 signal 자리에 교체 매도가 들어온다(원래의 매수는 창구가 가져간다).
    //  [inv] signal.symbol_id는 불린 쪽이 이미 찍어 둔다 — 여기서 종목 표에 등록하지 않는다.
    [[nodiscard]] Verdict consider(OrderSignal& signal, Clock::time_point now);

    // 예약 시한이 지난 보류 매수를 버린다. 버린 것은 expired 뒤에 붙는다 — 부른 쪽이 그 순번에 답을 돌려준다.
    void expire(Clock::time_point now, std::vector<OrderSignal>& expired);

    // 자리가 났으면 들고 있던 매수를 앞에서부터 하나 꺼낸다. 없으면 거짓.
    [[nodiscard]] bool take_ready(OrderSignal& out);

    [[nodiscard]] std::size_t      held_count() const noexcept;
    [[nodiscard]] symbol::SymbolId held_symbol() const noexcept; // 자리를 예약해 둔 수혜 종목(kNone=없음)

private:
    [[nodiscard]] std::string label(const std::string& ticker) const;

    OrderGate&                 gate_;
    LabelFn                    label_;
    strategy_table::StrategyId displace_index_;
    std::deque<OrderSignal>    held_;                        // 자리를 기다리는 매수(같은 분할 매수의 단계들)
    symbol::SymbolId           held_symbol_ = symbol::kNone; // 보류 목록이 비어도 시한까지는 남는다
    Clock::time_point          held_until_{};
    bool                       drain_logged_ = false; // 이번 보류분을 내보내기 시작했다고 한 번만 적는다
};

} // namespace risk
