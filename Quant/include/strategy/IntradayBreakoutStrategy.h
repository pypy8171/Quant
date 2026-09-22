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
//  [진입]  1분 버킷 종가가 최근 N분 채널 고점을 상향 돌파 + 당일 기준점 대비 +epsilon 위
//          → 시장가 신규 매수. 버킷 마감 시에만 평가(틱 노이즈/휩쏘 억제).
//          수량은 notional_per_position>0이면 floor(명목/현재가), 아니면 entry_quantity 고정.
//          기준점은 day_open_price(>0) 주입 시 당일 시가, 아니면 첫 관측 틱(자기참조 방지).
//  [청산]  포지션 성격에 따라 분기(매 틱, 손실통제 우선):
//    (A) 물린 보유분(position_is_seed_): 고점 기준 넓은 트레일링 스탑 seed_trail_percent + 본전근처
//        반등 청산 exit_near_average_percent. 이미 -30% 물린 평단에 -3% 하드손절을 걸어 개장
//        즉시 시장가 투매하는 자해(v1 결함)를 제거. 반등에 실어 던진다.
//    (B) 신규 진입분: 타이트 트레일 trail_percent + 진입가 하드손절 hard_percent.
//    공통: 평단손절(average_loss_percent, 보통 0=비활성), 장 마감(market_close_hhmm) 강제청산.
//  [신규진입 금지]  no_new_entry_hhmm(>0이면 이 시각부터, 아니면 market_close_hhmm) 이후 진입 금지
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
                             double hard_percent = 0.015, int market_close_hhmm = 1515,
                             int reentry_cooldown_sec = 60,
                             double average_price = 0.0, double average_loss_percent = 0.0,
                             // ── v2 추가 (뒤에 붙여 하위호환) ──
                             double seed_trail_percent = 0.0,      // 물린분 고점 기준 트레일(0→trail_percent)
                             double exit_near_average_percent = 0.0,   // 물린분 본전탈출(평단 -x% 이내, 0=비활성)
                             int no_new_entry_hhmm = 0,        // 신규진입 금지 시각(0→market_close_hhmm)
                             double notional_per_position = 0.0, // 종목당 명목(0→entry_quantity 고정)
                             double day_open_price = 0.0)         // 당일 시가 기준점 주입(0→첫 틱)
        : ticker_(std::move(ticker)), entry_quantity_(entry_quantity), hold_quantity_(hold_quantity),
          start_in_position_(start_in_position), channel_min_(channel_min),
          epsilon_(breakout_epsilon), trail_percent_(trail_percent), hard_percent_(hard_percent),
          market_close_hhmm_(market_close_hhmm), cooldown_sec_(reentry_cooldown_sec),
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

    std::string describe() const override;

    // 체결만 구독(호가 제외) — 구독 한도 절약.
    std::vector<WatchSpec> get_watch_specifications() const override
    {
        return {{ticker_, Market::KR, "", true}};
    }

    void on_start() override;

    // 일봉 경로 미사용(라이브 소스는 WS 체결) — 순수가상 요건 충족용 no-op.
    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    std::optional<OrderSignal> on_trade(const TradeData& trade) override;

private:
    // 청산 대기 중 틱 처리. 원장(confirmed_position)이 0이면 해제, 줄었으면 잔량으로 갱신하고
    //  백오프를 되돌린다. 아니면 백오프 창이 지났을 때 같은 수량을 재발주한다.
    //  원장이 이 종목을 모르면(미시드) 0이 나와 곧바로 해제된다 — 예전 동작과 같다.
    //  체결 뒤 남은 재발주는 게이트가 안 잡고 브로커가 거부하므로 횟수 상한으로 스팸을 끊는다.
    std::optional<OrderSignal> exit_pending_tick(double price, std::chrono::system_clock::time_point timestamp);

    OrderSignal make_signal(OrderSide side, int quantity, double price,
                            std::chrono::system_clock::time_point timestamp,
                            const std::string& reason = "");

    static std::string price_string(double value) { return std::to_string(static_cast<long long>(std::llround(value))); }

    std::string ticker_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    symbol::SymbolId symbol_id_ = symbol::kNone; // ticker_의 id — on_start에서 한 번(미주입=kNone, 문자열 비교로 폴백)
    std::string name_; // 표시명(로깅 전용)
    int entry_quantity_;   // 신규 돌파 진입 수량(명목 미지정 시 고정)
    int hold_quantity_;    // 현재 보유수량(시드분 또는 진입분) — 매도 전량 기준
    bool start_in_position_;
    int channel_min_; // 채널 창(완성 1분 버킷 개수)
    double epsilon_;      // 진입 버퍼(기준점 대비)
    double trail_percent_;
    double hard_percent_;
    int market_close_hhmm_;    // 마감 강제청산 기준(KST HHMM)
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
    int no_new_entry_hhmm_ = 0;         // 신규진입 금지 시각(0→market_close_hhmm)
    double notional_per_position_ = 0.0; // 종목당 명목(원)
    double day_open_price_ = 0.0;          // 당일 시가 기준점 주입

    std::deque<double> closes_; // 완성 1분 버킷 종가(최근 channel_min_개)
    int current_hhmm_ = -1;         // 현재 집계 중 버킷(HHMM)
    double current_bucket_last_ = 0.0;
    double day_base_price_ = 0.0; // 당일 기준점(시가 또는 첫 틱)
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
