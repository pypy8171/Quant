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
    std::string odno;
    try
    {
        odno = kis_.submit_order(sig);
    }
    catch (const std::exception& e)
    {
        mo.status        = OrderStatus::REJECTED;
        mo.reject_reason = std::string("KIS 예외: ") + e.what();
        ++rejected_count_;
        LOG_ERROR("[OrderRouter] KIS 예외 [" + mo.order_id + "] " + sig.ticker + " — " + e.what());
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_order(sig, false);
#endif
        record(mo);
        return mo;
    }

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
        mo.reject_reason = "KIS API 거부 (빈 ODNO)";
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

// ─── 이력 저장 (max_history 초과 시 체결 완료/거부된 것만 삭제) ───────────
void OrderRouter::record(const ManagedOrder& mo)
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
    history_.push_back(mo);
    while (static_cast<int>(history_.size()) > cfg_.max_history)
    {
        // ACCEPTED(체결 대기 중) 주문은 보호 — ODNO 매핑이 끊기면 체결통보 누락
        if (history_.front().status == OrderStatus::ACCEPTED)
            break;
        history_.pop_front();
    }
}

// ─── 체결통보 처리 — ODNO 매핑 → 부분/전량 체결 처리 ─────────────────────
void OrderRouter::on_fill(const FillNotification& fn)
{
    std::lock_guard<std::mutex> lk(hist_mtx_);
    for (auto& mo : history_)
    {
        if (mo.kis_order_no != fn.odno)
            continue;
        // 부분체결: ACCEPTED(최초) 또는 FILLED(분할 진행 중) 모두 허용
        if (mo.status != OrderStatus::ACCEPTED && mo.status != OrderStatus::FILLED)
            continue;
        // 이미 전량 체결 완료된 주문은 재처리 방지
        if (mo.confirmed_qty >= mo.signal.quantity)
            continue;

        mo.confirmed_qty += fn.filled_qty;
        mo.updated_at     = fn.timestamp;
        if (mo.confirmed_qty >= mo.signal.quantity)
            mo.status = OrderStatus::FILLED;

        LOG_INFO("[OrderRouter] 체결 확인 [" + mo.order_id + "] ODNO=" + fn.odno +
                 " " + fn.ticker +
                 (fn.side == OrderSide::BUY ? " BUY " : " SELL ") +
                 std::to_string(fn.filled_qty) + "주 @" +
                 std::to_string(static_cast<int>(fn.filled_price)) +
                 " (누적 " + std::to_string(mo.confirmed_qty) +
                 "/" + std::to_string(mo.signal.quantity) + "주)");

        // 포지션 원장 갱신 (avg_price 재계산 + 실현손익)
        auto result = gate_.on_fill_confirmed(fn.ticker, fn.side,
                                              fn.filled_qty, fn.filled_price);
#ifdef HAS_ZMQ
        if (zmq_)
            zmq_->publish_fill(fn, result.commission, result.tax,
                               result.avg_price, result.net_qty,
                               result.realized_pnl);
#endif
        return;
    }
    LOG_WARN("[OrderRouter] 체결통보 매핑 실패 ODNO=" + fn.odno +
             " (이미 처리됐거나 이력 범위 초과)");
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
