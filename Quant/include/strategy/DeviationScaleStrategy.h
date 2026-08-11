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
        last_ladder_sig_.clear();
        in_zone_ = false;
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
            bool acted = cancel_all(out);
            int pos = confirmed_position(p_.account, p_.ticker);
            if (pos > 0)
            {
                out.push_back(make_market_sell(pos));
                acted = true;
            }
            if (acted)
                LOG_INFO("[" + id() + "] EOD(" + std::to_string(hhmm) + ") — 전량 취소+청산 pos=" +
                         std::to_string(pos));
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
        const double d_dev   = d_s20 > 0.0 ? std::fabs(cur_px - d_s20) / d_s20 * 100.0 : -1.0;
        // 히스테리시스: 진입은 좁게(pullback_pct), 한번 활성이면 넓게(pullback+hyst)까지 유지.
        //   절대이격이라 경계에서 0-폭 활성↔대기 진동 → 매수 직후 시장가 청산되던 휩쏘 방지.
        const double enter_th = p_.pullback_pct;
        const double exit_th  = p_.pullback_pct + p_.zone_hyst_pct;
        const double zone_th  = in_zone_ ? exit_th : enter_th;
        const bool   zone     = aligned && d_s20 > 0.0 && d_dev <= zone_th;
        in_zone_ = zone;

        // 존 판정 가시화: 상태 변화 시 또는 60초마다 1회(관찰용).
        if (zone != last_zone_ || zone_log_ts_.time_since_epoch().count() == 0 ||
            now - zone_log_ts_ >= std::chrono::seconds(60))
        {
            LOG_INFO("[" + id() + "] 존 판정 " + std::string(zone ? "활성" : "대기") +
                     " | 정배열=" + std::string(aligned ? "Y" : "N") +
                     " 일봉SMA20=" + fmt1(d_s20) + " 현재가=" + fmt1(cur_px) +
                     " 이격=" + fmt1(d_dev) + "% (한도 " + fmt1(zone_th) +
                     "% 진입" + fmt1(enter_th) + "/청산" + fmt1(exit_th) +
                     ") 일봉수=" + std::to_string(daily_.size()));
            last_zone_    = zone;
            zone_log_ts_  = now;
        }

        if (!zone)
        {
            // 존 이탈 → 미체결 전부 취소 + 보유분 시장가 청산.
            bool acted = cancel_all(out);
            int pos = confirmed_position(p_.account, p_.ticker);
            if (pos > 0)
            {
                out.push_back(make_market_sell(pos));
                acted = true;
            }
            if (acted)
                LOG_INFO("[" + id() + "] 존 이탈 — 취소+청산 pos=" + std::to_string(pos));
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

        // ── no-change 가드: 시그니처(side:price:qty 나열)가 직전과 동일하고
        //    사다리가 살아있으면 취소·재발주 스킵(처닝 차단). ──────────────────
        std::string sig;
        for (const auto& r : plan)
            sig += std::string(r.side == OrderSide::BUY ? "B" : "S") + fmt1(r.price) +
                   "x" + std::to_string(r.qty) + "|";
        if (sig == last_ladder_sig_ && !live_.empty())
            return; // 동일 사다리 → 유지

        // ── 재구성: 기존 취소 후 신규 지정가 ──────────────────────────────────
        cancel_all(out);
        for (const auto& r : plan)
            place(out, r.side, r.price, r.qty);

        last_sma_ = sma;
        last_ladder_sig_ = sig;
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

    bool zone_active(double cur_px) const
    {
        if (!is_aligned(daily_))
            return false;
        double s20 = sma_close(daily_, 20);
        if (s20 <= 0.0)
            return false;
        double dev = std::fabs(cur_px - s20) / s20 * 100.0; // 일봉 20MA 대비 이격(%)
        return dev <= p_.pullback_pct;                      // 눌림목(기준선 ±pullback_pct 이내)
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
    std::string last_ladder_sig_;          // 마지막 발주 사다리 시그니처(no-change 가드)
    std::chrono::steady_clock::time_point last_work_{};   // 스로틀
    bool last_zone_ = false;                              // 마지막 존 상태(변화 로그용)
    bool in_zone_   = false;                              // 존 히스테리시스 상태(진입/청산 임계 전환)
    std::chrono::steady_clock::time_point zone_log_ts_{}; // 마지막 존 판정 로그 시각
    uint64_t seq_ = 0;
};
