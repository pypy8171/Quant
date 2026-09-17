#pragma once
#include "api/KisClient.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 선별 단계에서 종목마다 KIS REST를 연속 호출하므로 호출 사이에 짧게 쉰다
// (초당 호출 한도(EGW00201) 회피용 호출 간격 조절 간격).
namespace
{
constexpr int kSdpRestPacingMs = 60;
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
//  [진입 모드 A — EOD 스윙]
//    on_data(일봉): 5일선 눌림목 판정 → 당일 BUY (다음날 실질 진입)
//
//  [진입 모드 B — INTRADAY 일중]
//    on_start에서 전일 확정 일봉으로 ref_ma5_ 고정
//    on_trade/on_order_book에서 장중 5일선 눌림목 터치 포착 → 즉시 BUY
//    eod_exit_hhmm 또는 ma5 이탈 시 손절/청산
// ─────────────────────────────────────────────────────────────────────────────
class SupplyDemandPullbackStrategy : public StrategyBase
{
public:
    // 진입 모드 — StrategyType과 같은 스마트enum idiom. EntryMode::from_string으로
    //  config "entry_mode" 문자열을 파싱한다.
    class EntryMode
    {
    public:
        enum Value { EOD, INTRADAY };

        EntryMode() = default;
        constexpr EntryMode(Value value) : value_(value) {}
        constexpr operator Value() const { return value_; }

        // INTRADAY가 아니면 EOD로 본다(기존 (s=="INTRADAY")?INTRADAY:EOD 관례 유지).
        static EntryMode from_string(const std::string& text)
        {
            return text == "INTRADAY" ? EntryMode(INTRADAY) : EntryMode(EOD);
        }

    private:
        Value value_ = EOD;
    };

    struct Params
    {
        std::string market_div       = "J";    // J: KOSPI/KOSDAQ 주식
        int         universe_size    = 50;     // 시총 상위 N 유니버스
        int         lookback_days    = 5;      // 수급 집계 기간
        int         min_dual_days    = 3;      // 쌍끌이 최소 일수
        int         min_consec_days  = 0;      // 연속 쌍끌이 최소 일수 (0=미사용)
        int64_t     net_buy_threshold= 0;      // 누적 순매수 하한 (수량)
        int         ma_period        = 5;
        double      pullback_band    = 0.01;   // ma5 ±1% 눌림목 인식 밴드
        bool        require_prev_above = true; // 직전봉이 ma5 위에 있었는지
        EntryMode   mode             = EntryMode::EOD;
        int         quantity         = 10;
        std::string eod_exit_hhmm    = "1500"; // INTRADAY 청산 시각
        double      stop_below_ma    = 0.0;    // ma5*(1-stop) 이탈 손절 (0=미사용)
    };

    explicit SupplyDemandPullbackStrategy(Params params) : p_(std::move(params)) {}

    std::string id() const override { return "SUPPLY_DEMAND_PULLBACK"; }
    std::string describe() const override
    {
        return id() + " | uni=" + std::to_string(p_.universe_size) +
               " | dual>=" + std::to_string(p_.min_dual_days) +
               " | band=" + std::to_string(static_cast<int>(p_.pullback_band * 100)) + "%" +
               " | qty=" + std::to_string(p_.quantity) +
               " | mode=" + (p_.mode == EntryMode::EOD ? "EOD" : "INTRADAY");
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        std::vector<WatchSpec> specs;

        if (p_.mode != EntryMode::INTRADAY)
        {
            return specs;
        }

        for (const auto& ticker : candidates_)
        {
            specs.push_back({ticker, Market::KR, "", /*trade_only=*/true});
        }

        return specs;
    }

    // ── 선별 ─────────────────────────────────────────────────────────────────
    void on_start() override
    {
        if (!kis_) { LOG_ERROR("[SDP] KisClient 없음"); return; }

        candidates_.clear();
        closes_.clear();
        ref_ma5_.clear();
        held_.clear();

        const std::string today = today_yyyymmdd();

        // 1. 유니버스: 시총 상위 N
        auto ranked = kis_->fetch_kr_ranking(p_.universe_size, p_.market_div);
        LOG_INFO("[SDP] 유니버스: " + std::to_string(ranked.size()) + "종목");

        // 2. 종목별 수급 시계열 조회 → 쌍끌이 점수
        for (const auto& stock : ranked)
        {
            const std::string& ticker = stock.ticker;
            auto flows = kis_->get_investor_flow(ticker, p_.market_div);
            std::this_thread::sleep_for(std::chrono::milliseconds(kSdpRestPacingMs));

            if (flows.empty())
            {
                continue;
            }

            Score sc = calc_score(ticker, flows, today);
            bool dual_ok  = sc.dual_days  >= p_.min_dual_days;
            bool cons_ok  = (p_.min_consec_days == 0) || (sc.consec_days >= p_.min_consec_days);
            bool cum_ok   = sc.cum_foreign > p_.net_buy_threshold
                         && sc.cum_inst    > p_.net_buy_threshold;

            if (dual_ok && cons_ok && cum_ok)
            {
                candidates_.push_back(ticker);
                LOG_INFO("[SDP] 후보: " + ticker + " " + stock.name +
                         " | 쌍끌이=" + std::to_string(sc.dual_days) + "일" +
                         " | 연속=" + std::to_string(sc.consec_days) + "일" +
                         " | 외인누적=" + std::to_string(sc.cum_foreign) +
                         " | 기관누적=" + std::to_string(sc.cum_inst));
            }
        }

        LOG_INFO("[SDP] 수급 필터 통과: " + std::to_string(candidates_.size()) + "종목");

        // candidates_ 확정 후 O(1) 조회용 set 동기화 (필수 — 누락 시 is_candidate가 항상 false)
        rebuild_set();

        // 3-A. EOD 모드: 최근 일봉으로 ma 초기화
        if (p_.mode == EntryMode::EOD)
        {
            for (const auto& ticker : candidates_)
            {
                auto bars = kis_->get_daily_ohlcv(ticker, p_.ma_period + 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(kSdpRestPacingMs));
                auto& closes = closes_[symbol_of(ticker)];

                for (auto iterator = bars.rbegin(); iterator != bars.rend(); ++iterator)
                {
                    closes.push_back(iterator->close);
                }

                trim(closes);
            }
        }

        // 3-B. INTRADAY 모드: 전일 확정 일봉으로 ref_ma5_ 고정
        else
        {
            for (const auto& ticker : candidates_)
            {
                auto bars = kis_->get_daily_ohlcv(ticker, p_.ma_period + 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(kSdpRestPacingMs));

                if (static_cast<int>(bars.size()) < p_.ma_period)
                {
                    continue;
                }

                // bars[0]=최신(당일 미완성 가능) → bars[1..ma_period] 사용
                double sum = 0.0;
                // 현재는 두 가지(당일봉 유무)를 구분하지 않고 항상 1부터 쓴다(보수적).
                // 삼항의 두 분기 값이 같아 실질 무조건 1 — 당일봉 처리 분기 지점만 남겨둔 자리(보류 목록).
                int start = (bars[0].volume == 0) ? 1 : 1;

                if (static_cast<int>(bars.size()) <= start + p_.ma_period - 1)
                {
                    continue;
                }

                for (int ma_period_index = start; ma_period_index < start + p_.ma_period; ++ma_period_index)
                {
                    sum += bars[ma_period_index].close;
                }

                ref_ma5_[symbol_of(ticker)] = sum / p_.ma_period;
            }
        }
    }

    // EOD 모드에서만 일봉을 쓴다. 장중(INTRADAY) 모드면 on_data가 바로 빠져나가므로 폴링도 불필요.
    bool wants_daily_bars() const override { return p_.mode == EntryMode::EOD; }

    // ── EOD 모드 진입/청산 (일봉) ─────────────────────────────────────────────
    std::optional<OrderSignal> on_data(const MarketData& market_data) override
    {
        if (p_.mode != EntryMode::EOD)
        {
            return std::nullopt;
        }

        const symbol::SymbolId id = market_data.symbol_id != symbol::kNone ? market_data.symbol_id : symbol_of(market_data.ticker);

        if (!is_candidate(id))
        {
            return std::nullopt;
        }

        auto& closes = closes_[id];
        double prev_close = closes.empty() ? market_data.close : closes.back();
        closes.push_back(market_data.close);
        trim(closes);

        if (static_cast<int>(closes.size()) < p_.ma_period)
        {
            return std::nullopt;
        }

        double ma    = sma(closes, p_.ma_period);
        double price = market_data.close;

        // 보유 중이면 손절만 체크
        if (held_.count(id))
        {
            if (p_.stop_below_ma > 0.0 && price < ma * (1.0 - p_.stop_below_ma))
            {
                held_.erase(id);
                return make_signal(id, market_data.ticker, OrderSide::SELL, price);
            }

            return std::nullopt;
        }

        // 눌림목 진입 조건
        bool prev_above = !p_.require_prev_above || (prev_close >= ma);
        bool in_band    = price >= ma * (1.0 - p_.pullback_band)
                       && price <= ma * (1.0 + p_.pullback_band);
        bool supported  = price >= ma;

        if (is_active() && prev_above && in_band && supported)   // 진입 — 국면 게이트
        {
            held_.insert(id);
            return make_signal(id, market_data.ticker, OrderSide::BUY, price);
        }

        return std::nullopt;
    }

    // ── INTRADAY 모드 진입/청산 (체결 이벤트) ────────────────────────────────
    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        if (p_.mode != EntryMode::INTRADAY)
        {
            return std::nullopt;
        }

        if (trade.market != Market::KR)
        {
            return std::nullopt;
        }

        const symbol::SymbolId id = trade.symbol_id != symbol::kNone ? trade.symbol_id : symbol_of(trade.ticker);

        if (!is_candidate(id))
        {
            return std::nullopt;
        }

        auto ref = ref_ma5_.find(id);

        if (ref == ref_ma5_.end())
        {
            return std::nullopt;
        }

        double ma    = ref->second;
        double price = trade.price;

        // 청산 시각 도달
        if (past_hhmm(p_.eod_exit_hhmm))
        {
            if (held_.count(id))
            {
                held_.erase(id);
                return make_signal(id, trade.ticker, OrderSide::SELL, price);
            }

            return std::nullopt;
        }

        // 보유 중 손절
        if (held_.count(id))
        {
            if (p_.stop_below_ma > 0.0 && price < ma * (1.0 - p_.stop_below_ma))
            {
                held_.erase(id);
                return make_signal(id, trade.ticker, OrderSide::SELL, price);
            }

            return std::nullopt;
        }

        // 눌림목 진입
        bool in_band   = price >= ma * (1.0 - p_.pullback_band)
                      && price <= ma * (1.0 + p_.pullback_band);
        bool supported = price >= ma;

        if (is_active() && in_band && supported)   // 진입 — 국면 게이트
        {
            held_.insert(id);
            return make_signal(id, trade.ticker, OrderSide::BUY, price);
        }

        return std::nullopt;
    }

    std::optional<OrderSignal> on_order_book(const OrderBook&) override { return std::nullopt; }

    void on_stop() override
    {
        LOG_INFO("[SDP] 종료: 진입=" + std::to_string(held_.size()) + "종목 보유");
    }

private:
    // 수급 점수 계산 내부 타입 (전략 내부용, Types.h에 노출 불필요)
    struct Score
    {
        int64_t cum_foreign = 0;
        int64_t cum_inst    = 0;
        int     dual_days   = 0;
        int     consec_days = 0;
    };

    Score calc_score(const std::string& /*ticker*/,
                     const std::vector<InvestorFlow>& flows,
                     const std::string& today) const
    {
        Score sc;
        int used = 0;
        bool consec_broken = false;

        for (const auto& flow : flows)
        {
            if (flow.date == today)
            {
                continue;  // look-ahead 방지: 당일 제외
            }

            if (used >= p_.lookback_days)
            {
                break;
            }

            ++used;

            bool dual = (flow.foreign_net > 0) && (flow.institution_net > 0);
            sc.cum_foreign += flow.foreign_net;
            sc.cum_inst    += flow.institution_net;

            if (dual)
            {
                sc.dual_days++;
            }

            if (!consec_broken)
            {
                if (dual)
                {
                    sc.consec_days++;
                }
                else
                {
                    consec_broken = true;
                }
            }
        }

        return sc;
    }

    bool is_candidate(symbol::SymbolId id) const
    {
        return cand_set_.count(id) > 0;
    }

    // candidates_ 변경 시 cand_set_ 동기화 — 틱 경로는 id 집합만 본다
    void rebuild_set()
    {
        cand_set_.clear();

        for (const auto& ticker : candidates_)
        {
            cand_set_.insert(symbol_of(ticker));
        }
    }

    static double sma(const std::deque<double>& closes, int count)
    {
        if (static_cast<int>(closes.size()) < count)
        {
            return 0.0;
        }

        double sum = 0.0;

        for (int close_index = static_cast<int>(closes.size()) - count; close_index < static_cast<int>(closes.size()); ++close_index)
        {
            sum += closes[close_index];
        }

        return sum / count;
    }

    void trim(std::deque<double>& closes) const
    {
        while (static_cast<int>(closes.size()) > p_.ma_period + 2)
        {
            closes.pop_front();
        }
    }

    OrderSignal make_signal(symbol::SymbolId sid, std::string_view ticker, OrderSide side, double price) const
    {
        OrderSignal signal;
        signal.ticker      = ticker;
        signal.symbol_id         = sid;
        signal.side        = side;
        signal.type        = OrderType::MARKET;
        signal.quantity    = p_.quantity;
        signal.price       = price;
        signal.market      = Market::KR;
        signal.strategy_id = id();
        signal.timestamp   = std::chrono::system_clock::now();
        return signal;
    }

    static std::string today_yyyymmdd()
    {
        return kst::date_yyyymmdd(std::time(nullptr));
    }

    static bool past_hhmm(const std::string& hhmm)
    {
        if (hhmm.size() < 4)
        {
            return false;
        }

        const auto time_of_day    = kst::time_of_day(std::time(nullptr));
        const int  now    = static_cast<int>(time_of_day.hours().count() * 100 + time_of_day.minutes().count());
        const int  target = std::stoi(hhmm.substr(0, 2)) * 100 + std::stoi(hhmm.substr(2, 2));
        return now >= target;
    }

    Params                                               p_;
    std::vector<std::string>                               candidates_; // 문자열 — 구독 스펙·REST·로그
    std::unordered_set<symbol::SymbolId>                      cand_set_;   // O(1) 조회(id)
    std::unordered_map<symbol::SymbolId, std::deque<double>>  closes_;     // EOD ma용
    std::unordered_map<symbol::SymbolId, double>              ref_ma5_;    // INTRADAY 기준선
    std::unordered_set<symbol::SymbolId>                      held_;       // 보유 종목
};
