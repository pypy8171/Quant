// strategy/DeviationScaleStrategy.cpp — 이격 분할 전략 본체: 생성·시작·종료와 체결 한 건의 평가 순서(on_trade_batch).
//  존 판정은 DeviationScaleZone.cpp, 분할 주문은 DeviationScaleOrders.cpp, 청산은 DeviationScaleExit.cpp,
//  봉·프리페치는 DeviationScaleBars.cpp에 있다. 관련 결정: D-033·D-069·D-072·D-081·D-114·D-156.
#include "strategy/DeviationScaleStrategy.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

DeviationScaleStrategy::DeviationScaleStrategy(Params parameters)
    : parameters_(std::move(parameters)), aggregator_(aggregator_config(parameters_))
{
    id_ = parameters_.id_prefix + "_" + parameters_.ticker;
    websocket_bars_ = (parameters_.bar_source == "ws");

    // 닫힌 1분봉 한 줄 — 분마다 종목마다 나오므로 DEBUG. 비교표(compare_ws_bars.py)가 이 줄을 REST 1분봉과
    //  같은 규칙으로 접어 interval_min 봉끼리 견준다.
    aggregator_.set_sink(
        [this](const MarketData& market_data)
        {
            LOG_DEBUG("[" + id() + "] 봉 닫힘 src=ws t=" +
                      kst::hhmmss(std::chrono::system_clock::to_time_t(market_data.timestamp)).substr(0, 4) +
                      " o=" + format_one_decimal(market_data.open) + " h=" + format_one_decimal(market_data.high) +
                      " l=" + format_one_decimal(market_data.low) + " c=" + format_one_decimal(market_data.close) +
                      " v=" + std::to_string(market_data.volume));
        });

    if (parameters_.split_step_count < 1)
    {
        parameters_.split_step_count = 1;
    }

    if (parameters_.buy_split_steps < 0)
    {
        parameters_.buy_split_steps = parameters_.split_step_count;
    }

    // [formula] 지터 = hash(티커) mod (봉 길이 × 비율). 같은 종목은 매번 같은 지연을 받아
    //  조회 순서가 재현된다.
    const int jitter_percent = (std::max)(0, (std::min)(100, parameters_.prefetch_jitter_percent));
    const int span = (std::max)(1, parameters_.interval_min * 60 * jitter_percent / 100);
    prefetch_jitter_sec_ = static_cast<int>(std::hash<std::string>{}(parameters_.ticker) % static_cast<size_t>(span));

    if (parameters_.simple_moving_average_period < 2)
    {
        parameters_.simple_moving_average_period = 2;
    }

    if (parameters_.interval_min < 1)
    {
        parameters_.interval_min = 1;
    }
}

DeviationScaleStrategy::~DeviationScaleStrategy()
{
    stop_prefetch();
}

std::string DeviationScaleStrategy::display() const
{
    return parameters_.name.empty() ? parameters_.ticker : parameters_.ticker + "(" + parameters_.name + ")";
}

std::vector<WatchSpec> DeviationScaleStrategy::get_watch_specifications() const
{
    return {{parameters_.ticker, Market::KR, "", /*trade_only=*/true}};
}

std::string DeviationScaleStrategy::describe() const
{
    // 사이징은 자본×base_percent(또는 notional_krw) 우선, base_quantity/step_quantity는 폴백이다.
    return "DeviationScale | " + display() + " | base_pct=" + format_one_decimal(parameters_.base_percent * 100.0) +
           "% max_pct=" + format_one_decimal(parameters_.max_percent * 100.0) + "% (fallback qty " +
           std::to_string(parameters_.base_quantity) + "/" + std::to_string(parameters_.step_quantity) +
           ") sma=" + std::to_string(parameters_.simple_moving_average_period) + "(" +
           std::to_string(parameters_.interval_min) + "m src=" + (websocket_bars_ ? "ws" : "rest") +
           ") dev_sell=" + format_one_decimal(parameters_.deviation_sell) +
           "% dev_buy=" + format_one_decimal(parameters_.deviation_buy) +
           "% split_steps=" + std::to_string(parameters_.split_step_count) + "/buy" +
           std::to_string(parameters_.buy_split_steps) + " zone=" + format_one_decimal(-parameters_.pullback_percent) +
           "~+" + format_one_decimal(parameters_.entry_upper_percent) + "%" +
           (parameters_.stop_loss_percent > 0.0 ? " stop=-" + format_one_decimal(parameters_.stop_loss_percent) + "%"
                                                : "") +
           (parameters_.sell_base_average ? " sell@avg" : "") +
           (parameters_.trail_arm_percent > 0.0 ? " peak-trail" : "") +
           (parameters_.entry_atr_max_percent > 0.0
                ? " atr<=" + format_one_decimal(parameters_.entry_atr_max_percent) + "%"
                : "") +
           (parameters_.entry_open_deviation_min_percent > Params::kOpenDeviationOff
                ? " open_dev>=" + format_one_decimal(parameters_.entry_open_deviation_min_percent) + "%"
                : "") +
           (parameters_.no_new_entry_hhmm > 0 ? " no_entry>=" + std::to_string(parameters_.no_new_entry_hhmm) : "") +
           (parameters_.market_close_hhmm >= devscale_rules::kNoMarketCloseHhmm ? " carry" : "");
}

void DeviationScaleStrategy::on_start()
{
    symbol_id_ = symbol_of(parameters_.ticker); // 집계기 키·틱 비교·신호 도장 — 여기서 한 번

    // 손절·트레일 조건을 주문 쪽 보호 주문 표에 올린다 — 이 전략이 멈춰도 표가 보유분을 지킨다. [why D-114]
    arm_protective(parameters_.account, parameters_.ticker, symbol_id_, parameters_.stop_loss_percent,
                   parameters_.trail_arm_percent, parameters_.trail_percent);
    live_.clear();
    trading_.last_split_buy_reference = 0.0;
    trading_.last_position = -1;
    trading_.last_rebuild = std::chrono::steady_clock::time_point{};
    trading_.last_split_buy_signal.clear();
    trading_.in_zone = false;
    trading_.entry_closed_logged = false;
    liquidation_.next = std::chrono::steady_clock::time_point{};
    liquidation_.last_position = -1;
    liquidation_.fail_streak = 0;
    trading_.last_work = std::chrono::steady_clock::time_point{};
    daily_.reset();
    equity_ = 0.0;
    sequence_ = 0;
    trading_.last_zone = false;
    trading_.zone_log_ts = std::chrono::steady_clock::time_point{};
    trading_.last_warm_log_ms = 0;
    liquidation_.last_price = 0.0;
    liquidation_.last_average_price = 0.0;
    liquidation_.average_position_seen = 0;
    trading_.base_target_quantity = 0;
    trading_.peak_position = 0;
    trading_.entry_date.clear();
    trading_.entry_cutoff_logged = false;
    liquidation_.hold_peak_price = 0.0;
    trading_.entry_filter_date.clear();
    trading_.day_entry_allowed = true;
    // 집계기 이력은 지우지 않는다(재등록 경로에서 같은 날이면 그대로 쓸 수 있다). 날짜가 바뀌었으면
    //  on_trade_batch의 날짜 검사가 비운다. 시드는 다시 받는다.
    seeded_version_ = 0;
    websocket_live_ = false;
    reseed_pending_ = true;
    seed_wanted_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.daily.reset();
        snapshot_.daily_date.clear();
        snapshot_.equity = 0.0;
        snapshot_.bars.reset();
        snapshot_.bars_bucket = -1;
    }

    LOG_INFO("[" + id() + "] 시작 — " + describe());
    // set_kis()·set_prefetch_pool()이 on_start 직전 호출됨(Engine start/재스캔 둘 다) → 여기서 프리페치 등록.
    stop_prefetch(); // 재등록 경로 대비 — 이전 등록의 해제를 여기서 끝낸다

    if (prefetch_pool_)
    {
        prefetch_task_ = prefetch_pool_->add([this]
        {
            prefetch_once();
        });
    }
}

void DeviationScaleStrategy::on_stop()
{
    stop_prefetch();

    // 보호 주문 표에서 내린다 — 떨어진 전략의 규칙이 남아 다른 전략의 보유분을 팔면 안 된다. [why D-114]
    disarm_protective(parameters_.account, symbol_id_);

    // 마지막 진행 봉(마감 동시호가 뒤엔 다음 틱이 없다)을 시계로 닫아 비교표에 남긴다. 샤드 스레드는 이미
    //  이 전략을 안 부른다(엔진 종료 뒤이거나 재스캔이 뗀 뒤). [why D-074]
    if (websocket_bars_)
    {
        aggregator_.close_stale(symbol_id_, std::time(nullptr));
    }

    LOG_INFO("[" + id() + "] 종료");
}

void DeviationScaleStrategy::on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out)
{
    if (!same_symbol(symbol_id_, parameters_.ticker, trade.symbol_id, trade.ticker))
    {
        return;
    }

    cancelled_sell_quantity_ = 0; // 이번 처리에서 낸 취소만 센다(sell_room)

    if (trade.price > 0.0)
    {
        liquidation_.last_price = trade.price; // 시장가 청산의 명목 평가 기준가(reference_price). 장 마감 경로보다 먼저 갱신
    }

    if (fire_startup_check(trade, out))
    {
        return;
    }

    feed_bar_aggregator(trade);

    const int hhmm = kst_hhmm(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

    if (close_at_market_end(hhmm, out))
    {
        return;
    }

    // ── 무거운 작업 스로틀(3분봉 조회·발주) ──────────────────────────────
    const auto now = std::chrono::steady_clock::now();

    if (trading_.last_work.time_since_epoch().count() != 0 &&
        now - trading_.last_work < std::chrono::milliseconds(parameters_.min_action_ms))
    {
        return;
    }

    trading_.last_work = now;

    const double current_price = trade.price;

    if (current_price <= 0.0)
    {
        return;
    }

    const std::optional<DecisionBars> decision_bars = load_decision_bars(trade, current_price);

    if (!decision_bars)
    {
        return; // 일봉 미준비 — 프리페치 대기
    }

    const bool holding = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker) > 0;
    const ZoneJudgement zone_judgement = judge_zone(current_price, holding, now);

    if (!zone_judgement.hold_zone && exit_on_zone_loss(out, now))
    {
        return;
    }

    if (exit_on_protective_rules(current_price, out, now))
    {
        return;
    }

    if (!zone_judgement.zone)
    {
        // 진입 축만 닫혔다. 미체결 매수는 거두되 보유와 익절 매도는 그대로 둔다 — 장중에 이평이
        //  깨졌다고 파는 대신 되돌아오면 그대로 이어간다. 청산은 위 hold_zone이 맡는다.
        //  매도까지 거두면 정배열이 현재가 한 호가에 걸쳐 오갈 때 익절 매도를 매번 취소·재발주한다
        //  (10-06 모의 005930: 11:11부터 274,000↔274,250 사이 떨림에 1분마다 SELL 3주 접수·취소 12회). [why D-156]
        if (cancel_buys(out) && !trading_.entry_closed_logged)
        {
            LOG_INFO("[" + id() + "] " + display() + " 진입 축 닫힘(장중 정배열) — 미체결 매수 취소, 보유·익절 매도 유지");
            trading_.entry_closed_logged = true;
        }

        return;
    }

    trading_.entry_closed_logged = false;

    std::optional<RebuildState> rebuild =
        prepare_rebuild(decision_bars->bars, zone_judgement, current_price, hhmm, out, now);

    if (!rebuild)
    {
        return;
    }

    rebuild_split_steps(trade, zone_judgement, decision_bars->local, current_price, *rebuild, out, now);
}

bool DeviationScaleStrategy::fire_startup_check(const TradeData& trade, std::vector<OrderSignal>& out)
{
    if (!parameters_.startup_check_buy || trade.price <= 0.0 || trade.price > parameters_.startup_check_max_price)
    {
        return false;
    }

    // [inv] 프로세스 전체에서 한 번 — 샤드 스레드 여럿이 동시에 와도 exchange가 하나만 통과시킨다.
    static std::atomic<bool> startup_check_fired{false};

    if (startup_check_fired.exchange(true))
    {
        return false;
    }

    OrderSignal signal = make_market_sell(1, "STARTUP_CHECK_BUY");
    signal.side = OrderSide::BUY;
    signal.reference_price = trade.price;
    LOG_INFO("[" + id() + "] 기동 점검 매수 1주 — 첫 체결 " + std::to_string(trade.hhmmss) + " " +
             std::to_string(static_cast<long long>(trade.price)) + "원");
    out.push_back(std::move(signal));
    return true;
}

std::optional<DeviationScaleStrategy::RebuildState>
DeviationScaleStrategy::prepare_rebuild(const std::vector<MarketData>& bars, const ZoneJudgement& zone_judgement,
                                        double current_price, int hhmm, std::vector<OrderSignal>& out,
                                        std::chrono::steady_clock::time_point now)
{
    const std::optional<Baseline> baseline =
        resolve_baseline(bars, zone_judgement.average_20, current_price, out, now);

    if (!baseline)
    {
        return std::nullopt;
    }

    RebuildState state;
    state.simple_moving_average = baseline->value;
    state.warming = baseline->warming;
    state.position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);
    const int position = state.position;

    // ── 하루 단위 진입 필터(전일 ATR·개장 이격) — 보유가 없을 때만 새 진입을 막는다 ──
    if (position <= 0 && !day_entry_allowed(current_price, zone_judgement.previous_average_20, hhmm))
    {
        cancel_all(out);
        return std::nullopt;
    }

    // 스탑 청산이 아직 진행 중이면(청산을 냈는데 보유가 줄지 않음) 재구성하지 않는다. 시장가 스탑이 체결되기 전에
    //  다음 하트비트가 익절 지정가 매도를 다시 깔면, 라우터가 매도가능 0을 풀려고 살아 있는 스탑 주문을 취소하려 든다
    //  (09-14 13:00 079650: 스탑 RTT 7초 사이 익절 매도가 들어가 취소 시도, 체결이 먼저라 피해 없음).
    //  청산 진행은 스탑 블록·라우터 재시도가 맡고, 쿨다운이 끝나면 여기로 돌아온다.
    if (position > 0 && liquidation_.last_position > 0 && position >= liquidation_.last_position &&
        liquidation_.stop_cooldown_until != std::chrono::steady_clock::time_point{} && now < liquidation_.stop_cooldown_until)
    {
        return std::nullopt;
    }

    state.plan = plan_split_steps(position, current_price, state.simple_moving_average, state.warming, now);

    // 신규 진입 마감: 보유가 없으면 미체결 매수만 거두고 끝낸다. 보유가 있으면 아래 entry_on을 끄는 것으로 매수 분할
    //  단계만 걷고 익절 매도는 그대로 재구성한다. 계획 갱신(trading_.peak_position·재진입 대기)은 위에서 이미 했다.
    state.entry_time_closed = devscale_rules::entry_time_closed(hhmm, parameters_.no_new_entry_hhmm);

    if (state.entry_time_closed && !trading_.entry_cutoff_logged)
    {
        LOG_INFO("[" + id() + "] " + display() + " 신규 진입 마감(" + std::to_string(parameters_.no_new_entry_hhmm) +
                 ") — 매수 분할 단계를 깔지 않는다, pos=" + std::to_string(position));
        trading_.entry_cutoff_logged = true;
    }

    if (position <= 0 && state.entry_time_closed)
    {
        cancel_all(out);
        return std::nullopt;
    }

    return state;
}

void DeviationScaleStrategy::rebuild_split_steps(const TradeData& trade, const ZoneJudgement& zone_judgement,
                                                 bool local_bars, double current_price, RebuildState& state,
                                                 std::vector<OrderSignal>& out,
                                                 std::chrono::steady_clock::time_point now)
{
    const SplitPlan& plan = state.plan;
    const int position = state.position;
    const double simple_moving_average = state.simple_moving_average;

    // 국면 게이트: 비활성 국면(regime→전략 자동선택에서 미선택)에선 매수(진입·물타기)
    //  분할 단계를 깔지 않는다. 익절 매도·청산은 국면과 무관하게 유지(is_active 계약: 진입만 차단).
    //  신규매수 차단(entry_halt)도 같은 축이다. 스탑·트레일 뒤 쿨다운과 전량 청산 뒤 재진입 대기도
    //  같은 축이다 — 존이 열려 있어도 분할 매수를 걷는다. 시그니처에 붙는 이유는 plan_signature. [why D-033]
    const bool cooling =
        (liquidation_.stop_cooldown_until != std::chrono::steady_clock::time_point{} && now < liquidation_.stop_cooldown_until) ||
        (liquidation_.reentry_cooldown_until != std::chrono::steady_clock::time_point{} && now < liquidation_.reentry_cooldown_until);
    const bool entry_on =
        is_active() && !entry_halted() && !cooling && !state.entry_time_closed && plan.entry_scale_ratio > 0.0;
    std::string signal = plan_signature(plan, entry_on);

    if (clear_dust(plan, entry_on, position, current_price, out, now))
    {
        return;
    }

    int planned_sell_quantity = 0;

    for (const auto& split_step : plan.steps)
    {
        if (split_step.side == OrderSide::SELL)
        {
            planned_sell_quantity += split_step.quantity;
        }
    }

    if (rebuild_suppressed(signal, position, simple_moving_average, planned_sell_quantity))
    {
        return;
    }

    // ── 재구성: 기존 취소 후 신규 지정가 ──────────────────────────────────
    //  익절 매도는 재구성 시점의 실매도가능분(ord_psbl_qty)으로 클램프 → 장부 보유와
    //  매도가능 괴리(예약매도·미결제)로 KIS가 전량 거부(40240000 "잔고내역 없습니다")하던
    //  것을 차단. 청산 경로(emit_liquidation)와 동일 원칙. 잔고조회는 재구성 시 1회만
    //  (지연에 민감한 경로 부하 억제 — 매수는 캡을 OrderGate가 처리하므로 클램프 불필요).
    cancel_all(out);
    // 이 분할 매수를 깐 판단 근거 — 존 판정 지표를 신호에 실어 영속(로그 재구성 불필요).
    const std::string buy_context = std::string("정배열눌림진입 이격=") + format_one_decimal(zone_judgement.deviation20_percent) +
                                    "% 일봉SMA20=" + format_one_decimal(zone_judgement.average_20) +
                                    " 현재가=" + format_one_decimal(current_price) +
                                    entry_context_text(trade, current_price);
    place_split_steps(plan, entry_on, buy_context, position, out);

    if (position <= 0 && entry_on)
    {
        trading_.entry_date = kst_date_yyyymmdd(); // 오늘 무포지션에서 낸 베이스 — 체결되면 먼지 정리가 분할 진행 중으로 본다
    }

    trading_.last_split_buy_reference = simple_moving_average;
    trading_.last_split_buy_signal = std::move(signal);
    trading_.last_position = position;
    trading_.last_rebuild = std::chrono::steady_clock::now();
    LOG_INFO("[" + id() + "] 분할 매수 재구성 sma=" + format_one_decimal(simple_moving_average) +
             (state.warming ? "(일봉)" : "") + " src=" + (local_bars ? "ws" : "rest") +
             " px=" + format_one_decimal(current_price) + " pos=" + std::to_string(position) +
             " live=" + std::to_string(live_.size()) +
             " 명목=" + std::to_string(static_cast<long long>(plan.base_notional + plan.split_step_budget)) + "원");
}

struct tm DeviationScaleStrategy::kst_tm()
{
    return kst::to_tm(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

std::string DeviationScaleStrategy::kst_date_yyyymmdd()
{
    return kst::date_yyyymmdd(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

std::string DeviationScaleStrategy::format_one_decimal(double value)
{
    char byte_value[32];
    std::snprintf(byte_value, sizeof(byte_value), "%.1f", value);
    return std::string(byte_value);
}
