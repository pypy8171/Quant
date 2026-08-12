#pragma once
#include "api/KisClient.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
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
        int    base_qty       = 10;    // 존 진입 베이스 매수 수량(목표의 절반 개념)
        int    step_qty       = 5;     // 각 밴드(rung) 분할 수량
        int    sma_period     = 20;    // 3분봉 기준선 SMA 기간
        double dev_sell       = 1.5;   // 매도 밴드 이격도(%) — 층당 배수
        double dev_buy        = 0.8;   // 매수 밴드 이격도(%) — 층당 배수
        int    n_rungs        = 2;     // 밴드 층수
        double pullback_pct   = 2.0;   // 일봉 SMA20 눌림 허용폭(±%) — 존 진입 임계
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

    std::string id() const override { return "DEVSCALE_" + p_.ticker; }

    std::string describe() const override
    {
        return "DeviationScale | " + p_.ticker + " | base=" + std::to_string(p_.base_qty) +
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
        seq_ = 0;
        LOG_INFO("[" + id() + "] 시작 — " + describe());
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

        // ── 일봉 존 판정(정배열 + SMA20 눌림) ────────────────────────────────
        refresh_daily_if_needed();
        const bool   aligned = is_aligned(daily_);
        const double d_s20   = sma_close(daily_, 20);
        // 방향성 이격(부호 유지): +면 SMA20 위(확장추격), −면 아래(눌림). 절대값 금지.
        const double s_dev   = d_s20 > 0.0 ? (cur_px - d_s20) / d_s20 * 100.0 : 999.0;
        // 방향성 눌림 게이트: 진입은 "SMA20 이하(≤0%) ~ pullback_pct 아래"의 눌림 구간에서만.
        //   정배열 상승추세에서 SMA20으로 되돌린(눌린) 자리만 잡고, 위로 벌어진 확장은 배제.
        //   히스테리시스: 활성이면 상단 +zone_hyst, 하단 −(pullback+zone_hyst)까지 유지(경계 진동 방지).
        const double up_th   = in_zone_ ? p_.zone_hyst_pct : 0.0;            // 진입 상단=SMA20(0%), 유지=+hyst
        const double down_th = in_zone_ ? p_.pullback_pct + p_.zone_hyst_pct // 유지 하단
                                        : p_.pullback_pct;                   // 진입 하단
        const bool   zone    = aligned && d_s20 > 0.0 && s_dev <= up_th && s_dev >= -down_th;
        in_zone_ = zone;

        // 존 판정 가시화: 상태 변화 시 또는 60초마다 1회(관찰용).
        if (zone != last_zone_ || zone_log_ts_.time_since_epoch().count() == 0 ||
            now - zone_log_ts_ >= std::chrono::seconds(60))
        {
            LOG_INFO("[" + id() + "] 존 판정 " + std::string(zone ? "활성" : "대기") +
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

        // ── 3분봉 기준선 ─────────────────────────────────────────────────────
        if (!kis_)
            return;
        std::vector<MarketData> bars = kis_->get_minute_ohlcv(p_.ticker, p_.sma_period + 1, p_.interval_min);
        if (static_cast<int>(bars.size()) < p_.sma_period)
            return; // 봉 부족 — 다음 하트비트 재시도
        const double sma = sma_close(bars, p_.sma_period); // bars[0]=최신
        if (sma <= 0.0)
            return;
        int pos = confirmed_position(p_.account, p_.ticker);

        // ── 목표 사다리 산출(발주 전) ─────────────────────────────────────────
        //  계단 가격·수량은 sma·pos의 순수 함수. 먼저 계획을 만들고 직전 사다리와
        //  시그니처를 비교해 "동일하면 재발주 스킵". 분봉 정지(HTTP 500 폴백)로
        //  sma=px가 고정될 때 동일 사다리를 취소·재발주하던 처닝을 근본 차단.
        struct Rung { OrderSide side; double price; int qty; };
        std::vector<Rung> plan;

        // 베이스: 무포지션이면 기준선 근처 지정가 매수(목표 절반).
        if (pos <= 0)
        {
            double bp = round_to_tick(sma, OrderSide::BUY);
            if (bp > 0.0)
                plan.push_back({OrderSide::BUY, bp, p_.base_qty});
        }

        // 매도 밴드: 이격 +dev_sell%*i. 보유분 한도 내에서만(숏 방지).
        int sell_avail = pos;
        for (int i = 1; i <= p_.n_rungs && sell_avail > 0; ++i)
        {
            double sp = round_to_tick(sma * (1.0 + p_.dev_sell * i / 100.0), OrderSide::SELL);
            int q = sell_avail < p_.step_qty ? sell_avail : p_.step_qty;
            if (sp > 0.0 && q > 0)
            {
                plan.push_back({OrderSide::SELL, sp, q});
                sell_avail -= q;
            }
        }

        // 매수 밴드: 이격 −dev_buy%*i. 순보유 한도는 OrderGate가 캡.
        for (int i = 1; i <= p_.n_rungs; ++i)
        {
            double bp = round_to_tick(sma * (1.0 - p_.dev_buy * i / 100.0), OrderSide::BUY);
            if (bp > 0.0)
                plan.push_back({OrderSide::BUY, bp, p_.step_qty});
        }

        // ── no-change 가드: 처닝 차단. 사다리가 살아있고 (a)계획 시그니처가 직전과 동일하거나
        //    (b)SMA 이동이 reprice_move_ticks 데드밴드 이내이고 포지션도 그대로면 재발주 스킵.
        //    (b)가 핵심: SMA가 틱경계를 스치며 미세이동할 때마다 전량 취소·재발주해 매도 rung이
        //    체결 전에 취소되고 매수만 쌓여 pos가 편증하던 처닝을 근본 차단(reprice_move_ticks 구현).
        std::string sig;
        for (const auto& r : plan)
            sig += std::string(r.side == OrderSide::BUY ? "B" : "S") + fmt1(r.price) +
                   "x" + std::to_string(r.qty) + "|";
        const double reprice_band = p_.reprice_move_ticks * tick_size(sma);
        const bool   sma_quiet    = last_sma_ > 0.0 && std::fabs(sma - last_sma_) < reprice_band;
        if (!live_.empty() && (sig == last_ladder_sig_ || (sma_quiet && pos == last_pos_)))
            return; // 동일 사다리 또는 데드밴드 내 미세이동 → 유지

        // ── 재구성: 기존 취소 후 신규 지정가 ──────────────────────────────────
        cancel_all(out);
        for (const auto& r : plan)
            place(out, r.side, r.price, r.qty);

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

    void refresh_daily_if_needed()
    {
        std::string today = kst_ymd();
        if (daily_date_ == today && !daily_.empty())
            return;
        if (!kis_)
            return;
        auto d = kis_->get_daily_ohlcv(p_.ticker, p_.daily_lookback);
        if (!d.empty())
        {
            daily_ = std::move(d);
            daily_date_ = today;
        }
    }

    // ── KRX 호가단위(2023 통합) — MM-1과 동일 ────────────────────────────────
    static double tick_size(double price)
    {
        if (price < 2000)    return 1;
        if (price < 5000)    return 5;
        if (price < 20000)   return 10;
        if (price < 50000)   return 50;
        if (price < 200000)  return 100;
        if (price < 500000)  return 500;
        return 1000;
    }
    static double round_to_tick(double p, OrderSide side)
    {
        const double t = tick_size(p);
        if (t <= 0.0)
            return p;
        return (side == OrderSide::BUY) ? std::floor(p / t) * t : std::ceil(p / t) * t;
    }

    std::string next_oid(const char* tag)
    {
        return id() + ":" + tag + ":" + std::to_string(++seq_);
    }

    void place(std::vector<OrderSignal>& out, OrderSide side, double price, int qty)
    {
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

    OrderSignal make_market_sell(int qty)
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
            out.push_back(make_market_sell(q));
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
};
