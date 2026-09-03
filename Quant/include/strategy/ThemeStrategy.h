#pragma once
#include "api/KisClient.h"
#include "core/MarketSession.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ThemeStrategy  —  3단 필터 테마 모멘텀 전략
//
//  [on_start() — 장 시작 전 스크리닝]
//  1. 업종 지수 5일 모멘텀 → 상위 N개 업종 선택
//  2. 해당 업종 내 등락률/거래량 급증 종목 필터
//  3. 외국인 + 기관 동시 순매수 종목 → candidates_ 확정
//
//  [진입 — on_order_book() 호가 이벤트]
//  장 시작 후 첫 이벤트에서 시장가 매수 (1종목당 1회)
//
//  [청산 — on_order_book()]
//  eod_exit_hhmm 도달 시 시장가 청산
// ─────────────────────────────────────────────────────────────────────────────

// 스크리닝 단계에서 종목·업종마다 KIS REST를 연속 호출하므로 호출 사이에 짧게 쉰다
// (초당 호출 한도(EGW00201) 회피용 페이싱 간격). 지수 조회가 더 길다.
namespace
{
constexpr int kThemeIndexPacingMs = 500; // 업종 지수 일봉 조회 후 대기
constexpr int kThemeRestPacingMs  = 200; // 종목 순위·일봉·수급 조회 후 대기
constexpr size_t kThemeMaxSurgeCandidates = 50; // 거래량 급증 후보 안전 상한
}

// KOSPI 주요 업종 코드
// 0005:화학  0006:의약품  0008:철강금속  0009:기계  0010:전기전자
// 0011:의료정밀  0012:운수장비  0015:건설업  0022:서비스업
static const std::vector<std::pair<std::string,std::string>> KOSPI_SECTORS = {
    {"0005","화학"},{"0006","의약품"},{"0008","철강금속"},{"0009","기계"},
    {"0010","전기전자"},{"0011","의료정밀"},{"0012","운수장비"},
    {"0015","건설업"},{"0017","통신업"},{"0022","서비스업"}
};

class ThemeStrategy : public StrategyBase
{
public:
    // sector_codes: 스캔할 업종코드 목록 (빈 벡터면 KOSPI_SECTORS 전체)
    // top_n_sectors: 모멘텀 상위 N개 업종 선택
    // volume_surge_mult: 거래량 급증 배수 (최근 vs 20일 평균)
    // inst_filter: true면 외국인+기관 동시 순매수 필터 적용
    ThemeStrategy(std::vector<std::string> sector_codes,
                  int top_n_sectors,
                  double volume_surge_mult,
                  bool inst_filter,
                  int quantity,
                  int eod_exit_hhmm)
        : sector_codes_(std::move(sector_codes)),
          top_n_sectors_(top_n_sectors),
          volume_surge_mult_(volume_surge_mult),
          inst_filter_(inst_filter),
          quantity_(quantity),
          eod_exit_hhmm_(eod_exit_hhmm)
    {}

    std::string id() const override { return "THEME_KR"; }

    std::string describe() const override
    {
        return "THEME_KR | top_sectors=" + std::to_string(top_n_sectors_) +
               " | vol_surge=" + std::to_string(static_cast<int>(volume_surge_mult_)) + "x" +
               " | inst=" + (inst_filter_ ? "Y" : "N") +
               " | qty=" + std::to_string(quantity_) +
               " | EOD=" + std::to_string(eod_exit_hhmm_);
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        std::vector<WatchSpec> specs;
        for (const auto& tk : candidates_)
            specs.push_back({tk, Market::KR, ""});
        return specs;
    }

    // ── 스크리닝 ──────────────────────────────────────────────────────────
    void on_start() override
    {
        candidates_.clear();
        buy_sent_.clear();
        sell_sent_.clear();

        if (!kis_)
        {
            LOG_WARN("[ThemeStrategy] KisClient 없음");
            return;
        }

        // 스캔 대상 업종 결정
        std::vector<std::pair<std::string,std::string>> sectors;
        if (sector_codes_.empty())
            sectors = KOSPI_SECTORS;
        else
        {
            for (const auto& code : sector_codes_)
                sectors.push_back({code, code});
        }

        // ── Step 1: 업종 5일 모멘텀 계산 ─────────────────────────────────
        LOG_INFO("[ThemeStrategy] Step1 — 업종 모멘텀 계산 (" +
                 std::to_string(sectors.size()) + "개 업종)");

        std::vector<std::pair<double,std::string>> momentum_rank; // <수익률, 코드>
        for (const auto& [code, name] : sectors)
        {
            auto bars = kis_->get_index_daily_ohlcv(code, 6);
            std::this_thread::sleep_for(std::chrono::milliseconds(kThemeIndexPacingMs));

            if (bars.size() < 2 || bars[0].close <= 0 || bars.back().close <= 0)
            {
                LOG_WARN("[ThemeStrategy] " + name + "(" + code + ") 일봉 부족");
                continue;
            }
            // 최신이 bars[0], 과거가 bars[N-1]
            double ret = (bars[0].close - bars.back().close) / bars.back().close * 100.0;
            LOG_INFO("[ThemeStrategy] " + name + "(" + code + ") 5일 수익률: " +
                     std::to_string(ret).substr(0, 6) + "%");
            momentum_rank.push_back({ret, code});
        }

        // 수익률 내림차순 정렬 → 상위 N개 선택
        std::sort(momentum_rank.begin(), momentum_rank.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        int n = std::min(top_n_sectors_, static_cast<int>(momentum_rank.size()));
        LOG_INFO("[ThemeStrategy] Step1 완료 — 상위 " + std::to_string(n) + "개 업종 선택:");
        for (int i = 0; i < n; ++i)
            LOG_INFO("  [" + std::to_string(i+1) + "] " + momentum_rank[i].second +
                     " (" + std::to_string(momentum_rank[i].first).substr(0,6) + "%)");

        // ── Step 2: 업종 내 종목 스캔 + 거래량 급증 필터 ─────────────────
        LOG_INFO("[ThemeStrategy] Step2 — 거래량 급증 종목 스캔");

        std::vector<std::string> surge_candidates;
        for (int i = 0; i < n; ++i)
        {
            const std::string& sector_code = momentum_rank[i].second;
            auto ranked = kis_->fetch_sector_ranking(sector_code, 30);
            std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestPacingMs));

            for (const auto& stock : ranked)
            {
                if (surge_candidates.size() >= kThemeMaxSurgeCandidates) break; // 안전 상한

                auto bars = kis_->get_daily_ohlcv(stock.ticker, 21);
                std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestPacingMs));

                if (bars.size() < 5) continue;

                // 20일 평균 거래량
                double vol_sum = 0;
                int vol_cnt = std::min(20, static_cast<int>(bars.size()) - 1);
                for (int j = 1; j <= vol_cnt; ++j)
                    vol_sum += static_cast<double>(bars[j].volume);
                double avg_vol = vol_sum / vol_cnt;

                if (avg_vol <= 0) continue;

                double surge = static_cast<double>(bars[0].volume) / avg_vol;
                if (surge >= volume_surge_mult_)
                {
                    LOG_INFO("[ThemeStrategy] 거래량 급증: " + stock.ticker +
                             " " + stock.name +
                             " (배수: " + std::to_string(surge).substr(0, 4) + "x)");
                    surge_candidates.push_back(stock.ticker);
                }
            }
        }
        LOG_INFO("[ThemeStrategy] Step2 완료 — 거래량 급증 " +
                 std::to_string(surge_candidates.size()) + "종목");

        // ── Step 3: 외국인+기관 동시 순매수 필터 ─────────────────────────
        if (!inst_filter_)
        {
            // 필터 미적용 시 surge_candidates 바로 사용
            for (const auto& tk : surge_candidates)
                candidates_.insert(tk);
        }
        else
        {
            LOG_INFO("[ThemeStrategy] Step3 — 수급 필터 (외국인+기관 동시 순매수)");
            for (const auto& tk : surge_candidates)
            {
                auto trend = kis_->get_investor_trend(tk);
                std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestPacingMs));

                if (trend.foreign_net > 0 && trend.inst_net > 0)
                {
                    LOG_INFO("[ThemeStrategy] 수급 통과: " + tk +
                             " 외국인=" + std::to_string(trend.foreign_net) +
                             " 기관=" + std::to_string(trend.inst_net));
                    candidates_.insert(tk);
                }
            }
        }

        LOG_INFO("[ThemeStrategy] 최종 후보: " +
                 std::to_string(candidates_.size()) + "종목");
        for (const auto& tk : candidates_)
            LOG_INFO("  → " + tk);
    }

    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 호가 이벤트 — 진입/청산
    std::optional<OrderSignal> on_order_book(const OrderBook& ob) override
    {
        double px = ob.asks[0].price > 0 ? ob.asks[0].price : ob.bids[0].price;
        return check_entry_exit(ob.ticker, ob.time, px);
    }

    // 체결 이벤트 — 호가 보완
    std::optional<OrderSignal> on_trade(const TradeData& td) override
    {
        if (td.market != Market::KR) return std::nullopt;
        return check_entry_exit(td.ticker, td.time, td.price);
    }

    void on_stop() override
    {
        LOG_INFO("[ThemeStrategy] 종료: 매수=" + std::to_string(buy_sent_.size()) +
                 " 청산=" + std::to_string(sell_sent_.size()));
    }

private:
    std::optional<OrderSignal> check_entry_exit(const std::string& ticker,
                                                 const std::string& time_str,
                                                 double ref_px)
    {
        int hhmm = parse_hhmm(time_str);
        if (!krx::in_session(hhmm)) return std::nullopt; // 09:00~15:30 정규장만(core/MarketSession.h)

        // 진입: 후보이고 아직 매수 안 했으면 (국면 게이트 적용)
        if (is_active() && candidates_.count(ticker) && !buy_sent_.count(ticker))
        {
            buy_sent_.insert(ticker);
            candidates_.erase(ticker);

            OrderSignal sig;
            sig.ticker      = ticker;
            sig.side        = OrderSide::BUY;
            sig.type        = OrderType::MARKET;
            sig.quantity    = quantity_;
            sig.ref_price   = ref_px;  // 시장가 명목 백스톱 기준가(현재가/체결가)
            sig.market      = Market::KR;
            sig.strategy_id = id();
            sig.timestamp   = std::chrono::system_clock::now();

            LOG_INFO("[ThemeStrategy] BUY: " + ticker + " @" + time_str);
            return sig;
        }

        // 청산: 매수했고, 아직 청산 안 했고, 청산 시각 도달
        if (buy_sent_.count(ticker) && !sell_sent_.count(ticker) && hhmm >= eod_exit_hhmm_)
        {
            sell_sent_.insert(ticker);

            OrderSignal sig;
            sig.ticker      = ticker;
            sig.side        = OrderSide::SELL;
            sig.type        = OrderType::MARKET;
            sig.quantity    = quantity_;
            sig.ref_price   = ref_px;  // 시장가 명목 백스톱 기준가(현재가/체결가)
            sig.market      = Market::KR;
            sig.strategy_id = id();
            sig.timestamp   = std::chrono::system_clock::now();

            LOG_INFO("[ThemeStrategy] SELL(EOD): " + ticker + " @" + time_str);
            return sig;
        }

        return std::nullopt;
    }

    static int parse_hhmm(const std::string& t)
    {
        if (t.size() < 4) return 0;
        try { return std::stoi(t.substr(0, 2)) * 100 + std::stoi(t.substr(2, 2)); }
        catch (...) { return 0; }
    }

    std::vector<std::string>          sector_codes_;
    int                               top_n_sectors_;
    double                            volume_surge_mult_;
    bool                              inst_filter_;
    int                               quantity_;
    int                               eod_exit_hhmm_;

    std::unordered_set<std::string>   candidates_;
    std::unordered_set<std::string>   buy_sent_;
    std::unordered_set<std::string>   sell_sent_;
};
