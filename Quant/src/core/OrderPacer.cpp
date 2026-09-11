// 발주 조절기 구현 — order_thread 전용. 재시도 규칙은 pacing::classify 하나에 있다. [why D-065]
#include "core/OrderPacer.h"
#include "api/KisErrorCodes.h"
#include "utils/Logger.h"

#include <algorithm>

namespace pacing
{
RetryPlan classify(const OrderSignal& sig, int attempts, int max_retries, OrderStatus status,
                   const std::string& reject_reason, std::chrono::milliseconds retry_delay)
{
    if (status != OrderStatus::REJECTED || attempts >= max_retries)
    {
        return {};
    }

    // 유량 한도 거부 — KIS가 '접수 전' 거부라 중복주문 위험 없음(빈-ODNO 모호성 없음). 모든 action(취소·정정·
    //  매수·매도)을 dedup 창 밖으로 재예약해 유실 없이 자가치유한다. 예전엔 취소·매수가 드롭돼 미연결 주문이 남고,
    //  다음 사이클에 다시 처리되다 또 한도초과가 나는 악순환이었다. 간격은 짧게 두고, 한도에 부딪힐 때만 물러난다.
    // [wire] KIS 서버 거부는 EGW00201, OrderGate 자체 거부는 "Rate limit 초과 (…)" 문자열이라 둘을 같이 받는다.
    //  앞엣것만 보던 동안 게이트 분당한도에 걸린 BUY가 조용히 드롭됐다.
    if (reject_reason.find(kis_err::kRateLimit) != std::string::npos || reject_reason.rfind("Rate limit", 0) == 0)
    {
        const bool per_min = reject_reason.find("분당") != std::string::npos;
        return {Retry::RATE_LIMIT, per_min ? std::chrono::milliseconds(20000) : retry_delay};
    }

    // 청산 SELL 유실 방지(C-2). BUY는 제외: 빈-ODNO 응답이 실제로는 접수됐을 수 있어 재시도가 중복주문을 낳는다.
    //  40240000(주문가능분 없음)도 제외: 보유수량이 예약매도/미결제로 묶인 '지속성' 조건이라 되쏘면 매번 같은
    //  거부다. 유일 해법(예약매도 취소→시장가 재매도)은 라우터의 reconcile_blocked_sell이 이미 1회 시도했다.
    if (sig.action == OrderAction::NEW && sig.side == OrderSide::SELL &&
        reject_reason.find(kis_err::kNoSellableQty) == std::string::npos)
    {
        return {Retry::SELL_REJECTED, retry_delay};
    }

    return {};
}
} // namespace pacing

OrderPacer::OrderPacer(Config cfg, Clock::time_point now)
    : cfg_(cfg),
      min_interval_(cfg.min_interval_ms),
      retry_delay_(std::max(cfg.min_interval_ms, 1200)),
      last_submit_(now - min_interval_) // 첫 주문은 기다리지 않는다
{
}

std::optional<OrderPacer::Pending> OrderPacer::take_due_retry(Clock::time_point now)
{
    while (!retry_q_.empty() && now >= retry_q_.front().not_before)
    {
        Retry r = std::move(retry_q_.front());
        retry_q_.pop_front();

        // 청산이 이미 끝났으면 버린다. 원주문이 체결되는 동안 예약된 청산 SELL 재시도가 남아 있다가 보유가 0이
        //  된 뒤에 발주돼 40240000으로 거부되곤 했다. 거부라 원장은 다치지 않지만 청산 한 건마다 오거부가 몇 줄씩
        //  쌓여 진짜 거부를 덮는다. 재시도의 목적은 미청산분을 마저 파는 것이니 보유가 0이면 이미 이뤄진 것이다.
        if (r.sig.action == OrderAction::NEW && r.sig.side == OrderSide::SELL && position_ &&
            position_(r.sig.account_id, r.sig.ticker) <= 0)
        {
            LOG_INFO("[OrderThread] 청산 완료 — 재시도 취소 " + r.sig.ticker + " " + std::to_string(r.sig.quantity) +
                     "주");
            continue;
        }

        return Pending{std::move(r.sig), r.attempts};
    }

    return std::nullopt;
}

OrderPacer::Clock::duration OrderPacer::wait_before_send(Clock::time_point now) const
{
    const auto since = now - last_submit_;
    return since >= min_interval_ ? Clock::duration::zero() : Clock::duration(min_interval_ - since);
}

bool OrderPacer::on_rejected(const Pending& p, OrderStatus status, const std::string& reject_reason,
                             Clock::time_point now)
{
    const auto plan = pacing::classify(p.sig, p.attempts, cfg_.max_retries, status, reject_reason, retry_delay_);

    if (plan.kind == pacing::Retry::NONE)
    {
        return false;
    }

    retry_q_.push_back({p.sig, p.attempts + 1, now + plan.delay});
    const std::string tries = " (" + std::to_string(p.attempts + 1) + "/" + std::to_string(cfg_.max_retries) +
                              ") 이유=" + reject_reason;

    if (plan.kind == pacing::Retry::RATE_LIMIT)
    {
        const std::string act = p.sig.action == OrderAction::CANCEL    ? "CANCEL"
                                : p.sig.action == OrderAction::REPLACE ? "REPLACE"
                                                                       : "NEW";
        LOG_WARN("[OrderThread] 유량한도 거부 → 재시도 예약 " + p.sig.ticker + " " + act + tries);
    }
    else
    {
        LOG_WARN("[OrderThread] 청산 SELL 거부 → 재시도 예약 " + p.sig.ticker + tries);
    }

    return true;
}
