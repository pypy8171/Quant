#pragma once
#include "api/KisClient.h"
#include "core/TickSize.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// DeviationScaleStrategy — 일봉 존(정배열+눌림) 게이트 + 3분봉 이격도 분할매매(지정가 예약)
//
//  아이디어:
//   • "매매할 자리"는 일봉에서 정한다: 정배열(SMA5>10>20>60) AND 현재가가 일봉 SMA20
//     ±pullback_pct 이내(눌림목). 이 조건이 참일 때만 오실레이션을 켠다(존 활성).
//   • 자리 안에서는 3분봉 SMA를 기준선으로 삼아:
//       - 존 진입 시 무포지션이면 목표수량 절반(base_qty)을 기준선 근처 지정가로 베이스 매수.
//       - 이격도가 위로 벌어지는 지점(+dev_sell%·n_rungs층)에 지정가 매도(분할 익절).
//       - 평균으로 되돌아오는 지점(−dev_buy%·n_rungs층)에 지정가 매수(재진입).
//   • 시장가가 아니라 지정가 예약을 미리 걸어 "기다리는" 매매. 3분봉이 갱신되거나 SMA가
//     reprice_move_ticks 이상 이동하면 미체결 사다리를 CANCEL+NEW로 재호가(MM-1 패턴).
//
//  구동: rest_price_feed 모드에서 DataThread가 매 사이클 현재가를 TradeData로 주입 →
//        on_trade_batch가 하트비트로 호출된다(WS 불필요). 3분봉/일봉은 kis_로 자가조회.
//
//  포지션 진실원천: OrderGate 확정 포지션(confirmed_position). 체결콜백 부재(rest)에도
//        리컨사일로 원장이 최신이라 신뢰 가능 → 별도 on_fill 불필요.
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
        double fallback_equity = 0.0;  // 잔고조회 실패 시 사용할 기준자본(원). 0이면 주수 폴백
        int    base_qty       = 10;    // (폴백) 존 진입 베이스 매수 수량
        int    step_qty       = 5;     // (폴백) 각 밴드(rung) 분할 수량
        int    sma_period     = 20;    // 3분봉 기준선 SMA 기간
        double dev_sell       = 1.5;   // 매도 밴드 이격도(%) — 층당 배수
        double dev_buy        = 0.8;   // 매수 밴드 이격도(%) — 층당 배수
        int    n_rungs        = 2;     // 밴드 층수
        bool   add_below_sma_only = true; // 물타기(매수 밴드)를 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 깐다.
                                          //  true=점진 진입: 활성 시 base만 → 진짜 눌림에서만 평단 낮춤(즉시 10% 만재 방지).
                                          //  false=예전 성격: 활성 즉시 base+물타기 전부 예약(빠른 풀사이즈).
        double pullback_pct   = 2.0;   // 일봉 SMA20 눌림 허용폭(하단,%) — 존 진입 임계(SMA20 아래)
        double entry_upper_pct = 0.0;  // 진입 상단(SMA20 위 허용%). 0=SMA20 이하만(순수 눌림). >0이면 SMA20 위 그만큼까지 진입 허용(완만상승·소폭눌림 포착)
        double zone_hyst_pct  = 4.0;   // 존 히스테리시스 밴드(%) — 청산 임계 = pullback + 이 값
        int    reprice_move_ticks = 2; // SMA가 이만큼(틱) 이동하면 재호가
        int    eod_hhmm       = 1515;  // 이 시각(KST HHMM) 이후 전량 취소+청산
        int    interval_min   = 3;     // 집계봉 간격(분)
        int    min_action_ms  = 3000;  // 무거운 작업(3분봉 조회+발주) 최소 간격
        int    daily_lookback = 70;    // 일봉 조회 개수(SMA60 판정 위해 ≥60)
        std::string account;           // 원장 계좌키(단일계좌는 "")
    };

    explicit DeviationScaleStrategy(Params p) : p_(std::move(p))
    {
        if (p_.n_rungs < 1) p_.n_rungs = 1;
        if (p_.sma_period < 2) p_.sma_period = 2;
        if (p_.interval_min < 1) p_.interval_min = 1;
    }

    ~DeviationScaleStrategy() { stop_prefetch(); }

    std::string id() const override { return "DEVSCALE_" + p_.ticker; }

    // 로그 표시용 "티커(종목명)". 이름 없으면 티커만. id()·데이터키와는 분리.
    std::string disp() const
    {
        return p_.name.empty() ? p_.ticker : p_.ticker + "(" + p_.name + ")";
    }

    std::string describe() const override
    {
        return "DeviationScale | " + disp() + " | base=" + std::to_string(p_.base_qty) +
               " step=" + std::to_string(p_.step_qty) + " sma=" + std::to_string(p_.sma_period) +
               "(3m) dev_sell=" + fmt1(p_.dev_sell) + "% dev_buy=" + fmt1(p_.dev_buy) +
               "% rungs=" + std::to_string(p_.n_rungs) + " pullback=" + fmt1(p_.pullback_pct) + "%";
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
        last_sma_ = 0.0;
        last_pos_ = -1;
        last_ladder_sig_.clear();
        in_zone_ = false;
        liq_next_ = std::chrono::steady_clock::time_point{};
        liq_last_pos_ = -1;
        liq_fail_streak_ = 0;
        last_work_ = std::chrono::steady_clock::time_point{};
        daily_.clear();
        daily_date_.clear();
        equity_ = 0.0;
        seq_ = 0;
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            snap_daily_.clear();
            snap_daily_date_.clear();
            snap_equity_ = 0.0;
            snap_bars_.clear();
        }
        LOG_INFO("[" + id() + "] 시작 — " + describe());
        // set_kis()가 on_start 직전 호출됨(Engine start/재스캔 둘 다) → kis_ 확정. 여기서 프리페치 기동.
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
            return;

        const int hhmm = kst_hhmm();

        // ── EOD 안전장치: 전량 취소 + 시장가 청산 ────────────────────────────
        if (hhmm >= p_.eod_hhmm)
        {
            bool cancelled = cancel_all(out);
            int pos = confirmed_position(p_.account, p_.ticker);
            const std::string tag = "EOD(" + std::to_string(hhmm) + ")";
            emit_liquidation(out, pos, std::chrono::steady_clock::now(), tag); // 클램프+백오프(자체 로깅)
            if (cancelled && pos <= 0)
                LOG_INFO("[" + id() + "] " + tag + " — 미체결 취소(보유 0)");
            return;
        }

        // ── 무거운 작업 스로틀(3분봉 조회·발주) ──────────────────────────────
        const auto now = std::chrono::steady_clock::now();
        if (last_work_.time_since_epoch().count() != 0 &&
            now - last_work_ < std::chrono::milliseconds(p_.min_action_ms))
            return;
        last_work_ = now;

        const double cur_px = td.price;
        if (cur_px <= 0.0)
            return;

        // ── 프리페치 스냅샷 스냅(일봉·자본·3분봉). 아직 준비 전이면 다음 하트비트 대기 ──
        //  무거운 REST는 프리페치 스레드가 미리 당겨둔다. 여기선 락을 짧게 잡고 복사만.
        std::vector<MarketData> bars;
        {
            std::lock_guard<std::mutex> lk(snap_mtx_);
            if (snap_daily_.empty())
                return; // 일봉 미준비 — 프리페치 대기
            daily_  = snap_daily_;
            equity_ = snap_equity_;
            bars    = snap_bars_;
        }

        // ── 일봉 존 판정(정배열 + SMA20 눌림) ────────────────────────────────
        const bool   aligned = is_aligned(daily_);
        const double d_s20   = sma_close(daily_, 20);
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
        const bool   zone    = aligned && d_s20 > 0.0 && s_dev <= up_th && s_dev >= -down_th;
        in_zone_ = zone;

        // 존 판정 로그: 상태 변화 시 또는 60초마다 1회.
        if (zone != last_zone_ || zone_log_ts_.time_since_epoch().count() == 0 ||
            now - zone_log_ts_ >= std::chrono::seconds(60))
        {
            LOG_INFO("[" + id() + "] " + disp() + " 존 판정 " + std::string(zone ? "활성" : "대기") +
                     " | 정배열=" + std::string(aligned ? "Y" : "N") +
                     " 일봉SMA20=" + fmt1(d_s20) + " 현재가=" + fmt1(cur_px) +
                     " 이격=" + fmt1(s_dev) + "% (눌림밴드 " + fmt1(-down_th) + "%~" + fmt1(up_th) +
                     "%) 일봉수=" + std::to_string(daily_.size()));
            last_zone_    = zone;
            zone_log_ts_  = now;
        }

        if (!zone)
        {
            // 존 이탈 → 미체결 전부 취소 + 보유분 시장가 청산(매도가능분 클램프+백오프).
            bool cancelled = cancel_all(out);
            int pos = confirmed_position(p_.account, p_.ticker);
            emit_liquidation(out, pos, now, "존 이탈"); // 클램프+백오프(자체 로깅)
            if (cancelled && pos <= 0)
                LOG_INFO("[" + id() + "] 존 이탈 — 미체결 취소(보유 0)");
            return;
        }

        // ── 3분봉 기준선(스냅샷에서 이미 받음) ───────────────────────────────
        if (static_cast<int>(bars.size()) < p_.sma_period)
            return; // 봉 부족 — 다음 하트비트 재시도
        const double sma = sma_close(bars, p_.sma_period); // bars[0]=최신
        if (sma <= 0.0)
            return;
        int pos = confirmed_position(p_.account, p_.ticker);

        // ── 목표 사다리 산출(발주 전) ─────────────────────────────────────────
        //  계단 가격·수량은 sma·pos의 순수 함수. 먼저 계획을 만들고 직전 사다리와
        //  시그니처를 비교해 "동일하면 재발주 스킵". 분봉 정지(HTTP 500 폴백)로
        //  sma=px가 고정될 때 동일 사다리를 취소·재발주하던 처닝을 차단.
        struct Rung { OrderSide side; double price; int qty; };
        std::vector<Rung> plan;

        // ── 명목 사이징: 자본%를 가격으로 나눠 수량 산출(자본 미상이면 주수 폴백) ──
        //  베이스=자본×base_pct(5%), 물타기 총예산=자본×(max_pct−base_pct)(5%)를 n_rungs로 분할.
        //  베이스+물타기 합 ≈ 자본×max_pct(10%) → OrderGate 명목캡과 정합(캡은 백스톱).
        const double eq            = equity_ > 0.0 ? equity_ : p_.fallback_equity;
        const double base_notional = eq * p_.base_pct;
        const double rung_budget   = eq * (p_.max_pct > p_.base_pct ? p_.max_pct - p_.base_pct : 0.0);
        const double rung_notional = p_.n_rungs > 0 ? rung_budget / p_.n_rungs : 0.0;

        // 베이스: 무포지션이면 기준선 근처 지정가 매수(자본의 base_pct).
        if (pos <= 0)
        {
            double bp = round_to_tick(sma, OrderSide::BUY);
            int    bq = qty_for(base_notional, bp);
            if (bq <= 0) bq = p_.base_qty;              // 자본 미상 폴백
            if (bp > 0.0 && bq > 0)
                plan.push_back({OrderSide::BUY, bp, bq});
        }

        // 매도 밴드: 이격 +dev_sell%*i. 보유분을 n_rungs로 균등 분할(숏 방지).
        int sell_avail = pos;
        const int sell_per = p_.n_rungs > 0 ? (pos + p_.n_rungs - 1) / p_.n_rungs : pos; // ceil
        for (int i = 1; i <= p_.n_rungs && sell_avail > 0; ++i)
        {
            double sp = round_to_tick(sma * (1.0 + p_.dev_sell * i / 100.0), OrderSide::SELL);
            int q = sell_avail < sell_per ? sell_avail : sell_per;
            if (sp > 0.0 && q > 0)
            {
                plan.push_back({OrderSide::SELL, sp, q});
                sell_avail -= q;
            }
        }

        // 매수 밴드(물타기): 이격 −dev_buy%*i. rung당 자본의 rung_notional. 종목당 상한은 OrderGate가 캡.
        //  점진 진입: add_below_sma_only면 현재가가 3분봉 기준선 아래(실제 눌림)일 때만 물타기를 깐다.
        //  → 활성 즉시 base+물타기를 한꺼번에 예약해 1분 만에 10% 만재되던 성격을 제거. 기준선 위/근처에선
        //    base(+익절 매도레그)만 유지하고, 진짜 눌림이 와야 평단을 낮춘다.
        if (!p_.add_below_sma_only || cur_px < sma)
        {
            for (int i = 1; i <= p_.n_rungs; ++i)
            {
                double bp = round_to_tick(sma * (1.0 - p_.dev_buy * i / 100.0), OrderSide::BUY);
                int    rq = qty_for(rung_notional, bp);
                if (rq <= 0) rq = p_.step_qty;              // 자본 미상 폴백
                if (bp > 0.0 && rq > 0)
                    plan.push_back({OrderSide::BUY, bp, rq});
            }
        }

        // ── no-change 가드: 처닝 차단. 사다리가 살아있고 (a)계획 시그니처가 직전과 동일하거나
        //    (b)SMA 이동이 reprice_move_ticks 데드밴드 이내이고 포지션도 그대로면 재발주 스킵.
        //    (b)가 핵심: SMA가 틱경계를 스치며 미세이동할 때마다 전량 취소·재발주해 매도 rung이
        //    체결 전에 취소되고 매수만 쌓여 pos가 편증하던 처닝을 차단(reprice_move_ticks 구현).
        std::string sig;
        for (const auto& r : plan)
            sig += std::string(r.side == OrderSide::BUY ? "B" : "S") + fmt1(r.price) +
                   "x" + std::to_string(r.qty) + "|";
        // G1 국면 게이트: 비활성 국면(regime→전략 자동선택에서 미선택)에선 매수(진입·물타기)
        //  rung을 깔지 않는다. 익절 매도·청산은 국면과 무관하게 유지(is_active 계약: 진입만 차단).
        //  active 상태를 시그니처에 접미 → 국면 플립 시 no-change 가드에 걸리지 않고 재구성되어
        //  기존 매수 예약이 cancel_all로 취소된다(플립 후 매수만 잔존하는 구멍 차단).
        const bool entry_on = is_active();
        sig += entry_on ? "A1" : "A0";
        const double reprice_band = p_.reprice_move_ticks * tick_size(sma);
        const bool   sma_quiet    = last_sma_ > 0.0 && std::fabs(sma - last_sma_) < reprice_band;
        // (a) 계획 시그니처+pos가 직전과 동일하면 live 유무와 무관하게 스킵.
        //     매도가능=0이라 아무것도 못 깔아 live_가 빈 채로 남을 때(원장 보유↔매도가능 괴리)
        //     매 하트비트 재진입해 잔고조회를 난사하던 스핀을 차단. 체결로 pos가 바뀌면 즉시 재구성.
        // (b) 데드밴드(reprice 이내 미세이동)+pos 동일 스킵은 살아있는 사다리에만 적용.
        if (sig == last_ladder_sig_ && pos == last_pos_)
            return; // 동일 계획 → 유지(빈 계획 포함)
        if (!live_.empty() && sma_quiet && pos == last_pos_)
            return; // 데드밴드 내 미세이동 → 유지

        // ── 재구성: 기존 취소 후 신규 지정가 ──────────────────────────────────
        //  익절 매도는 재구성 시점의 실매도가능분(ord_psbl_qty)으로 클램프 → 원장 보유와
        //  매도가능 괴리(예약매도·미결제)로 KIS가 전량 거부(40240000 "잔고내역 없습니다")하던
        //  것을 차단. 청산 경로(emit_liquidation)와 동일 원칙. 잔고조회는 재구성 시 1회만
        //  (핫패스 부하 억제 — 매수는 캡을 OrderGate가 처리하므로 클램프 불필요).
        cancel_all(out);
        // G4: 이 사다리를 깐 판단 근거 — 존 판정 지표를 신호에 실어 영속(로그 재구성 불필요).
        const std::string buy_ctx  = "정배열눌림진입 이격=" + fmt1(s_dev) + "% 일봉SMA20=" +
                                     fmt1(d_s20) + " 현재가=" + fmt1(cur_px);
        int sell_room = -1;                          // -1=미조회(지연). 첫 매도 rung에서 1회 조회.
        for (const auto& r : plan)
        {
            if (r.side == OrderSide::SELL)
            {
                if (sell_room < 0)
                    sell_room = sellable_qty();      // 안전 우선: 불확실하면 0(매도 보류)
                int q = r.qty < sell_room ? r.qty : sell_room;
                if (q <= 0)
                    continue;                        // 매도가능 소진/없음 → 이 rung 스킵
                place(out, OrderSide::SELL, r.price, q, "익절밴드 지정가=" + fmt1(r.price));
                sell_room -= q;
            }
            else if (entry_on)                       // G1: 비활성 국면이면 매수 rung 스킵(진입 차단)
                place(out, r.side, r.price, r.qty, buy_ctx);
        }

        last_sma_ = sma;
        last_ladder_sig_ = sig;
        last_pos_ = pos;
        LOG_INFO("[" + id() + "] 사다리 재구성 sma=" + fmt1(sma) + " px=" + fmt1(cur_px) +
                 " pos=" + std::to_string(pos) + " live=" + std::to_string(live_.size()));
    }

private:
    // ── 지표 (indicators.py 이식, bars[0]=최신) ──────────────────────────────
    static double sma_close(const std::vector<MarketData>& bars, int period)
    {
        if (static_cast<int>(bars.size()) < period || period <= 0)
            return 0.0;
        double s = 0.0;
        for (int i = 0; i < period; ++i)
            s += bars[i].close;
        return s / period;
    }
    // 일봉 정배열: SMA5>SMA10>SMA20>SMA60 (모두 확보돼야 판정).
    static bool is_aligned(const std::vector<MarketData>& daily)
    {
        if (static_cast<int>(daily.size()) < 60)
            return false;
        double s5  = sma_close(daily, 5);
        double s10 = sma_close(daily, 10);
        double s20 = sma_close(daily, 20);
        double s60 = sma_close(daily, 60);
        return s5 > s10 && s10 > s20 && s20 > s60;
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
            if (kis_)
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
                    auto   d  = kis_->get_daily_ohlcv(p_.ticker, p_.daily_lookback);
                    double eq = fetch_equity();
                    if (!d.empty())
                    {
                        std::lock_guard<std::mutex> lk(snap_mtx_);
                        snap_daily_      = std::move(d);
                        snap_daily_date_ = today;
                        snap_equity_     = eq;
                    }
                }
                // 3분봉: 매 주기 갱신(기존 핫패스).
                auto bars = kis_->get_minute_ohlcv(p_.ticker, p_.sma_period + 1, p_.interval_min);
                if (!bars.empty())
                {
                    std::lock_guard<std::mutex> lk(snap_mtx_);
                    snap_bars_ = std::move(bars);
                }
            }
            // min_action_ms를 50ms 조각으로 자며 stop 신호에 빠르게 반응.
            for (int slept = 0;
                 slept < p_.min_action_ms && !prefetch_stop_.load(std::memory_order_relaxed);
                 slept += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void stop_prefetch()
    {
        prefetch_stop_.store(true, std::memory_order_relaxed);
        if (prefetch_thread_.joinable())
            prefetch_thread_.join();
    }

    // 사이징 기준 자본(총평가금) 조회. output2 tot_evlu_amt(없으면 nass_amt). 알 수 없으면 0.
    //  프리페치 스레드에서 호출(읽기전용). 폴백(fallback_equity) 적용은 호출측(on_trade_batch, line eq).
    double fetch_equity()
    {
        double eq = 0.0;
        if (kis_ && kis_->has_account())
        {
            try
            {
                nlohmann::json bal = kis_->get_balance();
                const nlohmann::json* row = nullptr;
                if (bal.contains("output2"))
                {
                    const auto& o2 = bal["output2"];
                    if (o2.is_array() && !o2.empty()) row = &o2[0];
                    else if (o2.is_object())          row = &o2;
                }
                if (row)
                {
                    std::string s = row->value("tot_evlu_amt", std::string());
                    if (s.empty()) s = row->value("nass_amt", std::string());
                    if (!s.empty()) { try { eq = std::stod(s); } catch (...) {} }
                }
            }
            catch (...) {}
        }
        return eq;
    }

    // 명목→수량(주). 가격/명목 유효하지 않으면 0(호출측이 폴백 결정).
    static int qty_for(double notional, double price)
    {
        if (price <= 0.0 || notional <= 0.0) return 0;
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
            return; // qty=0 NEW 발주 억제 — 게이트 거부·로그 노이즈 원천 차단(SELL은 상위서도 클램프)
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
            return false;
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
        s.reason      = reason; // G4: 청산 사유(존 이탈/EOD 등)를 신호에 실어 영속
        s.timestamp   = std::chrono::system_clock::now();
        return s;
    }

    // ── 청산 발주: 매도가능분 클램프 + 지수 백오프 ───────────────────────────
    //  버그 이력: 존이탈/EOD 청산이 원장 보유수량 전량을 시장가 매도했으나, 예약매도
    //  (고아/미결제)로 실매도가능분(ord_psbl_qty)이 보유보다 작으면 KIS가 전량 거부
    //  (40240000 "잔고내역 없습니다") → 매 하트비트 무한 재거부 스팸. 실계좌 동일.
    //  대책: (1) 매 시도 get_balance의 '실시간' 주문가능수량으로 클램프 → 잠긴 수량
    //  초과분 미발주(과매도·이중주문 위험 0, 브로커 상태 기준이라 체결지연에도 자기교정).
    //  (2) 시도 후 진행(pos 감소) 없으면 30·60·120·240·480s(cap 300s) 지수 백오프.
    //  반환: 시장가 매도를 실제로 out에 넣었으면 true.
    bool emit_liquidation(std::vector<OrderSignal>& out, int pos,
                          std::chrono::steady_clock::time_point now, const std::string& tag)
    {
        if (pos <= 0)
            return false;
        // 진행 판정: 직전 시도보다 pos가 줄었으면(부분체결) 백오프 리셋.
        if (liq_last_pos_ < 0 || pos < liq_last_pos_)
        {
            liq_fail_streak_ = 0;
            liq_next_ = std::chrono::steady_clock::time_point{};
        }
        if (liq_next_.time_since_epoch().count() != 0 && now < liq_next_)
            return false; // 백오프 창 내 — 재발주 스킵(스팸 차단)

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
        if (shift > 4) shift = 4;
        long long ms = 30000LL << shift;                 // 30/60/120/240/480…
        if (ms > 300000LL) ms = 300000LL;                // cap 5분
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
        if (!kis_ || !kis_->has_account())
            return 0;
        try
        {
            nlohmann::json bal = kis_->get_balance();
            if (!bal.contains("output1") || !bal["output1"].is_array())
                return 0;
            for (const auto& h : bal["output1"])
            {
                if (h.value("pdno", std::string()) != p_.ticker)
                    continue;
                std::string p = h.value("ord_psbl_qty", std::string());
                if (p.empty())
                    return 0;
                try { return std::stoi(p); } catch (...) { return 0; }
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
    std::string daily_date_;               // 캐시 기준일(KST YYYYMMDD)
    double equity_ = 0.0;                   // 사이징 기준 자본(총평가금) 스냅샷 — 일별 갱신
    double last_sma_ = 0.0;                // 마지막 재호가 기준 SMA
    int    last_pos_ = -1;                  // 마지막 재구성 시 포지션(데드밴드 가드)
    std::string last_ladder_sig_;          // 마지막 발주 사다리 시그니처(no-change 가드)
    std::chrono::steady_clock::time_point last_work_{};   // 스로틀
    bool last_zone_ = false;                              // 마지막 존 상태(변화 로그용)
    bool in_zone_   = false;                              // 존 히스테리시스 상태(진입/청산 임계 전환)
    std::chrono::steady_clock::time_point zone_log_ts_{}; // 마지막 존 판정 로그 시각
    std::chrono::steady_clock::time_point liq_next_{};    // 청산 재시도 백오프 해제 시각
    int    liq_last_pos_    = -1;                          // 직전 청산시도 pos(진행 판정)
    int    liq_fail_streak_ = 0;                           // 연속 미진행 횟수(백오프 지수)
    uint64_t seq_ = 0;

    // ── 프리페치(무거운 REST를 공유 전략 스레드 밖으로) ──────────────────────
    std::thread             prefetch_thread_;
    std::atomic<bool>       prefetch_stop_{false};
    std::mutex              snap_mtx_;               // 아래 snap_* 보호
    std::vector<MarketData> snap_daily_;             // 일봉 스냅샷
    std::string             snap_daily_date_;        // 스냅샷 기준일(KST YYYYMMDD)
    double                  snap_equity_ = 0.0;      // 자본 스냅샷(raw, 폴백 미적용)
    std::vector<MarketData> snap_bars_;              // 3분봉 스냅샷
};
