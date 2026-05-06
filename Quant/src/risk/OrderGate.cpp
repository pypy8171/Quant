#include "risk/OrderGate.h"

bool OrderGate::check(const OrderSignal& sig, std::string& reject_reason)
{
    // TODO: 종목별 포지션 한도 체크 (per-ticker max position)
    // TODO: 일일 누적 손실 한도 (daily loss kill switch)
    // TODO: 주문 빈도 제한 (rate limit per second)
    // TODO: 시장가 대비 가격 이탈 sanity check (limit only)
    // TODO: 동일 신호 idempotency (strategy_id + ticker dedup window)

    (void)sig;
    (void)reject_reason;
    return true;
}
