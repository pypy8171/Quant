#pragma once
// 목표 비중표 바스켓 슬리브 — 파이썬이 장 전에 쓴 Quant/config/basket_targets.json을 읽어 바스켓 자본 × 비중과
//  원장 보유의 차이만 주문한다. 신호 계산은 파일 쪽(가치 기울임·모멘텀), 이 전략은 집행과 소유권 격리만 맡는다.
//  - 집행 창 안에서 매도 레그 → (잔고 대조가 현금을 갱신할 시간) → 매수 레그. 창을 넘긴 잔량은 다음 날 파일이 다시 낸다.
//  - 종목·방향마다 하루 한 번만 낸다(상태 파일에 먼저 적고 낸다 — 재기동 뒤 같은 주문을 두 번 내지 않는다).
//  - 자기 종목을 OrderGate 슬롯 계산 밖으로 두고(owned_sink), DEVSCALE 유니버스에서 뺀다.
//  - 틱 구독이 없어 다른 전략의 틱을 심장박동으로 쓴다(get_watch_specifications 빈 벡터 = 전부 받는 전략).
//  [why D-109]
#include "strategy/StrategyBase.h"
#include "strategy/TargetBasketPlan.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

class TargetBasketStrategy : public StrategyBase
{
public:
    struct Params
    {
        std::string label = "MAIN";                  // id = BASKET_<label>. regime_strategies의 "BASKET_*"가 이걸 본다
        std::string account;
        std::string targets_file = "Quant/config/basket_targets.json";
        std::string state_file   = "Quant/config/basket_state.json";
        double      capital_krw  = 0.0;              // 고정 시드. 0이면 아무것도 안 낸다
        double      band         = 0.10;
        int         window_start_hhmm = 1440;        // 매도 레그 시작
        int         window_end_hhmm   = 1500;        // 이 뒤로는 새 주문을 안 낸다(잔량 이월)
        int         buy_leg_delay_sec = 90;          // 매도 레그 끝 → 매수 레그까지 대기(잔고 대조 30초 주기 × 3)
        int         max_signals_per_pass = 3;        // 한 번에 내는 신호 수(초당 한도 5 아래)
        int         pass_interval_ms     = 1000;
        int         reload_sec           = 60;       // 파일 변경 확인 주기
        bool        dry_run              = false;    // 계산·로그만, 주문 없음
    };

    using OwnedSink = std::function<void(const std::vector<std::string>&)>;
    using Clock     = std::function<std::time_t()>;

    TargetBasketStrategy(Params parameters, OwnedSink owned_sink);

    const std::string& id() const override
    {
        return id_;
    }

    std::string        describe() const override;
    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    void on_start() override;
    void on_stop() override;
    void on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out) override;

    // 로더가 기동 때 소유 종목을 LoadPass::scan_covered·DEVSCALE held에 넣으려고 읽는다. on_start 전에도 파일을 읽어 둔다.
    const std::vector<std::string>& owned_tickers() const
    {
        return owned_;
    }

    bool load_targets(); // 파일을 읽어 targets_를 바꾼다. 실패면 직전 것을 유지하고 false

    // 시험용 — 시각을 바꿔 집행 창을 지난다.
    void set_clock(Clock clock)
    {
        clock_ = std::move(clock);
    }

    const Params& parameters() const
    {
        return parameters_;
    }

private:
    struct DayState
    {
        std::string           date;        // YYYYMMDD
        std::string           targets_key;
        bool                  sell_leg_done = false;
        bool                  buy_leg_done  = false;
        std::time_t           sell_leg_at   = 0;
        std::set<std::string> sent;        // "ticker:BUY" — 하루 한 번
    };

    void run_pass(std::vector<OrderSignal>& out);
    basket::Plan current_plan() const;
    bool emit_leg(std::vector<basket::PlannedOrder>& orders, std::vector<OrderSignal>& out, int& budget);
    void load_state();
    void save_state() const;
    void publish_owned();
    int  kst_hhmm() const;
    std::string kst_date() const;

    Params                            parameters_;
    OwnedSink                         owned_sink_;
    Clock                             clock_;
    std::string                       id_;
    std::optional<basket::Targets>    targets_;
    std::filesystem::file_time_type   targets_mtime_{};
    std::vector<std::string>          owned_;
    DayState                          day_;
    double                            realized_pnl_krw_ = 0.0;
    std::set<std::string>             dry_sent_;        // dry_run에서 이미 로그한 주문(상태 파일엔 안 적는다)
    std::chrono::steady_clock::time_point last_pass_{};
    std::chrono::steady_clock::time_point last_reload_{};
    bool                              window_closed_logged_ = false;
    bool                              halt_logged_          = false;
};
