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
    // market_close_exit_hhmm : 청산 시각 KST(기본 1520). US는 자정을 넘기는 창이라 장 시작(22:30)부터 잰
    //                          순서로 비교한다 — 0330은 같은 밤 22:30 이후가 아니라 새벽 03:30에 청산한다
    ValueContraryStrategy(Market market, std::string exchange, double pbr_max, int quantity, int market_close_exit_hhmm)
        : market_(market), exchange_(std::move(exchange)), pbr_max_(pbr_max), quantity_(quantity),
          market_close_exit_hhmm_(market_close_exit_hhmm)
    {
        id_ = std::string("VALUE_CONTRARY_") + (market_ == Market::KR ? "KR" : "US");
    }

    const std::string& id() const override { return id_; }

    // 그 시장의 정규장 안인가(KST hhmm). KR 09:00~15:30, US 22:30~05:00(서머타임 기준). 로더가 청산 시각을 이것으로
    //  걸러낸다 — 틱 처리가 세션 밖에서 바로 돌아가므로 세션 밖 청산 시각에는 청산 분기가 닿지 않는다.
    static bool in_session(Market market, int hhmm);

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

    // 장 시작부터의 순서로 편 hhmm. US는 자정 뒤(05:00 전)를 +2400 해 22:30 창 뒤로 놓는다. KR은 그대로.
    int session_order(int hhmm) const;

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
