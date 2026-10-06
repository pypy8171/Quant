#pragma once
// 급등 뒤 되돌림 보유 슬리브(SURGE_HOLD) — 스터디 35 c1 규칙의 계획 파일(Quant/config/surge_plan.json)을 집행한다.
//  신호 계산은 파이썬(daily_watch.py --emit-plan), 이 전략은 집행·청산 감시·소유권 격리만 맡는다. [why D-157]
//  - 매수: BUY_OPEN 행을 다음 거래일 08:50–08:59 장 시작 동시호가에 시장가로 낸다(체결 틱이 없는 시간이라 on_clock으로).
//    계획 하나는 한 번만 집행한다(상태 파일 last_executed_as_of). 국면은 그 자리에서 regime.json을 읽어 본다 —
//    엔진의 국면 선택은 09:00에 적용되기 때문이다. 약세장·계좌 신규매수 차단·다른 슬리브 보유나 매수 중·바스켓 종목·
//    같은 급등 신호 재매수·보유 상한·하루 매수 상한이면 건너뛴다. 09:30까지 안 잡힌 매수는 취소 주문을 낸다.
//  - 체결: 장부 보유·평단이 잡히면 실제 매수가로 익절을 다시 계산해 상태 파일에 적는다(재기동 때 복원).
//  - 청산: 손절(관찰 최저 저가 −3%)·익절(매수가 +15%)을 체결 틱으로 보되 주문은 09:00–15:19에만 낸다. 그 밖(장 마감 뒤
//    NXT 체결 등)에 닿으면 표시만 해 두고 다음 창에서 판다. 시가가 손절선 아래로 체결되면 09:00 뒤 바로 판다.
//    계획의 EXIT_CLOSE 행은 15:20–15:29 종가 동시호가에 판다(120거래일째, 휴장일은 파이썬이 센다). 계획이 없는 날은
//    매수일부터 평일 backstop_weekdays일이 지난 보유를 같은 창에 판다.
//  - 매수는 상태 파일에 먼저 적고 낸다 — 재기동 뒤 같은 매수를 두 번 내지 않는다. 쓰기가 실패하면 그날 매수를 멈춘다.
//    매도·체결 반영은 표시만 하고 on_clock이 저장한다(체결 틱 경로에서 파일을 쓰지 않는다).
//  - 상태 파일이 있는데 못 읽으면 매수를 막고 그 파일을 덮어쓰지 않는다(빈 보유로 다시 시작하지 않는다).
//  - 자기 종목을 OrderGate 슬롯 계산 밖으로 두고(owned_sink), 로더가 청산 관리(ITB) 부착에서 뺀다. dry_run이면 두지 않는다.
#include "strategy/StrategyBase.h"
#include "strategy/SurgeHoldPlan.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class SurgeHoldStrategy : public StrategyBase
{
public:
    struct Params
    {
        std::string label      = "MAIN";                         // id = SURGE_<label>. regime_strategies의 "SURGE_*"가 이걸 본다
        std::string account;
        std::string plan_file  = "Quant/config/surge_plan.json";
        std::string state_file = "Quant/config/surge_state.json";
        double      amount_krw        = 5'000'000.0;             // 종목당 매수 금액
        int         max_positions     = 4;                       // 보유 + 매수 대기 상한
        int         max_daily_buys    = 3;                       // 하루 신규 매수 상한
        int         buy_start_hhmm    = 850;                     // 장 시작 동시호가 매수 창(포함)
        int         buy_end_hhmm      = 859;                     // 이 분까지(포함)
        int         close_start_hhmm  = 1520;                    // 종가 동시호가 만기 매도 창(포함)
        int         close_end_hhmm    = 1529;
        int         exit_start_hhmm   = 900;                     // 손절·익절 주문을 내는 창의 시작(끝은 close_start_hhmm 앞)
        int         backstop_weekdays = 130;                     // 계획이 없는 날 이만큼 평일이 지난 보유는 종가에 판다
        int         fill_wait_hhmm    = 930;                     // 이 시각까지 매수가 안 잡히면 미체결로 지운다
        int         exit_retry_sec    = 60;                      // 매도가 남아 있으면 다시 내는 간격
        int         max_exit_attempts = 3;
        int         reload_sec        = 60;                      // 계획 파일 변경 확인 주기
        bool        dry_run           = false;                   // 계산·로그만, 주문 없음
        std::vector<std::string> excluded_tickers;               // 로더가 넣는 바스켓 소유 종목
    };

    enum class Status
    {
        Pending, // 매수 냄
        Open,    // 체결 확인
        Exiting, // 매도 냄
    };

    // 한 보유. 상태 파일 한 줄과 같다.
    struct Position
    {
        std::string ticker;
        std::string d0;                    // YYYY-MM-DD 급등일 — 같은 신호 재매수를 막는 열쇠
        std::string entry_date;            // YYYYMMDD — 매수 주문을 낸 날
        int         quantity        = 0;   // 체결된 주 수(장부에서 읽음)
        int         ordered         = 0;   // 낸 주 수
        double      entry_price     = 0.0; // 실제 매수가(장부 평단)
        double      stop_basis_low  = 0.0;
        double      stop_percent    = 0.0;
        double      take_percent    = 0.0;
        double      stop            = 0.0; // 실제 매수가로 다시 계산한 손절가
        double      take            = 0.0;
        Status      status          = Status::Pending;
        std::string exit_reason;
        std::string exit_due;              // 창 밖에서 손절·익절에 닿았다 — 다음 창에서 이 사유로 판다
        int         exit_attempts   = 0;
        std::time_t last_exit_at    = 0;
        uint64_t    order_number    = 0;   // 매수 주문 번호 — 09:30 미체결 취소 대상
        bool        cancel_sent     = false;
    };

    // 슬롯 면제 표를 보낸다. 못 보냈으면 false — 다음 on_clock에 다시 보낸다.
    using OwnedSink = std::function<bool(const std::vector<std::string>&)>;
    using Clock     = std::function<std::time_t()>;
    // regime.json으로 본 오늘 신규 매수 허용(Engine::regime_file_allows_entry). nullopt = 모름.
    using RegimeCheck = std::function<std::optional<bool>()>;

    SurgeHoldStrategy(Params parameters, OwnedSink owned_sink);

    const std::string& id() const override
    {
        return id_;
    }

    std::string                describe() const override;
    std::optional<OrderSignal> on_data(const MarketData&) override;
    void                       on_start() override;
    void                       on_stop() override;
    void                       on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out) override;
    bool                       wants_clock() const override;
    void                       on_clock(std::vector<OrderSignal>& out) override;
    std::vector<WatchSpec>     get_watch_specifications() const override;

    bool load_plan();   // 파일을 읽어 plan_을 바꾼다. 실패면 직전 것을 유지하고 false
    void load_state();  // 상태 파일에서 보유를 복원한다(생성자 뒤 로더가 부른다). 못 읽으면 매수를 막는다

    // 로더가 기동 때 읽는다 — 보유 종목은 청산 관리 부착에서, 매수 후보는 DEVSCALE 초기 유니버스에서 뺀다.
    std::vector<std::string> held_tickers() const;
    std::vector<std::string> buy_candidates() const;

    const std::vector<Position>& positions() const
    {
        return positions_;
    }

    void set_clock(Clock clock)
    {
        clock_ = std::move(clock);
    }

    void set_regime_check(RegimeCheck check)
    {
        regime_check_ = std::move(check);
    }

    static const char* status_text(Status status);

private:
    void        run_buys(std::vector<OrderSignal>& out);
    void        run_due_exits(std::vector<OrderSignal>& out);
    void        run_close_exits(std::vector<OrderSignal>& out);
    void        run_exit_retries(std::vector<OrderSignal>& out);
    void        run_unfilled_cancels(std::vector<OrderSignal>& out);
    void        track_fills();
    void        roll_day();
    bool        emit_sell(Position& position, const std::string& reason, std::vector<OrderSignal>& out);
    OrderSignal make_signal(const std::string& ticker, OrderSide side, int quantity, double reference_price,
                            const std::string& reason) const;
    std::optional<bool> regime_allows_buy() const;
    bool        bought_same_signal(const std::string& ticker, const std::string& d0) const;
    bool        save_state();
    void        publish_owned();
    void        rebuild_index();
    bool        in_exit_window() const;
    int         kst_hhmm() const;
    std::string kst_date() const;

    Params                                       parameters_;
    OwnedSink                                    owned_sink_;
    Clock                                        clock_;
    RegimeCheck                                  regime_check_;
    std::string                                  id_;
    std::optional<surge::Plan>                   plan_;
    std::filesystem::file_time_type              plan_modified_{};
    std::vector<Position>                        positions_;
    nlohmann::json                               closed_ = nlohmann::json::array(); // 청산 기록(최근 100건). 같은 신호 판정에 쓴다
    std::unordered_map<symbol::SymbolId, size_t> index_of_symbol_; // 틱 → positions_ 칸. 정수 비교만 [why D-071]
    std::string                                  day_;             // 오늘 YYYYMMDD
    std::set<std::string>                        attempted_today_; // 오늘 매수 판정을 끝낸 종목(건너뜀 포함)
    int                                          bought_today_ = 0;
    int                                          last_executed_as_of_ = 0; // 집행을 시작한 계획의 as_of(YYYYMMDD)
    std::string                                  executed_on_;             // 그 계획을 집행한 날(YYYYMMDD)
    std::set<std::string>                        dry_sent_;
    std::vector<std::string>                     owned_published_;
    std::vector<std::string>                     owned_scratch_;           // publish_owned가 매번 새로 잡지 않게 재사용
    std::chrono::steady_clock::time_point        last_reload_{};
    bool                                         plan_stale_logged_     = false;
    bool                                         plan_done_logged_      = false;
    bool                                         regime_unknown_logged_ = false;
    bool                                         broken_logged_         = false;
    bool                                         state_broken_          = false; // 상태 파일을 못 읽었다 — 매수 금지, 덮어쓰기 금지
    bool                                         buys_stopped_today_    = false; // 상태 파일 쓰기 실패 — 오늘 매수 멈춤
    bool                                         dirty_                 = false; // 저장할 변경이 있다(on_clock이 쓴다)
};
