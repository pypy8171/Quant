// strategy/DeviationScaleZone.cpp — 이격 분할 전략의 일봉 존 판정·분할 기준선·하루 진입 필터·이동평균 도우미.
//  관련 결정: D-005(오늘 현재가 접기)·D-033(유지 축)·D-088(hold_zone)·D-111(하루 진입 필터).
#include "strategy/DeviationScaleStrategy.h"

namespace
{
// 예열 안내 로그 사이 최소 간격 — 종목마다 나오므로 1분에 한 줄로 묶는다.
constexpr int64_t kWarmLogIntervalMs = 60000;
} // namespace

DeviationScaleStrategy::ZoneJudgement DeviationScaleStrategy::judge_zone(double current_price, bool holding,
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
    //   히스테리시스: 활성이거나 보유 중이면 양쪽에 zone_hysteresis를 더 둔다(경계 진동 방지·재기동 직후 오판 매도 방지).
    const devscale_rules::ZoneBand zone_band = devscale_rules::zone_band(
        parameters_.entry_upper_percent, parameters_.pullback_percent, parameters_.zone_hysteresis_percent,
        trading_.in_zone || holding);
    const double up_threshold = zone_band.up_percent;
    const double low_threshold = zone_band.low_percent;
    const bool band = d_s20 > 0.0 && deviation20_percent <= up_threshold && deviation20_percent >= low_threshold;
    const bool zone = aligned && band; // 진입 게이트
    // [inv] hold_zone은 zone보다 넓어야 한다(정배열 축이 느린 쪽) — 좁아지면 막 산 걸 다음
    //  틱에 바로 되판다. aligned_hold(전일 확정)만 쓰면 당일 막 정배열이 시작된 종목은 전일
    //  축이 아직 못 따라와 hold_zone=N인데 zone=Y가 나온다(GS건설 09-15: 정배열=Y 101건
    //  전부 유지=N — 진입 자체가 막혔다). aligned_today를 OR로 더해 진입 순간엔 항상
    //  hold_zone⊇zone이 되게 한다 — research/studies/14_hold_axis V2로 짝비교(무해, 유의한
    //  손실 없음)까지 확인했다 [why D-088].
    const bool hold_zone = (aligned_hold || aligned) && band; // 유지 게이트 — 청산 판정
    trading_.in_zone = zone;

    // 존 판정 로그: 상태 변화 시 또는 60초마다 1회.
    if (zone != trading_.last_zone || trading_.zone_log_ts.time_since_epoch().count() == 0 ||
        now - trading_.zone_log_ts >= std::chrono::seconds(60))
    {
        LOG_INFO("[" + id() + "] " + display() + " 존 판정 " + (zone ? "활성" : "대기") +
                 " | 정배열=" + (aligned ? "Y" : "N") + " 일봉SMA20=" + format_one_decimal(d_s20) +
                 " 현재가=" + format_one_decimal(current_price) + " 이격=" + format_one_decimal(deviation20_percent) +
                 "% (진입밴드 " + format_one_decimal(low_threshold) + "%~" + format_one_decimal(up_threshold) +
                 "%) 유지=" + (hold_zone ? "Y" : "N") + " 일봉수=" + std::to_string(daily_->size()));
        trading_.last_zone = zone;
        trading_.zone_log_ts = now;
    }

    ZoneJudgement zone_judgement;
    zone_judgement.previous_average_20 = daily_averages_previous.average_20;
    zone_judgement.average_20 = d_s20;
    zone_judgement.deviation20_percent = deviation20_percent;
    zone_judgement.zone = zone;
    zone_judgement.hold_zone = hold_zone;
    return zone_judgement;
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

        if (now_ms - trading_.last_warm_log_ms >= kWarmLogIntervalMs)
        {
            trading_.last_warm_log_ms = now_ms;
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

        if (now_ms - trading_.last_warm_log_ms >= kWarmLogIntervalMs)
        {
            trading_.last_warm_log_ms = now_ms;
            LOG_INFO("[" + id() + "] 일봉 기준선 대체 — 3분봉 " + std::to_string(bars.size()) + "/" +
                     std::to_string(parameters_.simple_moving_average_period) +
                     "봉, 일봉SMA20=" + format_one_decimal(d_s20) + " 현재가=" + format_one_decimal(current_price));
        }
    }

    return Baseline{simple_moving_average, warming};
}

std::string DeviationScaleStrategy::entry_context_text(const TradeData& trade, double current_price) const
{
    // 진입 문맥 스탬프(2026-09-11 회의 §3·§5): 체결강도(CTTR)·20일 평균 대비 누적거래량
    //  배율·직전 250봉 고가 대비 거리. 나중에 "저항 아래서 샀나"를 장부에서 바로 대조한다.
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

    if (trading_.entry_filter_date != today)
    {
        if (hhmm < kOpenDeviationSampleHhmm || previous_sma20 <= 0.0)
        {
            return false;
        }

        const double atr_percent = devscale_rules::average_true_range(*daily_, kAtrPeriod) / previous_sma20 * 100.0;
        const double open_deviation_percent = (current_price - previous_sma20) / previous_sma20 * 100.0;
        trading_.entry_filter_date = today;
        trading_.day_entry_allowed = devscale_rules::entry_day_allowed(
            atr_percent, open_deviation_percent, parameters_.entry_atr_max_percent,
            parameters_.entry_open_deviation_min_percent, parameters_.entry_open_deviation_max_percent);
        LOG_INFO("[" + id() + "] 진입 필터 " + (trading_.day_entry_allowed ? "통과" : "차단") + "(ATR14 " +
                 format_one_decimal(atr_percent) + "% 개장 이격 " + format_one_decimal(open_deviation_percent) +
                 "% 기준 ATR<=" + format_one_decimal(parameters_.entry_atr_max_percent) +
                 " 이격>=" + format_one_decimal(parameters_.entry_open_deviation_min_percent) + ")");
    }

    return trading_.day_entry_allowed;
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
