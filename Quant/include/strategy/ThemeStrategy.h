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
//  market_close_exit_hhmm 도달 시 시장가 청산
// ─────────────────────────────────────────────────────────────────────────────

// 스크리닝 단계에서 종목·업종마다 KIS REST를 연속 호출하므로 호출 사이에 짧게 쉰다
// (초당 호출 한도(EGW00201) 회피용 호출 간격 조절 간격). 지수 조회가 더 길다.
namespace
{
constexpr int kThemeIndexIntervalMs = 500; // 업종 지수 일봉 조회 후 대기
constexpr int kThemeRestIntervalMs  = 200; // 종목 순위·일봉·수급 조회 후 대기
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
    // institution_filter: true면 외국인+기관 동시 순매수 필터 적용
    ThemeStrategy(std::vector<std::string> sector_codes,
                  int top_n_sectors,
                  double volume_surge_mult,
                  bool institution_filter,
                  int quantity,
                  int market_close_exit_hhmm)
        : sector_codes_(std::move(sector_codes)),
          top_n_sectors_(top_n_sectors),
          volume_surge_mult_(volume_surge_mult),
          institution_filter_(institution_filter),
          quantity_(quantity),
          market_close_exit_hhmm_(market_close_exit_hhmm)
    {}

    const std::string& id() const override;

    std::string describe() const override;

    std::vector<WatchSpec> get_watch_specifications() const override;

    // ── 스크리닝 ──────────────────────────────────────────────────────────
    void on_start() override;

    std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }

    // 호가 이벤트 — 진입/청산
    std::optional<OrderSignal> on_order_book(const OrderBook& order_book) override;

    // 체결 이벤트 — 호가 보완
    std::optional<OrderSignal> on_trade(const TradeData& trade) override;

    void on_stop() override;

private:
    std::optional<OrderSignal> check_entry_exit(symbol::SymbolId symbol_id,
                                                 std::string_view ticker,
                                                 int32_t hhmmss,
                                                 double reference_price);

    std::vector<std::string>          sector_codes_;
    int                               top_n_sectors_;
    double                            volume_surge_mult_;
    bool                              institution_filter_;
    int                               quantity_;
    int                               market_close_exit_hhmm_;

    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> candidates_; // 문자열 — 구독 스펙·로그. 틱 경로는 아래 id 집합만 본다. string_view로 찾는다
    std::unordered_set<symbol::SymbolId> pending_;    // 매수 대기 후보 id
    std::unordered_set<symbol::SymbolId> buy_sent_;
    std::unordered_set<symbol::SymbolId> sell_sent_;
};
