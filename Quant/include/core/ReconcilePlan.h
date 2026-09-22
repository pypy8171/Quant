// core/ReconcilePlan.h — 잔고 대조 차이 계산: 원장 스냅샷 vs 브로커 잔고 → RECONCILE 행 목록.
//  순수 함수(헤더 전용, 상태 없음). Engine이 덮어쓰기·정리 전에 뜬 원장 값으로 부르고, 결과는
//  OrderRouter::record_reconcile이 logs/trades_YYYYMMDD.csv에 남긴다. [why D-038] 대사 단계 귀속.
#pragma once

#include "core/SymbolTable.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace reconcile
{

struct Held
{
    std::string      ticker;                        // 행 출력용 라벨
    int              quantity = 0;
    double           average  = 0.0;                // 원. 브로커는 pchs_avg_pric(소수 넷째 자리), 원장은 체결가 가중평균
    symbol::SymbolId symbol   = symbol::kNone;      // 맞춰 보는 키. 호출부가 원장 id·브로커 티커 intern으로 채운다 [why D-112]
};

// OrderRouter::ReconcileNote와 같은 모양. 라우터 헤더를 끌어오지 않으려고 여기서 정의하고 라우터가 별칭으로 쓴다.
struct Row
{
    std::string ticker;
    int         ledger_quantity = 0;
    int         broker_quantity = 0;
    double      ledger_average = 0.0;
    double      broker_average = 0.0;
    std::string action;      // "OVERWRITE" | "PRUNE" | "KEEP"
    std::string note;        // 자유 문구(대조 모드 등). 콤마는 라우터가 공백으로 바꾼다
};

// [formula] 평단 허용 오차 1원. 브로커 평단은 소수 넷째 자리까지 오고 원장은 체결가 정수 가중평균이라
//  같은 포지션도 소수점 아래에서 어긋난다. 1원 이상 벌어지면 체결 하나가 빠졌거나 다른 가격으로 들어간 것.
constexpr double kAvgToleranceWon = 1.0;

inline bool average_differs(double amount, double base)
{
    return std::fabs(amount - base) >= kAvgToleranceWon;
}

// 차이가 있는 종목만 행으로 만든다. 일치 종목은 행을 만들지 않는다 — 보유 40종목이 대조 주기마다
//  40행씩 쌓이면 체결 행이 대조 행에 묻힌다.
//  - 브로커에 있는 종목: 수량 또는 평단이 어긋나면 resync면 OVERWRITE, 아니면 KEEP(WS 모드는 원장이 정본).
//  - 원장에만 있는 종목: pruned에 들어 있으면 PRUNE(엔진이 이미 걷어냄), 아니면 KEEP(갓 열린 포지션 유예).
//  브로커 quantity<=0 항목은 보유가 아니므로 건너뛴다. 순서는 브로커 목록 → 원장 목록으로 결정적이다.
std::vector<Row> plan(const std::vector<Held>& ledger, const std::vector<Held>& broker, bool resync,
                             const std::vector<symbol::SymbolId>& pruned, const std::string& note);

} // namespace reconcile
