#pragma once
#include "api/KisClient.h"
#include "core/BarAggregator.h"
#include "core/DataPoller.h"
#include "core/KstTime.h"
#include "core/PrefetchPool.h"
#include "core/TickSize.h"
#include "core/WakeGate.h"
#include "strategy/DevScaleRules.h"
#include "strategy/StrategyBase.h"
#include "universe/MaAlign.h"
#include "utils/Logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// DeviationScaleStrategy — 일봉 존(정배열+눌림) 게이트 + 3분봉 이격도 분할매매(지정가 예약)
//
//  아이디어:
//   • "매매할 자리"는 일봉에서 정한다: 정배열(SMA5>10>20>60) AND 현재가가 일봉 SMA20
//     대비 이격 밴드 안. 밴드는 슬리브가 정한다 — 눌림(DEVSCALE)은 −pullback_percent~+entry_upper_percent,
//     추세확장(TRENDX)은 +entry_lower_percent~+entry_upper_percent(SMA20 위 구간). 존 유지는 진입보다
//     zone_hyst_pct만큼 넓다. 이 조건이 참일 때만 오실레이션을 켠다(존 활성).
//   • 자리 안에서는 3분봉 SMA를 기준선으로 삼아:
//       - 존 진입 시 무포지션이면 목표수량 절반(base_quantity)을 기준선 근처 지정가로 베이스 매수.
//       - 이격도가 위로 벌어지는 지점(+deviation_sell%·split_step_count층)에 지정가 매도(분할 익절).
//       - 평균으로 되돌아오는 지점(−deviation_buy%·buy_split_steps층)에 지정가 매수(재진입). buy_split_steps=0이면
//         되돌림 매수(물타기)를 깔지 않는다 — 추세확장 슬리브의 기본이다.
//   • 청산: 존 이탈(유지 게이트) · 평단 대비 stop_loss_percent 하드 스탑 · 평단 +trail_arm_percent 무장 뒤
//     최고가 대비 −trail_percent 트레일(옵션) · 3분봉 기준선 이탈 트레일(옵션) · market_close_hhmm 장 마감(2400이면 안 팔고
//     다음 날로 넘긴다). 스탑·트레일 뒤에는 stop_cooldown_sec 동안 재진입을 막는다.
//   • 하루 단위 진입 필터(옵션): 전일 ATR14/SMA20이 entry_atr_max_percent를 넘거나 개장 봉 이격이
//     entry_open_deviation_[min|max]_percent 밖이면 그날은 새로 사지 않는다(보유분 관리는 그대로).
//   • 시장가가 아니라 지정가 예약을 미리 걸어 "기다리는" 매매. 3분봉이 갱신되거나 SMA가
//     reprice_move_ticks 이상 이동하면 미체결 분할 매수를 CANCEL+NEW로 재호가(MM-1 패턴).
//
//  구동: rest_price_feed 모드에서 DataThread가 매 사이클 현재가를 TradeData로 주입 →
//        on_trade_batch가 하트비트로 호출된다(WS 불필요). 일봉은 kis_로 자가조회(프리페치 스레드).
//        3분봉은 bar_source로 고른다 — "rest"는 프리페치 스레드가 봉마다 REST를 다시 받고, "ws"(기본)는
//        전략 스레드가 체결 틱을 BarAggregator로 1분봉에 모으고 resample로 interval_min 봉을 만든다
//        (REST 1분봉은 시드·폴백, D-068·D-069·D-072). 판단은 언제나 interval_min 봉으로 한다 — 1분봉은 기저일 뿐이다.
//
//  포지션 진실원천: OrderGate 확정 포지션(confirmed_position). 체결콜백 부재(rest)에도
//        잔고 대조로 원장이 최신이라 신뢰 가능 → 별도 on_fill 불필요.
//
//  첫 컷 한계:
//   • 재호가는 CANCEL+NEW 전량(REPLACE 미사용). 자기 예약 live 여부는 낙관 가정,
//     체결된 예약의 후속 취소는 KIS 에러로 자가치유(OrderRouter).
//   • 존 이탈 시 청산은 시장가(지정가 청산은 후속).
//   • 지정가는 OrderGate가 틱격자 미검증 → 전략에서 round_to_tick 필수(MM 패턴 차용).
// ─────────────────────────────────────────────────────────────────────────────
class DeviationScaleStrategy : public StrategyBase
{
public:
    struct Params
    {
        std::string ticker;
        std::string name;             // 종목명(로그 표시용). 비면 티커만. id()/데이터키는 티커 그대로 유지.
        // ── 명목 사이징(자본%) — 우선. 자본 스냅샷(총평가금)×percent 를 가격으로 나눠 수량 산출.
        //    베이스=자본×base_percent, 물타기 총예산=자본×(max_percent−base_percent)를 split_step_count로 분할.
        //    자본을 알 수 없고(조회 실패) fallback_equity 도 0이면 아래 base_quantity/step_quantity 로 폴백.
        double base_percent       = 0.05;  // 존 진입 베이스 명목 = 자본의 5%
        double max_percent        = 0.10;  // 종목당 상한 명목 = 자본의 10%(물타기 포함)
        // 종목별 비중 배수 — 유니버스 스캐너의 종합 점수(추세·눌림·변동성)에서 나온다.
        //  베이스와 물타기 예산에 함께 곱해 종목당 명목 전체를 스케일한다. 1.0이면 균등(기본값)
        //  이라 배선 전 동작이 바뀌지 않는다. 등록 시점에 고정하고 재스캔이 갱신하지 않는다 —
        //  이미 포지션이 있는 종목의 분할 매수를 도중에 재산정하면 잔여 물타기 예산이 평단과 어긋난다.
        double size_mult      = 1.0;
        // 종목당 명목 총액(원). 0보다 크면 자본%·size_mult 대신 이 금액을 베이스+물타기로 나눈다
        //  (비율은 base_percent:max_percent 그대로). 점수 z를 바닥~천장 원 구간에 대응시킨 값이 들어온다.
        //  [why D-036] 자본%×정규화 배수는 종목당 20만~60만원이 나와 자본이 아니라 슬롯이 먼저 마른다.
        double notional_krw   = 0.0;
        double fallback_equity = 0.0;  // 잔고조회 실패 시 사용할 기준자본(원). 0이면 주수 폴백
        int    base_quantity       = 10;    // (폴백) 존 진입 베이스 매수 수량
        int    step_quantity       = 5;     // (폴백) 각 밴드(분할 단계) 분할 수량
        int    simple_moving_average_period     = 20;    // 3분봉 기준선 SMA 기간
        double deviation_sell       = 1.5;   // 매도 밴드 이격도(%) — 층당 배수
        double deviation_buy        = 0.8;   // 매수 밴드 이격도(%) — 층당 배수
        // 교차 가드: 분할 매수 각 층이 현재가를 넘어가지 않도록 기준점을 현재가 쪽으로 클램프한다.
        //  기준선이 현재가에서 멀어지면 한쪽 층 전체가 현재가를 넘어가 지정가가 아니라 즉시
        //  체결되는 시장가가 된다(매수는 위, 매도는 아래). 분할 매수의 전제가 깨진다.
        //  해당 층을 '건너뛰지' 않는다 — 건너뛰면 눌림 진입이나 익절이 통째로 사라진다.
        //  매도는 max(simple_moving_average,현재가), 매수는 min(simple_moving_average,현재가) 기준으로 층을 다시 깐다.
        bool   cross_guard    = true;  // docs/DECISIONS.market_data D-006
        int    split_step_count        = 2;     // 밴드 층수
        bool   add_below_simple_moving_average_only = true; // 물타기(매수 밴드)를 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 깐다.
                                          //  true=점진 진입: 활성 시 base만 → 진짜 눌림에서만 평단 낮춤(즉시 10% 만재 방지).
                                          //  false=예전 성격: 활성 즉시 base+물타기 전부 예약(빠른 풀사이즈).
        double pullback_percent   = 2.0;   // 일봉 SMA20 눌림 허용폭(하단,%) — 존 진입 임계(SMA20 아래)
        double entry_upper_percent = 0.0;  // 진입 상단(SMA20 위 허용%). 0=SMA20 이하만(순수 눌림). >0이면 SMA20 위 그만큼까지 진입 허용(완만상승·소폭눌림 포착)
        double zone_hysteresis_percent  = 4.0;   // 존 히스테리시스 밴드(%) — 청산 임계 = pullback + 이 값
        // 정배열 마지막 조건(SMA20>SMA60)의 허용오차. 스캐너 config.align_ma_tol_pct와 같은 값을
        //  받아야 "등록은 됐는데 활성은 안 되는" 슬롯이 생기지 않는다.
        double align_moving_average_tolerance_percent = 0.0;
        // 개장 직후 3분봉이 sma_period만큼 안 쌓인 구간(20봉×3분=60분)에서 분할 매수 기준선을
        //  일봉 SMA20으로 대신한다. 그 구간에도 일봉 존 게이트(정배열+눌림)는 이미 통과한 상태라
        //  판단 근거가 없는 게 아니라 기준선 하나가 없을 뿐이다. 기준선이 현재가에서 멀어도
        //  cross_guard가 기준점을 현재가로 클램프하고, 워밍업 중에는 add_below_simple_moving_average_only 값과
        //  무관하게 물타기 분할 매수를 잠근다(분할 주문 구성부 `!warming`). 베이스 매수·익절 매도는 나간다.
        bool   daily_basis_warmup = true;
        int    reprice_move_ticks = 2; // SMA가 이만큼(틱) 이동하면 재호가
        int    min_rebuild_sec = 0;    // 분할 매수 전면 재구성 최소 간격(0=제한 없음). 첫 구성 뒤부터 적용
        // ── TRENDX(추세확장) 슬리브용. 전부 기본값이 기존 눌림 동작이다. ──
        //  id_prefix: 같은 클래스를 두 슬리브로 돌릴 때 전략 id를 가른다(regime_strategies
        //   매칭 키이자 로그 식별자). 유니버스가 이격 밴드로 상호배타라 티커는 겹치지 않는다.
        std::string id_prefix = "DEVSCALE";
        //  entry_lower_percent: >0이면 존 하단을 SMA20 "위" 그 지점으로 올린다. 눌림 슬리브가
        //   버리는 이격 +5% 초과 구간을 이 슬리브가 받는다(0=기존 -pullback_percent 하단).
        double entry_lower_percent = 0.0;
        //  base_on_price: 분할 매수 기준점을 SMA20이 아니라 현재가로 잡는다. 이격이 벌어진
        //   종목은 SMA20 기준점으로 깔면 매수층이 시장가에서 5~30% 아래에 놓여 영원히 안 붙는다.
        bool   base_on_price = false;
        //  buy_split_steps: 되돌림 매수(물타기) 층수. -1이면 split_step_count와 같다(기존 동작), 0이면 베이스
        //   매수만 내고 하방 분할 매수를 깔지 않는다. 방향성 이격 게이트에서 하방 분할 매수는 추세
        //   반전에 그대로 노출된다 — 09-08~11 원장에서 매수 수량의 89%가 미청산으로 남았다.
        int    buy_split_steps = -1;
        //  stop_loss_percent: >0이면 잔고 평단 대비 이만큼(%) 아래에서 미체결 취소+시장가 청산.
        //   [inv] 기준은 진입봉 저가가 아니라 평단이다 — 재기동해도 잔고 조회로 되살아나는 값이다.
        double stop_loss_percent = 0.0;
        //  trail_simple_moving_average_exit: 보유 중 현재가가 3분봉 기준선의 trail_simple_moving_average_tolerance_percent(%) 아래로 내려오면
        //   청산한다. 워밍업(기준선=일봉 SMA20) 구간에는 보지 않는다.
        bool   trail_simple_moving_average_exit = false;
        double trail_simple_moving_average_tolerance_percent = 1.0;
        //  stop_cooldown_sec: 스탑·트레일 청산 뒤 이 시간 동안 분할 매수를 깔지 않는다. 존이
        //   그대로 열려 있으면 다음 하트비트에 베이스 매수가 도로 나가 같은 자리를 되산다.
        int    stop_cooldown_sec = 900;
        //  reentry_cooldown_sec: 전량 청산(익절·교체·장마감 포함) 뒤 이 시간 동안 새 베이스를 깔지 않는다.
        //   익절 지정가가 다 나간 3초 뒤 같은 값에 베이스를 되사는 일이 있었다(09-18 010170 17,720 매도 →
        //   17,710 매수, 375500 87,600 → 87,700). 왕복 비용만 내고 자리는 그대로다. 0이면 끄기.
        int    reentry_cooldown_sec = 600;
        //  dust_krw: 보유 평가금이 이 값(원) 아래이고 깔 매수 분할 단계가 없으면 시장가로 정리한다. 1~5주짜리
        //   잔존 보유가 슬롯 하나를 종일 차지했다(09-14 15:00 스윕 대상 3종목). 0이면 끄기. [why D-081]
        double dust_krw = 250000.0;
        //  sell_base_average: 분할 익절 기준점을 현재가가 아니라 잔고 평단으로 둔다. 현재가 기준점은
        //   목표가가 값을 따라 올라가 8초 안의 급등에서만 붙는다. 평단 기준이면 +deviation_sell%가
        //   진입 대비 익절이 된다. 목표가가 이미 현재가 아래면 현재가에 지정가를 낸다.
        bool   sell_base_average = false;
        //  trail_arm_percent / trail_percent: 보유 중 현재가가 평단 대비 +trail_arm_percent(%)에 한 번 닿으면(무장)
        //   그 뒤 보유 구간 최고가 대비 −trail_percent(%)에서 전량 청산한다. 고정 익절은 오른 만큼을 못 먹고,
        //   무장 없는 트레일은 진입 직후 흔들림에 팔린다. 0이면 끄기. 1년 리플레이(09-21, 3,135 종목일)에서
        //   무장 1.0/트레일 1.0 + 익절 3.0이 라이브 설정보다 손실 22% 적었다. [why D-111]
        double trail_arm_percent = 0.0;
        double trail_percent = 1.0;
        // 하루 단위 진입 필터 — 전일 ATR14/SMA20(%) 상한과 개장 봉(09:03) 종가의 SMA20 이격(%) 범위. 하루에 한 번
        //   판정해 그날 새 진입만 막는다. 1년 리플레이(09-21)에서 ATR≤5·이격≥−3이 비용 전 +0.10 → +0.26%/건,
        //   ATR 상한이 낮을수록 단조로 좋아졌다. 0/−99/99면 끔. [why D-111]
        double entry_atr_max_percent = 0.0;
        double entry_open_deviation_min_percent = -99.0;
        double entry_open_deviation_max_percent = 99.0;
        //  prefetch_jitter_percent: 봉 경계 직후 분봉 조회를 종목별로 흩는다(봉 길이의 0~이 비율,
        //   티커 해시로 고정). 50종목이 같은 초에 조회하면 초당 한도(20)에 걸려 뒤쪽이 HTTP 500이다.
        int    prefetch_jitter_percent = 50;
        //  bar_source: interval_min 봉을 어디서 받나. "rest"는 프리페치 스레드가 봉마다 REST 분봉을 다시 받고,
        //   "ws"(기본)는 전략 스레드가 체결 틱을 1분봉으로 모아(BarAggregator) resample로 접는다. REST 1분봉은
        //   시드와 폴백으로 남는다 — 틱이 REST 대체 모양이면(WS 폴백·구독 상한 넘침) 그 종목은 REST 봉으로
        //   되돌아간다. [why D-069] [why D-072]
        std::string bar_source = "ws";
        int    market_close_hhmm       = 1515;  // 이 시각(KST HHMM) 이후 전량 취소+청산
        int    interval_min   = 3;     // 집계봉 간격(분)
        int    min_action_ms  = 3000;  // on_trade_batch 판단·발주 스로틀 겸 프리페치 루프 주기(분봉 REST는 프리페치 스레드가 당긴다)
        int    daily_lookback = 70;    // 일봉 조회 개수(SMA60 판정 위해 ≥60)
        std::string account;           // 원장 계좌키(단일계좌는 "")
    };

    explicit DeviationScaleStrategy(Params parameters) : parameters_(std::move(parameters)), aggregator_(aggregator_config(parameters_))
    {
        id_ = parameters_.id_prefix + "_" + parameters_.ticker;
        websocket_bars_ = (parameters_.bar_source == "ws");

        // 닫힌 1분봉 한 줄 — 분마다 종목마다 나오므로 DEBUG. 비교표(compare_ws_bars.py)가 이 줄을 REST 1분봉과
        //  같은 규칙으로 접어 interval_min 봉끼리 견준다.
        aggregator_.set_sink([this](const MarketData& market_data) {
            LOG_DEBUG("[" + id() + "] 봉 닫힘 src=ws t=" +
                      kst::hhmmss(std::chrono::system_clock::to_time_t(market_data.timestamp)).substr(0, 4) +
                      " o=" + format_one_decimal(market_data.open) + " h=" + format_one_decimal(market_data.high) + " l=" + format_one_decimal(market_data.low) +
                      " c=" + format_one_decimal(market_data.close) + " v=" + std::to_string(market_data.volume));
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
        const int span    = (std::max)(1, parameters_.interval_min * 60 * jitter_percent / 100);
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

    // 프리페치 등록·스냅샷 뮤텍스를 안고 있다 — 복사 대상이 아니다.
    DeviationScaleStrategy(const DeviationScaleStrategy&)            = delete;
    DeviationScaleStrategy& operator=(const DeviationScaleStrategy&) = delete;

    ~DeviationScaleStrategy() { stop_prefetch(); }

    const std::string& id() const override { return id_; }

    // 로그 표시용 "티커(종목명)". 이름 없으면 티커만. id()·데이터키와는 분리.
    std::string display() const
    {
        return parameters_.name.empty() ? parameters_.ticker : parameters_.ticker + "(" + parameters_.name + ")";
    }

    std::string describe() const override
    {
        // 사이징은 자본×base_percent(또는 notional_krw) 우선, base_quantity/step_quantity는 폴백이다.
        return "DeviationScale | " + display() + " | base_pct=" + format_one_decimal(parameters_.base_percent * 100.0) +
               "% max_pct=" + format_one_decimal(parameters_.max_percent * 100.0) + "% (fallback qty " + std::to_string(parameters_.base_quantity) +
               "/" + std::to_string(parameters_.step_quantity) + ") sma=" + std::to_string(parameters_.simple_moving_average_period) +
               "(" + std::to_string(parameters_.interval_min) + "m src=" + (websocket_bars_ ? "ws" : "rest") +
               ") dev_sell=" + format_one_decimal(parameters_.deviation_sell) +
               "% dev_buy=" + format_one_decimal(parameters_.deviation_buy) + "% split_steps=" + std::to_string(parameters_.split_step_count) +
               "/buy" + std::to_string(parameters_.buy_split_steps) + " zone=" + format_one_decimal(-parameters_.pullback_percent) +
               "~+" + format_one_decimal(parameters_.entry_upper_percent) + "%" +
               (parameters_.entry_lower_percent > 0.0 ? " lower=+" + format_one_decimal(parameters_.entry_lower_percent) + "%" : "") +
               (parameters_.stop_loss_percent > 0.0 ? " stop=-" + format_one_decimal(parameters_.stop_loss_percent) + "%" : "") +
               (parameters_.trail_simple_moving_average_exit ? " trail" : "") + (parameters_.sell_base_average ? " sell@avg" : "") +
               (parameters_.trail_arm_percent > 0.0 ? " peak-trail" : "") +
               (parameters_.entry_atr_max_percent > 0.0 ? " atr<=" + format_one_decimal(parameters_.entry_atr_max_percent) + "%" : "") +
               (parameters_.entry_open_deviation_min_percent > -99.0
                    ? " open_dev>=" + format_one_decimal(parameters_.entry_open_deviation_min_percent) + "%" : "") +
               (parameters_.market_close_hhmm >= devscale_rules::kNoMarketCloseHhmm ? " carry" : "");
    }

    // 현재가 하트비트만 필요 → trade_only=true(호가 구독 절약). rest 모드에선 DataThread가 주입.
    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{parameters_.ticker, Market::KR, "", /*trade_only=*/true}};
    }

    // 일봉 이벤트 미사용(자가조회) — 순수가상 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    void on_start() override
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
        websocket_live_        = false;
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

    void on_stop() override
    {
        stop_prefetch();

        // 보호 주문 표에서 내린다 — 떨어진 전략의 규칙이 남아 다른 전략의 보유분을 팔면 안 된다. [why D-114]
        disarm_protective(parameters_.account, symbol_id_);

        // 마지막 진행 봉(마감 동시호가 뒤엔 다음 틱이 없다)을 시계로 닫아 비교표에 남긴다. 전략 스레드는 이미
        //  이 전략을 안 부른다(엔진 종료 뒤이거나 재스캔이 뗀 뒤). [why D-074]
        if (websocket_bars_)
        {
            aggregator_.close_stale(symbol_id_, std::time(nullptr));
        }

        LOG_INFO("[" + id() + "] 종료");
    }

    void on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out) override
    {
        if (!same_symbol(symbol_id_, parameters_.ticker, trade.symbol_id, trade.ticker))
        {
            return;
        }

        if (trade.price > 0.0)
        {
            last_price_ = trade.price; // 시장가 청산의 명목 평가 기준가(reference_price). 장 마감 경로보다 먼저 갱신
        }

        // ── 봉 집계: 스로틀 앞이다 — 모든 틱을 먹어야 고가·저가·거래량이 맞다 ──────────
        //  REST 대체 틱(폴백·구독 상한 넘침)은 넣지 않는다. 체결량이 없어 봉이 비고, 주기가 초 단위라
        //  고저가 빠진다. 그동안은 REST 봉을 쓰고, WS 틱이 돌아오면 REST로 다시 시드해 빈 자리를 메운다.
        if (websocket_bars_)
        {
            const bool live = !poller::is_rest_tick(trade);

            if (live != websocket_live_)
            {
                websocket_live_        = live;
                reseed_pending_ = true;
                LOG_INFO("[" + id() + "] 봉 출처 전환 src=" + (live ? "ws" : "rest") +
                         (live ? " — 체결 틱을 봉으로 모은다, REST 봉은 시드" : " — REST 대체 틱, REST 봉으로 판단"));
            }

            if (live)
            {
                aggregator_.on_tick(trade);
            }
        }

        const int hhmm = kst_hhmm();

        // ── 장 마감 안전장치: 전량 취소 + 시장가 청산 ────────────────────────────
        if (hhmm >= parameters_.market_close_hhmm)
        {
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

        // ── 프리페치 스냅샷 스냅(일봉·자본·3분봉). 아직 준비 전이면 다음 하트비트 대기 ──
        //  무거운 REST는 공용 프리페치 풀이 미리 당겨둔다. 여기선 락을 짧게 잡고 포인터만 잡는다.
        BarSnapshot snapshot_bars;
        int      bars_bucket  = -1;
        uint64_t bars_version = 0;
        {
            std::lock_guard<std::mutex> lock(snap_mutex_);

            if (!snap_daily_)
            {
                return; // 일봉 미준비 — 프리페치 대기
            }

            daily_  = snap_daily_; // 포인터 하나 — 벡터 복사가 아니다
            equity_ = snap_equity_;
            snapshot_bars = snap_bars_;
            bars_bucket   = snap_bars_bucket_;
            bars_version  = snap_bars_version_;
        }

        // 판단 봉은 아래에서 접거나 진행 봉 종가를 덮으므로 이 평가가 소유해야 한다. 체결이 살아 있으면
        //  집계기에서 새로 만들어지고, 그 경로에서는 스냅샷을 아예 복사하지 않는다.
        std::vector<MarketData> bars;

        const bool local_bars = websocket_bars_ && websocket_live_;

        if (websocket_bars_)
        {
            // 날짜가 바뀌면 집계기를 비운다 — REST 분봉은 당일치만 돌려주므로 어제 봉이 SMA에 섞이면 뜻이 다르다.
            std::string today = kst_ymd();

            if (aggregator_day_ != today)
            {
                aggregator_.clear(symbol_id_);
                aggregator_day_        = std::move(today);
                reseed_pending_ = true;
            }

            // 새 REST 스냅샷(1분봉)은 한 번만 시드한다. 닫힌 자리는 REST가 이기고 빈 자리는 채워지므로,
            //  폴백 동안 못 본 분·구독 뒤 늦게 붙은 종목의 앞 분이 여기서 메워진다.
            if (snapshot_bars && !snapshot_bars->empty() && bars_version != seeded_version_)
            {
                const int  before = aggregator_.closed_count(symbol_id_);
                const int  added  = aggregator_.seed(symbol_id_, *snapshot_bars);
                const bool first  = seeded_version_ == 0;
                seeded_version_   = bars_version;
                reseed_pending_   = false;
                const std::string line = "[" + id() + "] 봉 시드 src=" + (local_bars ? "ws" : "rest") +
                                         " REST " + std::to_string(snapshot_bars->size()) + "봉, 새 " + std::to_string(added) +
                                         ", 닫힌 " + std::to_string(aggregator_.closed_count(symbol_id_)) + "(전 " + std::to_string(before) + ")" +
                                         ", 진행 " + (aggregator_.current_slot(symbol_id_).valid() ? "있음" : "없음");

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
                bars = bars::resample(*snapshot_bars, parameters_.interval_min, parameters_.simple_moving_average_period + 1);
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

        // ── 일봉 존 판정(정배열 + SMA20 눌림) ────────────────────────────────
        // 오늘 현재가를 이동평균에 접어 넣는다. 접지 않으면 정배열도 SMA20도 하루 종일
        //  전일 값이라, 장중에 이평이 깨져도 존은 활성으로 남고 이격만 움직인다.
        //  스캐너(UniverseScanner)와 같은 식·같은 허용오차를 쓴다(MaAlign.h).
        const quant::moving_average::SimpleMovingAverages daily_averages_previous = daily_simple_moving_averages_previous(*daily_);
        const quant::moving_average::SimpleMovingAverages daily_averages   = daily_simple_moving_averages(*daily_, current_price);
        // 축이 둘이다. 접은 정배열은 진입만 연다. 유지·청산은 전일 확정 정배열로 판정한다.
        //  접은 값은 min_action_ms(3초)마다 뒤집힐 수 있는데 존 이탈에 붙은 행위가 보유 전량
        //  시장가 매도다. 09-10 일봉 캐시(정배열 통과 128종목)로 재면 average_5>s10이 73%에서 가장
        //  먼저 깨지고, 정배열이 무너지는 장중 하락폭 5분위가 1.45%다 — 유니버스의 약 10%가
        //  매일 "2~3% 밀리면 전량 매도, 되돌아오면 재매수"가 된다. 왕복마다 수수료·세금·
        //  슬리피지가 실현손실로 남고, 지수 게이트에서 방금 없앤 떨림을 종목 단위로 되살린다.
        //  청산은 되돌릴 수 없으니 느린 축에 맡긴다. [why D-033]
        const bool   aligned      = daily_averages.average_60 > 0.0 && quant::moving_average::aligned(daily_averages, parameters_.align_moving_average_tolerance_percent);
        const bool   aligned_hold = daily_averages_previous.average_60 > 0.0 && quant::moving_average::aligned(daily_averages_previous, parameters_.align_moving_average_tolerance_percent);
        const double d_s20   = daily_averages.average_20;
        // 방향성 이격(부호 유지): +면 SMA20 위(확장추격), −면 아래(눌림). 절대값 금지.
        //  SMA20 미확보(≤0) 시 큰 양수 센티넬로 둬 존 상단 밖으로 밀어내 진입을 막는다.
        constexpr double kNoDataDeviationPct = 999.0;
        const double deviation20_percent   = d_s20 > 0.0 ? (current_price - d_s20) / d_s20 * 100.0 : kNoDataDeviationPct;
        // 방향성 눌림 게이트: 진입은 "SMA20 이하(≤0%) ~ pullback_pct 아래"의 눌림 구간에서만.
        //   정배열 상승추세에서 SMA20 눌림(entry_upper_percent=0) 또는 SMA20 위 소폭(entry_upper_percent>0)까지 진입 허용.
        //   히스테리시스: 활성이면 상단 +zone_hysteresis 더 여유, 하단 −(pullback+zone_hysteresis)까지 유지(경계 진동 방지).
        const double up_threshold   = parameters_.entry_upper_percent + (in_zone_ ? parameters_.zone_hysteresis_percent : 0.0); // 진입 상단=entry_upper, 유지=+hysteresis
        const double down_threshold = in_zone_ ? parameters_.pullback_percent + parameters_.zone_hysteresis_percent // 유지 하단
                                        : parameters_.pullback_percent;                   // 진입 하단
        // 존 하단: 기본은 SMA20 아래 -down_threshold(눌림). entry_lower_percent>0인 추세확장 슬리브는
        //  하단을 SMA20 위로 올려, 눌림 슬리브의 상단과 맞물리되 겹치지 않게 한다.
        const double low_threshold  = parameters_.entry_lower_percent > 0.0
                                   ? parameters_.entry_lower_percent - (in_zone_ ? parameters_.zone_hysteresis_percent : 0.0)
                                   : -down_threshold;
        const bool   band    = d_s20 > 0.0 && deviation20_percent <= up_threshold && deviation20_percent >= low_threshold;
        const bool   zone    = aligned && band;          // 진입 게이트
        // [inv] hold_zone은 zone보다 넓어야 한다(정배열 축이 느린 쪽) — 좁아지면 막 산 걸 다음
        //  틱에 바로 되판다. aligned_hold(전일 확정)만 쓰면 당일 막 정배열이 시작된 종목은 전일
        //  축이 아직 못 따라와 hold_zone=N인데 zone=Y가 나온다(GS건설 09-15: 정배열=Y 101건
        //  전부 유지=N — 진입 자체가 막혔다). aligned_today를 OR로 더해 진입 순간엔 항상
        //  hold_zone⊇zone이 되게 한다 — research/studies/14_hold_axis V2로 짝비교(무해, 유의한
        //  손실 없음)까지 확인했다 [why D-088].
        const bool   hold_zone = (aligned_hold || aligned) && band;   // 유지 게이트 — 청산 판정
        in_zone_ = zone;

        // 존 판정 로그: 상태 변화 시 또는 60초마다 1회.
        if (zone != last_zone_ || zone_log_ts_.time_since_epoch().count() == 0 ||
            now - zone_log_ts_ >= std::chrono::seconds(60))
        {
            LOG_INFO("[" + id() + "] " + display() + " 존 판정 " + (zone ? "활성" : "대기") +
                     " | 정배열=" + (aligned ? "Y" : "N") +
                     " 일봉SMA20=" + format_one_decimal(d_s20) + " 현재가=" + format_one_decimal(current_price) +
                     " 이격=" + format_one_decimal(deviation20_percent) + "% (진입밴드 " + format_one_decimal(low_threshold) + "%~" + format_one_decimal(up_threshold) +
                     "%) 유지=" + (hold_zone ? "Y" : "N") +
                     " 일봉수=" + std::to_string(daily_->size()));
            last_zone_    = zone;
            zone_log_ts_  = now;
        }

        if (!hold_zone)
        {
            int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

            if (position > 0)
            {
                // 존 이탈 → 미체결 전부 취소 + 보유분 시장가 청산(매도가능분 클램프+백오프).
                cancel_all(out);
                emit_liquidation(out, position, now, "존 이탈"); // 클램프+백오프(자체 로깅)
                return;
            }

            // 보유가 없으면 청산할 게 없다 — 미체결 매수만 거두고, 진입 여부는 아래 zone에 맡긴다.
            if (cancel_all(out))
            {
                LOG_INFO("[" + id() + "] 존 이탈 — 미체결 취소(보유 0)");
            }
        }

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
                const std::string tag = "손절(평단 " + format_one_decimal(last_average_price_) + " -" + format_one_decimal(parameters_.stop_loss_percent) + "%)";
                // 스탑은 지수 백오프 상한을 30초로 둔다 — 5분 보류는 손절이 아니다.
                emit_liquidation(out, position, now, tag, kLiquidationBackoffMs);
                stop_cooldown_until_ = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
                return;
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
                    return;
                }
            }
        }

        if (!zone)
        {
            // 진입 축만 닫혔다. 미체결 매수는 거두되 보유는 그대로 둔다 — 장중에 이평이
            //  깨졌다고 파는 대신 되돌아오면 그대로 이어간다. 청산은 위 hold_zone이 맡는다.
            if (cancel_all(out) && !entry_closed_logged_)
            {
                LOG_INFO("[" + id() + "] " + display() +
                         " 진입 축 닫힘(장중 정배열) — 미체결 취소, 보유 유지");
                entry_closed_logged_ = true;
            }

            return;
        }

        entry_closed_logged_ = false;

        // ── 3분봉 기준선(스냅샷에서 이미 받음) ───────────────────────────────
        const bool warming = static_cast<int>(bars.size()) < parameters_.simple_moving_average_period;

        if (warming && !parameters_.daily_basis_warmup)
        {
            // 개장 직후엔 3분봉이 simple_moving_average_period(20봉=60분)만큼 쌓이지 않아 여기서 매번 되돌아간다.
            //  로그가 없으면 '존 활성인데 주문 0건'이 원인 불명으로 보인다(2026-09-07 실제 발생).
            //  60초에 한 번만 남겨 개장 구간 로그가 넘치지 않게 한다.
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()).count();

            if (now_ms - last_warm_log_ms_ >= 60000)
            {
                last_warm_log_ms_ = now_ms;
                LOG_INFO("[" + id() + "] 봉 부족 — 대기 " + std::to_string(bars.size()) + "/" +
                         std::to_string(parameters_.simple_moving_average_period) + "봉(" + std::to_string(parameters_.interval_min) +
                         "분) 현재가=" + format_one_decimal(current_price));
            }

            return;
        }

        // 워밍업 구간에는 일봉 SMA20(존 게이트가 이미 쓴 값)을 기준선으로 대신 쓴다.
        const double simple_moving_average = warming ? d_s20 : simple_moving_average_close(bars, parameters_.simple_moving_average_period); // bars[0]=최신

        if (simple_moving_average <= 0.0)
        {
            return;
        }

        // ── 트레일: 3분봉 기준선 아래로 tol만큼 내려오면 청산(워밍업 제외) ────────────
        if (parameters_.trail_simple_moving_average_exit && !warming &&
            current_price < simple_moving_average * (1.0 - parameters_.trail_simple_moving_average_tolerance_percent / 100.0))
        {
            const int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

            if (position > 0)
            {
                cancel_all(out);
                emit_liquidation(out, position, now, "3분봉 기준선 이탈(" + format_one_decimal(simple_moving_average) + ")", kLiquidationBackoffMs);
                stop_cooldown_until_ = now + std::chrono::seconds(parameters_.stop_cooldown_sec);
                return;
            }
        }

        if (warming)
        {
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()).count();

            if (now_ms - last_warm_log_ms_ >= 60000)
            {
                last_warm_log_ms_ = now_ms;
                LOG_INFO("[" + id() + "] 일봉 기준선 대체 — 3분봉 " + std::to_string(bars.size()) + "/" +
                         std::to_string(parameters_.simple_moving_average_period) + "봉, 일봉SMA20=" + format_one_decimal(d_s20) +
                         " 현재가=" + format_one_decimal(current_price));
            }
        }

        int position = confirmed_position(parameters_.account, symbol_id_, parameters_.ticker);

        // ── 하루 단위 진입 필터(전일 ATR·개장 이격) — 보유가 없을 때만 새 진입을 막는다 ──
        if (position <= 0 && !day_entry_allowed(current_price, daily_averages_previous.average_20, hhmm))
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

        // ── 목표 분할 매수 산출(발주 전) ─────────────────────────────────────────
        //  계단 가격·수량은 simple_moving_average·pos의 순수 함수. 먼저 계획을 만들고 직전 분할 매수와
        //  시그니처를 비교해 "동일하면 재발주 스킵". 분봉 정지(HTTP 500 폴백)로
        //  simple_moving_average=price가 고정될 때 동일 분할 매수를 취소·재발주하던 처닝을 차단.
        struct SplitStep { OrderSide side; double price; int quantity; };
        std::vector<SplitStep> plan;

        // ── 명목 사이징: 자본%를 가격으로 나눠 수량 산출(자본 미상이면 주수 폴백) ──
        //  베이스=자본×base_percent(5%), 물타기 총예산=자본×(max_percent−base_percent)(5%)를 split_step_count로 분할.
        //  베이스+물타기 합 ≈ 자본×max_percent(10%) → OrderGate 명목캡과 정합(캡은 백스톱).
        const double equity            = equity_ > 0.0 ? equity_ : parameters_.fallback_equity;
        // 국면 매수비율(OrderGate::entry_scale, 0~1)을 명목에 곱한다. 스위치(halt)가 아니라 비율이라
        //  코스피 −1%면 70%, −2%면 40%처럼 줄어들고 반등하면 돌아온다. 0.1 단위로 끊어 3분마다
        //  분할 매수가 재구성되는 일을 막고, 시그니처에 붙여 바뀐 회차에만 다시 깐다. [why D-083]
        const double rscale        = std::round(std::clamp(entry_scale(), 0.0, 1.0) * 10.0) / 10.0;
        const double multiplier          = (parameters_.size_mult > 0.0 ? parameters_.size_mult : 1.0) * rscale;
        // 원 단위 총액이 주어지면 그 금액을 base_percent:max_percent 비율로 베이스·물타기에 나눈다.
        const double base_share    = parameters_.max_percent > parameters_.base_percent ? parameters_.base_percent / parameters_.max_percent : 1.0;
        const double base_notional = parameters_.notional_krw > 0.0 ? parameters_.notional_krw * rscale * base_share
                                                            : equity * parameters_.base_percent * multiplier;
        const double split_step_budget   = parameters_.notional_krw > 0.0 ? parameters_.notional_krw * rscale - base_notional
                                                            : equity * (parameters_.max_percent > parameters_.base_percent ? parameters_.max_percent - parameters_.base_percent : 0.0) * multiplier;
        const double split_step_notional = parameters_.buy_split_steps > 0 ? split_step_budget / parameters_.buy_split_steps : 0.0;

        // 분할 매수 기준점. 교차 가드가 켜져 있으면 각 방향 층이 현재가를 넘지 않도록 기준선을
        //  현재가 쪽으로 당긴다. 이격이 벌어진 상태에서도 분할 매수 간격은 그대로 유지된다.
        //  base_on_price면 기준선을 현재가로 둔다. 이격 +5~30% 구간에서 SMA20을 기준점으로
        //   쓰면 매수층 전부가 시장가에서 그만큼 아래에 깔려 하루 종일 한 주도 안 붙는다.
        //   추세 슬리브는 "지금 값에서 한 호가 아래"로 붙어야 추세에 올라탄다.
        const bool   guard_on   = parameters_.cross_guard && current_price > 0.0;
        const double base_line  = (parameters_.base_on_price && current_price > 0.0) ? current_price : simple_moving_average;
        const double sell_base_line = guard_on ? (std::max)(base_line, current_price) : base_line;
        const double buy_base_line  = guard_on ? (std::min)(base_line, current_price) : base_line;

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
            peak_position_        = 0;
        }
        else
        {
            peak_position_ = (std::max)(peak_position_, position);
        }

        const bool base_short = position > 0 && base_target_quantity_ > 0 && peak_position_ < base_target_quantity_;

        if (position <= 0 || base_short)
        {
            double buy_price = round_to_tick(base_line, OrderSide::BUY);

            // 교차 가드: 기준선이 현재가 이상이면 이 지정가는 즉시 시장가로 체결된다.
            //  베이스를 건너뛰면 add_below_sma_only가 노리는 눌림 진입에서 가장 큰 레그가
            //  빠지므로, 억제 대신 현재가 한 틱 아래로 옮겨 지정가로 남긴다.
            if (parameters_.cross_guard && current_price > 0.0 && buy_price >= current_price)
            {
                buy_price = round_to_tick(current_price - krx::tick_size(current_price), OrderSide::BUY);
            }

            int    base_quantity = base_short ? base_target_quantity_ : quantity_for(base_notional, buy_price);

            if (base_quantity <= 0)
            {
                base_quantity = parameters_.base_quantity;  // 자본 미상 폴백
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
        const int sell_per = parameters_.split_step_count > 0 ? (position + parameters_.split_step_count - 1) / parameters_.split_step_count : position; // ceil
        const bool   sell_from_average = parameters_.sell_base_average && last_average_price_ > 0.0;
        const double sell_base  = sell_from_average ? last_average_price_ : sell_base_line;

        for (int split_step_index = 1; split_step_index <= parameters_.split_step_count && sell_avail > 0; ++split_step_index)
        {
            double sell_price = round_to_tick(sell_base * (1.0 + parameters_.deviation_sell * split_step_index / 100.0), OrderSide::SELL);

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

        // 매수 밴드(물타기): 이격 −deviation_buy%*i, buy_split_steps층. 분할 단계당 자본의 split_step_notional. 종목당 상한은 OrderGate가 캡.
        //  점진 진입: add_below_sma_only면 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 물타기를 깐다.
        //  → 활성 즉시 base+물타기를 한꺼번에 예약해 1분 만에 10% 만재되던 성격을 제거. 기준선 위/근처에선
        //    base(+익절 매도레그)만 유지하고, 진짜 눌림이 와야 평단을 낮춘다.
        //  추세확장 슬리브(base_on_price)는 add_below_simple_moving_average_only=false로 돌린다 — 이격이 벌어진
        //   구간에서 "기준선 아래"는 거의 안 오므로 켜 두면 분할 매수가 영영 안 깔린다. 그 슬리브의
        //   하방 분할 매수 자체는 buy_split_steps=0으로 끈다(2026-09-11 회의 §1-4).
        // 워밍업(기준선=일봉SMA20) 구간에는 물타기를 잠근다. 존 진입 조건이 이격 -pullback_percent~
        //  +entry_upper_pct라 `cur_px < 일봉SMA20`이 거의 항상 참이 되어, 3분봉 기준선이 뜻하던
        //  "단기 눌림에서만 추가"가 사실상 상시 개방으로 바뀐다. 변동성이 가장 큰 첫 60분에
        //  base와 물타기가 한꺼번에 나가는 것을 막는다(base 진입과 익절 매도는 그대로 둔다).
        if ((!parameters_.add_below_simple_moving_average_only || current_price < simple_moving_average) && !warming)
        {
            for (int buy_split_step_index = 1; buy_split_step_index <= parameters_.buy_split_steps; ++buy_split_step_index)
            {
                double buy_price = round_to_tick(buy_base_line * (1.0 - parameters_.deviation_buy * buy_split_step_index / 100.0), OrderSide::BUY);
                int    split_step_quantity = quantity_for(split_step_notional, buy_price);

                if (split_step_quantity <= 0)
                {
                    split_step_quantity = parameters_.step_quantity;  // 자본 미상 폴백
                }

                if (buy_price > 0.0 && split_step_quantity > 0)
                {
                    plan.push_back({OrderSide::BUY, buy_price, split_step_quantity});
                }
            }
        }

        // ── no-change 가드: 처닝 차단. 분할 매수가 살아있고 (a)계획 시그니처가 직전과 동일하거나
        //    (b)SMA 이동이 reprice_move_ticks 데드밴드 이내이고 포지션도 그대로면 재발주 스킵.
        //    (b)가 핵심: SMA가 틱경계를 스치며 미세이동할 때마다 전량 취소·재발주해 매도 분할 단계가
        //    체결 전에 취소되고 매수만 쌓여 pos가 편증하던 처닝을 차단(reprice_move_ticks 구현).
        std::string signal;

        for (const auto& split_step : plan)
        {
            signal += split_step.side == OrderSide::BUY ? "B" : "S";
            signal += format_one_decimal(split_step.price) + "x" + std::to_string(split_step.quantity) + "|";
        }

        // G1 국면 게이트: 비활성 국면(regime→전략 자동선택에서 미선택)에선 매수(진입·물타기)
        //  분할 단계를 깔지 않는다. 익절 매도·청산은 국면과 무관하게 유지(is_active 계약: 진입만 차단).
        //  active 상태를 시그니처에 접미 → 국면 플립 시 no-change 가드에 걸리지 않고 재구성되어
        //  기존 매수 예약이 cancel_all로 취소된다(플립 후 매수만 잔존하는 구멍 차단).
        //  신규매수 차단(entry_halt)도 같은 축이다 — 게이트가 거부만 하면 계획이 안 바뀌어
        //  차단 해제 뒤에도 매수 분할 단계가 돌아오지 않았다. 차단 중엔 매수 분할 단계를 걷고(취소),
        //  풀리면 시그니처가 바뀌어 다시 깐다. 떨림은 D-033 체류가 막는다. [why D-033]
        //  스탑·트레일 뒤 쿨다운도 같은 축이다 — 존이 열려 있어도 분할 매수를 걷는다.
        //  전량 청산 뒤 재진입 대기도 같은 축이다.
        const bool cooling  = (stop_cooldown_until_ != std::chrono::steady_clock::time_point{} && now < stop_cooldown_until_) ||
                              (reentry_cooldown_until_ != std::chrono::steady_clock::time_point{} && now < reentry_cooldown_until_);
        const bool entry_on = is_active() && !entry_halted() && !cooling && rscale > 0.0;
        signal += entry_on ? "A1" : "A0";
        signal += 'S';
        signal += std::to_string(static_cast<int>(rscale * 10.0 + 0.5)); // 매수비율 0~10

        // 먼지 정리: 보유 평가금이 dust_krw 아래인데 깔 매수 분할 단계가 없으면(베이스 끝·물타기 없음·진입 차단)
        //  이 보유는 커질 길이 없이 슬롯만 차지한다. 익절 지정가 대신 시장가로 정리한다. 매수 분할 단계가 있으면
        //  베이스 잔량이 채워지는 중이라 둔다. 재시도 간격은 emit_liquidation 백오프가 맡는다. [why D-081]
        if (position > 0 && parameters_.dust_krw > 0.0 && current_price > 0.0 && position * current_price < parameters_.dust_krw)
        {
            bool has_buy = false;

            for (const auto& split_step : plan)
            {
                has_buy = has_buy || (entry_on && split_step.side == OrderSide::BUY);
            }

            if (!has_buy)
            {
                cancel_all(out);
                emit_liquidation(out, position, now, "먼지 정리(평가금 " + format_one_decimal(position * current_price) + " < " +
                                                    format_one_decimal(parameters_.dust_krw) + ")");
                return;
            }
        }

        // 데드밴드는 분할 주문 기준점 기준이다. 현재가 기준점(base_on_price)에서 SMA로 재면 값이
        //  틱마다 바뀌는데 데드밴드는 조용하다고 판정해 (b)가 걸리지 않았다(09-11 TRENDX 재구성
        //  669회 vs DEVSCALE 160회).
        const double split_buy_reference   = parameters_.base_on_price ? base_line : simple_moving_average;
        const double reprice_band = parameters_.reprice_move_ticks * tick_size(split_buy_reference);
        const bool   simple_moving_average_quiet    = last_split_buy_reference_ > 0.0 && std::fabs(split_buy_reference - last_split_buy_reference_) < reprice_band;

        // (a) 계획 시그니처+pos가 직전과 동일하면 live 유무와 무관하게 스킵.
        //     매도가능=0이라 아무것도 못 깔아 live_가 빈 채로 남을 때(원장 보유↔매도가능 괴리)
        //     매 하트비트 재진입해 잔고조회를 난사하던 스핀을 차단. 체결로 pos가 바뀌면 즉시 재구성.
        // (b) 데드밴드(reprice 이내 미세이동)+position 동일 스킵은 살아있는 분할 매수에만 적용.
        if (signal == last_split_buy_signal_ && position == last_position_)
        {
            return; // 동일 계획 → 유지(빈 계획 포함)
        }

        if (!live_.empty() && simple_moving_average_quiet && position == last_position_)
        {
            return; // 데드밴드 내 미세이동 → 유지
        }

        // (c) 재구성 최소 간격. (a)(b)는 둘 다 position == last_pos_를 요구하므로, 부분체결이
        //     연달아 들어오는 종목은 체결마다 분할 매수를 통째로 헐고 다시 깐다. 분할 단계 2개면
        //     체결 1건에 취소 2 + 신규 2가 나가고, 이게 계좌 공용 주문예산(5/s·20/min)을
        //     한 종목이 독점한다(09-08 12:31~12:36 016610 단독 84건 = 전체의 8할).
        //     여기서 막아도 기존 분할 단계는 살아 있으므로 체결 기회를 잃지 않는다. 잠깐 크기가
        //     낡은 채로 유지될 뿐이다. 청산(emit_liquidation)은 이 경로를 타지 않는다.
        //     live_가 비어 있어도 적용한다 — 매도가능=0으로 아무것도 못 깔면 live_가 빈 채
        //     남는데, 그때 이 가드를 건너뛰면 하트비트(3초)마다 재구성·잔고조회가 돈다.
        if (parameters_.min_rebuild_sec > 0 &&
            last_rebuild_ != std::chrono::steady_clock::time_point{})
        {
            const auto since = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - last_rebuild_).count();

            if (since < parameters_.min_rebuild_sec)
            {
                return;
            }
        }

        // ── 재구성: 기존 취소 후 신규 지정가 ──────────────────────────────────
        //  익절 매도는 재구성 시점의 실매도가능분(ord_psbl_qty)으로 클램프 → 원장 보유와
        //  매도가능 괴리(예약매도·미결제)로 KIS가 전량 거부(40240000 "잔고내역 없습니다")하던
        //  것을 차단. 청산 경로(emit_liquidation)와 동일 원칙. 잔고조회는 재구성 시 1회만
        //  (지연에 민감한 경로 부하 억제 — 매수는 캡을 OrderGate가 처리하므로 클램프 불필요).
        cancel_all(out);
        // G4: 이 분할 매수를 깐 판단 근거 — 존 판정 지표를 신호에 실어 영속(로그 재구성 불필요).
        // 슬리브에 따라 근거 문구를 바꾼다. 추세확장(TRENDX)은 SMA20 위 과확장 구간을
        //  일부러 사는 슬리브라 "눌림"이라고 찍으면 운영자가 오독한다(09-08 한미사이언스
        //  이격 +23.8%가 "정배열눌림진입"으로 남아 눌림목이 아닌데 왜 샀냐는 질문이 나왔다).
        const std::string entry_kind = parameters_.entry_lower_percent > 0.0 ? "정배열추세확장진입"
                                                                : "정배열눌림진입";
        // 진입 문맥 스탬프(2026-09-11 회의 §3·§5): 체결강도(CTTR)·20일 평균 대비 누적거래량
        //  배율·직전 250봉 고가 대비 거리. 나중에 "저항 아래서 샀나"를 원장에서 바로 대조한다.
        //  REST 폴링 틱은 strength/accumulated_volume이 0이라 그때는 찍지 않는다.
        std::string entry_context;
        {
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
                entry_context += " 누적거래량/20일평균=" + format_one_decimal(static_cast<double>(trade.accumulated_volume) / volume20);
            }

            if (hi250 > 0.0)
            {
                entry_context += " 250봉고가대비=" + format_one_decimal((current_price - hi250) / hi250 * 100.0) + "%";
            }

            if (parameters_.stop_loss_percent > 0.0)
            {
                entry_context += " 손절=-" + format_one_decimal(parameters_.stop_loss_percent) + "%";
            }
        }

        const std::string buy_context  = entry_kind + " 이격=" + format_one_decimal(deviation20_percent) + "% 일봉SMA20=" +
                                     format_one_decimal(d_s20) + " 현재가=" + format_one_decimal(current_price) + entry_context;
        int sell_room = -1;                          // -1=미조회(지연). 첫 매도 분할 단계에서 1회 조회.

        for (const auto& split_step : plan)
        {
            if (split_step.side == OrderSide::SELL)
            {
                if (sell_room < 0)
                {
                    sell_room = sellable_quantity();      // 안전 우선: 불확실하면 0(매도 보류)
                }

                int quantity = split_step.quantity < sell_room ? split_step.quantity : sell_room;

                if (quantity <= 0)
                {
                    continue;                        // 매도가능 소진/없음 → 이 분할 단계 스킵
                }

                place(out, OrderSide::SELL, split_step.price, quantity, "익절밴드 지정가=" + format_one_decimal(split_step.price));
                sell_room -= quantity;
            }
            else if (entry_on)                       // G1: 비활성 국면이면 매수 분할 단계 스킵(진입 차단)
            {
                place(out, split_step.side, split_step.price, split_step.quantity, buy_context);
            }
        }

        last_split_buy_reference_ = split_buy_reference;
        last_split_buy_signal_ = std::move(signal);
        last_position_ = position;
        last_rebuild_ = std::chrono::steady_clock::now();
        LOG_INFO("[" + id() + "] 분할 매수 재구성 sma=" + format_one_decimal(simple_moving_average) +
                 (warming ? "(일봉)" : "") + " src=" + (local_bars ? "ws" : "rest") + " px=" + format_one_decimal(current_price) +
                 " pos=" + std::to_string(position) + " live=" + std::to_string(live_.size()) +
                 " 명목=" + std::to_string(static_cast<long long>(base_notional + split_step_budget)) + "원");
    }

private:
    // ── 지표 (indicators.py 이식, bars[0]=최신) ──────────────────────────────
    // 하루 단위 진입 필터 판정 — 그날 첫 평가(개장 봉이 닫힌 09:03 이후)에서 한 번 정하고 하루 동안 고정한다.
    //  개장 이격은 그 첫 평가의 현재가로 잰다(재기동이 늦으면 그 시각 가격 — 갭 회피가 목적이라 근사로 충분).
    //  09:03 전에는 판정을 못 하므로 진입을 미룬다(그 구간은 봉 부족으로 어차피 안 산다).
    bool day_entry_allowed(double current_price, double previous_sma20, int hhmm)
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
            day_entry_allowed_ = devscale_rules::entry_day_allowed(atr_percent, open_deviation_percent,
                                                                   parameters_.entry_atr_max_percent,
                                                                   parameters_.entry_open_deviation_min_percent,
                                                                   parameters_.entry_open_deviation_max_percent);
            LOG_INFO("[" + id() + "] 진입 필터 " + (day_entry_allowed_ ? "통과" : "차단") +
                     "(ATR14 " + format_one_decimal(atr_percent) + "% 개장 이격 " + format_one_decimal(open_deviation_percent) +
                     "% 기준 ATR<=" + format_one_decimal(parameters_.entry_atr_max_percent) + " 이격>=" +
                     format_one_decimal(parameters_.entry_open_deviation_min_percent) + ")");
        }

        return day_entry_allowed_;
    }

    static double simple_moving_average_close(const std::vector<MarketData>& bars, int period)
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

    // 전일까지의 일봉 이동평균에 오늘 현재가를 접어 넣어 돌려준다. 60봉 미만이면 average_60=0인
    //  빈 값이라 호출부가 정배열을 false로 떨어뜨린다(판정 자체를 못 하는 상태).
    //  [why D-005] 일봉 조회가 당일 봉을 자르므로 여기서 오늘을 되살린다.
    // 전일 확정 이동평균. 오늘 현재가를 접지 않아 세션 내내 상수다 — 청산처럼 되돌릴 수
    //  없는 판정이 이쪽을 쓴다. 60봉 미만이면 전 필드 0을 돌려준다(호출자가 average_60>0으로 거른다).
    static quant::moving_average::SimpleMovingAverages daily_simple_moving_averages_previous(const std::vector<MarketData>& daily)
    {
        quant::moving_average::SimpleMovingAverages previous;

        if (static_cast<int>(daily.size()) < 60)
        {
            return previous;
        }

        previous.average_5  = simple_moving_average_close(daily, 5);
        previous.average_10 = simple_moving_average_close(daily, 10);
        previous.average_20 = simple_moving_average_close(daily, 20);
        previous.average_60 = simple_moving_average_close(daily, 60);
        return previous;
    }

    static quant::moving_average::SimpleMovingAverages daily_simple_moving_averages(const std::vector<MarketData>& daily, double current_price)
    {
        const quant::moving_average::SimpleMovingAverages previous = daily_simple_moving_averages_previous(daily);

        if (previous.average_60 <= 0.0)
        {
            return previous;
        }

        return quant::moving_average::fold_today(previous, daily[4].close, daily[9].close,
                                     daily[19].close, daily[59].close, current_price);
    }

    // ── 프리페치: 무거운 REST(3분봉·일봉·잔고)를 공유 전략 스레드 밖에서 미리 당겨
    //    스냅샷에 적재한다. on_trade_batch는 스냅샷만 읽어(락 짧게) 발주를 판단 → 특정
    //    종목의 느린 REST가 전 전략을 막던 head-output_file-line 블로킹을 없앤다. 발주·매도가능
    //    (sellable_quantity)은 원장 최신성을 위해 동기 유지. 여기서 부르는 KIS 메서드는 전부
    //    읽기전용(get_daily_ohlcv·get_minute_ohlcv·get_balance, 동시호출 감사 완료).
    //  주기(min_action_ms)와 스레드는 공용 풀이 가진다 — 이 함수는 한 주기에 한 번 불린다. [why D-071]
    void prefetch_once()
    {
        // 장 밖에서는 받아봐야 같은 응답이다. KIS 분봉은 기준시각을 15:30으로 클램프하므로
        //  (KisClient.cpp) 장 마감 후엔 종일 같은 봉을 다시 받고, 그 호출이 초당 한도를
        //  차지해 다른 조회를 500으로 밀어낸다. 발주는 어차피 장중에만 나가므로 건너뛴다.
        //  창은 08:50~15:35로 장 마감 청산(15:15)까지 덮는다.
        const int hhmm = kst_hhmm();
        const int wday = kst_tm().tm_wday;
        const bool in_session = (wday >= 1 && wday <= 5) && hhmm >= kPrefetchWindowOpenHhmm && hhmm <= kPrefetchWindowCloseHhmm;

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
                    snap_daily_      = std::make_shared<const std::vector<MarketData>>(std::move(daily_ohlcv));
                    snap_daily_date_ = std::move(today);
                    snap_equity_     = equity;
                }
            }

            // 3분봉: 봉이 바뀔 때만 갱신한다. 이 조회는 페이지네이션이라 1회에 HTTP GET이
            //  세 번 나가는데(당일 63분치 1분봉을 다시 받아 집계), 그중 마감된 봉은 불변이고
            //  달라지는 건 진행 중인 봉 하나뿐이다. 그 하나는 아래 on_trade_batch가 들어오는
            //  체결 틱으로 덮으므로 SMA 값은 같게 유지되면서 조회는 봉 주기당 1회로 준다.
            //  bar_source=ws면 이 조회는 시드용이다 — 첫 스냅샷, 그리고 전략 스레드가 원할 때(워밍업·
            //  REST 대체 틱·출처 전환 뒤)만 봉마다 한 번 받고, 틱이 살아 있고 봉이 찼으면 쉰다. 이때는
            //  1분봉 그대로 받는다(같은 63분치·같은 GET 수) — 접는 건 전략 스레드의 resample이다. [why D-072]
            const int bucket = kst_bar_bucket(parameters_.interval_min);
            bool need_bars;
            {
                std::lock_guard<std::mutex> lock(snap_mutex_);
                need_bars = !snap_bars_ || snap_bars_bucket_ != bucket;

                if (need_bars && websocket_bars_ && snap_bars_ &&
                    !seed_wanted_.load(std::memory_order_relaxed))
                {
                    need_bars = false;
                }

                // 봉 경계 직후 종목별 지터만큼 미룬다(첫 스냅샷은 바로). 진행 중인 봉은
                //  on_trade_batch의 체결가 덮어쓰기가 채우므로 늦게 받아도 SMA는 같다.
                if (need_bars && snap_bars_ &&
                    kst_sec_into_bucket(parameters_.interval_min) < prefetch_jitter_sec_)
                {
                    need_bars = false;
                }
            }

            if (need_bars)
            {
                auto bars = websocket_bars_
                                ? kis_->get_minute_ohlcv(parameters_.ticker, (parameters_.simple_moving_average_period + 1) * parameters_.interval_min, 1)
                                : kis_->get_minute_ohlcv(parameters_.ticker, parameters_.simple_moving_average_period + 1, parameters_.interval_min);

                if (!bars.empty())
                {
                    std::lock_guard<std::mutex> lock(snap_mutex_);
                    snap_bars_        = std::make_shared<const std::vector<MarketData>>(std::move(bars));
                    snap_bars_bucket_ = bucket;
                    ++snap_bars_version_;
                }
            }
        }
    }

    // 등록 해제. 돌아온 뒤에는 prefetch_once()가 실행 중이지도, 다시 불리지도 않는다.
    void stop_prefetch()
    {
        if (prefetch_pool_ && prefetch_task_ != 0)
        {
            prefetch_pool_->remove(prefetch_task_);
        }

        prefetch_task_ = 0;
    }

    // 사이징 기준 자본(총평가금) 조회. output2 tot_evlu_amt(없으면 nass_amt). 알 수 없으면 0.
    //  프리페치 스레드에서 호출(읽기전용). 폴백(fallback_equity) 적용은 호출측(on_trade_batch, line equity).
    // 총평가금은 계좌 하나의 값이라 종목마다 다시 부를 이유가 없다. 전략 스레드가 종목 수만큼
    //  있어 기동 직후 같은 잔고 조회가 40건 동시에 나갔고, 모의 키 2건/초 한도를 주문까지
    //  끌어내렸다(09-11 09:18 EGW00201). 프로세스 공용으로 하루 한 번만 부르고, 실패(0)는
    //  캐시하지 않아 다음 종목이 다시 시도한다. 뮤텍스를 조회 동안 잡아 동시 진입도 한 번으로 접는다.
    double fetch_equity()
    {
        static std::mutex  s_mutex;
        static std::string s_ymd;
        static double      static_equity = 0.0;
        std::lock_guard<std::mutex> lock(s_mutex);
        std::string today = kst_ymd();

        if (s_ymd == today && static_equity > 0.0)
        {
            return static_equity;
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
            catch (...) {}
        }

        if (equity > 0.0)
        {
            s_ymd = std::move(today);
            static_equity  = equity;
        }

        return equity;
    }

    // 명목→수량(주). 가격/명목 유효하지 않으면 0(호출측이 폴백 결정).
    static int quantity_for(double notional, double price)
    {
        if (price <= 0.0 || notional <= 0.0)
        {
            return 0;
        }

        int quantity = static_cast<int>(std::floor(notional / price));
        return quantity > 0 ? quantity : 0;
    }

    // ── KRX 호가단위/격자 절사는 core/TickSize.h(krx::)로 일원화. 얇은 위임만 유지. ──
    static double tick_size(double price) { return krx::tick_size(price); }
    static double round_to_tick(double price, OrderSide side) { return krx::round_to_tick(price, side); }

    std::string next_order_id(const char* tag)
    {
        return id() + ":" + tag + ":" + std::to_string(++sequence_);
    }

    void place(std::vector<OrderSignal>& out, OrderSide side, double price, int quantity,
               const std::string& reason = "")
    {
        if (quantity <= 0)
        {
            return; // quantity=0 NEW 발주 억제 — 게이트 거부·로그 노이즈 원천 차단(SELL은 상위서도 클램프)
        }

        std::string    order_id     = next_order_id(side == OrderSide::BUY ? "B" : "S");
        const uint64_t order_number = next_client_order_number(); // 라우터가 취소 대상을 찾는 키. 문자열은 로그용
        OrderSignal    signal;
        signal.ticker      = parameters_.ticker;
        signal.symbol_id         = symbol_id_;
        signal.side        = side;
        signal.type        = OrderType::LIMIT;
        signal.quantity    = quantity;
        signal.price       = price;
        signal.strategy_id = id();
        signal.market      = Market::KR;
        signal.action      = OrderAction::NEW;
        signal.client_order_id  = order_id;
        signal.client_order_number = order_number;
        signal.account_id  = parameters_.account;
        signal.reason      = reason; // G4: 판단 근거를 신호에 실어 영속
        signal.timestamp   = std::chrono::system_clock::now();
        out.push_back(std::move(signal));
        live_.push_back({std::move(order_id), order_number, side});
    }

    // 미체결 전량 취소. 발주가 있었으면 true.
    bool cancel_all(std::vector<OrderSignal>& out)
    {
        if (live_.empty())
        {
            return false;
        }

        for (auto& live_entry : live_) // live_는 아래에서 비우므로 주문 id를 옮긴다
        {
            OrderSignal signal;
            signal.ticker          = parameters_.ticker;
            signal.symbol_id             = symbol_id_;
            signal.side            = live_entry.side;
            signal.type            = OrderType::LIMIT;
            signal.quantity        = 0;
            signal.strategy_id     = id();
            signal.market          = Market::KR;
            signal.action          = OrderAction::CANCEL;
            signal.original_client_order_id     = std::move(live_entry.order_id);
            signal.original_client_order_number = live_entry.order_number;
            signal.account_id      = parameters_.account;
            signal.timestamp       = std::chrono::system_clock::now();
            out.push_back(std::move(signal));
        }

        live_.clear();
        return true;
    }

    OrderSignal make_market_sell(int quantity, const std::string& reason = "")
    {
        OrderSignal signal;
        signal.ticker      = parameters_.ticker;
        signal.symbol_id         = symbol_id_;
        signal.side        = OrderSide::SELL;
        signal.type        = OrderType::MARKET;
        signal.quantity    = quantity;
        signal.strategy_id = id();
        signal.market      = Market::KR;
        signal.action      = OrderAction::NEW;
        signal.account_id  = parameters_.account;
        signal.reason      = reason; // G4: 청산 사유(존 이탈/장 마감 등)를 신호에 실어 영속
        signal.reference_price   = liquidation_reference_price(); // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
        signal.timestamp   = std::chrono::system_clock::now();
        return signal;
    }

    // 시장가 청산 신호의 명목 평가 기준가. 직전 체결가 > 잔고 평단 > 최근 3분봉 종가 순.
    double liquidation_reference_price()
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

    // ── 청산 발주: 매도가능분 클램프 + 지수 백오프 ───────────────────────────
    //  버그 이력: 존이탈/장 마감 청산이 원장 보유수량 전량을 시장가 매도했으나, 예약매도
    //  (미연결/미결제)로 실매도가능분(ord_psbl_qty)이 보유보다 작으면 KIS가 전량 거부
    //  (40240000 "잔고내역 없습니다") → 매 하트비트 무한 재거부 스팸. 실계좌 동일.
    //  대책: (1) 매 시도 get_balance의 '실시간' 주문가능수량으로 클램프 → 잠긴 수량
    //  초과분 미발주(과매도·이중주문 위험 0, 브로커 상태 기준이라 체결지연에도 자기교정).
    //  (2) 시도 후 진행(position 감소) 없으면 30·60·120·240·480s(capture 300s) 지수 백오프.
    //  반환: 시장가 매도를 실제로 out에 넣었으면 true.
    //  clamp_sellable=false 면 잔고 조회 없이 position 전량을 낸다 — 라우터의 게이트 클램프·자가정리에 맡긴다(장 마감).
    bool emit_liquidation(std::vector<OrderSignal>& out, int position,
                          std::chrono::steady_clock::time_point now, const std::string& tag,
                          long long max_backoff_ms = 300000, bool clamp_sellable = true)
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
            LOG_WARN("[" + id() + "] " + tag + " 청산 보류 — 매도가능=0 (잠긴 " +
                     std::to_string(position) + "주, 예약취소/결제 대기) 백오프#" +
                     std::to_string(liquidation_fail_streak_ + 1));
        }

        liquidation_last_position_ = position;
        ++liquidation_fail_streak_;
        int shift = liquidation_fail_streak_ - 1;

        if (shift > 4)
        {
            shift = 4;
        }

        long long milliseconds = 30000LL << shift;                 // 30/60/120/240/480…

        if (milliseconds > max_backoff_ms)
        {
            milliseconds = max_backoff_ms;  // capture 기본 5분, 스탑 경로는 30초
        }

        liquidation_next_ = now + std::chrono::milliseconds(milliseconds);
        return emitted;
    }

    // 해당 종목의 실시간 매도가능수량(ord_psbl_qty). 안전 우선: 확실히 알 수 없으면 0
    //  (보류)을 반환해 절대 과매도/이중주문을 유발하지 않는다.
    //  - kis_ 없음/시세전용(계좌 없는 quote) 클라이언트 → 0 (불필요한 잔고 조회도 안 함).
    //    실계좌(단일 클라이언트)는 account 보유 → 정상 조회로 클램프.
    //  - 조회 실패(예외·output1 없음)·잔고에 종목 없음·필드 없음 → 0 (다음 백오프에 재시도).
    int sellable_quantity()
    {
        // 원장 접근자가 주입돼 있으면 그것으로 끝낸다 — 전략 스레드에서 REST를 부르지 않는다. [why D-055]
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
            // 접근자 미주입(단독 실행·테스트) 경로. 공유 전략 스레드에서 동기로 돌므로 재시도만 뗀다 —
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

    // ── KST 시각 헬퍼(서버 TZ 독립: core/KstTime.h) ─────────────────────────
    static struct tm kst_tm()
    {
        return kst::to_tm(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    static int kst_hhmm()
    {
        struct tm local_time = kst_tm();
        return local_time.tm_hour * 100 + local_time.tm_min;
    }

    // 지금이 몇 번째 봉인가(KST). 날짜를 섞어 자정을 넘겨도 값이 겹치지 않게 한다.
    static int kst_bar_bucket(int interval_min)
    {
        if (interval_min < 1)
        {
            interval_min = 1;
        }

        struct tm local_time = kst_tm();
        return (local_time.tm_yday * 1440 + local_time.tm_hour * 60 + local_time.tm_min) / interval_min;
    }

    // 현재 봉이 시작한 뒤 흐른 초(KST). 프리페치 지터 판정용.
    static int kst_sec_into_bucket(int interval_min)
    {
        if (interval_min < 1)
        {
            interval_min = 1;
        }

        struct tm local_time = kst_tm();
        return ((local_time.tm_hour * 60 + local_time.tm_min) % interval_min) * 60 + local_time.tm_sec;
    }

    static std::string kst_ymd()
    {
        return kst::date_yyyymmdd(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    static std::string format_one_decimal(double value)
    {
        char byte_value[32];
        std::snprintf(byte_value, sizeof(byte_value), "%.1f", value);
        return std::string(byte_value);
    }

    struct Live
    {
        std::string order_id;     // 로그용 이름
        uint64_t    order_number; // 취소 키
        OrderSide   side;
    };

    Params parameters_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    std::vector<Live> live_;               // 현재 live로 낙관하는 예약들
    // [inv] on_trade_batch가 스냅샷에서 잡는다. null이면 그 자리에서 return하므로, 아래 판정
    //  함수들이 불리는 시점에는 항상 유효하다.
    std::shared_ptr<const std::vector<MarketData>> daily_; // 이번 평가가 붙잡은 일봉(정배열/눌림 판정)
    double equity_ = 0.0;                   // 사이징 기준 자본(총평가금) 스냅샷 — 일별 갱신
    double last_split_buy_reference_ = 0.0;             // 마지막 재구성의 분할 주문 기준점(SMA 또는 현재가) — 데드밴드 기준
    double last_price_ = 0.0;                 // 직전 체결가 — 시장가 청산 reference_price
    double last_average_price_ = 0.0;             // 잔고 평단(sellable_qty가 갱신) — reference_price 폴백
    int64_t last_warm_log_ms_ = 0;         // 봉 부족 로그 스로틀(60초)
    int    last_position_ = -1;                  // 마지막 재구성 시 포지션(데드밴드 가드)
    int    base_target_quantity_ = 0;            // 베이스 분할 단계를 처음 깔 때의 목표 수량(부분체결 잔량 기준, 보유 0이면 초기화)
    int    peak_position_ = 0;                   // 이번 보유 구간의 최대 보유 수량(목표에 닿았으면 베이스 잔량을 더 깔지 않음)
    double hold_peak_price_ = 0.0;               // 이번 보유 구간의 최고 현재가(무장 후 트레일 기준, 보유 0이면 초기화)
    std::string entry_filter_date_;              // 진입 필터를 판정한 날(KST YYYYMMDD) — 하루 한 번
    bool   day_entry_allowed_ = true;            // 그날 새 진입 허용 여부(진입 필터 판정 결과)
    static constexpr int kOpenDeviationSampleHhmm = 903;   // 개장 봉(3분) 종가 시각 — 이 뒤 첫 평가에서 개장 이격을 잰다
    static constexpr int kPrefetchWindowOpenHhmm  = 850;   // 프리페치 REST를 부르는 창(KST). 장 마감 청산(15:15)까지 덮는다
    static constexpr int kPrefetchWindowCloseHhmm = 1535;
    static constexpr int kAtrPeriod = 14;
    static constexpr int kLiquidationBackoffMs = 30000;    // 손절·트레일·기준선 이탈 청산의 재시도 상한(ms)
    static constexpr int kMarketCloseBackoffMs = 300000;   // 장 마감 청산의 재시도 상한(ms) — 마감까지 계속 민다
    std::string last_split_buy_signal_;          // 마지막 발주 분할 매수 시그니처(no-change 가드)
    std::chrono::steady_clock::time_point last_work_{};   // 스로틀
    std::chrono::steady_clock::time_point last_rebuild_{}; // 마지막 분할 매수 전면 재구성
    bool last_zone_ = false;                              // 마지막 존 상태(변화 로그용)
    bool in_zone_   = false;                              // 존 히스테리시스 상태(진입/청산 임계 전환)
    bool entry_closed_logged_ = false;                    // 진입 축 닫힘 로그를 냈나(도배 방지)
    std::chrono::steady_clock::time_point zone_log_ts_{}; // 마지막 존 판정 로그 시각
    std::chrono::steady_clock::time_point liquidation_next_{};    // 청산 재시도 백오프 해제 시각
    int    liquidation_last_position_    = -1;                          // 직전 청산시도 position(진행 판정)
    int    liquidation_fail_streak_ = 0;                           // 연속 미진행 횟수(백오프 지수)
    std::chrono::steady_clock::time_point stop_cooldown_until_{}; // 스탑·트레일 뒤 분할 매수 재개 시각
    std::chrono::steady_clock::time_point reentry_cooldown_until_{}; // 전량 청산 뒤 새 베이스 재개 시각
    std::chrono::steady_clock::time_point average_query_ts_{};        // 평단 직접 조회 스로틀(60초)
    int    average_position_seen_ = 0;                                     // last_average_price_를 읽었을 때의 보유 수량(바뀌면 다시 읽음)
    int    prefetch_jitter_sec_ = 0;                       // 봉 경계 뒤 분봉 조회 지연(초, 티커 해시)
    uint64_t sequence_ = 0;

    // ── 프리페치(무거운 REST를 공유 전략 스레드 밖으로) ──────────────────────
    prefetch::Pool::TaskId  prefetch_task_ = 0;   // 풀 등록 번호(0=없음). 해제는 stop_prefetch()가 명시(멤버 소멸 순서 앞)
    std::mutex              snap_mutex_;               // 아래 snap_* 보호
    // 스냅샷은 포인터를 바꿔 넘긴다 — 평가마다 일봉 250봉(20KB)을 복사하지 않기 위해서다. 한 번 담은
    //  벡터는 const라 아무도 고치지 않고, 읽는 쪽은 자기 shared_ptr로 수명을 붙잡는다. [why D-071]
    using BarSnapshot = std::shared_ptr<const std::vector<MarketData>>;

    BarSnapshot             snap_daily_;             // 일봉 스냅샷(미수신이면 null)
    std::string             snap_daily_date_;        // 스냅샷 기준일(KST YYYYMMDD)
    double                  snap_equity_ = 0.0;      // 자본 스냅샷(raw, 폴백 미적용)
    BarSnapshot             snap_bars_;              // 3분봉 스냅샷(미수신이면 null)
    int snap_bars_bucket_ = -1;                      // 그 스냅샷을 받은 봉 번호(kst_bar_bucket)
    uint64_t snap_bars_version_ = 0;                 // 받을 때마다 +1 — 전략 스레드가 새 스냅샷만 시드한다

    // ── 틱 집계 봉(bar_source=websocket). 집계기·아래 상태는 전략 스레드만 만진다. [why D-069] ──
    //  기저는 1분이다 — interval_min 봉은 판단 직전 resample이 만든다. keep은 SMA 창을 1분으로 편 길이. [why D-072]
    static bars::BarAggregator::Config aggregator_config(const Params& parameters)
    {
        bars::BarAggregator::Config config;
        config.interval_min = 1;
        config.keep         = (std::max)(64, (parameters.simple_moving_average_period + 2) * (std::max)(1, parameters.interval_min));
        return config;
    }

    bars::BarAggregator aggregator_;
    symbol::SymbolId symbol_id_ = symbol::kNone;     // parameters_.ticker의 id — on_start에서 한 번. 집계기는 이 키로만 찾는다
    bool        websocket_bars_        = false; // bar_source=="ws"
    bool        websocket_live_        = false; // 마지막 틱이 WS 체결 틱이었나(REST 대체 틱이면 REST 봉으로 판단)
    bool        reseed_pending_ = true;  // 출처 전환·날짜 변경 뒤 REST 시드를 한 번 더 받아야 한다
    uint64_t    seeded_version_ = 0;     // 마지막으로 시드한 snap_bars_version_
    std::string aggregator_day_;                // 집계기에 든 봉의 KST 날짜 — 바뀌면 비운다
    std::atomic<bool> seed_wanted_{true}; // 전략 스레드가 프리페치 스레드에 "다음 봉에 REST 시드를 받아 달라"
};
