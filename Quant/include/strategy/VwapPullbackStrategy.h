#pragma once
// 강한 종목 첫 VWAP 눌림(VWAPPB) — 한 종목짜리 전략. 09:30에 고른 종목의 1분봉을 모아 PullbackMachine에 넣고,
//  무장·해제·신호·청산을 logs/vwappb_shadow_YYYYMMDD.csv에 적는다. Phase 1은 그림자 모드라 주문을 내지 않는다
//  (on_trade_batch의 out에 아무것도 넣지 않는다). 숫자 규칙은 전부 strategy/VwapPullbackRules.h에 있다.
//  - 봉: 체결 틱을 1분 집계기로 모으고, 기동 때 REST 1분봉(09:00부터)을 한 번 받아 앞 분을 채운 뒤에야 상태 기계를 돌린다.
//  - 15:10 뒤: REST 1분봉을 다시 받아 같은 규칙으로 처음부터 돌려 보고 실시간 결과와 같은지 RECHECK 행으로 남긴다.
//  - 진입 가능 여부(다른 전략 보유·청산 관리·바스켓 소유·신규매수 차단)는 신호 행의 blocked 열로만 남긴다.
//  스레드: on_trade_batch·on_start·on_stop은 소유 샤드 스레드, run_prefetch_once는 프리페치 풀 스레드.
//   둘 사이에는 받은 REST 봉 사본 포인터만 오간다(fetch_mutex_).
//  관련 결정: D-109(슬리브 소유권), D-115(프리페치 풀), D-071(종목 단위 순서 보장).
// 테스트: Quant/tests/test_vwap_pullback_strategy.cpp
#include "core/BarAggregator.h"
#include "strategy/StrategyBase.h"
#include "strategy/VwapPullbackRules.h"
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class VwapPullbackStrategy : public StrategyBase
{
public:
    struct Params
    {
        std::string               account;
        std::string               ticker;
        std::string               name;
        double                    previous_close = 0.0; // 선정 때 시세판 값으로 되돌린 전일 종가
        vwap_pullback::RuleParams rules;
        bool                      shadow           = true; // Phase 1은 늘 true(로더가 강제한다)
        long long                 own_net_quantity = 0;    // 재기동 재인수(Full) 때 장부 순수량. 0이면 자기 보유 없음
        std::string               shadow_file;             // 비면 Logger 폴더의 vwappb_shadow_YYYYMMDD.csv
    };

    using Clock         = std::function<std::time_t()>;
    using MinuteFetcher = std::function<std::vector<MarketData>(const std::string& ticker, int count)>; // [0]=최신
    using BoardVwap     = std::function<double(const std::string& ticker)>; // 시세판 거래대금/거래량. 모르면 0
    using OwnerBlock    = std::function<std::string(symbol::SymbolId)>;    // 다른 슬리브 몫이면 사유, 아니면 빈 문자열

    explicit VwapPullbackStrategy(Params parameters);
    ~VwapPullbackStrategy() override;

    const std::string& id() const override
    {
        return id_;
    }

    std::string                describe() const override;
    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    void                   on_start() override;
    void                   on_stop() override;
    void                   on_trade_batch(const TradeData& trade, std::vector<OrderSignal>& out) override;
    std::vector<WatchSpec> get_watch_specifications() const override;

    // 주입 — 미주입이면 시계는 std::time, 분봉은 kis_, 시세판 VWAP은 MarketBoard, 소유 판정은 없음.
    void set_clock(Clock clock);
    void set_minute_fetcher(MinuteFetcher fetcher);
    void set_board_vwap(BoardVwap board_vwap);
    void set_owner_block(OwnerBlock owner_block);

    // 프리페치 한 번 — 풀이 주기마다 부른다. 시험은 풀 없이 직접 부른다.
    void run_prefetch_once();

    // 이 시각에 시간 청산을 한다면 팔 수량 = min(확정 잔고, 자기 순수량). 그림자 모드는 EXIT 행에만 적는다.
    int own_exit_quantity() const;

    const Params& parameters() const
    {
        return parameters_;
    }

    vwap_pullback::Phase phase() const
    {
        return machine_.phase();
    }

private:
    struct ShadowRow
    {
        std::string event;
        int         bar_hhmm = 0;
        std::string reason;
        double      price      = 0.0;
        double      vwap       = 0.0;
        double      board_vwap = 0.0;
        double      retrace    = 0.0;
        double      entry      = 0.0;
        double      stop       = 0.0;
        int         quantity   = 0;
        std::string blocked;
        std::string match;
    };

    std::time_t now() const;
    int         kst_hhmm() const;
    void        feed_closed_bars();
    void        handle_step(const vwap_pullback::MinuteBar& bar, const vwap_pullback::StepResult& step);
    void        track_shadow_position(const vwap_pullback::MinuteBar& bar);
    void        run_exit_checks(double last_price);
    void        run_recheck();
    std::string entry_block() const;
    double      board_vwap() const;
    std::filesystem::path shadow_path() const;
    void        write_row(const ShadowRow& row) const;
    void        restore_from_shadow_file();
    void        stop_prefetch();

    Params                         parameters_;
    std::string                    id_;
    symbol::SymbolId               symbol_id_ = symbol::kNone;
    vwap_pullback::PullbackMachine machine_;
    bars::BarAggregator            aggregator_;
    Clock                          clock_;
    MinuteFetcher                  minute_fetcher_;
    BoardVwap                      board_vwap_;
    OwnerBlock                     owner_block_;

    // 프리페치 풀 ↔ 샤드 스레드. 포인터는 각자 한 번만 채운다.
    mutable std::mutex                             fetch_mutex_;
    std::shared_ptr<const std::vector<MarketData>> bootstrap_bars_;
    std::shared_ptr<const std::vector<MarketData>> recheck_bars_;
    bool                                           recheck_wanted_ = false;
    uint64_t                                       prefetch_task_  = 0;

    // 샤드 스레드 전용
    bool        seeded_           = false;
    int         fed_closed_count_ = -1;    // 마지막으로 상태 기계에 넘길 때의 닫힌 봉 수
    int         last_feed_hhmm_   = -1;    // 마지막으로 넘긴 시각(분) — 분이 바뀌면 다시 본다
    bool        restored_done_    = false; // 오늘 이미 신호를 냈다(그림자 파일에서 복원)
    int         live_signal_hhmm_ = 0;     // 실시간으로 낸 신호의 봉 시각. 0=없음
    double      shadow_entry_     = 0.0;   // 그림자 진입가(신호가 막히지 않았을 때). 0=없음
    double      shadow_stop_      = 0.0;
    bool        shadow_exited_    = false;
    bool        own_exit_written_ = false;
    bool        summary_written_  = false;
    bool        recheck_written_  = false;
    double      last_price_       = 0.0;
    int64_t     websocket_ticks_  = 0;
    int64_t     rest_ticks_       = 0;
};
