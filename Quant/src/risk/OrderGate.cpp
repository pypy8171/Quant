#include "risk/OrderGate.h"
#include <ctime>
#include <iomanip>
#include <sstream>

using Clock = std::chrono::steady_clock;

namespace
{
// 국내 주식 체결 비용률 (KIS 실계좌 기준).
constexpr double kCommissionRate = 0.00015; // 위탁수수료 0.015% (매수·매도 공통)
constexpr double kSellTaxRate    = 0.0018;  // 증권거래세 0.18% (매도에만 부과)

// 장중 잔여시간 비율 — 09:00에 1.0, 15:00 이후 0.0. 점수 우선순위 바를 오후로 갈수록 낮춘다.
//  빈 슬롯의 가치는 마감이 다가올수록 떨어진다. 14:30에 상위 종목을 기다리는 건 현금을
//  들고 하루를 끝내는 것과 같아서, 그때는 바를 없애고 아무나 받는 게 맞다.
constexpr int kSessionOpenMin  = 9 * 60;   // 09:00 KST
constexpr int kSessionBarEndMin = 15 * 60; // 15:00 KST — 이후로는 바 없음

double session_remaining_ratio()
{
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    const int now_min = lt.tm_hour * 60 + lt.tm_min;
    if (now_min <= kSessionOpenMin)  return 1.0;
    if (now_min >= kSessionBarEndMin) return 0.0;
    return static_cast<double>(kSessionBarEndMin - now_min) /
           static_cast<double>(kSessionBarEndMin - kSessionOpenMin);
}
} // namespace

// ─── 주문 검증 ──────────────────────────────────────────────────────────────
// 주의(C6): check()는 항목별 뮤텍스를 독립 스코프로 잡아 호출 단위가 원자적이지 않다.
// 현재 호출자는 단일 order_thread(Engine::order_thread_fn → OrderRouter::submit)뿐이라
// check()+on_accept이 직렬 실행돼 검사~사용 사이 경합(TOCTOU, Time-Of-Check-To-Time-Of-Use)이
// 없다. 멀티 producer로 확장하려면
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

            // 3c-1. 교체 쿨다운 / 비운 슬롯 예약 — 교체가 켜져 있을 때만.
            //   쿨다운: 방금 밀려난 종목이 곧장 되돌아오면 교체 비용만 왕복으로 나간다.
            //   슬롯 예약: 교체로 비운 자리를 수혜 종목이 아닌 다른 종목이 가로채면
            //             매도 비용만 치르고 사려던 종목은 또 못 산다.
            if (cfg_.displace_enabled)
            {
                const auto now = Clock::now();
                std::string blocked_by;
                bool cooling = false;
                {
                    std::lock_guard<std::mutex> lk(displace_mtx_);
                    auto cd = displace_cooldown_.find(sig.ticker);
                    if (cd != displace_cooldown_.end())
                    {
                        if (now < cd->second) cooling = true;
                        else                  displace_cooldown_.erase(cd);
                    }
                    if (!cooling && !slot_reserved_for_.empty())
                    {
                        if (now >= slot_reserved_until_)
                        {
                            slot_reserved_for_.clear();
                        }
                        else if (slot_reserved_for_ == sig.ticker)
                        {
                            slot_reserved_for_.clear(); // 수혜 종목이 자리를 가져갔다
                        }
                        else
                        {
                            blocked_by = slot_reserved_for_;
                        }
                    }
                }
                if (cooling)
                {
                    reject_reason = "교체 쿨다운 중 — 방금 슬롯을 내준 종목의 재진입 금지";
                    return false;
                }
                if (!blocked_by.empty())
                {
                    reject_reason = "교체로 비운 슬롯 예약분 (" + blocked_by + ") — 다른 종목 진입 보류";
                    return false;
                }
            }

            // 3c-2. 점수 우선순위 바 — 남은 슬롯이 적을수록 더 높은 점수를 요구한다.
            //   rank/total ≤ 1 − (open/slots) × decay(t)
            //  슬롯이 비어 있으면 아무나 통과하고, 마지막 칸에 가까울수록 상위만 남는다.
            //  랭크를 모르는 종목(스캔 유니버스 밖 보유분 가디언 등)은 바를 적용하지 않는다.
            if (cfg_.entry_priority_enabled)
            {
                int  rank = 0, total = 0;
                {
                    std::lock_guard<std::mutex> lk(prio_mtx_);
                    total = entry_total_;
                    auto it = entry_rank_.find(sig.ticker);
                    if (it != entry_rank_.end()) rank = it->second;
                }
                if (rank > 0 && total > 0)
                {
                    const double occupancy = static_cast<double>(open) /
                                             static_cast<double>(cfg_.max_concurrent_positions);
                    const double bar   = 1.0 - occupancy * session_remaining_ratio();
                    const double quant = static_cast<double>(rank) / static_cast<double>(total);
                    if (quant > bar)
                    {
                        std::ostringstream ss;
                        ss << "점수 우선순위 미달 (랭크 " << rank << "/" << total
                           << " = " << std::fixed << std::setprecision(2) << quant
                           << " > 기준 " << bar << ", 슬롯 " << open << "/"
                           << cfg_.max_concurrent_positions << ") — 더 높은 점수 종목을 위해 보류";
                        reject_reason = ss.str();
                        return false;
                    }
                }
            }
        }

        // 3d. 포트폴리오 총노출 상한 — 모든 종목 보유(positions_×평단)+미체결 선점(reserved_×선점가) 합이
        //     자본의 max_gross_exposure_pct를 넘게 만드는 BUY를 차단(신규·물타기 공통). 청산(SELL)은 위에서 제외.
        //     종목당 명목(15%)×동시보유(10)=150% 같은 과노출을 총합 단에서 막는다. equity 미주입(0)이면 비활성.
        //     보유분은 원가(평단)로, 분모 equity는 시장 총평가금이라 상승장 과소·하락장 과대의 근사(수용).
        const double equity = equity_.load(std::memory_order_relaxed);
        if (cfg_.max_gross_exposure_pct > 0.0 && equity > 0.0 && eval_px > 0.0)
        {
            double gross = 0.0;
            for (const auto& kv : positions_)
            {
                if (kv.second <= 0) continue;
                auto ap = avg_prices_.find(kv.first);
                gross += kv.second * (ap != avg_prices_.end() ? ap->second : 0.0);
            }
            for (const auto& kv : reserved_)
            {
                if (kv.second <= 0) continue; // BUY 선점(+)만 노출 증가. SELL 선점(-)은 축소라 보수적으로 무시
                auto pp = reserved_px_.find(kv.first);
                gross += kv.second * (pp != reserved_px_.end() ? pp->second : 0.0);
            }
            const double cap        = cfg_.max_gross_exposure_pct * equity;
            const double next_gross = gross + sig.quantity * eval_px;
            if (next_gross > cap)
            {
                std::ostringstream ss;
                ss << "총노출 한도 초과 (" << static_cast<long long>(next_gross) << " > "
                   << static_cast<long long>(cap) << " = 자본 " << static_cast<long long>(equity)
                   << "×" << cfg_.max_gross_exposure_pct << ") — 신규 매수 정지(청산 허용)";
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
    {
        reserved_.erase(k);
        reserved_px_.erase(k);      // 선점이 해소되면 선점가도 정리(§3d 명목이 남아 부풀지 않게)
    }
    else
    {
        reserved_[k] = next;
        if (price > 0.0)
            reserved_px_[k] = price; // 최신 선점가 기록. 시장가(0)면 유지(직전 값)해 총노출 근사 보존
    }
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
    // 잔고 대조(리컨사일)가 reserved_를 비운 뒤 온 취소 통보는 대상이 이미 없으므로 아무 것도 하지 않는다.
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
    {
        reserved_.erase(k);
        reserved_px_.erase(k);
    }
    else
        reserved_[k] = r;
}

// ─── 선점 전면 초기화 (REST 리컨사일 전용) ──────────────────────────────────
void OrderGate::reset_reserved()
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    reserved_.clear();
    reserved_px_.clear();
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
    // 기동 시드는 "오늘 산 것"이 아니다. 최소 보유 시간 판정에서 즉시 교체 대상이 되도록
    //  과거 시각으로 찍는다(전일 물린 보유분을 15분 붙잡아 둘 이유가 없다).
    opened_at_[k] = Clock::now() - std::chrono::hours(24);
}

// ─── 체결 확인 — avg_price 재계산 + 실현손익 적립 ──────────────────────────
OrderGate::FillResult OrderGate::on_fill_confirmed(
    const std::string& account, const std::string& ticker, OrderSide side, int qty, double price)
{
    FillResult result;
    result.commission = price * qty * kCommissionRate;                              // 수수료 0.015%
    result.tax        = (side == OrderSide::SELL) ? price * qty * kSellTaxRate : 0.0; // 거래세 매도만

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
            if (pre_qty <= 0)
                opened_at_[k] = Clock::now(); // 0에서 열린 시각 — 교체 최소 보유 판정 기준
            result.avg_price = avg_prices_[k];
            result.net_qty   = new_qty;

            // 선점 해제 (BUY 선점은 +였으므로 -qty)
            int r = (reserved_.count(k) ? reserved_[k] : 0) - qty;
            if (r == 0) { reserved_.erase(k); reserved_px_.erase(k); } else reserved_[k] = r;
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
                opened_at_.erase(k);
            }
            else
                positions_[k] = new_qty;

            // 선점 해제 (SELL 선점은 -였으므로 +qty)
            int r = (reserved_.count(k) ? reserved_[k] : 0) + qty;
            if (r == 0) { reserved_.erase(k); reserved_px_.erase(k); } else reserved_[k] = r;
        }
    }

    if (side == OrderSide::SELL)
        add_realized_pnl(result.realized_pnl);

    return result;
}

// ─── 교체 진입 ──────────────────────────────────────────────────────────────
//  슬롯이 꽉 찼을 때 "먼저 온 순서"가 하루 종일 자리를 지키는 것을 막는다.
//  락 순서: prio_mtx_ → displace_mtx_ → positions_mtx_ 를 겹치지 않고 차례로 잡는다
//  (헤더의 중첩 금지 규약 유지 — 각 구간에서 필요한 값만 복사해 나온다).
bool OrderGate::slots_full() const
{
    if (cfg_.max_concurrent_positions <= 0)
        return false;
    std::lock_guard<std::mutex> lk(positions_mtx_);
    size_t open = 0;
    for (const auto& kv : positions_)
        if (kv.second > 0) ++open;
    for (const auto& kv : reserved_)
        if (kv.second > 0)
        {
            auto it = positions_.find(kv.first);
            if (it == positions_.end() || it->second <= 0) ++open;
        }
    return open >= static_cast<size_t>(cfg_.max_concurrent_positions);
}

OrderGate::DisplacePlan OrderGate::plan_displacement(const std::string& account,
                                                    const std::string& new_ticker) const
{
    DisplacePlan plan;
    if (!cfg_.displace_enabled || cfg_.max_concurrent_positions <= 0)
        return plan;

    // (1) 신규 종목의 점수. 점수를 모르면 교체 근거가 없다.
    std::unordered_map<std::string, double> z;
    {
        std::lock_guard<std::mutex> lk(prio_mtx_);
        z = entry_z_;
    }
    auto zn = z.find(new_ticker);
    if (zn == z.end())
        return plan;
    plan.new_z = zn->second;

    // (2) 당일 교체 횟수·슬롯 예약 상태. 이미 비워 둔 슬롯이 있으면 또 비우지 않는다.
    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lk(displace_mtx_);
        if (cfg_.displace_max_per_day > 0 && displace_count_ >= cfg_.displace_max_per_day)
            return plan;
        if (!slot_reserved_for_.empty() && now < slot_reserved_until_)
            return plan; // 직전 교체로 비운 자리가 아직 안 찼다
        auto cd = displace_cooldown_.find(new_ticker);
        if (cd != displace_cooldown_.end() && now < cd->second)
            return plan; // 방금 밀려난 종목이 곧장 되돌아오는 핑퐁 차단
    }

    // (3) 보유분 중 최약체. 점수를 아는 종목만 대상 — 스캔 유니버스 밖 보유분(가디언 관리,
    //     전일 물린 물량)은 이 판정의 모집단이 아니다. 점수가 없는 것과 낮은 것은 다르다.
    std::string best_key;
    double      worst_z = 0.0;
    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        for (const auto& kv : positions_)
        {
            if (kv.second <= 0)
                continue;
            const std::string& key = kv.first;
            auto colon = key.find(':');
            if (colon == std::string::npos) continue;
            int n = 0;
            try { n = std::stoi(key.substr(0, colon)); } catch (...) { continue; }
            const std::string rest = key.substr(colon + 1);
            if (n < 0 || static_cast<size_t>(n) > rest.size()) continue;
            const std::string tk = rest.substr(n);
            if (tk == new_ticker)
                continue;

            auto zi = z.find(tk);
            if (zi == z.end())
                continue; // 점수 미상 — 교체 대상 아님

            // 미체결 매도가 이미 걸린 종목은 건드리지 않는다(중복 매도).
            auto rv = reserved_.find(key);
            const int sell_pending = (rv != reserved_.end() && rv->second < 0) ? -rv->second : 0;
            if (sell_pending >= kv.second)
                continue;

            // 방금 산 종목은 빼지 않는다. 사자마자 파는 왕복은 비용만 남는다.
            auto oi = opened_at_.find(key);
            if (oi != opened_at_.end() && cfg_.displace_min_hold_sec > 0 &&
                now - oi->second < std::chrono::seconds(cfg_.displace_min_hold_sec))
                continue;

            if (best_key.empty() || zi->second < worst_z)
            {
                best_key = key;
                worst_z  = zi->second;
            }
        }

        if (best_key.empty())
            return plan;
        if (plan.new_z - worst_z < cfg_.displace_min_z_gap)
            return plan; // 격차가 잡음 수준이면 비용만 나간다

        auto colon = best_key.find(':');
        int  n     = std::stoi(best_key.substr(0, colon));
        const std::string rest = best_key.substr(colon + 1);
        plan.account = rest.substr(0, n);
        plan.ticker  = rest.substr(n);
        auto pit = positions_.find(best_key);
        auto rv  = reserved_.find(best_key);
        const int sell_pending = (rv != reserved_.end() && rv->second < 0) ? -rv->second : 0;
        plan.qty = (pit != positions_.end() ? pit->second : 0) - sell_pending;
        auto ap = avg_prices_.find(best_key);
        plan.avg_price = (ap != avg_prices_.end()) ? ap->second : 0.0;
    }

    if (plan.qty <= 0)
        return plan;

    (void)account; // 계좌는 피교체 종목 쪽에서 복원한다(신호 계좌와 다를 수 있음)
    plan.victim_z = worst_z;
    std::ostringstream ss;
    ss << "교체 진입 — " << new_ticker << "(z=" << std::fixed << std::setprecision(2) << plan.new_z
       << ")가 " << plan.ticker << "(z=" << plan.victim_z << ")보다 "
       << (plan.new_z - plan.victim_z) << "σ 높아 슬롯을 넘긴다";
    plan.reason = ss.str();
    plan.ok = true;
    return plan;
}

void OrderGate::note_displacement(const DisplacePlan& plan, const std::string& beneficiary)
{
    if (!plan.ok)
        return;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lk(displace_mtx_);
    ++displace_count_;
    if (cfg_.displace_cooldown_sec > 0)
        displace_cooldown_[plan.ticker] = now + std::chrono::seconds(cfg_.displace_cooldown_sec);
    // 비운 슬롯을 수혜 종목에 예약한다. 예약이 없으면 매도 체결 직후 다른 종목이 가로채고,
    //  그러면 교체 비용만 치르고 정작 사려던 종목은 또 못 산다.
    slot_reserved_for_   = beneficiary;
    slot_reserved_until_ = now + std::chrono::seconds(cfg_.displace_slot_hold_sec > 0
                                                     ? cfg_.displace_slot_hold_sec : 120);
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
        std::lock_guard<std::mutex> lk(displace_mtx_);
        displace_cooldown_.clear();
        slot_reserved_for_.clear();
        slot_reserved_until_ = TimePoint{};
        displace_count_ = 0;
    }
    {
        // 미체결 선점은 일일 만료 (KIS 당일 주문은 EOD 소멸 → 다음날 잘못된 차단 방지).
        // C5(MM-1): 명시적 취소는 on_cancel()로 일원화. reserved_.clear()는 EOD 안전망
        //   — 취소 없이 장 마감까지 미체결로 만료된 분의 선점을 청소한다.
        std::lock_guard<std::mutex> lk(positions_mtx_);
        reserved_.clear();
        reserved_px_.clear();
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
