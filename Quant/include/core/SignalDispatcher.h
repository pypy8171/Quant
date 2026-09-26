#pragma once
// 신호 디스패처 — 전략·보호 주문 표·시스템(강제청산·한도 정리)이 만든 OrderSignal에 순번을 찍어 주문 큐로 보내기 전에
//  거른다: 비활성 전략의 신규 매수, 청산 관리 보유 종목의 신규, 수동 매도 정지. 교체 진입은 여기 있지 않다 —
//  주문 쪽 risk::DisplacementDesk가 한다(읽고-고쳐-쓰기 한 덩어리라 단일 시퀀서에 둔다, D-114 갈래 B).
//  Engine의 strategy_thread만 부른다 — pipeline_.requests 단일 생산자라 순번·차단 로그 집합에 락이 없다.
//  큐·ZMQ·종목 표기·청산 관리 여부는 std::function으로 받아 Engine 없이 시험한다. [why D-063]
#include "core/Types.h"
#include "ipc/LedgerSnapshot.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dispatch
{
// 미체결 매도 수량(0 이상) 조회 — 보유 스냅샷(HeldPos)이 든 종목 id로 묻는다. 문자열 티커는 로그에만 남긴다. [why D-112]
//  계좌 인자는 장부 사본을 읽게 된 뒤로 쓰지 않는다 — 한 판은 한 계좌만 담기 때문이다. 자리는 남겨 둔다:
//  이 함수 모양은 시험이 직접 넘기는 자리라, 계좌가 다시 필요해질 때 부르는 쪽을 안 고치게. [why D-114]
using SellPendingFn = std::function<int(const std::string& account, symbol::SymbolId symbol)>;

// 강제청산 매도 — 보유마다 이미 낸 미체결 매도를 뺀 잔량을 시장가로. 잔량이 남는 한 다음 주기에 다시 만든다.
//  시장가라 price=0 → 명목 백스톱이 우회되지 않게 평단을 ref_price에 stamp한다(현재가가 없는 경로라 평단이 최선).
//  strategy는 "FORCE_LIQ"의 전략 번호(OrderGate::strategy_index_of) — 신호의 strategy_index에 찍는다.
std::vector<OrderSignal> force_liquidation_orders(const std::vector<OrderGate::HeldPos>& held, const SellPendingFn& sell_pending_of,
                                                  strategy_table::StrategyId strategy = strategy_table::kNone);

// 종목당 명목 한도 초과분 정리 — 한도수량(cap_notional/평단)을 넘는 만큼만 시장가 매도. 이미 낸 미체결 매도는
//  곧 줄어들 분량이라 뺀다. cap_notional≤0이면 비어 있다. strategy는 "LIMIT_TRIM"의 전략 번호.
std::vector<OrderSignal> trim_orders(const std::vector<OrderGate::HeldPos>& held, double cap_notional,
                                     const SellPendingFn& sell_pending_of, strategy_table::StrategyId strategy = strategy_table::kNone);

// 신호 로그 한 줄. 취소·정정은 수량이 0이라 side만 찍으면 "BUY 0"으로 나온다 — 무엇을 하는 신호인지 앞에 적는다.
std::string describe(const OrderSignal& signal, const std::string& label);
} // namespace dispatch

class SignalDispatcher
{
public:
    using Clock   = std::chrono::steady_clock;
    using Sink    = std::function<void(const OrderSignal&)>;     // 순번 찍힌 신호의 목적지(주문 큐·ZMQ)
    using LabelFn = std::function<std::string(const std::string&)>; // 로그용 종목 표기
    using GuardFn = std::function<bool(symbol::SymbolId)>;       // 청산 관리가 맡은 보유 종목인가(id로 묻는다)

    // 시스템 신호("FORCE_LIQ"·"LIMIT_TRIM")의 전략 번호. 주문 쪽 표를 고쳐 받는 번호라 여기서 받지 않고
    //  Engine이 스레드를 띄우기 전에 받아 둔 것을 넘겨받는다 — 디스패처는 전략 스레드에서 지어진다. [why D-114]
    struct SystemIds
    {
        strategy_table::StrategyId force_liquidation = strategy_table::kNone;
        strategy_table::StrategyId limit_trim        = strategy_table::kNone;
    };

    // now는 강제청산 스로틀의 기준 시각. 한도 정리는 now+20초 뒤 한 번(set_trim_at으로 바꾼다).
    //  ledger는 전략 쪽이 보는 장부 사본이다. 보유·미체결·여력은 전부 여기서 읽는다 — 주문 쪽 장부를
    //  직접 부르면 단계 4에서 프로세스가 갈릴 때 그 자리가 전부 막힌다. gate는 종목 표를 읽기만 한다. [why D-114]
    SignalDispatcher(OrderGate& gate, const ipc::LedgerSnapshot& ledger, Sink sink, Clock::time_point now,
                     SystemIds system_ids);

    void set_label(LabelFn label)
    {
        label_ = std::move(label);
    }

    void set_exit_managed_check(GuardFn exit_managed_check)
    {
        exit_managed_check_ = std::move(exit_managed_check);
    }

    void set_liquidation_interval(std::chrono::milliseconds milliseconds)
    {
        liquidation_interval_ = milliseconds;
    }

    void set_trim_at(Clock::time_point at)
    {
        trim_at_ = at;
    }

    // 전략이 낸 신호. 비활성 전략의 BUY NEW는 버리고(청산·취소·정정은 통과 — entry_halt와 같은 규약), 청산 관리
    //  보유 종목의 NEW는 청산 관리 전략(exit_manager, ITB_ 계열)이 아니면 종목당 한 번 로그하고 버린다.
    //  exit_manager는 Engine이 전략 등록 때 이름으로 한 번 정한다 — 신호마다 접두를 비교하지 않는다.
    void from_strategy(bool active, bool exit_manager, const OrderSignal& signal);

    // 보호 주문 표·강제청산·한도 정리 등 전략 밖에서 온 신호. 종목 번호를 찍어 emit한다.
    //  값으로 받는다 — 순번을 찍어 내보내는 sink라, 임시로 온 신호는 이동으로 들어온다.
    void submit(OrderSignal signal);

    std::vector<OrderGate::HeldPos> scan_sleeve_positions() const; // 바스켓 소유 종목을 뺀 보유분 [why D-109]

    // 강제청산 재발주 — liquidation_interval_마다 보유 전량(미체결 매도 제외) 시장가 매도. force_liquidate 동안 매 루프 부른다.
    void force_liquidate(Clock::time_point now);

    // 종목당 명목 한도 초과분 정리 — trim_at 이후 한 번만. 매 루프 부른다.
    void trim_excess_once(Clock::time_point now);

    uint64_t sequence() const  // 마지막으로 부여한 순번(0=아직 없음)
    {
        return sequence_;
    }

    bool     trim_done() const
    {
        return trim_done_;
    }

private:
    // 순번 stamp → 로그 → 싱크. 큐에 넣는 유일한 길. 값으로 받아 그 자리에서 순번을 찍는다(sink).
    void        emit(OrderSignal signal);
    std::string label(const std::string& ticker) const
    {
        return label_ ? label_(ticker) : ticker;
    }

    std::string label(symbol::SymbolId symbol) const
    {
        return label(gate_.ledger().symbols().name(symbol).string());
    }

    // 신호의 종목 id — 전략 경로는 이미 찍혀 온다. 안 찍힌 신호(테스트·운영단말)는 모르는 종목이면 kNone이고,
    //  번호를 주는 것은 주문 쪽이다(주문 스레드가 받는 자리에서 등록한다). [why D-114]
    symbol::SymbolId symbol_of(const OrderSignal& signal) const;

    // 종목당 한 번 로그 — id 인덱스 비트. 처음이면 true. 배열은 종목 테이블 용량으로 한 번 잡는다.
    static bool mark_once(std::vector<bool>& flags, symbol::SymbolId symbol);

    OrderGate&                 gate_;
    const ipc::LedgerSnapshot& ledger_;
    Sink                       sink_;
    LabelFn                    label_;
    GuardFn                    exit_managed_check_;

    // 시스템 신호 전략 번호 — 생성자에서 한 번 받는다.
    strategy_table::StrategyId force_liquidation_index_;
    strategy_table::StrategyId limit_trim_index_;

    // [inv] 단조 증가, strategy_thread 전용. 게이트 거부·큐 드롭·접수·체결 행이 전부 이 번호를 물고 간다. [why D-038]
    uint64_t sequence_ = 0;

    std::vector<bool> guard_logged_;     // 청산 관리 차단 로그는 종목당 한 번(id 인덱스)
    std::vector<bool> sell_halt_logged_; // 수동 매도 정지 차단 로그도 종목당 한 번, 정지가 풀리면 비운다 [why D-095]
    bool              sell_halt_logged_any_ = false; // 위 배열에 켜진 비트가 있는가 — 정지 해제 때만 전부 끈다

    // 사본 한 판을 통째로 받아 오는 자리(강제청산·한도 정리). 이 스레드 전용이라 재사용해 매번 새로 잡지 않는다.
    mutable std::vector<symbol::SymbolId> snapshot_ids_;
    mutable std::vector<ipc::LedgerRow>   snapshot_rows_;

    Clock::time_point         last_liquidation_;
    std::chrono::milliseconds liquidation_interval_{2000}; // deduplicate 윈도우(1s)보다 길어야 재발주가 통과한다
    Clock::time_point         trim_at_;
    bool                      trim_done_ = false;
};
