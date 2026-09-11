#pragma once
#include "api/KisClient.h"
#include "core/TickSize.h"
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// DeviationScaleStrategy — 일봉 존(정배열+눌림) 게이트 + 3분봉 이격도 분할매매(지정가 예약)
//
//  아이디어:
//   • "매매할 자리"는 일봉에서 정한다: 정배열(SMA5>10>20>60) AND 현재가가 일봉 SMA20
//     대비 이격 밴드 안. 밴드는 슬리브가 정한다 — 눌림(DEVSCALE)은 −pullback_pct~+entry_upper_pct,
//     추세확장(TRENDX)은 +entry_lower_pct~+entry_upper_pct(SMA20 위 구간). 존 유지는 진입보다
//     zone_hyst_pct만큼 넓다. 이 조건이 참일 때만 오실레이션을 켠다(존 활성).
//   • 자리 안에서는 3분봉 SMA를 기준선으로 삼아:
//       - 존 진입 시 무포지션이면 목표수량 절반(base_qty)을 기준선 근처 지정가로 베이스 매수.
//       - 이격도가 위로 벌어지는 지점(+dev_sell%·n_rungs층)에 지정가 매도(분할 익절).
//       - 평균으로 되돌아오는 지점(−dev_buy%·buy_rungs층)에 지정가 매수(재진입). buy_rungs=0이면
//         되돌림 매수(물타기)를 깔지 않는다 — 추세확장 슬리브의 기본이다.
//   • 청산: 존 이탈(유지 게이트) · 평단 대비 stop_loss_pct 하드 스탑 · 3분봉 기준선 이탈
//     트레일(옵션) · eod_hhmm 장 마감. 스탑·트레일 뒤에는 stop_cooldown_sec 동안 재진입을 막는다.
//   • 시장가가 아니라 지정가 예약을 미리 걸어 "기다리는" 매매. 3분봉이 갱신되거나 SMA가
//     reprice_move_ticks 이상 이동하면 미체결 분할 매수를 CANCEL+NEW로 재호가(MM-1 패턴).
//
//  구동: rest_price_feed 모드에서 DataThread가 매 사이클 현재가를 TradeData로 주입 →
//        on_trade_batch가 하트비트로 호출된다(WS 불필요). 3분봉/일봉은 kis_로 자가조회.
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
        // ── 명목 사이징(자본%) — 우선. 자본 스냅샷(총평가금)×pct 를 가격으로 나눠 수량 산출.
        //    베이스=자본×base_pct, 물타기 총예산=자본×(max_pct−base_pct)를 n_rungs로 분할.
        //    자본을 알 수 없고(조회 실패) fallback_equity 도 0이면 아래 base_qty/step_qty 로 폴백.
        double base_pct       = 0.05;  // 존 진입 베이스 명목 = 자본의 5%
        double max_pct        = 0.10;  // 종목당 상한 명목 = 자본의 10%(물타기 포함)
        // 종목별 비중 배수 — 유니버스 스캐너의 종합 점수(추세·눌림·변동성)에서 나온다.
        //  베이스와 물타기 예산에 함께 곱해 종목당 명목 전체를 스케일한다. 1.0이면 균등(기본값)
        //  이라 배선 전 동작이 바뀌지 않는다. 등록 시점에 고정하고 재스캔이 갱신하지 않는다 —
        //  이미 포지션이 있는 종목의 분할 매수를 도중에 재산정하면 잔여 물타기 예산이 평단과 어긋난다.
        double size_mult      = 1.0;
        // 종목당 명목 총액(원). 0보다 크면 자본%·size_mult 대신 이 금액을 베이스+물타기로 나눈다
        //  (비율은 base_pct:max_pct 그대로). 점수 z를 바닥~천장 원 구간에 대응시킨 값이 들어온다.
        //  [why D-036] 자본%×정규화 배수는 종목당 20만~60만원이 나와 자본이 아니라 슬롯이 먼저 마른다.
        double notional_krw   = 0.0;
        double fallback_equity = 0.0;  // 잔고조회 실패 시 사용할 기준자본(원). 0이면 주수 폴백
        int    base_qty       = 10;    // (폴백) 존 진입 베이스 매수 수량
        int    step_qty       = 5;     // (폴백) 각 밴드(rung) 분할 수량
        int    sma_period     = 20;    // 3분봉 기준선 SMA 기간
        double dev_sell       = 1.5;   // 매도 밴드 이격도(%) — 층당 배수
        double dev_buy        = 0.8;   // 매수 밴드 이격도(%) — 층당 배수
        // 교차 가드: 분할 매수 각 층이 현재가를 넘어가지 않도록 앵커를 현재가 쪽으로 클램프한다.
        //  기준선이 현재가에서 멀어지면 한쪽 층 전체가 현재가를 넘어가 지정가가 아니라 즉시
        //  체결되는 시장가가 된다(매수는 위, 매도는 아래). 분할 매수의 전제가 깨진다.
        //  해당 층을 '건너뛰지' 않는다 — 건너뛰면 눌림 진입이나 익절이 통째로 사라진다.
        //  매도는 max(sma,현재가), 매수는 min(sma,현재가) 기준으로 층을 다시 깐다.
        bool   cross_guard    = true;  // docs/DECISIONS.md D-006
        int    n_rungs        = 2;     // 밴드 층수
        bool   add_below_sma_only = true; // 물타기(매수 밴드)를 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 깐다.
                                          //  true=점진 진입: 활성 시 base만 → 진짜 눌림에서만 평단 낮춤(즉시 10% 만재 방지).
                                          //  false=예전 성격: 활성 즉시 base+물타기 전부 예약(빠른 풀사이즈).
        double pullback_pct   = 2.0;   // 일봉 SMA20 눌림 허용폭(하단,%) — 존 진입 임계(SMA20 아래)
        double entry_upper_pct = 0.0;  // 진입 상단(SMA20 위 허용%). 0=SMA20 이하만(순수 눌림). >0이면 SMA20 위 그만큼까지 진입 허용(완만상승·소폭눌림 포착)
        double zone_hyst_pct  = 4.0;   // 존 히스테리시스 밴드(%) — 청산 임계 = pullback + 이 값
        // 정배열 마지막 조건(SMA20>SMA60)의 허용오차. 스캐너 cfg.align_ma_tol_pct와 같은 값을
        //  받아야 "등록은 됐는데 활성은 안 되는" 슬롯이 생기지 않는다.
        double align_ma_tol_pct = 0.0;
        // 개장 직후 3분봉이 sma_period만큼 안 쌓인 구간(20봉×3분=60분)에서 분할 매수 기준선을
        //  일봉 SMA20으로 대신한다. 그 구간에도 일봉 존 게이트(정배열+눌림)는 이미 통과한 상태라
        //  판단 근거가 없는 게 아니라 기준선 하나가 없을 뿐이다. 기준선이 현재가에서 멀어도
        //  cross_guard가 앵커를 현재가로 클램프하고, 워밍업 중에는 add_below_sma_only 값과
        //  무관하게 물타기 분할 매수를 잠근다(분할 주문 구성부 `!warming`). 베이스 매수·익절 매도는 나간다.
        bool   daily_basis_warmup = true;
        int    reprice_move_ticks = 2; // SMA가 이만큼(틱) 이동하면 재호가
        int    min_rebuild_sec = 0;    // 분할 매수 전면 재구성 최소 간격(0=제한 없음). 첫 구성 뒤부터 적용
        // ── TRENDX(추세확장) 슬리브용. 전부 기본값이 기존 눌림 동작이다. ──
        //  id_prefix: 같은 클래스를 두 슬리브로 돌릴 때 전략 id를 가른다(regime_strategies
        //   매칭 키이자 로그 식별자). 유니버스가 이격 밴드로 상호배타라 티커는 겹치지 않는다.
        std::string id_prefix = "DEVSCALE";
        //  entry_lower_pct: >0이면 존 하단을 SMA20 "위" 그 지점으로 올린다. 눌림 슬리브가
        //   버리는 이격 +5% 초과 구간을 이 슬리브가 받는다(0=기존 -pullback_pct 하단).
        double entry_lower_pct = 0.0;
        //  anchor_on_price: 분할 매수 앵커를 SMA20이 아니라 현재가로 잡는다. 이격이 벌어진
        //   종목은 SMA20 앵커로 깔면 매수층이 시장가에서 5~30% 아래에 놓여 영원히 안 붙는다.
        bool   anchor_on_price = false;
        //  buy_rungs: 되돌림 매수(물타기) 층수. -1이면 n_rungs와 같다(기존 동작), 0이면 베이스
        //   매수만 내고 하방 분할 매수를 깔지 않는다. 방향성 이격 게이트에서 하방 분할 매수는 추세
        //   반전에 그대로 노출된다 — 09-08~11 원장에서 매수 수량의 89%가 미청산으로 남았다.
        int    buy_rungs = -1;
        //  stop_loss_pct: >0이면 잔고 평단 대비 이만큼(%) 아래에서 미체결 취소+시장가 청산.
        //   [inv] 기준은 진입봉 저가가 아니라 평단이다 — 재기동해도 잔고 조회로 되살아나는 값이다.
        double stop_loss_pct = 0.0;
        //  trail_sma_exit: 보유 중 현재가가 3분봉 기준선의 trail_sma_tol_pct(%) 아래로 내려오면
        //   청산한다. 워밍업(기준선=일봉 SMA20) 구간에는 보지 않는다.
        bool   trail_sma_exit = false;
        double trail_sma_tol_pct = 1.0;
        //  stop_cooldown_sec: 스탑·트레일 청산 뒤 이 시간 동안 분할 매수를 깔지 않는다. 존이
        //   그대로 열려 있으면 다음 하트비트에 베이스 매수가 도로 나가 같은 자리를 되산다.
        int    stop_cooldown_sec = 900;
        //  sell_anchor_avg: 분할 익절 앵커를 현재가가 아니라 잔고 평단으로 둔다. 현재가 앵커는
        //   목표가가 값을 따라 올라가 8초 안의 급등에서만 붙는다. 평단 기준이면 +dev_sell%가
        //   진입 대비 익절이 된다. 목표가가 이미 현재가 아래면 현재가에 지정가를 낸다.
        bool   sell_anchor_avg = false;
        //  prefetch_jitter_pct: 봉 경계 직후 분봉 조회를 종목별로 흩는다(봉 길이의 0~이 비율,
        //   티커 해시로 고정). 50종목이 같은 초에 조회하면 초당 한도(20)에 걸려 뒤쪽이 HTTP 500이다.
        int    prefetch_jitter_pct = 50;
        int    eod_hhmm       = 1515;  // 이 시각(KST HHMM) 이후 전량 취소+청산
        int    interval_min   = 3;     // 집계봉 간격(분)
        int    min_action_ms  = 3000;  // on_trade_batch 판단·발주 스로틀 겸 프리페치 루프 주기(분봉 REST는 프리페치 스레드가 당긴다)
        int    daily_lookback = 70;    // 일봉 조회 개수(SMA60 판정 위해 ≥60)
        std::string account;           // 원장 계좌키(단일계좌는 "")
    };

    explicit DeviationScaleStrategy(Params p) : p_(std::move(p))
    {
        if (p_.n_rungs < 1)
        {
            p_.n_rungs = 1;
        }

        if (p_.buy_rungs < 0)
        {
            p_.buy_rungs = p_.n_rungs;
        }

        // [formula] 지터 = hash(티커) mod (봉 길이 × 비율). 같은 종목은 매번 같은 지연을 받아
        //  조회 순서가 재현된다.
        const int jit_pct = (std::max)(0, (std::min)(100, p_.prefetch_jitter_pct));
        const int span    = (std::max)(1, p_.interval_min * 60 * jit_pct / 100);
        prefetch_jitter_sec_ = static_cast<int>(std::hash<std::string>{}(p_.ticker) % static_cast<size_t>(span));

        if (p_.sma_period < 2)
        {
            p_.sma_period = 2;
        }

        if (p_.interval_min < 1)
        {
            p_.interval_min = 1;
        }
    }

    // 프리페치 스레드·스냅샷 뮤텍스를 안고 있다 — 복사 대상이 아니다.
    DeviationScaleStrategy(const DeviationScaleStrategy&)            = delete;
    DeviationScaleStrategy& operator=(const DeviationScaleStrategy&) = delete;

    ~DeviationScaleStrategy() { stop_prefetch(); }

    std::string id() const override { return p_.id_prefix + "_" + p_.ticker; }

    // 로그 표시용 "티커(종목명)". 이름 없으면 티커만. id()·데이터키와는 분리.
    std::string disp() const
    {
        return p_.name.empty() ? p_.ticker : p_.ticker + "(" + p_.name + ")";
    }

    std::string describe() const override
    {
        // 사이징은 자본×base_pct(또는 notional_krw) 우선, base_qty/step_qty는 폴백이다.
        return "DeviationScale | " + disp() + " | base_pct=" + fmt1(p_.base_pct * 100.0) +
               "% max_pct=" + fmt1(p_.max_pct * 100.0) + "% (fallback qty " + std::to_string(p_.base_qty) +
               "/" + std::to_string(p_.step_qty) + ") sma=" + std::to_string(p_.sma_period) +
               "(" + std::to_string(p_.interval_min) + "m) dev_sell=" + fmt1(p_.dev_sell) +
               "% dev_buy=" + fmt1(p_.dev_buy) + "% rungs=" + std::to_string(p_.n_rungs) +
               "/buy" + std::to_string(p_.buy_rungs) + " zone=" + fmt1(-p_.pullback_pct) +
               "~+" + fmt1(p_.entry_upper_pct) + "%" +
               (p_.entry_lower_pct > 0.0 ? " lower=+" + fmt1(p_.entry_lower_pct) + "%" : "") +
               (p_.stop_loss_pct > 0.0 ? " stop=-" + fmt1(p_.stop_loss_pct) + "%" : "") +
               (p_.trail_sma_exit ? " trail" : "") + (p_.sell_anchor_avg ? " sell@avg" : "");
    }

    // 현재가 하트비트만 필요 → trade_only=true(호가 구독 절약). rest 모드에선 DataThread가 주입.
    std::vector<WatchSpec> get_watch_specs() const override
    {
        return {{p_.ticker, Market::KR, "", /*trade_only=*/true}};
    }

    // 일봉 이벤트 미사용(자가조회) — 순수가상 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    void on_start() override
    {
        live_.clear();
        last_anchor_ = 0.0;
        last_pos_ = -1;
        last_rebuild_ = std::chrono::steady_clock::time_point{};
        last_ladder_sig_.clear();
        in_zone_ = false;
        entry_closed_logged_ = false;
        liq_next_ = std::chrono::steady_clock::time_point{};
        liq_last_pos_ = -1;
        liq_fail_streak_ = 0;
        last_work_ = std::chrono::steady_clock::time_point{};
        daily_.clear();
        equity_ = 0.0;
        seq_ = 0;
        last_zone_ = false;
        zone_log_ts_ = std::chrono::steady_clock::time_point{};
        last_warm_log_ms_ = 0;
        last_px_ = 0.0;
        last_avg_px_ = 0.0;
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            snap_daily_.clear();
            snap_daily_date_.clear();
            snap_equity_ = 0.0;
            snap_bars_.clear();
            snap_bars_bucket_ = -1;
        }

        LOG_INFO("[" + id() + "] 시작 — " + describe());
        // set_kis()가 on_start 직전 호출됨(Engine start/재스캔 둘 다) → kis_ 확정. 여기서 프리페치 기동.
        stop_prefetch(); // joinable 스레드에 재대입하면 std::terminate — 재등록 경로 대비
        prefetch_stop_.store(false, std::memory_order_relaxed);
        prefetch_thread_ = std::thread([this] { prefetch_loop(); });
    }

    void on_stop() override
    {
        stop_prefetch();
        LOG_INFO("[" + id() + "] 종료");
    }

    void on_trade_batch(const TradeData& td, std::vector<OrderSignal>& out) override
    {
        if (td.ticker != p_.ticker)
        {
            return;
        }

        if (td.price > 0.0)
        {
            last_px_ = td.price; // 시장가 청산의 명목 평가 기준가(ref_price). 장 마감 경로보다 먼저 갱신
        }

        const int hhmm = kst_hhmm();

        // ── 장 마감 안전장치: 전량 취소 + 시장가 청산 ────────────────────────────
        if (hhmm >= p_.eod_hhmm)
        {
            bool cancelled = cancel_all(out);
            int pos = confirmed_position(p_.account, p_.ticker);
            const std::string tag = "장 마감(" + std::to_string(hhmm) + ")";
            emit_liquidation(out, pos, std::chrono::steady_clock::now(), tag); // 클램프+백오프(자체 로깅)

            if (cancelled && pos <= 0)
            {
                LOG_INFO("[" + id() + "] " + tag + " — 미체결 취소(보유 0)");
            }

            return;
        }

        // ── 무거운 작업 스로틀(3분봉 조회·발주) ──────────────────────────────
        const auto now = std::chrono::steady_clock::now();

        if (last_work_.time_since_epoch().count() != 0 &&
            now - last_work_ < std::chrono::milliseconds(p_.min_action_ms))
        {
            return;
        }

        last_work_ = now;

        const double cur_px = td.price;

        if (cur_px <= 0.0)
        {
            return;
        }

        // ── 프리페치 스냅샷 스냅(일봉·자본·3분봉). 아직 준비 전이면 다음 하트비트 대기 ──
        //  무거운 REST는 프리페치 스레드가 미리 당겨둔다. 여기선 락을 짧게 잡고 복사만.
        std::vector<MarketData> bars;
        int bars_bucket = -1;
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);

            if (snap_daily_.empty())
            {
                return; // 일봉 미준비 — 프리페치 대기
            }

            daily_  = snap_daily_;
            equity_ = snap_equity_;
            bars    = snap_bars_;
            bars_bucket = snap_bars_bucket_;
        }

        // 진행 중인 봉(bars[0])의 종가를 방금 들어온 체결가로 덮는다. 프리페치가 봉 주기당
        //  한 번만 받으므로 그 사이의 가격 변화는 이 한 줄이 반영한다. 봉이 이미 넘어갔는데
        //  프리페치가 아직 안 왔으면(bucket 불일치) 덮지 않는다 — 마감된 봉의 종가를 고칠 순 없다.
        if (!bars.empty() && bars_bucket == kst_bar_bucket(p_.interval_min))
        {
            bars[0].close = cur_px;
        }

        // ── 일봉 존 판정(정배열 + SMA20 눌림) ────────────────────────────────
        // 오늘 현재가를 이동평균에 접어 넣는다. 접지 않으면 정배열도 SMA20도 하루 종일
        //  전일 값이라, 장중에 이평이 깨져도 존은 활성으로 남고 이격만 움직인다.
        //  스캐너(UniverseScanner)와 같은 식·같은 허용오차를 쓴다(MaAlign.h).
        const quant::ma::Smas d_prev = daily_smas_prev(daily_);
        const quant::ma::Smas d_ma   = daily_smas(daily_, cur_px);
        // 축이 둘이다. 접은 정배열은 진입만 연다. 유지·청산은 전일 확정 정배열로 판정한다.
        //  접은 값은 min_action_ms(3초)마다 뒤집힐 수 있는데 존 이탈에 붙은 행위가 보유 전량
        //  시장가 매도다. 09-10 일봉 캐시(정배열 통과 128종목)로 재면 s5>s10이 73%에서 가장
        //  먼저 깨지고, 정배열이 무너지는 장중 하락폭 5분위가 1.45%다 — 유니버스의 약 10%가
        //  매일 "2~3% 밀리면 전량 매도, 되돌아오면 재매수"가 된다. 왕복마다 수수료·세금·
        //  슬리피지가 실현손실로 남고, 지수 게이트에서 방금 없앤 떨림을 종목 단위로 되살린다.
        //  청산은 되돌릴 수 없으니 느린 축에 맡긴다. [why D-033]
        const bool   aligned      = d_ma.s60 > 0.0 && quant::ma::aligned(d_ma, p_.align_ma_tol_pct);
        const bool   aligned_hold = d_prev.s60 > 0.0 && quant::ma::aligned(d_prev, p_.align_ma_tol_pct);
        const double d_s20   = d_ma.s20;
        // 방향성 이격(부호 유지): +면 SMA20 위(확장추격), −면 아래(눌림). 절대값 금지.
        //  SMA20 미확보(≤0) 시 큰 양수 센티넬로 둬 존 상단 밖으로 밀어내 진입을 막는다.
        constexpr double kNoDataDeviationPct = 999.0;
        const double s_dev   = d_s20 > 0.0 ? (cur_px - d_s20) / d_s20 * 100.0 : kNoDataDeviationPct;
        // 방향성 눌림 게이트: 진입은 "SMA20 이하(≤0%) ~ pullback_pct 아래"의 눌림 구간에서만.
        //   정배열 상승추세에서 SMA20 눌림(entry_upper_pct=0) 또는 SMA20 위 소폭(entry_upper_pct>0)까지 진입 허용.
        //   히스테리시스: 활성이면 상단 +zone_hyst 더 여유, 하단 −(pullback+zone_hyst)까지 유지(경계 진동 방지).
        const double up_th   = p_.entry_upper_pct + (in_zone_ ? p_.zone_hyst_pct : 0.0); // 진입 상단=entry_upper, 유지=+hyst
        const double down_th = in_zone_ ? p_.pullback_pct + p_.zone_hyst_pct // 유지 하단
                                        : p_.pullback_pct;                   // 진입 하단
        // 존 하단: 기본은 SMA20 아래 -down_th(눌림). entry_lower_pct>0인 추세확장 슬리브는
        //  하단을 SMA20 위로 올려, 눌림 슬리브의 상단과 맞물리되 겹치지 않게 한다.
        const double low_th  = p_.entry_lower_pct > 0.0
                                   ? p_.entry_lower_pct - (in_zone_ ? p_.zone_hyst_pct : 0.0)
                                   : -down_th;
        const bool   band    = d_s20 > 0.0 && s_dev <= up_th && s_dev >= low_th;
        const bool   zone    = aligned && band;          // 진입 게이트
        // [inv] hold_zone은 zone보다 넓다(정배열 축이 느린 쪽). 좁아지면 청산이 진입보다 먼저 돈다.
        const bool   hold_zone = aligned_hold && band;   // 유지 게이트 — 청산 판정
        in_zone_ = zone;

        // 존 판정 로그: 상태 변화 시 또는 60초마다 1회.
        if (zone != last_zone_ || zone_log_ts_.time_since_epoch().count() == 0 ||
            now - zone_log_ts_ >= std::chrono::seconds(60))
        {
            LOG_INFO("[" + id() + "] " + disp() + " 존 판정 " + std::string(zone ? "활성" : "대기") +
                     " | 정배열=" + std::string(aligned ? "Y" : "N") +
                     " 일봉SMA20=" + fmt1(d_s20) + " 현재가=" + fmt1(cur_px) +
                     " 이격=" + fmt1(s_dev) + "% (진입밴드 " + fmt1(low_th) + "%~" + fmt1(up_th) +
                     "%) 유지=" + std::string(hold_zone ? "Y" : "N") +
                     " 일봉수=" + std::to_string(daily_.size()));
            last_zone_    = zone;
            zone_log_ts_  = now;
        }

        if (!hold_zone)
        {
            // 존 이탈 → 미체결 전부 취소 + 보유분 시장가 청산(매도가능분 클램프+백오프).
            bool cancelled = cancel_all(out);
            int pos = confirmed_position(p_.account, p_.ticker);
            emit_liquidation(out, pos, now, "존 이탈"); // 클램프+백오프(자체 로깅)

            if (cancelled && pos <= 0)
            {
                LOG_INFO("[" + id() + "] 존 이탈 — 미체결 취소(보유 0)");
            }

            return;
        }

        // ── 하드 스탑: 평단 대비 stop_loss_pct 아래면 존 상태와 무관하게 청산 ─────────
        //  유지 게이트가 진입보다 넓어(히스테리시스) 존 안에서도 평단에서 크게 밀릴 수 있다.
        //  평단은 sellable_qty()가 잔고 조회 때 채운다. 재기동 직후 첫 재구성 전에는 0이라
        //  보유가 있으면 60초에 한 번 직접 채운다(REST 1회).
        if (p_.stop_loss_pct > 0.0)
        {
            const int pos = confirmed_position(p_.account, p_.ticker);

            if (pos > 0 && last_avg_px_ <= 0.0 &&
                (avg_query_ts_ == std::chrono::steady_clock::time_point{} ||
                 now - avg_query_ts_ >= std::chrono::seconds(60)))
            {
                avg_query_ts_ = now;
                (void)sellable_qty();
            }

            if (pos > 0 && last_avg_px_ > 0.0 &&
                cur_px <= last_avg_px_ * (1.0 - p_.stop_loss_pct / 100.0))
            {
                cancel_all(out);
                const std::string tag = "손절(평단 " + fmt1(last_avg_px_) + " -" + fmt1(p_.stop_loss_pct) + "%)";
                // 스탑은 지수 백오프 상한을 30초로 둔다 — 5분 보류는 손절이 아니다.
                emit_liquidation(out, pos, now, tag, /*max_backoff_ms=*/30000);
                stop_cooldown_until_ = now + std::chrono::seconds(p_.stop_cooldown_sec);
                return;
            }
        }

        if (!zone)
        {
            // 진입 축만 닫혔다. 미체결 매수는 거두되 보유는 그대로 둔다 — 장중에 이평이
            //  깨졌다고 파는 대신 되돌아오면 그대로 이어간다. 청산은 위 hold_zone이 맡는다.
            if (cancel_all(out) && !entry_closed_logged_)
            {
                LOG_INFO("[" + id() + "] " + disp() +
                         " 진입 축 닫힘(장중 정배열) — 미체결 취소, 보유 유지");
                entry_closed_logged_ = true;
            }

            return;
        }

        entry_closed_logged_ = false;

        // ── 3분봉 기준선(스냅샷에서 이미 받음) ───────────────────────────────
        const bool warming = static_cast<int>(bars.size()) < p_.sma_period;

        if (warming && !p_.daily_basis_warmup)
        {
            // 개장 직후엔 3분봉이 sma_period(20봉=60분)만큼 쌓이지 않아 여기서 매번 되돌아간다.
            //  로그가 없으면 '존 활성인데 주문 0건'이 원인 불명으로 보인다(2026-09-07 실제 발생).
            //  60초에 한 번만 남겨 개장 구간 로그가 넘치지 않게 한다.
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()).count();

            if (now_ms - last_warm_log_ms_ >= 60000)
            {
                last_warm_log_ms_ = now_ms;
                LOG_INFO("[" + id() + "] 봉 부족 — 대기 " + std::to_string(bars.size()) + "/" +
                         std::to_string(p_.sma_period) + "봉(" + std::to_string(p_.interval_min) +
                         "분) 현재가=" + fmt1(cur_px));
            }

            return;
        }

        // 워밍업 구간에는 일봉 SMA20(존 게이트가 이미 쓴 값)을 기준선으로 대신 쓴다.
        const double sma = warming ? d_s20 : sma_close(bars, p_.sma_period); // bars[0]=최신

        if (sma <= 0.0)
        {
            return;
        }

        // ── 트레일: 3분봉 기준선 아래로 tol만큼 내려오면 청산(워밍업 제외) ────────────
        if (p_.trail_sma_exit && !warming &&
            cur_px < sma * (1.0 - p_.trail_sma_tol_pct / 100.0))
        {
            const int pos = confirmed_position(p_.account, p_.ticker);

            if (pos > 0)
            {
                cancel_all(out);
                emit_liquidation(out, pos, now, "3분봉 기준선 이탈(" + fmt1(sma) + ")", /*max_backoff_ms=*/30000);
                stop_cooldown_until_ = now + std::chrono::seconds(p_.stop_cooldown_sec);
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
                         std::to_string(p_.sma_period) + "봉, 일봉SMA20=" + fmt1(d_s20) +
                         " 현재가=" + fmt1(cur_px));
            }
        }

        int pos = confirmed_position(p_.account, p_.ticker);

        // ── 목표 분할 매수 산출(발주 전) ─────────────────────────────────────────
        //  계단 가격·수량은 sma·pos의 순수 함수. 먼저 계획을 만들고 직전 분할 매수와
        //  시그니처를 비교해 "동일하면 재발주 스킵". 분봉 정지(HTTP 500 폴백)로
        //  sma=px가 고정될 때 동일 분할 매수를 취소·재발주하던 처닝을 차단.
        struct Rung { OrderSide side; double price; int qty; };
        std::vector<Rung> plan;

        // ── 명목 사이징: 자본%를 가격으로 나눠 수량 산출(자본 미상이면 주수 폴백) ──
        //  베이스=자본×base_pct(5%), 물타기 총예산=자본×(max_pct−base_pct)(5%)를 n_rungs로 분할.
        //  베이스+물타기 합 ≈ 자본×max_pct(10%) → OrderGate 명목캡과 정합(캡은 백스톱).
        const double eq            = equity_ > 0.0 ? equity_ : p_.fallback_equity;
        const double mult          = p_.size_mult > 0.0 ? p_.size_mult : 1.0;
        // 원 단위 총액이 주어지면 그 금액을 base_pct:max_pct 비율로 베이스·물타기에 나눈다.
        const double base_share    = p_.max_pct > p_.base_pct ? p_.base_pct / p_.max_pct : 1.0;
        const double base_notional = p_.notional_krw > 0.0 ? p_.notional_krw * base_share
                                                            : eq * p_.base_pct * mult;
        const double rung_budget   = p_.notional_krw > 0.0 ? p_.notional_krw - base_notional
                                                            : eq * (p_.max_pct > p_.base_pct ? p_.max_pct - p_.base_pct : 0.0) * mult;
        const double rung_notional = p_.buy_rungs > 0 ? rung_budget / p_.buy_rungs : 0.0;

        // 분할 매수 앵커. 교차 가드가 켜져 있으면 각 방향 층이 현재가를 넘지 않도록 기준선을
        //  현재가 쪽으로 당긴다. 이격이 벌어진 상태에서도 분할 매수 간격은 그대로 유지된다.
        //  anchor_on_price면 기준선을 현재가로 둔다. 이격 +5~30% 구간에서 SMA20을 앵커로
        //   쓰면 매수층 전부가 시장가에서 그만큼 아래에 깔려 하루 종일 한 주도 안 붙는다.
        //   추세 슬리브는 "지금 값에서 한 호가 아래"로 붙어야 추세에 올라탄다.
        const bool   guard_on   = p_.cross_guard && cur_px > 0.0;
        const double base_line  = (p_.anchor_on_price && cur_px > 0.0) ? cur_px : sma;
        const double sell_anchor = guard_on ? (std::max)(base_line, cur_px) : base_line;
        const double buy_anchor  = guard_on ? (std::min)(base_line, cur_px) : base_line;

        // 베이스: 무포지션이면 기준선 근처 지정가 매수(자본의 base_pct).
        if (pos <= 0)
        {
            double bp = round_to_tick(base_line, OrderSide::BUY);

            // 교차 가드: 기준선이 현재가 이상이면 이 지정가는 즉시 시장가로 체결된다.
            //  베이스를 건너뛰면 add_below_sma_only가 노리는 눌림 진입에서 가장 큰 레그가
            //  빠지므로, 억제 대신 현재가 한 틱 아래로 옮겨 지정가로 남긴다.
            if (p_.cross_guard && cur_px > 0.0 && bp >= cur_px)
            {
                bp = round_to_tick(cur_px - krx::tick_size(cur_px), OrderSide::BUY);
            }

            int    bq = qty_for(base_notional, bp);

            if (bq <= 0)
            {
                bq = p_.base_qty;  // 자본 미상 폴백
            }

            if (bp > 0.0 && bq > 0)
            {
                plan.push_back({OrderSide::BUY, bp, bq});
            }
        }

        // 매도 밴드: 이격 +dev_sell%*i. 보유분을 n_rungs로 균등 분할(숏 방지).
        //  sell_anchor_avg면 앵커가 평단이다. 목표가가 현재가 아래면(이미 목표 초과) 현재가에
        //  낸다 — 지정가로 남되 다음 체결에 붙는다.
        int sell_avail = pos;
        const int sell_per = p_.n_rungs > 0 ? (pos + p_.n_rungs - 1) / p_.n_rungs : pos; // ceil
        const bool   avg_anchor = p_.sell_anchor_avg && last_avg_px_ > 0.0;
        const double sell_base  = avg_anchor ? last_avg_px_ : sell_anchor;

        for (int i = 1; i <= p_.n_rungs && sell_avail > 0; ++i)
        {
            double sp = round_to_tick(sell_base * (1.0 + p_.dev_sell * i / 100.0), OrderSide::SELL);

            if (avg_anchor && cur_px > 0.0 && sp <= cur_px)
            {
                sp = round_to_tick(cur_px, OrderSide::SELL);
            }

            int q = sell_avail < sell_per ? sell_avail : sell_per;

            if (sp > 0.0 && q > 0)
            {
                plan.push_back({OrderSide::SELL, sp, q});
                sell_avail -= q;
            }
        }

        // 매수 밴드(물타기): 이격 −dev_buy%*i, buy_rungs층. rung당 자본의 rung_notional. 종목당 상한은 OrderGate가 캡.
        //  점진 진입: add_below_sma_only면 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 물타기를 깐다.
        //  → 활성 즉시 base+물타기를 한꺼번에 예약해 1분 만에 10% 만재되던 성격을 제거. 기준선 위/근처에선
        //    base(+익절 매도레그)만 유지하고, 진짜 눌림이 와야 평단을 낮춘다.
        //  추세확장 슬리브(anchor_on_price)는 add_below_sma_only=false로 돌린다 — 이격이 벌어진
        //   구간에서 "기준선 아래"는 거의 안 오므로 켜 두면 분할 매수가 영영 안 깔린다. 그 슬리브의
        //   하방 분할 매수 자체는 buy_rungs=0으로 끈다(2026-09-11 회의 §1-4).
        // 워밍업(기준선=일봉SMA20) 구간에는 물타기를 잠근다. 존 진입 조건이 이격 -pullback_pct~
        //  +entry_upper_pct라 `cur_px < 일봉SMA20`이 거의 항상 참이 되어, 3분봉 기준선이 뜻하던
        //  "단기 눌림에서만 추가"가 사실상 상시 개방으로 바뀐다. 변동성이 가장 큰 첫 60분에
        //  base와 물타기가 한꺼번에 나가는 것을 막는다(base 진입과 익절 매도는 그대로 둔다).
        if ((!p_.add_below_sma_only || cur_px < sma) && !warming)
        {
            for (int i = 1; i <= p_.buy_rungs; ++i)
            {
                double bp = round_to_tick(buy_anchor * (1.0 - p_.dev_buy * i / 100.0), OrderSide::BUY);
                int    rq = qty_for(rung_notional, bp);

                if (rq <= 0)
                {
                    rq = p_.step_qty;  // 자본 미상 폴백
                }

                if (bp > 0.0 && rq > 0)
                {
                    plan.push_back({OrderSide::BUY, bp, rq});
                }
            }
        }

        // ── no-change 가드: 처닝 차단. 분할 매수가 살아있고 (a)계획 시그니처가 직전과 동일하거나
        //    (b)SMA 이동이 reprice_move_ticks 데드밴드 이내이고 포지션도 그대로면 재발주 스킵.
        //    (b)가 핵심: SMA가 틱경계를 스치며 미세이동할 때마다 전량 취소·재발주해 매도 rung이
        //    체결 전에 취소되고 매수만 쌓여 pos가 편증하던 처닝을 차단(reprice_move_ticks 구현).
        std::string sig;

        for (const auto& r : plan)
        {
            sig += std::string(r.side == OrderSide::BUY ? "B" : "S") + fmt1(r.price) +
                   "x" + std::to_string(r.qty) + "|";
        }

        // G1 국면 게이트: 비활성 국면(regime→전략 자동선택에서 미선택)에선 매수(진입·물타기)
        //  rung을 깔지 않는다. 익절 매도·청산은 국면과 무관하게 유지(is_active 계약: 진입만 차단).
        //  active 상태를 시그니처에 접미 → 국면 플립 시 no-change 가드에 걸리지 않고 재구성되어
        //  기존 매수 예약이 cancel_all로 취소된다(플립 후 매수만 잔존하는 구멍 차단).
        //  신규매수 차단(entry_halt)도 같은 축이다 — 게이트가 거부만 하면 계획이 안 바뀌어
        //  차단 해제 뒤에도 매수 rung이 돌아오지 않았다. 차단 중엔 매수 rung을 걷고(취소),
        //  풀리면 시그니처가 바뀌어 다시 깐다. 떨림은 D-033 체류가 막는다. [why D-033]
        //  스탑·트레일 뒤 쿨다운도 같은 축이다 — 존이 열려 있어도 분할 매수를 걷는다.
        const bool cooling  = stop_cooldown_until_ != std::chrono::steady_clock::time_point{} && now < stop_cooldown_until_;
        const bool entry_on = is_active() && !entry_halted() && !cooling;
        sig += entry_on ? "A1" : "A0";
        // 데드밴드는 분할 주문 앵커 기준이다. 현재가 앵커(anchor_on_price)에서 SMA로 재면 값이
        //  틱마다 바뀌는데 데드밴드는 조용하다고 판정해 (b)가 걸리지 않았다(09-11 TRENDX 재구성
        //  669회 vs DEVSCALE 160회).
        const double anchor_ref   = p_.anchor_on_price ? base_line : sma;
        const double reprice_band = p_.reprice_move_ticks * tick_size(anchor_ref);
        const bool   sma_quiet    = last_anchor_ > 0.0 && std::fabs(anchor_ref - last_anchor_) < reprice_band;

        // (a) 계획 시그니처+pos가 직전과 동일하면 live 유무와 무관하게 스킵.
        //     매도가능=0이라 아무것도 못 깔아 live_가 빈 채로 남을 때(원장 보유↔매도가능 괴리)
        //     매 하트비트 재진입해 잔고조회를 난사하던 스핀을 차단. 체결로 pos가 바뀌면 즉시 재구성.
        // (b) 데드밴드(reprice 이내 미세이동)+pos 동일 스킵은 살아있는 분할 매수에만 적용.
        if (sig == last_ladder_sig_ && pos == last_pos_)
        {
            return; // 동일 계획 → 유지(빈 계획 포함)
        }

        if (!live_.empty() && sma_quiet && pos == last_pos_)
        {
            return; // 데드밴드 내 미세이동 → 유지
        }

        // (c) 재구성 최소 간격. (a)(b)는 둘 다 pos == last_pos_를 요구하므로, 부분체결이
        //     연달아 들어오는 종목은 체결마다 분할 매수를 통째로 헐고 다시 깐다. rung 2개면
        //     체결 1건에 취소 2 + 신규 2가 나가고, 이게 계좌 공용 주문예산(5/s·20/min)을
        //     한 종목이 독점한다(09-08 12:31~12:36 016610 단독 84건 = 전체의 8할).
        //     여기서 막아도 기존 rung은 살아 있으므로 체결 기회를 잃지 않는다. 잠깐 크기가
        //     낡은 채로 유지될 뿐이다. 청산(emit_liquidation)은 이 경로를 타지 않는다.
        //     live_가 비어 있어도 적용한다 — 매도가능=0으로 아무것도 못 깔면 live_가 빈 채
        //     남는데, 그때 이 가드를 건너뛰면 하트비트(3초)마다 재구성·잔고조회가 돈다.
        if (p_.min_rebuild_sec > 0 &&
            last_rebuild_ != std::chrono::steady_clock::time_point{})
        {
            const auto since = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - last_rebuild_).count();

            if (since < p_.min_rebuild_sec)
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
        const std::string entry_kind = p_.entry_lower_pct > 0.0 ? "정배열추세확장진입"
                                                                : "정배열눌림진입";
        // 진입 문맥 스탬프(2026-09-11 회의 §3·§5): 체결강도(CTTR)·20일 평균 대비 누적거래량
        //  배율·직전 250봉 고가 대비 거리. 나중에 "저항 아래서 샀나"를 원장에서 바로 대조한다.
        //  REST 폴링 틱은 strength/acml_volume이 0이라 그때는 찍지 않는다.
        std::string entry_ctx;
        {
            double vol20 = 0.0, hi250 = 0.0;
            const size_t nv = (std::min)(daily_.size(), static_cast<size_t>(20));

            for (size_t i = 0; i < nv; ++i)
            {
                vol20 += static_cast<double>(daily_[i].volume);
            }

            vol20 = nv > 0 ? vol20 / static_cast<double>(nv) : 0.0;

            for (const auto& b : daily_)
            {
                hi250 = (std::max)(hi250, b.high);
            }

            if (td.strength > 0.0)
            {
                entry_ctx += " 체결강도=" + fmt1(td.strength);
            }

            if (td.acml_volume > 0 && vol20 > 0.0)
            {
                entry_ctx += " 누적거래량/20일평균=" + fmt1(static_cast<double>(td.acml_volume) / vol20);
            }

            if (hi250 > 0.0)
            {
                entry_ctx += " 250봉고가대비=" + fmt1((cur_px - hi250) / hi250 * 100.0) + "%";
            }

            if (p_.stop_loss_pct > 0.0)
            {
                entry_ctx += " 손절=-" + fmt1(p_.stop_loss_pct) + "%";
            }
        }

        const std::string buy_ctx  = entry_kind + " 이격=" + fmt1(s_dev) + "% 일봉SMA20=" +
                                     fmt1(d_s20) + " 현재가=" + fmt1(cur_px) + entry_ctx;
        int sell_room = -1;                          // -1=미조회(지연). 첫 매도 rung에서 1회 조회.

        for (const auto& r : plan)
        {
            if (r.side == OrderSide::SELL)
            {
                if (sell_room < 0)
                {
                    sell_room = sellable_qty();      // 안전 우선: 불확실하면 0(매도 보류)
                }

                int q = r.qty < sell_room ? r.qty : sell_room;

                if (q <= 0)
                {
                    continue;                        // 매도가능 소진/없음 → 이 rung 스킵
                }

                place(out, OrderSide::SELL, r.price, q, "익절밴드 지정가=" + fmt1(r.price));
                sell_room -= q;
            }
            else if (entry_on)                       // G1: 비활성 국면이면 매수 rung 스킵(진입 차단)
            {
                place(out, r.side, r.price, r.qty, buy_ctx);
            }
        }

        last_anchor_ = anchor_ref;
        last_ladder_sig_ = sig;
        last_pos_ = pos;
        last_rebuild_ = std::chrono::steady_clock::now();
        LOG_INFO("[" + id() + "] 분할 매수 재구성 sma=" + fmt1(sma) +
                 std::string(warming ? "(일봉)" : "") + " px=" + fmt1(cur_px) +
                 " pos=" + std::to_string(pos) + " live=" + std::to_string(live_.size()) +
                 " 명목=" + std::to_string(static_cast<long long>(base_notional + rung_budget)) + "원");
    }

private:
    // ── 지표 (indicators.py 이식, bars[0]=최신) ──────────────────────────────
    static double sma_close(const std::vector<MarketData>& bars, int period)
    {
        if (static_cast<int>(bars.size()) < period || period <= 0)
        {
            return 0.0;
        }

        double s = 0.0;

        for (int i = 0; i < period; ++i)
        {
            s += bars[i].close;
        }

        return s / period;
    }

    // 전일까지의 일봉 이동평균에 오늘 현재가를 접어 넣어 돌려준다. 60봉 미만이면 s60=0인
    //  빈 값이라 호출부가 정배열을 false로 떨어뜨린다(판정 자체를 못 하는 상태).
    //  [why D-005] 일봉 조회가 당일 봉을 자르므로 여기서 오늘을 되살린다.
    // 전일 확정 이동평균. 오늘 현재가를 접지 않아 세션 내내 상수다 — 청산처럼 되돌릴 수
    //  없는 판정이 이쪽을 쓴다. 60봉 미만이면 전 필드 0을 돌려준다(호출자가 s60>0으로 거른다).
    static quant::ma::Smas daily_smas_prev(const std::vector<MarketData>& daily)
    {
        quant::ma::Smas prev;

        if (static_cast<int>(daily.size()) < 60)
        {
            return prev;
        }

        prev.s5  = sma_close(daily, 5);
        prev.s10 = sma_close(daily, 10);
        prev.s20 = sma_close(daily, 20);
        prev.s60 = sma_close(daily, 60);
        return prev;
    }

    static quant::ma::Smas daily_smas(const std::vector<MarketData>& daily, double cur_px)
    {
        const quant::ma::Smas prev = daily_smas_prev(daily);

        if (prev.s60 <= 0.0)
        {
            return prev;
        }

        return quant::ma::fold_today(prev, daily[4].close, daily[9].close,
                                     daily[19].close, daily[59].close, cur_px);
    }

    // ── 프리페치: 무거운 REST(3분봉·일봉·잔고)를 공유 전략 스레드 밖에서 미리 당겨
    //    스냅샷에 적재한다. on_trade_batch는 스냅샷만 읽어(락 짧게) 발주를 판단 → 특정
    //    종목의 느린 REST가 전 전략을 막던 head-of-line 블로킹을 없앤다. 발주·매도가능
    //    (sellable_qty)은 원장 최신성을 위해 동기 유지. 여기서 부르는 KIS 메서드는 전부
    //    읽기전용(get_daily_ohlcv·get_minute_ohlcv·get_balance, 동시호출 감사 완료).
    void prefetch_loop()
    {
        while (!prefetch_stop_.load(std::memory_order_relaxed))
        {
            // 장 밖에서는 받아봐야 같은 응답이다. KIS 분봉은 기준시각을 15:30으로 클램프하므로
            //  (KisClient.cpp) 장 마감 후엔 종일 같은 봉을 다시 받고, 그 호출이 초당 한도를
            //  차지해 다른 조회를 500으로 밀어낸다. 발주는 어차피 장중에만 나가므로 건너뛴다.
            //  창은 08:50~15:35로 장 마감 청산(15:15)까지 덮는다.
            const int hhmm = kst_hhmm();
            const int wday = kst_tm().tm_wday;
            const bool in_session = (wday >= 1 && wday <= 5) && hhmm >= 850 && hhmm <= 1535;

            if (kis_ && in_session)
            {
                // 일봉·자본: 날짜 바뀌면 1회 갱신(장중엔 사실상 1일 1회).
                std::string today = kst_ymd();
                bool need_daily;
                {
                    std::lock_guard<std::mutex> lk(snap_mtx_);
                    need_daily = snap_daily_.empty() || snap_daily_date_ != today;
                }

                if (need_daily)
                {
                    auto d = kis_->get_daily_ohlcv(p_.ticker, p_.daily_lookback);

                    // 일봉이 비면(500·휴장) 스냅샷을 안 채우므로 need_daily가 참으로 남아
                    //  다음 주기에 또 온다. 그때 잔고까지 같이 부르면 한도 초과 상황에서
                    //  호출을 오히려 늘린다 — 일봉이 온 경우에만 잔고를 부른다.
                    if (!d.empty())
                    {
                        double eq = fetch_equity();
                        std::lock_guard<std::mutex> lk(snap_mtx_);
                        snap_daily_      = std::move(d);
                        snap_daily_date_ = today;
                        snap_equity_     = eq;
                    }
                }

                // 3분봉: 봉이 바뀔 때만 갱신한다. 이 조회는 페이지네이션이라 1회에 HTTP GET이
                //  세 번 나가는데(당일 63분치 1분봉을 다시 받아 집계), 그중 마감된 봉은 불변이고
                //  달라지는 건 진행 중인 봉 하나뿐이다. 그 하나는 아래 on_trade_batch가 들어오는
                //  체결 틱으로 덮으므로 SMA 값은 같게 유지되면서 조회는 봉 주기당 1회로 준다.
                const int bucket = kst_bar_bucket(p_.interval_min);
                bool need_bars;
                {
                    std::lock_guard<std::mutex> lk(snap_mtx_);
                    need_bars = snap_bars_.empty() || snap_bars_bucket_ != bucket;

                    // 봉 경계 직후 종목별 지터만큼 미룬다(첫 스냅샷은 바로). 진행 중인 봉은
                    //  on_trade_batch의 체결가 덮어쓰기가 채우므로 늦게 받아도 SMA는 같다.
                    if (need_bars && !snap_bars_.empty() &&
                        kst_sec_into_bucket(p_.interval_min) < prefetch_jitter_sec_)
                    {
                        need_bars = false;
                    }
                }

                if (need_bars)
                {
                    auto bars = kis_->get_minute_ohlcv(p_.ticker, p_.sma_period + 1, p_.interval_min);

                    if (!bars.empty())
                    {
                        std::lock_guard<std::mutex> lk(snap_mtx_);
                        snap_bars_        = std::move(bars);
                        snap_bars_bucket_ = bucket;
                    }
                }
            }

            // min_action_ms를 50ms 조각으로 자며 stop 신호에 빠르게 반응.
            for (int slept = 0;
                 slept < p_.min_action_ms && !prefetch_stop_.load(std::memory_order_relaxed);
                 slept += 50)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void stop_prefetch()
    {
        prefetch_stop_.store(true, std::memory_order_relaxed);

        if (prefetch_thread_.joinable())
        {
            prefetch_thread_.join();
        }
    }

    // 사이징 기준 자본(총평가금) 조회. output2 tot_evlu_amt(없으면 nass_amt). 알 수 없으면 0.
    //  프리페치 스레드에서 호출(읽기전용). 폴백(fallback_equity) 적용은 호출측(on_trade_batch, line eq).
    // 총평가금은 계좌 하나의 값이라 종목마다 다시 부를 이유가 없다. 전략 스레드가 종목 수만큼
    //  있어 기동 직후 같은 잔고 조회가 40건 동시에 나갔고, 모의 키 2건/초 한도를 주문까지
    //  끌어내렸다(09-11 09:18 EGW00201). 프로세스 공용으로 하루 한 번만 부르고, 실패(0)는
    //  캐시하지 않아 다음 종목이 다시 시도한다. 뮤텍스를 조회 동안 잡아 동시 진입도 한 번으로 접는다.
    double fetch_equity()
    {
        static std::mutex  s_mu;
        static std::string s_ymd;
        static double      s_eq = 0.0;
        std::lock_guard<std::mutex> lk(s_mu);
        const std::string today = kst_ymd();

        if (s_ymd == today && s_eq > 0.0)
        {
            return s_eq;
        }

        double eq = 0.0;
        KisClient* akis = account_kis();

        if (akis && akis->has_account())
        {
            try
            {
                const KisResult<AccountBalance> bal = akis->get_balance();

                if (bal && bal->total_eval_amt)
                {
                    eq = *bal->total_eval_amt;
                }
            }
            catch (...) {}
        }

        if (eq > 0.0)
        {
            s_ymd = today;
            s_eq  = eq;
        }

        return eq;
    }

    // 명목→수량(주). 가격/명목 유효하지 않으면 0(호출측이 폴백 결정).
    static int qty_for(double notional, double price)
    {
        if (price <= 0.0 || notional <= 0.0)
        {
            return 0;
        }

        int q = static_cast<int>(std::floor(notional / price));
        return q > 0 ? q : 0;
    }

    // ── KRX 호가단위/격자 절사는 core/TickSize.h(krx::)로 일원화. 얇은 위임만 유지. ──
    static double tick_size(double price) { return krx::tick_size(price); }
    static double round_to_tick(double p, OrderSide side) { return krx::round_to_tick(p, side); }

    std::string next_oid(const char* tag)
    {
        return id() + ":" + tag + ":" + std::to_string(++seq_);
    }

    void place(std::vector<OrderSignal>& out, OrderSide side, double price, int qty,
               const std::string& reason = "")
    {
        if (qty <= 0)
        {
            return; // qty=0 NEW 발주 억제 — 게이트 거부·로그 노이즈 원천 차단(SELL은 상위서도 클램프)
        }

        std::string oid = next_oid(side == OrderSide::BUY ? "B" : "S");
        OrderSignal s;
        s.ticker      = p_.ticker;
        s.side        = side;
        s.type        = OrderType::LIMIT;
        s.quantity    = qty;
        s.price       = price;
        s.strategy_id = id();
        s.market      = Market::KR;
        s.action      = OrderAction::NEW;
        s.client_oid  = oid;
        s.account_id  = p_.account;
        s.reason      = reason; // G4: 판단 근거를 신호에 실어 영속
        s.timestamp   = std::chrono::system_clock::now();
        out.push_back(s);
        live_.push_back({oid, side});
    }

    // 미체결 전량 취소. 발주가 있었으면 true.
    bool cancel_all(std::vector<OrderSignal>& out)
    {
        if (live_.empty())
        {
            return false;
        }

        for (const auto& o : live_)
        {
            OrderSignal s;
            s.ticker          = p_.ticker;
            s.side            = o.side;
            s.type            = OrderType::LIMIT;
            s.quantity        = 0;
            s.strategy_id     = id();
            s.market          = Market::KR;
            s.action          = OrderAction::CANCEL;
            s.orig_client_oid = o.oid;
            s.account_id      = p_.account;
            s.timestamp       = std::chrono::system_clock::now();
            out.push_back(s);
        }

        live_.clear();
        return true;
    }

    OrderSignal make_market_sell(int qty, const std::string& reason = "")
    {
        OrderSignal s;
        s.ticker      = p_.ticker;
        s.side        = OrderSide::SELL;
        s.type        = OrderType::MARKET;
        s.quantity    = qty;
        s.strategy_id = id();
        s.market      = Market::KR;
        s.action      = OrderAction::NEW;
        s.account_id  = p_.account;
        s.reason      = reason; // G4: 청산 사유(존 이탈/장 마감 등)를 신호에 실어 영속
        s.ref_price   = liquidation_ref_price(); // 시장가는 price=0이라 이 값이 없으면 1주문 명목 상한이 비어 버린다
        s.timestamp   = std::chrono::system_clock::now();
        return s;
    }

    // 시장가 청산 신호의 명목 평가 기준가. 직전 체결가 > 잔고 평단 > 최근 3분봉 종가 순.
    double liquidation_ref_price()
    {
        if (last_px_ > 0.0)
        {
            return last_px_;
        }

        if (last_avg_px_ > 0.0)
        {
            return last_avg_px_;
        }

        std::lock_guard<std::mutex> lk(snap_mtx_);
        return snap_bars_.empty() ? 0.0 : snap_bars_[0].close;
    }

    // ── 청산 발주: 매도가능분 클램프 + 지수 백오프 ───────────────────────────
    //  버그 이력: 존이탈/장 마감 청산이 원장 보유수량 전량을 시장가 매도했으나, 예약매도
    //  (미연결/미결제)로 실매도가능분(ord_psbl_qty)이 보유보다 작으면 KIS가 전량 거부
    //  (40240000 "잔고내역 없습니다") → 매 하트비트 무한 재거부 스팸. 실계좌 동일.
    //  대책: (1) 매 시도 get_balance의 '실시간' 주문가능수량으로 클램프 → 잠긴 수량
    //  초과분 미발주(과매도·이중주문 위험 0, 브로커 상태 기준이라 체결지연에도 자기교정).
    //  (2) 시도 후 진행(pos 감소) 없으면 30·60·120·240·480s(cap 300s) 지수 백오프.
    //  반환: 시장가 매도를 실제로 out에 넣었으면 true.
    bool emit_liquidation(std::vector<OrderSignal>& out, int pos,
                          std::chrono::steady_clock::time_point now, const std::string& tag,
                          long long max_backoff_ms = 300000)
    {
        if (pos <= 0)
        {
            return false;
        }

        // 진행 판정: 직전 시도보다 pos가 줄었으면(부분체결) 백오프 리셋.
        if (liq_last_pos_ < 0 || pos < liq_last_pos_)
        {
            liq_fail_streak_ = 0;
            liq_next_ = std::chrono::steady_clock::time_point{};
        }

        if (liq_next_.time_since_epoch().count() != 0 && now < liq_next_)
        {
            return false; // 백오프 창 내 — 재발주 스킵(스팸 차단)
        }

        const int sellable = sellable_qty();             // 안전 우선: 불확실하면 0(보류)
        const int q = sellable > 0 ? (pos < sellable ? pos : sellable) : 0;
        bool emitted = false;

        if (q > 0)
        {
            out.push_back(make_market_sell(q, "청산:" + tag));
            emitted = true;
            LOG_INFO("[" + id() + "] " + tag + " — 취소+청산 pos=" + std::to_string(pos) +
                     " 매도가능=" + std::to_string(sellable) + " 발주=" + std::to_string(q));
        }
        else
        {
            LOG_WARN("[" + id() + "] " + tag + " 청산 보류 — 매도가능=0 (잠긴 " +
                     std::to_string(pos) + "주, 예약취소/결제 대기) 백오프#" +
                     std::to_string(liq_fail_streak_ + 1));
        }

        liq_last_pos_ = pos;
        ++liq_fail_streak_;
        int shift = liq_fail_streak_ - 1;

        if (shift > 4)
        {
            shift = 4;
        }

        long long ms = 30000LL << shift;                 // 30/60/120/240/480…

        if (ms > max_backoff_ms)
        {
            ms = max_backoff_ms;  // cap 기본 5분, 스탑 경로는 30초
        }

        liq_next_ = now + std::chrono::milliseconds(ms);
        return emitted;
    }

    // 해당 종목의 실시간 매도가능수량(ord_psbl_qty). 안전 우선: 확실히 알 수 없으면 0
    //  (보류)을 반환해 절대 과매도/이중주문을 유발하지 않는다.
    //  - kis_ 없음/시세전용(계좌 없는 quote) 클라이언트 → 0 (불필요한 잔고 조회도 안 함).
    //    실계좌(단일 클라이언트)는 account 보유 → 정상 조회로 클램프.
    //  - 조회 실패(예외·output1 없음)·잔고에 종목 없음·필드 없음 → 0 (다음 백오프에 재시도).
    int sellable_qty()
    {
        // 원장 접근자가 주입돼 있으면 그것으로 끝낸다 — 전략 스레드에서 REST를 부르지 않는다. [why D-055]
        if (const auto led = ledger_sellable(p_.account, p_.ticker))
        {
            if (led->avg_px > 0.0)
            {
                last_avg_px_ = led->avg_px;
            }

            return led->sellable;
        }

        KisClient* akis = account_kis();

        if (!akis || !akis->has_account())
        {
            return 0;
        }

        try
        {
            // 접근자 미주입(단독 실행·테스트) 경로. 공유 전략 스레드에서 동기로 돌므로 재시도만 뗀다 —
            //  실패는 아래에서 0으로 떨어지고 다음 하트비트에 다시 온다.
            KisClient::FastFailScope ff;
            const KisResult<AccountBalance> bal = akis->get_balance();

            if (!bal)
            {
                return 0;
            }

            for (const Holding& h : bal->holdings)
            {
                if (h.ticker != p_.ticker)
                {
                    continue;
                }

                last_avg_px_ = h.avg_price;
                return h.sellable_qty.value_or(0);
            }

            return 0; // 잔고에 종목 없음 → 매도가능 0
        }
        catch (...)
        {
            return 0;
        }
    }

    // ── KST 시각 헬퍼(서버 TZ 독립: gmtime + 9h) ─────────────────────────────
    static struct tm kst_tm()
    {
        time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + 9 * 3600;
        struct tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        return tmv;
    }

    static int kst_hhmm()
    {
        struct tm k = kst_tm();
        return k.tm_hour * 100 + k.tm_min;
    }

    // 지금이 몇 번째 봉인가(KST). 날짜를 섞어 자정을 넘겨도 값이 겹치지 않게 한다.
    static int kst_bar_bucket(int interval_min)
    {
        if (interval_min < 1)
        {
            interval_min = 1;
        }

        struct tm k = kst_tm();
        return (k.tm_yday * 1440 + k.tm_hour * 60 + k.tm_min) / interval_min;
    }

    // 현재 봉이 시작한 뒤 흐른 초(KST). 프리페치 지터 판정용.
    static int kst_sec_into_bucket(int interval_min)
    {
        if (interval_min < 1)
        {
            interval_min = 1;
        }

        struct tm k = kst_tm();
        return ((k.tm_hour * 60 + k.tm_min) % interval_min) * 60 + k.tm_sec;
    }

    static std::string kst_ymd()
    {
        struct tm k = kst_tm();
        char buf[9];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &k);
        return std::string(buf);
    }

    static std::string fmt1(double v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.1f", v);
        return std::string(b);
    }

    struct Live { std::string oid; OrderSide side; };

    Params p_;
    std::vector<Live> live_;               // 현재 live로 낙관하는 예약들
    std::vector<MarketData> daily_;        // 일봉 캐시(정배열/눌림 판정)
    double equity_ = 0.0;                   // 사이징 기준 자본(총평가금) 스냅샷 — 일별 갱신
    double last_anchor_ = 0.0;             // 마지막 재구성의 분할 주문 앵커(SMA 또는 현재가) — 데드밴드 기준
    double last_px_ = 0.0;                 // 직전 체결가 — 시장가 청산 ref_price
    double last_avg_px_ = 0.0;             // 잔고 평단(sellable_qty가 갱신) — ref_price 폴백
    int64_t last_warm_log_ms_ = 0;         // 봉 부족 로그 스로틀(60초)
    int    last_pos_ = -1;                  // 마지막 재구성 시 포지션(데드밴드 가드)
    std::string last_ladder_sig_;          // 마지막 발주 분할 매수 시그니처(no-change 가드)
    std::chrono::steady_clock::time_point last_work_{};   // 스로틀
    std::chrono::steady_clock::time_point last_rebuild_{}; // 마지막 분할 매수 전면 재구성
    bool last_zone_ = false;                              // 마지막 존 상태(변화 로그용)
    bool in_zone_   = false;                              // 존 히스테리시스 상태(진입/청산 임계 전환)
    bool entry_closed_logged_ = false;                    // 진입 축 닫힘 로그를 냈나(도배 방지)
    std::chrono::steady_clock::time_point zone_log_ts_{}; // 마지막 존 판정 로그 시각
    std::chrono::steady_clock::time_point liq_next_{};    // 청산 재시도 백오프 해제 시각
    int    liq_last_pos_    = -1;                          // 직전 청산시도 pos(진행 판정)
    int    liq_fail_streak_ = 0;                           // 연속 미진행 횟수(백오프 지수)
    std::chrono::steady_clock::time_point stop_cooldown_until_{}; // 스탑·트레일 뒤 분할 매수 재개 시각
    std::chrono::steady_clock::time_point avg_query_ts_{};        // 평단 직접 조회 스로틀(60초)
    int    prefetch_jitter_sec_ = 0;                       // 봉 경계 뒤 분봉 조회 지연(초, 티커 해시)
    uint64_t seq_ = 0;

    // ── 프리페치(무거운 REST를 공유 전략 스레드 밖으로) ──────────────────────
    std::thread             prefetch_thread_;
    std::atomic<bool>       prefetch_stop_{false};
    std::mutex              snap_mtx_;               // 아래 snap_* 보호
    std::vector<MarketData> snap_daily_;             // 일봉 스냅샷
    std::string             snap_daily_date_;        // 스냅샷 기준일(KST YYYYMMDD)
    double                  snap_equity_ = 0.0;      // 자본 스냅샷(raw, 폴백 미적용)
    std::vector<MarketData> snap_bars_;              // 3분봉 스냅샷
    int snap_bars_bucket_ = -1;                      // 그 스냅샷을 받은 봉 번호(kst_bar_bucket)
};
