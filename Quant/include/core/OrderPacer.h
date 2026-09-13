#pragma once
// 발주 조절기 — 주문 스레드가 KIS에 주문을 낼 때의 간격(초당 한도 회피)과, 거부된 주문의 재시도 예약·만기·폐기를
//  든다. Engine의 order_thread만 부른다 — order_queue_는 SPSC(생산자=전략 스레드)라 되밀 수 없어 재시도는 이 객체의
//  전용 버퍼에 산다. 보유 수량 조회는 std::function으로 받아 OrderGate 없이 시험한다. [why D-065]
#include "core/Types.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>

namespace pacing
{
enum class Retry
{
    NONE,          // 재시도하지 않는다(접수됐거나, 되쏘면 같은 거부거나, 횟수 소진)
    RATE_LIMIT,    // 유량 한도 거부 — 접수 전 거부라 어떤 action이든 되쏜다
    SELL_REJECTED, // 청산 SELL 거부 — 유실 방지로 되쏜다(40240000은 제외)
};

struct RetryPlan
{
    Retry                     kind  = Retry::NONE;
    std::chrono::milliseconds delay = std::chrono::milliseconds(0);
};

// 거부 결과를 보고 재시도할지·언제 할지 정한다. 순수 함수.
//  retry_delay는 dedup 창(1s) 위여야 한다 — 그 아래로 되쏘면 게이트 dedup(§5)에 또 막힌다.
//  분당 한도 거부는 창이 비기까지 최대 60초라 20초로 물러난다(1.2초면 3회가 4초 안에 소진돼 같은 드롭이 된다).
RetryPlan classify(const OrderSignal& sig, int attempts, int max_retries, OrderStatus status,
                   const std::string& reject_reason, std::chrono::milliseconds retry_delay);
} // namespace pacing

class OrderPacer
{
public:
    using Clock      = std::chrono::steady_clock;
    using PositionFn = std::function<int(const std::string& account, const std::string& ticker)>; // 현재 보유 수량

    struct Config
    {
        int min_interval_ms = 350; // KIS 발주 간 최소 간격
        int max_retries     = 3;   // 거부된 주문의 재시도 횟수
    };

    struct Pending
    {
        OrderSignal sig;
        int         attempts = 0; // 이 신호가 이미 KIS에 간 횟수
    };

    OrderPacer(Config cfg, Clock::time_point now);

    void set_position(PositionFn fn)
    {
        position_ = std::move(fn);
    }

    // 만기된 재시도 가운데 아직 목적이 남은 것 하나. 청산 SELL은 보유가 0이면 목적이 이미 이뤄진 것이라 버린다.
    //  nullopt면 호출자가 새 신호 큐를 본다.
    std::optional<Pending> take_due_retry(Clock::time_point now);

    // 가장 이른 재시도 만기. 재시도가 없으면 nullopt — 주문 스레드가 그때까지 자도 되는 시각이다.
    std::optional<Clock::time_point> next_retry_at() const
    {
        if (retry_q_.empty())
        {
            return std::nullopt;
        }

        return retry_q_.front().not_before;
    }

    // 직전 KIS 호출 뒤 min_interval을 채우기까지 남은 시간. 0이면 바로 낸다.
    Clock::duration wait_before_send(Clock::time_point now) const;

    // KIS를 실제로 부른 뒤에 부른다(예외로 끝났어도). 로컬 거부(게이트·ENTRY_HALT)는 한도와 무관하니 세지 않는다.
    void note_sent(Clock::time_point now)
    {
        last_submit_ = now;
    }

    // 거부 결과를 보고 재시도를 예약한다. 예약했으면 true.
    bool on_rejected(const Pending& p, OrderStatus status, const std::string& reject_reason, Clock::time_point now);

    std::size_t retry_count() const
    {
        return retry_q_.size();
    }

    std::chrono::milliseconds retry_delay() const
    {
        return retry_delay_;
    }

private:
    struct Retry
    {
        OrderSignal       sig;
        int               attempts;
        Clock::time_point not_before;
    };

    Config                    cfg_;
    std::chrono::milliseconds min_interval_;
    std::chrono::milliseconds retry_delay_; // max(min_interval, 1200ms) — dedup 창 위
    Clock::time_point         last_submit_;
    PositionFn                position_;
    std::deque<Retry>         retry_q_;
};
