#pragma once
#include "core/StrategyTable.h"
#include "core/Types.h"
#include "risk/ProtectiveRule.h"
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class KisClient;

namespace prefetch
{
class Pool;
}

// 문자열 집합·맵을 std::string_view로 찾기 위한 해시 — 조회마다 std::string을 만들지 않는다.
//  std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> 로 쓴다.
struct TransparentStringHash
{
    using is_transparent = void;

    size_t operator()(std::string_view text) const
    {
        return std::hash<std::string_view>{}(text);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// StrategyBase  —  모든 전략이 구현해야 하는 인터페이스
// ─────────────────────────────────────────────────────────────────────────────
class StrategyBase
{
public:
    virtual ~StrategyBase() = default;

    // 전략 이름 — 생성 시 한 번 만들어 둔 문자열의 참조. 신호 봉투가 신호마다 받아 가므로
    //  호출마다 결합하지 않는다(D-071 원칙 6). [inv] 반환 참조는 전략 객체가 살아 있는 동안 유효.
    virtual const std::string& id() const = 0;
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
    virtual std::vector<WatchSpec> get_watch_specifications() const
    {
        return {};
    }

    // 이 전략이 활성화될 시장 국면. 기본값=전 국면(기존 전략 무변경 호환).
    // config "active_regimes"로 set_active_regimes() 오버라이드. Engine::apply_regime_selection 폴백이 참조.
    const std::vector<Regime>& active_regimes() const { return active_regimes_; }
    void set_active_regimes(std::vector<Regime> active_regimes) { active_regimes_ = std::move(active_regimes); }

    // 신규 진입 게이트 두 축. 진입 분기에서 is_active()를 보고 막는다(청산은 무관). 둘 다 기본 true.
    //  - active_: Engine이 국면 판정 뒤 설정(현재 국면 ∈ active_regimes). 재스캔 뒤 재적용된다.
    //  - in_universe_: 재스캔 결과에 이 종목이 있는지. 빠지면 그 주기부터 신규매수를 막고, 돌아오면 푼다.
    //    국면 재적용이 active_만 다시 쓰므로 축을 따로 둔다 [why D-077].
    void set_active(bool active) { active_.store(active, std::memory_order_relaxed); }
    void set_in_universe(bool in_universe) { in_universe_.store(in_universe, std::memory_order_relaxed); }
    bool in_universe() const { return in_universe_.load(std::memory_order_relaxed); }
    bool is_active() const
    {
        return active_.load(std::memory_order_relaxed) && in_universe_.load(std::memory_order_relaxed);
    }

    // 이 전략 객체를 돌리는 샤드(스레드) 번호. Engine이 등록할 때 정하고(라운드로빈) 그 샤드 스레드만 on_trade 등을
    //  부른다 — 종목이 여러 개여도 객체는 스레드 하나만 만진다. 스레드 시작 전·전략 목록 락 하에서만 바꾼다. [why D-110]
    uint32_t shard_index() const { return shard_index_; }
    void     set_shard_index(uint32_t shard_index) { shard_index_ = shard_index; }

    // 전략 번호(OrderGate::strategy_index_of(id())) — Engine이 등록 때 한 번 정한다. 신호 봉투가 이 번호를 싣고,
    //  게이트의 서브원장·중복 신호 키가 문자열 id 대신 이 번호를 쓴다. [why D-112]
    strategy_table::StrategyId strategy_index() const { return strategy_index_; }
    void                       set_strategy_index(strategy_table::StrategyId index) { strategy_index_ = index; }

    // 청산 관리 전략(ITB_ 계열)인가 — 청산 관리 보유 종목의 신규 차단을 면제받는다. Engine이 등록 때 id로 한 번 정한다.
    bool is_exit_manager() const { return exit_manager_; }
    void set_exit_manager(bool exit_manager) { exit_manager_ = exit_manager; }

    // Engine이 unique_ptr<KisClient>로 수명을 관리한다.
    // set_kis()는 Engine::start() 내부에서만 호출되며, 전략 소멸 전에 Engine이 먼저 종료된다.
    void set_kis(KisClient* kis)
    {
        kis_ = kis;
    }

    // 무거운 REST를 미리 당기는 공용 프리페치 풀. Engine이 소유하며 set_kis()와 같은 자리에서
    //  주입한다 — 전략마다 스레드를 띄우지 않기 위한 것이다. [why D-071]
    //  [inv] 풀은 Engine이 들고 있고 전략보다 늦게 사라진다(set_kis와 같은 수명 보장).
    void set_prefetch_pool(prefetch::Pool* pool)
    {
        prefetch_pool_ = pool;
    }

    // 계좌 조회(잔고·매도가능수량·총평가금) 전용 클라이언트 주입.
    //  set_kis()가 받는 것은 시세 클라이언트다. 주문을 모의로 내면서 시세만 실전 도메인으로
    //  받는 구성에서는 그 시세 클라이언트에 계좌번호가 없어 has_account()가 false가 되고,
    //  잔고를 쓰는 코드가 전부 "알 수 없음"으로 떨어진다(매도가능=0 → 청산·익절 미발주).
    //  계좌를 가진 주문 클라이언트를 따로 받아 두 관심사를 분리한다.
    //  미주입이면 account_kis()가 kis_로 되돌아가 단일 클라이언트 구성의 기존 동작을 유지한다.
    void set_account_kis(KisClient* account_kis)
    {
        account_kis_ = account_kis;
    }

    // OrderGate 확정 포지션 접근자 주입 — WS/REST 양모드 공용 원장 진실원천.
    // Engine::start()에서 order_gate_.position(account,ticker)로 바인딩. 미주입 시 0 반환.
    // (체결콜백 부재 rest 모드에서도 잔고 대조로 원장이 최신이라 이 값이 신뢰 가능)
    void set_position_provider(std::function<int(const std::string&, const std::string&)> provider)
    {
        position_provider_ = std::move(provider);
    }

    int confirmed_position(const std::string& account, const std::string& ticker) const
    {
        return position_provider_ ? position_provider_(account, ticker) : 0;
    }

    // 정수 id 버전 — 틱마다 묻는 전략(ITB 청산 대기·DevScale 장 마감 블록)이 쓴다. 문자열 해시가 없다.
    //  id가 kNone(배선 전)이거나 id 제공자가 없으면 문자열 버전으로 돌아간다. [why D-105]
    void set_position_provider_by_id(std::function<int(const std::string&, symbol::SymbolId)> provider)
    {
        position_provider_by_id_ = std::move(provider);
    }

    int confirmed_position(const std::string& account, symbol::SymbolId symbol, const std::string& ticker) const;

    // 신규매수 차단(OrderGate::is_entry_halted) 접근자 주입 — Engine이 바인딩한다.
    //  게이트는 라우터 앞에서 매수를 거부하지만 전략은 그걸 모르고 같은 계획을 유지하므로,
    //  차단이 풀려도 분할 매수를 다시 깔지 않았다(09-10 결함 C). 전략이 계획 단계에서 읽게 한다.
    //  미주입이면 false = 차단 없음.
    void set_entry_halt_provider(std::function<bool()> provider)
    {
        entry_halt_provider_ = std::move(provider);
    }

    bool entry_halted() const
    {
        return entry_halt_provider_ ? entry_halt_provider_() : false;
    }

    // 매수 명목 비율(OrderGate::entry_scale) 접근자 주입 — Engine이 바인딩한다. 미주입이면 1.0.
    void set_entry_scale_provider(std::function<double()> provider)
    {
        entry_scale_provider_ = std::move(provider);
    }

    double entry_scale() const
    {
        return entry_scale_provider_ ? entry_scale_provider_() : 1.0;
    }

    // 매도가능수량·평단 접근자 주입 — OrderGate 원장 기준(잔고 대조가 맞춘 주문가능분에서 이 세션의
    //  미체결 매도를 뺀 값). 전략 스레드가 잔고 REST를 동기로 부르면 한 종목의 조회(13~16초)가
    //  다른 전략 전부를 막고 체결 큐가 넘친다(09-11 15:15~15:22). [why D-055]
    //  미주입이면 nullopt — 호출측이 기존 동기 조회로 되돌아간다.
    struct SellableInfo
    {
        int    sellable = 0;   // 주
        double average_price   = 0.0; // 원, 0=원장에 없음
    };

    void set_sellable_provider(std::function<SellableInfo(const std::string&, const std::string&)> provider)
    {
        sellable_provider_ = std::move(provider);
    }

    std::optional<SellableInfo> ledger_sellable(const std::string& account, const std::string& ticker) const;

    // 보호 주문 표 주입 — Engine이 전략 등록 때 넣는다. 미주입이면 아래 arm_protective가 아무것도 하지 않는다. [why D-114]
    //  수명은 Engine이 가진다 — 표는 Engine 멤버고 전략보다 늦게 죽는다. [inv]
    void set_protective_registry(risk::ProtectiveOrderRegistry* registry)
    {
        protective_registry_ = registry;
    }

    // 종목 문자열 → 정수 id. Engine이 SymbolTable::intern을 넣는다 — 전략은 기동·설정 때 한 번 받아 두고
    //  틱에서는 trade.symbol_id과 정수로만 비교한다(원칙 6, D-071). 미주입이면 kNone — 아래 same_symbol이 문자열로 되돌아간다.
    using SymbolResolver = std::function<symbol::SymbolId(std::string_view)>;

    void set_symbol_resolver(SymbolResolver symbol_resolver)
    {
        symbol_resolver_ = std::move(symbol_resolver);
    }

protected:
    symbol::SymbolId symbol_of(std::string_view ticker) const
    {
        return symbol_resolver_ ? symbol_resolver_(ticker) : symbol::kNone;
    }

    // 보호 주문 등록 — "이 종목은 평단 -stop_loss_percent면 판다"를 주문 쪽 표에 미리 올려둔다.
    //  전략이 멈춰도 그 표만 보고 청산이 나간다. 조건이 없으면(전부 0) 해제로 친다. [why D-114]
    void arm_protective(const std::string& account, const std::string& ticker, symbol::SymbolId symbol,
                        double stop_loss_percent, double trail_arm_percent, double trail_percent);

    void disarm_protective(const std::string& account, symbol::SymbolId symbol);

    // 표가 이 종목의 청산을 맡았는가. true면 전략은 자기 손절·트레일 판정을 건너뛰다.
    bool protective_owns(const std::string& account, symbol::SymbolId symbol) const
    {
        return protective_registry_ != nullptr && protective_registry_->owns(account, symbol);
    }

    // 표가 방금 청산했는가(한 번의 발사를 한 번만). 전략은 이것을 보고 미체결 매수를 거두고 재진입 우도를 건다.
    bool consume_protective_fire(const std::string& account, symbol::SymbolId symbol)
    {
        return protective_registry_ != nullptr && protective_registry_->consume_fired(account, symbol);
    }

    // 틱이 내 종목인가. 둘 다 id가 있으면 정수 비교, 한쪽이라도 kNone(주입 전·시험)이면 문자열.
    static bool same_symbol(symbol::SymbolId symbol_id_a, std::string_view a_ticker, symbol::SymbolId symbol_id_b, std::string_view b_ticker);

    // 잔고·매도가능수량·총평가금을 조회할 클라이언트. 주입됐으면 그쪽, 아니면 시세 클라이언트.
    //  (호출측은 지금까지처럼 has_account()로 한 번 더 확인한다.)
    KisClient* account_kis() const
    {
        return account_kis_ ? account_kis_ : kis_;
    }

    KisClient* kis_ = nullptr;         // non-owning; lifetime guaranteed by Engine
    prefetch::Pool* prefetch_pool_ = nullptr; // non-owning; Engine 소유(미주입이면 프리페치 없음)
    KisClient* account_kis_ = nullptr; // non-owning; 계좌 조회용(미주입 시 kis_ 사용)
    std::function<int(const std::string&, const std::string&)> position_provider_; // 결제완료 확정 포지션(D2=결제일 T+2)
    std::function<int(const std::string&, symbol::SymbolId)> position_provider_by_id_; // 같은 원장, 종목 정수 id로 [why D-105]
    std::function<bool()> entry_halt_provider_; // 신규매수 차단 여부(OrderGate). 미주입=false
    std::function<double()> entry_scale_provider_; // 매수 명목 비율(OrderGate). 미주입=1.0
    std::function<SellableInfo(const std::string&, const std::string&)> sellable_provider_; // 원장 매도가능·평단
    risk::ProtectiveOrderRegistry* protective_registry_ = nullptr; // non-owning; 보호 주문 표(Engine 소유). 미주입=표 없음
    SymbolResolver symbol_resolver_; // 종목 문자열 → id(SymbolTable::intern). 미주입=kNone
    std::atomic<bool> active_{true};      // 국면 게이트(Engine이 설정). 기본 true=통과 (G-1)
    std::atomic<bool> in_universe_{true}; // 유니버스 재스캔 게이트(Engine이 설정). 미등록 전략은 늘 true
    uint32_t          shard_index_ = 0;   // 소유 샤드. Engine::setup_shards·register_strategy_runtime가 쓴다
    strategy_table::StrategyId strategy_index_ = strategy_table::kNone; // 전략 번호. Engine이 등록 때 정한다
    bool                       exit_manager_   = false;                 // ITB_ 계열. Engine이 등록 때 정한다
    std::vector<Regime> active_regimes_ = {Regime::BULL, Regime::NEUTRAL, Regime::BEAR};
};
