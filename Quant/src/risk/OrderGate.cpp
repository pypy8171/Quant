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

    // 2. NONE side — 전략이 신호 없음을 나타낼 때 사용; 주문 처리 불가
    if (sig.side == OrderSide::NONE)
    {
        reject_reason = "OrderSide::NONE — 유효하지 않은 주문 방향";
        return false;
    }

    // 3. 포지션 수량 한도 (BUY에만 적용)
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

    // 4. 일일 손실 한도 (BUY에만 적용 — 추가 손실 가능성 차단)
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

    // 5. 중복 신호 제거 — rate 소비 전에 먼저 검사해 중복이 rate slot을 소모하지 않도록 함
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

    // 6. Rate limit — 초당 / 분당 두 단계 검사 (dedup 통과 후에만 카운터 소모)
    {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lk(rate_mtx_);

        // 초당 제한
        auto cutoff_sec = now - std::chrono::seconds(1);
        while (!order_times_sec_.empty() && order_times_sec_.front() < cutoff_sec)
            order_times_sec_.pop_front();
        if (static_cast<int>(order_times_sec_.size()) >= cfg_.max_orders_per_sec)
        {
            reject_reason = "Rate limit 초과 (초당 " + std::to_string(cfg_.max_orders_per_sec) + "건)";
            return false;
        }

        // 분당 제한
        auto cutoff_min = now - std::chrono::minutes(1);
        while (!order_times_min_.empty() && order_times_min_.front() < cutoff_min)
            order_times_min_.pop_front();
        if (static_cast<int>(order_times_min_.size()) >= cfg_.max_orders_per_min)
        {
            reject_reason = "Rate limit 초과 (분당 " + std::to_string(cfg_.max_orders_per_min) + "건)";
            return false;
        }

        order_times_sec_.push_back(now);
        order_times_min_.push_back(now);
    }

    return true;
}

// ─── 접수 후 포지션 선점 ─────────────────────────────────────────────────────
void OrderGate::on_accept(const std::string& ticker, OrderSide side, int qty, double price)
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    int delta = (side == OrderSide::BUY) ? qty : -qty;
    int next  = positions_.count(ticker) ? positions_[ticker] + delta : delta;
    if (next <= 0)
        positions_.erase(ticker);  // SELL은 0 아래로 내려가지 않음 (공매도 미지원)
    else
        positions_[ticker] = next;
    (void)price;
}

// ─── 실현 손익 누적 ─────────────────────────────────────────────────────────
void OrderGate::add_realized_pnl(double pnl)
{
    std::lock_guard<std::mutex> lk(pnl_mtx_);
    daily_pnl_ += pnl;
}

// ─── 체결 확인 — avg_price 재계산 + 실현손익 적립 ──────────────────────────
OrderGate::FillResult OrderGate::on_fill_confirmed(
    const std::string& ticker, OrderSide side, int qty, double price)
{
    FillResult result;
    result.commission = price * qty * 0.00015;                              // 수수료 0.015%
    result.tax        = (side == OrderSide::SELL) ? price * qty * 0.0018 : 0.0; // 거래세 매도만

    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        int cur_qty  = positions_.count(ticker) ? positions_[ticker] : 0;
        double cur_avg = avg_prices_.count(ticker) ? avg_prices_[ticker] : 0.0;

        if (side == OrderSide::BUY)
        {
            // positions_[ticker]는 on_accept에서 이미 qty 선점됨
            // post_qty = 선점 후 수량, pre_qty = 체결 전 보유 수량
            int post_qty = positions_.count(ticker) ? positions_[ticker] : qty;
            int pre_qty  = post_qty - qty;
            avg_prices_[ticker] = (post_qty > 0)
                ? (pre_qty * cur_avg + qty * price) / post_qty
                : price;
            result.avg_price = avg_prices_[ticker];
            result.net_qty   = post_qty;
        }
        else // SELL
        {
            // positions_[ticker]는 on_accept에서 이미 qty 차감됨
            result.realized_pnl = (price - cur_avg) * qty
                                  - result.commission - result.tax;
            result.avg_price = cur_avg; // SELL 후 평균단가 불변
            result.net_qty   = positions_.count(ticker) ? positions_[ticker] : 0;
            if (result.net_qty == 0)
                avg_prices_.erase(ticker); // 포지션 청산 시 평균단가 초기화
        }
    }

    if (side == OrderSide::SELL)
        add_realized_pnl(result.realized_pnl);

    return result;
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
        order_times_min_.clear();
        order_times_sec_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(dedup_mtx_);
        last_signal_.clear();
    }
    // avg_prices_ / positions_ 는 영속 원장 — 장 시작에 초기화하지 않는다
}

// ─── 조회 ───────────────────────────────────────────────────────────────────
int OrderGate::position(const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    auto it = positions_.find(ticker);
    return (it != positions_.end()) ? it->second : 0;
}

double OrderGate::avg_price(const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    auto it = avg_prices_.find(ticker);
    return (it != avg_prices_.end()) ? it->second : 0.0;
}

double OrderGate::daily_pnl() const
{
    std::lock_guard<std::mutex> lk(pnl_mtx_);
    return daily_pnl_;
}
