// 제어 요청 통로 — 전략 쪽이 주문 쪽 표(OrderGate·원장·보호 주문 표·종목 표)를 고쳐 달라고 보내는 요청을
//  싣고(send), 경계 너머로 옮기고(relay), 주문 스레드가 적용한다(apply). 통로는 두 토막이다 — 전략 프로세스
//  안에서 여럿이 모이는 앞 토막(MPSC)과, 공유 자리표 위에서 경계를 넘는 뒤 토막(SPSC 둘: 주문 쪽·시세 쪽).
//  Engine이 쥐던 통로 필드·보내기·옮기기·적용을 한 클래스로 모아 Engine 없이 시험하려고 뗐다. [why D-114]
//
//  스레드: send는 여러 스레드(기동 main·샤드·데이터·감시·운영단말·ZMQ 명령·번호를 기다리는 쪽), relay는 전략 스레드 또는 번호를
//  기다리는 쪽(자물쇠로 한 줄), apply는 주문 스레드, pop_feed는 시세 쪽 감시 스레드.
//  [inv] bind()는 스레드를 띄우기 전에 한 번 부른다 — 그 전에 relay·apply·pop_feed를 부르면 안 된다.
#pragma once

#include "core/MpscQueue.h"
#include "core/SymbolTable.h"
#include "core/WakeGate.h"
#include "ipc/ControlChannel.h"
#include "ipc/SharedSpscRing.h"
#include "risk/ProtectiveRule.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

class OrderGate;

namespace risk
{
class ProtectiveOrderBook;
}

class ControlPlane
{
public:
    // 표 하나가 여러 줄로 오므로 용량은 표 상한의 몇 배로 둔다 — 한 줄만 잃어도 그 표는 통째로 버려진다.
    //  뒤 토막(자리표 제어 면)의 용량도 이 값으로 잡는다(Quant/src/core/EngineLayout.cpp).
    static constexpr size_t kQueueCapacity = 8192;

    using Lane = ipc::SharedSpscRing<ipc::ControlRequest>;

    // reset_daily는 하루치 새로 열기 요청을 받았을 때 부른다(Engine::apply_reset_daily).
    ControlPlane(OrderGate& order_gate, risk::ProtectiveOrderBook& protective_book, symbol::SymbolTable& table,
                 wake::WakeGate& strategy_wake, wake::WakeGate& order_wake, std::function<void()> reset_daily);

    ControlPlane(const ControlPlane&)            = delete;
    ControlPlane& operator=(const ControlPlane&) = delete;

    // 뒤 토막 둘을 꽂는다 — 주문 쪽 줄과 시세 쪽 줄. 자리표 위에 있고 여기는 그 자리를 가리키기만 한다.
    void bind(Lane* order_lane, Lane* feed_lane);

    // 요청 한 줄 보내기. 순번은 여기서 찍는다. 앞 토막이 가득이면 거짓 — 표를 보내는 쪽은 그때 commit 을
    //  보내지 않고 접는다(반쪽 표를 거느니 이번 판을 통째로 거른다). [why D-114]
    bool send(ipc::ControlRequest& request);

    // 스위치 요청 한 줄을 싣는다. 못 실으면 큰 소리로 남긴다 — 사라진 것이 kill switch 일 수 있다.
    void send_switch(ipc::ControlRequest& request, std::string_view what);

    // 앞 토막에 쌓인 요청을 낱말에 따라 주문 쪽·시세 쪽 줄로 옮긴다. 주문 쪽으로 옮긴 것이 있으면 주문 스레드를 깨운다.
    //  [inv] 꺼내는 자리는 여기 하나뿐이고 relay_mutex_가 감싼다 — 앞 토막이 MPSC라 꺼내는 쪽이 하나여야 한다.
    void relay();

    // 주문 쪽 줄을 비우고 완성된 표를 건다. [inv] order_thread에서만 부른다(원칙 4).
    void apply();

    // 시세 쪽 줄에서 한 줄 꺼낸다. 구독·해지·칸 우선순위 낱말만 이 줄로 온다(ipc::routes_to_feed).
    bool pop_feed(ipc::ControlRequest& request);

    // 주문 쪽 줄이 비었나 — 주문 스레드가 잠들어도 되는지 볼 때 쓴다.
    bool order_lane_empty() const;

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t relay_dropped() const { return relay_dropped_.load(std::memory_order_relaxed); }
    uint64_t discarded() const { return discarded_.load(std::memory_order_relaxed); }

    // 전략에 꽂아 주는 보호 주문 창구.
    risk::ProtectiveOrderRegistry& protective_registry() { return protective_registry_; }

private:
    // 켜고 끄기는 요청으로 주문 스레드에 넘기고, 읽기 둘은 이 프로세스의 표를 그대로 본다 — 표를 고치는 것은
    //  단일 시퀀서다(원칙 4). 읽기가 맞는 것은 한 프로세스(Both)일 때뿐이다. 갈라 띄우면 전략 프로세스의 표는
    //  apply()가 돌지 않아 늘 비어 있고 둘 다 거짓을 돌려준다(응답 통로는 아직 없다). [why D-114]
    class ProtectiveRegistry : public risk::ProtectiveOrderRegistry
    {
    public:
        explicit ProtectiveRegistry(ControlPlane& plane);

        void arm(const risk::ProtectiveRule& rule) override;
        void disarm(const std::string& account, symbol::SymbolId symbol) override;
        bool owns(const std::string& account, symbol::SymbolId symbol) const override;
        bool consume_fired(const std::string& account, symbol::SymbolId symbol) override;

    private:
        // [inv] ControlPlane 멤버라 ControlPlane보다 오래 살지 않는다.
        ControlPlane& plane_;
    };

    OrderGate&                 order_gate_;
    risk::ProtectiveOrderBook& protective_book_;
    symbol::SymbolTable&       table_;
    wake::WakeGate&            strategy_wake_; // 깨울 쪽은 옮겨 줄 전략 스레드
    wake::WakeGate&            order_wake_;    // 옮긴 뒤 깨울 주문 스레드
    std::function<void()>      reset_daily_;

    // 앞 토막. 생산자가 여럿이라(머리 주석의 send 쪽) MPSC(원칙 5). 자물쇠를 둔 것은 번호를 기다리는
    //  쪽이 제 손으로 옮겨야 하기 때문이다(전략 스레드가 on_start 안에서 막히면 아무도 안 옮긴다). [why D-114]
    MpscQueue<ipc::ControlRequest> outbox_{kQueueCapacity};
    std::mutex                     relay_mutex_;
    Lane*                          order_lane_ = nullptr;
    Lane*                          feed_lane_  = nullptr;

    std::atomic<uint64_t> sequence_{0};      // 제어 요청 순번 발급기. 0은 안 쓴다
    std::atomic<uint64_t> dropped_{0};       // 앞 토막이 가득 차 못 보낸 줄 수. 0이 아니면 표가 버려졌다
    std::atomic<uint64_t> relay_dropped_{0}; // 뒤 토막이 가득 차 못 옮긴 줄 수. 보낸 쪽은 성공을 받은 뒤다
    std::atomic<uint64_t> discarded_{0};     // 주문 쪽이 반쪽 표로 보고 버린 줄 수

    // 주문 스레드가 표를 모으는 자리. 표마다 하나씩 둬 둘이 줄에서 섞여 와도 각자 모인다. apply 전용.
    ipc::ControlTableBuilder slot_exempt_{ipc::kControlTableMax};
    ipc::ControlTableBuilder entry_priority_{ipc::kControlTableMax};

    ProtectiveRegistry protective_registry_{*this};
};
