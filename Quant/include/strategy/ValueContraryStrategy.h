#pragma once
#include "api/KisClient.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ValueContraryStrategy  —  저PBR 3일 연속 하락 반전 매수
//
//  [on_start() — 장 시작 전]
//    KR: KIS 시가총액 순위 API → PBR ≤ pbr_max 전 종목 조회 (동적 Universe)
//    US: 내장 S&P500 리스트 → PBR/PER 필터
//    → 3일 연속 하락 체크 → candidates_ 확정
//
//  [진입 — on_order_book() KR / on_trade() US]
//    장 시작 후 첫 이벤트에서 시장가 매수 (4일차 시가 효과)
//
//  [청산 — on_order_book() / on_trade()]
//    eod_exit_hhmm(KST) 도달 시 시장가 청산
// ─────────────────────────────────────────────────────────────────────────────
class ValueContraryStrategy : public StrategyBase
{
public:
    // market   : Market::KR 또는 Market::US
    // exchange : US일 때 "NAS" / "NYS" (KR은 무시)
    // pbr_max  : PBR 상한 (0이면 PBR 조건 미적용)
    // eod_exit_hhmm : 청산 시각 KST (KR=1520, US=0330)
    ValueContraryStrategy(Market market, std::string exchange, double pbr_max, int quantity, int eod_exit_hhmm)
        : market_(market), exchange_(std::move(exchange)), pbr_max_(pbr_max), quantity_(quantity),
          eod_exit_hhmm_(eod_exit_hhmm)
    {
    }

    std::string id() const override
    {
        return std::string("VALUE_CONTRARY_") + (market_ == Market::KR ? "KR" : "US");
    }

    std::string describe() const override
    {
        return id() + " | PBR<=" + std::to_string(pbr_max_) + " | qty=" + std::to_string(quantity_) +
               " | EOD=" + std::to_string(eod_exit_hhmm_);
    }

    // ── 스크리닝 ──────────────────────────────────────────────────────────
    void on_start() override
    {
        candidates_.clear();
        buy_sent_.clear();
        sell_sent_.clear();

        if (!kis_)
        {
            LOG_WARN("[ValueContrary] KisClient 없음");
            return;
        }

        LOG_INFO("[ValueContrary] " + id() + " 스크리닝 시작");

        // 1. Universe 조회
        std::vector<std::string> universe;
        if (market_ == Market::KR)
        {
            // KOSPI(J) + KOSDAQ(W) 합산
            auto kospi = kis_->fetch_universe_by_pbr(pbr_max_ > 0 ? pbr_max_ : 999.0, "J");
            auto kosdaq = kis_->fetch_universe_by_pbr(pbr_max_ > 0 ? pbr_max_ : 999.0, "W");
            universe.insert(universe.end(), kospi.begin(), kospi.end());
            universe.insert(universe.end(), kosdaq.begin(), kosdaq.end());
        }
        else
        {
            universe = kis_->fetch_us_universe_by_pbr(pbr_max_, exchange_);
        }

        LOG_INFO("[ValueContrary] Universe 크기: " + std::to_string(universe.size()));

        // 2. 3일 연속 하락 필터
        for (const auto& ticker : universe)
        {
            std::vector<MarketData> bars;
            if (market_ == Market::KR)
                bars = kis_->get_daily_ohlcv(ticker, 5);
            else
                bars = kis_->get_us_daily_ohlcv(ticker, 5, exchange_);

            // KIS 초당 거래건수 제한 — 매 호출 후 대기
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // 오늘 미완성 bar 제거 (pre-market or 장 개시 전: volume=0)
            while (!bars.empty() && bars[0].volume == 0)
                bars.erase(bars.begin());

            if ((int)bars.size() < 4)
                continue;

            // bars[0]=최근 완성 거래일, bars[1~3]=그 이전 3일
            bool declining =
                bars[0].close < bars[1].close && bars[1].close < bars[2].close && bars[2].close < bars[3].close;
            if (!declining)
                continue;

            candidates_.insert(ticker);
            LOG_INFO("[ValueContrary] 후보: " + ticker + "  종가 " + std::to_string(bars[0].close) + " < " +
                     std::to_string(bars[1].close) + " < " + std::to_string(bars[2].close));
        }

        LOG_INFO("[ValueContrary] 후보 확정: " + std::to_string(candidates_.size()) + "종목");
    }

    // ── WS 구독 스펙 — on_start() 이후 candidates_ 기준 ──────────────────
    std::vector<WatchSpec> get_watch_specs() const override
    {
        std::vector<WatchSpec> specs;
        for (const auto& tk : candidates_)
        {
            WatchSpec s;
            s.ticker = tk;
            s.market = market_;
            s.exchange = exchange_;
            specs.push_back(s);
        }
        return specs;
    }

    // ── 일봉 — 이 전략은 이벤트 드리븐으로만 동작 ────────────────────────
    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    // ── 호가 이벤트 (국내 전용) ───────────────────────────────────────────
    std::optional<OrderSignal> on_order_book(const OrderBook& ob) override
    {
        if (market_ != Market::KR)
            return std::nullopt;
        return check_entry_exit(ob.ticker, ob.time);
    }

    // ── 체결 이벤트 (미국 + 국내) ─────────────────────────────────────────
    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        if (td.market != market_)
            return std::nullopt;
        return check_entry_exit(td.ticker, td.time);
    }

    void on_stop() override
    {
        LOG_INFO("[ValueContrary] " + id() + " 매수=" + std::to_string(buy_sent_.size()) +
                 " 청산=" + std::to_string(sell_sent_.size()));
    }

private:
    // 진입·청산 공통 로직
    std::optional<OrderSignal> check_entry_exit(const std::string& ticker, const std::string& time_str)
    {
        int hhmm = parse_hhmm(time_str);
        if (!is_in_session(hhmm))
            return std::nullopt;

        // 진입: 후보이고 아직 매수 안 했으면 (국면 게이트 적용)
        if (is_active() && candidates_.count(ticker) && !buy_sent_.count(ticker))
        {
            buy_sent_.insert(ticker);
            candidates_.erase(ticker);

            OrderSignal sig;
            sig.ticker = ticker;
            sig.side = OrderSide::BUY;
            sig.type = OrderType::MARKET;
            sig.quantity = quantity_;
            sig.market = market_;
            sig.exchange = exchange_;
            sig.strategy_id = id();
            sig.timestamp = std::chrono::system_clock::now();

            LOG_INFO("[ValueContrary] BUY: " + ticker + " @" + time_str);
            return sig;
        }

        // 청산: 매수 보냈고, 청산 안 했고, 청산 시각 도달
        if (buy_sent_.count(ticker) && !sell_sent_.count(ticker) && hhmm >= eod_exit_hhmm_)
        {
            sell_sent_.insert(ticker);

            OrderSignal sig;
            sig.ticker = ticker;
            sig.side = OrderSide::SELL;
            sig.type = OrderType::MARKET;
            sig.quantity = quantity_;
            sig.market = market_;
            sig.exchange = exchange_;
            sig.strategy_id = id();
            sig.timestamp = std::chrono::system_clock::now();

            LOG_INFO("[ValueContrary] SELL(EOD): " + ticker + " @" + time_str);
            return sig;
        }

        return std::nullopt;
    }

    // time_str: HHMMSS → HHMM 정수
    static int parse_hhmm(const std::string& t)
    {
        if (t.size() < 4)
            return 0;
        try
        {
            return std::stoi(t.substr(0, 2)) * 100 + std::stoi(t.substr(2, 2));
        }
        catch (...)
        {
            return 0;
        }
    }

    // 장 세션 내 여부 (KST 기준)
    bool is_in_session(int hhmm) const
    {
        if (market_ == Market::KR)
            return hhmm >= 900 && hhmm < 1530;
        // 미국 정규장 KST: 22:30~익일 05:00
        return hhmm >= 2230 || hhmm < 500;
    }

    Market market_;
    std::string exchange_;
    double pbr_max_;
    int quantity_;
    int eod_exit_hhmm_;

    std::unordered_set<std::string> candidates_;
    std::unordered_set<std::string> buy_sent_;
    std::unordered_set<std::string> sell_sent_;
};
