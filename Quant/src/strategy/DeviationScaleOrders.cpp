// strategy/DeviationScaleOrders.cpp — 이격 분할 전략의 분할 주문 계획·재구성 판정·지정가 발주와 취소.
//  관련 결정: D-006(교차 가드)·D-033(시그니처의 진입 축)·D-036(원 사이징)·D-081(베이스 잔량·먼지)·D-083(국면 매수비율)·D-156.
#include "strategy/DeviationScaleStrategy.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

DeviationScaleStrategy::SplitPlan DeviationScaleStrategy::plan_split_steps(int position, double current_price,
                                                                           double simple_moving_average, bool warming,
                                                                           std::chrono::steady_clock::time_point now)
{
    // ── 목표 분할 매수 산출(발주 전) ─────────────────────────────────────────
    //  계단 가격·수량은 simple_moving_average·pos의 순수 함수. 먼저 계획을 만들고 직전 분할 매수와
    //  시그니처를 비교해 "동일하면 재발주 스킵". 분봉 정지(HTTP 500 폴백)로
    //  simple_moving_average=price가 고정될 때 동일 분할 매수를 취소·재발주하던 처닝을 차단.
    SplitPlan split_plan;
    std::vector<SplitStep>& plan = split_plan.steps;
    const SplitSizing sizing = size_split_budget();

    // 분할 매수 기준점. 교차 가드가 켜져 있으면 각 방향 층이 현재가를 넘지 않도록 기준선을
    //  현재가 쪽으로 당긴다. 이격이 벌어진 상태에서도 분할 매수 간격은 그대로 유지된다.
    const bool guard_on = parameters_.cross_guard && current_price > 0.0;
    const double sell_base_line = guard_on ? (std::max)(simple_moving_average, current_price) : simple_moving_average;
    const double buy_base_line = guard_on ? (std::min)(simple_moving_average, current_price) : simple_moving_average;

    update_hold_span(position, now);
    plan_base_step(position, current_price, simple_moving_average, sizing.base_notional, plan);
    plan_sell_steps(position, current_price, sell_base_line, plan);
    plan_buy_steps(current_price, simple_moving_average, warming, buy_base_line, sizing.split_step_notional, plan);

    split_plan.base_notional = sizing.base_notional;
    split_plan.split_step_budget = sizing.split_step_budget;
    split_plan.entry_scale_ratio = sizing.entry_scale_ratio;
    return split_plan;
}

DeviationScaleStrategy::SplitSizing DeviationScaleStrategy::size_split_budget() const
{
    // ── 명목 사이징: 자본%를 가격으로 나눠 수량 산출(자본 미상이면 주수 폴백) ──
    //  베이스=자본×base_percent(5%), 물타기 총예산=자본×(max_percent−base_percent)(5%)를 split_step_count로 분할.
    //  베이스+물타기 합 ≈ 자본×max_percent(10%) → OrderGate 명목캡과 정합(캡은 백스톱).
    const double equity = equity_ > 0.0 ? equity_ : parameters_.fallback_equity;
    // 국면 매수비율(OrderGate::entry_scale, 0~1)을 명목에 곱한다. 스위치(halt)가 아니라 비율이라
    //  코스피 −1%면 70%, −2%면 40%처럼 줄어들고 반등하면 돌아온다. 0.1 단위로 끊어 3분마다
    //  분할 매수가 재구성되는 일을 막고, 시그니처에 붙여 바뀐 회차에만 다시 깐다. [why D-083]
    const double rounded_scale = std::round(std::clamp(entry_scale(), 0.0, 1.0) * 10.0) / 10.0;
    const double multiplier = (parameters_.size_multiplier > 0.0 ? parameters_.size_multiplier : 1.0) * rounded_scale;
    // 원 단위 총액이 주어지면 그 금액을 base_percent:max_percent 비율로 베이스·물타기에 나눈다.
    const double base_share =
        parameters_.max_percent > parameters_.base_percent ? parameters_.base_percent / parameters_.max_percent : 1.0;
    const double base_notional = parameters_.notional_krw > 0.0 ? parameters_.notional_krw * rounded_scale * base_share
                                                                : equity * parameters_.base_percent * multiplier;
    const double split_step_budget = parameters_.notional_krw > 0.0
                                         ? parameters_.notional_krw * rounded_scale - base_notional
                                         : equity *
                                               (parameters_.max_percent > parameters_.base_percent
                                                    ? parameters_.max_percent - parameters_.base_percent
                                                    : 0.0) *
                                               multiplier;
    SplitSizing sizing;
    sizing.base_notional = base_notional;
    sizing.split_step_budget = split_step_budget;
    sizing.split_step_notional =
        parameters_.buy_split_steps > 0 ? split_step_budget / parameters_.buy_split_steps : 0.0;
    sizing.entry_scale_ratio = rounded_scale;
    return sizing;
}

void DeviationScaleStrategy::update_hold_span(int position, std::chrono::steady_clock::time_point now)
{
    if (position <= 0)
    {
        // 보유가 있다가 0이 된 순간만 잡는다 — 처음부터 0인 하트비트마다 대기를 늘리면 진입이 영영 안 된다.
        if (trading_.peak_position > 0 && parameters_.reentry_cooldown_sec > 0)
        {
            liquidation_.reentry_cooldown_until = now + std::chrono::seconds(parameters_.reentry_cooldown_sec);
        }

        trading_.base_target_quantity = 0;
        trading_.peak_position = 0;
    }
    else
    {
        trading_.peak_position = (std::max)(trading_.peak_position, position);
    }
}

void DeviationScaleStrategy::plan_base_step(int position, double current_price, double simple_moving_average,
                                            double base_notional, std::vector<SplitStep>& plan)
{
    // 베이스: 무포지션이면 기준선 근처 지정가 매수(자본의 base_percent). 부분체결로 보유가 목표에 못 미치면
    //  잔량을 같은 자리에 다시 깐다 — 예전엔 1주만 체결돼도 position>0이라 베이스 분할 단계가 빠졌고, 재구성이 잔량
    //  주문을 취소해 익절 매도 1주만 남았다(09-14 375500 09:00:00 1/16 체결 → 09:00:10 잔량 취소, 096770
    //  13:10 같은 경로). 목표 수량은 처음 깔 때 값을 기억한다 — 값이 움직여 base_target_quantity가 ±1 흔들리면 1주 매수가
    //  반복된다. 익절로 줄어든 보유는 채우지 않는다(peak_position이 목표에 닿았으면 베이스는 끝난 것). [why D-081]
    //  보유 구간 갱신(update_hold_span)이 먼저 돌아 trading_.peak_position은 이번 보유를 이미 담고 있다.
    const bool base_short = position > 0 && trading_.base_target_quantity > 0 && trading_.peak_position < trading_.base_target_quantity;

    if (position > 0 && !base_short)
    {
        return;
    }

    double buy_price = round_to_tick(simple_moving_average, OrderSide::BUY);

    // 교차 가드: 기준선이 현재가 이상이면 이 지정가는 즉시 시장가로 체결된다.
    //  베이스를 건너뛰면 add_below_sma_only가 노리는 눌림 진입에서 가장 큰 레그가
    //  빠지므로, 억제 대신 현재가 한 틱 아래로 옮겨 지정가로 남긴다.
    if (parameters_.cross_guard && current_price > 0.0 && buy_price >= current_price)
    {
        buy_price = round_to_tick(current_price - krx::tick_size(current_price), OrderSide::BUY);
    }

    int base_quantity = base_short ? trading_.base_target_quantity : quantity_for(base_notional, buy_price);

    if (base_quantity <= 0)
    {
        base_quantity = parameters_.base_quantity; // 자본 미상 폴백
    }

    const int need = base_short ? base_quantity - position : base_quantity;

    if (buy_price > 0.0 && need > 0)
    {
        plan.push_back({OrderSide::BUY, buy_price, need});
        trading_.base_target_quantity = base_quantity;
    }
}

void DeviationScaleStrategy::plan_sell_steps(int position, double current_price, double sell_base_line,
                                             std::vector<SplitStep>& plan) const
{
    // 추세 청산 모드는 익절 매도를 깔지 않는다 — 청산은 exit_on_trend_loss 하나다. [why D-158]
    if (parameters_.trend_exit_period > 0)
    {
        return;
    }

    // 매도 밴드: 이격 +deviation_sell%*i. 보유분을 split_step_count로 균등 분할(숏 방지).
    //  sell_base_average면 기준점이 평단이다. 목표가가 현재가 아래면(이미 목표 초과) 현재가에
    //  낸다 — 지정가로 남되 다음 체결에 붙는다.
    int sell_available = position;
    const int sell_per_step = parameters_.split_step_count > 0
                             ? (position + parameters_.split_step_count - 1) / parameters_.split_step_count
                             : position; // ceil
    const bool sell_from_average = parameters_.sell_base_average && liquidation_.last_average_price > 0.0;
    const double sell_base = sell_from_average ? liquidation_.last_average_price : sell_base_line;

    for (int split_step_index = 1; split_step_index <= parameters_.split_step_count && sell_available > 0;
         ++split_step_index)
    {
        double sell_price =
            round_to_tick(sell_base * (1.0 + parameters_.deviation_sell * split_step_index / 100.0), OrderSide::SELL);

        if (sell_from_average && current_price > 0.0 && sell_price <= current_price)
        {
            sell_price = round_to_tick(current_price, OrderSide::SELL);
        }

        int quantity = sell_available < sell_per_step ? sell_available : sell_per_step;

        if (sell_price > 0.0 && quantity > 0)
        {
            plan.push_back({OrderSide::SELL, sell_price, quantity});
            sell_available -= quantity;
        }
    }
}

void DeviationScaleStrategy::plan_buy_steps(double current_price, double simple_moving_average, bool warming,
                                            double buy_base_line, double split_step_notional,
                                            std::vector<SplitStep>& plan) const
{
    // 매수 밴드(물타기): 이격 −deviation_buy%*i, buy_split_steps층. 분할 단계당 자본의 split_step_notional. 종목당
    // 상한은 OrderGate가 캡.
    //  점진 진입: add_below_sma_only면 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 물타기를 깐다.
    //  → 활성 즉시 base+물타기를 한꺼번에 예약해 1분 만에 10% 만재되던 성격을 제거. 기준선 위/근처에선
    //    base(+익절 매도레그)만 유지하고, 진짜 눌림이 와야 평단을 낮춘다.
    // 워밍업(기준선=일봉SMA20) 구간에는 물타기를 잠근다. 존 진입 조건이 이격 -pullback_percent~
    //  +entry_upper_pct라 `현재가 < 일봉SMA20`이 거의 항상 참이 되어, 3분봉 기준선이 뜻하던
    //  "단기 눌림에서만 추가"가 사실상 상시 개방으로 바뀐다. 변동성이 가장 큰 첫 60분에
    //  base와 물타기가 한꺼번에 나가는 것을 막는다(base 진입과 익절 매도는 그대로 둔다).
    if (!((!parameters_.add_below_simple_moving_average_only || current_price < simple_moving_average) && !warming))
    {
        return;
    }

    for (int buy_split_step_index = 1; buy_split_step_index <= parameters_.buy_split_steps; ++buy_split_step_index)
    {
        double buy_price = round_to_tick(
            buy_base_line * (1.0 - parameters_.deviation_buy * buy_split_step_index / 100.0), OrderSide::BUY);
        int split_step_quantity = quantity_for(split_step_notional, buy_price);

        if (split_step_quantity <= 0)
        {
            split_step_quantity = parameters_.step_quantity; // 자본 미상 폴백
        }

        if (buy_price > 0.0 && split_step_quantity > 0)
        {
            plan.push_back({OrderSide::BUY, buy_price, split_step_quantity});
        }
    }
}

std::string DeviationScaleStrategy::plan_signature(const SplitPlan& split_plan, bool entry_on)
{
    // ── no-change 가드: 처닝 차단. 분할 매수가 살아있고 (a)계획 시그니처가 직전과 동일하거나
    //    (b)SMA 이동이 reprice_move_ticks 데드밴드 이내이고 포지션도 그대로면 재발주 스킵.
    //    (b)가 핵심: SMA가 틱경계를 스치며 미세이동할 때마다 전량 취소·재발주해 매도 분할 단계가
    //    체결 전에 취소되고 매수만 쌓여 pos가 편증하던 처닝을 차단(reprice_move_ticks 구현).
    std::string signal;

    for (const auto& split_step : split_plan.steps)
    {
        signal += split_step.side == OrderSide::BUY ? "B" : "S";
        signal += format_one_decimal(split_step.price) + "x" + std::to_string(split_step.quantity) + "|";
    }

    //  active 상태를 시그니처에 접미 → 국면 플립 시 no-change 가드에 걸리지 않고 재구성되어
    //  기존 매수 예약이 cancel_all로 취소된다(플립 후 매수만 잔존하는 구멍 차단).
    //  신규매수 차단(entry_halt)도 같은 축이다 — 게이트가 거부만 하면 계획이 안 바뀌어
    //  차단 해제 뒤에도 매수 분할 단계가 돌아오지 않았다. 차단 중엔 매수 분할 단계를 걷고(취소),
    //  풀리면 시그니처가 바뀌어 다시 깐다. 떨림은 D-033 체류가 막는다. [why D-033]
    signal += entry_on ? "A1" : "A0";
    signal += 'S';
    signal += std::to_string(static_cast<int>(split_plan.entry_scale_ratio * 10.0 + 0.5)); // 매수비율 0~10
    return signal;
}

bool DeviationScaleStrategy::clear_dust(const SplitPlan& split_plan, bool entry_on, int position, double current_price,
                                        std::vector<OrderSignal>& out, std::chrono::steady_clock::time_point now)
{
    // 먼지 정리: 보유 평가금이 dust_krw 아래인데 깔 매수 분할 단계가 없으면(베이스 끝·물타기 없음·진입 차단)
    //  이 보유는 커질 길이 없이 슬롯만 차지한다. 익절 지정가 대신 시장가로 정리한다. 매수 분할 단계가 있으면
    //  베이스 잔량이 채워지는 중이라 둔다. 오늘 진입해 아직 줄이지 않은 보유는 분할 진행 중이라 먼지로 보지 않는다
    //  (판정은 devscale_rules::is_dust). 재시도 간격은 emit_liquidation 백오프가 맡는다. [why D-081]
    const bool entered_today = !trading_.entry_date.empty() && trading_.entry_date == kst_date_yyyymmdd();

    if (devscale_rules::is_dust(position, current_price, parameters_.dust_krw, entered_today, trading_.peak_position))
    {
        bool has_buy = false;

        for (const auto& split_step : split_plan.steps)
        {
            has_buy = has_buy || (entry_on && split_step.side == OrderSide::BUY);
        }

        if (!has_buy)
        {
            cancel_all(out);

            // 백오프 창 안이면 사유 문자열을 만들지 않는다 — 먼지 판정은 틱마다 참으로 남는다.
            if (liquidation_due(position, now))
            {
                emit_liquidation(out, position, now,
                                 "먼지 정리(평가금 " + format_one_decimal(position * current_price) + " < " +
                                     format_one_decimal(parameters_.dust_krw) + ")");
            }

            return true;
        }
    }

    return false;
}

bool DeviationScaleStrategy::rebuild_suppressed(const std::string& signal, int position, double split_buy_reference,
                                                int planned_sell_quantity)
{
    const double reprice_band = parameters_.reprice_move_ticks * tick_size(split_buy_reference);
    const bool simple_moving_average_quiet =
        trading_.last_split_buy_reference > 0.0 && std::fabs(split_buy_reference - trading_.last_split_buy_reference) < reprice_band;

    // 덮개 점검: (a)(b)는 낸 익절 매도가 살아 있다고 믿고 건너뛴다. 그 매도가 거부됐거나(장전 세션 창 밖) 처음부터
    //  못 냈으면(매도가능 0) 같은 계획이 이어지는 한 다시 내지 않는다 — 10-02 모의 3종목은 08:30 거부 뒤 하루 종일,
    //  실계좌 138930은 16:07·19:40 뒤 각각 마감까지 익절 매도가 없었다. 장부 사본은 잔고 조회 없이 읽히므로
    //  접근자가 있을 때만 본다(REST 대체 경로로 매 하트비트 조회하지 않는다). [why D-156]
    bool cover_missing = false;

    if (trading_.last_rebuild != std::chrono::steady_clock::time_point{})
    {
        if (const auto ledger = ledger_sellable(parameters_.account, symbol_id_, parameters_.ticker))
        {
            const long long since_rebuild =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - trading_.last_rebuild).count();
            cover_missing = devscale_rules::sell_cover_missing(position, planned_sell_quantity, ledger->sellable,
                                                               since_rebuild, kSellCoverRecheckSec);

            if (cover_missing)
            {
                LOG_WARN("[" + id() + "] " + display() + " 익절 매도가 보유를 덮지 못함 — 재구성 pos=" +
                         std::to_string(position) + " 묶이지않은수량=" + std::to_string(ledger->sellable) +
                         " 계획매도=" + std::to_string(planned_sell_quantity));
            }
        }
    }

    // (a) 계획 시그니처+pos가 직전과 동일하면 live 유무와 무관하게 스킵.
    //     매도가능=0이라 아무것도 못 깔아 live_가 빈 채로 남을 때(장부 보유↔매도가능 괴리)
    //     매 하트비트 재진입해 잔고조회를 난사하던 스핀을 차단. 체결로 pos가 바뀌면 즉시 재구성.
    // (b) 데드밴드(reprice 이내 미세이동)+position 동일 스킵은 살아있는 분할 매수에만 적용.
    if (!cover_missing && signal == trading_.last_split_buy_signal && position == trading_.last_position)
    {
        return true; // 동일 계획 → 유지(빈 계획 포함)
    }

    if (!cover_missing && !live_.empty() && simple_moving_average_quiet && position == trading_.last_position)
    {
        return true; // 데드밴드 내 미세이동 → 유지
    }

    // (c) 재구성 최소 간격. (a)(b)는 둘 다 position == last_position을 요구하므로, 부분체결이
    //     연달아 들어오는 종목은 체결마다 분할 매수를 통째로 헐고 다시 깐다. 분할 단계 2개면
    //     체결 1건에 취소 2 + 신규 2가 나가고, 이게 계좌 공용 주문예산(5/s·20/min)을
    //     한 종목이 독점한다(09-08 12:31~12:36 016610 단독 84건 = 전체의 8할).
    //     여기서 막아도 기존 분할 단계는 살아 있으므로 체결 기회를 잃지 않는다. 잠깐 크기가
    //     낡은 채로 유지될 뿐이다. 청산(emit_liquidation)은 이 경로를 타지 않는다.
    //     live_가 비어 있어도 적용한다 — 매도가능=0으로 아무것도 못 깔면 live_가 빈 채
    //     남는데, 그때 이 가드를 건너뛰면 하트비트(3초)마다 재구성·잔고조회가 돈다.
    if (parameters_.min_rebuild_sec > 0 && trading_.last_rebuild != std::chrono::steady_clock::time_point{})
    {
        const auto since =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - trading_.last_rebuild).count();

        if (since < parameters_.min_rebuild_sec)
        {
            return true;
        }
    }

    return false;
}

void DeviationScaleStrategy::place_split_steps(const SplitPlan& split_plan, bool entry_on,
                                               const std::string& buy_context, int position,
                                               std::vector<OrderSignal>& out)
{
    int sell_room = -1; // -1=미조회(지연). 첫 매도 분할 단계에서 1회 조회.

    for (const auto& split_step : split_plan.steps)
    {
        if (split_step.side == OrderSide::SELL)
        {
            if (sell_room < 0)
            {
                sell_room = this->sell_room(position); // 안전 우선: 불확실하면 0(매도 보류). 방금 취소한 자기 매도는 더한다
            }

            int quantity = split_step.quantity < sell_room ? split_step.quantity : sell_room;

            if (quantity <= 0)
            {
                continue; // 매도가능 소진/없음 → 이 분할 단계 스킵
            }

            place(out, OrderSide::SELL, split_step.price, quantity,
                  "익절밴드 지정가=" + format_one_decimal(split_step.price));
            sell_room -= quantity;
        }
        else if (entry_on) // 비활성 국면이면 매수 분할 단계 스킵(진입 차단)
        {
            place(out, split_step.side, split_step.price, split_step.quantity, buy_context);
        }
    }
}

int DeviationScaleStrategy::quantity_for(double notional, double price)
{
    if (price <= 0.0 || notional <= 0.0)
    {
        return 0;
    }

    int quantity = static_cast<int>(std::floor(notional / price));
    return quantity > 0 ? quantity : 0;
}

void DeviationScaleStrategy::place(std::vector<OrderSignal>& out, OrderSide side, double price, int quantity,
                                   std::string reason)
{
    if (quantity <= 0)
    {
        return; // quantity=0 NEW 발주 억제 — 게이트 거부·로그 노이즈 원천 차단(SELL은 상위서도 클램프)
    }

    std::string order_id = next_order_id(side == OrderSide::BUY ? "B" : "S");
    const uint64_t order_number = next_client_order_number(); // 라우터가 취소 대상을 찾는 키. 문자열은 로그용
    OrderSignal signal = make_signal(parameters_.ticker, symbol_id_, side, OrderType::LIMIT, quantity);
    signal.price = price;
    signal.action = OrderAction::NEW;
    signal.client_order_id = order_id;
    signal.client_order_number = order_number;
    signal.account_id = parameters_.account;
    signal.reason = std::move(reason); // 판단 근거를 신호에 실어 영속
    out.push_back(std::move(signal));
    live_.push_back({std::move(order_id), order_number, side, quantity});
}

bool DeviationScaleStrategy::cancel_all(std::vector<OrderSignal>& out)
{
    if (live_.empty())
    {
        return false;
    }

    for (auto& live_entry : live_) // live_는 아래에서 비우므로 주문 id를 옮긴다
    {
        push_cancel(out, live_entry);
    }

    live_.clear();
    return true;
}

bool DeviationScaleStrategy::cancel_buys(std::vector<OrderSignal>& out)
{
    bool cancelled = false;

    for (auto& live_entry : live_)
    {
        if (live_entry.side == OrderSide::BUY)
        {
            push_cancel(out, live_entry);
            cancelled = true;
        }
    }

    std::erase_if(live_, [](const Live& live_entry)
    {
        return live_entry.side == OrderSide::BUY;
    });
    return cancelled;
}

void DeviationScaleStrategy::push_cancel(std::vector<OrderSignal>& out, Live& live_entry)
{
    if (live_entry.side == OrderSide::SELL)
    {
        cancelled_sell_quantity_ += live_entry.quantity;
    }

    OrderSignal signal = make_signal(parameters_.ticker, symbol_id_, live_entry.side, OrderType::LIMIT, 0);
    signal.action = OrderAction::CANCEL;
    signal.original_client_order_id = std::move(live_entry.order_id);
    signal.original_client_order_number = live_entry.order_number;
    signal.account_id = parameters_.account;
    out.push_back(std::move(signal));
}

double DeviationScaleStrategy::tick_size(double price)
{
    return krx::tick_size(price);
}

double DeviationScaleStrategy::round_to_tick(double price, OrderSide side)
{
    return krx::round_to_tick(price, side);
}

std::string DeviationScaleStrategy::next_order_id(const char* tag)
{
    return make_order_id(tag, ++sequence_);
}
