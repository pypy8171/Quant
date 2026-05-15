#include "risk/OrderGate.h"
#include <sstream>

using Clock = std::chrono::steady_clock;

// ─── 주문 검증 ──────────────────────────────────────────────────────────────
bool OrderGate::check(const OrderSignal& sig, std::string& reject_reason)
{
    // 1. Kill switch
    if (kill_switch_.load())
    {
        reject_reason = "KILL_SWITCH 활성";
        return false;
    }

    // 2. 포지션 수량 한도 (BUY에만 적용)
    if (sig.side == OrderSide::BUY)
    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        auto it = positions_.find(sig.ticker);
        int cur_qty = (it != positions_.end()) ? it->second : 0;
        if (cur_qty + sig.quantity > cfg_.max_qty_per_ticker)
        {
            std::ostringstream ss;
            ss << "포지션 한도 초과 (" << cur_qty << "+" << sig.quantity << " > " << cfg_.max_qty_per_ticker << ")";
            reject_reason = ss.str();
            return false;
        }
    }

    // 3. 일일 손실 한도 (BUY에만 적용 — 추가 손실 가능성 차단)
    if (sig.side == OrderSide::BUY)
    {
        std::lock_guard<std::mutex> lk(pnl_mtx_);
        if (daily_pnl_ <= cfg_.daily_loss_limit)
        {
            std::ostringstream ss;
            ss << "일일 손실 한도 초과 (현재 " << static_cast<int>(daily_pnl_) << "원 / 한도 "
               << static_cast<int>(cfg_.daily_loss_limit) << "원)";
            reject_reason = ss.str();
            return false;
        }
    }

    // 4. Rate limit (분당 최대 주문)
    {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lk(rate_mtx_);
        auto cutoff = now - std::chrono::minutes(1);
        while (!order_times_.empty() && order_times_.front() < cutoff)
            order_times_.pop_front();
        if (static_cast<int>(order_times_.size()) >= cfg_.max_orders_per_min)
        {
            reject_reason = "Rate limit 초과 (분당 " + std::to_string(cfg_.max_orders_per_min) + "건)";
            return false;
        }
        order_times_.push_back(now);
    }

    // 5. 중복 신호 제거 (동일 strategy+ticker, dedup_window 이내)
    {
        auto now = Clock::now();
        std::string key = sig.strategy_id + ":" + sig.ticker;
        std::lock_guard<std::mutex> lk(dedup_mtx_);
        auto it = last_signal_.find(key);
        if (it != last_signal_.end())
        {
            double elapsed = std::chrono::duration<double>(now - it->second).count();
            if (elapsed < cfg_.dedup_window_sec)
            {
                reject_reason = "중복 신호 (윈도우 " + std::to_string(cfg_.dedup_window_sec) + "초)";
                return false;
            }
        }
        last_signal_[key] = now;
    }

    return true;
}

// ─── 체결 후 포지션 업데이트 ────────────────────────────────────────────────
void OrderGate::on_fill(const std::string& ticker, OrderSide side, int qty, double price)
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    int delta = (side == OrderSide::BUY) ? qty : -qty;
    positions_[ticker] += delta;
    if (positions_[ticker] == 0)
        positions_.erase(ticker);
    (void)price;
}

// ─── 실현 손익 누적 ─────────────────────────────────────────────────────────
void OrderGate::add_realized_pnl(double pnl)
{
    std::lock_guard<std::mutex> lk(pnl_mtx_);
    daily_pnl_ += pnl;
}

// ─── 일별 리셋 (장 시작 시) ─────────────────────────────────────────────────
void OrderGate::reset_daily()
{
    {
        std::lock_guard<std::mutex> lk(pnl_mtx_);
        daily_pnl_ = 0.0;
    }
    {
        std::lock_guard<std::mutex> lk(rate_mtx_);
        order_times_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(dedup_mtx_);
        last_signal_.clear();
    }
}

// ─── 조회 ───────────────────────────────────────────────────────────────────
int OrderGate::position(const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    auto it = positions_.find(ticker);
    return (it != positions_.end()) ? it->second : 0;
}

double OrderGate::daily_pnl() const
{
    std::lock_guard<std::mutex> lk(pnl_mtx_);
    return daily_pnl_;
}
