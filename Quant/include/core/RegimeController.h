#pragma once
#include "core/Types.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class KisClient;

// ─────────────────────────────────────────────────────────────────────────────
// RegimeController — 시장 국면(BULL/NEUTRAL/BEAR)을 장 시작 1회 수치 판정.
//
//  v0: 가격 2축만 (코스피 지수 일봉)
//    (1) 지수 종가 > 200일선  ? +1 : -1
//    (2) 정배열(ma20>ma60>ma120) ? +1 : 역배열 -1 : 혼조 0
//    score ∈ {-2..+2} → BULL(+2) / BEAR(-2) / NEUTRAL
//
//  - evaluate()는 장 시작 1회만(장중 재폴링 금지). 당일 미완성봉 제외, 전일 확정봉 기준.
//  - 조회 실패 시 직전 국면 유지, 연속 fail_fallback_n회 실패 시 NEUTRAL fallback.
//  - compute_score/classify는 KIS 없이 단위 테스트 가능한 순수 함수.
// ─────────────────────────────────────────────────────────────────────────────
class RegimeController
{
public:
    struct Config
    {
        std::string index_code = "0001";   // 코스피 종합 (지수 일봉 TR)
        int ma_long   = 200;
        int ma_mid    = 60;
        int ma_short  = 20;
        int ma_align3 = 120;
        int score_bull_threshold =  2;     // v0: 2축이라 ±2
        int score_bear_threshold = -2;
        int fail_fallback_n      =  3;     // 연속 N회 조회실패 → NEUTRAL fallback
    };

    explicit RegimeController(Config cfg = Config()) : cfg_(cfg) {}
    void set_kis(KisClient* k) { kis_ = k; }

    // ⚠ 계약: evaluate()는 data_thread에서 장 시작 1회만 호출(단일 호출자). fail_streak_가
    //   비원자적이라 다중 스레드/재호출 시 fallback 카운팅이 깨진다 (W2). 재폴링 금지.
    RegimeSnapshot evaluate();                  // 장 시작 1회 — 산출+상태갱신+스냅샷 반환
    Regime         current() const { return current_.load(); }
    RegimeSnapshot last_snapshot() const;
    bool           is_active_for(const std::vector<Regime>& active) const;

    // ── 순수 로직 (KIS 불필요 — 헤더 inline으로 단위 테스트가 KIS 링크 없이 가능) ──
    static int compute_score(const RegimeSnapshot& s, const Config& /*cfg*/)
    {
        int score = s.above_ma200 ? +1 : -1;            // 축1: 200일선
        if (s.aligned_bull)      score += 1;            // 축2: 정배열/역배열/혼조
        else if (s.aligned_bear) score -= 1;
        return score;
    }
    static Regime classify(int score, const Config& cfg)
    {
        if (score >= cfg.score_bull_threshold) return Regime::BULL;
        if (score <= cfg.score_bear_threshold) return Regime::BEAR;
        return Regime::NEUTRAL;
    }

private:
    Config cfg_;
    KisClient* kis_ = nullptr;                  // non-owning (Engine 수명관리)
    std::atomic<Regime> current_{Regime::UNKNOWN};
    mutable std::mutex snap_mtx_;
    RegimeSnapshot last_;
    int fail_streak_ = 0;
};

std::string to_string(Regime r);
