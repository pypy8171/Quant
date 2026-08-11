#pragma once
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// IntradayBreakoutStrategy (ITB v2)  —  장중 채널 돌파 + 물린분/신규분 분리 청산
//
//  협의체(전략·아키텍처·데이터·리스크) 확정 스펙(strategies/ITB/SPEC.md §2).
//  입력은 오직 WS/REST 체결 틱(on_trade) — 깨진 REST 일봉 경로(G1/G2)를 우회한다.
//
//  [입력]  H0STCNT0 체결 틱 → on_trade(TradeData). td.price=현재가, td.time=HHMMSS.
//  [진입]  1분 버킷 종가가 최근 N분 채널 고점을 상향 돌파 + 당일 앵커 대비 +eps 위
//          → 시장가 신규 매수. 버킷 마감 시에만 평가(틱 노이즈/휩쏘 억제).
//          수량은 notional_per_position>0이면 floor(명목/현재가), 아니면 entry_qty 고정.
//          앵커는 day_open_px(>0) 주입 시 당일 시가, 아니면 첫 관측 틱(자기참조 방지).
//  [청산]  포지션 성격에 따라 분기(매 틱, 손실통제 우선):
//    (A) 물린 보유분(position_is_seed_): 넓은 앵커 트레일 seed_trail_pct + 본전근처
//        반등 청산 exit_near_avg_pct. 이미 -30% 물린 평단에 -3% 하드손절을 걸어 개장
//        즉시 시장가 투매하는 자해(v1 결함)를 제거. 반등에 실어 던진다.
//    (B) 신규 진입분: 타이트 트레일 trail_pct + 진입가 하드손절 hard_pct.
//    공통: 평단손절(avg_loss_pct, 보통 0=비활성), EOD(eod_hhmm) 강제청산.
//  [신규진입 금지]  no_new_entry_hhmm(>0이면 이 시각부터, 아니면 eod_hhmm) 이후 진입 금지
//          — 마감 임박 진입은 트레일 발동 전 EOD 강제청산되므로.
//
//  안전장치: 재진입 쿨다운으로 청산 직후 재매수 폭주 방지. account_id 기본 "" → OrderGate
//           원장 시드 키 일치(C-1). 신규 진입 후 position_is_seed_=false로 성격 전환.
// ─────────────────────────────────────────────────────────────────────────────
class IntradayBreakoutStrategy : public StrategyBase
{
public:
    IntradayBreakoutStrategy(std::string ticker, int entry_qty, int hold_qty,
                             bool start_in_position, int channel_min = 10,
                             double breakout_eps = 0.002, double trail_pct = 0.010,
                             double hard_pct = 0.015, int eod_hhmm = 1515,
                             int reentry_cooldown_sec = 60,
                             double avg_px = 0.0, double avg_loss_pct = 0.0,
                             // ── v2 추가 (뒤에 붙여 하위호환) ──
                             double seed_trail_pct = 0.0,      // 물린분 앵커 트레일(0→trail_pct)
                             double exit_near_avg_pct = 0.0,   // 물린분 본전탈출(평단 -x% 이내, 0=비활성)
                             int no_new_entry_hhmm = 0,        // 신규진입 금지 시각(0→eod_hhmm)
                             double notional_per_position = 0.0, // 종목당 명목(0→entry_qty 고정)
                             double day_open_px = 0.0)         // 당일 시가 앵커 주입(0→첫 틱)
        : ticker_(std::move(ticker)), entry_qty_(entry_qty), hold_qty_(hold_qty),
          start_in_position_(start_in_position), channel_min_(channel_min),
          eps_(breakout_eps), trail_pct_(trail_pct), hard_pct_(hard_pct),
          eod_hhmm_(eod_hhmm), cooldown_sec_(reentry_cooldown_sec),
          avg_px_(avg_px), avg_loss_pct_(avg_loss_pct),
          seed_trail_pct_(seed_trail_pct), exit_near_avg_pct_(exit_near_avg_pct),
          no_new_entry_hhmm_(no_new_entry_hhmm), notional_per_position_(notional_per_position),
          day_open_px_(day_open_px)
    {
    }

    std::string id() const override { return "ITB_" + ticker_; }

    // 로그 확인 편의용 표시명(종목명). id()/dedup 키는 ticker 기반 유지 — 로깅에만 사용.
    void set_name(std::string n) { name_ = std::move(n); }
    std::string tag() const { return name_.empty() ? ticker_ : (ticker_ + " " + name_); }

    std::string describe() const override
    {
        return "ITB | " + ticker_ + " | ch=" + std::to_string(channel_min_) + "m eps=" +
               std::to_string(eps_) + " trail=" + std::to_string(trail_pct_) + " hard=" +
               std::to_string(hard_pct_) + " seed_trail=" + std::to_string(seed_trail_pct_) +
               " exit_avg=" + std::to_string(exit_near_avg_pct_) + " eod=" + std::to_string(eod_hhmm_) +
               " no_entry=" + std::to_string(no_new_entry_hhmm_) + " notional=" +
               std::to_string(static_cast<long long>(notional_per_position_)) + " hold=" +
               std::to_string(hold_qty_);
    }

    // 체결만 구독(호가 제외) — 구독 한도 절약.
    std::vector<WatchSpec> get_watch_specs() const override
    {
        return {{ticker_, Market::KR, "", true}};
    }

    void on_start() override
    {
        closes_.clear();
        cur_hhmm_ = -1;
        cur_bucket_last_ = 0.0;
        anchor_px_ = 0.0;
        last_ = 0.0;
        in_position_ = start_in_position_;
        position_is_seed_ = start_in_position_; // 기동 보유분 = 물린 시드분
        entry_px_ = 0.0;
        peak_ = 0.0;
        have_cooldown_ = false;
    }

    // 일봉 경로 미사용(라이브 소스는 WS 체결) — 순수가상 요건 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        if (td.ticker != ticker_)
            return std::nullopt;
        double px = td.price;
        if (px <= 0.0)
            return std::nullopt; // 방어: 잘못된 틱
        last_ = px;

        int hhmmss = parse_hhmmss(td.time);
        int hhmm = hhmmss / 100;

        // 당일 앵커 — day_open 주입 우선, 아니면 첫 유효 틱. 신규 돌파 기준가.
        if (anchor_px_ <= 0.0)
        {
            anchor_px_ = (day_open_px_ > 0.0 ? day_open_px_ : px);
            if (in_position_) // 보유분: 트레일은 현재가 기준(물린 평단 무시 → 개장 투매 방지)
            {
                entry_px_ = px;
                peak_ = px;
            }
        }

        // ── 청산: 보유 중이면 매 틱 스탑 평가 (손실통제 우선) ──────────────
        if (in_position_ && hold_qty_ > 0)
        {
            if (px > peak_)
                peak_ = px;

            bool hit = false;
            const char* why = " (stop)";

            if (position_is_seed_)
            {
                // (A) 물린 보유분: 넓은 앵커 트레일 + 본전근처 반등 청산.
                double strail = (seed_trail_pct_ > 0.0 ? seed_trail_pct_ : trail_pct_);
                double seed_trail_stop = peak_ * (1.0 - strail);
                if (px <= seed_trail_stop)
                {
                    hit = true;
                    why = " (seed-trail)";
                }
                else if (exit_near_avg_pct_ > 0.0 && avg_px_ > 0.0 &&
                         px < avg_px_ && // 상단 가드: 아직 물린(underwater) 상태에서만
                         px >= avg_px_ * (1.0 - exit_near_avg_pct_))
                {
                    // 본전탈출 = "물린 보유분이 평단 근처까지 회복하면 재하락 전에 탈출".
                    //  밴드: avg*(1-pct) ≤ px < avg. 상단 가드(px<avg)가 없으면 평단 위(수익)
                    //  포지션도 본전에서 청산돼 상방을 스스로 잘라먹는다 → 수익 구간은 seed-trail에 태운다.
                    hit = true;
                    why = " (본전탈출)";
                }
            }
            else
            {
                // (B) 신규 진입분: 타이트 트레일 + 진입가 하드손절.
                double trail_stop = peak_ * (1.0 - trail_pct_);
                double hard_stop = entry_px_ * (1.0 - hard_pct_);
                if (px <= trail_stop)
                {
                    hit = true;
                    why = " (trail)";
                }
                else if (px <= hard_stop)
                {
                    hit = true;
                    why = " (hard)";
                }
            }

            // 평단 손절(opt-in, 보통 0=비활성) — 성격 무관 실제 손실률 초과 시 청산.
            if (!hit && avg_loss_pct_ > 0.0 && avg_px_ > 0.0 && px <= avg_px_ * (1.0 - avg_loss_pct_))
            {
                hit = true;
                why = " (평단손절)";
            }

            bool eod = hhmm >= eod_hhmm_;
            if (hit || eod)
            {
                auto sig = make_signal(OrderSide::SELL, hold_qty_, px, td.timestamp);
                if (eod && !hit)
                    why = " (EOD)";
                LOG_INFO("[ITB] SELL " + tag() + " qty=" + std::to_string(hold_qty_) + " @" +
                         px_str(px) + why);
                in_position_ = false;
                hold_qty_ = 0;
                position_is_seed_ = false;
                cooldown_until_ = td.timestamp + std::chrono::seconds(cooldown_sec_);
                have_cooldown_ = true;
                return sig;
            }
        }

        // ── 진입: 1분 버킷 마감 시에만 평가 ──────────────────────────────
        if (cur_hhmm_ < 0)
        {
            cur_hhmm_ = hhmm;
            cur_bucket_last_ = px;
            return std::nullopt;
        }
        if (hhmm == cur_hhmm_)
        {
            cur_bucket_last_ = px; // 같은 버킷: 종가 갱신만
            return std::nullopt;
        }

        // 버킷 롤오버: 직전 버킷 종가 확정
        double bucket_close = cur_bucket_last_;
        std::optional<OrderSignal> sig;

        if (!in_position_ && is_active() && static_cast<int>(closes_.size()) >= channel_min_)
        {
            double hi_n = *std::max_element(closes_.begin(), closes_.end());
            bool cooldown_ok = !have_cooldown_ || td.timestamp >= cooldown_until_;
            int no_entry_hhmm = (no_new_entry_hhmm_ > 0 ? no_new_entry_hhmm_ : eod_hhmm_);
            bool session_ok = hhmm < no_entry_hhmm; // 마감 임박 신규진입 금지
            if (cooldown_ok && session_ok && bucket_close > hi_n &&
                bucket_close > anchor_px_ * (1.0 + eps_))
            {
                int qty = entry_qty_;
                if (notional_per_position_ > 0.0 && bucket_close > 0.0)
                    qty = std::max(1, static_cast<int>(std::floor(notional_per_position_ / bucket_close)));
                sig = make_signal(OrderSide::BUY, qty, bucket_close, td.timestamp);
                LOG_INFO("[ITB] BUY " + tag() + " qty=" + std::to_string(qty) + " @" +
                         px_str(bucket_close) + " (돌파 hiN=" + px_str(hi_n) + ")");
                in_position_ = true;
                position_is_seed_ = false; // 신규 진입분 — 타이트 스탑 적용
                hold_qty_ = qty;
                entry_px_ = bucket_close;
                peak_ = bucket_close;
            }
        }

        // 채널 갱신(완성 버킷만 유지, 최근 N분)
        closes_.push_back(bucket_close);
        while (static_cast<int>(closes_.size()) > channel_min_)
            closes_.pop_front();

        cur_hhmm_ = hhmm;
        cur_bucket_last_ = px;
        return sig;
    }

private:
    OrderSignal make_signal(OrderSide side, int qty, double px,
                            std::chrono::system_clock::time_point ts)
    {
        OrderSignal s;
        s.ticker = ticker_;
        s.side = side;
        s.type = OrderType::MARKET;
        s.quantity = qty;
        s.price = px; // MARKET은 미사용이나 로깅·명목 상한 계산·향후 LIMIT 대비
        s.market = Market::KR;
        s.strategy_id = id();
        s.timestamp = ts;
        return s; // account_id="" (기본) — OrderGate 원장 시드 계좌키와 일치(C-1)
    }

    static int parse_hhmmss(const std::string& t)
    {
        if (t.size() < 6)
            return 0;
        try
        {
            return std::stoi(t.substr(0, 6));
        }
        catch (...)
        {
            return 0;
        }
    }

    static std::string px_str(double v) { return std::to_string(static_cast<long long>(std::llround(v))); }

    std::string ticker_;
    std::string name_; // 표시명(로깅 전용)
    int entry_qty_;   // 신규 돌파 진입 수량(명목 미지정 시 고정)
    int hold_qty_;    // 현재 보유수량(시드분 또는 진입분) — 매도 전량 기준
    bool start_in_position_;
    int channel_min_; // 채널 창(완성 1분 버킷 개수)
    double eps_;      // 진입 버퍼(앵커 대비)
    double trail_pct_;
    double hard_pct_;
    int eod_hhmm_;    // 마감 강제청산 기준(KST HHMM)
    int cooldown_sec_;
    double avg_px_ = 0.0;       // 매입 평단(시드분) — 평단손절/본전탈출 기준가
    double avg_loss_pct_ = 0.0; // 평단 대비 손절률(0=비활성)
    // ── v2 ──
    double seed_trail_pct_ = 0.0;       // 물린분 앵커 트레일(넓게)
    double exit_near_avg_pct_ = 0.0;    // 물린분 본전탈출 임계(평단 -x% 이내 반등)
    int no_new_entry_hhmm_ = 0;         // 신규진입 금지 시각(0→eod_hhmm)
    double notional_per_position_ = 0.0; // 종목당 명목(원)
    double day_open_px_ = 0.0;          // 당일 시가 앵커 주입

    std::deque<double> closes_; // 완성 1분 버킷 종가(최근 channel_min_개)
    int cur_hhmm_ = -1;         // 현재 집계 중 버킷(HHMM)
    double cur_bucket_last_ = 0.0;
    double anchor_px_ = 0.0; // 당일 앵커(시가 또는 첫 틱)
    double last_ = 0.0;
    bool in_position_ = false;
    bool position_is_seed_ = false; // 현재 포지션이 물린 시드분인가(청산 로직 분기)
    double entry_px_ = 0.0;
    double peak_ = 0.0;
    bool have_cooldown_ = false;
    std::chrono::system_clock::time_point cooldown_until_;
};
