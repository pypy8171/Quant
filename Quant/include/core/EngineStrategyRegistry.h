#pragma once
#include "core/Types.h" // Regime
#include "strategy/StrategyBase.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Engine의 전략 레지스트리 — 등록된 전략 목록, 스냅샷 버전, 뗀 전략 대기열, 국면별 활성 전략 표.
//  Engine.h에서 떼어 낸 것이다 — Engine 안에서는 같은 이름(Engine::StrategyRegistry)으로 쓴다.
//  관련 결정: D-071·D-077.
// ─────────────────────────────────────────────────────────────────────────────

namespace engine
{

// 뗀 전략 대기열 항목(data_thread 전용). version=뗀 직후의 strategy_.version.
struct Retired
{
    std::unique_ptr<StrategyBase> strategy;
    uint64_t                      version = 0;
};
struct StrategyRegistry
{
    std::vector<std::unique_ptr<StrategyBase>> list;
    // list 동시성 보호: strategy_thread는 version 변경 시에만 StrategyBase*
    // 스냅샷을 재구성(무락 순회), data_thread는 재스캔 등록 시 락+version 증가.
    std::mutex             mutex;
    std::atomic<uint64_t>  version{0};
    std::atomic<uint64_t>  seen_version{0}; // strategy_thread가 마지막으로 스냅샷에 반영한 버전
    std::vector<Retired>   retired;
    // G1: 국면→전략 자동선택 상태 (data_thread 전용)
    std::map<Regime, std::vector<std::string>> regime_map; // 국면별 활성 전략 id(빈 항목=아무 전략도 활성 안 함)
    bool   has_regime_map = false;                 // false면 per-strategy active_regimes 폴백
    Regime last_selected_regime = Regime::UNKNOWN; // 직전 선택 국면(변화 감지→재선택·로그)
};

} // namespace engine
