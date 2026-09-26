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
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// DeviationScaleStrategy — 일봉 존(정배열+눌림) 게이트 + 3분봉 이격도 분할매매(지정가 예약)
//
//  아이디어:
//   • "매매할 자리"는 일봉에서 정한다: 정배열(SMA5>10>20, D-141) AND 현재가가 일봉 SMA20
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
//        샤드 스레드(이 전략을 소유한 샤드)가 체결 틱을 BarAggregator로 1분봉에 모으고 resample로 interval_min 봉을 만든다
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
        bool   cross_guard    = true;  // docs/DECISIONS.md D-006
        int    split_step_count        = 2;     // 밴드 층수
        bool   add_below_simple_moving_average_only = true; // 물타기(매수 밴드)를 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 깐다.
                                          //  true=점진 진입: 활성 시 base만 → 진짜 눌림에서만 평단 낮춤(즉시 10% 만재 방지).
                                          //  false=예전 성격: 활성 즉시 base+물타기 전부 예약(빠른 풀사이즈).
        double pullback_percent   = 2.0;   // 일봉 SMA20 눌림 허용폭(하단,%) — 존 진입 임계(SMA20 아래)
        double entry_upper_percent = 0.0;  // 진입 상단(SMA20 위 허용%). 0=SMA20 이하만(순수 눌림). >0이면 SMA20 위 그만큼까지 진입 허용(완만상승·소폭눌림 포착)
        double zone_hysteresis_percent  = 4.0;   // 존 히스테리시스 밴드(%) — 청산 임계 = pullback + 이 값
        // 정배열 마지막 조건(SMA10>SMA20)의 허용오차. 스캐너 config.align_ma_tol_pct와 같은 값을
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
        //   "ws"(기본)는 소유 샤드 스레드가 체결 틱을 1분봉으로 모아(BarAggregator) resample로 접는다. REST 1분봉은
        //   시드와 폴백으로 남는다 — 틱이 REST 대체 모양이면(WS 폴백·구독 상한 넘침) 그 종목은 REST 봉으로
        //   되돌아간다. [why D-069] [why D-072]
        std::string bar_source = "ws";
        int    market_close_hhmm       = 1515;  // 이 시각(KST HHMM) 이후 전량 취소+청산
        int    interval_min   = 3;     // 집계봉 간격(분)
        int    min_action_ms  = 3000;  // on_trade_batch 판단·발주 스로틀(프리페치 주기는 공용 풀이 정한다)
        int    daily_lookback = 70;    // 일봉 조회 개수(정배열 판정 ≥20, 나머지는 고가 대비 표기 여유)
        std::string account;           // 원장 계좌키(단일계좌는 "")
    };

    explicit DeviationScaleStrategy(Params parameters);

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

    std::string describe() const override;

    // 현재가 하트비트만 필요 → trade_only=true(호가 구독 절약). rest 모드에선 DataThread가 주입.
    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{parameters_.ticker, Market::KR, "", /*trade_only=*/true}};
    }

    // 일봉 이벤트 미사용(자가조회) — 순수가상 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    void on_start() override;

    void on_stop() override;

    void on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out) override;

private:
    // ── on_trade_batch 단계 — 틱 하나의 평가를 순서대로 나눈 것이다. bool을 돌려주는 단계는 true면 이 틱의
    //    평가를 거기서 끝낸다(주문을 이미 냈거나 더 볼 것이 없다). 순서와 조기 종료 조건은 on_trade_batch가 쥔다.
    struct DecisionBars
    {
        std::vector<MarketData> bars;         // 판단 봉(bars[0]=진행 중 봉)
        bool                    local = false; // 체결로 모은 봉이면 true, REST 봉이면 false
    };

    struct ZoneJudgement
    {
        double previous_average_20 = 0.0; // 전일 확정 SMA20(하루 진입 필터가 쓴다)
        double average_20 = 0.0;          // 오늘 현재가를 접은 SMA20
        double deviation20_percent = 0.0; // 방향성 이격(+면 SMA20 위)
        bool   zone = false;              // 진입 게이트
        bool   hold_zone = false;         // 유지 게이트 — false면 보유 청산
    };

    struct SplitStep
    {
        OrderSide side;
        double    price;
        int       quantity;
    };

    struct SplitPlan
    {
        std::vector<SplitStep> steps;
        double base_line = 0.0;         // 분할 매수 기준점(base_on_price면 현재가, 아니면 SMA)
        double base_notional = 0.0;     // 베이스 명목(원)
        double split_step_budget = 0.0; // 물타기 총예산(원)
        double entry_scale_ratio = 1.0; // 국면 매수비율(0.1 단위)
    };

    struct Baseline
    {
        double value;   // 분할 매수 기준선
        bool   warming; // 3분봉이 모자라 일봉 SMA20으로 대신했으면 true
    };

    void feed_bar_aggregator(const TradeData& trade);
    bool close_at_market_end(int hhmm, std::vector<OrderSignal>& out);
    // 일봉 스냅샷이 아직 없으면 nullopt — 프리페치를 기다린다.
    std::optional<DecisionBars> load_decision_bars(const TradeData& trade, double current_price);
    ZoneJudgement judge_zone(double current_price, std::chrono::steady_clock::time_point now);
    bool exit_on_zone_loss(std::vector<OrderSignal>& out, std::chrono::steady_clock::time_point now);
    bool exit_on_protective_rules(double current_price, std::vector<OrderSignal>& out,
                                  std::chrono::steady_clock::time_point now);
    // 기준선을 못 정했거나(봉 부족·SMA 0) 기준선 이탈로 청산했으면 nullopt.
    std::optional<Baseline> resolve_baseline(const std::vector<MarketData>& bars, double d_s20, double current_price,
                                             std::vector<OrderSignal>& out, std::chrono::steady_clock::time_point now);
    // 발주 전 계획. peak_position_·base_target_quantity_·reentry_cooldown_until_을 갱신한다.
    SplitPlan plan_split_steps(int position, double current_price, double simple_moving_average, bool warming,
                               std::chrono::steady_clock::time_point now);
    static std::string plan_signature(const SplitPlan& split_plan, bool entry_on);
    bool clear_dust(const SplitPlan& split_plan, bool entry_on, int position, double current_price,
                    std::vector<OrderSignal>& out, std::chrono::steady_clock::time_point now);
    // 직전 재구성과 같은 계획이거나 데드밴드·최소 간격 안이면 true — 기존 분할 주문을 둔다.
    bool rebuild_suppressed(const std::string& signal, int position, double split_buy_reference);
    std::string entry_context_text(const TradeData& trade, double current_price) const;
    void place_split_steps(const SplitPlan& split_plan, bool entry_on, const std::string& buy_context,
                           std::vector<OrderSignal>& out);

    // ── 지표 (indicators.py 이식, bars[0]=최신) ──────────────────────────────
    // 하루 단위 진입 필터 판정 — 그날 첫 평가(개장 봉이 닫힌 09:03 이후)에서 한 번 정하고 하루 동안 고정한다.
    //  개장 이격은 그 첫 평가의 현재가로 잰다(재기동이 늦으면 그 시각 가격 — 갭 회피가 목적이라 근사로 충분).
    //  09:03 전에는 판정을 못 하므로 진입을 미룬다(그 구간은 봉 부족으로 어차피 안 산다).
    bool day_entry_allowed(double current_price, double previous_sma20, int hhmm);

    static double simple_moving_average_close(const std::vector<MarketData>& bars, int period);

    // 전일까지의 일봉 이동평균에 오늘 현재가를 접어 넣어 돌려준다. 20봉 미만이면 average_20=0인
    //  빈 값이라 호출부가 정배열을 false로 떨어뜨린다(판정 자체를 못 하는 상태).
    //  [why D-005] 일봉 조회가 당일 봉을 자르므로 여기서 오늘을 되살린다.
    // 전일 확정 이동평균. 오늘 현재가를 접지 않아 세션 내내 상수다 — 청산처럼 되돌릴 수
    //  없는 판정이 이쪽을 쓴다. 20봉 미만이면 전 필드 0을 돌려준다(호출자가 average_20>0으로 거른다).
    static quant::moving_average::SimpleMovingAverages daily_simple_moving_averages_previous(const std::vector<MarketData>& daily);

    static quant::moving_average::SimpleMovingAverages daily_simple_moving_averages(const std::vector<MarketData>& daily, double current_price);

    // ── 프리페치: 무거운 REST(3분봉·일봉·잔고)를 샤드 스레드 밖에서 미리 당겨
    //    스냅샷에 적재한다. on_trade_batch는 스냅샷만 읽어(락 짧게) 발주를 판단 → 특정
    //    종목의 느린 REST가 전 전략을 막던 head-output_file-line 블로킹을 없앤다. 발주·매도가능
    //    (sellable_quantity)은 원장 최신성을 위해 동기 유지. 여기서 부르는 KIS 메서드는 전부
    //    읽기전용(get_daily_ohlcv·get_minute_ohlcv·get_balance, 동시호출 감사 완료).
    //  주기(Engine::kPrefetchPeriodMs 3초)와 스레드는 공용 풀이 가진다 — 이 함수는 한 주기에 한 번 불린다. [why D-115]
    void prefetch_once();

    // 등록 해제. 돌아온 뒤에는 prefetch_once()가 실행 중이지도, 다시 불리지도 않는다.
    void stop_prefetch();

    // 사이징 기준 자본(총평가금) 조회. output2 tot_evlu_amt(없으면 nass_amt). 알 수 없으면 0.
    //  프리페치 스레드에서 호출(읽기전용). 폴백(fallback_equity) 적용은 호출측(on_trade_batch, line equity).
    // 총평가금은 계좌 하나의 값이라 종목마다 다시 부를 이유가 없다. 전략 스레드가 종목 수만큼
    //  있어 기동 직후 같은 잔고 조회가 40건 동시에 나갔고, 모의 키 2건/초 한도를 주문까지
    //  끌어내렸다(09-11 09:18 EGW00201). 프로세스 공용으로 하루 한 번만 부르고, 뮤텍스를 조회 동안
    //  잡아 동시 진입도 한 번으로 접는다.
    // 실패(0)도 60초 쿨다운을 둔다 — 성공만 캐시하면 조회가 실패했을 때 프리페치 풀 주기(3초)마다
    //  전 종목이 각자 다시 부르고, 그 부하가 다시 실패를 키운다(09-22: 잔고조회 실패 21건과
    //  초당 한도 재시도 37건이 같이 올랐다). 쿨다운 안에는 0을 돌려 호출측 폴백에 맡긴다.
    double fetch_equity();

    // 명목→수량(주). 가격/명목 유효하지 않으면 0(호출측이 폴백 결정).
    static int quantity_for(double notional, double price);

    // ── KRX 호가단위/격자 절사는 core/TickSize.h(krx::)로 일원화. 얇은 위임만 유지. ──
    static double tick_size(double price) { return krx::tick_size(price); }
    static double round_to_tick(double price, OrderSide side) { return krx::round_to_tick(price, side); }

    std::string next_order_id(const char* tag)
    {
        return id() + ":" + tag + ":" + std::to_string(++sequence_);
    }

    void place(std::vector<OrderSignal>& out, OrderSide side, double price, int quantity,
               const std::string& reason = "");

    // 미체결 전량 취소. 발주가 있었으면 true.
    bool cancel_all(std::vector<OrderSignal>& out);

    OrderSignal make_market_sell(int quantity, const std::string& reason = "");

    // 시장가 청산 신호의 명목 평가 기준가. 직전 체결가 > 잔고 평단 > 최근 3분봉 종가 순.
    double liquidation_reference_price();

    // ── 청산 발주: 매도가능분 클램프 + 지수 백오프 ───────────────────────────
    //  버그 이력: 존이탈/장 마감 청산이 원장 보유수량 전량을 시장가 매도했으나, 예약매도
    //  (미연결/미결제)로 실매도가능분(ord_psbl_qty)이 보유보다 작으면 KIS가 전량 거부
    //  (40240000 "잔고내역 없습니다") → 매 하트비트 무한 재거부 스팸. 실계좌 동일.
    //  대책: (1) 매 시도 원장 사본의 매도가능분으로 클램프(sellable_quantity) → 잠긴 수량
    //  초과분 미발주(과매도·이중주문 위험 0, 브로커 상태 기준이라 체결지연에도 자기교정).
    //  (2) 시도 후 진행(position 감소) 없으면 30·60·120·240·480s(capture 300s) 지수 백오프.
    //  반환: 시장가 매도를 실제로 out에 넣었으면 true.
    //  clamp_sellable=false 면 잔고 조회 없이 position 전량을 낸다 — 라우터의 게이트 클램프·자가정리에 맡긴다(장 마감).
    bool emit_liquidation(std::vector<OrderSignal>& out, int position,
                          std::chrono::steady_clock::time_point now, const std::string& tag,
                          long long max_backoff_ms = 300000, bool clamp_sellable = true);

    // 해당 종목의 매도가능수량. 원장 사본의 매도가능분으로 클램프한다(접근자가 없으면 계좌 클라이언트
    //  account_kis() 잔고 조회로 대체, 그것도 없으면 0). 안전 우선: 확실히 알 수 없으면 0(보류)을 반환해
    //  과매도/이중주문을 유발하지 않는다.
    //  - 대체 경로에서 조회 실패(예외)·잔고에 종목 없음 → 0 (다음 백오프에 재시도).
    int sellable_quantity();

    // ── KST 시각 헬퍼(서버 TZ 독립: core/KstTime.h) ─────────────────────────
    static struct tm kst_tm()
    {
        return kst::to_tm(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    static int kst_hhmm();

    // 지금이 몇 번째 봉인가(KST). 날짜를 섞어 자정을 넘겨도 값이 겹치지 않게 한다.
    static int kst_bar_bucket(int interval_min);

    // 현재 봉이 시작한 뒤 흐른 초(KST). 프리페치 지터 판정용.
    static int kst_sec_into_bucket(int interval_min);

    static std::string kst_ymd()
    {
        return kst::date_yyyymmdd(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    static std::string format_one_decimal(double value);

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

    // ── 프리페치(무거운 REST를 샤드 스레드 밖으로) ──────────────────────
    prefetch::Pool::TaskId  prefetch_task_ = 0;   // 풀 등록 번호(0=없음). 해제는 stop_prefetch()가 명시(멤버 소멸 순서 앞)
    std::mutex              snap_mutex_;               // 아래 snap_* 보호
    // 스냅샷은 포인터를 바꿔 넘긴다 — 평가마다 일봉 250봉(20KB)을 복사하지 않기 위해서다. 한 번 담은
    //  벡터는 const라 아무도 고치지 않고, 읽는 쪽은 자기 shared_ptr로 수명을 붙잡는다. [why D-115]
    using BarSnapshot = std::shared_ptr<const std::vector<MarketData>>;

    BarSnapshot             snap_daily_;             // 일봉 스냅샷(미수신이면 null)
    std::string             snap_daily_date_;        // 스냅샷 기준일(KST YYYYMMDD)
    double                  snap_equity_ = 0.0;      // 자본 스냅샷(raw, 폴백 미적용)
    BarSnapshot             snap_bars_;              // 3분봉 스냅샷(미수신이면 null)
    int snap_bars_bucket_ = -1;                      // 그 스냅샷을 받은 봉 번호(kst_bar_bucket)
    uint64_t snap_bars_version_ = 0;                 // 받을 때마다 +1 — 소유 샤드 스레드가 새 스냅샷만 시드한다

    // ── 틱 집계 봉(bar_source=websocket). 집계기·아래 상태는 소유 샤드 스레드만 만진다(on_stop은 샤드가 이 전략을 놓은 뒤 reap·종료 경로에서 부른다). [why D-069] ──
    //  기저는 1분이다 — interval_min 봉은 판단 직전 resample이 만든다. keep은 SMA 창을 1분으로 편 길이. [why D-072]
    static bars::BarAggregator::Config aggregator_config(const Params& parameters);

    bars::BarAggregator aggregator_;
    symbol::SymbolId symbol_id_ = symbol::kNone;     // parameters_.ticker의 id — on_start에서 한 번. 집계기는 이 키로만 찾는다
    bool        websocket_bars_        = false; // bar_source=="ws"
    bool        websocket_live_        = false; // 마지막 틱이 WS 체결 틱이었나(REST 대체 틱이면 REST 봉으로 판단)
    bool        reseed_pending_ = true;  // 출처 전환·날짜 변경 뒤 REST 시드를 한 번 더 받아야 한다
    uint64_t    seeded_version_ = 0;     // 마지막으로 시드한 snap_bars_version_
    std::string aggregator_day_;                // 집계기에 든 봉의 KST 날짜 — 바뀌면 비운다
    std::atomic<bool> seed_wanted_{true}; // 샤드 스레드가 프리페치 스레드에 "다음 봉에 REST 시드를 받아 달라"
};
