#pragma once
#include "api/KisClient.h"
#include "core/MarketSession.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"
#include <chrono>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// 유니버스 종목마다 KIS REST를 연속 호출하므로 호출 사이에 짧게 쉰다
// (초당 호출 한도(EGW00201) 회피용 호출 간격 조절 간격).
namespace
{
constexpr int kValueContraryRestIntervalMs = 200;
}

// ─────────────────────────────────────────────────────────────────────────────
// ValueContraryStrategy  —  저PBR 3일 연속 하락 반전 매수
//
//  [on_start() — 장 시작 전]
//    KR: KIS 시가총액 순위 API → PBR ≤ pbr_max 전 종목 조회 (동적 Universe)
//    US: 내장 S&P500 리스트 → PBR/PER 필터
//    → 3일 연속 하락 체크 → candidates_ 확정
//
//  [진입 — on_order_book()(KR)·on_trade()(KR·US)]
//    장 시작 후 첫 이벤트에서 시장가 매수 (4일차 시가 효과)
//
//  [청산 — on_order_book() / on_trade()]
//    market_close_exit_hhmm(KST) 도달 시 시장가 청산
// ─────────────────────────────────────────────────────────────────────────────
class ValueContraryStrategy : public StrategyBase
{
public:
    // market   : Market::KR 또는 Market::US
    // exchange : US일 때 "NAS" / "NYS" (KR은 무시)
    // pbr_max  : PBR 상한 (0이면 PBR 조건 미적용)
    // market_close_exit_hhmm : 청산 시각 KST (KR=1520, US=0330)
    ValueContraryStrategy(Market market, std::string exchange, double pbr_max, int quantity, int market_close_exit_hhmm)
        : market_(market), exchange_(std::move(exchange)), pbr_max_(pbr_max), quantity_(quantity),
          market_close_exit_hhmm_(market_close_exit_hhmm)
    {
        id_ = std::string("VALUE_CONTRARY_") + (market_ == Market::KR ? "KR" : "US");
    }

    const std::string& id() const override { return id_; }

    std::string describe() const override;

    // ── 스크리닝 ──────────────────────────────────────────────────────────
    void on_start() override;

    // ── WS 구독 스펙 — on_start() 이후 candidates_ 기준 ──────────────────
    std::vector<WatchSpec> get_watch_specifications() const override;

    // ── 일봉 — 이 전략은 이벤트 드리븐으로만 동작 ────────────────────────
    std::optional<OrderSignal> on_data(const MarketData&) override
    {
        return std::nullopt;
    }

    // ── 호가 이벤트 (국내 전용) ───────────────────────────────────────────
    std::optional<OrderSignal> on_order_book(const OrderBook& order_book) override;

    // ── 체결 이벤트 (미국 + 국내) ─────────────────────────────────────────
    std::optional<OrderSignal> on_trade(const TradeData& trade) override;

    void on_stop() override;

private:
    // 진입·청산 공통 로직
    std::optional<OrderSignal> check_entry_exit(symbol::SymbolId symbol_id, std::string_view ticker, int32_t hhmmss,
                                                double reference_price);

    // 장 세션 내 여부 (KST 기준)
    bool is_in_session(int hhmm) const;

    Market market_;
    std::string exchange_;
    std::string id_; // 전략 이름, 생성자에서 한 번
    double pbr_max_;
    int quantity_;
    int market_close_exit_hhmm_;

    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> candidates_; // 문자열 — 구독 스펙·로그. 틱 경로는 아래 id 집합만 본다. string_view로 찾는다
    std::unordered_set<symbol::SymbolId> pending_;    // 매수 대기 후보 id
    std::unordered_set<symbol::SymbolId> buy_sent_;
    std::unordered_set<symbol::SymbolId> sell_sent_;
};
