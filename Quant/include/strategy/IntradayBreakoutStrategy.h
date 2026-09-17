#pragma once
#include "strategy/SeedPeakStore.h"
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
//  협의체(전략·아키텍처·데이터·리스크) 확정 스펙(strategies/ITB/SPEC.market_data §2).
//  입력은 오직 WS/REST 체결 틱(on_trade) — 깨진 REST 일봉 경로(G1/G2)를 우회한다.
//
//  [입력]  국내 실시간 체결 채널(H0STCNT0) 틱을 on_trade(TradeData)로 받는다. trade.price=현재가, trade.hhmmss=HHMMSS 정수.
//  [진입]  1분 버킷 종가가 최근 N분 채널 고점을 상향 돌파 + 당일 앵커 대비 +epsilon 위
//          → 시장가 신규 매수. 버킷 마감 시에만 평가(틱 노이즈/휩쏘 억제).
//          수량은 notional_per_position>0이면 floor(명목/현재가), 아니면 entry_quantity 고정.
//          앵커는 day_open_price(>0) 주입 시 당일 시가, 아니면 첫 관측 틱(자기참조 방지).
//  [청산]  포지션 성격에 따라 분기(매 틱, 손실통제 우선):
//    (A) 물린 보유분(position_is_seed_): 고점 기준 넓은 트레일링 스탑 seed_trail_percent + 본전근처
//        반등 청산 exit_near_average_percent. 이미 -30% 물린 평단에 -3% 하드손절을 걸어 개장
//        즉시 시장가 투매하는 자해(v1 결함)를 제거. 반등에 실어 던진다.
//    (B) 신규 진입분: 타이트 트레일 trail_percent + 진입가 하드손절 hard_percent.
//    공통: 평단손절(average_loss_percent, 보통 0=비활성), 장 마감(eod_hhmm) 강제청산.
//  [신규진입 금지]  no_new_entry_hhmm(>0이면 이 시각부터, 아니면 eod_hhmm) 이후 진입 금지
//          — 마감 임박 진입은 트레일 발동 전 장 마감 강제청산되므로.
//
//  안전장치: 재진입 쿨다운으로 청산 직후 재매수 폭주 방지. account_id 기본 "" → OrderGate
//           원장 시드 키 일치(C-1). 신규 진입 후 position_is_seed_=false로 성격 전환.
// ─────────────────────────────────────────────────────────────────────────────
class IntradayBreakoutStrategy : public StrategyBase
{
public:
    IntradayBreakoutStrategy(std::string ticker, int entry_quantity, int hold_quantity,
                             bool start_in_position, int channel_min = 10,
                             double breakout_epsilon = 0.002, double trail_percent = 0.010,
                             double hard_percent = 0.015, int eod_hhmm = 1515,
                             int reentry_cooldown_sec = 60,
                             double average_price = 0.0, double average_loss_percent = 0.0,
                             // ── v2 추가 (뒤에 붙여 하위호환) ──
                             double seed_trail_percent = 0.0,      // 물린분 고점 기준 트레일(0→trail_percent)
                             double exit_near_average_percent = 0.0,   // 물린분 본전탈출(평단 -x% 이내, 0=비활성)
                             int no_new_entry_hhmm = 0,        // 신규진입 금지 시각(0→eod_hhmm)
                             double notional_per_position = 0.0, // 종목당 명목(0→entry_quantity 고정)
                             double day_open_price = 0.0)         // 당일 시가 앵커 주입(0→첫 틱)
        : ticker_(std::move(ticker)), entry_quantity_(entry_quantity), hold_quantity_(hold_quantity),
          start_in_position_(start_in_position), channel_min_(channel_min),
          epsilon_(breakout_epsilon), trail_percent_(trail_percent), hard_percent_(hard_percent),
          eod_hhmm_(eod_hhmm), cooldown_sec_(reentry_cooldown_sec),
          average_price_(average_price), average_loss_percent_(average_loss_percent),
          seed_trail_percent_(seed_trail_percent), exit_near_average_percent_(exit_near_average_percent),
          no_new_entry_hhmm_(no_new_entry_hhmm), notional_per_position_(notional_per_position),
          day_open_price_(day_open_price)
    {
        id_ = "ITB_" + ticker_;
    }

    const std::string& id() const override { return id_; }

    // 표시명(종목명) — 로깅 전용. id()/deduplicate 키는 ticker 기반 유지.
    void set_name(std::string name) { name_ = std::move(name); }

    // 본전탈출 무장 임계 — 이 깊이만큼 실제로 물려 본 적이 있어야 본전탈출이 켜진다.
    //  0이면 무장 조건 없음(예전 동작). 재기동 직후 평단 -0.2% 보유분이 첫 틱에
    //  전량 청산되던 사고가 여기서 나왔다 — 그건 "물린" 것이 아니라 그냥 본전이다.
    void set_exit_near_average_arm(double percent) { exit_near_average_arm_percent_ = percent; }
    // 부착 직후 보호구간 — 이 시간 동안은 청산 관리 청산을 내지 않는다. 재기동 첫 틱과
    //  국면 배선(RegimeSelect) 적용 사이의 경합으로 투매가 나가는 것을 막는다.
    void set_guard_warmup_sec(int guard_warmup_sec) { guard_warmup_sec_ = guard_warmup_sec; }
    // 이월 보유분 평단 하드스톱(조합안) — 평단 −hard_percent 아래를 완성 1분봉 종가 confirm_bars 개가 연속 확인하고
    //  from_hhmm 이후일 때만. 부착 때 이미 skip_percent 넘게 물린 구형 보유는 대상에서 뺀다(개장 투매 방지). [why D-082]
    void set_seed_hard_stop(double hard_percent, double skip_percent, int from_hhmm, int confirm_bars)
    {
        seed_hard_percent_          = hard_percent;
        seed_hard_skip_percent_     = skip_percent;
        seed_hard_from_hhmm_    = from_hhmm;
        seed_hard_confirm_bars_ = confirm_bars;
    }

    std::string tag() const { return name_.empty() ? ticker_ : (ticker_ + " " + name_); }

    std::string describe() const override
    {
        return "ITB | " + ticker_ + " | ch=" + std::to_string(channel_min_) + "m eps=" +
               std::to_string(epsilon_) + " trail=" + std::to_string(trail_percent_) + " hard=" +
               std::to_string(hard_percent_) + " seed_trail=" + std::to_string(seed_trail_percent_) +
               " exit_avg=" + std::to_string(exit_near_average_percent_) + " eod=" + std::to_string(eod_hhmm_) +
               " no_entry=" + std::to_string(no_new_entry_hhmm_) + " notional=" +
               std::to_string(static_cast<long long>(notional_per_position_)) + " hold=" +
               std::to_string(hold_quantity_);
    }

    // 체결만 구독(호가 제외) — 구독 한도 절약.
    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{ticker_, Market::KR, "", true}};
    }

    void on_start() override
    {
        symbol_id_ = symbol_of(ticker_);
        closes_.clear();
        current_hhmm_ = -1;
        current_bucket_last_ = 0.0;
        anchor_price_ = 0.0;
        last_ = 0.0;
        in_position_ = start_in_position_;
        position_is_seed_ = start_in_position_; // 기동 보유분 = 물린 시드분
        // 시드분은 잔고에서 읽어 온 것이라 원장이 이미 인정한 보유다. false로 두면 교체 진입이
        //  이 종목을 먼저 팔았을 때 원장 0을 "아직 안 보임"으로 읽고 없는 21주를 또 판다
        //  (09-11 001820, KIS 40240000 거부). [why D-046]
        ledger_confirmed_ = start_in_position_;
        entry_price_ = 0.0;
        peak_ = 0.0;
        // 시드분은 당일 저장 고점을 이어받는다 — 재기동마다 첫 틱으로 다시 잡으면 트레일이 무력화된다. [why D-052]
        saved_peak_ = position_is_seed_ ? SeedPeakStore::load(ticker_) : 0.0;
        last_saved_peak_ = saved_peak_;
        trough_ = 0.0;
        start_tp_ = std::chrono::steady_clock::now();
        have_cooldown_ = false;
        exit_pending_ = false;
        exit_retries_ = 0;
        exit_backoff_sec_ = kExitBackoffFirstSec;
        exit_next_retry_ = std::chrono::steady_clock::time_point{};
        exit_why_.clear();
    }

    // 일봉 경로 미사용(라이브 소스는 WS 체결) — 순수가상 요건 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        if (!same_symbol(symbol_id_, ticker_, trade.symbol_id, trade.ticker))
        {
            return std::nullopt;
        }

        double price = trade.price;

        if (price <= 0.0)
        {
            return std::nullopt; // 방어: 잘못된 틱
        }

        last_ = price;

        int hhmmss = trade.hhmmss;
        int hhmm = hhmmss / 100;

        // 당일 앵커 — day_open 주입 우선, 아니면 첫 유효 틱. 신규 돌파 기준가.
        if (anchor_price_ <= 0.0)
        {
            anchor_price_ = (day_open_price_ > 0.0 ? day_open_price_ : price);

            if (in_position_) // 보유분: 트레일은 현재가 기준(물린 평단 무시 → 개장 투매 방지)
            {
                entry_price_ = price;
                peak_ = price;

                if (position_is_seed_ && saved_peak_ > price)
                {
                    peak_ = saved_peak_;
                    LOG_INFO("[ITB] 시드 고점 복원 " + tag() + " peak=" + price_string(peak_) +
                             " (첫 틱 " + price_string(price) + ")");
                }
                else if (position_is_seed_)
                {
                    LOG_INFO("[ITB] 시드 앵커 " + tag() + " 첫 틱 " + price_string(price) + " (저장 고점 없음)");
                    // 첫 틱이 곧 고점이면 그것도 남긴다 — 미끄러지기만 하는 종목은 새 고점이 없어
                    //  저장 기회가 없고, 다음 재기동이 다시 첫 틱으로 앵커를 내린다.
                    SeedPeakStore::save(ticker_, peak_);
                    last_saved_peak_ = peak_;
                }
            }
        }

        // ── 청산: 보유 중이면 매 틱 스탑 평가 (손실통제 우선) ──────────────
        if (in_position_ && hold_quantity_ > 0)
        {
            if (exit_pending_)
            {
                return exit_pending_tick(price, trade.timestamp);
            }

            if (price > peak_)
            {
                peak_ = price;

                // 시드 고점 저장은 0.1% 이상 올랐을 때만 — 틱마다 파일을 쓰지 않는다.
                if (position_is_seed_ && peak_ >= last_saved_peak_ * 1.001)
                {
                    SeedPeakStore::save(ticker_, peak_);
                    last_saved_peak_ = peak_;
                }
            }

            if (trough_ <= 0.0 || price < trough_)
            {
                trough_ = price;
            }

            // 부착 직후 보호구간. 첫 틱에 스탑을 평가하면 재기동이 곧 투매가 된다.
            const bool warm =
                guard_warmup_sec_ <= 0 ||
                std::chrono::steady_clock::now() - start_tp_ >=
                    std::chrono::seconds(guard_warmup_sec_);

            bool hit = false;
            const char* why = " (stop)";

            if (position_is_seed_ && warm)
            {
                // (A) 물린 보유분: 고점 기준 넓은 트레일링 스탑 + 본전근처 반등 청산.
                double strail = (seed_trail_percent_ > 0.0 ? seed_trail_percent_ : trail_percent_);
                double seed_trail_stop = peak_ * (1.0 - strail);

                // 평단 하드스톱 — 당일 기준(고점 트레일)만으로는 매수가 대비 손실이 커지는 이월분을 못 자른다.
                //  갭일 첫 틱에 던지지 않게 세 가지로 묶는다: 시각(from_hhmm), 1분봉 종가 연속 확인(confirm_bars),
                //  부착 때 이미 깊게 물린 구형 보유 제외(skip_percent). 파라미터는 판단값 — 갭일 되돌림 분포를 재 본
                //  뒤 확정한다. [why D-082]
                if (seed_hard_percent_ > 0.0 && average_price_ > 0.0 && !seed_hard_checked_)
                {
                    seed_hard_checked_  = true;
                    seed_hard_excluded_ = seed_hard_skip_percent_ > 0.0 && price <= average_price_ * (1.0 - seed_hard_skip_percent_);

                    if (seed_hard_excluded_)
                    {
                        LOG_INFO("[ITB] 평단 하드스톱 제외 " + tag() + " — 부착 때 평단 " + price_string(average_price_) + " 대비 " +
                                 std::to_string(static_cast<int>((price / average_price_ - 1.0) * 100.0)) + "% (기준 -" +
                                 std::to_string(static_cast<int>(seed_hard_skip_percent_ * 100.0)) + "% 초과)");
                    }
                }

                const double seed_hard_stop = average_price_ * (1.0 - seed_hard_percent_);
                bool seed_hard_hit = seed_hard_percent_ > 0.0 && average_price_ > 0.0 && !seed_hard_excluded_ &&
                                     hhmm >= seed_hard_from_hhmm_ && price <= seed_hard_stop &&
                                     static_cast<int>(closes_.size()) >= seed_hard_confirm_bars_;

                for (int seed_hard_confirm_bar_index = 1; seed_hard_hit && seed_hard_confirm_bar_index <= seed_hard_confirm_bars_; ++seed_hard_confirm_bar_index)
                {
                    if (closes_[closes_.size() - seed_hard_confirm_bar_index] > seed_hard_stop)
                    {
                        seed_hard_hit = false;
                    }
                }

                if (price <= seed_trail_stop)
                {
                    hit = true;
                    why = " (seed-trail)";
                }
                else if (seed_hard_hit)
                {
                    hit = true;
                    why = " (평단 하드스톱)";
                }
                else if (exit_near_average_percent_ > 0.0 && average_price_ > 0.0 &&
                         price < average_price_ && // 상단 가드: 아직 물린(underwater) 상태에서만
                         price >= average_price_ * (1.0 - exit_near_average_percent_) &&
                         // 하단 무장: 실제로 arm%만큼 물려 본 적이 있어야 한다.
                         (exit_near_average_arm_percent_ <= 0.0 ||
                          (trough_ > 0.0 && trough_ <= average_price_ * (1.0 - exit_near_average_arm_percent_))))
                {
                    // 본전탈출 = "물린 보유분이 평단 근처까지 회복하면 재하락 전에 탈출".
                    //  밴드: average*(1-percent) ≤ price < average. 상단 가드(price<average)가 없으면 평단 위(수익)
                    //  포지션도 본전에서 청산돼 상방을 스스로 잘라먹는다 → 수익 구간은 seed-trail에 태운다.
                    hit = true;
                    why = " (본전탈출)";
                }
            }
            else
            {
                // (B) 신규 진입분: 타이트 트레일 + 진입가 하드손절.
                double trail_stop = peak_ * (1.0 - trail_percent_);
                double hard_stop = entry_price_ * (1.0 - hard_percent_);

                if (price <= trail_stop)
                {
                    hit = true;
                    why = " (trail)";
                }
                else if (price <= hard_stop)
                {
                    hit = true;
                    why = " (hard)";
                }
            }

            // 평단 손절(option-in, 보통 0=비활성) — 성격 무관 실제 손실률 초과 시 청산.
            if (!hit && warm && average_loss_percent_ > 0.0 && average_price_ > 0.0 && price <= average_price_ * (1.0 - average_loss_percent_))
            {
                hit = true;
                why = " (평단손절)";
            }

            bool eod = hhmm >= eod_hhmm_;

            if (hit || eod)
            {
                if (eod && !hit)
                {
                    why = " (장 마감)";
                }

                // 보유수량은 시드 이후 이 객체 안에서만 줄어든다. 재기동 전에 접수된 매도가
                //  나중에 체결되면 그 통보는 미매핑 경로로 원장에만 반영되고 여기까지 오지 않아,
                //  이미 판 수량을 또 판다(09-09 14:31 033790 — 14:15에 116주가 전량 체결됐는데
                //  116주를 다시 내 [40240000] "모의투자 잔고내역이 없습니다"로 거부됐다).
                //  발주 직전에 원장을 정본으로 한 번 맞춘다. 다만 원장 0을 "이미 팔렸다"로 읽으려면
                //  그 보유를 원장이 한 번은 인정했어야 한다. rest_price_feed 구성에서 원장은
                //  fetch_interval_sec(30초) 잔고 폴링으로만 갱신되므로, 방금 낸 매수는 최대 30초
                //  동안 0으로 보인다. 그 창에서 손절이 걸리면 전략은 자기가 플랫이라 믿고, 실제
                //  보유에는 트레일도 하드손절도 15:15 마감청산도 안 붙는다 — 판 걸 또 파는 것보다
                //  산 걸 방치하는 쪽이 비싸다. exit_pending_tick은 SELL을 낸 뒤라 0이 "체결 완료"
                //  지만, 여기는 내기 전이라 0이 "아직 안 보임"과 갈리지 않는다.
                const int ledger_quantity = confirmed_position("", ticker_);

                if (hold_quantity_ > 0 && ledger_quantity >= hold_quantity_)
                {
                    ledger_confirmed_ = true;
                }

                if (ledger_quantity <= 0 && ledger_confirmed_)
                {
                    LOG_INFO("[ITB] 청산 생략 " + tag() + why + " — 원장 보유 0 (이미 청산됨)");
                    in_position_ = false;
                    hold_quantity_ = 0;
                    position_is_seed_ = false;
                    SeedPeakStore::erase(ticker_);
                    exit_pending_ = false;
                    cooldown_until_ = trade.timestamp + std::chrono::seconds(cooldown_sec_);
                    have_cooldown_ = true;
                    return std::nullopt;
                }

                // 이번 발주 수량만 깎는다. 멤버를 깎으면 부분 반영된 원장(30초 폴링)이
                //  hold_qty_를 영구히 내려앉히고 남은 수량은 어느 청산 경로에도 안 잡힌다.
                //  잔량 정합은 exit_pending_tick이 매 틱 맞춘다.
                const int sell_quantity = ledger_quantity > 0 ? std::min(hold_quantity_, ledger_quantity) : hold_quantity_;

                if (sell_quantity < hold_quantity_)
                {
                    LOG_INFO("[ITB] 청산 수량 보정 " + tag() + " " + std::to_string(hold_quantity_) +
                             " → " + std::to_string(sell_quantity) + "주 (원장 기준, 보유 상태는 유지)");
                }

                auto signal = make_signal(OrderSide::SELL, sell_quantity, price, trade.timestamp,
                                       std::string("청산") + why);
                LOG_INFO("[ITB] SELL " + tag() + " qty=" + std::to_string(sell_quantity) + " @" +
                         price_string(price) + why);
                // 신호는 큐에 들어갈 뿐 접수·체결을 보장하지 않는다. 여기서 상태를 지우면 게이트에
                //  튕긴 포지션이 어느 청산 경로에도 다시 잡히지 않는다. 확정 포지션이 0이 될 때까지
                //  보유 상태를 유지한 채 백오프 재발주한다(exit_pending_tick).
                exit_pending_ = true;
                exit_retries_ = 0;
                exit_backoff_sec_ = kExitBackoffFirstSec;
                exit_next_retry_ = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(exit_backoff_sec_);
                exit_why_ = why;
                return signal;
            }
        }

        // ── 진입: 1분 버킷 마감 시에만 평가 ──────────────────────────────
        if (current_hhmm_ < 0)
        {
            current_hhmm_ = hhmm;
            current_bucket_last_ = price;
            return std::nullopt;
        }

        if (hhmm == current_hhmm_)
        {
            current_bucket_last_ = price; // 같은 버킷: 종가 갱신만
            return std::nullopt;
        }

        // 버킷 롤오버: 직전 버킷 종가 확정
        double bucket_close = current_bucket_last_;
        std::optional<OrderSignal> signal;

        if (!in_position_ && is_active() && static_cast<int>(closes_.size()) >= channel_min_)
        {
            double high_count = *std::max_element(closes_.begin(), closes_.end());
            bool cooldown_ok = !have_cooldown_ || trade.timestamp >= cooldown_until_;
            int no_entry_hhmm = (no_new_entry_hhmm_ > 0 ? no_new_entry_hhmm_ : eod_hhmm_);
            bool session_ok = hhmm < no_entry_hhmm; // 마감 임박 신규진입 금지

            if (cooldown_ok && session_ok && bucket_close > high_count &&
                bucket_close > anchor_price_ * (1.0 + epsilon_))
            {
                int quantity = entry_quantity_;

                if (notional_per_position_ > 0.0 && bucket_close > 0.0)
                {
                    quantity = std::max(1, static_cast<int>(std::floor(notional_per_position_ / bucket_close)));
                }

                signal = make_signal(OrderSide::BUY, quantity, bucket_close, trade.timestamp,
                                  "채널돌파 종가=" + price_string(bucket_close) + ">hiN=" + price_string(high_count) +
                                  " 앵커=" + price_string(anchor_price_));
                LOG_INFO("[ITB] BUY " + tag() + " qty=" + std::to_string(quantity) + " @" +
                         price_string(bucket_close) + " (돌파 hiN=" + price_string(high_count) + ")");
                in_position_ = true;
                position_is_seed_ = false; // 신규 진입분 — 타이트 스탑 적용
                SeedPeakStore::erase(ticker_);
                hold_quantity_ = quantity;
                entry_price_ = bucket_close;
                peak_ = bucket_close;
            }
        }

        // 채널 갱신(완성 버킷만 유지, 최근 N분)
        closes_.push_back(bucket_close);

        while (static_cast<int>(closes_.size()) > channel_min_)
        {
            closes_.pop_front();
        }

        current_hhmm_ = hhmm;
        current_bucket_last_ = price;
        return signal;
    }

private:
    // 청산 대기 중 틱 처리. 원장(confirmed_position)이 0이면 해제, 줄었으면 잔량으로 갱신하고
    //  백오프를 되돌린다. 아니면 백오프 창이 지났을 때 같은 수량을 재발주한다.
    //  원장이 이 종목을 모르면(미시드) 0이 나와 곧바로 해제된다 — 예전 동작과 같다.
    //  체결 뒤 남은 재발주는 게이트가 안 잡고 브로커가 거부하므로 횟수 상한으로 스팸을 끊는다.
    std::optional<OrderSignal> exit_pending_tick(double price, std::chrono::system_clock::time_point timestamp)
    {
        const int position = confirmed_position("", ticker_); // make_signal의 account_id=""와 같은 키
        const auto now = std::chrono::steady_clock::now();

        if (position <= 0)
        {
            LOG_INFO("[ITB] 청산 확인 " + tag() + exit_why_ + " 재발주=" + std::to_string(exit_retries_));
            in_position_ = false;
            hold_quantity_ = 0;
            position_is_seed_ = false;
            SeedPeakStore::erase(ticker_);
            exit_pending_ = false;
            cooldown_until_ = timestamp + std::chrono::seconds(cooldown_sec_);
            have_cooldown_ = true;
            return std::nullopt;
        }

        if (position < hold_quantity_)
        {
            LOG_INFO("[ITB] 부분 청산 " + tag() + " 잔량=" + std::to_string(position) + "/" + std::to_string(hold_quantity_));
            hold_quantity_ = position;
            exit_backoff_sec_ = kExitBackoffFirstSec;
            exit_next_retry_ = now;
        }

        if (now < exit_next_retry_)
        {
            return std::nullopt;
        }

        if (exit_retries_ >= kExitMaxRetries)
        {
            if (exit_retries_ == kExitMaxRetries)
            {
                ++exit_retries_; // 한 번만 남기고 침묵
                LOG_WARN("[ITB] 청산 재발주 상한 " + tag() + " qty=" + std::to_string(hold_quantity_) +
                         " — 원장 잔량이 남아 있다. 수동 확인 필요");
            }

            return std::nullopt;
        }

        ++exit_retries_;
        auto signal = make_signal(OrderSide::SELL, hold_quantity_, price, timestamp,
                               "청산 재발주#" + std::to_string(exit_retries_) + exit_why_);
        LOG_WARN("[ITB] SELL 재발주#" + std::to_string(exit_retries_) + " " + tag() + " qty=" +
                 std::to_string(hold_quantity_) + " @" + price_string(price) + exit_why_ + " 다음 " +
                 std::to_string(exit_backoff_sec_) + "s");
        exit_next_retry_ = now + std::chrono::seconds(exit_backoff_sec_);
        exit_backoff_sec_ = std::min(exit_backoff_sec_ * 2, kExitBackoffMaxSec);
        return signal;
    }

    OrderSignal make_signal(OrderSide side, int quantity, double price,
                            std::chrono::system_clock::time_point timestamp,
                            const std::string& reason = "")
    {
        OrderSignal signal;
        signal.ticker = ticker_;
        signal.symbol_id    = symbol_id_;
        signal.side = side;
        signal.type = OrderType::MARKET;
        signal.quantity = quantity;
        signal.price = price; // MARKET은 미사용이나 로깅·명목 상한 계산·향후 LIMIT 대비
        signal.market = Market::KR;
        signal.strategy_id = id();
        signal.reason = reason; // G4: 판단 근거(돌파/청산 사유)를 신호에 실어 영속
        signal.timestamp = timestamp;
        return signal; // account_id="" (기본) — OrderGate 원장 시드 계좌키와 일치(C-1)
    }

    static std::string price_string(double value) { return std::to_string(static_cast<long long>(std::llround(value))); }

    std::string ticker_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    symbol::SymbolId symbol_id_ = symbol::kNone; // ticker_의 id — on_start에서 한 번(미주입=kNone, 문자열 비교로 폴백)
    std::string name_; // 표시명(로깅 전용)
    int entry_quantity_;   // 신규 돌파 진입 수량(명목 미지정 시 고정)
    int hold_quantity_;    // 현재 보유수량(시드분 또는 진입분) — 매도 전량 기준
    bool start_in_position_;
    int channel_min_; // 채널 창(완성 1분 버킷 개수)
    double epsilon_;      // 진입 버퍼(앵커 대비)
    double trail_percent_;
    double hard_percent_;
    int eod_hhmm_;    // 마감 강제청산 기준(KST HHMM)
    int cooldown_sec_;
    double average_price_ = 0.0;       // 매입 평단(시드분) — 평단손절/본전탈출 기준가
    double average_loss_percent_ = 0.0; // 평단 대비 손절률(0=비활성)
    // ── v2 ──
    double seed_trail_percent_ = 0.0;       // 물린분 고점 기준 트레일(넓게)
    double exit_near_average_percent_ = 0.0;    // 물린분 본전탈출 임계(평단 -x% 이내 반등)
    double exit_near_average_arm_percent_ = 0.03; // 본전탈출 무장 깊이(평단 -x% 도달 이력 필요)
    int    guard_warmup_sec_ = 60;      // 부착 직후 청산 유예(초)
    double seed_hard_percent_ = 0.0;        // 이월분 평단 하드스톱(0=비활성)
    double seed_hard_skip_percent_ = 0.15;  // 부착 때 이보다 깊게 물린 보유는 하드스톱 대상 제외
    int    seed_hard_from_hhmm_ = 915;  // 하드스톱 판정 시작 시각(KST HHMM)
    int    seed_hard_confirm_bars_ = 3; // 완성 1분봉 종가 연속 확인 개수
    bool   seed_hard_checked_ = false;  // 부착 뒤 첫 판정에서 제외 여부를 정했나
    bool   seed_hard_excluded_ = false; // 구형 보유라 하드스톱 대상에서 뺐나
    int no_new_entry_hhmm_ = 0;         // 신규진입 금지 시각(0→eod_hhmm)
    double notional_per_position_ = 0.0; // 종목당 명목(원)
    double day_open_price_ = 0.0;          // 당일 시가 앵커 주입

    std::deque<double> closes_; // 완성 1분 버킷 종가(최근 channel_min_개)
    int current_hhmm_ = -1;         // 현재 집계 중 버킷(HHMM)
    double current_bucket_last_ = 0.0;
    double anchor_price_ = 0.0; // 당일 앵커(시가 또는 첫 틱)
    double last_ = 0.0;
    bool in_position_ = false;
    bool position_is_seed_ = false;
    bool ledger_confirmed_ = false;   // [inv] 원장이 이 보유를 최소 1회 인정했나 // 현재 포지션이 물린 시드분인가(청산 로직 분기)
    double entry_price_ = 0.0;
    double peak_ = 0.0;
    double saved_peak_ = 0.0;      // 부착 시 seed_peaks.json에서 읽은 당일 고점(없으면 0)
    double last_saved_peak_ = 0.0; // 마지막으로 파일에 남긴 고점 — 0.1% 이상 오를 때만 다시 쓴다
    double trough_ = 0.0;              // 부착 이후 최저가 — 본전탈출 무장 판정용
    std::chrono::steady_clock::time_point start_tp_{}; // on_start 시각(워밍업 기준)
    bool have_cooldown_ = false;
    std::chrono::system_clock::time_point cooldown_until_;

    // ── 청산 대기(C-2): 확정 포지션 0 확인 전까지 보유 상태 유지 + 백오프 재발주 ──
    static constexpr int kExitBackoffFirstSec = 2;
    static constexpr int kExitBackoffMaxSec = 60;
    static constexpr int kExitMaxRetries = 20;
    bool exit_pending_ = false;
    int exit_retries_ = 0;
    int exit_backoff_sec_ = kExitBackoffFirstSec;
    std::chrono::steady_clock::time_point exit_next_retry_{};
    std::string exit_why_; // 최초 청산 사유(재발주 로그·reason에 그대로 싣는다)
};
