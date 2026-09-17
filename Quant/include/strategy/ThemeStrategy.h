#pragma once
#include "api/KisClient.h"
#include "core/MarketSession.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <algorithm>
#include <functional>
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
// (초당 호출 한도(EGW00201) 회피용 호출 간격 조절 간격). 지수 조회가 더 길다.
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
    // KRX 정본. 2026-09-08 구성종목으로 확증(직전 표는 이름이 밀려 있었다 — 0017을 "통신업"으로
    //  불렀으나 구성은 한국전력·한국가스공사, 즉 전기가스업). 0022(은행)·0023은 폐지돼 지수 0.00.
    {"0005","음식료품"},{"0006","섬유의복"},{"0007","종이목재"},{"0008","화학"},
    {"0009","의약품"},{"0010","비금속광물"},{"0011","철강금속"},{"0012","기계"},
    {"0013","전기전자"},{"0014","의료정밀"},{"0015","운수장비"},{"0016","유통업"},
    {"0017","전기가스업"},{"0018","건설업"},{"0019","운수창고"},{"0020","통신업"},
    {"0021","금융업"},{"0024","증권"},{"0025","보험"},{"0026","서비스업"}
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
               " | 장 마감=" + std::to_string(eod_exit_hhmm_);
    }

    std::vector<WatchSpec> get_watch_specs() const override
    {
        std::vector<WatchSpec> specs;

        for (const auto& ticker : candidates_)
        {
            specs.push_back({ticker, Market::KR, ""});
        }

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
        {
            sectors = KOSPI_SECTORS;
        }
        else
        {
            for (const auto& code : sector_codes_)
            {
                sectors.push_back({code, code});
            }
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
        std::ranges::sort(momentum_rank, std::ranges::greater{},
                          [](const auto& element) { return element.first; });

        int count = std::min(top_n_sectors_, static_cast<int>(momentum_rank.size()));
        LOG_INFO("[ThemeStrategy] Step1 완료 — 상위 " + std::to_string(count) + "개 업종 선택:");

        for (int index = 0; index < count; ++index)
        {
            LOG_INFO("  [" + std::to_string(index+1) + "] " + momentum_rank[index].second +
                     " (" + std::to_string(momentum_rank[index].first).substr(0,6) + "%)");
        }

        // ── Step 2: 업종 내 종목 스캔 + 거래량 급증 필터 ─────────────────
        LOG_INFO("[ThemeStrategy] Step2 — 거래량 급증 종목 스캔");

        std::vector<std::string> surge_candidates;

        for (int index = 0; index < count; ++index)
        {
            const std::string& sector_code = momentum_rank[index].second;
            auto ranked = kis_->fetch_sector_ranking(sector_code, 30);
            std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestPacingMs));

            for (const auto& stock : ranked)
            {
                if (surge_candidates.size() >= kThemeMaxSurgeCandidates)
                {
                    break;  // 안전 상한
                }

                auto bars = kis_->get_daily_ohlcv(stock.ticker, 21);
                std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestPacingMs));

                if (bars.size() < 5)
                {
                    continue;
                }

                // 20일 평균 거래량
                double volume_sum = 0;
                int volume_count = std::min(20, static_cast<int>(bars.size()) - 1);

                for (int volume_index = 1; volume_index <= volume_count; ++volume_index)
                {
                    volume_sum += static_cast<double>(bars[volume_index].volume);
                }

                double avg_vol = volume_sum / volume_count;

                if (avg_vol <= 0)
                {
                    continue;
                }

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
            for (const auto& ticker : surge_candidates)
            {
                candidates_.insert(ticker);
            }
        }
        else
        {
            LOG_INFO("[ThemeStrategy] Step3 — 수급 필터 (외국인+기관 동시 순매수)");

            for (const auto& ticker : surge_candidates)
            {
                auto trend = kis_->get_investor_trend(ticker);
                std::this_thread::sleep_for(std::chrono::milliseconds(kThemeRestPacingMs));

                if (trend.foreign_net > 0 && trend.institution_net > 0)
                {
                    LOG_INFO("[ThemeStrategy] 수급 통과: " + ticker +
                             " 외국인=" + std::to_string(trend.foreign_net) +
                             " 기관=" + std::to_string(trend.institution_net));
                    candidates_.insert(ticker);
                }
            }
        }

        LOG_INFO("[ThemeStrategy] 최종 후보: " +
                 std::to_string(candidates_.size()) + "종목");

        // 틱 경로는 id로만 본다 — 후보 문자열은 구독 스펙·로그용으로 남긴다.
        pending_.clear();

        for (const auto& ticker : candidates_)
        {
            pending_.insert(symbol_of(ticker));
        }

        for (const auto& ticker : candidates_)
        {
            LOG_INFO("  → " + ticker);
        }
    }

    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 호가 이벤트 — 진입/청산
    std::optional<OrderSignal> on_order_book(const OrderBook& order_book) override
    {
        double price = order_book.asks[0].price > 0 ? order_book.asks[0].price : order_book.bids[0].price;
        return check_entry_exit(order_book.symbol_id, order_book.ticker, order_book.hhmmss, price);
    }

    // 체결 이벤트 — 호가 보완
    std::optional<OrderSignal> on_trade(const TradeData& trade) override
    {
        if (trade.market != Market::KR)
        {
            return std::nullopt;
        }

        return check_entry_exit(trade.symbol_id, trade.ticker, trade.hhmmss, trade.price);
    }

    void on_stop() override
    {
        LOG_INFO("[ThemeStrategy] 종료: 매수=" + std::to_string(buy_sent_.size()) +
                 " 청산=" + std::to_string(sell_sent_.size()));
    }

private:
    std::optional<OrderSignal> check_entry_exit(symbol::SymbolId symbol_id,
                                                 std::string_view ticker,
                                                 int32_t hhmmss,
                                                 double ref_px)
    {
        int hhmm = hhmmss / 100;

        if (!krx::in_session(hhmm))
        {
            return std::nullopt;  // 09:00~15:30 정규장만(core/MarketSession.h)
        }

        if (symbol_id == symbol::kNone)
        {
            symbol_id = symbol_of(ticker); // id 없이 온 틱(옛 경로) — 찍힌 게 정상이라 여기는 드물다
        }

        // 진입: 후보이고 아직 매수 안 했으면 (국면 게이트 적용)
        if (is_active() && pending_.count(symbol_id) && !buy_sent_.count(symbol_id))
        {
            buy_sent_.insert(symbol_id);
            pending_.erase(symbol_id);
            candidates_.erase(std::string(ticker));

            OrderSignal signal;
            signal.ticker      = ticker;
            signal.symbol_id         = symbol_id;
            signal.side        = OrderSide::BUY;
            signal.type        = OrderType::MARKET;
            signal.quantity    = quantity_;
            signal.ref_price   = ref_px;  // 시장가 명목 백스톱 기준가(현재가/체결가)
            signal.market      = Market::KR;
            signal.strategy_id = id();
            signal.timestamp   = std::chrono::system_clock::now();

            LOG_INFO("[ThemeStrategy] BUY: " + std::string(ticker) + " @" + krx::hhmmss_str(hhmmss));
            return signal;
        }

        // 청산: 매수했고, 아직 청산 안 했고, 청산 시각 도달
        if (buy_sent_.count(symbol_id) && !sell_sent_.count(symbol_id) && hhmm >= eod_exit_hhmm_)
        {
            sell_sent_.insert(symbol_id);

            OrderSignal signal;
            signal.ticker      = ticker;
            signal.symbol_id         = symbol_id;
            signal.side        = OrderSide::SELL;
            signal.type        = OrderType::MARKET;
            signal.quantity    = quantity_;
            signal.ref_price   = ref_px;  // 시장가 명목 백스톱 기준가(현재가/체결가)
            signal.market      = Market::KR;
            signal.strategy_id = id();
            signal.timestamp   = std::chrono::system_clock::now();

            LOG_INFO("[ThemeStrategy] SELL(장 마감): " + std::string(ticker) + " @" + krx::hhmmss_str(hhmmss));
            return signal;
        }

        return std::nullopt;
    }

    std::vector<std::string>          sector_codes_;
    int                               top_n_sectors_;
    double                            volume_surge_mult_;
    bool                              inst_filter_;
    int                               quantity_;
    int                               eod_exit_hhmm_;

    std::unordered_set<std::string>   candidates_; // 문자열 — 구독 스펙·로그. 틱 경로는 아래 id 집합만 본다
    std::unordered_set<symbol::SymbolId> pending_;    // 매수 대기 후보 id
    std::unordered_set<symbol::SymbolId> buy_sent_;
    std::unordered_set<symbol::SymbolId> sell_sent_;
};
