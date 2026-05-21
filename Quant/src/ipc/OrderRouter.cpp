#include "ipc/OrderRouter.h"
#include "utils/Logger.h"
#include <iomanip>
#include <sstream>

// ─── 내부 순번 ID 생성  "ORD-000001" ─────────────────────────────────────
std::string OrderRouter::next_id()
{
    uint64_t n = ++seq_;
    std::ostringstream ss;
    ss << "ORD-" << std::setfill('0') << std::setw(6) << n;
    return ss.str();
}

// ─── 주문 제출 ────────────────────────────────────────────────────────────
ManagedOrder OrderRouter::submit(const OrderSignal& sig)
{
    auto now = std::chrono::system_clock::now();

    ManagedOrder mo;
    mo.order_id    = next_id();
    mo.signal      = sig;
    mo.submitted_at = now;
    mo.updated_at   = now;
    mo.status       = OrderStatus::PENDING;

    ++total_count_;

    // 1. OrderGate 검증
    std::string reject_reason;
    if (!gate_.check(sig, reject_reason))
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = reject_reason;
        ++rejected_count_;
        LOG_WARN("[OrderRouter] 거부 [" + mo.order_id + "] " +
                 sig.ticker + " → " + reject_reason);
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_order(sig, false);
#endif
        record(mo);
        return mo;
    }

    // 2. KIS 주문 전송
    mo.status = OrderStatus::SUBMITTED;
    std::string odno = kis_.submit_order(sig);

    mo.updated_at = std::chrono::system_clock::now();

    if (!odno.empty())
    {
        mo.status      = OrderStatus::ACCEPTED;
        mo.kis_order_no = odno;
        ++accepted_count_;
        // KIS 접수 시점에 포지션 선점 (보수적 추적 — 실제 체결 확인 전까지 재주문 차단)
        gate_.on_accept(sig.ticker, sig.side, sig.quantity, sig.price);
        LOG_INFO("[OrderRouter] 접수 [" + mo.order_id + "] ODNO=" + odno +
                 " " + sig.ticker +
                 (sig.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(sig.quantity) + "주");
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_order(sig, true);
#endif
    }
    else
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = "KIS API 오류";
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] KIS 거부 [" + mo.order_id + "] " + sig.ticker);
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_order(sig, false);
#endif
    }

    record(mo);
    return mo;
}

// ─── 이력 저장 (max_history 초과 시 가장 오래된 것 삭제) ─────────────────
void OrderRouter::record(const ManagedOrder& mo)
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
    history_.push_back(mo);
    while (static_cast<int>(history_.size()) > cfg_.max_history)
        history_.pop_front();
}

// ─── 통계 ─────────────────────────────────────────────────────────────────
OrderRouter::Stats OrderRouter::stats() const
{
    return {total_count_.load(), accepted_count_.load(), rejected_count_.load()};
}

// ─── 최근 N건 이력 ────────────────────────────────────────────────────────
std::vector<ManagedOrder> OrderRouter::recent(int n) const
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
    int start = std::max(0, static_cast<int>(history_.size()) - n);
    return {history_.begin() + start, history_.end()};
}
