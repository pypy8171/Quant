#pragma once
// strategy/SwingTrendStrategy.h — 스윙 추세 슬리브(SWING_TREND): 파이썬이 쓴 계획 파일(research/studies/42_swing_trend/live/plan.json)의
//  되돌림 매수를 지정가로 내고, 매수 뒤 확정된 일봉 저점으로 손절선을 올리며 종가 동시호가에 손절 매도한다.
//  스레드: on_clock·on_start·on_stop은 샤드 스레드, prefetch_once는 prefetch::Pool 스레드. 둘은 snapshot_mutex_로만 만난다.
//  규칙 원본 research/studies/42_swing_trend/swing_trend.py, 판정은 SwingTrendRules.h. [why D-160]
//  - 매수: 09:00 뒤 그날 시가(REST 현재가 조회의 시가)를 알면 판단한다. 시가 > 매수가면 매수 대기(armed)에 넣고, 아니면 그날은 사지 않는다.
//    대기 종목은 현재가를 REST 묶음 조회로 받아 매수가×(1+buy_trigger_percent/100) 이하로 다가오면 그때 매수가 지정가를 낸다.
//    체결 0주인 매수가 매수가×(1+buy_disarm_percent/100) 위로 멀어지면 취소하고, 장부 미체결 선점이 풀린 뒤 다시 대기로 둔다
//    (아침에 걸어 둔 지정가가 하루 종일 현금·노출을 묶던 것을 막는다, 2026-10-16 실측).
//    계좌가 이미 든 종목·미체결 매수가 있는 종목·바스켓 소유 종목·오늘 이미 판단한 종목·국면 꺼짐·계좌 신규매수 차단이면 건너뛴다.
//    계획 as_of가 달력 stale_days일보다 오래됐거나 파일이 없으면 매수 없이 WARN만 남긴다. 15:19에 남은 매수는 취소하고 대기를 푼다.
//  - 매수는 상태 파일에 먼저 적고 낸다 — 재기동 뒤 같은 매수를 두 번 내지 않는다. 쓰기가 실패하면 그날 매수를 멈춘다.
//  - 손절선: max(cancel_below, 매수 뒤 확정된 저점). 일봉은 하루 한 번 REST로 받는다(오늘 봉 제외).
//  - 매도: 15:20–15:29 창의 첫 판단에서 현재가(REST) < 손절선이면 매도가능 수량 전부를 시장가로 판다(종가 체결). 매수 당일에도 판다.
//  - 체결 시세 구독은 하지 않는다 — 시가·현재가 모두 REST로 받아 웹소켓 칸(약 40)을 쓰지 않는다.
//  - 자기 종목을 OrderGate 슬롯 계산 밖으로 두고(owned_sink), 로더가 청산 관리(ITB) 부착과 DEVSCALE 유니버스에서 뺀다.
#include "core/PrefetchPool.h"
#include "strategy/StrategyBase.h"
#include "strategy/SwingTrendRules.h"
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

class SwingTrendStrategy : public StrategyBase
{
public:
    struct Params
    {
        std::string label      = "TREND"; // id = SWING_<label>. regime_strategies의 "SWING_*"가 이걸 본다
        std::string account;
        std::string plan_file  = "research/studies/42_swing_trend/live/plan.json";
        std::string state_file = "_private/state/swing_trend.json";
        int64_t     notional_krw      = 500'000; // 종목당 매수 금액(원)
        int         buy_start_hhmm    = 900;     // 시가를 알면 매수 판단을 시작하는 시각
        int         buy_cancel_hhmm   = 1519;    // 이 분부터 남은 매수를 취소하고 새 매수를 내지 않는다
        int         sell_start_hhmm   = 1520;    // 종가 동시호가 손절 판단 창(포함)
        int         sell_end_hhmm     = 1529;
        int         stale_days        = 7;       // 계획 as_of가 이보다 오래되면 신규 매수 없음(달력 일)
        int         pivot_window      = 20;      // 저점 확정 좌우 창(거래일)
        int         daily_lookback    = 250;     // 일봉 조회 개수
        int         exit_retry_sec    = 60;      // 매도가 남아 있으면 다시 내는 간격
        int         max_exit_attempts = 3;
        int         reload_sec        = 60;      // 계획 파일 변경 확인 주기
        // [formula] 발주선 = 매수가×(1+값/100). 현재가가 이 선 이하면 지정가를 낸다. 손으로 고른 값이고 측정 근거는 없다.
        double      buy_trigger_percent   = 1.0;
        // [formula] 취소선 = 매수가×(1+값/100). 체결 0주 매수는 현재가가 이 선 위면 취소한다. 손으로 고른 값이고 측정 근거는 없다.
        //  발주선보다 커야 내자마자 취소하는 왕복이 줄어든다.
        double      buy_disarm_percent    = 2.0;
        bool        dry_run           = false;   // 계산·로그만, 주문 없음
        std::vector<std::string> excluded_tickers; // 로더가 넣는 바스켓·급등 슬리브 소유 종목
    };

    enum class Status
    {
        Pending, // 매수 냄, 아직 체결 없음
        Open,    // 체결 확인
        Exiting, // 매도 냄
    };

    // 한 보유. 상태 파일 한 줄과 같다.
    struct Position
    {
        std::string ticker;
        std::string entry_date;            // YYYYMMDD — 매수 주문을 낸 날
        int64_t     entry_price   = 0;     // 지정가(원)
        int64_t     initial_stop  = 0;     // 계획의 cancel_below
        int64_t     stop          = 0;     // 지금 손절선(올리기만 한다)
        int         ordered       = 0;
        int         quantity      = 0;     // 장부에서 읽은 체결 주 수
        Status      status        = Status::Pending;
        uint64_t    order_number  = 0;     // 매수 주문 번호 — 15:19 미체결 취소 대상
        bool        cancel_sent   = false;
        bool        rearm_on_cancel = false; // 멀어져 취소했다 — 미체결 선점이 풀리면 다시 대기로 둔다
        int         exit_attempts = 0;
        std::time_t last_exit_at  = 0;
        std::string exit_reason;
        int         missing_checks = 0;    // 장부에 0주로 보인 연속 횟수(저장 안 함) — 일시적인 0으로 소유를 놓지 않는다
        std::string stop_checked_day;      // 손절선을 그날 일봉으로 다시 본 날(저장 안 함)
        std::string sell_checked_day;      // 그날 손절 판단을 끝낸 날(저장 안 함) — 매도 창의 첫 판단만 쓴다
    };

    using OwnedSink = std::function<bool(const std::vector<std::string>&)>;
    using Clock     = std::function<std::time_t()>;

    // 시가·현재가 출처. 비어 있는 칸은 KisClient로 받는다(시험이 REST 없이 값을 넣는 이음매).
    //  open은 종목 하나의 시가(모르면 0), prices는 여러 종목 현재가를 입력 순서대로(실패면 nullopt, 모르는 종목은 0).
    struct QuoteSource
    {
        std::function<double(const std::string&)>                                                open;
        std::function<std::optional<std::vector<double>>(const std::vector<std::string>&)> prices;
    };

    SwingTrendStrategy(Params parameters, OwnedSink owned_sink);
    ~SwingTrendStrategy() override;

    const std::string& id() const override
    {
        return id_;
    }

    std::string            describe() const override;
    void                   on_start() override;
    void                   on_stop() override;
    void                   on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out) override;
    bool                   wants_clock() const override;
    bool                   ignores_regime_halt() const override;
    void                   on_clock(std::vector<OrderSignal>& out) override;
    std::vector<WatchSpec> get_watch_specifications() const override;
    bool                   wants_all_ticks_when_unlisted() const override;

    bool load_plan();  // 파일을 읽어 plan_을 바꾼다. 실패면 직전 것을 유지하고 false
    void load_state(); // 상태 파일에서 보유를 복원한다(로더가 부른다). 못 읽으면 매수를 막는다

    // 기동 때 실제 잔고와 맞춘다 — 잔고에 있는 종목만 이 전략 보유로 남기고 수량을 잔고로 고친다. 오늘 낸 매수(Pending)는 남긴다.
    //  held_quantity는 종목 → 주 수. 버린 종목 수를 돌려준다.
    int reconcile_with_balance(const std::map<std::string, int>& held_quantity);

    // 로더가 기동 때 읽는다 — 보유는 청산 관리 부착에서, 후보는 DEVSCALE 초기 유니버스에서 뺀다.
    std::vector<std::string> held_tickers() const;
    std::vector<std::string> candidate_tickers() const;

    const std::vector<Position>& positions() const
    {
        return positions_;
    }

    void set_clock(Clock clock);
    void set_quote_source(QuoteSource source); // on_start 전에만 부른다(prefetch 스레드가 읽는다)

    // 오늘 매수 대기 중인 종목(시가 판정은 통과했고 아직 지정가를 내지 않음).
    const std::set<std::string>& armed_tickers() const
    {
        return armed_today_;
    }

    static const char* status_text(Status status);

    // 한 번 불릴 때 필요한 REST를 받는다(prefetch::Pool 스레드). 시험에서 직접 부를 수 있게 공개한다.
    void prefetch_once();

private:
    // prefetch 스레드와 주고받는 값. 요청은 샤드 스레드가 쓰고, 결과는 prefetch가 쓴다.
    struct DailyLows
    {
        std::string          day; // 받은 날 YYYYMMDD
        std::vector<int32_t> session_dates;
        std::vector<int64_t> lows;
    };

    // 받은 지 이 초를 넘은 현재가로는 발주·취소하지 않는다. prefetch 한 패스(3초)에 30종목씩 돌므로 대기 60종목까지 한 바퀴가 6초다.
    static constexpr std::time_t kQuoteMaxAgeSec = 15;

    struct Quote
    {
        double      price      = 0.0;
        std::time_t fetched_at = 0; // 받은 시각(clock_ 기준)
    };

    struct Snapshot
    {
        std::string                         day;          // 결과가 속한 날 YYYYMMDD — 바뀌면 시가·현재가를 비운다
        std::vector<std::string>            want_open;    // 시가가 필요한 후보
        std::vector<std::string>            want_bars;    // 일봉이 필요한 보유
        std::vector<std::string>            want_price;   // 손절 판단에 현재가가 필요한 보유
        std::vector<std::string>            want_quote;   // 근접 발주·멀어짐 취소에 현재가가 필요한 대기·미체결 종목
        std::map<std::string, double>       opens;        // 오늘 시가(0 = 아직 체결 없음)
        std::map<std::string, double>       prices;       // 매도 창에서 받은 현재가
        std::map<std::string, Quote>        quotes;       // 매수 창에서 받은 현재가와 받은 시각
        std::map<std::string, DailyLows>    bars;
    };

    void        roll_day();
    void        run_buys(std::vector<OrderSignal>& out);
    void        arm_entry(const swing_trend::PlanEntry& entry, double open_price);
    std::string entry_block_reason(const std::string& ticker) const;
    void        place_buy(const swing_trend::PlanEntry& entry, double price, std::vector<OrderSignal>& out);
    void        run_armed_buys(std::vector<OrderSignal>& out);
    void        run_far_cancels(std::vector<OrderSignal>& out);
    void        release_armed();
    const swing_trend::PlanEntry* plan_entry(const std::string& ticker) const;
    std::optional<double>         fresh_quote(const std::string& ticker) const;
    bool        plan_usable();
    void        run_unfilled_cancels(std::vector<OrderSignal>& out);
    void        fetch_quotes(const std::vector<std::string>& wanted);
    void        track_fills();
    void        update_stops();
    void        run_sells(std::vector<OrderSignal>& out);
    bool        emit_sell(Position& position, const std::string& reason, std::vector<OrderSignal>& out);
    void        publish_requests();
    void        publish_owned();
    bool        save_state();
    std::string kst_date() const;
    OrderSignal make_order(const std::string& ticker, OrderSide side, OrderType type, int quantity, double price,
                           const std::string& reason) const;
    void        stop_prefetch();

    Params                                parameters_;
    OwnedSink                             owned_sink_;
    Clock                                 clock_;
    std::string                           id_;
    std::optional<swing_trend::Plan>      plan_;
    std::filesystem::file_time_type       plan_modified_{};
    std::vector<Position>                 positions_;
    std::string                           day_;             // 오늘 YYYYMMDD
    std::set<std::string>                 attempted_today_; // 오늘 매수 판단을 끝낸 종목(건너뜀 포함)
    std::set<std::string>                 armed_today_;     // 오늘 매수 대기 중인 종목(상태 파일 "armed")
    std::vector<std::string>              owned_published_;
    std::vector<std::string>              owned_scratch_;
    std::chrono::steady_clock::time_point last_reload_{};
    bool                                  day_started_         = false; // 이 프로세스에서 roll_day가 오늘을 시작했다
    bool                                  plan_missing_logged_ = false;
    bool                                  plan_stale_logged_   = false;
    bool                                  inactive_logged_     = false; // 국면 꺼짐·계좌 신규매수 차단으로 미룬 것을 남겼다
    bool                                  broken_logged_       = false;
    bool                                  state_broken_        = false; // 상태 파일을 못 읽었다 — 매수 금지, 덮어쓰기 금지
    bool                                  buys_stopped_today_  = false; // 상태 파일 쓰기 실패 — 오늘 매수 멈춤
    bool                                  dirty_               = false; // 저장할 변경이 있다(on_clock이 쓴다)
    std::set<std::string>                 price_missing_logged_;        // 매도 창에서 현재가가 없다고 남긴 종목

    mutable std::mutex     snapshot_mutex_;
    Snapshot               snapshot_; // [inv] snapshot_mutex_를 쥐고만 읽고 쓴다
    prefetch::Pool::TaskId prefetch_task_ = 0;
    QuoteSource            quote_source_;     // [inv] on_start 전에만 바뀐다 — 그 뒤로는 prefetch 스레드가 읽기만 한다
    size_t                 quote_cursor_ = 0; // [inv] prefetch 스레드만 쓴다 — want_quote 순환 시작 위치
};
