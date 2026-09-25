// 주문 쪽 스위치 다섯·하루치 새로 열기·슬롯 면제 표 — 부르는 자리가 어느 프로세스 것인가에 따라 그 자리에서
//  고치거나 제어 요청 통로(Quant/src/core/ControlPlane.cpp)에 싣는다. 통로 자체(싣기·옮기기·적용)는 ControlPlane 몫이다.
//  Engine 클래스의 멤버 함수 본체다. [why D-114]
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  request_*() · apply_reset_daily() : 매크로 국면 폴링·ZMQ·운영단말·개장 전이
//  set_slot_exempt_tickers()         : 슬롯 면제 집합을 새로 정했을 때(전략 쪽)

#include "core/Engine.h"
#include "utils/Logger.h"
#include <string>

// ─── 주문 쪽 스위치 다섯 ──────────────────────────────────────────────────
//  OrderGate·원장은 주문 프로세스 것이다. 고치는 일은 주문 스레드가 하고(D-071 원칙 4), 딴 스레드는
//  제어 요청 한 줄로 부탁한다. 통로를 탈지 그 자리에서 고칠지는 **부르는 자리가 어느 프로세스 것인가**로
//  갈린다 — 역할 이름이 아니다. [why D-114]
//  - 신규진입 정지·매수 비율: 매크로 국면 폴링(전략 쪽 일감)만 부른다 → 통로
//  - 하루치 새로 열기·전방향 차단·수동 정지: 개장 전이(order_side 가드)·ZMQ·시세 감시·운영단말만 부르고
//    그 넷은 모두 주문 프로세스 것이다 → 그 자리에서 고친다. 전략 역할 분기는 남의 손이 닿을 때의 안전망이다

void Engine::apply_reset_daily()
{
    order_gate_.reset_daily();

    if (order_router_)
    {
        order_router_->reset_daily(); // V-4: 중복방지 키 일별 정리(거래일 prefix와 함께 cross-day 충돌 차단)
    }

    ledger_->new_trading_day(); // C-1: 새 거래일 → 총평가금 기준선 재캡처
}

void Engine::request_reset_daily()
{
    // 부르는 자리는 데이터 스레드의 장 시작 감지라 역할 셋이 다 지난다. 원장·게이트를 든 쪽만 그 자리서
    //  고치고, 전략은 통로로 청하고, 시세는 고칠 것이 없어 지나간다. [why D-114 단계 5]
    if (runs_order_side())
    {
        apply_reset_daily();
        return;
    }

    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind = ipc::ControlKind::kResetDaily;
    control_plane_.send_switch(request, "하루치 새로 열기");
}

void Engine::request_entry_halt(bool on)
{
    // 부르는 자리는 매크로 국면 폴링 하나이고 그것은 전략 쪽 일감이다 — Both 로 돌아도 같은 통로를 태운다.
    //  여기서 Both 만 질러가게 두면 통로가 갈라 띄운 날 처음 돈다. [why D-114]
    //  [inv] 주문 프로세스에는 옮겨 줄 전략 스레드가 없다 — 그 역할이면 넣지 않고 그 자리에서 고친다.
    if (role_ == ProcessRole::Order)
    {
        order_gate_.set_entry_halt(on);
        return;
    }

    // 시세 프로세스는 제어 줄의 보내는 쪽이 아니다 — 여기서 보내면 SPSC가 깨진다. [why D-114 단계 5]
    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kEntryHalt;
    request.toggle_on = on ? 1 : 0;
    control_plane_.send_switch(request, "신규 진입 정지");
}

void Engine::request_entry_scale(double entry_scale)
{
    // 신규진입 정지와 같은 자리에서 같은 판정으로 나온다 — 통로도 같다. [why D-114]
    if (role_ == ProcessRole::Order)
    {
        order_gate_.set_entry_scale(entry_scale);
        return;
    }

    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind        = ipc::ControlKind::kEntryScale;
    request.entry_scale = entry_scale;
    control_plane_.send_switch(request, "매수 비율");
}

void Engine::request_kill_switch(bool on)
{
    if (runs_order_side())
    {
        order_gate_.set_kill_switch(on);
        return;
    }

    // 시세 프로세스에는 게이트가 없다 — 주문을 내지 않으므로 막을 것도 없다. [why D-114 단계 5]
    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kKillSwitch;
    request.toggle_on = on ? 1 : 0;
    control_plane_.send_switch(request, "전방향 주문 차단");
}

void Engine::request_manual_halt(OrderSide side, bool on)
{
    if (runs_order_side())
    {
        order_gate_.set_manual_halt(side, on);
        return;
    }

    if (role_ == ProcessRole::Feed)
    {
        return;
    }

    ipc::ControlRequest request;
    request.kind      = ipc::ControlKind::kManualHalt;
    request.halt_side = static_cast<uint8_t>(static_cast<OrderSide::Value>(side));
    request.toggle_on = on ? 1 : 0;
    control_plane_.send_switch(request, "수동 정지");
}

void Engine::set_slot_exempt_tickers(const std::vector<std::string>& tickers)
{
    ipc::ControlRequest open;
    open.kind = ipc::ControlKind::kSlotExemptBegin;

    if (!control_plane_.send(open))
    {
        LOG_WARN("[Engine] 슬롯 면제 " + std::to_string(tickers.size()) + "종목 — 제어 통로가 가득 차 이번 판을 접는다");
        return;
    }

    uint32_t sent = 0;

    for (const auto& ticker : tickers)
    {
        ipc::ControlRequest row;
        row.kind      = ipc::ControlKind::kSlotExemptEntry;
        row.batch     = open.sequence;
        row.symbol_id = register_symbol(ticker); // 티커 문자열이 번호가 되는 경계 [why D-112]

        if (row.symbol_id == symbol::kNone)
        {
            continue;
        }

        // 한 줄이라도 못 보내면 commit 을 안 보내고 접는다 — 받는 쪽은 commit 없는 표를 걸지 않는다.
        if (!control_plane_.send(row))
        {
            LOG_WARN("[Engine] 슬롯 면제 집합을 보내다 통로가 가득 찼다 — 이번 판을 접는다(보낸 " +
                     std::to_string(sent) + "줄)");
            return;
        }

        ++sent;
    }

    ipc::ControlRequest close;
    close.kind      = ipc::ControlKind::kSlotExemptCommit;
    close.batch     = open.sequence;
    close.rank      = static_cast<int32_t>(sent);
    close.row_count = sent;

    if (!control_plane_.send(close))
    {
        LOG_WARN("[Engine] 슬롯 면제 집합 마무리를 못 보냈다 — 이번 판은 걸리지 않는다");
    }
}
