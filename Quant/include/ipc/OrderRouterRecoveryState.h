// 주문 라우터 체결·대조 쪽이 들고 있는 기록 — 재확인 대기 종목, 이전 세션 미체결 줄, 주문 사유, 체결통보 목격·미연결 누적,
//  접수 답보다 먼저 온 체결. OrderRouter 안에서는 같은 이름의 별칭으로 쓴다. 쓰는 곳은 Quant/src/ipc/OrderRouterFill.cpp·
//  OrderRouterReconcile.cpp. [why D-113] [why D-151]
#pragma once
#include "core/Types.h"
#include "core/SymbolTable.h"
#include "ipc/FillKey.h"
#include <array>
#include <cstdint>
#include <string>

namespace order_router
{

// 전송 타임아웃 뒤 재확인할 종목 하나. 번호는 주문 스레드가 받아 둔다 — 종목 표에 새로 넣는 일은 그 스레드 몫이다.
struct PendingReconcile
{
    std::string      ticker;
    symbol::SymbolId symbol_id;
};

// 기동 취소 단계의 부속 파일 한 줄 = odno|orgno|ticker|side|remaining.
using CarryRow = std::array<std::string, 5>;

// ── ODNO → 주문 사유 기록 ────────────────────────────────────────────────
//  history_는 메모리에만 있어 재기동하면 이전 세션 주문의 ODNO를 잊는다. 그 주문이
//  나중에 체결되면 전략도 사유도 모르는 미매핑 체결로 들어가고, 주문수량을 모르니
//  잔량 클램프도 걸 수 없다. 접수 시점에 한 줄씩 파일로 남겨 재기동 뒤에도 같은
//  정보를 복원한다. 파일은 거래일별 append 전용이다 — open_orders.txt는 스냅샷마다
//  통째로 덮어쓰고 살아 있는 주문만 담으므로 거기에 얹으면 안 된다.
struct OrderReason
{
    std::string ticker;
    std::string strategy_id;
    std::string reason;
    OrderSide   side      = OrderSide::BUY;
    int         quantity  = 0;
    double      price     = 0.0;
    double      reference_price = 0.0;
};

// 체결통보 키 하나를 어느 실시간 세션에서 몇 번 봤는가. 같은 세션 안의 같은 키는 분할체결이고,
//  새 세션에서 앞 세션까지 받은 횟수 이하로 다시 오면 재연결 뒤 재전송이다(on_fill 주석).
struct FillSighting
{
    uint32_t session_generation      = 0; // 마지막으로 이 키를 본 세션
    int      accepted_before_session = 0; // 그 세션이 시작되기 전까지 실체결로 받은 횟수
    int      seen_in_session         = 0; // 그 세션에서 본 횟수
    int      accepted                = 0; // 실체결로 받은 총 횟수
};

// 미연결 주문 하나의 누적 상태. 연결된 주문의 (signal.quantity, confirmed_quantity) 짝과 같은 역할이다.
struct UnlinkedOrder
{
    int order_quantity     = 0; // 전문 [16]ODER_QTY. 0이면 "전문이 안 줘서 모른다"
    int confirmed_quantity = 0; // 지금까지 장부에 반영한 수량
};

// 접수 답을 닫기 전에 온 체결 하나. 전송 스레드가 답을 받고 주문 스레드가 close_new로 ODNO를 적기까지의
//  틈에 체결통보가 먼저 올 수 있다. [why D-151]
struct EarlyFill
{
    FillNotification  notification;
    fill_key::FillKey key;
    uint64_t          order_number = 0;
};

} // namespace order_router
