#include "risk/OrderGate.h"
#include <sstream>

using Clock = std::chrono::steady_clock;

// ─── 주문 검증 ──────────────────────────────────────────────────────────────
// 주의(C6): check()는 항목별 뮤텍스를 독립 스코프로 잡으므로 호출 단위가 원자적이지 않다.
// 현재 호출자는 단일 order_thread(Engine::order_thread_fn → OrderRouter::submit)뿐이라
// check()+on_accept이 직렬 실행돼 TOCTOU가 발생하지 않는다. 멀티 producer로 확장하려면
// check()+on_accept을 하나의 임계구역으로 묶어 원자적 reserve로 만들어야 한다.
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

    // 3. 포지션 수량 한도 (BUY에만 적용) — 실체결(positions_) + 미체결 선점(reserved_) 합산
    //    계좌별 파티션 — 한 계좌 한도는 다른 계좌 주문을 막지 않는다.
    if (sig.side == OrderSide::BUY)
    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        const std::string k = make_key(sig.account_id, sig.ticker);
        int filled = positions_.count(k) ? positions_[k] : 0;
        int resv   = reserved_.count(k)  ? reserved_[k]  : 0;
        int cur_qty = filled + resv;
        if (cur_qty + sig.quantity > cfg_.max_qty_per_ticker)
        {
            std::ostringstream ss;
            ss << "포지션 한도 초과 (" << cur_qty << "+" << sig.quantity << " > " << cfg_.max_qty_per_ticker << ")";
            reject_reason = ss.str();
            return false;
        }
    }

    // 4. 일일 손실 한도 (BUY에만 적용 — 신규 진입 차단이 설계 의도)
    //    (C10) 보유분 추가 하락은 막지 않는다. 강제 청산이 필요하면 별도 청산 로직 도입.
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
    //    키에 side 포함(MM-1): 시장조성은 같은 틱에 동일 strategy+ticker로 BUY(bid)+SELL(ask)를
    //    동시 발주한다. side를 넣지 않으면 두 번째(ask)가 중복으로 오거부된다. BUY/SELL은 서로
    //    다른 의도이므로 중복이 아니다. (같은 side 반복은 여전히 dedup — 기존 전략 동작 불변)
    {
        auto now = Clock::now();
        std::string key = sig.account_id + ":" + sig.strategy_id + ":" + sig.ticker + ":" +
                          std::to_string(static_cast<int>(sig.side));
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

// ─── 접수 후 선점 (reserved_만 갱신, 실체결 원장 positions_는 불변) ──────────────
void OrderGate::on_accept(const std::string& account, const std::string& ticker,
                          OrderSide side, int qty, double price)
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    const std::string k = make_key(account, ticker);
    int delta = (side == OrderSide::BUY) ? qty : -qty;  // BUY 선점 +, SELL 선점 -
    int next  = (reserved_.count(k) ? reserved_[k] : 0) + delta;
    if (next == 0)
        reserved_.erase(k);
    else
        reserved_[k] = next;
    (void)price;
}

// ─── 미체결 취소/정정 축소 시 선점 해제 (C5) ────────────────────────────────
//  on_fill_confirmed의 reserved 해제 로직과 동일 방향. positions_/avg_price는 손대지 않는다
//  (취소는 체결이 아니므로 실보유·평단 불변). qty<=0이면 no-op(방어).
void OrderGate::on_cancel(const std::string& account, const std::string& ticker,
                          OrderSide side, int qty)
{
    if (qty <= 0)
        return;
    std::lock_guard<std::mutex> lk(positions_mtx_);
    const std::string k = make_key(account, ticker);
    // BUY 선점은 +였으므로 -qty, SELL 선점은 -였으므로 +qty (해제 = 반대부호 가산)
    int delta = (side == OrderSide::BUY) ? -qty : qty;
    int r = (reserved_.count(k) ? reserved_[k] : 0) + delta;
    if (r == 0)
        reserved_.erase(k);
    else
        reserved_[k] = r;
}

// ─── 실현 손익 누적 ─────────────────────────────────────────────────────────
void OrderGate::add_realized_pnl(double pnl)
{
    std::lock_guard<std::mutex> lk(pnl_mtx_);
    daily_pnl_ += pnl;
}

// ─── 체결 확인 — avg_price 재계산 + 실현손익 적립 ──────────────────────────
OrderGate::FillResult OrderGate::on_fill_confirmed(
    const std::string& account, const std::string& ticker, OrderSide side, int qty, double price)
{
    FillResult result;
    result.commission = price * qty * 0.00015;                              // 수수료 0.015%
    result.tax        = (side == OrderSide::SELL) ? price * qty * 0.0018 : 0.0; // 거래세 매도만

    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        const std::string k = make_key(account, ticker);
        int pre_qty    = positions_.count(k) ? positions_[k] : 0; // 체결 전 실보유
        double cur_avg = avg_prices_.count(k) ? avg_prices_[k] : 0.0;

        if (side == OrderSide::BUY)
        {
            // 실체결분만 원장에 반영 (부분체결도 정확) — 평단 분모는 실체결 수량
            int new_qty = pre_qty + qty;
            avg_prices_[k] = (new_qty > 0)
                ? (pre_qty * cur_avg + qty * price) / new_qty
                : price;
            positions_[k] = new_qty;
            result.avg_price = avg_prices_[k];
            result.net_qty   = new_qty;

            // 선점 해제 (BUY 선점은 +였으므로 -qty)
            int r = (reserved_.count(k) ? reserved_[k] : 0) - qty;
            if (r == 0) reserved_.erase(k); else reserved_[k] = r;
        }
        else // SELL
        {
            int new_qty = pre_qty - qty;
            if (new_qty < 0) new_qty = 0; // 공매도 미지원 — 보유 초과 매도는 0으로 클램프
            result.realized_pnl = (price - cur_avg) * qty
                                  - result.commission - result.tax;
            result.avg_price = cur_avg; // SELL 후 평균단가 불변
            result.net_qty   = new_qty;
            if (new_qty == 0)
            {
                positions_.erase(k);
                avg_prices_.erase(k); // 포지션 청산 시 평균단가 초기화
            }
            else
                positions_[k] = new_qty;

            // 선점 해제 (SELL 선점은 -였으므로 +qty)
            int r = (reserved_.count(k) ? reserved_[k] : 0) + qty;
            if (r == 0) reserved_.erase(k); else reserved_[k] = r;
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
    {
        // 미체결 선점은 일일 만료 (KIS 당일 주문은 EOD 소멸 → 다음날 잘못된 차단 방지).
        // C5(MM-1): 명시적 취소는 on_cancel()로 일원화됨. reserved_.clear()는 EOD 안전망
        //   — 취소 없이 만료된 미체결(장 마감까지 미체결분)의 선점을 청소한다.
        std::lock_guard<std::mutex> lk(positions_mtx_);
        reserved_.clear();
    }
    // avg_prices_ / positions_ 는 영속 원장 — 장 시작에 초기화하지 않는다
}

// ─── 조회 (계좌별) ───────────────────────────────────────────────────────────
int OrderGate::position(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    auto it = positions_.find(make_key(account, ticker));
    return (it != positions_.end()) ? it->second : 0;
}

int OrderGate::reserved(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    auto it = reserved_.find(make_key(account, ticker));
    return (it != reserved_.end()) ? it->second : 0;
}

double OrderGate::avg_price(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    auto it = avg_prices_.find(make_key(account, ticker));
    return (it != avg_prices_.end()) ? it->second : 0.0;
}

double OrderGate::daily_pnl() const
{
    std::lock_guard<std::mutex> lk(pnl_mtx_);
    return daily_pnl_;
}
