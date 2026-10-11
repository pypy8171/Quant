// 주문 게이트 — 판정(evaluate)과 매수 수량 클램프, 장부 사본 발행. 교체 진입은 OrderGateDisplace.cpp, 거부 문장·리셋·스위치는 OrderGateState.cpp(D-113).
#include "risk/OrderGate.h"

#include "ipc/LedgerSnapshot.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

using Clock = std::chrono::steady_clock;

namespace
{

// 장중 잔여시간 비율 — 09:00에 1.0, 15:00 이후 0.0. 점수 우선순위 바를 오후로 갈수록 낮춘다.
//  빈 슬롯의 가치는 마감이 다가올수록 떨어진다. 14:30에 상위 종목을 기다리는 건 현금을
//  들고 하루를 끝내는 것과 같아서, 그때는 바를 없애고 아무나 받는 게 맞다.
constexpr int kSessionBarEndMin = 15 * 60; // 15:00 KST — 이후로는 바 없음. 개장은 kst::kKrMarketOpenMinute
// 장 시작 동시호가 매수를 받는 창(분) — 정규장 시작 전 08:50–09:00
constexpr int kOpeningAuctionWindowMin = 10;

// 금액을 주문 수량으로 옮긴다. 몫이 int 범위를 넘으면 캐스팅 결과가 정해져 있지 않아
//  x86 에서 INT_MIN 이 나오고, 호출한 쪽 끝의 `quantity > 0 ? quantity : 0` 이 그것을 0 으로
//  바꾼다 — 한도에 걸리지도 않은 주문이 조용히 수량 0 이 되는 것이다. 그래서 잘라서 받는다.
//  현금이 조 단위인 구성(부하시험)에서 싼 종목을 만나면 바로 넘는다. 몫은 0 쪽으로 자른다(살 수 있는 주 수).
int quantity_from_notional(money::Won notional, money::Won price)
{
    if (price <= 0)
    {
        return 0;
    }

    const money::Won quantity = notional / price;

    if (quantity >= std::numeric_limits<int>::max())
    {
        return std::numeric_limits<int>::max();
    }

    if (quantity <= std::numeric_limits<int>::lowest())
    {
        return std::numeric_limits<int>::lowest();
    }

    return static_cast<int>(quantity);
}

// 명목 평가가(원) — 지정가는 price, 시장가(0)는 reference_price(직전 현재가). 둘 다 없으면 0이고, 그때는
//  명목을 못 재므로 수량 한도만 건다. 판정(check)과 클램프가 같은 값을 쓰게 한 곳에 둔다.
money::Won evaluation_price_of(const OrderSignal& signal)
{
    return money::round_won(signal.price > 0.0 ? signal.price : signal.reference_price);
}

// 총노출 상한(원) — 자본 × 비율을 내린다. "금액 > 실수 한도"와 "금액 > 내린 한도"는 같은 판정이다(Money.h floor_won).
money::Won gross_exposure_ceiling(double percent, money::Won equity)
{
    return money::floor_won(percent * static_cast<double>(equity));
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

    if (now_min <= kst::kKrMarketOpenMinute)
    {
        return 1.0;
    }

    if (now_min >= kSessionBarEndMin)
    {
        return 0.0;
    }

    return static_cast<double>(kSessionBarEndMin - now_min) /
           static_cast<double>(kSessionBarEndMin - kst::kKrMarketOpenMinute);
}
} // namespace

// 총노출(§3d) 분자 — 보유는 잔고 대조가 넣은 현재가로, 현재가가 아직 없는 종목(대조 사이에 새로 산 것)은
//  평단으로 잰다. 미체결 매수 선점은 선점가로 더하고 매도 선점은 노출을 줄이는 쪽이라 보수적으로 뺀다.
//  분모 equity가 시가 총평가금이라 분자도 시가로 맞춘다 — 원가로 재면 오른 보유분이 한도에서 빠진다.
money::Won OrderGate::gross_exposure(const PositionLedger::Reader& ledger)
{
    money::Won gross = 0;

    for (const auto& [key, row] : ledger.rows())
    {
        if (row.position > 0)
        {
            const money::Won price = (row.mark_price != 0) ? row.mark_price : row.average_price;
            gross += static_cast<money::Won>(row.position) * price;
        }

        if (row.reserved.buy > 0)
        {
            gross += static_cast<money::Won>(row.reserved.buy) * row.reserved_price;
        }
    }

    return gross;
}

// ─── 주문 검증 ──────────────────────────────────────────────────────────────
// 주의: check()는 항목별 뮤텍스를 독립 스코프로 잡아 호출 단위가 원자적이지 않다.
// 현재 호출자는 단일 order_thread(Engine::order_thread_fn → OrderRouter::submit)뿐이라
// check()+on_intent (on_accept는 테스트·도구용)이 직렬 실행돼 검사~사용 사이 경합(TOCTOU, Time-Of-Check-To-Time-Of-Use)이
// 없다. 멀티 producer로 확장하려면
// check()+on_intent (on_accept는 테스트·도구용)를 하나의 임계구역으로 묶어 원자적 reserve로 만들어야 한다.
// ─── 한도 클램프 (BUY NEW) ────────────────────────────────────────────────
//  check()가 쓰는 것과 같은 한도식을 "얼마까지 되나"로 뒤집어 푼다. 두 곳의 식이 어긋나면
//  클램프한 수량이 다시 거부되므로, 항목·평가가(evaluation_price)·합산 기준(보유+매수 선점)을
//  check()와 똑같이 맞춘다.
int OrderGate::clamp_buy_quantity(const OrderSignal& signal)
{
    int quantity = signal.quantity;

    if (signal.side == OrderSide::SELL && signal.action == OrderAction::NEW && quantity > 0)
    {
        return clamp_sell_quantity(signal, quantity);
    }

    if (signal.side != OrderSide::BUY || signal.action != OrderAction::NEW || quantity <= 0)
    {
        return quantity;
    }

    const money::Won evaluation_price = evaluation_price_of(signal);

    if (config_.max_quantity_per_order > 0 && quantity > config_.max_quantity_per_order)
    {
        quantity = config_.max_quantity_per_order;
    }

    if (evaluation_price > 0 && config_.max_notional_per_order > 0)
    {
        const int quantity_ceiling = quantity_from_notional(config_.max_notional_per_order, evaluation_price);

        if (quantity_ceiling < quantity)
        {
            quantity = quantity_ceiling;
        }
    }

    quantity = clamp_buy_to_ledger(signal, evaluation_price, quantity);
    return quantity > 0 ? quantity : 0;
}

// ── SELL NEW: 매도가능수량(보유 - 미체결매도) 클램프 ────────────────────
//  자기가 낸 익절 지정가가 자기 청산을 막는다. 그대로 내면 KIS가 40240000
//  (주문가능분 없음)으로 주문을 통째로 거부해 한 주도 못 빠져나온다(09-08 047050:
//  254주·381주 두 번 다 전량 거부, 보유분이 갇혔다). 나갈 수 있는 만큼이라도
//  내보내는 편이 낫다. 이미 FORCE_LIQ와 디스플레이스먼트는 같은 식으로 깎고 있고,
//  전략 청산 신호만 이 경로를 안 거치고 있었다.
//  장부가 그 종목을 모를 때(보유 없음 또는 0)는 손대지 않는다 - 과소 인식으로
//  정당한 청산을 0주로 깎는 쪽이 거부당하는 것보다 위험하다.
int OrderGate::clamp_sell_quantity(const OrderSignal& signal, int quantity) const
{
    const PositionLedger::Reader ledger = ledger_.read();
    const PositionRow& row = ledger.row(ledger.keys().lookup(signal));

    if (row.position <= 0)
    {
        return quantity;
    }

    // 상한은 보유수량이 아니라 매도가능수량이다. 기동 전 세션이 남긴 미체결 매도는
    //  선점에 없고(프로세스 메모리라 재기동으로 사라진다) 잔고의 ord_psbl_qty에만 보인다.
    const int holding_ceiling = std::min(row.position, row.sellable_or(row.position));
    const int sellable        = holding_ceiling - row.reserved.sell;

    // 여기서는 로그를 남기지 않는다.
    //  클램프가 실제로 걸리면 호출부 OrderRouter가 "한도 클램프" 한 줄을 남긴다.
    if (sellable <= 0)
    {
        return 0;
    }

    return quantity > sellable ? sellable : quantity;
}

// BUY NEW의 장부 쪽 한도 — 종목당 수량·명목, 총노출, 주문가능현금. 장부 잠금(Reader)을 한 번 쥐고 잰다.
//  돌려준 값이 0 이하일 수 있다 — 0으로 자르는 것은 부른 쪽 몫이다.
int OrderGate::clamp_buy_to_ledger(const OrderSignal& signal, money::Won evaluation_price, int quantity) const
{
    const PositionLedger::Reader ledger = ledger_.read();
    const PositionRow&           row    = ledger.row(ledger.keys().lookup(signal));
    const int current_quantity = row.position + row.reserved.buy;

    if (config_.max_quantity_per_ticker > 0)
    {
        const int room = config_.max_quantity_per_ticker - current_quantity;

        if (room < quantity)
        {
            quantity = room;
        }
    }

    if (evaluation_price > 0 && config_.max_notional_per_ticker > 0)
    {
        const int room = quantity_from_notional(config_.max_notional_per_ticker, evaluation_price) - current_quantity;

        if (room < quantity)
        {
            quantity = room;
        }
    }

    // 총노출(§3d)도 같은 방식으로 남은 여유를 수량으로 환산한다. 재는 법은 check()와 같은 gross_exposure()다.
    const money::Won equity = ledger_.equity();

    if (config_.max_gross_exposure_percent > 0.0 && equity > 0 && evaluation_price > 0)
    {
        const money::Won gross            = gross_exposure(ledger);
        const money::Won exposure_ceiling = gross_exposure_ceiling(config_.max_gross_exposure_percent, equity);
        const int        room             = quantity_from_notional(exposure_ceiling - gross, evaluation_price);

        if (room < quantity)
        {
            quantity = room;
        }
    }

    // 주문가능현금 클램프. 평가금이 아니라 현금이 매수의 진짜 상한이다. 우리가 이미 낸
    //  미체결 매수 명목을 빼는 것은 보수적으로 중복차감이 될 수 있으나(브로커 값이 이미
    //  반영했을 수 있다), 모자라게 사는 쪽이 전량 거부보다 낫다.
    const money::Won cash = ledger_.available_cash();

    if (cash > 0 && evaluation_price > 0)
    {
        money::Won pending_buy = 0;

        for (const auto& [key, ledger_row] : ledger.rows())
        {
            if (ledger_row.reserved.buy > 0)
            {
                pending_buy += static_cast<money::Won>(ledger_row.reserved.buy) * ledger_row.reserved_price;
            }
        }

        const int room = quantity_from_notional(cash - pending_buy, evaluation_price);

        if (room < quantity)
        {
            quantity = room;
        }
    }

    return quantity;
}

bool OrderGate::check(const OrderSignal& signal, std::string& reject_reason)
{
    const GateVerdict verdict = evaluate(signal);

    // 경고는 비동기 로거 큐로 넘긴다 — 게이트 스레드에서 콘솔 I/O를 하지 않는다.
    if (verdict.sell_over_notional)
    {
        LOG_WARN(std::format("[OrderGate] {} SELL 1주문 명목 한도 초과 ({} > {}{} — 청산이라 통과", signal.ticker, verdict.amount,
                             static_cast<long long>(config_.max_notional_per_order),
                             verdict.market_reference ? ", 시장가 참조평가)" : ")"));
    }

    if (verdict.passed())
    {
        return true;
    }

    reject_reason = describe(verdict);
    return false;
}

// 판정 — 아래 단계를 적힌 순서대로 부르고, 처음 거부한 단계에서 멈춘다. 단계 순서가 곧 거부 우선순위다.
//  각 단계는 거부면 verdict.code와 값 칸을 채우고 true를 돌려준다. 문장은 describe()가 만든다.
GateVerdict OrderGate::evaluate(const OrderSignal& signal)
{
    GateVerdict verdict;

    if (reject_by_switches_and_session(signal, verdict) || reject_by_order_size(signal, verdict))
    {
        return verdict;
    }

    // 3. 포지션 수량 한도 (BUY에만 적용) — 실체결 보유 + 미체결 매수 선점 합산. 미체결 매도는
    //    체결될지 모르므로 빼 주지 않는다(clamp_buy_quantity와 같은 셈).
    //    계좌별 파티션 — 한 계좌 한도는 다른 계좌 주문을 막지 않는다.
    // 장부 키를 여기서 한 번 만든다 — 3절(한도)과 5절(중복 신호 키)이 같은 번호를 쓴다. 처음 보는 계좌·종목은
    //  등록한다(모르는 계좌끼리 중복 키가 겹치지 않게).
    const PosKey key = ledger_.register_signal(signal);

    if (signal.side == OrderSide::BUY && reject_by_buy_limits(signal, key, verdict))
    {
        return verdict;
    }

    if (reject_by_loss_guards(signal, verdict))
    {
        return verdict;
    }

    // 5. 중복 신호 제거 — rate 소비 전에 검사해 중복이 rate slot을 소모하지 않게 함.
    //    키에 side 포함: 시장조성은 같은 틱에 동일 strategy+ticker로 BUY(bid)+SELL(ask)를
    //    동시 발주한다. side가 없으면 두 번째(ask)가 중복 오거부된다. BUY/SELL은 다른 의도라
    //    중복이 아니다. (같은 side 반복은 여전히 deduplicate — 기존 전략 동작 불변)
    //    스탬프(last_signal_)는 6절 rate 통과 뒤에 찍는다 — rate로 거부된 신호가 deduplicate 창을
    //    소모하면 창 안의 정당한 재시도까지 "중복"으로 막힌다.
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

    if (reject_by_duplicate(deduplicate_key, verdict) || reject_by_rate(verdict))
    {
        return verdict;
    }

    // 모든 검사를 지난 신호만 deduplicate 창을 연다.
    {
        std::lock_guard<std::mutex> lock(deduplicate_mutex_);
        last_signal_[deduplicate_key] = Clock::now();
    }

    return verdict;
}

// 판정 1 → 2 → 1b → 1c — 스위치·방향·세션 창. 번호는 단계가 생긴 순서라 부르는 순서와 다르다.
bool OrderGate::reject_by_switches_and_session(const OrderSignal& signal, GateVerdict& verdict) const
{
    // 1. Kill switch — 전방향 하드스톱(BUY·SELL 모두). 연결단절/수동 긴급정지용.
    if (kill_switch_.load())
    {
        verdict.code = GateReject::KillSwitch;
        return true;
    }

    // 2. NONE side — 전략이 신호 없음을 나타낼 때 사용; 주문 처리 불가
    if (signal.side == OrderSide::NONE)
    {
        verdict.code = GateReject::SideNone;
        return true;
    }

    // 1b. Entry halt — 신규 진입(BUY NEW)만 차단. SELL 청산·취소(CANCEL/REPLACE)는 통과시켜
    //     지수 급락 시 "신규정지 + 보유분 청산"이 게이트에서 미완료로 남지 않게 한다.
    //     kill_switch_(전방향)와 분리된 국면 리스크 플래그.
    //     국면을 받지 않는 전략(regime_halt_exempt, 스터디 42 SwingTrend)은 통과시킨다. [why D-160]
    if (entry_halt_.load() && signal.side == OrderSide::BUY && signal.action == OrderAction::NEW &&
        !signal.regime_halt_exempt)
    {
        verdict.code = GateReject::EntryHalt;
        return true;
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
        // 장 시작 동시호가 매수는 정규장 시작 전 10분(08:50–09:00)을 더 받는다 — 표시한 신규 매수만. 08:50 앞은
        //  NXT 프리마켓이라 시장가가 거부되고, 매도는 이 창을 쓰지 않는다 [why D-157]
        const bool in_opening_auction = signal.opening_auction && signal.side == OrderSide::BUY &&
                                        now_min >= config_.session_open_min - kOpeningAuctionWindowMin && now_min < config_.session_open_min;

        if (!in_regular && !in_after && !in_opening_auction)
        {
            verdict.code   = GateReject::OutsideSession;
            verdict.amount = now_min;
            return true;
        }
    }

    return false;
}

// 판정 2b — 1주문 fat-finger 백스톱 (C-3). NEW BUY/SELL 공통, 시장가 대량주문 슬리피지 방어.
//  CANCEL/REPLACE는 대상 아님 → action==NEW로 한정해 MM 취소경로에 무영향.
bool OrderGate::reject_by_order_size(const OrderSignal& signal, GateVerdict& verdict) const
{
    if (signal.action != OrderAction::NEW)
    {
        return false;
    }

    if (signal.quantity <= 0)
    {
        verdict.code   = GateReject::BadQuantity;
        verdict.amount = signal.quantity;
        return true;
    }

    if (signal.quantity > config_.max_quantity_per_order)
    {
        verdict.code   = GateReject::OrderQuantityLimit;
        verdict.amount = signal.quantity;
        return true;
    }

    // 명목 평가가: 지정가는 price, 시장가(price=0)는 reference_price(직전 현재가).
    // 시장가가 ref_price도 없으면 명목 백스톱 불가(수량 한도로만 방어).
    const money::Won evaluation_price = evaluation_price_of(signal);
    const money::Won notional         = money::notional(evaluation_price, signal.quantity);

    if (evaluation_price > 0 && notional > config_.max_notional_per_order)
    {
        verdict.amount           = notional;
        verdict.market_reference = !(signal.price > 0.0);

        // SELL은 청산 계열이라 거부하지 않는다 — 정당한 청산을 막는 쪽이 대량 매도보다 위험하다.
        //  (수량 한도는 위에서 이미 걸렸다.) 경고 한 줄은 check()가 남긴다.
        if (signal.side == OrderSide::SELL)
        {
            verdict.sell_over_notional = true;
        }
        else
        {
            verdict.code = GateReject::OrderNotionalLimit;
            return true;
        }
    }

    return false;
}

// 판정 3 → 3b → 3c → 3d — BUY의 장부 쪽 한도. 장부 잠금(Reader)을 한 번 쥔 채 네 단계를 본다.
bool OrderGate::reject_by_buy_limits(const OrderSignal& signal, const PosKey& key, GateVerdict& verdict)
{
    const PositionLedger::Reader ledger = ledger_.read();
    const PositionRow&           row    = ledger.row(key);
    const int current_quantity = row.position + row.reserved.buy;

    if (current_quantity + signal.quantity > config_.max_quantity_per_ticker)
    {
        verdict.code   = GateReject::TickerQuantityLimit;
        verdict.amount = signal.quantity;
        verdict.base   = current_quantity;
        return true;
    }

    // 3b. 종목당 명목 한도 — 자본% 사이징의 상한 백스톱. 지정가는 price, 시장가는 ref_price로
    //     보유·예약 합산 평가(시장가가 백스톱을 우회하지 않도록).
    const money::Won evaluation_price = evaluation_price_of(signal);
    const money::Won ticker_notional  = static_cast<money::Won>(current_quantity + signal.quantity) * evaluation_price;

    if (config_.max_notional_per_ticker > 0 && evaluation_price > 0 && ticker_notional > config_.max_notional_per_ticker)
    {
        verdict.code   = GateReject::TickerNotionalLimit;
        verdict.amount = ticker_notional;
        return true;
    }

    // 3c. 동시 보유 종목 상한 — "새 종목"을 여는 BUY NEW에만 적용(기존 보유·예약 종목은 통과).
    //     총노출 제어: 실보유(보유>0)∪매수 선점 종목 수가 상한이면 신규 진입 차단.
    //     기존 보유·선점이 있는 종목(filled>0 또는 선점 있음)은 새로 여는 게 아니므로 예외.
    //     바스켓 슬리브 소유 종목(slot_exempt_)은 상한을 세지도, 상한에 걸리지도 않는다 [why D-109].
    if (config_.max_concurrent_positions > 0 && signal.action == OrderAction::NEW &&
        row.position == 0 && row.reserved.empty() && !ledger.slot_exempt().contains(key.symbol) &&
        reject_by_concurrent_slots(key, ledger, verdict))
    {
        return true;
    }

    return reject_by_gross_exposure(signal, ledger, evaluation_price, verdict);
}

// 판정 3c → 3c-1 → 3c-2 — 열린 자리 세기, 동시 보유 상한, 교체 쿨다운·슬롯 예약, 점수 우선순위 바.
//  [inv] ledger(positions_mutex_)를 쥔 채 부른다. EntryPriority의 잠금은 잎이라 이 안에서 잡아도 된다.
bool OrderGate::reject_by_concurrent_slots(const PosKey& key, const PositionLedger::Reader& ledger, GateVerdict& verdict)
{
    // 3c-2(아래)가 쓰는 "나보다 랭크가 위인데 이미 차지된 종목 수"를 같은 순회에서 센다 — 표를 도는 대신
    //  장부(보유·선점 ≤ 슬롯 수)를 돈다. 표는 불변 스냅샷이라 락 밖 포인터로 읽는다.
    const std::shared_ptr<const PriorityTable> table =
        config_.entry_priority_enabled ? entry_priority_.snapshot() : nullptr;
    const int rank = table ? EntryPriority::rank_of(*table, key.symbol) : 0;

    const SlotOccupancy occupancy = count_open_slots(key, ledger, table.get(), rank);

    if (occupancy.open >= static_cast<size_t>(config_.max_concurrent_positions))
    {
        // 교체가 떨어진 사유는 describe()가 장부 잠금 밖에서 붙인다.
        verdict.code   = GateReject::ConcurrentLimit;
        verdict.amount = static_cast<int64_t>(occupancy.open);
        verdict.base   = static_cast<int64_t>(occupancy.held);
        verdict.symbol = key.symbol;
        return true;
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
            verdict.code = GateReject::DisplaceCooling;
            return true;
        }

        if (admission == EntryPriority::Admission::SlotReserved)
        {
            verdict.code   = GateReject::SlotReserved;
            verdict.symbol = reserved_for;
            return true;
        }
    }

    return table && reject_by_priority_bar(key, *table, rank, occupancy, verdict);
}

// 3c의 자리 세기 — 실보유 종목과 선점만 있는 종목(중복 제외)을 세고, 그중 나보다 랭크가 위인 것을 따로 센다.
//  table이 비었거나 rank가 0이면 taken_ahead는 0으로 남는다. [inv] ledger를 쥔 채 부른다.
OrderGate::SlotOccupancy OrderGate::count_open_slots(const PosKey& key, const PositionLedger::Reader& ledger,
                                                     const PriorityTable* table, int rank) const
{
    SlotOccupancy occupancy;

    auto counts_ahead = [&](const PosKey& taken_key)
    {
        if (rank <= 0 || taken_key.account != key.account || taken_key.symbol == key.symbol)
        {
            return false;
        }

        const int taken_rank = EntryPriority::rank_of(*table, taken_key.symbol);
        return taken_rank > 0 && taken_rank < rank;
    };

    for (const auto& [taken_key, row] : ledger.rows())
    {
        if (!row.takes_slot() || ledger.slot_exempt().contains(taken_key.symbol))
        {
            continue;
        }

        ++occupancy.open; // 실보유 또는 선점만 있는 종목 — 한 줄이 한 종목이라 겹쳐 세지 않는다
        occupancy.held        += (row.position > 0) ? 1 : 0;
        occupancy.taken_ahead += counts_ahead(taken_key) ? 1 : 0;
    }

    return occupancy;
}

// 3c-2. 점수 우선순위 바 — 남은 슬롯이 적을수록 더 높은 점수를 요구한다.
//   eff_rank/pool ≤ 1 − (open/max_concurrent_positions) × session_remaining_ratio()
//   eff_rank = below_by_symbol − taken_ahead + 1, pool = max(total, max_concurrent_positions)
//  슬롯이 비어 있으면 아무나 통과하고, 마지막 칸에 가까울수록 상위만 남는다.
//  랭크를 모르는 종목(스캔 유니버스 밖 보유분 청산 관리 등)은 바를 적용하지 않는다.
bool OrderGate::reject_by_priority_bar(const PosKey& key, const PriorityTable& table, int rank,
                                       const SlotOccupancy& occupancy, GateVerdict& verdict) const
{
    const int total    = table.total;
    int       eff_rank = 0;

    // 유효 랭크 = 나보다 점수가 높으면서 "아직 슬롯을 안 차지한" 종목 수 + 1.
    //  전체 랭크를 그대로 쓰면 상위 랭크를 이미 보유한 순간 남은 후보는
    //  구조적으로 기준선을 넘을 수 없다(보유 23/25에서 최상위 후보의 랭크가
    //  24위라 quant=0.93 > 기준 0.54). 그러면 슬롯이 25에 닿지 못하고,
    //  교체 진입은 capacity_full()에서만 열리므로 둘 다 영원히 막힌다.
    //  살 수 있는 종목들 사이의 순위로 재면 최상위 후보는 항상 1위가 된다.
    //  "나보다 위" 전체 수는 표가 미리 세 두었고(below_by_symbol), 그중 차지된 수는 위 장부 순회가 셌다.
    if (rank > 0)
    {
        eff_rank = table.below_by_symbol[key.symbol] - occupancy.taken_ahead + 1;
    }

    if (rank <= 0 || total <= 0)
    {
        return false;
    }

    const double ratio_open = static_cast<double>(occupancy.open) /
                              static_cast<double>(config_.max_concurrent_positions);
    const double bar   = 1.0 - ratio_open * session_remaining_ratio();
    // 분모는 슬롯이 경합하는 모집단이다. 등록 종목이 슬롯보다 적으면 경합 자체가
    //  없는데도 rank/total 이 1.0에 붙어 하위 랭크가 영구 차단된다(등록 12 vs 슬롯 25
    //  이면 허용 랭크가 8에서 멈춰 슬롯의 1/3만 채운 채 하루가 끝난다). 모집단을
    //  최소 슬롯 수로 받쳐, 풀이 슬롯보다 클 때의 동작은 그대로 두고 작을 때만 푼다.
    const int    pool  = total > config_.max_concurrent_positions
                             ? total : config_.max_concurrent_positions;
    const double quant = static_cast<double>(eff_rank) / static_cast<double>(pool);

    if (quant > bar)
    {
        verdict.code    = GateReject::PriorityBar;
        verdict.amount  = static_cast<int64_t>(occupancy.open);
        verdict.base    = eff_rank;
        verdict.ceiling = pool;
        verdict.rank    = rank;
        verdict.ratio   = quant;
        verdict.bar     = bar;
        return true;
    }

    return false;
}

// 3d. 포트폴리오 총노출 상한 — 모든 종목 보유(수량×시가)+미체결 선점(수량×선점가) 합이
//     자본의 max_gross_exposure_percent를 넘게 만드는 BUY를 차단(신규·물타기 공통). 청산(SELL)은 위에서 제외.
//     종목당 명목(15%)×동시보유(10)=150% 같은 과노출을 총합 단에서 막는다. equity 미주입(0)이면 비활성.
//     재는 법은 gross_exposure() — 보유는 현재가(없으면 평단), 선점은 선점가. [inv] ledger를 쥔 채 부른다.
bool OrderGate::reject_by_gross_exposure(const OrderSignal& signal, const PositionLedger::Reader& ledger,
                                         money::Won evaluation_price, GateVerdict& verdict) const
{
    const money::Won equity = ledger_.equity();

    if (config_.max_gross_exposure_percent > 0.0 && equity > 0 && evaluation_price > 0)
    {
        const money::Won exposure_ceiling = gross_exposure_ceiling(config_.max_gross_exposure_percent, equity);
        const money::Won next_gross       = gross_exposure(ledger) + money::notional(evaluation_price, signal.quantity);

        if (next_gross > exposure_ceiling)
        {
            verdict.code    = GateReject::GrossExposure;
            verdict.amount  = next_gross;
            verdict.ceiling = exposure_ceiling;
            verdict.money   = equity;
            return true;
        }
    }

    return false;
}

// 판정 4 → 4b — 손실 쪽 진입 정지.
bool OrderGate::reject_by_loss_guards(const OrderSignal& signal, GateVerdict& verdict) const
{
    // 4. 일일 손실 한도 (BUY에만 적용 — 신규 진입 차단이 설계 의도)
    //    (C10) 보유분 추가 하락은 막지 않는다. 강제 청산이 필요하면 별도 청산 로직 도입.
    if (signal.side == OrderSide::BUY)
    {
        const money::Won daily_pnl = ledger_.daily_pnl();

        if (daily_pnl <= config_.daily_loss_limit)
        {
            verdict.code  = GateReject::DailyLoss;
            verdict.money = daily_pnl;
            return true;
        }
    }

    // 4b. PnL stale guard (B2) — daily_pnl_이 낡으면(잔고 대조 연속 정체) §4 손실컷을
    //     신뢰할 수 없어 BUY NEW만 보수적으로 정지. SELL 청산·BUY 취소/정정은 통과시켜
    //     "신규 위험만 억제, 탈출은 허용"(entry_halt와 동일 의미론). Engine이 잔고조회 복구 시 해제.
    if (pnl_stale_.load() && signal.side == OrderSide::BUY && signal.action == OrderAction::NEW)
    {
        verdict.code = GateReject::PnlStale;
        return true;
    }

    return false;
}

// 판정 5 — 같은 키의 신호가 창(deduplicate_window_sec) 안에 이미 지나갔는가. 스탬프는 evaluate 끝에서 찍는다.
bool OrderGate::reject_by_duplicate(const SignalKey& deduplicate_key, GateVerdict& verdict) const
{
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(deduplicate_mutex_);
    auto iterator = last_signal_.find(deduplicate_key);

    if (iterator != last_signal_.end())
    {
        double elapsed = std::chrono::duration<double>(now - iterator->second).count();

        if (elapsed < config_.deduplicate_window_sec)
        {
            verdict.code = GateReject::Duplicate;
            return true;
        }
    }

    return false;
}

// 6. Rate limit — 초당 / 분당 두 단계 검사 (deduplicate 통과 후에만 카운터 소모). 통과하면 주문 시각을 남긴다.
bool OrderGate::reject_by_rate(GateVerdict& verdict)
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
        verdict.code = GateReject::RatePerSecond;
        return true;
    }

    // 분당 제한
    auto cutoff_min = now - std::chrono::minutes(1);

    while (!order_times_min_.empty() && order_times_min_.front() < cutoff_min)
    {
        order_times_min_.pop_front();
    }

    if (static_cast<int>(order_times_min_.size()) >= config_.max_orders_per_min)
    {
        verdict.code = GateReject::RatePerMinute;
        return true;
    }

    order_times_sec_.push_back(now);
    order_times_min_.push_back(now);
    return false;
}

// ─── 장부 사본 발행 (D-114 단계 2.5) ─────────────────────────────────────────
//  전역값 중 게이트가 든 것(국면 플래그·한도)은 여기서 채우고, 종목별 값과 장부에서 셈하는 전역값(열린 슬롯·
//  여력)은 PositionLedger::publish가 채운다. 채우는 순서와 잠금 구간은 publish 안에서 그대로다 — 이 함수는
//  발행 잠금을 쥔 뒤, 장부 잠금을 잡기 전에 불린다.
void OrderGate::publish_ledger(ipc::LedgerSnapshot& snapshot) const
{
    const auto fill_globals = [this](ipc::LedgerGlobals& globals)
    {
        globals.entry_scale              = entry_scale_.load();
        globals.max_notional_per_ticker  = static_cast<double>(config_.max_notional_per_ticker); // 원 단위 정수 값
        globals.displace_unscored_z      = config_.displace_unscored_z;
        globals.max_concurrent_positions = config_.max_concurrent_positions;
        globals.displace_enabled         = config_.displace_enabled ? 1 : 0;
        // 네 원천을 OR한 결과만 싣는다. 어느 원천이 켰는지는 주문 쪽 일이다 — 한 원천의 자동 해제가
        //  다른 원천을 지우면 안 되므로 원천 자체는 가르지 않는다. [why D-091][why D-137]
        globals.entry_halted             = (entry_halt_.load() || manual_buy_halt_.load() ||
                                            strategy_down_halt_.load() || feed_down_halt_.load()) ? 1 : 0;
        globals.entry_halted_except_regime = (manual_buy_halt_.load() || strategy_down_halt_.load() ||
                                              feed_down_halt_.load()) ? 1 : 0;
        globals.manual_sell_halted       = manual_sell_halt_.load() ? 1 : 0;
    };

    ledger_.publish(snapshot, fill_globals, config_.max_concurrent_positions, config_.max_gross_exposure_percent);
}
