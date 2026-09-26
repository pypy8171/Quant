#include "strategy/DeviationScaleStrategy.h"

namespace
{
// 예열 안내 로그 사이 최소 간격 — 종목마다 나오므로 1분에 한 줄로 묶는다.
constexpr int64_t kWarmLogIntervalMs = 60000;

// 하루 분 수 — 날짜+시각을 분 일련번호 하나로 접을 때 쓴다.
constexpr int kMinutesPerDay = 1440;
} // namespace

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
           (parameters_.entry_open_deviation_min_percent > -99.0
                ? " open_dev>=" + format_one_decimal(parameters_.entry_open_deviation_min_percent) + "%"
                : "") +
           (parameters_.market_close_hhmm >= devscale_rules::kNoMarketCloseHhmm ? " carry" : "");
}

void DeviationScaleStrategy::on_start()
{
    symbol_id_ = symbol_of(parameters_.ticker); // 집계기 키·틱 비교·신호 도장 — 여기서 한 번

    // 손절·트레일 조건을 주문 쪽 보호 주문 표에 올린다 — 이 전략이 멈춰도 표가 보유분을 지킨다. [why D-114]
    arm_protective(parameters_.account, parameters_.ticker, symbol_id_, parameters_.stop_loss_percent,
                   parameters_.trail_arm_percent, parameters_.trail_percent);
    live_.clear();
    last_split_buy_reference_ = 0.0;
    last_position_ = -1;
    last_rebuild_ = std::chrono::steady_clock::time_point{};
    last_split_buy_signal_.clear();
    in_zone_ = false;
    entry_closed_logged_ = false;
    liquidation_next_ = std::chrono::steady_clock::time_point{};
    liquidation_last_position_ = -1;
    liquidation_fail_streak_ = 0;
    last_work_ = std::chrono::steady_clock::time_point{};
    daily_.reset();
    equity_ = 0.0;
    sequence_ = 0;
    last_zone_ = false;
    zone_log_ts_ = std::chrono::steady_clock::time_point{};
    last_warm_log_ms_ = 0;
    last_price_ = 0.0;
    last_average_price_ = 0.0;
    average_position_seen_ = 0;
    base_target_quantity_ = 0;
    peak_position_ = 0;
    hold_peak_price_ = 0.0;
    entry_filter_date_.clear();
    day_entry_allowed_ = true;
    // 집계기 이력은 지우지 않는다(재등록 경로에서 같은 날이면 그대로 쓸 수 있다). 날짜가 바뀌었으면
    //  on_trade_batch의 날짜 검사가 비운다. 시드는 다시 받는다.
    seeded_version_ = 0;
    websocket_live_ = false;
    reseed_pending_ = true;
    seed_wanted_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(snap_mutex_);
        snap_daily_.reset();
        snap_daily_date_.clear();
        snap_equity_ = 0.0;
        snap_bars_.reset();
        snap_bars_bucket_ = -1;
    }

    LOG_INFO("[" + id() + "] 시작 — " + describe());
    // set_kis()·set_prefetch_pool()이 on_start 직전 호출됨(Engine start/재스캔 둘 다) → 여기서 프리페치 등록.
    stop_prefetch(); // 재등록 경로 대비 — 이전 등록의 해제를 여기서 끝낸다

    if (prefetch_pool_)
    {
        prefetch_task_ = prefetch_pool_->add([this] { prefetch_once(); });
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

    if (trade.price > 0.0)
    {
        last_price_ = trade.price; // 시장가 청산의 명목 평가 기준가(reference_price). 장 마감 경로보다 먼저 갱신
    }

    feed_bar_aggregator(trade);

    const int hhmm = kst_hhmm();

    if (close_at_market_end(hhmm, out))
    {
        return;
    }

    // ── 무거운 작업 스로틀(3분봉 조회·발주) ──────────────────────────────
    const auto now = std::chrono::steady_clock::now();

    if (last_work_.time_since_epoch().count() != 0 &&
        now - last_work_ < std::chrono::milliseconds(parameters_.min_action_ms))
    {
        return;
    }

    last_work_ = now;

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

    const ZoneJudgement zone_judgement = judge_zone(current_price, now);

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
        // 진입 축만 닫혔다. 미체결 매수는 거두되 보유는 그대로 둔다 — 장중에 이평이
        //  깨졌다고 파는 대신 되돌아오면 그대로 이어간다. 청산은 위 hold_zone이 맡는다.
        if (cancel_all(out) && !entry_closed_logged_)
        {
            LOG_INFO("[" + id() + "] " + display() + " 진입 축 닫힘(장중 정배열) — 미체결 취소, 보유 유지");
            entry_closed_logged_ = true;
        }

        return;
    }

    entry_closed_logged_ = false;

    const std::optional<Baseline> baseline =
        resolve_baseline(decision_bars->bars, zone_judgement.average_20, current_price, out, now);

    if (!baseline)
    {
        return;
    }

    const double simple_moving_average = baseline->value;
    const bool warming = baseline->warming;
    int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

    // ── 하루 단위 진입 필터(전일 ATR·개장 이격) — 보유가 없을 때만 새 진입을 막는다 ──
    if (position <= 0 && !day_entry_allowed(current_price, zone_judgement.previous_average_20, hhmm))
    {
        cancel_all(out);
        return;
    }

    // 스탑 청산이 아직 진행 중이면(청산을 냈는데 보유가 줄지 않음) 재구성하지 않는다. 시장가 스탑이 체결되기 전에
    //  다음 하트비트가 익절 지정가 매도를 다시 깔면, 라우터가 매도가능 0을 풀려고 살아 있는 스탑 주문을 취소하려 든다
    //  (09-14 13:00 079650: 스탑 RTT 7초 사이 익절 매도가 들어가 취소 시도, 체결이 먼저라 피해 없음).
    //  청산 진행은 스탑 블록·라우터 재시도가 맡고, 쿨다운이 끝나면 여기로 돌아온다.
    if (position > 0 && liquidation_last_position_ > 0 && position >= liquidation_last_position_ &&
        stop_cooldown_until_ != std::chrono::steady_clock::time_point{} && now < stop_cooldown_until_)
    {
        return;
    }

    const SplitPlan plan = plan_split_steps(position, current_price, simple_moving_average, warming, now);

    // G1 국면 게이트: 비활성 국면(regime→전략 자동선택에서 미선택)에선 매수(진입·물타기)
    //  분할 단계를 깔지 않는다. 익절 매도·청산은 국면과 무관하게 유지(is_active 계약: 진입만 차단).
    //  신규매수 차단(entry_halt)도 같은 축이다. 스탑·트레일 뒤 쿨다운과 전량 청산 뒤 재진입 대기도
    //  같은 축이다 — 존이 열려 있어도 분할 매수를 걷는다. 시그니처에 붙는 이유는 plan_signature. [why D-033]
    const bool cooling =
        (stop_cooldown_until_ != std::chrono::steady_clock::time_point{} && now < stop_cooldown_until_) ||
        (reentry_cooldown_until_ != std::chrono::steady_clock::time_point{} && now < reentry_cooldown_until_);
    const bool entry_on = is_active() && !entry_halted() && !cooling && plan.entry_scale_ratio > 0.0;
    std::string signal = plan_signature(plan, entry_on);

    if (clear_dust(plan, entry_on, position, current_price, out, now))
    {
        return;
    }

    if (rebuild_suppressed(signal, position, simple_moving_average))
    {
        return;
    }

    // ── 재구성: 기존 취소 후 신규 지정가 ──────────────────────────────────
    //  익절 매도는 재구성 시점의 실매도가능분(ord_psbl_qty)으로 클램프 → 원장 보유와
    //  매도가능 괴리(예약매도·미결제)로 KIS가 전량 거부(40240000 "잔고내역 없습니다")하던
    //  것을 차단. 청산 경로(emit_liquidation)와 동일 원칙. 잔고조회는 재구성 시 1회만
    //  (지연에 민감한 경로 부하 억제 — 매수는 캡을 OrderGate가 처리하므로 클램프 불필요).
    cancel_all(out);
    // G4: 이 분할 매수를 깐 판단 근거 — 존 판정 지표를 신호에 실어 영속(로그 재구성 불필요).
    const std::string buy_context = std::string("정배열눌림진입 이격=") + format_one_decimal(zone_judgement.deviation20_percent) +
                                    "% 일봉SMA20=" + format_one_decimal(zone_judgement.average_20) +
                                    " 현재가=" + format_one_decimal(current_price) +
                                    entry_context_text(trade, current_price);
    place_split_steps(plan, entry_on, buy_context, out);

    last_split_buy_reference_ = simple_moving_average;
    last_split_buy_signal_ = std::move(signal);
    last_position_ = position;
    last_rebuild_ = std::chrono::steady_clock::now();
    LOG_INFO("[" + id() + "] 분할 매수 재구성 sma=" + format_one_decimal(simple_moving_average) +
             (warming ? "(일봉)" : "") + " src=" + (decision_bars->local ? "ws" : "rest") +
             " px=" + format_one_decimal(current_price) + " pos=" + std::to_string(position) +
             " live=" + std::to_string(live_.size()) +
             " 명목=" + std::to_string(static_cast<long long>(plan.base_notional + plan.split_step_budget)) + "원");
}

void DeviationScaleStrategy::feed_bar_aggregator(const TradeData& trade)
{
    // ── 봉 집계: 스로틀 앞이다 — 모든 틱을 먹어야 고가·저가·거래량이 맞다 ──────────
    //  REST 대체 틱(폴백·구독 상한 넘침)은 넣지 않는다. 체결량이 없어 봉이 비고, 주기가 초 단위라
    //  고저가 빠진다. 그동안은 REST 봉을 쓰고, WS 틱이 돌아오면 REST로 다시 시드해 빈 자리를 메운다.
    if (websocket_bars_)
    {
        const bool live = !poller::is_rest_tick(trade);

        if (live != websocket_live_)
        {
            websocket_live_ = live;
            reseed_pending_ = true;
            LOG_INFO("[" + id() + "] 봉 출처 전환 src=" + (live ? "ws" : "rest") +
                     (live ? " — 체결 틱을 봉으로 모은다, REST 봉은 시드" : " — REST 대체 틱, REST 봉으로 판단"));
        }

        if (live)
        {
            aggregator_.on_tick(trade);
        }
    }
}

bool DeviationScaleStrategy::close_at_market_end(int hhmm, std::vector<OrderSignal>& out)
{
    // ── 장 마감 안전장치: 전량 취소 + 시장가 청산 ────────────────────────────
    if (hhmm < parameters_.market_close_hhmm)
    {
        return false;
    }

    // 매도 먼저, 취소는 뒤 — 취소 N건이 발주 스레드 큐 앞을 차지하면 매도가 그 뒤에서 기다린다
    //  (09-14 15:15 청산 신호 41건 중 접수 3건, 6종목 1,120만원 이월). 전략 쪽 매도가능 클램프(잔고 조회
    //  1회, 09-11 마감엔 종목당 13초)도 건너뛴다 — 예약 익절이 묶은 수량은 라우터가 그 자리에서 취소하고
    //  전량을 낸다(청산차단 자가정리). 뒤따르는 취소는 라우터가 "취소 불요"로 닫는다. [why D-082]
    int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);
    const std::string tag = "장 마감(" + std::to_string(hhmm) + ")";
    emit_liquidation(out, position, std::chrono::steady_clock::now(), tag, kMarketCloseBackoffMs,
                     /*clamp_sellable=*/false); // 백오프(자체 로깅)
    bool cancelled = cancel_all(out);

    if (cancelled && position <= 0)
    {
        LOG_INFO("[" + id() + "] " + tag + " — 미체결 취소(보유 0)");
    }

    return true;
}

std::optional<DeviationScaleStrategy::DecisionBars>
DeviationScaleStrategy::load_decision_bars(const TradeData& trade, double current_price)
{
    // ── 프리페치 스냅샷 스냅(일봉·자본·3분봉). 아직 준비 전이면 다음 하트비트 대기 ──
    //  무거운 REST는 공용 프리페치 풀이 미리 당겨둔다. 여기선 락을 짧게 잡고 포인터만 잡는다.
    BarSnapshot snapshot_bars;
    int bars_bucket = -1;
    uint64_t bars_version = 0;
    {
        std::lock_guard<std::mutex> lock(snap_mutex_);

        if (!snap_daily_)
        {
            return std::nullopt; // 일봉 미준비 — 프리페치 대기
        }

        daily_ = snap_daily_; // 포인터 하나 — 벡터 복사가 아니다
        equity_ = snap_equity_;
        snapshot_bars = snap_bars_;
        bars_bucket = snap_bars_bucket_;
        bars_version = snap_bars_version_;
    }

    // 판단 봉은 아래에서 접거나 진행 봉 종가를 덮으므로 이 평가가 소유해야 한다. 체결이 살아 있으면
    //  집계기에서 새로 만들어지고, 그 경로에서는 스냅샷을 아예 복사하지 않는다.
    DecisionBars decision_bars;
    std::vector<MarketData>& bars = decision_bars.bars;

    const bool local_bars = websocket_bars_ && websocket_live_;
    decision_bars.local = local_bars;

    if (websocket_bars_)
    {
        // 날짜가 바뀌면 집계기를 비운다 — REST 분봉은 당일치만 돌려주므로 어제 봉이 SMA에 섞이면 뜻이 다르다.
        std::string today = kst_ymd();

        if (aggregator_day_ != today)
        {
            aggregator_.clear(symbol_id_);
            aggregator_day_ = std::move(today);
            reseed_pending_ = true;
        }

        // 새 REST 스냅샷(1분봉)은 한 번만 시드한다. 닫힌 자리는 REST가 이기고 빈 자리는 채워지므로,
        //  폴백 동안 못 본 분·구독 뒤 늦게 붙은 종목의 앞 분이 여기서 메워진다.
        if (snapshot_bars && !snapshot_bars->empty() && bars_version != seeded_version_)
        {
            const int before = aggregator_.closed_count(symbol_id_);
            const int added = aggregator_.seed(symbol_id_, *snapshot_bars);
            const bool first = seeded_version_ == 0;
            seeded_version_ = bars_version;
            reseed_pending_ = false;
            const std::string line = "[" + id() + "] 봉 시드 src=" + (local_bars ? "ws" : "rest") + " REST " +
                                     std::to_string(snapshot_bars->size()) + "봉, 새 " + std::to_string(added) +
                                     ", 닫힌 " + std::to_string(aggregator_.closed_count(symbol_id_)) + "(전 " +
                                     std::to_string(before) + ")" + ", 진행 " +
                                     (aggregator_.current_slot(symbol_id_).valid() ? "있음" : "없음");

            // 첫 시드·전환 뒤 시드만 INFO — 워밍업 동안 봉마다 오는 시드는 DEBUG로 내린다.
            if (first || added > 0)
            {
                LOG_INFO(line);
            }
            else
            {
                LOG_DEBUG(line);
            }
        }

        // 분이 지난 진행 봉은 시계로 닫는다 — WS 틱은 on_tick이 이미 닫았고, REST 대체 틱 동안 남은 로컬
        //  진행 봉이 여기서 확정된다. 틱 수신 시각이 시계다(체결 시각은 hhmmss뿐이라 날짜가 없다). [why D-074]
        aggregator_.close_stale(symbol_id_, std::chrono::system_clock::to_time_t(trade.timestamp));

        // 판단 봉은 1분봉을 interval_min으로 접은 것이다 — 틱이 살아 있으면 집계기 스냅샷([0]=진행 중 분),
        //  REST 대체 틱이면 REST 1분봉. 두 길이 같은 resample을 지나므로 자리·계산이 같다. [why D-072]
        std::vector<MarketData> local = bars::resample(aggregator_.snapshot(symbol_id_, 0), parameters_.interval_min,
                                                       parameters_.simple_moving_average_period + 1);

        // 다음 REST 조회를 받을지 프리페치 스레드에 알린다. 워밍업(SMA 창 + 진행 봉)이 끝나고 틱이 살아 있으면
        //  REST는 쉰다.
        const bool want_seed = !websocket_live_ || reseed_pending_ ||
                               static_cast<int>(local.size()) < parameters_.simple_moving_average_period + 1;
        seed_wanted_.store(want_seed, std::memory_order_relaxed);

        if (local_bars)
        {
            bars = std::move(local); // 종가는 이미 방금 틱
        }
        else if (snapshot_bars)
        {
            bars =
                bars::resample(*snapshot_bars, parameters_.interval_min, parameters_.simple_moving_average_period + 1);
        }
    }
    else if (snapshot_bars)
    {
        // bar_source=rest — 아래 진행 봉 종가 덮어쓰기가 값을 고치므로 이 경로만 스냅샷을 복사한다.
        bars = *snapshot_bars;
    }

    // 진행 중인 봉(bars[0])의 종가를 방금 들어온 체결가로 덮는다. 프리페치가 봉 주기당
    //  한 번만 받으므로 그 사이의 가격 변화는 이 한 줄이 반영한다. 봉이 이미 넘어갔는데
    //  프리페치가 아직 안 왔으면(bucket 불일치) 덮지 않는다 — 마감된 봉의 종가를 고칠 순 없다.
    if (!local_bars && !bars.empty() && bars_bucket == kst_bar_bucket(parameters_.interval_min))
    {
        bars[0].close = current_price;
    }

    return decision_bars;
}

DeviationScaleStrategy::ZoneJudgement DeviationScaleStrategy::judge_zone(double current_price,
                                                                         std::chrono::steady_clock::time_point now)
{
    // ── 일봉 존 판정(정배열 + SMA20 눌림) ────────────────────────────────
    // 오늘 현재가를 이동평균에 접어 넣는다. 접지 않으면 정배열도 SMA20도 하루 종일
    //  전일 값이라, 장중에 이평이 깨져도 존은 활성으로 남고 이격만 움직인다.
    //  스캐너(UniverseScanner)와 같은 식·같은 허용오차를 쓴다(MaAlign.h).
    const quant::moving_average::SimpleMovingAverages daily_averages_previous =
        daily_simple_moving_averages_previous(*daily_);
    const quant::moving_average::SimpleMovingAverages daily_averages =
        daily_simple_moving_averages(*daily_, current_price);
    // 축이 둘이다. 접은 정배열은 진입만 연다. 유지·청산은 전일 확정 정배열로 판정한다.
    //  접은 값은 min_action_ms(3초)마다 뒤집힐 수 있는데 존 이탈에 붙은 행위가 보유 전량
    //  시장가 매도다. 09-10 일봉 캐시(정배열 통과 128종목)로 재면 average_5>s10이 73%에서 가장
    //  먼저 깨지고, 정배열이 무너지는 장중 하락폭 5분위가 1.45%다 — 유니버스의 약 10%가
    //  매일 "2~3% 밀리면 전량 매도, 되돌아오면 재매수"가 된다. 왕복마다 수수료·세금·
    //  슬리피지가 실현손실로 남고, 지수 게이트에서 방금 없앤 떨림을 종목 단위로 되살린다.
    //  청산은 되돌릴 수 없으니 느린 축에 맡긴다. [why D-033]
    const bool aligned =
        daily_averages.average_20 > 0.0 &&
        quant::moving_average::aligned(daily_averages, parameters_.align_moving_average_tolerance_percent);
    const bool aligned_hold =
        daily_averages_previous.average_20 > 0.0 &&
        quant::moving_average::aligned(daily_averages_previous, parameters_.align_moving_average_tolerance_percent);
    const double d_s20 = daily_averages.average_20;
    // 방향성 이격(부호 유지): +면 SMA20 위(확장추격), −면 아래(눌림). 절대값 금지.
    //  SMA20 미확보(≤0) 시 큰 양수 센티넬로 둬 존 상단 밖으로 밀어내 진입을 막는다.
    constexpr double kNoDataDeviationPct = 999.0;
    const double deviation20_percent = d_s20 > 0.0 ? (current_price - d_s20) / d_s20 * 100.0 : kNoDataDeviationPct;
    // 방향성 눌림 게이트: 진입은 "SMA20 이하(≤0%) ~ pullback_pct 아래"의 눌림 구간에서만.
    //   정배열 상승추세에서 SMA20 눌림(entry_upper_percent=0) 또는 SMA20 위 소폭(entry_upper_percent>0)까지 진입 허용.
    //   히스테리시스: 활성이면 상단 +zone_hysteresis 더 여유, 하단 −(pullback+zone_hysteresis)까지 유지(경계 진동
    //   방지).
    const double up_threshold =
        parameters_.entry_upper_percent +
        (in_zone_ ? parameters_.zone_hysteresis_percent : 0.0); // 진입 상단=entry_upper, 유지=+hysteresis
    const double down_threshold = in_zone_
                                      ? parameters_.pullback_percent + parameters_.zone_hysteresis_percent // 유지 하단
                                      : parameters_.pullback_percent;                                      // 진입 하단
    // 존 하단: SMA20 아래 -down_threshold(눌림).
    const double low_threshold = -down_threshold;
    const bool band = d_s20 > 0.0 && deviation20_percent <= up_threshold && deviation20_percent >= low_threshold;
    const bool zone = aligned && band; // 진입 게이트
    // [inv] hold_zone은 zone보다 넓어야 한다(정배열 축이 느린 쪽) — 좁아지면 막 산 걸 다음
    //  틱에 바로 되판다. aligned_hold(전일 확정)만 쓰면 당일 막 정배열이 시작된 종목은 전일
    //  축이 아직 못 따라와 hold_zone=N인데 zone=Y가 나온다(GS건설 09-15: 정배열=Y 101건
    //  전부 유지=N — 진입 자체가 막혔다). aligned_today를 OR로 더해 진입 순간엔 항상
    //  hold_zone⊇zone이 되게 한다 — research/studies/14_hold_axis V2로 짝비교(무해, 유의한
    //  손실 없음)까지 확인했다 [why D-088].
    const bool hold_zone = (aligned_hold || aligned) && band; // 유지 게이트 — 청산 판정
    in_zone_ = zone;

    // 존 판정 로그: 상태 변화 시 또는 60초마다 1회.
    if (zone != last_zone_ || zone_log_ts_.time_since_epoch().count() == 0 ||
        now - zone_log_ts_ >= std::chrono::seconds(60))
    {
        LOG_INFO("[" + id() + "] " + display() + " 존 판정 " + (zone ? "활성" : "대기") +
                 " | 정배열=" + (aligned ? "Y" : "N") + " 일봉SMA20=" + format_one_decimal(d_s20) +
                 " 현재가=" + format_one_decimal(current_price) + " 이격=" + format_one_decimal(deviation20_percent) +
                 "% (진입밴드 " + format_one_decimal(low_threshold) + "%~" + format_one_decimal(up_threshold) +
                 "%) 유지=" + (hold_zone ? "Y" : "N") + " 일봉수=" + std::to_string(daily_->size()));
        last_zone_ = zone;
        zone_log_ts_ = now;
    }

    ZoneJudgement zone_judgement;
    zone_judgement.previous_average_20 = daily_averages_previous.average_20;
    zone_judgement.average_20 = d_s20;
    zone_judgement.deviation20_percent = deviation20_percent;
    zone_judgement.zone = zone;
    zone_judgement.hold_zone = hold_zone;
    return zone_judgement;
}

bool DeviationScaleStrategy::exit_on_zone_loss(std::vector<OrderSignal>& out, std::chrono::steady_clock::time_point now)
{
    int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

    if (position > 0)
    {
        // 존 이탈 → 미체결 전부 취소 + 보유분 시장가 청산(매도가능분 클램프+백오프).
        cancel_all(out);
        emit_liquidation(out, position, now, "존 이탈"); // 클램프+백오프(자체 로깅)
        return true;
    }

    // 보유가 없으면 청산할 게 없다 — 미체결 매수만 거두고, 진입 여부는 아래 zone에 맡긴다.
    if (cancel_all(out))
    {
        LOG_INFO("[" + id() + "] 존 이탈 — 미체결 취소(보유 0)");
    }

    return false;
}

bool DeviationScaleStrategy::exit_on_protective_rules(double current_price, std::vector<OrderSignal>& out,
                                                      std::chrono::steady_clock::time_point now)
{
    // 표가 이 종목을 맡았으면(owner 모드) 손절·트레일 판정은 표가 한다 — 두 곳이 같은 판정을 내면
    //  중복 매도가 된다. 여기서는 표가 낸 청산만 받아 미체결 매수를 거두고 재진입 우도를 건다. [why D-114]
    const bool protective_owner = protective_owns(parameters_.account, symbol_id_);

    if (protective_owner && consume_protective_fire(parameters_.account, symbol_id_))
    {
        cancel_all(out);
        stop_cooldown_until_ = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
    }

    // ── 하드 스탑: 평단 대비 stop_loss_percent 아래면 존 상태와 무관하게 청산 ─────────
    //  유지 게이트가 진입보다 넓어(히스테리시스) 존 안에서도 평단에서 크게 밀릴 수 있다.
    //  평단은 sellable_quantity()가 잔고 조회 때 채운다. 재기동 직후 첫 재구성 전에는 0이라
    //  보유가 있으면 60초에 한 번 직접 채운다(REST 1회).
    if (!protective_owner && parameters_.stop_loss_percent > 0.0)
    {
        const int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

        // [inv] 평단 캐시는 보유 수량이 바뀐 뒤 쓰지 않는다. 전량 청산 뒤 재진입하면 원장 평단은 새 체결가로
        //  바뀌는데 캐시는 옛 평단이라, 새 체결 직후 스탑이 바로 걸려 15초 왕복 매매가 났다(09-14 067290:
        //  옛 평단 3515.9, 새 체결 3415). 보유 0이면 비우고, 수량이 바뀌었으면 원장(REST 없음)에서 다시 읽는다.
        if (position <= 0)
        {
            last_average_price_ = 0.0;
        }
        else if (position != average_position_seen_ && ledger_sellable(parameters_.account, parameters_.ticker))
        {
            (void)sellable_quantity();
        }

        average_position_seen_ = position;

        if (position > 0 && last_average_price_ <= 0.0 &&
            (average_query_ts_ == std::chrono::steady_clock::time_point{} ||
             now - average_query_ts_ >= std::chrono::seconds(60)))
        {
            average_query_ts_ = now;
            (void)sellable_quantity();
        }

        if (position > 0 && last_average_price_ > 0.0 &&
            current_price <= last_average_price_ * (1.0 - parameters_.stop_loss_percent / 100.0))
        {
            cancel_all(out);
            const std::string tag = "손절(평단 " + format_one_decimal(last_average_price_) + " -" +
                                    format_one_decimal(parameters_.stop_loss_percent) + "%)";
            // 스탑은 지수 백오프 상한을 30초로 둔다 — 5분 보류는 손절이 아니다.
            emit_liquidation(out, position, now, tag, kLiquidationBackoffMs);
            stop_cooldown_until_ = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
            return true;
        }
    }

    // ── 무장 후 고가 트레일: 평단 대비 +trail_arm_percent에 닿은 뒤 최고가 대비 −trail_percent면 청산 ──
    //  최고가는 보유 구간 동안 현재가로 갱신하고 보유가 0이면 비운다. 평단은 위 스탑 블록이 채운 캐시를 쓴다
    //  (스탑이 꺼져 있으면 여기서 채운다). 무장 여부는 따로 들고 있지 않다 — 최고가 ≥ 평단×(1+무장%)이면 무장이다.
    if (!protective_owner && parameters_.trail_arm_percent > 0.0)
    {
        const int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

        if (position <= 0)
        {
            hold_peak_price_ = 0.0;
        }
        else
        {
            if (last_average_price_ <= 0.0 && ledger_sellable(parameters_.account, parameters_.ticker))
            {
                (void)sellable_quantity();
            }

            hold_peak_price_ = (std::max)(hold_peak_price_, current_price);

            if (devscale_rules::peak_trail_triggered(hold_peak_price_, last_average_price_, current_price,
                                                     parameters_.trail_arm_percent, parameters_.trail_percent))
            {
                cancel_all(out);
                const std::string tag = "트레일(고가 " + format_one_decimal(hold_peak_price_) + " -" +
                                        format_one_decimal(parameters_.trail_percent) + "%, 평단 " +
                                        format_one_decimal(last_average_price_) + ")";
                emit_liquidation(out, position, now, tag, kLiquidationBackoffMs);
                stop_cooldown_until_ = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
                return true;
            }
        }
    }

    return false;
}

std::optional<DeviationScaleStrategy::Baseline>
DeviationScaleStrategy::resolve_baseline(const std::vector<MarketData>& bars, double d_s20, double current_price,
                                         std::vector<OrderSignal>& out, std::chrono::steady_clock::time_point now)
{
    // ── 3분봉 기준선(스냅샷에서 이미 받음) ───────────────────────────────
    const bool warming = static_cast<int>(bars.size()) < parameters_.simple_moving_average_period;

    if (warming && !parameters_.daily_basis_warmup)
    {
        // 개장 직후엔 3분봉이 simple_moving_average_period(20봉=60분)만큼 쌓이지 않아 여기서 매번 되돌아간다.
        //  로그가 없으면 '존 활성인데 주문 0건'이 원인 불명으로 보인다(2026-09-07 실제 발생).
        //  60초에 한 번만 남겨 개장 구간 로그가 넘치지 않게 한다.
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        if (now_ms - last_warm_log_ms_ >= kWarmLogIntervalMs)
        {
            last_warm_log_ms_ = now_ms;
            LOG_INFO("[" + id() + "] 봉 부족 — 대기 " + std::to_string(bars.size()) + "/" +
                     std::to_string(parameters_.simple_moving_average_period) + "봉(" +
                     std::to_string(parameters_.interval_min) + "분) 현재가=" + format_one_decimal(current_price));
        }

        return std::nullopt;
    }

    // 워밍업 구간에는 일봉 SMA20(존 게이트가 이미 쓴 값)을 기준선으로 대신 쓴다.
    const double simple_moving_average =
        warming ? d_s20 : simple_moving_average_close(bars, parameters_.simple_moving_average_period); // bars[0]=최신

    if (simple_moving_average <= 0.0)
    {
        return std::nullopt;
    }

    if (warming)
    {
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        if (now_ms - last_warm_log_ms_ >= kWarmLogIntervalMs)
        {
            last_warm_log_ms_ = now_ms;
            LOG_INFO("[" + id() + "] 일봉 기준선 대체 — 3분봉 " + std::to_string(bars.size()) + "/" +
                     std::to_string(parameters_.simple_moving_average_period) +
                     "봉, 일봉SMA20=" + format_one_decimal(d_s20) + " 현재가=" + format_one_decimal(current_price));
        }
    }

    return Baseline{simple_moving_average, warming};
}

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

    // ── 명목 사이징: 자본%를 가격으로 나눠 수량 산출(자본 미상이면 주수 폴백) ──
    //  베이스=자본×base_percent(5%), 물타기 총예산=자본×(max_percent−base_percent)(5%)를 split_step_count로 분할.
    //  베이스+물타기 합 ≈ 자본×max_percent(10%) → OrderGate 명목캡과 정합(캡은 백스톱).
    const double equity = equity_ > 0.0 ? equity_ : parameters_.fallback_equity;
    // 국면 매수비율(OrderGate::entry_scale, 0~1)을 명목에 곱한다. 스위치(halt)가 아니라 비율이라
    //  코스피 −1%면 70%, −2%면 40%처럼 줄어들고 반등하면 돌아온다. 0.1 단위로 끊어 3분마다
    //  분할 매수가 재구성되는 일을 막고, 시그니처에 붙여 바뀐 회차에만 다시 깐다. [why D-083]
    const double rscale = std::round(std::clamp(entry_scale(), 0.0, 1.0) * 10.0) / 10.0;
    const double multiplier = (parameters_.size_mult > 0.0 ? parameters_.size_mult : 1.0) * rscale;
    // 원 단위 총액이 주어지면 그 금액을 base_percent:max_percent 비율로 베이스·물타기에 나눈다.
    const double base_share =
        parameters_.max_percent > parameters_.base_percent ? parameters_.base_percent / parameters_.max_percent : 1.0;
    const double base_notional = parameters_.notional_krw > 0.0 ? parameters_.notional_krw * rscale * base_share
                                                                : equity * parameters_.base_percent * multiplier;
    const double split_step_budget = parameters_.notional_krw > 0.0
                                         ? parameters_.notional_krw * rscale - base_notional
                                         : equity *
                                               (parameters_.max_percent > parameters_.base_percent
                                                    ? parameters_.max_percent - parameters_.base_percent
                                                    : 0.0) *
                                               multiplier;
    const double split_step_notional =
        parameters_.buy_split_steps > 0 ? split_step_budget / parameters_.buy_split_steps : 0.0;

    // 분할 매수 기준점. 교차 가드가 켜져 있으면 각 방향 층이 현재가를 넘지 않도록 기준선을
    //  현재가 쪽으로 당긴다. 이격이 벌어진 상태에서도 분할 매수 간격은 그대로 유지된다.
    const bool guard_on = parameters_.cross_guard && current_price > 0.0;
    const double sell_base_line = guard_on ? (std::max)(simple_moving_average, current_price) : simple_moving_average;
    const double buy_base_line = guard_on ? (std::min)(simple_moving_average, current_price) : simple_moving_average;

    // 베이스: 무포지션이면 기준선 근처 지정가 매수(자본의 base_percent). 부분체결로 보유가 목표에 못 미치면
    //  잔량을 같은 자리에 다시 깐다 — 예전엔 1주만 체결돼도 position>0이라 베이스 분할 단계가 빠졌고, 재구성이 잔량
    //  주문을 취소해 익절 매도 1주만 남았다(09-14 375500 09:00:00 1/16 체결 → 09:00:10 잔량 취소, 096770
    //  13:10 같은 경로). 목표 수량은 처음 깔 때 값을 기억한다 — 값이 움직여 bq가 ±1 흔들리면 1주 매수가
    //  반복된다. 익절로 줄어든 보유는 채우지 않는다(peak_pos_가 목표에 닿았으면 베이스는 끝난 것). [why D-081]
    if (position <= 0)
    {
        // 보유가 있다가 0이 된 순간만 잡는다 — 처음부터 0인 하트비트마다 대기를 늘리면 진입이 영영 안 된다.
        if (peak_position_ > 0 && parameters_.reentry_cooldown_sec > 0)
        {
            reentry_cooldown_until_ = now + std::chrono::seconds(parameters_.reentry_cooldown_sec);
        }

        base_target_quantity_ = 0;
        peak_position_ = 0;
    }
    else
    {
        peak_position_ = (std::max)(peak_position_, position);
    }

    const bool base_short = position > 0 && base_target_quantity_ > 0 && peak_position_ < base_target_quantity_;

    if (position <= 0 || base_short)
    {
        double buy_price = round_to_tick(simple_moving_average, OrderSide::BUY);

        // 교차 가드: 기준선이 현재가 이상이면 이 지정가는 즉시 시장가로 체결된다.
        //  베이스를 건너뛰면 add_below_sma_only가 노리는 눌림 진입에서 가장 큰 레그가
        //  빠지므로, 억제 대신 현재가 한 틱 아래로 옮겨 지정가로 남긴다.
        if (parameters_.cross_guard && current_price > 0.0 && buy_price >= current_price)
        {
            buy_price = round_to_tick(current_price - krx::tick_size(current_price), OrderSide::BUY);
        }

        int base_quantity = base_short ? base_target_quantity_ : quantity_for(base_notional, buy_price);

        if (base_quantity <= 0)
        {
            base_quantity = parameters_.base_quantity; // 자본 미상 폴백
        }

        const int need = base_short ? base_quantity - position : base_quantity;

        if (buy_price > 0.0 && need > 0)
        {
            plan.push_back({OrderSide::BUY, buy_price, need});
            base_target_quantity_ = base_quantity;
        }
    }

    // 매도 밴드: 이격 +deviation_sell%*i. 보유분을 split_step_count로 균등 분할(숏 방지).
    //  sell_base_average면 기준점이 평단이다. 목표가가 현재가 아래면(이미 목표 초과) 현재가에
    //  낸다 — 지정가로 남되 다음 체결에 붙는다.
    int sell_avail = position;
    const int sell_per = parameters_.split_step_count > 0
                             ? (position + parameters_.split_step_count - 1) / parameters_.split_step_count
                             : position; // ceil
    const bool sell_from_average = parameters_.sell_base_average && last_average_price_ > 0.0;
    const double sell_base = sell_from_average ? last_average_price_ : sell_base_line;

    for (int split_step_index = 1; split_step_index <= parameters_.split_step_count && sell_avail > 0;
         ++split_step_index)
    {
        double sell_price =
            round_to_tick(sell_base * (1.0 + parameters_.deviation_sell * split_step_index / 100.0), OrderSide::SELL);

        if (sell_from_average && current_price > 0.0 && sell_price <= current_price)
        {
            sell_price = round_to_tick(current_price, OrderSide::SELL);
        }

        int quantity = sell_avail < sell_per ? sell_avail : sell_per;

        if (sell_price > 0.0 && quantity > 0)
        {
            plan.push_back({OrderSide::SELL, sell_price, quantity});
            sell_avail -= quantity;
        }
    }

    // 매수 밴드(물타기): 이격 −deviation_buy%*i, buy_split_steps층. 분할 단계당 자본의 split_step_notional. 종목당
    // 상한은 OrderGate가 캡.
    //  점진 진입: add_below_sma_only면 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 물타기를 깐다.
    //  → 활성 즉시 base+물타기를 한꺼번에 예약해 1분 만에 10% 만재되던 성격을 제거. 기준선 위/근처에선
    //    base(+익절 매도레그)만 유지하고, 진짜 눌림이 와야 평단을 낮춘다.
    // 워밍업(기준선=일봉SMA20) 구간에는 물타기를 잠근다. 존 진입 조건이 이격 -pullback_percent~
    //  +entry_upper_pct라 `cur_px < 일봉SMA20`이 거의 항상 참이 되어, 3분봉 기준선이 뜻하던
    //  "단기 눌림에서만 추가"가 사실상 상시 개방으로 바뀐다. 변동성이 가장 큰 첫 60분에
    //  base와 물타기가 한꺼번에 나가는 것을 막는다(base 진입과 익절 매도는 그대로 둔다).
    if ((!parameters_.add_below_simple_moving_average_only || current_price < simple_moving_average) && !warming)
    {
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

    split_plan.base_notional = base_notional;
    split_plan.split_step_budget = split_step_budget;
    split_plan.entry_scale_ratio = rscale;
    return split_plan;
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
    //  베이스 잔량이 채워지는 중이라 둔다. 재시도 간격은 emit_liquidation 백오프가 맡는다. [why D-081]
    if (position > 0 && parameters_.dust_krw > 0.0 && current_price > 0.0 &&
        position * current_price < parameters_.dust_krw)
    {
        bool has_buy = false;

        for (const auto& split_step : split_plan.steps)
        {
            has_buy = has_buy || (entry_on && split_step.side == OrderSide::BUY);
        }

        if (!has_buy)
        {
            cancel_all(out);
            emit_liquidation(out, position, now,
                             "먼지 정리(평가금 " + format_one_decimal(position * current_price) + " < " +
                                 format_one_decimal(parameters_.dust_krw) + ")");
            return true;
        }
    }

    return false;
}

bool DeviationScaleStrategy::rebuild_suppressed(const std::string& signal, int position, double split_buy_reference)
{
    const double reprice_band = parameters_.reprice_move_ticks * tick_size(split_buy_reference);
    const bool simple_moving_average_quiet =
        last_split_buy_reference_ > 0.0 && std::fabs(split_buy_reference - last_split_buy_reference_) < reprice_band;

    // (a) 계획 시그니처+pos가 직전과 동일하면 live 유무와 무관하게 스킵.
    //     매도가능=0이라 아무것도 못 깔아 live_가 빈 채로 남을 때(원장 보유↔매도가능 괴리)
    //     매 하트비트 재진입해 잔고조회를 난사하던 스핀을 차단. 체결로 pos가 바뀌면 즉시 재구성.
    // (b) 데드밴드(reprice 이내 미세이동)+position 동일 스킵은 살아있는 분할 매수에만 적용.
    if (signal == last_split_buy_signal_ && position == last_position_)
    {
        return true; // 동일 계획 → 유지(빈 계획 포함)
    }

    if (!live_.empty() && simple_moving_average_quiet && position == last_position_)
    {
        return true; // 데드밴드 내 미세이동 → 유지
    }

    // (c) 재구성 최소 간격. (a)(b)는 둘 다 position == last_pos_를 요구하므로, 부분체결이
    //     연달아 들어오는 종목은 체결마다 분할 매수를 통째로 헐고 다시 깐다. 분할 단계 2개면
    //     체결 1건에 취소 2 + 신규 2가 나가고, 이게 계좌 공용 주문예산(5/s·20/min)을
    //     한 종목이 독점한다(09-08 12:31~12:36 016610 단독 84건 = 전체의 8할).
    //     여기서 막아도 기존 분할 단계는 살아 있으므로 체결 기회를 잃지 않는다. 잠깐 크기가
    //     낡은 채로 유지될 뿐이다. 청산(emit_liquidation)은 이 경로를 타지 않는다.
    //     live_가 비어 있어도 적용한다 — 매도가능=0으로 아무것도 못 깔면 live_가 빈 채
    //     남는데, 그때 이 가드를 건너뛰면 하트비트(3초)마다 재구성·잔고조회가 돈다.
    if (parameters_.min_rebuild_sec > 0 && last_rebuild_ != std::chrono::steady_clock::time_point{})
    {
        const auto since =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_rebuild_).count();

        if (since < parameters_.min_rebuild_sec)
        {
            return true;
        }
    }

    return false;
}

std::string DeviationScaleStrategy::entry_context_text(const TradeData& trade, double current_price) const
{
    // 진입 문맥 스탬프(2026-09-11 회의 §3·§5): 체결강도(CTTR)·20일 평균 대비 누적거래량
    //  배율·직전 250봉 고가 대비 거리. 나중에 "저항 아래서 샀나"를 원장에서 바로 대조한다.
    //  REST 폴링 틱은 strength/accumulated_volume이 0이라 그때는 찍지 않는다.
    std::string entry_context;
    double volume20 = 0.0, hi250 = 0.0;
    const size_t value_count = (std::min)(daily_->size(), static_cast<size_t>(20));

    for (size_t index = 0; index < value_count; ++index)
    {
        volume20 += static_cast<double>((*daily_)[index].volume);
    }

    volume20 = value_count > 0 ? volume20 / static_cast<double>(value_count) : 0.0;

    for (const auto& daily_bar : *daily_)
    {
        hi250 = (std::max)(hi250, daily_bar.high);
    }

    if (trade.strength > 0.0)
    {
        entry_context += " 체결강도=" + format_one_decimal(trade.strength);
    }

    if (trade.accumulated_volume > 0 && volume20 > 0.0)
    {
        entry_context +=
            " 누적거래량/20일평균=" + format_one_decimal(static_cast<double>(trade.accumulated_volume) / volume20);
    }

    if (hi250 > 0.0)
    {
        entry_context += " 250봉고가대비=" + format_one_decimal((current_price - hi250) / hi250 * 100.0) + "%";
    }

    if (parameters_.stop_loss_percent > 0.0)
    {
        entry_context += " 손절=-" + format_one_decimal(parameters_.stop_loss_percent) + "%";
    }

    return entry_context;
}

void DeviationScaleStrategy::place_split_steps(const SplitPlan& split_plan, bool entry_on,
                                               const std::string& buy_context, std::vector<OrderSignal>& out)
{
    int sell_room = -1; // -1=미조회(지연). 첫 매도 분할 단계에서 1회 조회.

    for (const auto& split_step : split_plan.steps)
    {
        if (split_step.side == OrderSide::SELL)
        {
            if (sell_room < 0)
            {
                sell_room = sellable_quantity(); // 안전 우선: 불확실하면 0(매도 보류)
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
        else if (entry_on) // G1: 비활성 국면이면 매수 분할 단계 스킵(진입 차단)
        {
            place(out, split_step.side, split_step.price, split_step.quantity, buy_context);
        }
    }
}

bool DeviationScaleStrategy::day_entry_allowed(double current_price, double previous_sma20, int hhmm)
{
    const bool filter_off = parameters_.entry_atr_max_percent <= 0.0 &&
                            parameters_.entry_open_deviation_min_percent <= -99.0 &&
                            parameters_.entry_open_deviation_max_percent >= 99.0;

    if (filter_off)
    {
        return true;
    }

    const std::string today = kst_ymd();

    if (entry_filter_date_ != today)
    {
        if (hhmm < kOpenDeviationSampleHhmm || previous_sma20 <= 0.0)
        {
            return false;
        }

        const double atr_percent = devscale_rules::average_true_range(*daily_, kAtrPeriod) / previous_sma20 * 100.0;
        const double open_deviation_percent = (current_price - previous_sma20) / previous_sma20 * 100.0;
        entry_filter_date_ = today;
        day_entry_allowed_ = devscale_rules::entry_day_allowed(
            atr_percent, open_deviation_percent, parameters_.entry_atr_max_percent,
            parameters_.entry_open_deviation_min_percent, parameters_.entry_open_deviation_max_percent);
        LOG_INFO("[" + id() + "] 진입 필터 " + (day_entry_allowed_ ? "통과" : "차단") + "(ATR14 " +
                 format_one_decimal(atr_percent) + "% 개장 이격 " + format_one_decimal(open_deviation_percent) +
                 "% 기준 ATR<=" + format_one_decimal(parameters_.entry_atr_max_percent) +
                 " 이격>=" + format_one_decimal(parameters_.entry_open_deviation_min_percent) + ")");
    }

    return day_entry_allowed_;
}

double DeviationScaleStrategy::simple_moving_average_close(const std::vector<MarketData>& bars, int period)
{
    if (static_cast<int>(bars.size()) < period || period <= 0)
    {
        return 0.0;
    }

    double sum = 0.0;

    for (int period_index = 0; period_index < period; ++period_index)
    {
        sum += bars[period_index].close;
    }

    return sum / period;
}

quant::moving_average::SimpleMovingAverages DeviationScaleStrategy::daily_simple_moving_averages_previous(
    const std::vector<MarketData>& daily)
{
    quant::moving_average::SimpleMovingAverages previous;

    if (static_cast<int>(daily.size()) < 20)
    {
        return previous;
    }

    previous.average_5 = simple_moving_average_close(daily, 5);
    previous.average_10 = simple_moving_average_close(daily, 10);
    previous.average_20 = simple_moving_average_close(daily, 20);
    return previous;
}

quant::moving_average::SimpleMovingAverages DeviationScaleStrategy::daily_simple_moving_averages(
    const std::vector<MarketData>& daily, double current_price)
{
    const quant::moving_average::SimpleMovingAverages previous = daily_simple_moving_averages_previous(daily);

    if (previous.average_20 <= 0.0)
    {
        return previous;
    }

    return quant::moving_average::fold_today(previous, daily[4].close, daily[9].close, daily[19].close,
                                             current_price);
}

void DeviationScaleStrategy::prefetch_once()
{
    // 장 밖에서는 받아봐야 같은 응답이다. KIS 분봉은 기준시각을 15:30으로 클램프하므로
    //  (KisClient.cpp) 장 마감 후엔 종일 같은 봉을 다시 받고, 그 호출이 초당 한도를
    //  차지해 다른 조회를 500으로 밀어낸다. 발주는 어차피 장중에만 나가므로 건너뛴다.
    //  창은 08:50~15:35로 장 마감 청산(15:15)까지 덮는다.
    const int hhmm = kst_hhmm();
    const int wday = kst_tm().tm_wday;
    const bool in_session =
        (wday >= 1 && wday <= 5) && hhmm >= kPrefetchWindowOpenHhmm && hhmm <= kPrefetchWindowCloseHhmm;

    if (kis_ && in_session)
    {
        // 일봉·자본: 날짜 바뀌면 1회 갱신(장중엔 사실상 1일 1회).
        std::string today = kst_ymd();
        bool need_daily;
        {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            need_daily = !snap_daily_ || snap_daily_date_ != today;
        }

        if (need_daily)
        {
            auto daily_ohlcv = kis_->get_daily_ohlcv(parameters_.ticker, parameters_.daily_lookback);

            // 일봉이 비면(500·휴장) 스냅샷을 안 채우므로 need_daily가 참으로 남아
            //  다음 주기에 또 온다. 그때 잔고까지 같이 부르면 한도 초과 상황에서
            //  호출을 오히려 늘린다 — 일봉이 온 경우에만 잔고를 부른다.
            if (!daily_ohlcv.empty())
            {
                double equity = fetch_equity();
                std::lock_guard<std::mutex> lock(snap_mutex_);
                snap_daily_ = std::make_shared<const std::vector<MarketData>>(std::move(daily_ohlcv));
                snap_daily_date_ = std::move(today);
                snap_equity_ = equity;
            }
        }

        // 3분봉: 봉이 바뀔 때만 갱신한다. 이 조회는 페이지네이션이라 1회에 HTTP GET이
        //  세 번 나가는데(당일 63분치 1분봉을 다시 받아 집계), 그중 마감된 봉은 불변이고
        //  달라지는 건 진행 중인 봉 하나뿐이다. 그 하나는 아래 on_trade_batch가 들어오는
        //  체결 틱으로 덮으므로 SMA 값은 같게 유지되면서 조회는 봉 주기당 1회로 준다.
        //  bar_source=ws면 이 조회는 시드용이다 — 첫 스냅샷, 그리고 샤드 스레드가 원할 때(워밍업·
        //  REST 대체 틱·출처 전환 뒤)만 봉마다 한 번 받고, 틱이 살아 있고 봉이 찼으면 쉰다. 이때는
        //  1분봉 그대로 받는다(같은 63분치·같은 GET 수) — 접는 건 샤드 스레드의 resample이다. [why D-072]
        const int bucket = kst_bar_bucket(parameters_.interval_min);
        bool need_bars;
        {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            need_bars = !snap_bars_ || snap_bars_bucket_ != bucket;

            if (need_bars && websocket_bars_ && snap_bars_ && !seed_wanted_.load(std::memory_order_relaxed))
            {
                need_bars = false;
            }

            // 봉 경계 직후 종목별 지터만큼 미룬다(첫 스냅샷은 바로). 진행 중인 봉은
            //  on_trade_batch의 체결가 덮어쓰기가 채우므로 늦게 받아도 SMA는 같다.
            if (need_bars && snap_bars_ && kst_sec_into_bucket(parameters_.interval_min) < prefetch_jitter_sec_)
            {
                need_bars = false;
            }
        }

        if (need_bars)
        {
            auto bars = websocket_bars_
                            ? kis_->get_minute_ohlcv(
                                  parameters_.ticker,
                                  (parameters_.simple_moving_average_period + 1) * parameters_.interval_min, 1)
                            : kis_->get_minute_ohlcv(parameters_.ticker, parameters_.simple_moving_average_period + 1,
                                                     parameters_.interval_min);

            if (!bars.empty())
            {
                std::lock_guard<std::mutex> lock(snap_mutex_);
                snap_bars_ = std::make_shared<const std::vector<MarketData>>(std::move(bars));
                snap_bars_bucket_ = bucket;
                ++snap_bars_version_;
            }
        }
    }
}

void DeviationScaleStrategy::stop_prefetch()
{
    if (prefetch_pool_ && prefetch_task_ != 0)
    {
        prefetch_pool_->remove(prefetch_task_);
    }

    prefetch_task_ = 0;
}

double DeviationScaleStrategy::fetch_equity()
{
    constexpr std::chrono::seconds kRetryCooldown{60}; // 조회 실패 뒤 다시 부르지 않는 시간

    static std::mutex s_mutex;
    static std::string s_ymd;
    static double static_equity = 0.0;
    static std::chrono::steady_clock::time_point s_retry_after{}; // 실패 쿨다운이 끝나는 시각
    std::lock_guard<std::mutex> lock(s_mutex);
    std::string today = kst_ymd();

    if (s_ymd == today && static_equity > 0.0)
    {
        return static_equity;
    }

    if (std::chrono::steady_clock::now() < s_retry_after)
    {
        return 0.0; // 직전 조회가 실패했다 — 쿨다운 안에는 서버를 다시 때리지 않는다
    }

    double equity = 0.0;
    KisClient* kis_client = account_kis();

    if (kis_client && kis_client->has_account())
    {
        try
        {
            const KisResult<AccountBalance> balance = kis_client->get_balance();

            if (balance && balance->total_evaluation_amount)
            {
                equity = *balance->total_evaluation_amount;
            }
        }
        catch (...)
        {
        }
    }

    if (equity > 0.0)
    {
        s_ymd = std::move(today);
        static_equity = equity;
    }
    else
    {
        s_retry_after = std::chrono::steady_clock::now() + kRetryCooldown;
        LOG_WARN("[DEVSCALE] 기준자본(총평가금) 조회 실패 — " + std::to_string(kRetryCooldown.count()) +
                 "초 쿨다운, 그 안에는 폴백 자본으로 간다");
    }

    return equity;
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
                                   const std::string& reason)
{
    if (quantity <= 0)
    {
        return; // quantity=0 NEW 발주 억제 — 게이트 거부·로그 노이즈 원천 차단(SELL은 상위서도 클램프)
    }

    std::string order_id = next_order_id(side == OrderSide::BUY ? "B" : "S");
    const uint64_t order_number = next_client_order_number(); // 라우터가 취소 대상을 찾는 키. 문자열은 로그용
    OrderSignal signal;
    signal.ticker = parameters_.ticker;
    signal.symbol_id = symbol_id_;
    signal.side = side;
    signal.type = OrderType::LIMIT;
    signal.quantity = quantity;
    signal.price = price;
    signal.strategy_id = id();
    signal.market = Market::KR;
    signal.action = OrderAction::NEW;
    signal.client_order_id = order_id;
    signal.client_order_number = order_number;
    signal.account_id = parameters_.account;
    signal.reason = reason; // G4: 판단 근거를 신호에 실어 영속
    signal.timestamp = std::chrono::system_clock::now();
    out.push_back(std::move(signal));
    live_.push_back({std::move(order_id), order_number, side});
}

bool DeviationScaleStrategy::cancel_all(std::vector<OrderSignal>& out)
{
    if (live_.empty())
    {
        return false;
    }

    for (auto& live_entry : live_) // live_는 아래에서 비우므로 주문 id를 옮긴다
    {
        OrderSignal signal;
        signal.ticker = parameters_.ticker;
        signal.symbol_id = symbol_id_;
        signal.side = live_entry.side;
        signal.type = OrderType::LIMIT;
        signal.quantity = 0;
        signal.strategy_id = id();
        signal.market = Market::KR;
        signal.action = OrderAction::CANCEL;
        signal.original_client_order_id = std::move(live_entry.order_id);
        signal.original_client_order_number = live_entry.order_number;
        signal.account_id = parameters_.account;
        signal.timestamp = std::chrono::system_clock::now();
        out.push_back(std::move(signal));
    }

    live_.clear();
    return true;
}

OrderSignal DeviationScaleStrategy::make_market_sell(int quantity, const std::string& reason)
{
    OrderSignal signal;
    signal.ticker = parameters_.ticker;
    signal.symbol_id = symbol_id_;
    signal.side = OrderSide::SELL;
    signal.type = OrderType::MARKET;
    signal.quantity = quantity;
    signal.strategy_id = id();
    signal.market = Market::KR;
    signal.action = OrderAction::NEW;
    signal.account_id = parameters_.account;
    signal.reason = reason; // G4: 청산 사유(존 이탈/장 마감 등)를 신호에 실어 영속
    signal.reference_price =
        liquidation_reference_price(); // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
    signal.timestamp = std::chrono::system_clock::now();
    return signal;
}

double DeviationScaleStrategy::liquidation_reference_price()
{
    if (last_price_ > 0.0)
    {
        return last_price_;
    }

    if (last_average_price_ > 0.0)
    {
        return last_average_price_;
    }

    std::lock_guard<std::mutex> lock(snap_mutex_);
    return snap_bars_ && !snap_bars_->empty() ? (*snap_bars_)[0].close : 0.0;
}

bool DeviationScaleStrategy::emit_liquidation(std::vector<OrderSignal>& out, int position,
                                              std::chrono::steady_clock::time_point now, const std::string& tag,
                                              long long max_backoff_ms, bool clamp_sellable)
{
    if (position <= 0)
    {
        return false;
    }

    // 진행 판정: 직전 시도보다 pos가 줄었으면(부분체결) 백오프 리셋.
    if (liquidation_last_position_ < 0 || position < liquidation_last_position_)
    {
        liquidation_fail_streak_ = 0;
        liquidation_next_ = std::chrono::steady_clock::time_point{};
    }

    if (liquidation_next_.time_since_epoch().count() != 0 && now < liquidation_next_)
    {
        return false; // 백오프 창 내 — 재발주 스킵(스팸 차단)
    }

    const int sellable = clamp_sellable ? sellable_quantity() : position; // 안전 우선: 불확실하면 0(보류)
    const int quantity = sellable > 0 ? (position < sellable ? position : sellable) : 0;
    bool emitted = false;

    if (quantity > 0)
    {
        out.push_back(make_market_sell(quantity, "청산:" + tag));
        emitted = true;
        LOG_INFO("[" + id() + "] " + tag + " — 취소+청산 pos=" + std::to_string(position) +
                 " 매도가능=" + std::to_string(sellable) + " 발주=" + std::to_string(quantity));
    }
    else
    {
        LOG_WARN("[" + id() + "] " + tag + " 청산 보류 — 매도가능=0 (잠긴 " + std::to_string(position) +
                 "주, 예약취소/결제 대기) 백오프#" + std::to_string(liquidation_fail_streak_ + 1));
    }

    liquidation_last_position_ = position;
    ++liquidation_fail_streak_;
    int shift = liquidation_fail_streak_ - 1;

    if (shift > 4)
    {
        shift = 4;
    }

    long long milliseconds = 30000LL << shift; // 30/60/120/240/480…

    if (milliseconds > max_backoff_ms)
    {
        milliseconds = max_backoff_ms; // capture 기본 5분, 스탑 경로는 30초
    }

    liquidation_next_ = now + std::chrono::milliseconds(milliseconds);
    return emitted;
}

int DeviationScaleStrategy::sellable_quantity()
{
    // 원장 접근자가 주입돼 있으면 그것으로 끝낸다 — 샤드 스레드에서 REST를 부르지 않는다. [why D-055]
    if (const auto sellable_from_ledger = ledger_sellable(parameters_.account, parameters_.ticker))
    {
        if (sellable_from_ledger->average_price > 0.0)
        {
            last_average_price_ = sellable_from_ledger->average_price;
        }

        return sellable_from_ledger->sellable;
    }

    KisClient* kis_client = account_kis();

    if (!kis_client || !kis_client->has_account())
    {
        return 0;
    }

    try
    {
        // 접근자 미주입(단독 실행·테스트) 경로. 샤드 스레드에서 동기로 돌므로 재시도만 뗀다 —
        //  실패는 아래에서 0으로 떨어지고 다음 하트비트에 다시 온다.
        KisClient::FastFailScope ff;
        const KisResult<AccountBalance> balance = kis_client->get_balance();

        if (!balance)
        {
            return 0;
        }

        for (const Holding& holding : balance->holdings)
        {
            if (holding.ticker != parameters_.ticker)
            {
                continue;
            }

            last_average_price_ = holding.average_price;
            return holding.sellable_quantity.value_or(0);
        }

        return 0; // 잔고에 종목 없음 → 매도가능 0
    }
    catch (...)
    {
        return 0;
    }
}

int DeviationScaleStrategy::kst_hhmm()
{
    struct tm local_time = kst_tm();
    return local_time.tm_hour * 100 + local_time.tm_min;
}

int DeviationScaleStrategy::kst_bar_bucket(int interval_min)
{
    if (interval_min < 1)
    {
        interval_min = 1;
    }

    struct tm local_time = kst_tm();
    return (local_time.tm_yday * kMinutesPerDay + local_time.tm_hour * 60 + local_time.tm_min) / interval_min;
}

int DeviationScaleStrategy::kst_sec_into_bucket(int interval_min)
{
    if (interval_min < 1)
    {
        interval_min = 1;
    }

    struct tm local_time = kst_tm();
    return ((local_time.tm_hour * 60 + local_time.tm_min) % interval_min) * 60 + local_time.tm_sec;
}

std::string DeviationScaleStrategy::format_one_decimal(double value)
{
    char byte_value[32];
    std::snprintf(byte_value, sizeof(byte_value), "%.1f", value);
    return std::string(byte_value);
}

bars::BarAggregator::Config DeviationScaleStrategy::aggregator_config(const Params& parameters)
{
    bars::BarAggregator::Config config;
    config.interval_min = 1;
    config.keep =
        (std::max)(64, (parameters.simple_moving_average_period + 2) * (std::max)(1, parameters.interval_min));
    return config;
}
