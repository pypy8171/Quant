#include "risk/OrderGate.h"

#include "ipc/LedgerSnapshot.h"
#include "risk/GateReasons.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <cmath>
#include <ctime>
#include <format>
#include <limits>

using Clock = std::chrono::steady_clock;

namespace
{

// 장중 잔여시간 비율 — 09:00에 1.0, 15:00 이후 0.0. 점수 우선순위 바를 오후로 갈수록 낮춘다.
//  빈 슬롯의 가치는 마감이 다가올수록 떨어진다. 14:30에 상위 종목을 기다리는 건 현금을
//  들고 하루를 끝내는 것과 같아서, 그때는 바를 없애고 아무나 받는 게 맞다.
constexpr int kSessionOpenMin  = 9 * 60;   // 09:00 KST
constexpr int kSessionBarEndMin = 15 * 60; // 15:00 KST — 이후로는 바 없음

// 금액을 주문 수량으로 옮긴다. 몫이 int 범위를 넘으면 캐스팅 결과가 정해져 있지 않아
//  x86 에서 INT_MIN 이 나오고, 호출한 쪽 끝의 `quantity > 0 ? quantity : 0` 이 그것을 0 으로
//  바꾼다 — 한도에 걸리지도 않은 주문이 조용히 수량 0 이 되는 것이다. 그래서 잘라서 받는다.
//  현금이 조 단위인 구성(부하시험)에서 싼 종목을 만나면 바로 넘는다.
int quantity_from_notional(double notional, double price)
{
    if (!(price > 0.0) || !std::isfinite(notional))
    {
        return 0;
    }

    const double quantity = notional / price;

    if (quantity >= static_cast<double>(std::numeric_limits<int>::max()))
    {
        return std::numeric_limits<int>::max();
    }

    if (quantity <= static_cast<double>(std::numeric_limits<int>::lowest()))
    {
        return std::numeric_limits<int>::lowest();
    }

    return static_cast<int>(quantity);
}

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
// check()+on_intent (on_accept는 테스트·도구용)이 직렬 실행돼 검사~사용 사이 경합(TOCTOU, Time-Of-Check-To-Time-Of-Use)이
// 없다. 멀티 producer로 확장하려면
// check()+on_intent (on_accept는 테스트·도구용)를 하나의 임계구역으로 묶어 원자적 reserve로 만들어야 한다.
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
        const PositionLedger::Reader ledger = ledger_.read();
        const PosKey key = ledger.keys().lookup(signal);
        auto position_iterator = ledger.positions().find(key);

        if (position_iterator == ledger.positions().end() || position_iterator->second <= 0)
        {
            return quantity;
        }

        auto reserved_iterator = ledger.reserved().find(key);
        const int sell_pending = (reserved_iterator != ledger.reserved().end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;
        // 상한은 보유수량이 아니라 매도가능수량이다. 기동 전 세션이 남긴 미체결 매도는
        //  reserved_에 없고(프로세스 메모리라 재기동으로 사라진다) 잔고의 ord_psbl_qty에만 보인다.
        int holding_ceiling = position_iterator->second;
        auto strategy_iterator = ledger.sellable().find(key);

        if (strategy_iterator != ledger.sellable().end() && strategy_iterator->second < holding_ceiling)
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
        const int quantity_ceiling = quantity_from_notional(config_.max_notional_per_order, evaluation_price);

        if (quantity_ceiling < quantity)
        {
            quantity = quantity_ceiling;
        }
    }

    {
        const PositionLedger::Reader ledger = ledger_.read();
        const PosKey key = ledger.keys().lookup(signal);
        auto position_iterator = ledger.positions().find(key);
        auto reserved_iterator = ledger.reserved().find(key);
        const int current_quantity = (position_iterator != ledger.positions().end() ? position_iterator->second : 0) +
                            (reserved_iterator != ledger.reserved().end() ? reserved_iterator->second : 0);

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
            const int room = quantity_from_notional(config_.max_notional_per_ticker, evaluation_price) - current_quantity;

            if (room < quantity)
            {
                quantity = room;
            }
        }

        // 총노출(§3d)도 같은 방식으로 남은 여유를 수량으로 환산한다. 보유는 평단, 선점은 선점가로
        //  재는 것까지 check()와 동일하게 둔다.
        const double equity = ledger_.equity();

        if (config_.max_gross_exposure_percent > 0.0 && equity > 0.0 && evaluation_price > 0.0)
        {
            double gross = 0.0;

            for (const auto& entry : ledger.positions())
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto average_price_iterator = ledger.average_prices().find(entry.first);
                gross += entry.second * (average_price_iterator != ledger.average_prices().end() ? average_price_iterator->second : 0.0);
            }

            for (const auto& entry : ledger.reserved())
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto reserved_price_iterator = ledger.reserved_price().find(entry.first);
                gross += entry.second * (reserved_price_iterator != ledger.reserved_price().end() ? reserved_price_iterator->second : 0.0);
            }

            const double exposure_ceiling  = config_.max_gross_exposure_percent * equity;
            const int    room = quantity_from_notional(exposure_ceiling - gross, evaluation_price);

            if (room < quantity)
            {
                quantity = room;
            }
        }

        // 주문가능현금 클램프. 평가금이 아니라 현금이 매수의 진짜 상한이다. 우리가 이미 낸
        //  미체결 매수 명목을 빼는 것은 보수적으로 중복차감이 될 수 있으나(브로커 값이 이미
        //  반영했을 수 있다), 모자라게 사는 쪽이 전량 거부보다 낫다.
        const double cash = ledger_.available_cash();

        if (cash > 0.0 && evaluation_price > 0.0)
        {
            double pending_buy = 0.0;

            for (const auto& entry : ledger.reserved())
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto reserved_price_iterator = ledger.reserved_price().find(entry.first);
                pending_buy += entry.second * (reserved_price_iterator != ledger.reserved_price().end() ? reserved_price_iterator->second : 0.0);
            }

            const int room = quantity_from_notional(cash - pending_buy, evaluation_price);

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
            //  (수량 한도는 위에서 이미 걸렸다.) 경고는 비동기 로거 큐로 넘긴다 — 게이트 스레드에서 콘솔 I/O를 하지 않는다.
            if (signal.side == OrderSide::SELL)
            {
                LOG_WARN("[OrderGate] " + signal.ticker + " SELL " + reason_text + " — 청산이라 통과");
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
    const PosKey key = ledger_.register_signal(signal);

    if (signal.side == OrderSide::BUY)
    {
        const PositionLedger::Reader ledger = ledger_.read();
        const auto filled_iterator   = ledger.positions().find(key);
        const auto reserved_iterator = ledger.reserved().find(key);
        int filled   = filled_iterator != ledger.positions().end() ? filled_iterator->second : 0;
        int reserved = reserved_iterator != ledger.reserved().end() ? reserved_iterator->second : 0;
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
            filled == 0 && reserved == 0 && !ledger.slot_exempt().contains(key.symbol))
        {
            size_t open = 0;
            size_t held = 0;   // 그중 실보유. 거부 문구에서 유령 선점과 갈라 보려고 따로 센다

            // 3c-2(아래)가 쓰는 "나보다 랭크가 위인데 이미 차지된 종목 수"를 같은 순회에서 센다 — 표를 도는 대신
            //  원장(보유·선점 ≤ 슬롯 수)을 돈다. 표는 불변 스냅샷이라 락 밖 포인터로 읽는다.
            const std::shared_ptr<const PriorityTable> table =
                config_.entry_priority_enabled ? entry_priority_.snapshot() : nullptr;
            const int rank        = table ? EntryPriority::rank_of(*table, key.symbol) : 0;
            int       taken_ahead = 0;

            auto counts_ahead = [&](const PosKey& taken_key)
            {
                if (rank <= 0 || taken_key.account != key.account || taken_key.symbol == key.symbol)
                {
                    return false;
                }

                const int taken_rank = EntryPriority::rank_of(*table, taken_key.symbol);
                return taken_rank > 0 && taken_rank < rank;
            };

            for (const auto& entry : ledger.positions())
            {
                if (entry.second > 0 && !ledger.slot_exempt().contains(entry.first.symbol))
                {
                    ++open;
                    ++held;
                    taken_ahead += counts_ahead(entry.first) ? 1 : 0;
                }
            }

            for (const auto& entry : ledger.reserved())
            {
                if (entry.second > 0 && !ledger.slot_exempt().contains(entry.first.symbol))
                {
                    auto iterator = ledger.positions().find(entry.first);

                    if (iterator == ledger.positions().end() || iterator->second <= 0)
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
                // [lock-order] positions_mutex_ → displace_mutex_(EntryPriority 안). 반대 순서로 겹쳐 잡는 곳은 없다
                //  (plan_displacement·note_displacement는 displace_mutex_를 단독 구간으로만 쓴다).
                if (!entry_priority_.append_decline(key.symbol, reason_text))
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
                symbol::SymbolId reserved_for = symbol::kNone;
                const EntryPriority::Admission admission = entry_priority_.admit(key.symbol, Clock::now(), reserved_for);

                if (admission == EntryPriority::Admission::Cooling)
                {
                    reject_reason = "교체 쿨다운 중 — 방금 슬롯을 내준 종목의 재진입 금지";
                    return false;
                }

                if (admission == EntryPriority::Admission::SlotReserved)
                {
                    reject_reason = std::format("교체로 비운 슬롯 예약분 ({}) — 다른 종목 진입 보류",
                                                ledger_.symbols().name(reserved_for).view());
                    return false;
                }
            }

            // 3c-2. 점수 우선순위 바 — 남은 슬롯이 적을수록 더 높은 점수를 요구한다.
            //   eff_rank/pool ≤ 1 − (open/max_concurrent_positions) × session_remaining_ratio()
            //   eff_rank = below_by_symbol − taken_ahead + 1, pool = max(total, max_concurrent_positions)
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
        //     자본의 max_gross_exposure_percent를 넘게 만드는 BUY를 차단(신규·물타기 공통). 청산(SELL)은 위에서 제외.
        //     종목당 명목(15%)×동시보유(10)=150% 같은 과노출을 총합 단에서 막는다. equity 미주입(0)이면 비활성.
        //     보유분은 원가(평단)로, 분모 equity는 시장 총평가금이라 상승장 과소·하락장 과대의 근사(수용).
        const double equity = ledger_.equity();

        if (config_.max_gross_exposure_percent > 0.0 && equity > 0.0 && evaluation_price > 0.0)
        {
            double gross = 0.0;

            for (const auto& entry : ledger.positions())
            {
                if (entry.second <= 0)
                {
                    continue;
                }

                auto average_price_iterator = ledger.average_prices().find(entry.first);
                gross += entry.second * (average_price_iterator != ledger.average_prices().end() ? average_price_iterator->second : 0.0);
            }

            for (const auto& entry : ledger.reserved())
            {
                if (entry.second <= 0)
                {
                    continue;  // BUY 선점(+)만 노출 증가. SELL 선점(-)은 축소라 보수적으로 무시
                }

                auto reserved_price_iterator = ledger.reserved_price().find(entry.first);
                gross += entry.second * (reserved_price_iterator != ledger.reserved_price().end() ? reserved_price_iterator->second : 0.0);
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
        const double daily_pnl = ledger_.daily_pnl();

        if (daily_pnl <= config_.daily_loss_limit)
        {
            reject_reason = std::format("일일 손실 한도 초과 (현재 {:.0f}원 / 한도 {:.0f}원)",
                                        daily_pnl, config_.daily_loss_limit);
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
                                        : ledger_.strategy_index_of(signal.strategy_id),
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

// ─── 교체 진입 ──────────────────────────────────────────────────────────────
//  슬롯이 꽉 찼을 때 "먼저 온 순서"가 하루 종일 자리를 지키는 것을 막는다.
//  여기서는 positions·priority·displace를 겹치지 않고 하나씩 잡는다. check()는 positions_mutex_ 안에서
//  EntryPriority의 priority_mutex_·displace_mutex_(둘 다 잎)를 잡는다(한도 거부 문구, 3c-1 쿨다운·슬롯 예약).
//  (헤더의 중첩 금지 규약 유지 — 각 구간에서 필요한 값만 복사해 나온다).
bool OrderGate::slots_full() const
{
    if (config_.max_concurrent_positions <= 0)
    {
        return false;
    }

    return ledger_.open_slot_count() >= static_cast<size_t>(config_.max_concurrent_positions);
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

    const double equity = ledger_.equity();

    if (config_.max_gross_exposure_percent <= 0.0 || equity <= 0.0)
    {
        return false;
    }

    double gross = 0.0;
    {
        const PositionLedger::Reader ledger = ledger_.read();

        for (const auto& entry : ledger.positions())
        {
            if (entry.second <= 0)
            {
                continue;
            }

            auto average_price_iterator = ledger.average_prices().find(entry.first);
            gross += entry.second * (average_price_iterator != ledger.average_prices().end() ? average_price_iterator->second : 0.0);
        }

        for (const auto& entry : ledger.reserved())
        {
            if (entry.second <= 0)
            {
                continue;
            }

            auto reserved_price_iterator = ledger.reserved_price().find(entry.first);
            gross += entry.second * (reserved_price_iterator != ledger.reserved_price().end() ? reserved_price_iterator->second : 0.0);
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
        entry_priority_.note_decline(new_symbol, why);
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
        const auto open = ledger_.open_slot_count();
        const auto capture  = static_cast<size_t>(config_.max_concurrent_positions);

        if (open > capture)
        {
            return decline(std::format("보유 {} > 한도 {} — 하나 비워도 자리가 안 나 교체 보류", open, capture));
        }
    }

    // (1) 신규 종목의 점수. 점수를 모르면 교체 근거가 없다.
    //  표는 불변 스냅샷이라 포인터만 들고 락 밖에서 읽는다 — 맵을 통째로 복사해 들고 나오던 비용이 없다.
    const std::shared_ptr<const PriorityTable> table = entry_priority_.snapshot();

    if (!table || !EntryPriority::z_of(*table, new_symbol, plan.new_z))
    {
        return decline("신규 종목 점수 없음");
    }

    // (2) 당일 교체 횟수·슬롯 예약 상태. 이미 비워 둔 슬롯이 있으면 또 비우지 않는다.
    const auto  now = Clock::now();
    std::string why = entry_priority_.refusal(new_symbol, now, config_.displace_max_per_day, ledger_.symbols());

    if (!why.empty())
    {
        return decline(why);
    }

    // (3) 보유분 중 최약체. 점수를 아는 종목만 대상 — 스캔 유니버스 밖 보유분(청산 관리,
    //     전일 물린 물량)은 이 판정의 모집단이 아니다. 점수가 없는 것과 낮은 것은 다르다.
    PosKey best_key;
    double worst_z = 0.0;
    {
        const PositionLedger::Reader ledger = ledger_.read();

        for (const auto& entry : ledger.positions())
        {
            if (entry.second <= 0)
            {
                continue;
            }

            const PosKey& key = entry.first;

            if (key.symbol == new_symbol || ledger.slot_exempt().contains(key.symbol)) // 바스켓 소유 종목은 교체 후보가 아니다 [why D-109]
            {
                continue;
            }

            double cand_z = 0.0;

            if (EntryPriority::z_of(*table, key.symbol, cand_z))
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
            auto reserved_found = ledger.reserved().find(key);
            const int sell_pending = (reserved_found != ledger.reserved().end() && reserved_found->second < 0) ? -reserved_found->second : 0;

            if (sell_pending >= entry.second)
            {
                continue;
            }

            // 이전 세션이 낸 미체결 매도는 reserved_에 없다 — 프로세스 메모리라 재기동으로
            //  사라졌다. 그 수량은 잔고 시드(ord_psbl_qty)를 거쳐 sellable_에만 남으므로
            //  여기서도 같이 본다. 안 보면 팔 수 없는 종목을 매번 최약체로 골라 교체가 헛돈다
            //  (09-09: 000215 4회, 001120 1회. 그동안 진짜 팔 수 있는 하위 종목은 그대로 있었다).
            //  상한 계산은 clamp_buy_quantity의 SELL 분기와 같은 규칙을 쓴다.
            int cand_cap = entry.second;
            auto sellable_iterator = ledger.sellable().find(key);

            if (sellable_iterator != ledger.sellable().end() && sellable_iterator->second < cand_cap)
            {
                cand_cap = sellable_iterator->second;
            }

            if (cand_cap - sell_pending <= 0)
            {
                continue;
            }

            // 방금 산 종목은 빼지 않는다. 사자마자 파는 왕복은 비용만 남는다.
            auto opened_iterator = ledger.opened_at().find(key);

            if (opened_iterator != ledger.opened_at().end() && config_.displace_min_hold_sec > 0 &&
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
        const PositionLedger::Reader ledger = ledger_.read();

        plan.account = ledger.keys().account_of(best_key);
        plan.ticker  = ledger.keys().ticker_of(best_key).string();
        plan.symbol  = best_key.symbol;
        auto position_iterator = ledger.positions().find(best_key);
        auto reserved_found  = ledger.reserved().find(best_key);
        const int sell_pending = (reserved_found != ledger.reserved().end() && reserved_found->second < 0) ? -reserved_found->second : 0;
        int capture = (position_iterator != ledger.positions().end() ? position_iterator->second : 0);
        auto sellable_iterator = ledger.sellable().find(best_key);

        if (sellable_iterator != ledger.sellable().end() && sellable_iterator->second < capture)
        {
            capture = sellable_iterator->second;
        }

        plan.quantity = capture - sell_pending;
        auto average_price_iterator = ledger.average_prices().find(best_key);
        plan.average_price = (average_price_iterator != ledger.average_prices().end()) ? average_price_iterator->second : 0.0;
    }

    if (plan.quantity <= 0)
    {
        return decline("최약체 " + plan.ticker + " 매도 가능 수량 0");
    }

    (void)account; // 계좌는 피교체 종목 쪽에서 복원한다(신호 계좌와 다를 수 있음)
    plan.victim_z = worst_z;
    plan.reason = std::format("교체 진입 — {}(z={:.2f})가 {}(z={:.2f})보다 {:.2f}σ 높아 슬롯을 넘긴다",
                              ledger_.symbols().name(new_symbol).view(), plan.new_z, plan.ticker, plan.victim_z,
                              plan.new_z - plan.victim_z);
    plan.ok = true;
    entry_priority_.clear_decline(new_symbol);
    return plan;
}

void OrderGate::note_displacement(const DisplacePlan& plan, symbol::SymbolId beneficiary)
{
    if (!plan.ok)
    {
        return;
    }

    entry_priority_.note_displacement(plan.symbol, beneficiary, Clock::now(), config_.displace_cooldown_sec,
                                      config_.displace_slot_hold_sec, ledger_.symbols().capacity());
}

// ─── 일별 리셋 (장 시작 시) ─────────────────────────────────────────────────
void OrderGate::reset_daily()
{
    ledger_.set_daily_pnl(0.0); // 저널에도 DAILY_PNL 0 — 리셋 전 체결이 리플레이로 되살아나지 않게

    {
        std::lock_guard<std::mutex> lock(rate_mutex_);
        order_times_min_.clear();
        order_times_sec_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(deduplicate_mutex_);
        last_signal_.clear();
    }

    entry_priority_.reset_daily();

    ledger_.expire_reservations(); // 미체결 선점 일일 만료 — 사유는 PositionLedger::expire_reservations

    // average_prices_ / positions_ 는 영속 원장 — 장 시작에 초기화하지 않는다
}

// ─── 진입 우선순위 표 ─────────────────────────────────────────────────────────
void OrderGate::set_entry_priority(const std::vector<PriorityEntry>& entries, int total)
{
    entry_priority_.set(entries, total, ledger_.symbols().capacity());
}

OrderGate::EntrySnapshot OrderGate::entry_snapshot(const std::string& account, const std::string& ticker) const
{
    EntrySnapshot snapshot;
    const PositionLedger::Reader ledger = ledger_.read();

    const auto key = ledger.keys().lookup(account, ticker);
    auto position_iterator = ledger.positions().find(key);
    snapshot.position = (position_iterator != ledger.positions().end()) ? position_iterator->second : 0;

    auto reserved_it = ledger.reserved().find(key);
    snapshot.reserved = (reserved_it != ledger.reserved().end()) ? reserved_it->second : 0;

    if (config_.max_concurrent_positions > 0)
    {
        // 바스켓 슬리브 소유 종목은 자리를 안 먹는다 — check()·slots_full()·plan_displacement()와 같은 규칙이다.
        //  여기만 빼먹고 세던 때는, 교체를 켠 날 바스켓을 든 계좌에서 "자리 꽉 참"이 잘못 나고
        //  plan_displacement가 자리 남음을 따로 안 보기 때문에 멀쩡한 최약체를 팔아 이미 있던 자리를 만들었다.
        //  [why D-109]
        size_t open = 0;

        for (const auto& entry : ledger.positions())
        {
            if (entry.second > 0 && !ledger.slot_exempt().contains(entry.first.symbol))
            {
                ++open;
            }
        }

        for (const auto& entry : ledger.reserved())
        {
            if (entry.second > 0 && !ledger.slot_exempt().contains(entry.first.symbol))
            {
                auto iterator = ledger.positions().find(entry.first);

                if (iterator == ledger.positions().end() || iterator->second <= 0)
                {
                    ++open;
                }
            }
        }

        snapshot.slots_full = open >= static_cast<size_t>(config_.max_concurrent_positions);
    }

    return snapshot;
}

// ─── 장부 사본 발행 (D-114 단계 2.5) ─────────────────────────────────────────
//  전역값 중 게이트가 든 것(국면 플래그·한도)은 여기서 채우고, 종목별 값과 원장에서 셈하는 전역값(열린 슬롯·
//  여력)은 PositionLedger::publish가 채운다. 채우는 순서와 잠금 구간은 publish 안에서 그대로다 — 이 함수는
//  발행 잠금을 쥔 뒤, 원장 잠금을 잡기 전에 불린다.
void OrderGate::publish_ledger(ipc::LedgerSnapshot& snapshot) const
{
    const auto fill_globals = [this](ipc::LedgerGlobals& globals)
    {
        globals.entry_scale              = entry_scale_.load();
        globals.max_notional_per_ticker  = config_.max_notional_per_ticker;
        globals.displace_unscored_z      = config_.displace_unscored_z;
        globals.max_concurrent_positions = config_.max_concurrent_positions;
        globals.displace_enabled         = config_.displace_enabled ? 1 : 0;
        // 세 원천을 OR한 결과만 싣는다. 어느 원천이 켰는지는 주문 쪽 일이다 — 한 원천의 자동 해제가
        //  다른 원천을 지우면 안 되므로 원천 자체는 가르지 않는다. [why D-091]
        globals.entry_halted             = (entry_halt_.load() || manual_buy_halt_.load() || strategy_down_halt_.load()) ? 1 : 0;
        globals.manual_sell_halted       = manual_sell_halt_.load() ? 1 : 0;
    };

    ledger_.publish(snapshot, fill_globals, config_.max_concurrent_positions, config_.max_gross_exposure_percent);
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

size_t OrderGate::SignalKeyHash::operator()(const SignalKey& key) const noexcept
{
    uint64_t mixed = (static_cast<uint64_t>(key.account) << 32) | key.symbol;
    mixed =
        (mixed ^ (static_cast<uint64_t>(key.strategy) << 8) ^ static_cast<uint64_t>(key.side)) * 0x9e3779b97f4a7c15ull;
    mixed ^= static_cast<uint64_t>(key.price) * 0xbf58476d1ce4e5b9ull;
    return static_cast<size_t>(mixed ^ (mixed >> 31));
}
