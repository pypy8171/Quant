// RegimeController 점수/분류 단위 테스트 (KIS 불필요 — 순수 로직)
// 빌드: cmake --build <dir> --target test_regime
//
// v0 규칙: 축1(지수>200일선 ±1) + 축2(정배열+1/역배열-1/혼조0) → score ∈ {-2..+2}
//   BULL(+2) / BEAR(-2) / NEUTRAL(그 외)

#include "core/RegimeController.h"
#include <cassert>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

static RegimeSnapshot snap(bool above200, bool bull, bool bear)
{
    RegimeSnapshot s;
    s.above_ma200  = above200;
    s.aligned_bull = bull;
    s.aligned_bear = bear;
    return s;
}

static void PASS(const std::string& n) { std::cout << "[PASS] " << n << "\n"; }

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "=== RegimeController Unit Tests ===\n";
    RegimeController::Config cfg; // bull_th=+2, bear_th=-2

    // ── 점수 계산 ─────────────────────────────────────────────────────────
    assert(RegimeController::compute_score(snap(true,  true,  false), cfg) ==  2); // +1+1
    assert(RegimeController::compute_score(snap(false, false, true ), cfg) == -2); // -1-1
    assert(RegimeController::compute_score(snap(true,  false, false), cfg) ==  1); // +1+0 혼조
    assert(RegimeController::compute_score(snap(false, false, false), cfg) == -1); // -1+0
    assert(RegimeController::compute_score(snap(true,  false, true ), cfg) ==  0); // +1-1
    assert(RegimeController::compute_score(snap(false, true,  false), cfg) ==  0); // -1+1
    PASS("compute_score");

    // ── 분류 (임계 ±2) ───────────────────────────────────────────────────
    assert(RegimeController::classify( 2, cfg) == Regime::BULL);
    assert(RegimeController::classify(-2, cfg) == Regime::BEAR);
    assert(RegimeController::classify( 1, cfg) == Regime::NEUTRAL);
    assert(RegimeController::classify( 0, cfg) == Regime::NEUTRAL);
    assert(RegimeController::classify(-1, cfg) == Regime::NEUTRAL);
    PASS("classify");

    // ── 만장일치만 강한 국면 (경합 쟁점3: 보수성 확인) ────────────────────
    // 전항목 동의 시에만 BULL/BEAR, 하나라도 어긋나면 NEUTRAL
    assert(RegimeController::classify(
        RegimeController::compute_score(snap(true, true, false), cfg), cfg) == Regime::BULL);
    assert(RegimeController::classify(
        RegimeController::compute_score(snap(true, false, false), cfg), cfg) == Regime::NEUTRAL);
    PASS("conservative_unanimous");

    std::cout << "=== All tests passed ===\n";
    return 0;
}
