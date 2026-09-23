#pragma once
// 목표 비중표 → 주문 계획. 파일 파싱과 "목표 − 보유 = 주문" 계산만 있는 순수 조각이라 엔진 없이 시험한다.
//  파일은 PYQuant(스터디 22·23의 가치 기울임, 모멘텀)가 장 전에 쓰고, 엔진의 TargetBasketStrategy가 읽는다.
//  파일 계약(정본 docs/DECISIONS.md D-109):
//   { "schema": 1, "generated_at": "...", "as_of": "YYYY-MM-DD", "count": N,
//     "sleeves": { "VALUE": {"share": 0.5, "is_rebalance_day": true}, "MOMENTUM": {...} },
//     "targets": [ {"ticker","name","sleeve","weight","reference_price","action": "NEW|KEEP|DROP","reason"} ... ],
//     "liquidate_all": false }
//  weight는 슬리브 안 비중(DROP이 아닌 행끼리 합 1), 종목 목표 금액 = 순자산 × share × weight를 슬리브마다 더한 값.
//  DROP 행은 "이 종목은 이제 바스켓 것이 아니니 다 팔라"는 명시 지시 — 파일에 없는 종목은 건드리지 않는다.
#include "core/Types.h"
#include <cmath>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace basket
{
struct Sleeve
{
    double share            = 0.0;  // 바스켓 자본 중 이 슬리브 몫(0~1)
    bool   is_rebalance_day = false; // 오늘 갈아 끼우는 날인가. 아니면 DROP·신규·미충족 채우기만 한다
};

struct TargetRow
{
    std::string ticker;
    std::string name;
    std::string sleeve;
    double      weight          = 0.0; // 슬리브 안 비중. DROP이면 0
    double      reference_price = 0.0; // 전일 종가 — 미구독 종목은 틱이 없어 수량 계산이 이 값으로 간다
    std::string action;                // NEW | KEEP | DROP
    std::string reason;
};

struct Targets
{
    std::string                   generated_at;
    std::string                   as_of;   // YYYYMMDD로 정규화해 둔다(파일은 YYYY-MM-DD)
    int                           count = 0;
    bool                          liquidate_all = false;
    std::map<std::string, Sleeve> sleeves;
    std::vector<TargetRow>        rows;

    // 같은 파일인지 가르는 열쇠 — 상태 파일이 "오늘 이 파일로 리밸 끝"을 이 값으로 적는다.
    std::string key() const
    {
        return generated_at + "/" + std::to_string(count);
    }

    // 파일이 적은 모든 종목(DROP 포함) — 바스켓 소유 집합. 슬롯 제외·DEVSCALE 유니버스 제외에 쓴다.
    std::vector<std::string> tickers() const;
};

// 파싱·검증. 실패면 nullopt와 사유. 검증: schema 1, as_of 있음, count == 행 수, 슬리브별 DROP 아닌 행 weight 합이 1±0.005,
//  reference_price > 0(DROP 제외), 모르는 슬리브 이름 없음.
std::optional<Targets> parse_targets(const nlohmann::json& document, std::string& error);

// 원장이 보는 보유분 — TargetBasketStrategy가 confirmed_position·ledger_sellable로 채운다.
struct Holding
{
    int    quantity      = 0;
    double average_price = 0.0;
    int    sellable      = 0;
};

struct PlannedOrder
{
    std::string ticker;
    std::string strategy_id;     // BASKET_<슬리브>. 두 슬리브가 같이 들면 BASKET_VALUE+MOMENTUM
    OrderSide   side = OrderSide::NONE;
    int         quantity        = 0;
    double      reference_price = 0.0;
    std::string reason;
};

struct Plan
{
    double                    nav = 0.0; // 바스켓 순자산 = 시드 + 실현손익 + 보유 평가손익
    std::vector<PlannedOrder> sells;
    std::vector<PlannedOrder> buys;
    std::vector<std::string>  notes;     // 계산은 했지만 주문을 안 낸 이유(밴드 안·매도가능 0 등)
};

struct PlanInput
{
    double capital_krw      = 0.0; // 고정 시드
    double realized_pnl_krw = 0.0; // 상태 파일에 쌓은 실현손익(추정)
    double band             = 0.10; // 리밸 날 목표 금액 대비 이만큼 넘게 어긋나야 주문
    std::function<Holding(const std::string&)> holding; // 종목 → 원장 보유
};

// [formula] 목표 수량 = floor(순자산 × Σ(share × weight) / 기준가). 차이 = 목표 − 보유.
//   보유 0 → 목표 매수(날과 무관, 밴드 없음). 그 밖에 리밸 날: |차이 × 기준가| ≥ band × 목표 금액이면 주문.
//   리밸 아닌 날: DROP 매도, 보유 < 목표 × (1 − band)면 채우기만(줄이지 않는다).
//   liquidate_all: 파일의 모든 종목을 매도가능 수량만큼 판다.
//   매도 수량은 min(차이, 매도가능). 매도가능 0이면 notes에 남긴다.
Plan make_plan(const Targets& targets, const PlanInput& input);
} // namespace basket
