#include "risk/OrderGate.h"
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>

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

    if (now_min <= kSessionOpenMin)
    {
        return 1.0;
    }

    if (now_min >= kSessionBarEndMin)
    {
        return 0.0;
    }

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
// ─── 한도 클램프 (BUY NEW) ────────────────────────────────────────────────
//  check()가 쓰는 것과 같은 한도식을 "얼마까지 되나"로 뒤집어 푼다. 두 곳의 식이 어긋나면
//  클램프한 수량이 다시 거부되므로, 항목·평가가(eval_px)·합산 기준(positions_+reserved_)을
//  check()와 똑같이 맞춘다.
int OrderGate::clamp_buy_qty(const OrderSignal& sig)
{
    int q = sig.quantity;

    // ── SELL NEW: 매도가능수량(보유 - 미체결매도) 클램프 ────────────────────
    //  자기가 낸 익절 지정가가 자기 청산을 막는다. 그대로 내면 KIS가 40240000
    //  (주문가능분 없음)으로 주문을 통째로 거부해 한 주도 못 빠져나온다(09-08 047050:
    //  254주·381주 두 번 다 전량 거부, 보유분이 갇혔다). 나갈 수 있는 만큼이라도
    //  내보내는 편이 낫다. 이미 FORCE_LIQ와 디스플레이스먼트는 같은 식으로 깎고 있고,
    //  전략 청산 신호만 이 경로를 안 거치고 있었다.
    //  원장이 그 종목을 모를 때(positions_ 없음 또는 0)는 손대지 않는다 - 과소 인식으로
    //  정당한 청산을 0주로 깎는 쪽이 거부당하는 것보다 위험하다.
    if (sig.side == OrderSide::SELL && sig.action == OrderAction::NEW && q > 0)
    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        const PosKey k = make_key(sig.account_id, sig.ticker);
        auto pit = positions_.find(k);

        if (pit == positions_.end() || pit->second <= 0)
        {
            return q;
        }

        auto rit = reserved_.find(k);
        const int sell_pending = (rit != reserved_.end() && rit->second < 0) ? -rit->second : 0;
        // 상한은 보유수량이 아니라 매도가능수량이다. 기동 전 세션이 남긴 미체결 매도는
        //  reserved_에 없고(프로세스 메모리라 재기동으로 사라진다) 잔고의 ord_psbl_qty에만 보인다.
        int cap = pit->second;
        auto sit = sellable_.find(k);

        if (sit != sellable_.end() && sit->second < cap)
        {
            cap = sit->second;
        }

        const int sellable = cap - sell_pending;

        // 이 파일은 Logger에 의존하지 않는다(게이트 단위 테스트가 단독 링크한다).
        //  클램프가 실제로 걸리면 호출부 OrderRouter가 "한도 클램프" 한 줄을 남긴다.
        if (sellable <= 0)
        {
            return 0;
        }

        return q > sellable ? sellable : q;
    }

    if (sig.side != OrderSide::BUY || sig.action != OrderAction::NEW || q <= 0)
    {
        return q;
    }

    // 지정가는 price, 시장가(0)는 ref_price. 둘 다 없으면 명목을 못 재므로 수량 한도만 건다.
    const double eval_px = sig.price > 0.0 ? sig.price : sig.ref_price;

    if (cfg_.max_qty_per_order > 0 && q > cfg_.max_qty_per_order)
    {
        q = cfg_.max_qty_per_order;
    }

    if (eval_px > 0.0 && cfg_.max_notional_per_order > 0.0)
    {
        const int cap = static_cast<int>(cfg_.max_notional_per_order / eval_px);

        if (cap < q)
        {
            q = cap;
        }
    }

    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        const PosKey k = make_key(sig.account_id, sig.ticker);
        auto pit = positions_.find(k);
        auto rit = reserved_.find(k);
        const int cur_qty = (pit != positions_.end() ? pit->second : 0) +
                            (rit != reserved_.end() ? rit->second : 0);

        if (cfg_.max_qty_per_ticker > 0)
        {
            const int room = cfg_.max_qty_per_ticker - cur_qty;

            if (room < q)
            {
                q = room;
            }
        }

        if (eval_px > 0.0 && cfg_.max_notional_per_ticker > 0.0)
        {
            const int room = static_cast<int>(cfg_.max_notional_per_ticker / eval_px) - cur_qty;

            if (room < q)
            {
                q = room;
            }
        }

        // 총노출(§3d)도 같은 방식으로 남은 여유를 수량으로 환산한다. 보유는 평단, 선점은 선점가로
        //  재는 것까지 check()와 동일하게 둔다.
        const double equity = equity_.load(std::memory_order_relaxed);

        if (cfg_.max_gross_exposure_pct > 0.0 && equity > 0.0 && eval_px > 0.0)
        {
            double gross = 0.0;

            for (const auto& kv : positions_)
            {
                if (kv.second <= 0)
                {
                    continue;
                }

                auto ap = avg_prices_.find(kv.first);
                gross += kv.second * (ap != avg_prices_.end() ? ap->second : 0.0);
            }

            for (const auto& kv : reserved_)
            {
                if (kv.second <= 0)
                {
                    continue;
                }

                auto pp = reserved_px_.find(kv.first);
                gross += kv.second * (pp != reserved_px_.end() ? pp->second : 0.0);
            }

            const double cap  = cfg_.max_gross_exposure_pct * equity;
            const int    room = static_cast<int>((cap - gross) / eval_px);

            if (room < q)
            {
                q = room;
            }
        }

        // 주문가능현금 클램프. 평가금이 아니라 현금이 매수의 진짜 상한이다. 우리가 이미 낸
        //  미체결 매수 명목을 빼는 것은 보수적으로 중복차감이 될 수 있으나(브로커 값이 이미
        //  반영했을 수 있다), 모자라게 사는 쪽이 전량 거부보다 낫다.
        const double cash = available_cash_.load(std::memory_order_relaxed);

        if (cash > 0.0 && eval_px > 0.0)
        {
            double pending_buy = 0.0;

            for (const auto& kv : reserved_)
            {
                if (kv.second <= 0)
                {
                    continue;
                }

                auto pp = reserved_px_.find(kv.first);
                pending_buy += kv.second * (pp != reserved_px_.end() ? pp->second : 0.0);
            }

            const int room = static_cast<int>((cash - pending_buy) / eval_px);

            if (room < q)
            {
                q = room;
            }
        }
    }

    return q > 0 ? q : 0;
}

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
    //     지수 급락 시 "신규정지 + 보유분 청산"이 게이트에서 미완료로 남지 않게 한다(C-2).
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

            // SELL은 청산 계열이라 거부하지 않는다 — 정당한 청산을 막는 쪽이 대량 매도보다 위험하다.
            //  (수량 한도는 위에서 이미 걸렸다.) 이 파일은 Logger를 안 쓰므로 stderr 한 줄.
            if (sig.side == OrderSide::SELL)
            {
                std::cerr << "[OrderGate] WARN " << sig.ticker << " SELL " << ss.str() << " — 청산이라 통과\n";
            }
            else
            {
                reject_reason = ss.str();
                return false;
            }
        }
    }

    // 3. 포지션 수량 한도 (BUY에만 적용) — 실체결(positions_) + 미체결 선점(reserved_) 합산
    //    계좌별 파티션 — 한 계좌 한도는 다른 계좌 주문을 막지 않는다.
    if (sig.side == OrderSide::BUY)
    {
        std::lock_guard<std::mutex> lk(positions_mtx_);
        const PosKey k = make_key(sig.account_id, sig.ticker);
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
            size_t held = 0;   // 그중 실보유. 거부 문구에서 유령 선점과 갈라 보려고 따로 센다

            for (const auto& kv : positions_)
            {
                if (kv.second > 0)
                {
                    ++open;
                    ++held;
                }
            }

            for (const auto& kv : reserved_)
            {
                if (kv.second > 0)
                {
                    auto it = positions_.find(kv.first);

                    if (it == positions_.end() || it->second <= 0)
                    {
                        ++open;  // 예약만 있는 종목(중복 제외)
                    }
                }
            }

            if (open >= static_cast<size_t>(cfg_.max_concurrent_positions))
            {
                std::ostringstream ss;
                ss << "동시 보유 종목 한도 초과 (" << open << " >= "
                   << cfg_.max_concurrent_positions << ", 실보유 " << held
                   << " 선점만 " << (open - held) << ")";

                // 교체 진입이 켜져 있으면 여기 오는 BUY는 교체 판정에서 떨어진 것이다. 그 사유를
                //  같이 적지 않으면 한도 문구만 남아 "교체가 안 도는 것"으로 읽힌다(09-11 11:21).
                std::string decline;
                {
                    // [lock-order] positions_mtx_ → displace_mtx_. 반대 순서로 겹쳐 잡는 곳은 없다
                    //  (plan_displacement·note_displacement는 displace_mtx_를 단독 구간으로만 쓴다).
                    std::lock_guard<std::mutex> dl(displace_mtx_);
                    auto di = displace_decline_.find(sig.ticker);

                    if (di != displace_decline_.end())
                    {
                        decline = di->second;
                    }
                }

                if (!decline.empty())
                {
                    ss << " — 교체 보류: " << decline;
                }
                else
                {
                    ss << " — 신규 종목 진입 정지";
                }

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
                        if (now < cd->second)
                        {
                            cooling = true;
                        }
                        else
                        {
                            displace_cooldown_.erase(cd);
                        }
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
            //  랭크를 모르는 종목(스캔 유니버스 밖 보유분 청산 관리 등)은 바를 적용하지 않는다.
            if (cfg_.entry_priority_enabled)
            {
                int  rank = 0, total = 0, eff_rank = 0;
                {
                    std::lock_guard<std::mutex> lk(prio_mtx_);
                    total = entry_total_;
                    auto it = entry_rank_.find(sig.ticker);

                    if (it != entry_rank_.end())
                    {
                        rank = it->second;
                    }

                    // 유효 랭크 = 나보다 점수가 높으면서 "아직 슬롯을 안 차지한" 종목 수 + 1.
                    //  전체 랭크를 그대로 쓰면 상위 랭크를 이미 보유한 순간 남은 후보는
                    //  구조적으로 기준선을 넘을 수 없다(보유 23/25에서 최상위 후보의 랭크가
                    //  24위라 quant=0.93 > 기준 0.54). 그러면 슬롯이 25에 닿지 못하고,
                    //  교체 진입은 capacity_full()에서만 열리므로 둘 다 영원히 막힌다.
                    //  살 수 있는 종목들 사이의 순위로 재면 최상위 후보는 항상 1위가 된다.
                    if (rank > 0)
                    {
                        int ahead = 0;

                        for (const auto& kv : entry_rank_)
                        {
                            if (kv.second >= rank || kv.first == sig.ticker)
                            {
                                continue;
                            }

                            // positions_/reserved_는 계좌 합성키를 쓴다(make_key). entry_rank_는
                            //  순수 티커라 그대로 조회하면 언제나 미보유로 잡혀 유효 랭크가
                            //  전체 랭크와 같아진다.
                            const PosKey hk = make_key(sig.account_id, kv.first);
                            auto ip = positions_.find(hk);
                            auto ir = reserved_.find(hk);
                            const bool taken = (ip != positions_.end() && ip->second > 0) ||
                                               (ir != reserved_.end() && ir->second > 0);

                            if (!taken)
                            {
                                ++ahead;
                            }
                        }

                        eff_rank = ahead + 1;
                    }
                }

                if (rank > 0 && total > 0)
                {
                    const double occupancy = static_cast<double>(open) /
                                             static_cast<double>(cfg_.max_concurrent_positions);
                    const double bar   = 1.0 - occupancy * session_remaining_ratio();
                    // 분모는 슬롯이 경합하는 모집단이다. 등록 종목이 슬롯보다 적으면 경합 자체가
                    //  없는데도 rank/total 이 1.0에 붙어 하위 랭크가 영구 차단된다(등록 12 vs 슬롯 25
                    //  이면 허용 랭크가 8에서 멈춰 슬롯의 1/3만 채운 채 하루가 끝난다). 모집단을
                    //  최소 슬롯 수로 받쳐, 풀이 슬롯보다 클 때의 동작은 그대로 두고 작을 때만 푼다.
                    const int    pool  = total > cfg_.max_concurrent_positions
                                             ? total : cfg_.max_concurrent_positions;
                    const double quant = static_cast<double>(eff_rank) / static_cast<double>(pool);

                    if (quant > bar)
                    {
                        std::ostringstream ss;
                        ss << "점수 우선순위 미달 (랭크 " << rank << " 유효 " << eff_rank
                           << "/" << pool
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
                if (kv.second <= 0)
                {
                    continue;
                }

                auto ap = avg_prices_.find(kv.first);
                gross += kv.second * (ap != avg_prices_.end() ? ap->second : 0.0);
            }

            for (const auto& kv : reserved_)
            {
                if (kv.second <= 0)
                {
                    continue;  // BUY 선점(+)만 노출 증가. SELL 선점(-)은 축소라 보수적으로 무시
                }

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

    // 4b. PnL stale guard (B2) — daily_pnl_이 낡으면(잔고 대조 연속 정체) §4 손실컷을
    //     신뢰할 수 없어 BUY NEW만 보수적으로 정지. SELL 청산·BUY 취소/정정은 통과시켜
    //     "신규 위험만 억제, 탈출은 허용"(entry_halt와 동일 의미론). Engine이 잔고조회 복구 시 해제.
    if (pnl_stale_.load() && sig.side == OrderSide::BUY && sig.action == OrderAction::NEW)
    {
        reject_reason = "PNL_STALE — 잔고 대조 정체(daily_pnl 미갱신), 신규 진입 보수적 정지";
        return false;
    }

    // 5. 중복 신호 제거 — rate 소비 전에 검사해 중복이 rate slot을 소모하지 않게 함.
    //    키에 side 포함(MM-1): 시장조성은 같은 틱에 동일 strategy+ticker로 BUY(bid)+SELL(ask)를
    //    동시 발주한다. side가 없으면 두 번째(ask)가 중복 오거부된다. BUY/SELL은 다른 의도라
    //    중복이 아니다. (같은 side 반복은 여전히 dedup — 기존 전략 동작 불변)
    //    스탬프(last_signal_)는 6절 rate 통과 뒤에 찍는다 — rate로 거부된 신호가 dedup 창을
    //    소모하면 창 안의 정당한 재시도까지 "중복"으로 막힌다(W-2).
    //    지정가는 가격까지 키에 넣는다 — 분할 매수는 같은 종목·같은 방향의 rung 여러 개를 한 틱에
    //    내는데, 주문 스레드가 1초 안에 연달아 처리하면 두 번째 rung부터 "중복"으로 잘렸다
    //    (09-11 10:04 232140 BUY 42@11790·42@11690 둘 다 거부). 같은 가격 반복만 중복이다.
    const std::string dedup_key = sig.account_id + ":" + sig.strategy_id + ":" + sig.ticker + ":" +
                                  std::to_string(static_cast<int>(sig.side)) +
                                  (sig.type == OrderType::LIMIT
                                       ? ":" + std::to_string(static_cast<long long>(sig.price))
                                       : std::string());
    {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lk(dedup_mtx_);
        auto it = last_signal_.find(dedup_key);

        if (it != last_signal_.end())
        {
            double elapsed = std::chrono::duration<double>(now - it->second).count();

            if (elapsed < cfg_.dedup_window_sec)
            {
                reject_reason = "중복 신호 (윈도우 " + std::to_string(cfg_.dedup_window_sec) + "초)";
                return false;
            }
        }
    }

    // 6. Rate limit — 초당 / 분당 두 단계 검사 (dedup 통과 후에만 카운터 소모)
    {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lk(rate_mtx_);

        // 초당 제한
        auto cutoff_sec = now - std::chrono::seconds(1);

        while (!order_times_sec_.empty() && order_times_sec_.front() < cutoff_sec)
        {
            order_times_sec_.pop_front();
        }

        if (static_cast<int>(order_times_sec_.size()) >= cfg_.max_orders_per_sec)
        {
            reject_reason = "Rate limit 초과 (초당 " + std::to_string(cfg_.max_orders_per_sec) + "건)";
            return false;
        }

        // 분당 제한
        auto cutoff_min = now - std::chrono::minutes(1);

        while (!order_times_min_.empty() && order_times_min_.front() < cutoff_min)
        {
            order_times_min_.pop_front();
        }

        if (static_cast<int>(order_times_min_.size()) >= cfg_.max_orders_per_min)
        {
            reject_reason = "Rate limit 초과 (분당 " + std::to_string(cfg_.max_orders_per_min) + "건)";
            return false;
        }

        order_times_sec_.push_back(now);
        order_times_min_.push_back(now);
    }

    // 모든 검사를 지난 신호만 dedup 창을 연다.
    {
        std::lock_guard<std::mutex> lk(dedup_mtx_);
        last_signal_[dedup_key] = Clock::now();
    }

    return true;
}

// ─── 접수 후 선점 (reserved_만 갱신, 실체결 원장 positions_는 불변) ──────────────
void OrderGate::on_accept(const std::string& account, const std::string& ticker,
                          OrderSide side, int qty, double price)
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    const PosKey k = make_key(account, ticker);
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
        {
            reserved_px_[k] = price; // 최신 선점가 기록. 시장가(0)면 유지(직전 값)해 총노출 근사 보존
        }
    }
}

// ─── 미체결 취소/정정 축소 시 선점 해제 (C5) ────────────────────────────────
//  on_fill_confirmed의 reserved 해제와 같은 방향. positions_/avg_price는 손대지 않는다
//  (취소는 체결이 아니므로 실보유·평단 불변). qty<=0이면 no-op(방어).
// 선점 해제 한 곳 — 취소 통보와 체결 통보가 같은 규칙을 쓰게 모았다. 규칙이 갈라져 있던 동안
//  on_cancel에만 가드가 있고 on_fill_confirmed에는 없어, 선점을 잡은 적 없는 포지션의 체결이
//  없던 선점을 만들어 냈다. delta는 해제 방향(BUY 선점 +는 -qty, SELL 선점 -는 +qty).
//  호출자가 positions_mtx_를 이미 쥐고 있다고 가정한다(여기서 다시 잡지 않는다).
void OrderGate::release_reservation(const PosKey& key, int delta)
{
    // 잔고 대조가 reserved_를 비운 뒤 온 통보는 대상이 이미 없으므로 아무 것도 하지 않는다.
    //  (없는 키를 갱신하면 부호가 뒤집힌 선점이 생겨 이후 한도·슬롯 계산이 왜곡됨)
    int cur = reserved_.count(key) ? reserved_[key] : 0;

    if (cur == 0)
    {
        return;
    }

    int r = cur + delta;

    // 과잉 해제(부호 역전) 시 0에서 정지 — 리셋·이중통보로 음수 선점이 남지 않게.
    if ((cur > 0 && r < 0) || (cur < 0 && r > 0))
    {
        r = 0;
    }

    if (r == 0)
    {
        reserved_.erase(key);
        reserved_px_.erase(key);
    }
    else
    {
        reserved_[key] = r;
    }
}

void OrderGate::on_cancel(const std::string& account, const std::string& ticker,
                          OrderSide side, int qty)
{
    if (qty <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(positions_mtx_);
    // BUY 선점은 +였으므로 -qty, SELL 선점은 -였으므로 +qty (해제 = 반대부호 가산)
    release_reservation(make_key(account, ticker), (side == OrderSide::BUY) ? -qty : qty);
}

// ─── 선점 전면 초기화 (REST 잔고 대조 전용) ──────────────────────────────────
void OrderGate::reset_reserved()
{
    std::lock_guard<std::mutex> lk(positions_mtx_);
    reserved_.clear();
    reserved_px_.clear();
}

// ─── 유령 슬롯 정리 ─────────────────────────────────────────────────────────
std::vector<std::string> OrderGate::prune_positions(const std::vector<std::string>& live_tickers,
                                                    int min_age_sec)
{
    std::vector<std::string> gone;
    const std::unordered_set<std::string> live(live_tickers.begin(), live_tickers.end());
    std::lock_guard<std::mutex> lk(positions_mtx_);
    const auto now = Clock::now();

    for (auto it = positions_.begin(); it != positions_.end();)
    {
        const std::string& tkr = it->first.ticker;

        if (it->second <= 0 || live.count(tkr))
        {
            ++it;
            continue;
        }

        // 방금 열린 포지션은 남긴다. 잔고 조회가 체결보다 먼저 떠난 왕복이면 잔고에 아직
        //  안 보이는데, 그걸 지우면 전략이 미보유로 읽고 같은 종목을 또 산다.
        auto oi = opened_at_.find(it->first);

        if (oi != opened_at_.end() && min_age_sec > 0 &&
            now - oi->second < std::chrono::seconds(min_age_sec))
        {
            ++it;
            continue;
        }

        gone.push_back(tkr);
        avg_prices_.erase(it->first);
        opened_at_.erase(it->first);
        sellable_.erase(it->first);
        it = positions_.erase(it);
    }

    return gone;
}

std::vector<std::string> OrderGate::prune_reservations(const std::vector<std::string>& live_tickers)
{
    std::vector<std::string> gone;
    const std::unordered_set<std::string> live(live_tickers.begin(), live_tickers.end());
    std::lock_guard<std::mutex> lk(positions_mtx_);

    for (auto it = reserved_.begin(); it != reserved_.end();)
    {
        const std::string& tkr = it->first.ticker;

        if (live.count(tkr))
        {
            ++it;
            continue;
        }

        gone.push_back(tkr);
        reserved_px_.erase(it->first);
        it = reserved_.erase(it);
    }

    return gone;
}

void OrderGate::restore_sellable(const std::string& account, const std::string& ticker, int qty)
{
    if (qty <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(positions_mtx_);
    const PosKey k = make_key(account, ticker);
    auto pit = positions_.find(k);

    if (pit == positions_.end() || pit->second <= 0)
    {
        return;   // 원장이 모르는 보유는 손대지 않는다 — 없는 매도가능수량을 만들어 낼 이유가 없다
    }

    auto sit = sellable_.find(k);
    const int cur = (sit != sellable_.end()) ? sit->second : 0;
    const int restored = cur + qty;
    sellable_[k] = (restored > pit->second) ? pit->second : restored;
}

OrderGate::SellableView OrderGate::sellable_view(const std::string& account, const std::string& ticker) const
{
    SellableView v;
    std::lock_guard<std::mutex> lk(positions_mtx_);
    const PosKey k = make_key(account, ticker);
    auto pit = positions_.find(k);

    if (pit == positions_.end() || pit->second <= 0)
    {
        return v;
    }

    v.held     = pit->second;
    v.psbl_cap = v.held;
    auto sit = sellable_.find(k);

    if (sit != sellable_.end() && sit->second < v.psbl_cap)
    {
        v.psbl_cap = sit->second;
    }

    auto rit = reserved_.find(k);
    v.pending = (rit != reserved_.end() && rit->second < 0) ? -rit->second : 0;
    return v;
}

void OrderGate::refresh_sellable(const std::string& account, const std::string& ticker, int ord_psbl_qty)
{
    if (ord_psbl_qty < 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(positions_mtx_);
    const PosKey k = make_key(account, ticker);
    auto pit = positions_.find(k);

    if (pit == positions_.end() || pit->second <= 0)
    {
        return;
    }

    auto rit = reserved_.find(k);
    const int sell_pending = (rit != reserved_.end() && rit->second < 0) ? -rit->second : 0;
    const int sellable     = ord_psbl_qty + sell_pending;
    sellable_[k] = (sellable > pit->second) ? pit->second : sellable;
}

int OrderGate::absorb_missed_sell(const std::string& account, const std::string& ticker, int balance_qty)
{
    if (balance_qty < 0)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lk(positions_mtx_);
    const PosKey k = make_key(account, ticker);
    auto pit = positions_.find(k);

    if (pit == positions_.end() || pit->second <= balance_qty)
    {
        missed_sell_seen_.erase(k);
        return 0;
    }

    const int diff = pit->second - balance_qty;
    auto rit = reserved_.find(k);
    const int sell_pending = (rit != reserved_.end() && rit->second < 0) ? -rit->second : 0;

    // 미체결 매도보다 큰 차이는 놓친 체결로 설명되지 않는다 — 손대지 않고 로그 관찰에 맡긴다.
    if (diff > sell_pending)
    {
        missed_sell_seen_.erase(k);
        return 0;
    }

    auto seen = missed_sell_seen_.find(k);

    if (seen == missed_sell_seen_.end() || seen->second != balance_qty)
    {
        missed_sell_seen_[k] = balance_qty; // 첫 관측 — 다음 대조에서 같으면 맞춘다
        return 0;
    }

    missed_sell_seen_.erase(k);
    pit->second = balance_qty;
    rit->second += diff;

    if (rit->second == 0)
    {
        reserved_.erase(rit);
    }

    auto si = sellable_.find(k);

    if (si != sellable_.end() && si->second > balance_qty)
    {
        si->second = balance_qty;
    }

    return diff;
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
void OrderGate::seed_position(const std::string& account, const std::string& ticker, int qty, double avg,
                              int sellable)
{
    if (qty <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(positions_mtx_);
    const PosKey k = make_key(account, ticker);
    positions_[k]  = qty;
    avg_prices_[k] = avg;
    // 매도가능수량. 모르면(-1) 보유수량으로 둔다 - 모르는 것을 0으로 두면 정당한 청산이 막힌다.
    sellable_[k] = (sellable >= 0 && sellable < qty) ? sellable : qty;
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
        const PosKey k = make_key(account, ticker);
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
            {
                opened_at_[k] = Clock::now(); // 0에서 열린 시각 — 교체 최소 보유 판정 기준
            }

            result.avg_price = avg_prices_[k];
            result.net_qty   = new_qty;

            // 당일 매수분은 당일 매도 가능하다.
            sellable_[k] = (sellable_.count(k) ? sellable_[k] : pre_qty) + qty;

            // 선점 해제 (BUY 선점은 +였으므로 -qty). on_cancel과 같은 가드를 둔다 —
            //  선점이 없는데 빼면 음수 선점이 생겨 이후 한도·슬롯 계산이 왜곡된다
            //  (잔고 재시드분처럼 게이트가 선점을 잡은 적 없는 포지션의 체결이 이 경로로 온다).
            release_reservation(k, -qty);
        }
        else // SELL
        {
            int new_qty = pre_qty - qty;

            if (new_qty < 0)
            {
                new_qty = 0;  // 공매도 미지원 — 보유 초과 매도는 0으로 클램프
            }

            // 평단 미상(원장이 종목을 모름·재기동 후 미시드)이면 손익을 계산할 수 없다.
            //  0으로 곱하면 매도대금 전액이 이익으로 적립되므로 0을 두고 플래그로 알린다.
            if (!avg_prices_.count(k) || cur_avg <= 0.0)
            {
                result.basis_unknown = true;
                result.realized_pnl  = 0.0;
            }
            else
            {
                result.realized_pnl = (price - cur_avg) * qty
                                      - result.commission - result.tax;
            }

            result.avg_price = cur_avg; // SELL 후 평균단가 불변
            result.net_qty   = new_qty;

            if (new_qty == 0)
            {
                positions_.erase(k);
                avg_prices_.erase(k); // 포지션 청산 시 평균단가 초기화
                opened_at_.erase(k);
                sellable_.erase(k);
            }
            else
            {
                positions_[k] = new_qty;
                int sv = (sellable_.count(k) ? sellable_[k] : pre_qty) - qty;
                sellable_[k] = sv > 0 ? sv : 0;
            }

            // 선점 해제 (SELL 선점은 -였으므로 +qty). 가드가 없으면 선점이 없던 종목의
            //  매도 체결이 reserved_[k] = +qty를 만들어 내고, slots_full()이 포지션도 없는
            //  종목의 슬롯을 점유로 세어 비운 자리가 그날 내내 열리지 않는다.
            release_reservation(k, qty);
        }
    }

    if (side == OrderSide::SELL && !result.basis_unknown)
    {
        add_realized_pnl(result.realized_pnl);
    }

    return result;
}

// ─── 교체 진입 ──────────────────────────────────────────────────────────────
//  슬롯이 꽉 찼을 때 "먼저 온 순서"가 하루 종일 자리를 지키는 것을 막는다.
//  락 순서: prio_mtx_ → displace_mtx_ → positions_mtx_ 를 겹치지 않고 차례로 잡는다
//  (check()의 한도 거부 문구만 positions_mtx_ 안에서 displace_mtx_를 읽는다 — 역순 중첩 금지)
//  (헤더의 중첩 금지 규약 유지 — 각 구간에서 필요한 값만 복사해 나온다).
bool OrderGate::slots_full() const
{
    if (cfg_.max_concurrent_positions <= 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lk(positions_mtx_);
    size_t open = 0;

    for (const auto& kv : positions_)
    {
        if (kv.second > 0)
        {
            ++open;
        }
    }

    for (const auto& kv : reserved_)
    {
        if (kv.second > 0)
        {
            auto it = positions_.find(kv.first);

            if (it == positions_.end() || it->second <= 0)
            {
                ++open;
            }
        }
    }

    return open >= static_cast<size_t>(cfg_.max_concurrent_positions);
}

// 신규 종목을 열 여력이 없는가. 교체 진입이 슬롯만 보면, 슬롯은 남았는데 총노출 상한에
//  닿아 매수가 전부 거부되는 상태에서 더 좋은 종목이 와도 최약체를 비우지 못한다.
//  2026-09-08 실측: 보유 12/25종목(슬롯 여유), 총노출 97.4M > 상한 96.0M, 교체 0회.
bool OrderGate::capacity_full() const
{
    if (slots_full())
    {
        return true;
    }

    const double equity = equity_.load(std::memory_order_relaxed);

    if (cfg_.max_gross_exposure_pct <= 0.0 || equity <= 0.0)
    {
        return false;
    }

    double gross = 0.0;
    {
        std::lock_guard<std::mutex> lk(positions_mtx_);

        for (const auto& kv : positions_)
        {
            if (kv.second <= 0)
            {
                continue;
            }

            auto ap = avg_prices_.find(kv.first);
            gross += kv.second * (ap != avg_prices_.end() ? ap->second : 0.0);
        }

        for (const auto& kv : reserved_)
        {
            if (kv.second <= 0)
            {
                continue;
            }

            auto pp = reserved_px_.find(kv.first);
            gross += kv.second * (pp != reserved_px_.end() ? pp->second : 0.0);
        }
    }

    // 상한의 95%를 넘으면 여력 없음으로 본다. 정확히 상한에 닿기를 기다리면 한 종목분
    //  명목이 애매하게 남아 교체도 매수도 안 되는 구간이 생긴다.
    return gross >= cfg_.max_gross_exposure_pct * equity * 0.95;
}

OrderGate::DisplacePlan OrderGate::plan_displacement(const std::string& account,
                                                    const std::string& new_ticker) const
{
    DisplacePlan plan;

    // 거절 사유는 한 곳에서 기록한다(뒤의 거부 문구가 읽는다). 락 안에서는 부르지 않는다.
    auto decline = [&](const std::string& why) -> DisplacePlan
    {
        plan.reason = why;
        std::lock_guard<std::mutex> lk(displace_mtx_);
        displace_decline_[new_ticker] = why;
        return plan;
    };

    if (!cfg_.displace_enabled || cfg_.max_concurrent_positions <= 0)
    {
        return plan;
    }

    // (1) 신규 종목의 점수. 점수를 모르면 교체 근거가 없다.
    std::unordered_map<std::string, double> z;
    {
        std::lock_guard<std::mutex> lk(prio_mtx_);
        z = entry_z_;
    }

    auto zn = z.find(new_ticker);

    if (zn == z.end())
    {
        return decline("신규 종목 점수 없음");
    }

    plan.new_z = zn->second;

    // (2) 당일 교체 횟수·슬롯 예약 상태. 이미 비워 둔 슬롯이 있으면 또 비우지 않는다.
    const auto now = Clock::now();
    std::string why;
    {
        std::lock_guard<std::mutex> lk(displace_mtx_);

        if (cfg_.displace_max_per_day > 0 && displace_count_ >= cfg_.displace_max_per_day)
        {
            why = "당일 교체 횟수 " + std::to_string(displace_count_) + "/" +
                  std::to_string(cfg_.displace_max_per_day) + " 소진";
        }
        else if (!slot_reserved_for_.empty() && now < slot_reserved_until_)
        {
            // 직전 교체로 비운 자리가 아직 안 찼다
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(slot_reserved_until_ - now).count();
            why = "비운 자리를 " + slot_reserved_for_ + "가 쓰는 중(" + std::to_string(left) + "초 남음)";
        }
        else
        {
            auto cd = displace_cooldown_.find(new_ticker);

            if (cd != displace_cooldown_.end() && now < cd->second)
            {
                // 방금 밀려난 종목이 곧장 되돌아오는 핑퐁 차단
                const auto left = std::chrono::duration_cast<std::chrono::seconds>(cd->second - now).count();
                why = "밀려난 종목 재진입 대기(" + std::to_string(left) + "초 남음)";
            }
        }
    }

    if (!why.empty())
    {
        return decline(why);
    }

    // (3) 보유분 중 최약체. 점수를 아는 종목만 대상 — 스캔 유니버스 밖 보유분(청산 관리,
    //     전일 물린 물량)은 이 판정의 모집단이 아니다. 점수가 없는 것과 낮은 것은 다르다.
    PosKey best_key;
    double worst_z = 0.0;
    {
        std::lock_guard<std::mutex> lk(positions_mtx_);

        for (const auto& kv : positions_)
        {
            if (kv.second <= 0)
            {
                continue;
            }

            const PosKey&      key = kv.first;
            const std::string& tk  = key.ticker;

            if (tk == new_ticker)
            {
                continue;
            }

            auto zi = z.find(tk);
            double cand_z = 0.0;

            if (zi != z.end())
            {
                cand_z = zi->second;
            }
            else if (cfg_.displace_unscored_z != 0.0)
            {
                // 오늘 어느 슬리브의 스캔에도 안 잡힌 보유분. 점수가 없는 게 아니라 오늘 필터를
                //  통과하지 못한 것이라, 통과한 종목들 아래로 내려 교체 후보에 넣는다.
                //  이 경로가 없으면 전일 이월분이 슬롯을 영구히 점유한다.
                cand_z = cfg_.displace_unscored_z;
            }
            else
            {
                continue; // 점수 미상 — 교체 대상 아님(기존 동작)
            }

            // 미체결 매도가 이미 걸린 종목은 건드리지 않는다(중복 매도).
            auto rv = reserved_.find(key);
            const int sell_pending = (rv != reserved_.end() && rv->second < 0) ? -rv->second : 0;

            if (sell_pending >= kv.second)
            {
                continue;
            }

            // 이전 세션이 낸 미체결 매도는 reserved_에 없다 — 프로세스 메모리라 재기동으로
            //  사라졌다. 그 수량은 잔고 시드(ord_psbl_qty)를 거쳐 sellable_에만 남으므로
            //  여기서도 같이 본다. 안 보면 팔 수 없는 종목을 매번 최약체로 골라 교체가 헛돈다
            //  (09-09: 000215 4회, 001120 1회. 그동안 진짜 팔 수 있는 하위 종목은 그대로 있었다).
            //  상한 계산은 clamp_buy_qty의 SELL 분기와 같은 규칙을 쓴다.
            int cand_cap = kv.second;
            auto si = sellable_.find(key);

            if (si != sellable_.end() && si->second < cand_cap)
            {
                cand_cap = si->second;
            }

            if (cand_cap - sell_pending <= 0)
            {
                continue;
            }

            // 방금 산 종목은 빼지 않는다. 사자마자 파는 왕복은 비용만 남는다.
            auto oi = opened_at_.find(key);

            if (oi != opened_at_.end() && cfg_.displace_min_hold_sec > 0 &&
                now - oi->second < std::chrono::seconds(cfg_.displace_min_hold_sec))
            {
                continue;
            }

            if (best_key.ticker.empty() || cand_z < worst_z)
            {
                best_key = key;
                worst_z  = cand_z;
            }
        }

        if (best_key.ticker.empty())
        {
            why = "내보낼 보유 종목 없음(전부 최소 보유 미달·매도 중·매도 불가)";
        }
        else if (plan.new_z - worst_z < cfg_.displace_min_z_gap)
        {
            // 격차가 잡음 수준이면 비용만 나간다
            std::ostringstream ws;
            ws << "점수 격차 " << std::fixed << std::setprecision(2) << (plan.new_z - worst_z)
               << "σ < " << cfg_.displace_min_z_gap << "σ (최약체 z=" << worst_z << ")";
            why = ws.str();
        }
    }

    if (!why.empty())
    {
        return decline(why);
    }

    {
        std::lock_guard<std::mutex> lk(positions_mtx_);

        plan.account = best_key.account;
        plan.ticker  = best_key.ticker;
        auto pit = positions_.find(best_key);
        auto rv  = reserved_.find(best_key);
        const int sell_pending = (rv != reserved_.end() && rv->second < 0) ? -rv->second : 0;
        int cap = (pit != positions_.end() ? pit->second : 0);
        auto si = sellable_.find(best_key);

        if (si != sellable_.end() && si->second < cap)
        {
            cap = si->second;
        }

        plan.qty = cap - sell_pending;
        auto ap = avg_prices_.find(best_key);
        plan.avg_price = (ap != avg_prices_.end()) ? ap->second : 0.0;
    }

    if (plan.qty <= 0)
    {
        return decline("최약체 " + plan.ticker + " 매도 가능 수량 0");
    }

    (void)account; // 계좌는 피교체 종목 쪽에서 복원한다(신호 계좌와 다를 수 있음)
    plan.victim_z = worst_z;
    std::ostringstream ss;
    ss << "교체 진입 — " << new_ticker << "(z=" << std::fixed << std::setprecision(2) << plan.new_z
       << ")가 " << plan.ticker << "(z=" << plan.victim_z << ")보다 "
       << (plan.new_z - plan.victim_z) << "σ 높아 슬롯을 넘긴다";
    plan.reason = ss.str();
    plan.ok = true;
    {
        std::lock_guard<std::mutex> lk(displace_mtx_);
        displace_decline_.erase(new_ticker);
    }

    return plan;
}

void OrderGate::note_displacement(const DisplacePlan& plan, const std::string& beneficiary)
{
    if (!plan.ok)
    {
        return;
    }

    const auto now = Clock::now();
    std::lock_guard<std::mutex> lk(displace_mtx_);
    ++displace_count_;

    if (cfg_.displace_cooldown_sec > 0)
    {
        displace_cooldown_[plan.ticker] = now + std::chrono::seconds(cfg_.displace_cooldown_sec);
    }

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
        // 미체결 선점은 일일 만료 (KIS 당일 주문은 장 마감 소멸 → 다음날 잘못된 차단 방지).
        // C5(MM-1): 명시적 취소는 on_cancel()로 일원화. reserved_.clear()는 장 마감 안전망
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
std::vector<OrderGate::HeldPos> OrderGate::snapshot_positions() const
{
    std::vector<HeldPos> out;
    std::lock_guard<std::mutex> lk(positions_mtx_);
    out.reserve(positions_.size());

    for (const auto& kv : positions_)
    {
        if (kv.second <= 0)
        {
            continue; // 롱 보유분만 청산 대상
        }

        const PosKey& key = kv.first;
        HeldPos h;
        h.account = key.account;
        h.ticker  = key.ticker;
        h.qty     = kv.second;
        auto ap = avg_prices_.find(key);
        h.avg_price = (ap != avg_prices_.end()) ? ap->second : 0.0;
        out.push_back(std::move(h));
    }

    return out;
}
