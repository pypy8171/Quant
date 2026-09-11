#pragma once
// 신호 디스패처 — 전략·운영단말·시스템(강제청산·한도 정리)이 만든 OrderSignal에 순번을 찍어 주문 큐로 보내기 전에
//  거른다: 비활성 전략의 신규 매수, 청산 관리 보유 종목의 신규, 슬롯이 찬 상태의 교체 진입(최약체 매도 뒤 매수 보류).
//  Engine의 strategy_thread만 부른다 — order_queue_ 단일 생산자라 순번·보류 목록·차단 로그 집합에 락이 없다.
//  큐·ZMQ·종목 표기·청산 관리 여부는 std::function으로 받아 Engine 없이 시험한다. [why D-063]
#include "core/Types.h"
#include "risk/OrderGate.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace dispatch
{
using ReservedFn = std::function<int(const std::string& account, const std::string& ticker)>;

// 강제청산 매도 — 보유마다 이미 낸 미체결 매도를 뺀 잔량을 시장가로. 잔량이 남는 한 다음 주기에 다시 만든다.
//  시장가라 price=0 → 명목 백스톱이 우회되지 않게 평단을 ref_price에 stamp한다(현재가가 없는 경로라 평단이 최선).
std::vector<OrderSignal> force_liq_orders(const std::vector<OrderGate::HeldPos>& held, const ReservedFn& reserved);

// 종목당 명목 한도 초과분 정리 — 한도수량(cap_notional/평단)을 넘는 만큼만 시장가 매도. 이미 낸 미체결 매도는
//  곧 줄어들 분량이라 뺀다. cap_notional≤0이면 비어 있다.
std::vector<OrderSignal> trim_orders(const std::vector<OrderGate::HeldPos>& held, double cap_notional,
                                     const ReservedFn& reserved);

// 신호 로그 한 줄. 취소·정정은 수량이 0이라 side만 찍으면 "BUY 0"으로 나온다 — 무엇을 하는 신호인지 앞에 적는다.
std::string describe(const OrderSignal& sig, const std::string& label);
} // namespace dispatch

class SignalDispatcher
{
public:
    using Clock   = std::chrono::steady_clock;
    using Sink    = std::function<void(const OrderSignal&)>;     // 순번 찍힌 신호의 목적지(주문 큐·ZMQ)
    using LabelFn = std::function<std::string(const std::string&)>; // 로그용 종목 표기
    using GuardFn = std::function<bool(const std::string&)>;     // 청산 관리가 맡은 보유 티커인가

    // now는 강제청산 스로틀의 기준 시각. 한도 정리는 now+20초 뒤 한 번(set_trim_at으로 바꾼다).
    SignalDispatcher(OrderGate& gate, Sink sink, Clock::time_point now);

    void set_label(LabelFn f) { label_ = std::move(f); }
    void set_guardian(GuardFn f) { guardian_ = std::move(f); }
    void set_liq_interval(std::chrono::milliseconds ms) { liq_interval_ = ms; }
    void set_trim_at(Clock::time_point at) { trim_at_ = at; }

    // 전략이 낸 신호. 비활성 전략의 BUY NEW는 버리고(청산·취소·정정은 통과 — entry_halt와 같은 규약), 청산 관리
    //  보유 종목의 NEW는 ITB_ 전략이 아니면 종목당 한 번 로그하고 버린다.
    void from_strategy(bool active, const std::string& strategy_id, const OrderSignal& sig);

    // 운영단말·기동 점검·강제청산 등 전략 밖에서 온 신호. 교체 진입 판단을 거쳐 emit한다.
    void submit(const OrderSignal& sig);

    // 교체 매도가 체결돼 자리가 났으면 보류 매수를 낸다. 예약 시한이 지나면 버린다. 루프 머리마다 부른다.
    void flush_held(Clock::time_point now);

    // 강제청산 재발주 — liq_interval마다 보유 전량(미체결 매도 제외) 시장가 매도. force_liquidate 동안 매 루프 부른다.
    void force_liquidate(Clock::time_point now);

    // 종목당 명목 한도 초과분 정리 — trim_at 이후 한 번만. 매 루프 부른다.
    void trim_excess_once(Clock::time_point now);

    uint64_t           seq() const { return seq_; }           // 마지막으로 부여한 순번(0=아직 없음)
    std::size_t        held_count() const { return held_.size(); }
    const std::string& held_ticker() const { return held_ticker_; }
    bool               trim_done() const { return trim_done_; }

private:
    // 순번 stamp → 로그 → 싱크. 큐에 넣는 유일한 길.
    void        emit(const OrderSignal& in);
    std::string label(const std::string& ticker) const { return label_ ? label_(ticker) : ticker; }

    OrderGate& gate_;
    Sink       sink_;
    LabelFn    label_;
    GuardFn    guardian_;

    // [inv] 단조 증가, strategy_thread 전용. 게이트 거부·큐 드롭·접수·체결 행이 전부 이 번호를 물고 간다. [why D-038]
    uint64_t seq_ = 0;

    // 교체 진입 보류 — 최약체 매도를 낸 뒤 수혜 종목의 매수(rung 전부)를 자리가 날 때까지 든다. [why D-019]
    std::vector<OrderSignal> held_;
    std::string              held_ticker_;
    Clock::time_point        held_until_{};

    std::unordered_set<std::string> guard_logged_; // 청산 관리 차단 로그는 종목당 한 번

    Clock::time_point         last_liq_;
    std::chrono::milliseconds liq_interval_{2000}; // dedup 윈도우(1s)보다 길어야 재발주가 통과한다
    Clock::time_point         trim_at_;
    bool                      trim_done_ = false;
};
