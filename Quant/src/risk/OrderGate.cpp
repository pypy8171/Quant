#include "risk/OrderGate.h"
#include <sstream>

using Clock = std::chrono::steady_clock;

// ─── 주문 검증 ──────────────────────────────────────────────────────────────
// 주의(C6): check()는 항목별 뮤텍스를 독립 스코프로 잡아 호출 단위가 원자적이지 않다.
// 현재 호출자는 단일 order_thread(Engine::order_thread_fn → OrderRouter::submit)뿐이라
// check()+on_accept이 직렬 실행돼 TOCTOU가 없다. 멀티 producer로 확장하려면
// check()+on_accept을 하나의 임계구역으로 묶어 원자적 reserve로 만들어야 한다.
bool OrderGate::check(const OrderSignal& sig, std::string& reject_reason)
{
    // 1. Kill switch — 전방향 하드스톱(BUY·SELL 모두). 연결단절/수동 긴급정지용.
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

    // 1b. Entry halt — 신규 진입(BUY NEW)만 차단. SELL 청산·취소(CANCEL/REPLACE)는 통과시켜
    //     지수 급락 시 "신규정지 + 보유분 청산"이 게이트에서 좌초되지 않게 한다(C-2).
    //     kill_switch_(전방향)와 분리된 국면 리스크 플래그.
    if (entry_halt_.load() && sig.side == OrderSide::BUY && sig.action == OrderAction::NEW)
    {
        reject_reason = "ENTRY_HALT 활성 — 신규 진입 정지(청산은 허용)";
        return false;
    }

    // 2b. 1주문 fat-finger 백스톱 (C-3) — NEW BUY/SELL 공통, 시장가 대량주문 슬리피지 방어.
    //     CANCEL/REPLACE는 대상 아님 → action==NEW로 한정해 MM 취소경로에 무영향.
    if (sig.action == OrderAction::NEW)
    {
        if (sig.quantity <= 0)
        {
            reject_reason = "잘못된 주문 수량 (" + std::to_string(sig.quantity) + ")";
            return false;
        }
        if (sig.quantity > cfg_.max_qty_per_order)
        {
            std::ostringstream ss;
            ss << "1주문 수량 한도 초과 (" << sig.quantity << " > " << cfg_.max_qty_per_order << ")";
            reject_reason = ss.str();
            return false;
        }
        // 명목 평가가: 지정가는 price, 시장가(price=0)는 ref_price(직전 현재가).
        // 시장가가 ref_price도 없으면 명목 백스톱 불가(수량 한도로만 방어).
        const double eval_px = sig.price > 0.0 ? sig.price : sig.ref_price;
        if (eval_px > 0.0 && eval_px * sig.quantity > cfg_.max_notional_per_order)
        {
            std::ostringstream ss;
            ss << "1주문 명목 한도 초과 (" << static_cast<long long>(eval_px * sig.quantity) << " > "
               << static_cast<long long>(cfg_.max_notional_per_order)
               << (sig.price > 0.0 ? ")" : ", 시장가 참조평가)");
            reject_reason = ss.str();
            return false;
        }
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

        // 3b. 종목당 명목 한도 — 자본% 사이징의 상한 백스톱. 지정가는 price, 시장가는 ref_price로
        //     보유·예약 합산 평가(시장가가 백스톱을 우회하지 않도록).
        const double eval_px = sig.price > 0.0 ? sig.price : sig.ref_price;
        if (cfg_.max_notional_per_ticker > 0.0 && eval_px > 0.0 &&
            (cur_qty + sig.quantity) * eval_px > cfg_.max_notional_per_ticker)
        {
            std::ostringstream ss;
            ss << "종목당 명목 한도 초과 ("
               << static_cast<long long>((cur_qty + sig.quantity) * eval_px) << " > "
               << static_cast<long long>(cfg_.max_notional_per_ticker) << ")";
            reject_reason = ss.str();
            return false;
        }

        // 3c. 동시 보유 종목 상한 — "새 종목"을 여는 BUY NEW에만 적용(기존 보유·예약 종목은 통과).
        //     총노출 제어: 실보유(positions_>0)∪예약(reserved_>0) 종목 수가 상한이면 신규 진입 차단.
        //     기존 보유·예약이 있는 종목(filled>0 또는 resv!=0)은 새로 여는 게 아니므로 예외.
        if (cfg_.max_concurrent_positions > 0 && sig.action == OrderAction::NEW &&
            filled == 0 && resv == 0)
        {
            size_t open = 0;
            for (const auto& kv : positions_)
                if (kv.second > 0) ++open;
            for (const auto& kv : reserved_)
                if (kv.second > 0)
                {
                    auto it = positions_.find(kv.first);
                    if (it == positions_.end() || it->second <= 0) ++open; // 예약만 있는 종목(중복 제외)
                }
            if (open >= static_cast<size_t>(cfg_.max_concurrent_positions))
            {
                std::ostringstream ss;
                ss << "동시 보유 종목 한도 초과 (" << open << " >= "
                   << cfg_.max_concurrent_positions << ") — 신규 종목 진입 정지";
                reject_reason = ss.str();
                return false;
            }
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

    // 4b. PnL stale guard (B2) — daily_pnl_이 낡으면(잔고 리컨사일 연속 정체) §4 손실컷을
    //     신뢰할 수 없어 BUY NEW만 보수적으로 정지. SELL 청산·BUY 취소/정정은 통과시켜
    //     "신규 위험만 억제, 탈출은 허용"(entry_halt와 동일 의미론). Engine이 잔고조회 복구 시 해제.
    if (pnl_stale_.load() && sig.side == OrderSide::BUY && sig.action == OrderAction::NEW)
    {
        reject_reason = "PNL_STALE — 잔고 리컨사일 정체(daily_pnl 미갱신), 신규 진입 보수적 정지";
        return false;
    }

    // 5. 중복 신호 제거 — rate 소비 전에 검사해 중복이 rate slot을 소모하지 않게 함.
    //    키에 side 포함(MM-1): 시장조성은 같은 틱에 동일 strategy+ticker로 BUY(bid)+SELL(ask)를
    //    동시 발주한다. side가 없으면 두 번째(ask)가 중복 오거부된다. BUY/SELL은 다른 의도라
    //    중복이 아니다. (같은 side 반복은 여전히 dedup — 기존 전략 동작 불변)
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
//  on_fill_confirmed의 reserved 해제와 같은 방향. positions_/avg_price는 손대지 않는다
//  (취소는 체결이 아니므로 실보유·평단 불변). qty<=0이면 no-op(방어).
void OrderGate::on_cancel(const std::string& account, const std::string& ticker,
                          OrderSide side, int qty)
{
    if (qty <= 0)
        return;
    std::lock_guard<std::mutex> lk(positions_mtx_);
    const std::string k = make_key(account, ticker);
    // 리컨사일이 reserved_를 비운 뒤 온 취소 통보는 대상이 이미 없다 → no-op.
    //  (없는 키를 -qty/+qty로 갱신하면 음수 선점이 생겨 이후 한도 계산이 왜곡됨)
    int cur = reserved_.count(k) ? reserved_[k] : 0;
    if (cur == 0)
        return;
    // BUY 선점은 +였으므로 -qty, SELL 선점은 -였으므로 +qty (해제 = 반대부호 가산)
    int delta = (side == OrderSide::BUY) ? -qty : qty;
    int r = cur + delta;
    // 과잉 해제(부호 역전) 시 0에서 정지 — 리셋·이중통보로 음수 선점이 남지 않게.
    if ((cur > 0 && r < 0) || (cur < 0 && r > 0))
        r = 0;
    if (r == 0)
        reserved_.erase(k);
    else
        reserved_[k] = r;
}

// ─── 선점 전면 초기화 (REST 리컨사일 전용) ──────────────────────────────────
void OrderGate::reset_reserved()
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    reserved_.clear();
}

// ─── 실현 손익 누적 ─────────────────────────────────────────────────────────
void OrderGate::add_realized_pnl(double pnl)
{
    std::lock_guard<std::mutex> lk(pnl_mtx_);
    daily_pnl_ += pnl;
}

// ─── 원장 부트스트랩 (G5) — 실계좌 보유분 시드 ──────────────────────────────
//  체결이 아니므로 reserved_·daily_pnl_은 두고 positions_/avg_prices_만 설정한다.
//  기동 init 구간(스레드 시작 전)에서만 호출 → 첫 주문/체결과 경합 없음.
void OrderGate::seed_position(const std::string& account, const std::string& ticker, int qty, double avg)
{
    if (qty <= 0)
        return;
    std::lock_guard<std::mutex> lk(positions_mtx_);
    const std::string k = make_key(account, ticker);
    positions_[k]  = qty;
    avg_prices_[k] = avg;
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
        // C5(MM-1): 명시적 취소는 on_cancel()로 일원화. reserved_.clear()는 EOD 안전망
        //   — 취소 없이 장 마감까지 미체결로 만료된 분의 선점을 청소한다.
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

// ─── 보유 포지션 스냅샷 (G3 강제청산) ────────────────────────────────────────
//  make_key = to_string(account.size()) + ":" + account + ticker 를 역파싱.
//  ':' 앞의 정수 n = account 길이 → 뒤 문자열의 앞 n자 = account, 나머지 = ticker.
//  파싱 실패(예상 밖 키)는 방어적으로 스킵한다.
std::vector<OrderGate::HeldPos> OrderGate::snapshot_positions() const
{
    std::vector<HeldPos> out;
    std::lock_guard<std::mutex> lk(positions_mtx_);
    out.reserve(positions_.size());
    for (const auto& kv : positions_)
    {
        if (kv.second <= 0)
            continue; // 롱 보유분만 청산 대상
        const std::string& key = kv.first;
        auto colon = key.find(':');
        if (colon == std::string::npos)
            continue;
        int n = 0;
        try { n = std::stoi(key.substr(0, colon)); }
        catch (...) { continue; }
        const std::string rest = key.substr(colon + 1);
        if (n < 0 || static_cast<size_t>(n) > rest.size())
            continue;
        HeldPos h;
        h.account = rest.substr(0, n);
        h.ticker  = rest.substr(n);
        h.qty     = kv.second;
        auto ap = avg_prices_.find(key);
        h.avg_price = (ap != avg_prices_.end()) ? ap->second : 0.0;
        out.push_back(std::move(h));
    }
    return out;
}
