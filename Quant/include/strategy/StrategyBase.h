#pragma once
#include "core/Types.h"
#include <atomic>
#include <functional>
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

    // 다건 발주 (취소/정정 포함) — 시장조성(MM) 등 틱당 여러 주문을 내는 전략 전용.
    // 기본 no-op → 기존 전략 무영향. Engine이 on_order_book 직후 호출하며, out에 채운
    // 신호를 order_queue_로 push한다. CANCEL/REPLACE는 side가 NONE이어도 통과된다.
    virtual void on_order_book_batch(const OrderBook&, std::vector<OrderSignal>& /*out*/)
    {
    }

    // 체결 이벤트 (미국 — HDFSCNT0, 국내 — H0STCNT0)
    virtual std::optional<OrderSignal> on_trade(const TradeData&)
    {
        return std::nullopt;
    }

    // 다건 발주 (체결틱/현재가 하트비트 구동) — 3분봉 이격도 분할매매(지정가 예약) 등
    // 틱마다 CANCEL+NEW 여러 주문을 내는 전략 전용. 기본 no-op → 기존 전략 무영향.
    // rest_price_feed 모드에서 DataThread가 종목별 현재가를 TradeData로 매 사이클 주입하므로
    // (WS 없이도) 이 훅이 하트비트로 동작한다. Engine이 on_trade 직후 호출해 out을 order_queue_로 push.
    virtual void on_trade_batch(const TradeData&, std::vector<OrderSignal>& /*out*/)
    {
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
    void set_active(bool a) { active_.store(a, std::memory_order_relaxed); }
    bool is_active() const { return active_.load(std::memory_order_relaxed); }

    // Engine이 unique_ptr<KisClient>로 수명을 관리한다.
    // set_kis()는 Engine::start() 내부에서만 호출되며, 전략 소멸 전에 Engine이 먼저 종료된다.
    void set_kis(KisClient* k)
    {
        kis_ = k;
    }

    // OrderGate 확정 포지션 접근자 주입 — WS/REST 양모드 공용 원장 진실원천.
    // Engine::start()에서 order_gate_.position(account,ticker)로 바인딩. 미주입 시 0 반환.
    // (체결콜백 부재 rest 모드에서도 리컨사일로 원장이 최신이라 이 값이 신뢰 가능)
    void set_position_provider(std::function<int(const std::string&, const std::string&)> f)
    {
        position_provider_ = std::move(f);
    }
    int confirmed_position(const std::string& account, const std::string& ticker) const
    {
        return position_provider_ ? position_provider_(account, ticker) : 0;
    }

protected:
    KisClient* kis_ = nullptr; // non-owning; lifetime guaranteed by Engine
    std::function<int(const std::string&, const std::string&)> position_provider_; // 확정 포지션(D2)
    std::atomic<bool> active_{true};   // 국면 게이트(Engine이 설정). 기본 true=통과 (G-1)
    std::vector<Regime> active_regimes_ = {Regime::BULL, Regime::NEUTRAL, Regime::BEAR};
};
