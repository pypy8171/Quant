#include "risk/OrderGate.h"

#include "ipc/LedgerSnapshot.h"
#include "risk/GateReasons.h"
#include "core/KstTime.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <format>
#include <iostream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;

namespace
{
// 국내 주식 체결 비용률 (KIS 실계좌 기준).
constexpr double kCommissionRate = 0.00015; // 위탁수수료 0.015% (매수·매도 공통)
constexpr double kSellTaxRate    = 0.0020;  // 증권거래세 0.20% (매도에만 부과. 2026년: 코스피 0.05%+농특세 0.15%, 코스닥 0.20%. 09-21까지 원장은 0.18%)

// 장중 잔여시간 비율 — 09:00에 1.0, 15:00 이후 0.0. 점수 우선순위 바를 오후로 갈수록 낮춘다.
//  빈 슬롯의 가치는 마감이 다가올수록 떨어진다. 14:30에 상위 종목을 기다리는 건 현금을
//  들고 하루를 끝내는 것과 같아서, 그때는 바를 없애고 아무나 받는 게 맞다.
constexpr int kSessionOpenMin  = 9 * 60;   // 09:00 KST
constexpr int kSessionBarEndMin = 15 * 60; // 15:00 KST — 이후로는 바 없음

// 지금 KST, 자정부터의 분.
int kst_minute_of_day()
{
    const auto time_of_day_now = kst::time_of_day(std::time(nullptr));
    return static_cast<int>(time_of_day_now.hours().count() * 60 + time_of_day_now.minutes().count());
}

double session_remaining_ratio()
{
    const int now_min = kst_minute_of_day();

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
//  클램프한 수량이 다시 거부되므로, 항목·평가가(evaluation_price)·합산 기준(positions_+reserved_)을
//  check()와 똑같이 맞춘다.
int OrderGate::clamp_buy_quantity(const OrderSignal& signal)
{
    int quantity = signal.quantity;

    // ── SELL NEW: 매도가능수량(보유 - 미체결매도) 클램프 ────────────────────
    //  자기가 낸 익절 지정가가 자기 청산을 막는다. 그대로 내면 KIS가 40240000
    //  (주문가능분 없음)으로 주문을 통째로 거부해 한 주도 못 빠져나온다(09-08 047050:
    //  254주·381주 두 번 다 전량 거부, 보유분이 갇혔다). 나갈 수 있는 만큼이라도
    //  내보내는 편이 낫다. 이미 FORCE_LIQ와 디스플레이스먼트는 같은 식으로 깎고 있고,
    //  전략 청산 신호만 이 경로를 안 거치고 있었다.
    //  원장이 그 종목을 모를 때(positions_ 없음 또는 0)는 손대지 않는다 - 과소 인식으로
    //  정당한 청산을 0주로 깎는 쪽이 거부당하는 것보다 위험하다.
    if (signal.side == OrderSide::SELL && signal.action == OrderAction::NEW && quantity > 0)
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        const PosKey key = lookup_key(signal);
        auto position_iterator = positions_.find(key);

        if (position_iterator == positions_.end() || position_iterator->second <= 0)
        {
            return quantity;
        }

        auto reserved_iterator = reserved_.find(key);
        const int sell_pending = (reserved_iterator != reserved_.end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;
        // 상한은 보유수량이 아니라 매도가능수량이다. 기동 전 세션이 남긴 미체결 매도는
        //  reserved_에 없고(프로세스 메모리라 재기동으로 사라진다) 잔고의 ord_psbl_qty에만 보인다.
        int holding_ceiling = position_iterator->second;
        auto strategy_iterator = sellable_.find(key);

        if (strategy_iterator != sellable_.end() && strategy_iterator->second < holding_ceiling)
        {
            holding_ceiling = strategy_iterator->second;
        }

        const int sellable = holding_ceiling - sell_pending;

        // 이 파일은 Logger에 의존하지 않는다(게이트 단위 테스트가 단독 링크한다).
        //  클램프가 실제로 걸리면 호출부 OrderRouter가 "한도 클램프" 한 줄을 남긴다.
        if (sellable <= 0)
        {
            return 0;
        }

        return quantity > sellable ? sellable : quantity;
    }

    if (signal.side != OrderSide::BUY || signal.action != OrderAction::NEW || quantity <= 0)
    {
        return quantity;
    }

    // 지정가는 price, 시장가(0)는 reference_price. 둘 다 없으면 명목을 못 재므로 수량 한도만 건다.
    const double evaluation_price = signal.price > 0.0 ? signal.price : signal.reference_price;

    if (config_.max_quantity_per_order > 0 && quantity > config_.max_quantity_per_order)
    {
        quantity = config_.max_quantity_per_order;
    }

    if (evaluation_price > 0.0 && config_.max_notional_per_order > 0.0)
    {
        const int quantity_ceiling = static_cast<int>(config_.max_notional_per_order / evaluation_price);

        if (quantity_ceiling < quantity)
        {
            quantity = quantity_ceiling;
        }
    }

    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        const PosKey key = lookup_key(signal);
        auto position_iterator = positions_.find(key);
        auto reserved_iterator = reserved_.find(key);
        const int current_quantity = (position_iterator != positions_.end() ? position_iterator->second : 0) +
                            (reserved_iterator != reserved_.end() ? reserved_iterator->second : 0);

        if (config_.max_quantity_per_ticker > 0)
        {
            const int room = config_.max_quantity_per_ticker - current_quantity;

            if (room < quantity)
            {
                quantity = room;
            }
        }

        if (evaluation_price > 0.0 && config_.max_notional_per_ticker > 0.0)
        {
            const int room = static_cast<int>(config_.max_notional_per_ticker / evaluation_price) - current_quantity;

            if (room < quantity)
            {
                quantity = room;
            }
        }

        // 총노출(§3d)도 같은 방식으로 남은 여유를 수량으로 환산한다. 보유는 평단, 선점은 선점가로
        //  재는 것까지 check()와 동일하게 둔다.
        const double equity = equity_.load(std::memory_order_relaxed);

        if (config_.max_gross_exposure_percent > 0.0 && equity > 0.0 && evaluation_price > 0.0)
        {
            double gross = 0.0;

            for (const auto& entry : positions_)
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto average_price_iterator = average_prices_.find(entry.first);
                gross += entry.second * (average_price_iterator != average_prices_.end() ? average_price_iterator->second : 0.0);
            }

            for (const auto& entry : reserved_)
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto reserved_price_iterator = reserved_price_.find(entry.first);
                gross += entry.second * (reserved_price_iterator != reserved_price_.end() ? reserved_price_iterator->second : 0.0);
            }

            const double exposure_ceiling  = config_.max_gross_exposure_percent * equity;
            const int    room = static_cast<int>((exposure_ceiling - gross) / evaluation_price);

            if (room < quantity)
            {
                quantity = room;
            }
        }

        // 주문가능현금 클램프. 평가금이 아니라 현금이 매수의 진짜 상한이다. 우리가 이미 낸
        //  미체결 매수 명목을 빼는 것은 보수적으로 중복차감이 될 수 있으나(브로커 값이 이미
        //  반영했을 수 있다), 모자라게 사는 쪽이 전량 거부보다 낫다.
        const double cash = available_cash_.load(std::memory_order_relaxed);

        if (cash > 0.0 && evaluation_price > 0.0)
        {
            double pending_buy = 0.0;

            for (const auto& entry : reserved_)
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto reserved_price_iterator = reserved_price_.find(entry.first);
                pending_buy += entry.second * (reserved_price_iterator != reserved_price_.end() ? reserved_price_iterator->second : 0.0);
            }

            const int room = static_cast<int>((cash - pending_buy) / evaluation_price);

            if (room < quantity)
            {
                quantity = room;
            }
        }
    }

    return quantity > 0 ? quantity : 0;
}

bool OrderGate::check(const OrderSignal& signal, std::string& reject_reason)
{
    // 1. Kill switch — 전방향 하드스톱(BUY·SELL 모두). 연결단절/수동 긴급정지용.
    if (kill_switch_.load())
    {
        reject_reason = "KILL_SWITCH 활성";
        return false;
    }

    // 2. NONE side — 전략이 신호 없음을 나타낼 때 사용; 주문 처리 불가
    if (signal.side == OrderSide::NONE)
    {
        reject_reason = "OrderSide::NONE — 유효하지 않은 주문 방향";
        return false;
    }

    // 1b. Entry halt — 신규 진입(BUY NEW)만 차단. SELL 청산·취소(CANCEL/REPLACE)는 통과시켜
    //     지수 급락 시 "신규정지 + 보유분 청산"이 게이트에서 미완료로 남지 않게 한다(C-2).
    //     kill_switch_(전방향)와 분리된 국면 리스크 플래그.
    if (entry_halt_.load() && signal.side == OrderSide::BUY && signal.action == OrderAction::NEW)
    {
        reject_reason = "ENTRY_HALT 활성 — 신규 진입 정지(청산은 허용)";
        return false;
    }

    // 1c. 세션 창 — 매매 창 밖의 NEW 주문은 막는다. 통합 피드(KRX+NXT)는 08:00~20:00 틱을 주므로 전략이 그 밖에서
    //     낸 신호를 여기서 잡는다. 창은 정규장(09:00~15:30)과 애프터마켓(16:00~20:00, D-097)의 합집합 — 둘 사이
    //     15:30~16:00은 장후 종가 거래뿐이라 막힌다. CANCEL/REPLACE는 통과(미체결 정리는 언제든).
    //     정규장 창이 0/0이면 검사 없음. [why D-096]
    if (signal.action == OrderAction::NEW && config_.session_close_min > config_.session_open_min)
    {
        const int  now_min    = kst_minute_of_day();
        const bool in_regular = now_min >= config_.session_open_min && now_min < config_.session_close_min;
        const bool in_after   = config_.after_close_min > config_.after_open_min && now_min >= config_.after_open_min &&
                                now_min < config_.after_close_min;

        if (!in_regular && !in_after)
        {
            reject_reason = std::format("세션 창 밖 ({:02}:{:02}, 허용 {:02}:{:02}~{:02}:{:02}", now_min / 60, now_min % 60,
                                        config_.session_open_min / 60, config_.session_open_min % 60,
                                        config_.session_close_min / 60, config_.session_close_min % 60);

            if (config_.after_close_min > config_.after_open_min)
            {
                reject_reason += std::format(" 및 {:02}:{:02}~{:02}:{:02}", config_.after_open_min / 60,
                                             config_.after_open_min % 60, config_.after_close_min / 60,
                                             config_.after_close_min % 60);
            }

            reject_reason += ") — 매매 창에만 주문한다";
            return false;
        }
    }

    // 2b. 1주문 fat-finger 백스톱 (C-3) — NEW BUY/SELL 공통, 시장가 대량주문 슬리피지 방어.
    //     CANCEL/REPLACE는 대상 아님 → action==NEW로 한정해 MM 취소경로에 무영향.
    if (signal.action == OrderAction::NEW)
    {
        if (signal.quantity <= 0)
        {
            reject_reason = std::format("잘못된 주문 수량 ({})", signal.quantity);
            return false;
        }

        if (signal.quantity > config_.max_quantity_per_order)
        {
            reject_reason = std::format("1주문 수량 한도 초과 ({} > {})", signal.quantity, config_.max_quantity_per_order);
            return false;
        }

        // 명목 평가가: 지정가는 price, 시장가(price=0)는 reference_price(직전 현재가).
        // 시장가가 ref_price도 없으면 명목 백스톱 불가(수량 한도로만 방어).
        const double evaluation_price = signal.price > 0.0 ? signal.price : signal.reference_price;

        if (evaluation_price > 0.0 && evaluation_price * signal.quantity > config_.max_notional_per_order)
        {
            const std::string reason_text = std::format("1주문 명목 한도 초과 ({} > {}{}",
                                               static_cast<long long>(evaluation_price * signal.quantity),
                                               static_cast<long long>(config_.max_notional_per_order),
                                               signal.price > 0.0 ? ")" : ", 시장가 참조평가)");

            // SELL은 청산 계열이라 거부하지 않는다 — 정당한 청산을 막는 쪽이 대량 매도보다 위험하다.
            //  (수량 한도는 위에서 이미 걸렸다.) 이 파일은 Logger를 안 쓰므로 stderr 한 줄.
            if (signal.side == OrderSide::SELL)
            {
                std::cerr << "[OrderGate] WARN " << signal.ticker << " SELL " << reason_text << " — 청산이라 통과\n";
            }
            else
            {
                reject_reason = reason_text;
                return false;
            }
        }
    }

    // 3. 포지션 수량 한도 (BUY에만 적용) — 실체결(positions_) + 미체결 선점(reserved_) 합산
    //    계좌별 파티션 — 한 계좌 한도는 다른 계좌 주문을 막지 않는다.
    // 원장 키를 여기서 한 번 만든다 — 3절(한도)과 5절(중복 신호 키)이 같은 번호를 쓴다. 처음 보는 계좌·종목은
    //  등록한다(모르는 계좌끼리 중복 키가 겹치지 않게).
    PosKey key;
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        key = register_key(signal);
    }

    if (signal.side == OrderSide::BUY)
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        int filled = positions_.count(key) ? positions_[key] : 0;
        int reserved   = reserved_.count(key)  ? reserved_[key]  : 0;
        int current_quantity = filled + reserved;

        if (current_quantity + signal.quantity > config_.max_quantity_per_ticker)
        {
            reject_reason = std::format("포지션 한도 초과 ({}+{} > {})", current_quantity, signal.quantity, config_.max_quantity_per_ticker);
            return false;
        }

        // 3b. 종목당 명목 한도 — 자본% 사이징의 상한 백스톱. 지정가는 price, 시장가는 ref_price로
        //     보유·예약 합산 평가(시장가가 백스톱을 우회하지 않도록).
        const double evaluation_price = signal.price > 0.0 ? signal.price : signal.reference_price;

        if (config_.max_notional_per_ticker > 0.0 && evaluation_price > 0.0 &&
            (current_quantity + signal.quantity) * evaluation_price > config_.max_notional_per_ticker)
        {
            reject_reason = std::format("종목당 명목 한도 초과 ({} > {})",
                                        static_cast<long long>((current_quantity + signal.quantity) * evaluation_price),
                                        static_cast<long long>(config_.max_notional_per_ticker));
            return false;
        }

        // 3c. 동시 보유 종목 상한 — "새 종목"을 여는 BUY NEW에만 적용(기존 보유·예약 종목은 통과).
        //     총노출 제어: 실보유(positions_>0)∪예약(reserved_>0) 종목 수가 상한이면 신규 진입 차단.
        //     기존 보유·예약이 있는 종목(filled>0 또는 reserved!=0)은 새로 여는 게 아니므로 예외.
        //     바스켓 슬리브 소유 종목(slot_exempt_)은 상한을 세지도, 상한에 걸리지도 않는다 [why D-109].
        if (config_.max_concurrent_positions > 0 && signal.action == OrderAction::NEW &&
            filled == 0 && reserved == 0 && !slot_exempt_.contains(key.symbol))
        {
            size_t open = 0;
            size_t held = 0;   // 그중 실보유. 거부 문구에서 유령 선점과 갈라 보려고 따로 센다

            // 3c-2(아래)가 쓰는 "나보다 랭크가 위인데 이미 차지된 종목 수"를 같은 순회에서 센다 — 표를 도는 대신
            //  원장(보유·선점 ≤ 슬롯 수)을 돈다. 표는 불변 스냅샷이라 락 밖 포인터로 읽는다.
            const std::shared_ptr<const PriorityTable> table =
                config_.entry_priority_enabled ? priority_snapshot() : nullptr;
            const int rank        = table ? rank_of(*table, key.symbol) : 0;
            int       taken_ahead = 0;

            auto counts_ahead = [&](const PosKey& taken_key)
            {
                if (rank <= 0 || taken_key.account != key.account || taken_key.symbol == key.symbol)
                {
                    return false;
                }

                const int taken_rank = rank_of(*table, taken_key.symbol);
                return taken_rank > 0 && taken_rank < rank;
            };

            for (const auto& entry : positions_)
            {
                if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
                {
                    ++open;
                    ++held;
                    taken_ahead += counts_ahead(entry.first) ? 1 : 0;
                }
            }

            for (const auto& entry : reserved_)
            {
                if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
                {
                    auto iterator = positions_.find(entry.first);

                    if (iterator == positions_.end() || iterator->second <= 0)
                    {
                        ++open;  // 예약만 있는 종목(중복 제외)
                        taken_ahead += counts_ahead(entry.first) ? 1 : 0;
                    }
                }
            }

            if (open >= static_cast<size_t>(config_.max_concurrent_positions))
            {
                std::string reason_text = std::format("동시 보유 종목 한도 초과 ({} >= {}, 실보유 {} 선점만 {})",
                                             open, config_.max_concurrent_positions, held, open - held);

                // 교체 진입이 켜져 있으면 여기 오는 BUY는 교체 판정에서 떨어진 것이다. 그 사유를
                //  같이 적지 않으면 한도 문구만 남아 "교체가 안 도는 것"으로 읽힌다(09-11 11:21).
                bool declined = false;
                {
                    // [lock-order] positions_mutex_ → displace_mutex_. 반대 순서로 겹쳐 잡는 곳은 없다
                    //  (plan_displacement·note_displacement는 displace_mtx_를 단독 구간으로만 쓴다).
                    std::lock_guard<std::mutex> dl(displace_mutex_);
                    auto di = displace_decline_.find(key.symbol);

                    if (di != displace_decline_.end())
                    {
                        reason_text += " — 교체 보류: ";
                        reason_text += di->second;
                        declined = true;
                    }
                }

                if (!declined)
                {
                    reason_text += " — 신규 종목 진입 정지";
                }

                reject_reason = std::move(reason_text);
                return false;
            }

            // 3c-1. 교체 쿨다운 / 비운 슬롯 예약 — 교체가 켜져 있을 때만.
            //   쿨다운: 방금 밀려난 종목이 곧장 되돌아오면 교체 비용만 왕복으로 나간다.
            //   슬롯 예약: 교체로 비운 자리를 수혜 종목이 아닌 다른 종목이 가로채면
            //             매도 비용만 치르고 사려던 종목은 또 못 산다.
            if (config_.displace_enabled)
            {
                const auto now = Clock::now();
                bool blocked = false;
                bool cooling = false;
                {
                    std::lock_guard<std::mutex> lock(displace_mutex_);

                    if (key.symbol < displace_cooldown_until_.size())
                    {
                        TimePoint& cooldown_until = displace_cooldown_until_[key.symbol];

                        if (cooldown_until != TimePoint{})
                        {
                            if (now < cooldown_until)
                            {
                                cooling = true;
                            }
                            else
                            {
                                cooldown_until = TimePoint{};
                            }
                        }
                    }

                    if (!cooling && slot_reserved_for_ != symbol::kNone)
                    {
                        if (now >= slot_reserved_until_)
                        {
                            slot_reserved_for_ = symbol::kNone;
                        }
                        else if (slot_reserved_for_ == key.symbol)
                        {
                            slot_reserved_for_ = symbol::kNone; // 수혜 종목이 자리를 가져갔다
                        }
                        else
                        {
                            blocked = true;
                            reject_reason = std::format("교체로 비운 슬롯 예약분 ({}) — 다른 종목 진입 보류",
                                                        symbols_->name(slot_reserved_for_).view());
                        }
                    }
                }

                if (cooling)
                {
                    reject_reason = "교체 쿨다운 중 — 방금 슬롯을 내준 종목의 재진입 금지";
                    return false;
                }

                if (blocked)
                {
                    return false;
                }
            }

            // 3c-2. 점수 우선순위 바 — 남은 슬롯이 적을수록 더 높은 점수를 요구한다.
            //   rank/total ≤ 1 − (open/slots) × decay(t)
            //  슬롯이 비어 있으면 아무나 통과하고, 마지막 칸에 가까울수록 상위만 남는다.
            //  랭크를 모르는 종목(스캔 유니버스 밖 보유분 청산 관리 등)은 바를 적용하지 않는다.
            if (table)
            {
                const int total    = table->total;
                int       eff_rank = 0;

                // 유효 랭크 = 나보다 점수가 높으면서 "아직 슬롯을 안 차지한" 종목 수 + 1.
                //  전체 랭크를 그대로 쓰면 상위 랭크를 이미 보유한 순간 남은 후보는
                //  구조적으로 기준선을 넘을 수 없다(보유 23/25에서 최상위 후보의 랭크가
                //  24위라 quant=0.93 > 기준 0.54). 그러면 슬롯이 25에 닿지 못하고,
                //  교체 진입은 capacity_full()에서만 열리므로 둘 다 영원히 막힌다.
                //  살 수 있는 종목들 사이의 순위로 재면 최상위 후보는 항상 1위가 된다.
                //  "나보다 위" 전체 수는 표가 미리 세 두었고(below_by_symbol), 그중 차지된 수는 위 원장 순회가 셌다.
                if (rank > 0)
                {
                    eff_rank = table->below_by_symbol[key.symbol] - taken_ahead + 1;
                }

                if (rank > 0 && total > 0)
                {
                    const double occupancy = static_cast<double>(open) /
                                             static_cast<double>(config_.max_concurrent_positions);
                    const double bar   = 1.0 - occupancy * session_remaining_ratio();
                    // 분모는 슬롯이 경합하는 모집단이다. 등록 종목이 슬롯보다 적으면 경합 자체가
                    //  없는데도 rank/total 이 1.0에 붙어 하위 랭크가 영구 차단된다(등록 12 vs 슬롯 25
                    //  이면 허용 랭크가 8에서 멈춰 슬롯의 1/3만 채운 채 하루가 끝난다). 모집단을
                    //  최소 슬롯 수로 받쳐, 풀이 슬롯보다 클 때의 동작은 그대로 두고 작을 때만 푼다.
                    const int    pool  = total > config_.max_concurrent_positions
                                             ? total : config_.max_concurrent_positions;
                    const double quant = static_cast<double>(eff_rank) / static_cast<double>(pool);

                    if (quant > bar)
                    {
                        reject_reason = std::format("점수 우선순위 미달 (랭크 {} 유효 {}/{} = {:.2f} > 기준 {:.2f}, 슬롯 {}/{}) — 더 높은 점수 종목을 위해 보류",
                                                    rank, eff_rank, pool, quant, bar, open,
                                                    config_.max_concurrent_positions);
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

        if (config_.max_gross_exposure_percent > 0.0 && equity > 0.0 && evaluation_price > 0.0)
        {
            double gross = 0.0;

            for (const auto& entry : positions_)
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto average_price_iterator = average_prices_.find(entry.first);
                gross += entry.second * (average_price_iterator != average_prices_.end() ? average_price_iterator->second : 0.0);
            }

            for (const auto& entry : reserved_)
            {
                if (entry.second <= 0)
                {
                    continue;  // BUY 선점(+)만 노출 증가. SELL 선점(-)은 축소라 보수적으로 무시
                }

                auto reserved_price_iterator = reserved_price_.find(entry.first);
                gross += entry.second * (reserved_price_iterator != reserved_price_.end() ? reserved_price_iterator->second : 0.0);
            }

            const double exposure_ceiling        = config_.max_gross_exposure_percent * equity;
            const double next_gross = gross + signal.quantity * evaluation_price;

            if (next_gross > exposure_ceiling)
            {
                reject_reason = std::format("총노출 한도 초과 ({} > {} = 자본 {}×{:g}) — 신규 매수 정지(청산 허용)",
                                            static_cast<long long>(next_gross), static_cast<long long>(exposure_ceiling),
                                            static_cast<long long>(equity), config_.max_gross_exposure_percent);
                return false;
            }
        }
    }

    // 4. 일일 손실 한도 (BUY에만 적용 — 신규 진입 차단이 설계 의도)
    //    (C10) 보유분 추가 하락은 막지 않는다. 강제 청산이 필요하면 별도 청산 로직 도입.
    if (signal.side == OrderSide::BUY)
    {
        std::lock_guard<std::mutex> lock(pnl_mutex_);

        if (daily_pnl_ <= config_.daily_loss_limit)
        {
            reject_reason = std::format("일일 손실 한도 초과 (현재 {}원 / 한도 {}원)",
                                        static_cast<int>(daily_pnl_), static_cast<int>(config_.daily_loss_limit));
            return false;
        }
    }

    // 4b. PnL stale guard (B2) — daily_pnl_이 낡으면(잔고 대조 연속 정체) §4 손실컷을
    //     신뢰할 수 없어 BUY NEW만 보수적으로 정지. SELL 청산·BUY 취소/정정은 통과시켜
    //     "신규 위험만 억제, 탈출은 허용"(entry_halt와 동일 의미론). Engine이 잔고조회 복구 시 해제.
    if (pnl_stale_.load() && signal.side == OrderSide::BUY && signal.action == OrderAction::NEW)
    {
        reject_reason = "PNL_STALE — 잔고 대조 정체(daily_pnl 미갱신), 신규 진입 보수적 정지";
        return false;
    }

    // 5. 중복 신호 제거 — rate 소비 전에 검사해 중복이 rate slot을 소모하지 않게 함.
    //    키에 side 포함(MM-1): 시장조성은 같은 틱에 동일 strategy+ticker로 BUY(bid)+SELL(ask)를
    //    동시 발주한다. side가 없으면 두 번째(ask)가 중복 오거부된다. BUY/SELL은 다른 의도라
    //    중복이 아니다. (같은 side 반복은 여전히 deduplicate — 기존 전략 동작 불변)
    //    스탬프(last_signal_)는 6절 rate 통과 뒤에 찍는다 — rate로 거부된 신호가 deduplicate 창을
    //    소모하면 창 안의 정당한 재시도까지 "중복"으로 막힌다(W-2).
    //    지정가는 가격까지 키에 넣는다 — 분할 매수는 같은 종목·같은 방향의 분할 단계 여러 개를 한 틱에
    //    내는데, 주문 스레드가 1초 안에 연달아 처리하면 두 번째 분할 단계부터 "중복"으로 잘렸다
    //    (09-11 10:04 232140 BUY 42@11790·42@11690 둘 다 거부). 같은 가격 반복만 중복이다.
    //    키는 정수 다섯 개(계좌 번호·전략 번호·종목 id·방향·지정가)다 — 문자열을 이어 붙이던 때는 연결만 105ns였고
    //    해시가 그 위에 얹혔다(D-070 측정). 전략 번호가 없는 신호(kNone)는 이름으로 한 번 등록해 번호를 받는다. [why D-112]
    const SignalKey deduplicate_key{key.account,
                                    signal.strategy_index != strategy_table::kNone
                                        ? signal.strategy_index
                                        : strategies_.intern(signal.strategy_id),
                                    key.symbol,
                                    static_cast<int32_t>(signal.side),
                                    signal.type == OrderType::LIMIT ? static_cast<int64_t>(signal.price) : 0};

    {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lock(deduplicate_mutex_);
        auto iterator = last_signal_.find(deduplicate_key);

        if (iterator != last_signal_.end())
        {
            double elapsed = std::chrono::duration<double>(now - iterator->second).count();

            if (elapsed < config_.deduplicate_window_sec)
            {
                reject_reason = std::format("중복 신호 (윈도우 {}초)", config_.deduplicate_window_sec);
                return false;
            }
        }
    }

    // 6. Rate limit — 초당 / 분당 두 단계 검사 (deduplicate 통과 후에만 카운터 소모)
    {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lock(rate_mutex_);

        // 초당 제한
        auto cutoff_sec = now - std::chrono::seconds(1);

        while (!order_times_sec_.empty() && order_times_sec_.front() < cutoff_sec)
        {
            order_times_sec_.pop_front();
        }

        if (static_cast<int>(order_times_sec_.size()) >= config_.max_orders_per_sec)
        {
            reject_reason = gate_reason::rate_limit(false, config_.max_orders_per_sec);
            return false;
        }

        // 분당 제한
        auto cutoff_min = now - std::chrono::minutes(1);

        while (!order_times_min_.empty() && order_times_min_.front() < cutoff_min)
        {
            order_times_min_.pop_front();
        }

        if (static_cast<int>(order_times_min_.size()) >= config_.max_orders_per_min)
        {
            reject_reason = gate_reason::rate_limit(true, config_.max_orders_per_min);
            return false;
        }

        order_times_sec_.push_back(now);
        order_times_min_.push_back(now);
    }

    // 모든 검사를 지난 신호만 deduplicate 창을 연다.
    {
        std::lock_guard<std::mutex> lock(deduplicate_mutex_);
        last_signal_[deduplicate_key] = Clock::now();
    }

    return true;
}

// ─── 주문 의도 — 전송 직전 선점 + INTENT 기록 (실체결 원장 positions_는 불변) ────────
namespace
{
ledger_journal::Record make_order_record(ledger_journal::Kind kind, OrderSide side, int quantity,
                                         const OrderGate::OrderRef& reference)
{
    ledger_journal::Record record;
    record.kind             = static_cast<uint16_t>(kind);
    record.side             = static_cast<uint8_t>(static_cast<OrderSide::Value>(side));
    record.order_type       = static_cast<uint8_t>(reference.type);
    record.order_id         = reference.order_id;
    record.kis_order_number = reference.kis_order_number;
    record.quantity         = quantity;
    return record;
}
} // namespace

bool OrderGate::on_intent(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          double price, const OrderRef& reference, strategy_table::StrategyId strategy)
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key   = make_key(account, ticker);
    const int    delta = (side == OrderSide::BUY) ? quantity : -quantity; // BUY 선점 +, SELL 선점 -
    const auto   previous_price_iterator = reserved_price_.find(key);
    const bool   had_price               = previous_price_iterator != reserved_price_.end();
    const double previous_price          = had_price ? previous_price_iterator->second : 0.0;
    apply_reservation_delta(account, ticker, delta, price);

    ledger_journal::Record record = make_order_record(ledger_journal::Kind::INTENT, side, quantity, reference);
    record.price                  = price;
    ledger_journal::put_string(record.strategy, sizeof(record.strategy), strategies_.name(strategy).view());

    if (journal_append(record, account, ticker))
    {
        return true;
    }

    // 적히지 않은 선점은 되돌린다 — 파일에 없는 주문은 나가지 않는다. 선점가도 직전 값으로.
    apply_reservation_delta(account, ticker, -delta, 0.0);

    if (had_price && reserved_.count(key))
    {
        reserved_price_[key] = previous_price;
    }

    return false;
}

void OrderGate::on_accepted(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                            const OrderRef& reference)
{
    ledger_journal::Record record = make_order_record(ledger_journal::Kind::ACCEPT, side, quantity, reference);
    journal_append(record, account, ticker);
}

void OrderGate::on_reject(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          const OrderRef& reference, std::string_view reason)
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    release_reservation(make_key(account, ticker), (side == OrderSide::BUY) ? -quantity : quantity);
    ledger_journal::Record record = make_order_record(ledger_journal::Kind::REJECT, side, quantity, reference);
    ledger_journal::put_string(record.reason, sizeof(record.reason), reason);
    journal_append(record, account, ticker);
}

// on_intent 본체 + 저널 리플레이(apply_record) 공용 — 재기동 복구가 실시간 경로와 같은 규칙을 탄다.
//  [inv] positions_mutex_를 잡고 부른다.
void OrderGate::apply_reservation_delta(std::string_view account, std::string_view ticker, int delta, double price)
{
    const PosKey key  = make_key(account, ticker);
    int          next = (reserved_.count(key) ? reserved_[key] : 0) + delta;

    if (next == 0)
    {
        reserved_.erase(key);
        reserved_price_.erase(key);      // 선점이 해소되면 선점가도 정리(§3d 명목이 남아 부풀지 않게)
    }
    else
    {
        reserved_[key] = next;

        if (price > 0.0)
        {
            reserved_price_[key] = price; // 최신 선점가 기록. 시장가(0)면 유지(직전 값)해 총노출 근사 보존
        }
    }
}

// ─── 미체결 취소/정정 축소 시 선점 해제 (C5) ────────────────────────────────
//  on_fill_confirmed의 reserved 해제와 같은 방향. positions_/average_price는 손대지 않는다
//  (취소는 체결이 아니므로 실보유·평단 불변). quantity<=0이면 no-op(방어).
// 선점 해제 한 곳 — 취소 통보와 체결 통보가 같은 규칙을 쓰게 모았다. 규칙이 갈라져 있던 동안
//  on_cancel에만 가드가 있고 on_fill_confirmed에는 없어, 선점을 잡은 적 없는 포지션의 체결이
//  없던 선점을 만들어 냈다. delta는 해제 방향(BUY 선점 +는 -quantity, SELL 선점 -는 +quantity).
//  호출자가 positions_mtx_를 이미 쥐고 있다고 가정한다(여기서 다시 잡지 않는다).
void OrderGate::release_reservation(const PosKey& key, int delta)
{
    // 잔고 대조가 reserved_를 비운 뒤 온 통보는 대상이 이미 없으므로 아무 것도 하지 않는다.
    //  (없는 키를 갱신하면 부호가 뒤집힌 선점이 생겨 이후 한도·슬롯 계산이 왜곡됨)
    int current = reserved_.count(key) ? reserved_[key] : 0;

    if (current == 0)
    {
        return;
    }

    int result = current + delta;

    // 과잉 해제(부호 역전) 시 0에서 정지 — 리셋·이중통보로 음수 선점이 남지 않게.
    if ((current > 0 && result < 0) || (current < 0 && result > 0))
    {
        result = 0;
    }

    if (result == 0)
    {
        reserved_.erase(key);
        reserved_price_.erase(key);
    }
    else
    {
        reserved_[key] = result;
    }
}

void OrderGate::on_cancel(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          const OrderRef& reference)
{
    if (quantity <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    // BUY 선점은 +였으므로 -quantity, SELL 선점은 -였으므로 +quantity (해제 = 반대부호 가산)
    release_reservation(make_key(account, ticker), (side == OrderSide::BUY) ? -quantity : quantity);
    ledger_journal::Record record = make_order_record(ledger_journal::Kind::CANCEL, side, quantity, reference);
    journal_append(record, account, ticker);
}

// ─── 선점 전면 초기화 (REST 잔고 대조 전용) ──────────────────────────────────
void OrderGate::reset_reserved()
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    reserved_.clear();
    reserved_price_.clear();
    ledger_journal::Record record;
    record.kind = static_cast<uint16_t>(ledger_journal::Kind::RESET_RESERVED);
    journal_append(record, std::string_view(), std::string_view());
}

// ─── 저널 ────────────────────────────────────────────────────────────────────
bool OrderGate::set_journal(const std::filesystem::path& directory, std::string_view date_yyyymmdd, bool fsync)
{
    journal_ = std::make_unique<ledger_journal::LedgerJournal>(directory, date_yyyymmdd, fsync);

    if (!journal_->ok())
    {
        std::cerr << std::format("[OrderGate] 원장 저널을 못 열었다: {}\n", journal_->path().string());
        return false;
    }

    replaying_     = true;
    replay_result_ = ledger_journal::LedgerJournal::replay(
        journal_->path(), [this](const ledger_journal::Record& record) { apply_record(record); });
    replaying_ = false;
    // 꼬리를 잘랐는지는 파일을 열 때만 알 수 있다 — 자르고 난 뒤 다시 읽으면 멀쩡해 보인다.
    replay_result_.truncated_tail = journal_->opened().truncated_tail;
    return true;
}

bool OrderGate::journal_append(ledger_journal::Record& record, std::string_view account, std::string_view ticker)
{
    if (replaying_ || !journal_)
    {
        return true;
    }

    ledger_journal::put_string(record.account, sizeof(record.account), account);
    ledger_journal::put_string(record.ticker, sizeof(record.ticker), ticker);
    std::lock_guard<std::mutex> lock(journal_mutex_);

    if (journal_->append(record))
    {
        return true;
    }

    journal_failures_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << std::format("[OrderGate] 원장 저널 기록 실패 kind={} {} {} — 파일이 원장보다 뒤처졌다\n", record.kind,
                             account, ticker);
    return false;
}

void OrderGate::journal_adjust(const PosKey& key, std::string_view reason)
{
    ledger_journal::Record record;
    record.kind = static_cast<uint16_t>(ledger_journal::Kind::ADJUST);
    const auto position_iterator = positions_.find(key);
    const auto average_iterator  = average_prices_.find(key);
    const auto sellable_iterator = sellable_.find(key);
    const auto reserved_iterator = reserved_.find(key);
    record.quantity          = position_iterator != positions_.end() ? position_iterator->second : 0;
    record.price             = average_iterator != average_prices_.end() ? average_iterator->second : 0.0;
    record.sellable          = sellable_iterator != sellable_.end() ? sellable_iterator->second : -1;
    record.reserved_quantity = reserved_iterator != reserved_.end() ? reserved_iterator->second : 0;
    ledger_journal::put_string(record.reason, sizeof(record.reason), reason);
    journal_append(record, account_of(key), ticker_of(key).view());
}

std::vector<OrderGate::OpenIntent> OrderGate::open_intents() const
{
    std::vector<OpenIntent> intents;
    intents.reserve(open_intents_.size());

    for (const auto& [order_id, intent] : open_intents_)
    {
        intents.push_back(intent);
    }

    std::sort(intents.begin(), intents.end(),
              [](const OpenIntent& left, const OpenIntent& right) { return left.order_id < right.order_id; });
    return intents;
}

// INTENT가 열고 FILL·CANCEL·REJECT가 닫는다 — 남은 것이 "보냈는데 결말을 못 본" 주문이다. 주문번호가 0인
//  레코드(시드·대조·현금)와 라우터가 번호를 못 붙인 주문은 셈에서 뺀다(닫을 방법이 없어 영원히 남는다).
void OrderGate::track_open_intent(const ledger_journal::Record& record)
{
    using ledger_journal::Kind;

    if (record.order_id == 0)
    {
        return;
    }

    const Kind kind = static_cast<Kind>(record.kind);

    if (kind == Kind::INTENT)
    {
        OpenIntent& intent   = open_intents_[record.order_id];
        intent.order_id      = record.order_id;
        intent.account       = record.account;
        intent.ticker        = record.ticker;
        intent.strategy_name = record.strategy;
        intent.side  = record.side == static_cast<uint8_t>(OrderSide::SELL) ? OrderSide::SELL : OrderSide::BUY;
        intent.type  = record.order_type == static_cast<uint8_t>(OrderType::LIMIT) ? OrderType::LIMIT : OrderType::MARKET;
        intent.price = record.price;
        intent.remaining += record.quantity;
        return;
    }

    auto iterator = open_intents_.find(record.order_id);

    if (iterator == open_intents_.end())
    {
        return;
    }

    if (kind == Kind::ACCEPT)
    {
        iterator->second.accepted         = true;
        iterator->second.kis_order_number = record.kis_order_number;
        return;
    }

    if (kind != Kind::FILL && kind != Kind::CANCEL && kind != Kind::REJECT)
    {
        return;
    }

    iterator->second.remaining -= record.quantity;

    if (iterator->second.remaining <= 0)
    {
        open_intents_.erase(iterator);
    }
}

void OrderGate::apply_record(const ledger_journal::Record& record)
{
    using ledger_journal::Kind;
    track_open_intent(record);
    const std::string account(record.account);
    const std::string ticker(record.ticker);
    const OrderSide   side = record.side == static_cast<uint8_t>(OrderSide::SELL) ? OrderSide::SELL : OrderSide::BUY;
    const int         release = (side == OrderSide::BUY) ? -record.quantity : record.quantity;

    switch (static_cast<Kind>(record.kind))
    {
    case Kind::SEED:
        seed_position(account, ticker, record.quantity, record.price, record.sellable);
        break;

    case Kind::INTENT:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        apply_reservation_delta(account, ticker, (side == OrderSide::BUY) ? record.quantity : -record.quantity,
                                record.price);
        break;
    }

    case Kind::ACCEPT:
        break; // 상태 변화 없음 — 미결 INTENT를 가리는 것은 재기동 대조(Engine) 몫

    case Kind::REJECT:
    case Kind::CANCEL:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        release_reservation(make_key(account, ticker), release);
        break;
    }

    case Kind::FILL:
        on_fill_confirmed(account, ticker, side, record.quantity, record.price,
                          record.strategy[0] != '\0' ? strategies_.intern(record.strategy) : strategy_table::kNone);
        break;

    case Kind::ADJUST:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        apply_adjust_locked(make_key(account, ticker), record);
        break;
    }

    case Kind::RESET_RESERVED:
        reset_reserved();
        break;

    case Kind::CASH:
        available_cash_.store(record.cash, std::memory_order_relaxed);
        equity_.store(record.equity, std::memory_order_relaxed);
        break;

    case Kind::DAILY_PNL:
        set_daily_pnl(record.pnl);
        break;
    }
}

void OrderGate::apply_adjust_locked(const PosKey& key, const ledger_journal::Record& record)
{
    if (record.quantity > 0)
    {
        positions_[key]      = record.quantity;
        average_prices_[key] = record.price;
        sellable_[key]       = (record.sellable >= 0 && record.sellable < record.quantity) ? record.sellable : record.quantity;

        if (!opened_at_.count(key))
        {
            opened_at_[key] = Clock::now() - std::chrono::hours(24);
        }
    }
    else
    {
        positions_.erase(key);
        average_prices_.erase(key);
        sellable_.erase(key);
        opened_at_.erase(key);
    }

    if (record.reserved_quantity != 0)
    {
        reserved_[key] = record.reserved_quantity;
    }
    else
    {
        reserved_.erase(key);
        reserved_price_.erase(key);
    }

    missed_sell_seen_.erase(key);
}

void OrderGate::set_daily_pnl(double pnl)
{
    {
        std::lock_guard<std::mutex> lock(pnl_mutex_);
        daily_pnl_ = pnl;
    }

    ledger_journal::Record record;
    record.kind = static_cast<uint16_t>(ledger_journal::Kind::DAILY_PNL);
    record.pnl  = pnl;
    journal_append(record, std::string_view(), std::string_view());
}

void OrderGate::set_equity(double equity)
{
    equity_.store(equity, std::memory_order_relaxed);
    ledger_journal::Record record;
    record.kind   = static_cast<uint16_t>(ledger_journal::Kind::CASH);
    record.cash   = available_cash_.load(std::memory_order_relaxed);
    record.equity = equity;
    journal_append(record, std::string_view(), std::string_view());
}

void OrderGate::set_available_cash(double available_cash)
{
    available_cash_.store(available_cash, std::memory_order_relaxed);
    ledger_journal::Record record;
    record.kind   = static_cast<uint16_t>(ledger_journal::Kind::CASH);
    record.cash   = available_cash;
    record.equity = equity_.load(std::memory_order_relaxed);
    journal_append(record, std::string_view(), std::string_view());
}

// ─── 유령 슬롯 정리 ─────────────────────────────────────────────────────────
//  살아 있는 종목 문자열(브로커 잔고·라우터 이력)을 종목 id 비트로 한 번만 바꾼다 — 원장을 돌며 항목마다
//  문자열 집합을 묻던 것을 비트 인덱스로 바꿨다. 테이블이 모르는 종목은 원장에도 없으니 빠뜨려도 같다.
std::vector<bool> OrderGate::live_symbols(const std::vector<std::string>& live_tickers) const
{
    std::vector<bool> live(symbols_->capacity(), false);

    for (const std::string& ticker : live_tickers)
    {
        const symbol::SymbolId symbol = symbols_->lookup(ticker);

        if (symbol != symbol::kNone && symbol < live.size())
        {
            live[symbol] = true;
        }
    }

    return live;
}

std::vector<symbol::SymbolId> OrderGate::prune_positions(const std::vector<std::string>& live_tickers, int min_age_sec)
{
    std::vector<symbol::SymbolId> gone;
    const std::vector<bool>  live = live_symbols(live_tickers);
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const auto now = Clock::now();

    for (auto iterator = positions_.begin(); iterator != positions_.end();)
    {
        if (iterator->second <= 0 || live[iterator->first.symbol])
        {
            ++iterator;
            continue;
        }

        // 방금 열린 포지션은 남긴다. 잔고 조회가 체결보다 먼저 떠난 왕복이면 잔고에 아직
        //  안 보이는데, 그걸 지우면 전략이 미보유로 읽고 같은 종목을 또 산다.
        auto opened_iterator = opened_at_.find(iterator->first);

        if (opened_iterator != opened_at_.end() && min_age_sec > 0 &&
            now - opened_iterator->second < std::chrono::seconds(min_age_sec))
        {
            ++iterator;
            continue;
        }

        const PosKey key = iterator->first;
        gone.push_back(key.symbol);
        average_prices_.erase(key);
        opened_at_.erase(key);
        sellable_.erase(key);
        iterator = positions_.erase(iterator);
        journal_adjust(key, "prune_positions");
    }

    return gone;
}

std::vector<std::string> OrderGate::prune_reservations(const std::vector<std::string>& live_tickers)
{
    return prune_reservations(live_symbols(live_tickers));
}

std::vector<std::string> OrderGate::prune_reservations(const std::vector<bool>& live)
{
    std::vector<std::string>    gone;
    std::lock_guard<std::mutex> lock(positions_mutex_);

    for (auto iterator = reserved_.begin(); iterator != reserved_.end();)
    {
        if (iterator->first.symbol < live.size() && live[iterator->first.symbol])
        {
            ++iterator;
            continue;
        }

        const PosKey key = iterator->first;
        gone.emplace_back(ticker_of(key).view());
        reserved_price_.erase(key);
        iterator = reserved_.erase(iterator);
        journal_adjust(key, "prune_reservations");
    }

    return gone;
}

void OrderGate::restore_sellable(const std::string& account, const std::string& ticker, int quantity)
{
    if (quantity <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = make_key(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= 0)
    {
        return;   // 원장이 모르는 보유는 손대지 않는다 — 없는 매도가능수량을 만들어 낼 이유가 없다
    }

    auto strategy_iterator = sellable_.find(key);
    const int current = (strategy_iterator != sellable_.end()) ? strategy_iterator->second : 0;
    const int restored = current + quantity;
    sellable_[key] = (restored > position_iterator->second) ? position_iterator->second : restored;
    journal_adjust(key, "restore_sellable");
}

OrderGate::SellableView OrderGate::sellable_view(const std::string& account, const std::string& ticker) const
{
    SellableView sellable_view;
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = lookup_key(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= 0)
    {
        return sellable_view;
    }

    sellable_view.held     = position_iterator->second;
    sellable_view.possible_quantity_cap = sellable_view.held;
    auto strategy_iterator = sellable_.find(key);

    if (strategy_iterator != sellable_.end() && strategy_iterator->second < sellable_view.possible_quantity_cap)
    {
        sellable_view.possible_quantity_cap = strategy_iterator->second;
    }

    auto reserved_iterator = reserved_.find(key);
    sellable_view.pending = (reserved_iterator != reserved_.end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;
    return sellable_view;
}

void OrderGate::refresh_sellable(const std::string& account, const std::string& ticker, int ord_psbl_qty)
{
    if (ord_psbl_qty < 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = make_key(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= 0)
    {
        return;
    }

    auto reserved_iterator = reserved_.find(key);
    const int sell_pending = (reserved_iterator != reserved_.end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;
    const int sellable     = ord_psbl_qty + sell_pending;
    sellable_[key] = (sellable > position_iterator->second) ? position_iterator->second : sellable;
    journal_adjust(key, "refresh_sellable");
}

int OrderGate::absorb_missed_sell(const std::string& account, const std::string& ticker, int balance_quantity)
{
    if (balance_quantity < 0)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = make_key(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= balance_quantity)
    {
        missed_sell_seen_.erase(key);
        return 0;
    }

    const int difference = position_iterator->second - balance_quantity;
    auto reserved_iterator = reserved_.find(key);
    const int sell_pending = (reserved_iterator != reserved_.end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;

    // 미체결 매도보다 큰 차이는 놓친 체결로 설명되지 않는다 — 손대지 않고 로그 관찰에 맡긴다.
    if (difference > sell_pending)
    {
        missed_sell_seen_.erase(key);
        return 0;
    }

    auto seen = missed_sell_seen_.find(key);

    if (seen == missed_sell_seen_.end() || seen->second != balance_quantity)
    {
        missed_sell_seen_[key] = balance_quantity; // 첫 관측 — 다음 대조에서 같으면 맞춘다
        return 0;
    }

    missed_sell_seen_.erase(key);
    position_iterator->second = balance_quantity;
    reserved_iterator->second += difference;

    if (reserved_iterator->second == 0)
    {
        reserved_.erase(reserved_iterator);
    }

    auto sellable_iterator = sellable_.find(key);

    if (sellable_iterator != sellable_.end() && sellable_iterator->second > balance_quantity)
    {
        sellable_iterator->second = balance_quantity;
    }

    journal_adjust(key, "absorb_missed_sell");
    return difference;
}

// ─── 실현 손익 누적 ─────────────────────────────────────────────────────────
void OrderGate::add_realized_pnl(double pnl)
{
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    daily_pnl_ += pnl;
}

// ─── 원장 부트스트랩 (G5) — 실계좌 보유분 시드 ──────────────────────────────
//  체결이 아니므로 reserved_·daily_pnl_은 두고 positions_/avg_prices_만 설정한다.
//  기동 initialize 구간(스레드 시작 전)에서만 호출 → 첫 주문/체결과 경합 없음.
void OrderGate::seed_position(const std::string& account, const std::string& ticker, int quantity, double average,
                              int sellable)
{
    if (quantity <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = make_key(account, ticker);
    positions_[key]  = quantity;
    average_prices_[key] = average;
    // 매도가능수량. 모르면(-1) 보유수량으로 둔다 - 모르는 것을 0으로 두면 정당한 청산이 막힌다.
    sellable_[key] = (sellable >= 0 && sellable < quantity) ? sellable : quantity;
    // 기동 시드는 "오늘 산 것"이 아니다. 최소 보유 시간 판정에서 즉시 교체 대상이 되도록
    //  과거 시각으로 찍는다(전일 물린 보유분을 15분 붙잡아 둘 이유가 없다).
    opened_at_[key] = Clock::now() - std::chrono::hours(24);

    ledger_journal::Record record;
    record.kind     = static_cast<uint16_t>(ledger_journal::Kind::SEED);
    record.quantity = quantity;
    record.price    = average;
    record.sellable = sellable;
    journal_append(record, account, ticker);
}

// ─── 체결 확인 — average_price 재계산 + 실현손익 적립 ──────────────────────────
OrderGate::FillResult OrderGate::on_fill_confirmed(
    const std::string& account, const std::string& ticker, OrderSide side, int quantity, double price,
    strategy_table::StrategyId strategy, const OrderRef& reference)
{
    FillResult result;
    result.commission = price * quantity * kCommissionRate;                              // 수수료 0.015%
    result.tax        = (side == OrderSide::SELL) ? price * quantity * kSellTaxRate : 0.0; // 거래세 매도만

    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        const PosKey key = make_key(account, ticker);
        int pre_quantity    = positions_.count(key) ? positions_[key] : 0; // 체결 전 실보유
        double current_average = average_prices_.count(key) ? average_prices_[key] : 0.0;

        // 전략별 서브원장(D-089) — 위 종목단위 pre_quantity/cur_avg와 별개로 같은 락에서 갱신.
        //  전략 번호가 없으면(kNone) 건드리지 않는다(계산·판정에 영향 없음, 참고용 집계일 뿐).
        if (strategy != strategy_table::kNone)
        {
            const StrategyKey strategy_key{strategy, key.symbol};
            auto              strategy_position_iterator = strategy_positions_.find(strategy_key);
            auto              strategy_average_iterator  = strategy_average_prices_.find(strategy_key);
            const int         strategy_pre_quantity =
                strategy_position_iterator != strategy_positions_.end() ? strategy_position_iterator->second : 0;
            const double strategy_current_average =
                strategy_average_iterator != strategy_average_prices_.end() ? strategy_average_iterator->second : 0.0;

            if (side == OrderSide::BUY)
            {
                int strategy_new_quantity = strategy_pre_quantity + quantity;
                strategy_average_prices_[strategy_key] = (strategy_new_quantity > 0)
                    ? (strategy_pre_quantity * strategy_current_average + quantity * price) / strategy_new_quantity
                    : price;
                strategy_positions_[strategy_key] = strategy_new_quantity;
            }
            else // SELL
            {
                if (strategy_pre_quantity <= 0 || strategy_current_average <= 0.0)
                {
                    result.strategy_basis_unknown = true;
                }
                else
                {
                    result.strategy_realized_pnl = (price - strategy_current_average) * quantity
                                                    - result.commission - result.tax;
                }

                int strategy_new_quantity = strategy_pre_quantity - quantity;

                if (strategy_new_quantity <= 0)
                {
                    strategy_positions_.erase(strategy_key);
                    strategy_average_prices_.erase(strategy_key);
                }
                else
                {
                    strategy_positions_[strategy_key] = strategy_new_quantity;
                }
            }
        }

        if (side == OrderSide::BUY)
        {
            // 실체결분만 원장에 반영 (부분체결도 정확) — 평단 분모는 실체결 수량
            int new_quantity = pre_quantity + quantity;
            average_prices_[key] = (new_quantity > 0)
                ? (pre_quantity * current_average + quantity * price) / new_quantity
                : price;
            positions_[key] = new_quantity;

            if (pre_quantity <= 0)
            {
                opened_at_[key] = Clock::now(); // 0에서 열린 시각 — 교체 최소 보유 판정 기준
            }

            result.average_price = average_prices_[key];
            result.net_quantity   = new_quantity;

            // 당일 매수분은 당일 매도 가능하다.
            sellable_[key] = (sellable_.count(key) ? sellable_[key] : pre_quantity) + quantity;

            // 선점 해제 (BUY 선점은 +였으므로 -quantity). on_cancel과 같은 가드를 둔다 —
            //  선점이 없는데 빼면 음수 선점이 생겨 이후 한도·슬롯 계산이 왜곡된다
            //  (잔고 재시드분처럼 게이트가 선점을 잡은 적 없는 포지션의 체결이 이 경로로 온다).
            release_reservation(key, -quantity);
        }
        else // SELL
        {
            int new_quantity = pre_quantity - quantity;

            if (new_quantity < 0)
            {
                new_quantity = 0;  // 공매도 미지원 — 보유 초과 매도는 0으로 클램프
            }

            // 평단 미상(원장이 종목을 모름·재기동 후 미시드)이면 손익을 계산할 수 없다.
            //  0으로 곱하면 매도대금 전액이 이익으로 적립되므로 0을 두고 플래그로 알린다.
            if (!average_prices_.count(key) || current_average <= 0.0)
            {
                result.basis_unknown = true;
                result.realized_pnl  = 0.0;
            }
            else
            {
                result.realized_pnl = (price - current_average) * quantity
                                      - result.commission - result.tax;
            }

            result.average_price = current_average; // SELL 후 평균단가 불변
            result.net_quantity   = new_quantity;

            if (new_quantity == 0)
            {
                positions_.erase(key);
                average_prices_.erase(key); // 포지션 청산 시 평균단가 초기화
                opened_at_.erase(key);
                sellable_.erase(key);
            }
            else
            {
                positions_[key] = new_quantity;
                int sellable_after = (sellable_.count(key) ? sellable_[key] : pre_quantity) - quantity;
                sellable_[key] = sellable_after > 0 ? sellable_after : 0;
            }

            // 선점 해제 (SELL 선점은 -였으므로 +quantity). 가드가 없으면 선점이 없던 종목의
            //  매도 체결이 reserved_[k] = +quantity를 만들어 내고, slots_full()이 포지션도 없는
            //  종목의 슬롯을 점유로 세어 비운 자리가 그날 내내 열리지 않는다.
            release_reservation(key, quantity);
        }

        // FILL 기록 — 같은 락 안이라 파일 순서가 원장 갱신 순서와 같다. 실현손익은 참고용(리플레이는 다시 계산한다).
        ledger_journal::Record record = make_order_record(ledger_journal::Kind::FILL, side, quantity, reference);
        record.price                  = price;
        record.pnl                    = result.realized_pnl;
        ledger_journal::put_string(record.strategy, sizeof(record.strategy), strategies_.name(strategy).view());
        journal_append(record, account, ticker);
    }

    if (side == OrderSide::BUY)
    {
        // 매수 수수료도 발생 즉시 비용으로 인식한다 — average_price에 얹으면 평단 표시가
        //  실제 체결가와 어긋나므로, daily_pnl_에서 바로 뺀다.
        add_realized_pnl(-result.commission);
    }
    else if (side == OrderSide::SELL && !result.basis_unknown)
    {
        add_realized_pnl(result.realized_pnl);
    }

    return result;
}

// ─── 교체 진입 ──────────────────────────────────────────────────────────────
//  슬롯이 꽉 찼을 때 "먼저 온 순서"가 하루 종일 자리를 지키는 것을 막는다.
//  락 순서: priority_mutex_ → displace_mutex_ → positions_mutex_ 를 겹치지 않고 차례로 잡는다
//  (check()의 한도 거부 문구만 positions_mutex_ 안에서 displace_mtx_를 읽는다 — 역순 중첩 금지)
//  (헤더의 중첩 금지 규약 유지 — 각 구간에서 필요한 값만 복사해 나온다).
bool OrderGate::slots_full() const
{
    if (config_.max_concurrent_positions <= 0)
    {
        return false;
    }

    return open_slot_count() >= static_cast<size_t>(config_.max_concurrent_positions);
}

size_t OrderGate::open_slot_count() const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    size_t open = 0;

    for (const auto& entry : positions_)
    {
        if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
        {
            ++open;
        }
    }

    for (const auto& entry : reserved_)
    {
        if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
        {
            auto iterator = positions_.find(entry.first);

            if (iterator == positions_.end() || iterator->second <= 0)
            {
                ++open;
            }
        }
    }

    return open;
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

    if (config_.max_gross_exposure_percent <= 0.0 || equity <= 0.0)
    {
        return false;
    }

    double gross = 0.0;
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);

        for (const auto& entry : positions_)
        {
            if (entry.second <= 0)
            {
                continue;
            }

            auto average_price_iterator = average_prices_.find(entry.first);
            gross += entry.second * (average_price_iterator != average_prices_.end() ? average_price_iterator->second : 0.0);
        }

        for (const auto& entry : reserved_)
        {
            if (entry.second <= 0)
            {
                continue;
            }

            auto reserved_price_iterator = reserved_price_.find(entry.first);
            gross += entry.second * (reserved_price_iterator != reserved_price_.end() ? reserved_price_iterator->second : 0.0);
        }
    }

    // 상한의 95%를 넘으면 여력 없음으로 본다. 정확히 상한에 닿기를 기다리면 한 종목분
    //  명목이 애매하게 남아 교체도 매수도 안 되는 구간이 생긴다.
    return gross >= config_.max_gross_exposure_percent * equity * 0.95;
}

OrderGate::DisplacePlan OrderGate::plan_displacement(const std::string& account, symbol::SymbolId new_symbol) const
{
    DisplacePlan plan;

    // 거절 사유는 한 곳에서 기록한다(뒤의 거부 문구가 읽는다). 락 안에서는 부르지 않는다.
    auto decline = [&](std::string why) -> DisplacePlan
    {
        {
            std::lock_guard<std::mutex> lock(displace_mutex_);
            displace_decline_[new_symbol] = why;
        }

        plan.reason = std::move(why);
        return plan;
    };

    if (!config_.displace_enabled || config_.max_concurrent_positions <= 0)
    {
        return plan;
    }

    // (0) 한도를 넘겨 들고 있으면(한도를 내린 날·이월 보유가 많은 날) 최약체 하나를 비워도 자리가 안 난다.
    //  그래도 비우면 수혜 종목의 보류 매수는 만료되고 매도만 남는다(09-14 실측: 한도 40→30 뒤 204270·388050
    //  이월분이 z=-2.5로 밀려 나갔는데 105560·067290은 끝내 못 들어옴). 보유가 한도 밑으로 내려갈 때까지 교체는 쉰다.
    {
        const auto open = open_slot_count();
        const auto capture  = static_cast<size_t>(config_.max_concurrent_positions);

        if (open > capture)
        {
            return decline(std::format("보유 {} > 한도 {} — 하나 비워도 자리가 안 나 교체 보류", open, capture));
        }
    }

    // (1) 신규 종목의 점수. 점수를 모르면 교체 근거가 없다.
    //  표는 불변 스냅샷이라 포인터만 들고 락 밖에서 읽는다 — 맵을 통째로 복사해 들고 나오던 비용이 없다.
    const std::shared_ptr<const PriorityTable> table = priority_snapshot();

    if (!table || !z_of(*table, new_symbol, plan.new_z))
    {
        return decline("신규 종목 점수 없음");
    }

    // (2) 당일 교체 횟수·슬롯 예약 상태. 이미 비워 둔 슬롯이 있으면 또 비우지 않는다.
    const auto now = Clock::now();
    std::string why;
    {
        std::lock_guard<std::mutex> lock(displace_mutex_);

        if (config_.displace_max_per_day > 0 && displace_count_ >= config_.displace_max_per_day)
        {
            why = std::format("당일 교체 횟수 {}/{} 소진", displace_count_, config_.displace_max_per_day);
        }
        else if (slot_reserved_for_ != symbol::kNone && now < slot_reserved_until_)
        {
            // 직전 교체로 비운 자리가 아직 안 찼다
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(slot_reserved_until_ - now).count();
            why = std::format("비운 자리를 {}가 쓰는 중({}초 남음)", symbols_->name(slot_reserved_for_).view(), left);
        }
        else if (new_symbol < displace_cooldown_until_.size())
        {
            const TimePoint cooldown_until = displace_cooldown_until_[new_symbol];

            if (cooldown_until != TimePoint{} && now < cooldown_until)
            {
                // 방금 밀려난 종목이 곧장 되돌아오는 핑퐁 차단
                const auto left = std::chrono::duration_cast<std::chrono::seconds>(cooldown_until - now).count();
                why = std::format("밀려난 종목 재진입 대기({}초 남음)", left);
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
        std::lock_guard<std::mutex> lock(positions_mutex_);

        for (const auto& entry : positions_)
        {
            if (entry.second <= 0)
            {
                continue;
            }

            const PosKey& key = entry.first;

            if (key.symbol == new_symbol || slot_exempt_.contains(key.symbol)) // 바스켓 소유 종목은 교체 후보가 아니다 [why D-109]
            {
                continue;
            }

            double cand_z = 0.0;

            if (z_of(*table, key.symbol, cand_z))
            {
                // 점수를 안다
            }
            else if (config_.displace_unscored_z != 0.0)
            {
                // 오늘 어느 슬리브의 스캔에도 안 잡힌 보유분. 점수가 없는 게 아니라 오늘 필터를
                //  통과하지 못한 것이라, 통과한 종목들 아래로 내려 교체 후보에 넣는다.
                //  이 경로가 없으면 전일 이월분이 슬롯을 영구히 점유한다.
                cand_z = config_.displace_unscored_z;
            }
            else
            {
                continue; // 점수 미상 — 교체 대상 아님(기존 동작)
            }

            // 미체결 매도가 이미 걸린 종목은 건드리지 않는다(중복 매도).
            auto reserved_found = reserved_.find(key);
            const int sell_pending = (reserved_found != reserved_.end() && reserved_found->second < 0) ? -reserved_found->second : 0;

            if (sell_pending >= entry.second)
            {
                continue;
            }

            // 이전 세션이 낸 미체결 매도는 reserved_에 없다 — 프로세스 메모리라 재기동으로
            //  사라졌다. 그 수량은 잔고 시드(ord_psbl_qty)를 거쳐 sellable_에만 남으므로
            //  여기서도 같이 본다. 안 보면 팔 수 없는 종목을 매번 최약체로 골라 교체가 헛돈다
            //  (09-09: 000215 4회, 001120 1회. 그동안 진짜 팔 수 있는 하위 종목은 그대로 있었다).
            //  상한 계산은 clamp_buy_qty의 SELL 분기와 같은 규칙을 쓴다.
            int cand_cap = entry.second;
            auto sellable_iterator = sellable_.find(key);

            if (sellable_iterator != sellable_.end() && sellable_iterator->second < cand_cap)
            {
                cand_cap = sellable_iterator->second;
            }

            if (cand_cap - sell_pending <= 0)
            {
                continue;
            }

            // 방금 산 종목은 빼지 않는다. 사자마자 파는 왕복은 비용만 남는다.
            auto opened_iterator = opened_at_.find(key);

            if (opened_iterator != opened_at_.end() && config_.displace_min_hold_sec > 0 &&
                now - opened_iterator->second < std::chrono::seconds(config_.displace_min_hold_sec))
            {
                continue;
            }

            if (best_key.symbol == symbol::kNone || cand_z < worst_z)
            {
                best_key = key;
                worst_z  = cand_z;
            }
        }

        if (best_key.symbol == symbol::kNone)
        {
            why = "내보낼 보유 종목 없음(전부 최소 보유 미달·매도 중·매도 불가)";
        }
        else if (plan.new_z - worst_z < config_.displace_min_z_gap)
        {
            // 격차가 잡음 수준이면 비용만 나간다
            why = std::format("점수 격차 {:.2f}σ < {:.2f}σ (최약체 z={:.2f})",
                              plan.new_z - worst_z, config_.displace_min_z_gap, worst_z);
        }
    }

    if (!why.empty())
    {
        return decline(why);
    }

    {
        std::lock_guard<std::mutex> lock(positions_mutex_);

        plan.account = account_of(best_key);
        plan.ticker  = ticker_of(best_key).string();
        plan.symbol  = best_key.symbol;
        auto position_iterator = positions_.find(best_key);
        auto reserved_found  = reserved_.find(best_key);
        const int sell_pending = (reserved_found != reserved_.end() && reserved_found->second < 0) ? -reserved_found->second : 0;
        int capture = (position_iterator != positions_.end() ? position_iterator->second : 0);
        auto sellable_iterator = sellable_.find(best_key);

        if (sellable_iterator != sellable_.end() && sellable_iterator->second < capture)
        {
            capture = sellable_iterator->second;
        }

        plan.quantity = capture - sell_pending;
        auto average_price_iterator = average_prices_.find(best_key);
        plan.average_price = (average_price_iterator != average_prices_.end()) ? average_price_iterator->second : 0.0;
    }

    if (plan.quantity <= 0)
    {
        return decline("최약체 " + plan.ticker + " 매도 가능 수량 0");
    }

    (void)account; // 계좌는 피교체 종목 쪽에서 복원한다(신호 계좌와 다를 수 있음)
    plan.victim_z = worst_z;
    plan.reason = std::format("교체 진입 — {}(z={:.2f})가 {}(z={:.2f})보다 {:.2f}σ 높아 슬롯을 넘긴다",
                              symbols_->name(new_symbol).view(), plan.new_z, plan.ticker, plan.victim_z,
                              plan.new_z - plan.victim_z);
    plan.ok = true;
    {
        std::lock_guard<std::mutex> lock(displace_mutex_);
        displace_decline_.erase(new_symbol);
    }

    return plan;
}

void OrderGate::note_displacement(const DisplacePlan& plan, symbol::SymbolId beneficiary)
{
    if (!plan.ok)
    {
        return;
    }

    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(displace_mutex_);
    ++displace_count_;

    if (config_.displace_cooldown_sec > 0 && plan.symbol != symbol::kNone)
    {
        // 종목 테이블 용량만큼 한 번만 늘린다 — id는 용량을 넘지 않으므로 그 뒤로는 인덱스 대입뿐이다.
        if (plan.symbol >= displace_cooldown_until_.size())
        {
            displace_cooldown_until_.resize(std::max<size_t>(symbols_->capacity(), plan.symbol + 1));
        }

        displace_cooldown_until_[plan.symbol] = now + std::chrono::seconds(config_.displace_cooldown_sec);
    }

    // 비운 슬롯을 수혜 종목에 예약한다. 예약이 없으면 매도 체결 직후 다른 종목이 가로채고,
    //  그러면 교체 비용만 치르고 정작 사려던 종목은 또 못 산다.
    slot_reserved_for_   = beneficiary;
    slot_reserved_until_ = now + std::chrono::seconds(config_.displace_slot_hold_sec > 0
                                                     ? config_.displace_slot_hold_sec : 120);
}

// ─── 일별 리셋 (장 시작 시) ─────────────────────────────────────────────────
void OrderGate::reset_daily()
{
    set_daily_pnl(0.0); // 저널에도 DAILY_PNL 0 — 리셋 전 체결이 리플레이로 되살아나지 않게

    {
        std::lock_guard<std::mutex> lock(rate_mutex_);
        order_times_min_.clear();
        order_times_sec_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(deduplicate_mutex_);
        last_signal_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(displace_mutex_);
        std::fill(displace_cooldown_until_.begin(), displace_cooldown_until_.end(), TimePoint{});
        slot_reserved_for_   = symbol::kNone;
        slot_reserved_until_ = TimePoint{};
        displace_count_ = 0;
    }

    {
        // 미체결 선점은 일일 만료 (KIS 당일 주문은 장 마감 소멸 → 다음날 잘못된 차단 방지).
        // C5(MM-1): 명시적 취소는 on_cancel()로 일원화. reserved_.clear()는 장 마감 안전망
        //   — 취소 없이 장 마감까지 미체결로 만료된 분의 선점을 청소한다.
        std::lock_guard<std::mutex> lock(positions_mutex_);
        reserved_.clear();
        reserved_price_.clear();
    }

    // average_prices_ / positions_ 는 영속 원장 — 장 시작에 초기화하지 않는다
}

// ─── 원장 키 — (계좌 id, 종목 id) ─────────────────────────────────────────────
//  계좌는 기동 중 몇 개뿐이라 벡터를 앞에서부터 비교한다(해시보다 싸다). positions_mutex_ 아래에서만 부른다.
uint32_t OrderGate::account_index(std::string_view account, bool create)
{
    for (size_t index = 0; index < account_names_.size(); ++index)
    {
        if (account_names_[index] == account)
        {
            return static_cast<uint32_t>(index);
        }
    }

    if (!create)
    {
        return kUnknownAccount;
    }

    account_names_.emplace_back(account);
    return static_cast<uint32_t>(account_names_.size() - 1);
}

OrderGate::PosKey OrderGate::make_key(std::string_view account, std::string_view ticker)
{
    const uint32_t         account_id = account_index(account, true);
    const symbol::SymbolId symbol     = symbols_->intern(ticker);

    if (symbol == symbol::kNone)
    {
        throw std::runtime_error(std::format("OrderGate: 종목 테이블이 가득 차 원장 키를 못 만든다 ticker={} capacity={}",
                                             ticker, symbols_->capacity()));
    }

    return PosKey{account_id, symbol};
}

OrderGate::PosKey OrderGate::lookup_key(std::string_view account, symbol::SymbolId symbol) const
{
    // const 경로라 등록하지 않는다 — account_index(create=false)와 같은 탐색이지만 const 멤버로 둔다.
    for (size_t index = 0; index < account_names_.size(); ++index)
    {
        if (account_names_[index] == account)
        {
            return PosKey{static_cast<uint32_t>(index), symbol};
        }
    }

    return PosKey{kUnknownAccount, symbol};
}

OrderGate::PosKey OrderGate::lookup_key(std::string_view account, std::string_view ticker) const
{
    return lookup_key(account, symbols_->lookup(ticker));
}

OrderGate::PosKey OrderGate::lookup_key(const OrderSignal& signal) const
{
    if (symbols_ != &own_symbols_ && signal.symbol_id != symbol::kNone)
    {
        return lookup_key(signal.account_id, signal.symbol_id);
    }

    return lookup_key(signal.account_id, signal.ticker);
}

OrderGate::PosKey OrderGate::register_key(const OrderSignal& signal)
{
    if (symbols_ != &own_symbols_ && signal.symbol_id != symbol::kNone)
    {
        return PosKey{account_index(signal.account_id, true), signal.symbol_id};
    }

    return make_key(signal.account_id, signal.ticker);
}

// ─── 진입 우선순위 표 ─────────────────────────────────────────────────────────
void OrderGate::set_entry_priority(const std::vector<PriorityEntry>& entries, int total)
{
    auto table   = std::make_shared<PriorityTable>();
    table->total = total;

    // id 배열은 종목 테이블 용량만큼 — id가 용량을 넘지 않으므로 경계 검사가 index < size() 하나로 끝난다.
    size_t extent = symbols_->capacity();

    for (const PriorityEntry& entry : entries)
    {
        extent = std::max<size_t>(extent, static_cast<size_t>(entry.symbol) + 1);
    }

    table->rank_by_symbol.assign(extent, 0);
    table->below_by_symbol.assign(extent, 0);
    table->z_by_symbol.assign(extent, 0.0);
    std::vector<symbol::SymbolId> symbols_by_rank; // 랭크 오름차순 id — below_by_symbol을 만들 때만 쓴다
    symbols_by_rank.reserve(entries.size());

    for (const PriorityEntry& entry : entries)
    {
        if (entry.symbol == symbol::kNone || entry.rank <= 0)
        {
            continue;
        }

        if (table->rank_by_symbol[entry.symbol] == 0)
        {
            symbols_by_rank.push_back(entry.symbol);
        }

        table->rank_by_symbol[entry.symbol] = entry.rank;
        table->z_by_symbol[entry.symbol]    = entry.z_score;
    }

    std::sort(symbols_by_rank.begin(), symbols_by_rank.end(),
              [&](symbol::SymbolId left, symbol::SymbolId right) {
                  return table->rank_by_symbol[left] < table->rank_by_symbol[right];
              });

    // 같은 랭크가 여럿이면 그 묶음의 첫 위치가 "나보다 위" 수다.
    for (size_t index = 0; index < symbols_by_rank.size(); ++index)
    {
        const symbol::SymbolId symbol = symbols_by_rank[index];
        int32_t                below  = static_cast<int32_t>(index);

        while (below > 0 && table->rank_by_symbol[symbols_by_rank[static_cast<size_t>(below) - 1]] ==
                                table->rank_by_symbol[symbol])
        {
            --below;
        }

        table->below_by_symbol[symbol] = below;
    }

    std::lock_guard<std::mutex> lock(priority_mutex_);
    priority_ = std::move(table);
}

std::shared_ptr<const OrderGate::PriorityTable> OrderGate::priority_snapshot() const
{
    std::lock_guard<std::mutex> lock(priority_mutex_);
    return priority_;
}

int OrderGate::rank_of(const PriorityTable& table, symbol::SymbolId symbol) noexcept
{
    return symbol < table.rank_by_symbol.size() ? table.rank_by_symbol[symbol] : 0;
}

bool OrderGate::z_of(const PriorityTable& table, symbol::SymbolId symbol, double& z_score) noexcept
{
    if (rank_of(table, symbol) == 0)
    {
        return false;
    }

    z_score = table.z_by_symbol[symbol];
    return true;
}

symbol::Ticker OrderGate::ticker_of(const PosKey& key) const
{
    return symbols_->name(key.symbol);
}

const std::string& OrderGate::account_of(const PosKey& key) const
{
    return account_names_[key.account];
}

// ─── 조회 (계좌별) ───────────────────────────────────────────────────────────
int OrderGate::position(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = positions_.find(lookup_key(account, ticker));
    return (iterator != positions_.end()) ? iterator->second : 0;
}

int OrderGate::position(const std::string& account, symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = positions_.find(lookup_key(account, symbol));
    return (iterator != positions_.end()) ? iterator->second : 0;
}

int OrderGate::reserved(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = reserved_.find(lookup_key(account, ticker));
    return (iterator != reserved_.end()) ? iterator->second : 0;
}

int OrderGate::reserved(const std::string& account, symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = reserved_.find(lookup_key(account, symbol));
    return (iterator != reserved_.end()) ? iterator->second : 0;
}

double OrderGate::average_price(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = average_prices_.find(lookup_key(account, ticker));
    return (iterator != average_prices_.end()) ? iterator->second : 0.0;
}

OrderGate::EntrySnapshot OrderGate::entry_snapshot(const std::string& account, const std::string& ticker) const
{
    EntrySnapshot snapshot;
    std::lock_guard<std::mutex> lock(positions_mutex_);

    const auto key = lookup_key(account, ticker);
    auto position_iterator = positions_.find(key);
    snapshot.position = (position_iterator != positions_.end()) ? position_iterator->second : 0;

    auto reserved_it = reserved_.find(key);
    snapshot.reserved = (reserved_it != reserved_.end()) ? reserved_it->second : 0;

    if (config_.max_concurrent_positions > 0)
    {
        // 바스켓 슬리브 소유 종목은 자리를 안 먹는다 — check()·slots_full()·plan_displacement()와 같은 규칙이다.
        //  여기만 빼먹고 세던 때는, 교체를 켠 날 바스켓을 든 계좌에서 "자리 꽉 참"이 잘못 나고
        //  plan_displacement가 자리 남음을 따로 안 보기 때문에 멀쩡한 최약체를 팔아 이미 있던 자리를 만들었다.
        //  [why D-109]
        size_t open = 0;

        for (const auto& entry : positions_)
        {
            if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
            {
                ++open;
            }
        }

        for (const auto& entry : reserved_)
        {
            if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
            {
                auto iterator = positions_.find(entry.first);

                if (iterator == positions_.end() || iterator->second <= 0)
                {
                    ++open;
                }
            }
        }

        snapshot.slots_full = open >= static_cast<size_t>(config_.max_concurrent_positions);
    }

    return snapshot;
}

double OrderGate::daily_pnl() const
{
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    return daily_pnl_;
}

// ─── 보유 포지션 스냅샷 (G3 강제청산) ────────────────────────────────────────
std::vector<OrderGate::HeldPos> OrderGate::snapshot_positions() const
{
    std::vector<HeldPos> out;
    std::lock_guard<std::mutex> lock(positions_mutex_);
    out.reserve(positions_.size());

    for (const auto& entry : positions_)
    {
        if (entry.second <= 0)
        {
            continue; // 롱 보유분만 청산 대상
        }

        const PosKey& key = entry.first;
        HeldPos held_position;
        held_position.account = account_of(key);
        held_position.ticker  = ticker_of(key).string();
        held_position.symbol  = key.symbol;
        held_position.quantity     = entry.second;
        auto average_price_iterator = average_prices_.find(key);
        held_position.average_price = (average_price_iterator != average_prices_.end()) ? average_price_iterator->second : 0.0;
        held_position.slot_exempt   = slot_exempt_.contains(key.symbol);
        out.push_back(std::move(held_position));
    }

    return out;
}

// ─── 장부 사본 발행 (D-114 단계 2.5) ─────────────────────────────────────────
//  전략 쪽이 OrderGate를 직접 부르는 자리를 이 사본 하나로 바꾸기 위한 채우기다. 지금은 채우기만 하고
//  읽는 쪽은 없다 — 배선은 뒤에 한다. 한 바퀴에 한 번 부르므로 사본을 읽는 쪽이 밀리지 않는다.
//
//  [inv] 판 안에서는 사본을 읽지 않는다. 판 번호가 홀수인 동안 읽는 쪽 함수(collect_rows·row)는
//  짝수가 될 때까지 도는데, 그 짝수를 만드는 것이 자기 자신이라 영영 안 끝난다.
//  그래서 미체결(reserved_)을 먼저 싣고 보유(positions_)를 돌 때 매도가능을 같이 셈한다.
void OrderGate::publish_ledger(ipc::LedgerSnapshot& snapshot) const
{
    // 발행끼리 줄을 세운다. 판 번호를 둘이 동시에 뒤집으면 짝수인 순간이 안 와 읽는 쪽이 밀린다.
    std::lock_guard<std::mutex> publish_lock(ledger_publish_mutex_);

    // 자기 원자변수가 지키는 값들은 positions_mutex_ 밖에서 읽는다 — 잠금을 쥔 구간을 좁게 둔다.
    const double equity = equity_.load(std::memory_order_relaxed);

    snapshot.begin_publish();

    ipc::LedgerGlobals& globals      = snapshot.globals_for_write();
    globals.entry_scale              = entry_scale_.load();
    globals.max_notional_per_ticker  = config_.max_notional_per_ticker;
    globals.displace_unscored_z      = config_.displace_unscored_z;
    globals.max_concurrent_positions = config_.max_concurrent_positions;
    globals.displace_enabled         = config_.displace_enabled ? 1 : 0;
    // 세 원천을 OR한 결과만 싣는다. 어느 원천이 켰는지는 주문 쪽 일이다 — 한 원천의 자동 해제가
    //  다른 원천을 지우면 안 되므로 원천 자체는 가르지 않는다. [why D-091]
    globals.entry_halted             = (entry_halt_.load() || manual_buy_halt_.load() || strategy_down_halt_.load()) ? 1 : 0;
    globals.manual_sell_halted       = manual_sell_halt_.load() ? 1 : 0;

    uint64_t foreign_rows = 0;

    {
        std::lock_guard<std::mutex> lock(positions_mutex_);

        // 어느 계좌를 싣는가 — 한 프로세스는 한 계좌만 다룬다. 가장 작은 계좌 번호를 이번 판의 계좌로
        //  삼고(같은 원장이면 판마다 같은 답이 나온다), 다른 계좌 줄은 싣지 않고 센다.
        uint32_t account = kUnknownAccount;

        for (const auto& entry : positions_)
        {
            if (entry.second != 0 && entry.first.account < account)
            {
                account = entry.first.account;
            }
        }

        for (const auto& entry : reserved_)
        {
            if (entry.second != 0 && entry.first.account < account)
            {
                account = entry.first.account;
            }
        }

        // 이번 판의 계좌 이름. 실린 줄이 하나도 없으면(기동 직후) 0번 = ""을 쓴다 — 단일 계좌에서
        //  원장 키가 쓰는 이름이 그것이고, 이름을 비워 두면 전략 쪽이 강제청산 주문에 계좌를 못 적는다.
        {
            const std::string& account_name = account_names_[(account == kUnknownAccount) ? 0 : account];
            const size_t       copied       = (account_name.size() < sizeof(globals.account)) ? account_name.size()
                                                                                              : sizeof(globals.account) - 1;
            std::memcpy(globals.account, account_name.data(), copied);
            globals.account[copied] = '\0';
        }

        size_t open_slots = 0;
        double gross      = 0.0;

        // ① 미체결 선점 먼저. 보유를 돌 때 매도가능(상한 - 미체결 매도)을 한 번에 셈하기 위해서다.
        for (const auto& entry : reserved_)
        {
            const PosKey& key = entry.first;

            if (entry.second == 0)
            {
                continue;
            }

            if (key.account != account)
            {
                ++foreign_rows;
                continue;
            }

            const bool exempt = slot_exempt_.contains(key.symbol);

            ipc::LedgerRow& row = snapshot.row_for_write(key.symbol);
            row.reserved        = entry.second;
            row.slot_exempt     = exempt ? 1 : 0;

            if (entry.second > 0)
            {
                const auto position_iterator = positions_.find(key);

                // 보유 없이 매수 선점만 있는 종목도 자리를 하나 문다.
                if (!exempt && (position_iterator == positions_.end() || position_iterator->second <= 0))
                {
                    ++open_slots;
                }

                const auto reserved_price_iterator = reserved_price_.find(key);
                gross += entry.second * ((reserved_price_iterator != reserved_price_.end()) ? reserved_price_iterator->second : 0.0);
            }
        }

        // ② 보유. 매도가능은 sellable_view()와 같은 셈이다 — 보유와 KIS 상한 중 작은 쪽에서 미체결 매도를 뺀다.
        for (const auto& entry : positions_)
        {
            const PosKey& key = entry.first;

            if (entry.second == 0)
            {
                continue; // 닫힌 자리는 사본에 없는 것과 같다
            }

            if (key.account != account)
            {
                ++foreign_rows;
                continue;
            }

            const auto   average_price_iterator = average_prices_.find(key);
            const double average_price = (average_price_iterator != average_prices_.end()) ? average_price_iterator->second : 0.0;
            const bool   exempt        = slot_exempt_.contains(key.symbol);

            ipc::LedgerRow& row = snapshot.row_for_write(key.symbol);
            row.position        = entry.second;
            row.average_price   = average_price;
            row.slot_exempt     = exempt ? 1 : 0;

            if (entry.second > 0)
            {
                if (!exempt)
                {
                    ++open_slots;
                }

                gross += entry.second * average_price;

                int        sellable_limit    = entry.second;
                const auto sellable_iterator = sellable_.find(key);

                if (sellable_iterator != sellable_.end() && sellable_iterator->second < sellable_limit)
                {
                    sellable_limit = sellable_iterator->second;
                }

                const int pending_sell = (row.reserved < 0) ? -row.reserved : 0;
                row.sellable = (sellable_limit > pending_sell) ? (sellable_limit - pending_sell) : 0;
            }
        }

        // 자리(슬롯)와 예산(총노출) 중 하나만 막혀도 신규 종목을 열 여력이 없다. capacity_full()과 같은 셈이다.
        //  [inv] 슬롯 면제 종목은 자리를 먹지 않는다 — check()·open_slot_count()·entry_snapshot()과 같은
        //  규칙이다. 자리 세는 곳이 다섯인데 셈이 하나라도 다르면 전략과 게이트가 딴 판단을 한다. [why D-109]
        const bool slots_are_full = config_.max_concurrent_positions > 0
                                    && open_slots >= static_cast<size_t>(config_.max_concurrent_positions);
        const bool budget_is_full = config_.max_gross_exposure_percent > 0.0 && equity > 0.0
                                    && gross >= config_.max_gross_exposure_percent * equity * 0.95;

        globals.open_slot_count = static_cast<int32_t>(open_slots);
        globals.capacity_full   = (slots_are_full || budget_is_full) ? 1 : 0;
    }

    snapshot.end_publish();

    if (foreign_rows != 0)
    {
        ledger_foreign_account_rows_.fetch_add(foreign_rows, std::memory_order_relaxed);
    }
}

void OrderGate::set_slot_exempt(const std::vector<std::string>& tickers)
{
    std::unordered_set<symbol::SymbolId> next;
    next.reserve(tickers.size());

    for (const auto& ticker : tickers)
    {
        const symbol::SymbolId symbol = symbols_->intern(ticker);

        if (symbol != symbol::kNone)
        {
            next.insert(symbol);
        }
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    slot_exempt_ = std::move(next);
}

void OrderGate::set_slot_exempt_by_id(const std::vector<symbol::SymbolId>& symbols)
{
    std::unordered_set<symbol::SymbolId> next;
    next.reserve(symbols.size());

    for (const symbol::SymbolId symbol : symbols)
    {
        if (symbol != symbol::kNone)
        {
            next.insert(symbol);
        }
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    slot_exempt_ = std::move(next);
}

bool OrderGate::is_slot_exempt(symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    return slot_exempt_.contains(symbol);
}

std::vector<symbol::SymbolId> OrderGate::slot_exempt_symbols() const
{
    std::vector<symbol::SymbolId> out;
    std::lock_guard<std::mutex>   lock(positions_mutex_);
    out.assign(slot_exempt_.begin(), slot_exempt_.end());
    std::sort(out.begin(), out.end()); // 집합 순서는 해시 순 — 호출자가 같은 입력에 같은 순서를 받게 한다
    return out;
}

void OrderGate::on_accept(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          double price)
{
    (void)on_intent(account, ticker, side, quantity, price, OrderRef{});
}

void OrderGate::on_accept(const std::string& ticker, OrderSide side, int quantity, double price)
{
    on_accept(std::string(), ticker, side, quantity, price);
}

void OrderGate::seed_position(const std::string& account, const std::string& ticker, int quantity, double average_price)
{
    seed_position(account, ticker, quantity, average_price, -1);
}

void OrderGate::seed_position(const std::string& ticker, int quantity, double average_price)
{
    seed_position(std::string(), ticker, quantity, average_price, -1);
}

void OrderGate::on_cancel(const std::string& account, const std::string& ticker, OrderSide side, int quantity)
{
    on_cancel(account, ticker, side, quantity, OrderRef{});
}

void OrderGate::on_cancel(const std::string& ticker, OrderSide side, int quantity)
{
    on_cancel(std::string(), ticker, side, quantity);
}

void OrderGate::set_kill_switch(bool on)
{
    kill_switch_.store(on);
}

void OrderGate::set_entry_halt(bool on)
{
    entry_halt_.store(on);
}

void OrderGate::set_strategy_down_halt(bool on)
{
    strategy_down_halt_.store(on);
}

void OrderGate::set_manual_halt(OrderSide side, bool on)
{
    (side == OrderSide::SELL ? manual_sell_halt_ : manual_buy_halt_).store(on);
}

void OrderGate::set_entry_scale(double entry_scale)
{
    entry_scale_.store(entry_scale);
}

void OrderGate::set_pnl_stale(bool on)
{
    pnl_stale_.store(on);
}

size_t OrderGate::PosKeyHash::operator()(const PosKey& key) const noexcept
{
    // 두 32비트를 64비트 하나로 붙여 곱셈으로 섞는다. xor만 하면 (a,b)와 (b,a)가 같은 버킷에 간다.
    const uint64_t packed = (static_cast<uint64_t>(key.account) << 32) | key.symbol;
    const uint64_t mixed = packed * 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(mixed ^ (mixed >> 29));
}

size_t OrderGate::StrategyKeyHash::operator()(const StrategyKey& key) const noexcept
{
    const uint64_t packed = (static_cast<uint64_t>(key.strategy) << 32) | key.symbol;
    const uint64_t mixed = packed * 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(mixed ^ (mixed >> 29));
}

size_t OrderGate::SignalKeyHash::operator()(const SignalKey& key) const noexcept
{
    uint64_t mixed = (static_cast<uint64_t>(key.account) << 32) | key.symbol;
    mixed =
        (mixed ^ (static_cast<uint64_t>(key.strategy) << 8) ^ static_cast<uint64_t>(key.side)) * 0x9e3779b97f4a7c15ull;
    mixed ^= static_cast<uint64_t>(key.price) * 0xbf58476d1ce4e5b9ull;
    return static_cast<size_t>(mixed ^ (mixed >> 31));
}
