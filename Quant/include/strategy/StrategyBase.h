#pragma once
#include "core/Types.h"
#include <optional>
#include <string>
#include <vector>

class KisClient;

// ─────────────────────────────────────────────────────────────────────────────
// StrategyBase  —  모든 전략이 구현해야 하는 인터페이스
// ─────────────────────────────────────────────────────────────────────────────
class StrategyBase
{
public:
    virtual ~StrategyBase() = default;

    virtual std::string id() const = 0;
    virtual std::string describe() const = 0;

    // 일봉 시세 이벤트
    virtual std::optional<OrderSignal> on_data(const MarketData&) = 0;

    // 호가 이벤트 (국내 전용 — H0STASP0)
    virtual std::optional<OrderSignal> on_order_book(const OrderBook&)
    {
        return std::nullopt;
    }

    // 체결 이벤트 (미국 — HDFSCNT0, 국내 — H0STCNT0)
    virtual std::optional<OrderSignal> on_trade(const TradeData&)
    {
        return std::nullopt;
    }

    // Engine이 on_start() 직전에 호출
    virtual void on_start()
    {
    }
    virtual void on_stop()
    {
    }

    // Engine이 WS 구독 목록 수집에 사용 — on_start() 이후 유효
    virtual std::vector<WatchSpec> get_watch_specs() const
    {
        return {};
    }

    // 이 전략이 활성화될 시장 국면. 기본값=전 국면(기존 전략 무변경 호환).
    // config "active_regimes"로 set_active_regimes() 오버라이드. RegimeController 게이트가 참조.
    std::vector<Regime> active_regimes() const { return active_regimes_; }
    void set_active_regimes(std::vector<Regime> r) { active_regimes_ = std::move(r); }

    // Engine이 장시작 국면 판정 후 설정 (현재 국면 ∈ active_regimes 이면 true).
    // 진입 분기에서 is_active() 체크 → 비활성 국면 진입 차단(청산은 무관). 기본 true(국면 모를 때 통과).
    void set_active(bool a) { active_ = a; }
    bool is_active() const { return active_; }

    // Engine이 unique_ptr<KisClient>로 수명을 관리한다.
    // set_kis()는 Engine::start() 내부에서만 호출되며, 전략 소멸 전에 Engine이 먼저 종료된다.
    void set_kis(KisClient* k)
    {
        kis_ = k;
    }

protected:
    KisClient* kis_ = nullptr; // non-owning; lifetime guaranteed by Engine
    bool active_ = true;       // 국면 게이트(Engine이 설정). 기본 true=통과
    std::vector<Regime> active_regimes_ = {Regime::BULL, Regime::NEUTRAL, Regime::BEAR};
};
