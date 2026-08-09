#pragma once
#include "api/KisClient.h" // KisConfig
#include <nlohmann/json.hpp>

class Engine;

// ─────────────────────────────────────────────────────────────────────────────
// 전략 로더 — config["strategies"] 배열을 타입별로 파싱해 엔진에 등록한다.
//  타입별 load_* 함수를 레지스트리 맵으로 디스패치하고, active_regimes 공통
//  후처리를 적용한다. 스캔 유니버스는 universe:: 스캐너에 위임한다.
// ─────────────────────────────────────────────────────────────────────────────

// 로더 공유 컨텍스트 — 각 load_* 가 필요로 하는 엔진·인증 의존성 묶음.
struct StrategyLoadCtx
{
    Engine&          engine;
    const KisConfig& kis_cfg;       // 주문/잔고용(모의 가능)
    const KisConfig& quote_kis_cfg; // 시세 전용(실전 도메인) — 스캔 유니버스가 사용
    bool             has_quote_kis = false;
};

void load_strategies(StrategyLoadCtx& ctx, const nlohmann::json& strategies);
