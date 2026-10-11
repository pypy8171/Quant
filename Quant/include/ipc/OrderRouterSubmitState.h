// 주문 라우터 발주 경로가 단계 함수 사이에 들고 다니는 상태 — 신규 주문(NewRoute)과 취소·정정(ModifyRoute).
//  OrderRouter 안에서는 같은 이름의 별칭으로 쓴다. 단계 함수는 Quant/src/ipc/OrderRouterSubmit.cpp·OrderRouterModify.cpp. [why D-151]
#pragma once
#include "core/Types.h"
#include "risk/OrderGate.h"
#include "api/IOrderExecutor.h"
#include <chrono>
#include <cstdint>
#include <string>

namespace order_router
{

// 신규 주문 한 건이 단계 함수들을 지나며 들고 다니는 상태. 순서는 클램프 → 이력 가드 → 게이트 → 전송 → 마무리다.
//  각 단계가 거짓을 돌려주면 그 자리에서 거부로 닫고 이력까지 적은 것이다 — new_route는 그대로 돌려주기만 한다.
struct NewRoute
{
    OrderSignal         signal;          // 들어온 신호의 사본 — 클램프가 수량을 고친다
    ManagedOrder        managed_order;
    OrderGate::OrderRef order_reference; // 장부 레코드 이름표 — 내부 주문번호와 주문 유형. ODNO는 접수 뒤에 붙는다
    OrderAck            acknowledgement;
    std::string         reject_reason;
    int                 allowed          = 0;     // 게이트가 허락한 수량(clamp_buy_quantity)
    bool                sell_no_quantity = false; // 매도가능이 모자라 예약매도 취소부터 해 봐야 하는가
    bool                freed            = false; // 예약매도 취소 뒤 재발주가 이미 접수됐는가
    bool                intent_taken     = false; // 장부에 INTENT를 적었는가
    // 구간 계측. 게이트까지 두 구간(이력 가드·게이트)은 stamp_gate_stages가 찍는다. [why D-117] [why D-126]
    int64_t entered_ns           = 0; // new_route에 들어온 시각
    int64_t history_guard_ns     = 0; // 이력 잠금·중복 가드에 쓴 시간 합
    int64_t history_lock_wait_ns = 0; // 그중 잠금을 기다린 몫
    // 접수 왕복지연과 그 안의 초당 한도 버킷 대기. count()의 타입 그대로 — MSVC는 long long이라 long이면 잘린다(C4244)
    std::chrono::milliseconds::rep rtt_ms         = 0;
    std::chrono::milliseconds::rep bucket_wait_ms = 0;
    // 왕복지연을 재기 시작한 시각 — INTENT 전이다. 전송 스레드로 넘기면 넘겨받기까지의 대기도 여기 들어간다.
    std::chrono::steady_clock::time_point send_started{};
    bool        transport_failed = false; // 전송이 예외로 끝났다 — 접수 여부를 모른다
    std::string transport_error;          // 그 예외 문구

    // 게이트까지의 두 구간을 한 번에 찍는다 — 이력 가드 몫을 게이트에서 빼 둘이 겹치지 않게 한다. [why D-117]
    void stamp_gate_stages();
};

// 취소·정정할 원주문의 값 사본 — 락 밖에서 KIS를 부르는 동안 history_ 원소가 축출될 수 있어 값으로 뜬다.
struct OriginalOrder
{
    std::string ticker;
    std::string kis_order_no;
    std::string krx_forwarding_org_no;
    std::string account;
    OrderSide   side        = OrderSide::NONE;
    int         outstanding = 0; // 뜬 시점의 미체결 잔량
};

// close_live_original_locked가 정한 원주문 선점 해제분. 장부 기록은 락을 푼 뒤 release_original이 한다.
struct OriginalRelease
{
    int      quantity     = 0; // 풀 선점 수량. 0이면 장부에 적지 않는다
    uint64_t order_number = 0; // 원주문의 내부 주문번호(이력에서 빠졌으면 0)
};

// 취소·정정 한 건이 세 토막(열기·전송·닫기)을 지나며 들고 다니는 상태.
struct ModifyRoute
{
    OrderSignal         signal;
    ManagedOrder        managed_order;
    OriginalOrder       original;
    int                 new_quantity = 0;    // 정정 수량. 취소는 쓰지 않는다
    OrderGate::OrderRef order_reference;      // 정정의 장부 레코드 이름표
    OrderAck            acknowledgement;
    bool                transport_failed = false; // 전송이 예외로 끝났다
    std::string         transport_error;          // 그 예외 문구
};

} // namespace order_router
