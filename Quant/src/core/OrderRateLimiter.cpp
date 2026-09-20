// 발주 조절기 구현 — order_thread 전용. 재시도 규칙은 order_rate::classify 하나에 있다. [why D-065]
#include "core/OrderRateLimiter.h"
#include "api/KisErrorCodes.h"
#include "risk/GateReasons.h"
#include "utils/Logger.h"

#include <algorithm>
#include <utility>

namespace order_rate
{
RetryPlan classify(const OrderSignal& signal, int attempts, int max_retries, OrderStatus status,
                   const std::string& reject_reason, std::chrono::milliseconds retry_delay)
{
    if (status != OrderStatus::REJECTED || attempts >= max_retries)
    {
        return {};
    }

    // 유량 한도 거부 — KIS가 '접수 전' 거부라 중복주문 위험 없음(빈-ODNO 모호성 없음). 모든 action(취소·정정·
    //  매수·매도)을 deduplicate 창 밖으로 재예약해 유실 없이 자가치유한다. 예전엔 취소·매수가 드롭돼 미연결 주문이 남고,
    //  다음 사이클에 다시 처리되다 또 한도초과가 나는 악순환이었다. 간격은 짧게 두고, 한도에 부딪힐 때만 물러난다.
    // [wire] KIS 서버 거부는 EGW00201, OrderGate 자체 거부는 gate_reason::kRateLimit 머리의 문장이라 둘을 같이 받는다.
    //  앞엣것만 보던 동안 게이트 분당한도에 걸린 BUY가 조용히 드롭됐다.
    if (reject_reason.find(kis_error::kRateLimit) != std::string::npos || gate_reason::is_rate_limit(reject_reason))
    {
        // 분당 한도는 창이 비기까지 최대 60초 — 짧게 되쏘면 재시도 3회가 몇 초 안에 소진돼 같은 드롭이 된다.
        constexpr std::chrono::seconds kPerMinuteBackoff{20};
        const bool per_min = gate_reason::is_per_minute(reject_reason);
        return {Retry::RATE_LIMIT, per_min ? std::chrono::milliseconds(kPerMinuteBackoff) : retry_delay};
    }

    // 청산 SELL 유실 방지(C-2). BUY는 제외: 빈-ODNO 응답이 실제로는 접수됐을 수 있어 재시도가 중복주문을 낳는다.
    //  40240000(주문가능분 없음)도 제외: 보유수량이 예약매도/미결제로 묶인 '지속성' 조건이라 되쏘면 매번 같은
    //  거부다. 유일 해법(예약매도 취소→시장가 재매도)은 라우터의 reconcile_blocked_sell이 이미 1회 시도했다.
    if (signal.action == OrderAction::NEW && signal.side == OrderSide::SELL &&
        reject_reason.find(kis_error::kNoSellableQty) == std::string::npos)
    {
        return {Retry::SELL_REJECTED, retry_delay};
    }

    return {};
}
} // namespace order_rate

OrderRateLimiter::OrderRateLimiter(Config config, Clock::time_point now)
    : config_(config),
      min_interval_(config.min_interval_ms),
      retry_delay_(std::max(config.min_interval_ms, kRetryDelayFloorMs)),
      last_submit_(now - min_interval_) // 첫 주문은 기다리지 않는다
{
}

std::optional<OrderRateLimiter::Pending> OrderRateLimiter::take_due_retry(Clock::time_point now)
{
    while (!retry_queue_.empty() && now >= retry_queue_.front().not_before)
    {
        Retry retry = std::move(retry_queue_.front());
        retry_queue_.pop_front();

        // 청산이 이미 끝났으면 버린다. 원주문이 체결되는 동안 예약된 청산 SELL 재시도가 남아 있다가 보유가 0이
        //  된 뒤에 발주돼 40240000으로 거부되곤 했다. 거부라 원장은 다치지 않지만 청산 한 건마다 오거부가 몇 줄씩
        //  쌓여 진짜 거부를 덮는다. 재시도의 목적은 미청산분을 마저 파는 것이니 보유가 0이면 이미 이뤄진 것이다.
        if (retry.signal.action == OrderAction::NEW && retry.signal.side == OrderSide::SELL && position_ &&
            position_(retry.signal.account_id, retry.signal.ticker) <= 0)
        {
            LOG_INFO("[OrderThread] 청산 완료 — 재시도 취소 " + retry.signal.ticker + " " + std::to_string(retry.signal.quantity) +
                     "주");
            continue;
        }

        return Pending{std::move(retry.signal), retry.attempts};
    }

    return std::nullopt;
}

OrderRateLimiter::Clock::duration OrderRateLimiter::wait_before_send(Clock::time_point now) const
{
    const auto since = now - last_submit_;
    return since >= min_interval_ ? Clock::duration::zero() : Clock::duration(min_interval_ - since);
}

bool OrderRateLimiter::on_rejected(Pending pending, OrderStatus status, const std::string& reject_reason,
                             Clock::time_point now)
{
    const auto plan = order_rate::classify(pending.signal, pending.attempts, config_.max_retries, status, reject_reason, retry_delay_);

    if (plan.kind == order_rate::Retry::NONE)
    {
        return false;
    }

    const std::string tries = " (" + std::to_string(pending.attempts + 1) + "/" + std::to_string(config_.max_retries) +
                              ") 이유=" + reject_reason;

    if (plan.kind == order_rate::Retry::RATE_LIMIT)
    {
        const char* action_text = pending.signal.action == OrderAction::CANCEL    ? "CANCEL"
                                : pending.signal.action == OrderAction::REPLACE ? "REPLACE"
                                                                       : "NEW";
        LOG_WARN("[OrderThread] 유량한도 거부 → 재시도 예약 " + pending.signal.ticker + " " + action_text + tries);
    }
    else
    {
        LOG_WARN("[OrderThread] 청산 SELL 거부 → 재시도 예약 " + pending.signal.ticker + tries);
    }

    retry_queue_.push_back({std::move(pending.signal), pending.attempts + 1, now + plan.delay});
    return true;
}
