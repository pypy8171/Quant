#pragma once
#include "api/KisClient.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <deque>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 선별 단계에서 종목마다 KIS REST를 연속 호출하므로 호출 사이에 짧게 쉰다
// (초당 호출 한도(EGW00201) 회피용 호출 간격 조절 간격).
namespace
{
constexpr int kSdpRestIntervalMs = 60;
}

// ─────────────────────────────────────────────────────────────────────────────
// SupplyDemandPullbackStrategy  —  수급 선별 + 5일선 눌림목 진입
//
//  [선별 — on_start()]
//    유니버스(시총 순위 N종목) → 종목별 inquire-investor(N일 시계열)
//    쌍끌이 조건:
//      - lookback_days 중 외인>0 AND 기관>0 인 날 >= min_dual_days
//      - 누적 외인 순매수 > 0 AND 누적 기관 순매수 > 0
//    look-ahead 방지: 당일(today_yyyymmdd) 데이터 제외 (장 종료 후 확정치 사용)
//
//  [진입 모드 A — 일봉 스윙]
//    on_data(일봉): 5일선 눌림목 판정 → 당일 BUY (다음날 실질 진입)
//
//  [진입 모드 B — INTRADAY 일중]
//    on_start에서 전일 확정 일봉으로 reference_ma5_ 고정
//    on_trade/on_order_book에서 장중 5일선 눌림목 터치 포착 → 즉시 BUY
//    market_close_exit_hhmm 또는 moving_average_5 이탈 시 손절/청산
// ─────────────────────────────────────────────────────────────────────────────
class SupplyDemandPullbackStrategy : public StrategyBase
{
public:
    // 진입 모드 — StrategyType과 같은 스마트enum idiom. EntryMode::from_string으로
    //  config "entry_mode" 문자열을 파싱한다.
    class EntryMode
    {
    public:
        enum Value { DAILY, INTRADAY };

        EntryMode() = default;
        constexpr EntryMode(Value value) : value_(value) {}
        constexpr operator Value() const { return value_; }

        // INTRADAY가 아니면 DAILY로 본다(기존 (s=="INTRADAY")?INTRADAY:DAILY 관례 유지).
        static EntryMode from_string(const std::string& text)
        {
            return text == "INTRADAY" ? EntryMode(INTRADAY) : EntryMode(DAILY);
        }

    private:
        Value value_ = DAILY;
    };

    struct Params
    {
        std::string market_div       = "J";    // J: KOSPI/KOSDAQ 주식
        int         universe_size    = 50;     // 시총 상위 N 유니버스
        int         lookback_days    = 5;      // 수급 집계 기간
        int         min_dual_days    = 3;      // 쌍끌이 최소 일수
        int         min_consec_days  = 0;      // 연속 쌍끌이 최소 일수 (0=미사용)
        int64_t     net_buy_threshold= 0;      // 누적 순매수 하한 (수량)
        int         moving_average_period        = 5;
        double      pullback_band    = 0.01;   // moving_average_5 ±1% 눌림목 인식 밴드
        bool        require_previous_above = true; // 직전봉이 moving_average_5 위에 있었는지
        EntryMode   mode             = EntryMode::DAILY;
        int         quantity         = 10;
        std::string market_close_exit_hhmm    = "1500"; // INTRADAY 청산 시각
        double      stop_below_moving_average    = 0.0;    // moving_average_5*(1-stop) 이탈 손절 (0=미사용)
    };

    explicit SupplyDemandPullbackStrategy(Params parameters) : parameters_(std::move(parameters)) {}

    const std::string& id() const override;

    std::string describe() const override;

    std::vector<WatchSpec> get_watch_specifications() const override;

    // ── 선별 ─────────────────────────────────────────────────────────────────
    void on_start() override;

    // 일봉 모드에서만 일봉을 쓴다. 장중(INTRADAY) 모드면 on_data가 바로 빠져나가므로 폴링도 불필요.
    bool wants_daily_bars() const override { return parameters_.mode == EntryMode::DAILY; }

    // ── 일봉 모드 진입/청산 (일봉) ─────────────────────────────────────────────
    std::optional<OrderSignal> on_data(const MarketData& market_data) override;

    // ── INTRADAY 모드 진입/청산 (체결 이벤트) ────────────────────────────────
    std::optional<OrderSignal> on_trade(const TradeData& trade) override;

    std::optional<OrderSignal> on_order_book(const OrderBook&) override { return std::nullopt; }

    void on_stop() override;

private:
    // 수급 점수 계산 내부 타입 (전략 내부용, Types.h에 노출 불필요)
    struct Score
    {
        int64_t cumulative_foreign = 0;
        int64_t cumulative_institution    = 0;
        int     dual_days   = 0;
        int     consec_days = 0;
    };

    Score calculate_score(const std::string& /*ticker*/,
                     const std::vector<InvestorFlow>& flows,
                     const std::string& today) const;

    bool is_candidate(symbol::SymbolId id) const
    {
        return cand_set_.count(id) > 0;
    }

    // candidates_ 변경 시 cand_set_ 동기화 — 틱 경로는 id 집합만 본다
    void rebuild_set();

    static double simple_moving_average(const std::deque<double>& closes, int count);

    void trim(std::deque<double>& closes) const;

    OrderSignal make_signal(symbol::SymbolId sid, std::string_view ticker, OrderSide side, double price) const;

    static std::string today_yyyymmdd()
    {
        return kst::date_yyyymmdd(std::time(nullptr));
    }

    static bool valid_hhmm(const std::string& hhmm);

    static bool past_hhmm(const std::string& hhmm);

    Params                                               parameters_;
    std::vector<std::string>                               candidates_; // 문자열 — 구독 스펙·REST·로그
    std::unordered_set<symbol::SymbolId>                      cand_set_;   // O(1) 조회(id)
    std::unordered_map<symbol::SymbolId, std::deque<double>>  closes_;     // 일봉 ma용
    std::unordered_map<symbol::SymbolId, double>              reference_ma5_;    // INTRADAY 기준선
    std::unordered_set<symbol::SymbolId>                      held_;       // 보유 종목
};
