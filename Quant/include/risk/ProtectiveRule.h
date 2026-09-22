#pragma once
// 보호 주문 규칙과 등록 창구 — 전략이 보는 것은 여기까지다.
//  전략은 "이 종목은 평단 −2%면 판다"를 미리 등록해 두고, 판정과 발주는 주문 쪽(risk/ProtectiveOrders.h)이 한다.
//  전략이 멈추거나 죽어도 보유분이 지켜지는 것이 프로세스를 가르는 실익이라, 가르기 전에 이 표부터 둔다. [why D-114]
//  지금은 한 프로세스 안이고 경계는 이 인터페이스로만 있다 — 프로세스를 가르면 이 구현이 요청·응답 통로로 바뀐다(단계 2).
#include "core/Types.h"

#include <string>

namespace risk
{
// 표가 실제로 주문을 내는가.
//  off    — 표를 돌리지 않는다.
//  shadow — 판정만 하고 로그로 남긴다. 발주는 전략이 하던 대로 한다(기본값, 라이브 동작 불변).
//  owner  — 표가 발주한다. 등록한 전략은 자기 손절·트레일 판정을 건너뛴다(owns).
enum class ProtectiveMode
{
    Off,
    Shadow,
    Owner
};

ProtectiveMode protective_mode_from_string(const std::string& text);

const char* protective_mode_name(ProtectiveMode mode);

// 보호 주문 한 건. 퍼센트는 전부 평단 기준이고 0이면 그 조건을 안 본다.
struct ProtectiveRule
{
    std::string                account;
    std::string                ticker;
    symbol::SymbolId           symbol            = symbol::kNone;
    double                     stop_loss_percent = 0.0; // 평단 대비 이만큼 아래면 전량 시장가 매도
    double                     trail_arm_percent = 0.0; // 평단 대비 이만큼 위에 닿으면 트레일을 켠다
    double                     trail_percent     = 0.0; // 켜진 뒤 보유 구간 최고가 대비 이만큼 아래면 매도
    std::string                owner;                   // 등록한 전략 이름(로그·신호 strategy_id)
    strategy_table::StrategyId owner_index       = strategy_table::kNone; // 손익 귀속용 전략 번호

    bool watches_anything() const
    {
        return stop_loss_percent > 0.0 || (trail_arm_percent > 0.0 && trail_percent > 0.0);
    }
};

// 전략이 보는 창구 — 등록과 해제, 그리고 "표가 이 종목을 맡았나"뿐이다.
class ProtectiveOrderRegistry
{
public:
    virtual ~ProtectiveOrderRegistry() = default;

    // 같은 (계좌, 종목)이면 덮어쓴다. 조건이 하나도 없는 규칙은 해제로 친다.
    virtual void arm(const ProtectiveRule& rule) = 0;
    virtual void disarm(const std::string& account, symbol::SymbolId symbol) = 0;

    // 표가 발주를 맡았는가(owner 모드 + 등록됨). 전략은 true면 자기 손절 판정을 건너뛴다.
    virtual bool owns(const std::string& account, symbol::SymbolId symbol) const = 0;

    // 표가 이 종목을 청산했는가. 한 번의 발사를 한 번만 돌려준다 — 전략이 이것을 보고 미체결 매수를 거두고
    //  재진입 쿨다운을 건다(프로세스를 가르면 응답 통로가 이 자리를 대신한다).
    virtual bool consume_fired(const std::string& account, symbol::SymbolId symbol) = 0;
};
} // namespace risk
