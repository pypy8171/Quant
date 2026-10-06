// 주문 쪽 스위치 다섯·하루치 새로 열기·슬롯 면제 표 — 부르는 자리가 어느 프로세스 것인가에 따라 그 자리에서
//  고치거나 제어 요청 통로(Quant/src/core/ControlPlane.cpp)에 싣는다. 통로 자체(싣기·옮기기·적용)는 ControlPlane 몫이다.
//  Engine 클래스의 멤버 함수 본체다. [why D-114]
//
//  ── 부르는 자리 ──────────────────────────────────────────────────────────
//  request_*() · apply_reset_daily() : 매크로 국면 폴링·ZMQ·운영단말·개장 전이
//  set_slot_exempt_tickers()         : 슬롯 면제 집합을 새로 정했을 때(전략 쪽)

#include "core/Engine.h"
#include "core/KstTime.h"
#include "utils/Logger.h"
#include <string>

// ─── 주문 쪽 스위치 다섯 ──────────────────────────────────────────────────
//  OrderGate·장부는 주문 프로세스 것이다. 고치는 일은 주문 스레드가 하고(D-071 원칙 4), 딴 스레드는
//  제어 요청 한 줄로 부탁한다. 통로를 탈지 그 자리에서 고칠지는 **부르는 자리가 어느 프로세스 것인가**로
//  갈린다 — 역할 이름이 아니다. [why D-114]
//  - 신규진입 정지·매수 비율: 매크로 국면 폴링(전략 쪽 일감)만 부른다 → 통로
//  - 하루치 새로 열기·전방향 차단·수동 정지: 개장 전이(order_side 가드)·ZMQ·시세 감시·운영단말만 부르고
//    그 넷은 모두 주문 프로세스 것이다 → 그 자리에서 고친다. 전략 역할 분기는 남의 손이 닿을 때의 안전망이다

void Engine::apply_reset_daily()
{
    // 같은 거래일 두 번째 호출(장중 재기동·US 22:30)이면 게이트가 거절한다. 라우터·기준선도 그날 이미
    //  새로 열었으니 같이 건너뛴다. [why A-4]
    const auto trading_date = static_cast<uint32_t>(std::stoul(kst::date_yyyymmdd(std::time(nullptr))));

    if (!order_gate_.reset_daily(trading_date))
    {
        return;
    }

    if (order_router_)
    {
        order_router_->reset_daily(); // V-4: 중복방지 키 일별 정리(거래일 prefix와 함께 cross-day 충돌 차단)
    }

    ledger_->new_trading_day(); // C-1: 새 거래일 → 총평가금 기준선 재캡처
}

void Engine::request_reset_daily()
{
    // 부르는 자리는 데이터 스레드의 장 시작 감지라 역할 셋이 다 지난다. 장부·게이트를 든 쪽만 그 자리서
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

bool Engine::set_slot_exempt_tickers(std::string_view owner, const std::vector<std::string>& tickers)
{
    const strategy_table::StrategyId owner_index = order_gate_.ledger().strategy_index_of(owner);

    if (owner_index == strategy_table::kNone)
    {
        LOG_WARN("[Engine] 슬롯 면제 소유자 " + std::string(owner) + " 번호를 못 받았다 — 이번 판을 접는다");
        return false;
    }

    // 번호 등록은 기다릴 수 있어 잠그기 전에 끝낸다.
    std::vector<symbol::SymbolId> symbols;
    symbols.reserve(tickers.size());

    for (const auto& ticker : tickers)
    {
        const symbol::SymbolId symbol = register_symbol(ticker); // 티커 문자열이 번호가 되는 경계 [why D-112]

        if (symbol != symbol::kNone)
        {
            symbols.push_back(symbol);
        }
    }

    // 받는 쪽 표(ControlTableBuilder)는 한 번에 하나만 연다 — 소유자 둘이 동시에 보내 줄이 섞이면 앞 표가 버려지므로
    //  여는 줄부터 닫는 줄까지를 한 번에 보낸다. [why D-157]
    const char* failure = nullptr;
    uint32_t    sent    = 0;
    {
        std::lock_guard<std::mutex> lock(slot_exempt_send_mutex_);

        ipc::ControlRequest open;
        open.kind        = ipc::ControlKind::kSlotExemptBegin;
        open.owner_index = owner_index;

        if (!control_plane_.send(open))
        {
            failure = "여는 줄";
        }

        for (size_t index = 0; failure == nullptr && index < symbols.size(); ++index)
        {
            ipc::ControlRequest row;
            row.kind        = ipc::ControlKind::kSlotExemptEntry;
            row.batch       = open.sequence;
            row.owner_index = owner_index;
            row.symbol_id   = symbols[index];

            // 한 줄이라도 못 보내면 commit 을 안 보내고 접는다 — 받는 쪽은 commit 없는 표를 걸지 않는다.
            if (!control_plane_.send(row))
            {
                failure = "종목 줄";
                break;
            }

            ++sent;
        }

        if (failure == nullptr)
        {
            ipc::ControlRequest close;
            close.kind        = ipc::ControlKind::kSlotExemptCommit;
            close.batch       = open.sequence;
            close.owner_index = owner_index;
            close.rank        = static_cast<int32_t>(sent);
            close.row_count   = sent;

            if (!control_plane_.send(close))
            {
                failure = "닫는 줄";
            }
        }
    }

    if (failure != nullptr)
    {
        LOG_WARN("[Engine] 슬롯 면제 " + std::string(owner) + " " + std::to_string(symbols.size()) +
                 "종목 — 통로가 가득 차 " + failure + "을 못 보내 이번 판을 접는다(보낸 종목 " + std::to_string(sent) + "줄)");
        return false;
    }

    LOG_INFO("[Engine] 슬롯 면제 보냄 " + std::string(owner) + "(번호 " + std::to_string(owner_index) + ") " +
             std::to_string(symbols.size()) + "종목");
    return true;
}
