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

    // 이 전략이 on_data(일봉)를 실제로 쓰는가. 기본 false — 대부분의 전략은 호가·체결
    //  이벤트로만 동작하고 on_data는 인터페이스 충족용 no-op이다. Engine은 등록된 전략 중
    //  하나라도 true일 때만 일봉을 폴링한다. 아무도 안 쓰면 종목 수만큼의 차트 TR 호출이
    //  매 사이클 그대로 버려지고, 그 호출량이 초당 한도를 밀어올려 다른 조회까지 500으로 떨어뜨린다.
    virtual bool wants_daily_bars() const { return false; }

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

    // 계좌 조회(잔고·매도가능수량·총평가금) 전용 클라이언트 주입.
    //  set_kis()가 받는 것은 시세 클라이언트다. 주문을 모의로 내면서 시세만 실전 도메인으로
    //  받는 구성에서는 그 시세 클라이언트에 계좌번호가 없어 has_account()가 false가 되고,
    //  잔고를 쓰는 코드가 전부 "알 수 없음"으로 떨어진다(매도가능=0 → 청산·익절 미발주).
    //  계좌를 가진 주문 클라이언트를 따로 받아 두 관심사를 분리한다.
    //  미주입이면 account_kis()가 kis_로 되돌아가 단일 클라이언트 구성의 기존 동작을 유지한다.
    void set_account_kis(KisClient* k)
    {
        account_kis_ = k;
    }

    // OrderGate 확정 포지션 접근자 주입 — WS/REST 양모드 공용 원장 진실원천.
    // Engine::start()에서 order_gate_.position(account,ticker)로 바인딩. 미주입 시 0 반환.
    // (체결콜백 부재 rest 모드에서도 잔고 대조로 원장이 최신이라 이 값이 신뢰 가능)
    void set_position_provider(std::function<int(const std::string&, const std::string&)> f)
    {
        position_provider_ = std::move(f);
    }

    int confirmed_position(const std::string& account, const std::string& ticker) const
    {
        return position_provider_ ? position_provider_(account, ticker) : 0;
    }

    // 신규매수 차단(OrderGate::is_entry_halted) 접근자 주입 — Engine이 바인딩한다.
    //  게이트는 라우터 앞에서 매수를 거부하지만 전략은 그걸 모르고 같은 계획을 유지하므로,
    //  차단이 풀려도 분할 매수를 다시 깔지 않았다(09-10 결함 C). 전략이 계획 단계에서 읽게 한다.
    //  미주입이면 false = 차단 없음.
    void set_entry_halt_provider(std::function<bool()> f)
    {
        entry_halt_provider_ = std::move(f);
    }

    bool entry_halted() const
    {
        return entry_halt_provider_ ? entry_halt_provider_() : false;
    }

    // 매도가능수량·평단 접근자 주입 — OrderGate 원장 기준(잔고 대조가 맞춘 주문가능분에서 이 세션의
    //  미체결 매도를 뺀 값). 전략 스레드가 잔고 REST를 동기로 부르면 한 종목의 조회(13~16초)가
    //  다른 전략 전부를 막고 체결 큐가 넘친다(09-11 15:15~15:22). [why D-055]
    //  미주입이면 nullopt — 호출측이 기존 동기 조회로 되돌아간다.
    struct SellableInfo
    {
        int    sellable = 0;   // 주
        double avg_px   = 0.0; // 원, 0=원장에 없음
    };

    void set_sellable_provider(std::function<SellableInfo(const std::string&, const std::string&)> f)
    {
        sellable_provider_ = std::move(f);
    }

    std::optional<SellableInfo> ledger_sellable(const std::string& account, const std::string& ticker) const
    {
        if (!sellable_provider_)
        {
            return std::nullopt;
        }

        return sellable_provider_(account, ticker);
    }

protected:
    // 잔고·매도가능수량·총평가금을 조회할 클라이언트. 주입됐으면 그쪽, 아니면 시세 클라이언트.
    //  (호출측은 지금까지처럼 has_account()로 한 번 더 확인한다.)
    KisClient* account_kis() const
    {
        return account_kis_ ? account_kis_ : kis_;
    }

    KisClient* kis_ = nullptr;         // non-owning; lifetime guaranteed by Engine
    KisClient* account_kis_ = nullptr; // non-owning; 계좌 조회용(미주입 시 kis_ 사용)
    std::function<int(const std::string&, const std::string&)> position_provider_; // 결제완료 확정 포지션(D2=결제일 T+2)
    std::function<bool()> entry_halt_provider_; // 신규매수 차단 여부(OrderGate). 미주입=false
    std::function<SellableInfo(const std::string&, const std::string&)> sellable_provider_; // 원장 매도가능·평단
    std::atomic<bool> active_{true};   // 국면 게이트(Engine이 설정). 기본 true=통과 (G-1)
    std::vector<Regime> active_regimes_ = {Regime::BULL, Regime::NEUTRAL, Regime::BEAR};
};
