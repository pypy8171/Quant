// strategy/DeviationScaleExit.cpp — 이격 분할 전략의 청산: 장 마감·추세 이탈·존 이탈·손절·트레일과 매도가능 클램프·백오프.
//  관련 결정: D-046·D-055(매도가능 클램프)·D-082(장 마감 순서)·D-111(트레일)·D-114(보호 주문 표)·D-156·D-158(추세 이탈).
#include "strategy/DeviationScaleStrategy.h"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
// 장 마감 청산의 사유 문자열. 낼 때만 만든다.
std::string market_close_tag(int hhmm)
{
    return "장 마감(" + std::to_string(hhmm) + ")";
}
} // namespace

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
    //  마감 뒤에는 체결마다 여기를 지난다 — 사유 문자열은 실제로 낼 때와 취소를 로그로 남길 때만 만든다.
    const int  position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);
    const auto now      = std::chrono::steady_clock::now();

    if (liquidation_due(position, now))
    {
        emit_liquidation(out, position, now, market_close_tag(hhmm), kMarketCloseBackoffMs,
                         /*clamp_sellable=*/false); // 백오프(자체 로깅)
    }

    const bool cancelled = cancel_all(out);

    if (cancelled && position <= 0)
    {
        LOG_INFO("[" + id() + "] " + market_close_tag(hhmm) + " — 미체결 취소(보유 0)");
    }

    return true;
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

bool DeviationScaleStrategy::exit_on_trend_loss(int hhmm, std::vector<OrderSignal>& out,
                                                std::chrono::steady_clock::time_point now)
{
    const int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

    if (position <= 0)
    {
        trend_exit_.entry_date = 0; // 다음 보유는 새 매수일로 판정한다
        trend_exit_.sell_due = false;
        return false;
    }

    if (trend_exit_.entry_date == 0)
    {
        trend_exit_.entry_date = std::stoi(kst_date_yyyymmdd()); // 이 프로세스에서 산 보유 — 오늘 매수
    }

    const std::vector<MarketData>& daily = *daily_;
    const int session = daily.empty() ? 0 : daily[0].session_date;

    if (session != trend_exit_.judged_session || trend_exit_.entry_date != trend_exit_.judged_entry_date)
    {
        const devscale_rules::TrendExitJudgement judgement =
            devscale_rules::judge_trend_exit(daily, parameters_.trend_exit_period, trend_exit_.entry_date);
        trend_exit_.judged_session = session;
        trend_exit_.judged_entry_date = trend_exit_.entry_date;
        trend_exit_.sell_due = judgement.sell;
        LOG_INFO("[" + id() + "] " + display() + " 추세 판정 SMA" + std::to_string(parameters_.trend_exit_period) +
                 " 전일=" + std::to_string(session) + " 종가=" + format_one_decimal(judgement.close) +
                 " 평균=" + format_one_decimal(judgement.average) + " 매수일=" + std::to_string(trend_exit_.entry_date) +
                 " 위에선적=" + (judgement.armed ? "있음" : "없음") + (judgement.sell ? " → 매도" : " → 보유"));
    }

    constexpr int kRegularOpenHhmm = 900; // 장 시작 전(장전 시간외)에는 시장가가 거부된다

    if (!trend_exit_.sell_due || hhmm < kRegularOpenHhmm)
    {
        return false;
    }

    cancel_all(out);

    if (liquidation_due(position, now))
    {
        emit_liquidation(out, position, now, "추세 이탈(SMA" + std::to_string(parameters_.trend_exit_period) + " 종가)",
                         kLiquidationBackoffMs);
    }

    return true;
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
        liquidation_.stop_cooldown_until = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
    }

    // ── 하드 스탑: 평단 대비 stop_loss_percent 아래면 존 상태와 무관하게 청산 ─────────
    //  유지 게이트가 진입보다 넓어(히스테리시스) 존 안에서도 평단에서 크게 밀릴 수 있다.
    //  평단은 sellable_quantity()가 잔고 조회 때 채운다. 재기동 직후 첫 재구성 전에는 0이라
    //  보유가 있으면 60초에 한 번 직접 채운다(REST 1회).
    if (!protective_owner && parameters_.stop_loss_percent > 0.0)
    {
        const int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

        // [inv] 평단 캐시는 보유 수량이 바뀐 뒤 쓰지 않는다. 전량 청산 뒤 재진입하면 장부 평단은 새 체결가로
        //  바뀌는데 캐시는 옛 평단이라, 새 체결 직후 스탑이 바로 걸려 15초 왕복 매매가 났다(09-14 067290:
        //  옛 평단 3515.9, 새 체결 3415). 보유 0이면 비우고, 수량이 바뀌었으면 장부(REST 없음)에서 다시 읽는다.
        if (position <= 0)
        {
            liquidation_.last_average_price = 0.0;
        }
        else if (position != liquidation_.average_position_seen && ledger_sellable(parameters_.account, symbol_id_, parameters_.ticker))
        {
            (void)sellable_quantity();
        }

        liquidation_.average_position_seen = position;

        if (position > 0 && liquidation_.last_average_price <= 0.0 &&
            (liquidation_.average_query_ts == std::chrono::steady_clock::time_point{} ||
             now - liquidation_.average_query_ts >= std::chrono::seconds(60)))
        {
            liquidation_.average_query_ts = now;
            (void)sellable_quantity();
        }

        if (position > 0 && liquidation_.last_average_price > 0.0 &&
            current_price <= liquidation_.last_average_price * (1.0 - parameters_.stop_loss_percent / 100.0))
        {
            cancel_all(out);

            // 손절 조건이 참인 동안 틱마다 여기를 지난다 — 백오프 창 안이면 사유 문자열을 만들지 않는다.
            if (liquidation_due(position, now))
            {
                const std::string tag = "손절(평단 " + format_one_decimal(liquidation_.last_average_price) + " -" +
                                        format_one_decimal(parameters_.stop_loss_percent) + "%)";
                // 스탑은 지수 백오프 상한을 30초로 둔다 — 5분 보류는 손절이 아니다.
                emit_liquidation(out, position, now, tag, kLiquidationBackoffMs);
            }

            liquidation_.stop_cooldown_until = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
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
            liquidation_.hold_peak_price = 0.0;
        }
        else
        {
            if (liquidation_.last_average_price <= 0.0 && ledger_sellable(parameters_.account, symbol_id_, parameters_.ticker))
            {
                (void)sellable_quantity();
            }

            liquidation_.hold_peak_price = (std::max)(liquidation_.hold_peak_price, current_price);

            if (devscale_rules::peak_trail_triggered(liquidation_.hold_peak_price, liquidation_.last_average_price, current_price,
                                                     parameters_.trail_arm_percent, parameters_.trail_percent))
            {
                cancel_all(out);

                if (liquidation_due(position, now))
                {
                    const std::string tag = "트레일(고가 " + format_one_decimal(liquidation_.hold_peak_price) + " -" +
                                            format_one_decimal(parameters_.trail_percent) + "%, 평단 " +
                                            format_one_decimal(liquidation_.last_average_price) + ")";
                    emit_liquidation(out, position, now, tag, kLiquidationBackoffMs);
                }

                liquidation_.stop_cooldown_until = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
                return true;
            }
        }
    }

    return false;
}

OrderSignal DeviationScaleStrategy::make_market_sell(int quantity, std::string reason)
{
    OrderSignal signal = make_signal(parameters_.ticker, symbol_id_, OrderSide::SELL, OrderType::MARKET, quantity);
    signal.action = OrderAction::NEW;
    signal.account_id = parameters_.account;
    signal.reason = std::move(reason); // 청산 사유(존 이탈/장 마감 등)를 신호에 실어 영속
    signal.reference_price =
        liquidation_reference_price(); // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
    return signal;
}

double DeviationScaleStrategy::liquidation_reference_price()
{
    if (liquidation_.last_price > 0.0)
    {
        return liquidation_.last_price;
    }

    if (liquidation_.last_average_price > 0.0)
    {
        return liquidation_.last_average_price;
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_.bars && !snapshot_.bars->empty() ? (*snapshot_.bars)[0].close : 0.0;
}

bool DeviationScaleStrategy::liquidation_due(int position, std::chrono::steady_clock::time_point now) const
{
    if (position <= 0)
    {
        return false;
    }

    // emit_liquidation의 진행 판정과 같다 — 직전 시도보다 보유가 줄었으면 백오프가 풀린다.
    const bool progressed = liquidation_.last_position < 0 || position < liquidation_.last_position;
    const bool in_backoff = liquidation_.next.time_since_epoch().count() != 0 && now < liquidation_.next;
    return progressed || !in_backoff;
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
    if (liquidation_.last_position < 0 || position < liquidation_.last_position)
    {
        liquidation_.fail_streak = 0;
        liquidation_.next = std::chrono::steady_clock::time_point{};
    }

    if (liquidation_.next.time_since_epoch().count() != 0 && now < liquidation_.next)
    {
        return false; // 백오프 창 내 — 재발주 스킵(스팸 차단)
    }

    const int sellable = clamp_sellable ? sell_room(position) : position; // 안전 우선: 불확실하면 0(보류)
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
                 "주, 예약취소/결제 대기) 백오프#" + std::to_string(liquidation_.fail_streak + 1));
    }

    liquidation_.last_position = position;
    ++liquidation_.fail_streak;
    constexpr int kBackoffShiftMax = 4; // 30초 × 2^4 = 480초까지만 늘린다
    int shift = liquidation_.fail_streak - 1;

    if (shift > kBackoffShiftMax)
    {
        shift = kBackoffShiftMax;
    }

    long long milliseconds = static_cast<long long>(kLiquidationBackoffMs) << shift; // 30/60/120/240/480초

    if (milliseconds > max_backoff_ms)
    {
        milliseconds = max_backoff_ms; // 장 마감 경로 기본 5분, 스탑 경로는 30초
    }

    liquidation_.next = now + std::chrono::milliseconds(milliseconds);
    return emitted;
}

int DeviationScaleStrategy::sell_room(int position)
{
    // 취소를 먼저 고르고 다시 내는 방식(취소 확인 뒤 재구성)은 쓰지 않는다 — 재구성 한 번이 두 처리로 갈라져 그 사이
    //  매수 분할 단계도 비고, 취소 답을 못 받으면 다음 재구성까지 빈 채로 남는다. 여기서는 취소한 수량을 그대로
    //  다시 쓴다. 같은 종목의 새 주문은 앞 취소 답 뒤에 판정되고, 넘친 몫은 게이트 클램프와 라우터
    //  자가정리(reconcile_blocked_sell)가 깎는다. [why D-156]
    return devscale_rules::sell_room_after_cancel(sellable_quantity(), cancelled_sell_quantity_, position);
}

int DeviationScaleStrategy::sellable_quantity()
{
    // 장부 접근자가 주입돼 있으면 그것으로 끝낸다 — 샤드 스레드에서 REST를 부르지 않는다. [why D-055]
    if (const auto sellable_from_ledger = ledger_sellable(parameters_.account, symbol_id_, parameters_.ticker))
    {
        if (sellable_from_ledger->average_price > 0.0)
        {
            liquidation_.last_average_price = sellable_from_ledger->average_price;
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
        KisClient::FastFailScope fast_fail;
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

            liquidation_.last_average_price = holding.average_price;
            return holding.sellable_quantity.value_or(0);
        }

        return 0; // 잔고에 종목 없음 → 매도가능 0
    }
    catch (...)
    {
        return 0;
    }
}
