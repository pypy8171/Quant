#pragma once
#include "api/KisClient.h"
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// 유니버스 스캐너 — config 스캔 파라미터로 "오늘 어떤 종목을 볼지" 티커 목록을 산출.
//  전략 생성(파라미터·인스턴스화)은 호출자(StrategyFactory)가 담당하고, 여기서는
//  레짐 게이트·랭킹·필터만 수행한다. 인증된 시세 클라이언트(KisClient&)를 받는다.
// ─────────────────────────────────────────────────────────────────────────────
namespace universe
{

// ── ITB(장중 돌파) 스캔 ────────────────────────────────────────────────────
struct ItbScanCfg
{
    int    scan_top_n   = 30;
    double chg_min      = 0.02;
    double chg_max      = 0.12;
    double min_price    = 3000.0;
    bool   sd_filter    = true;
    double risk_off_idx = -0.01;
    int    max_register = 6;
};

struct ItbCandidate
{
    std::string ticker;
    std::string name;
    double      day_open = 0.0; // 당일 시가 앵커(0이면 랭킹 스냅샷가로 폴백됨)
};

// 거래대금 상위 → 등락률·최소가·(opt)수급 필터 → 레짐 게이트.
//  레짐 위험회피면 빈 목록(신규 미등록). kis는 인증된 실전 시세 클라이언트여야 한다.
std::vector<ItbCandidate> scan_itb(KisClient& kis, const ItbScanCfg& cfg);

// ── DeviationScale 스캔 ─────────────────────────────────────────────────────
struct DevScanCfg
{
    int    scan_top_n      = 80;
    int    value_top_n     = 30;
    double min_price       = 5000.0;
    double max_price       = 0.0; // 0이면 상한 없음
    int    max_register    = 40;
    double risk_off_idx    = -0.02;
    bool   require_aligned = true;
    int    align_probe_max = 60;
    int    align_daily_n   = 70;
    // 횡단면 스코어러(2026-08-09 회의 Task 4) — 오너 원안 "점수 내고 5개 골라서".
    //  score_top_n>0이면 정배열 통과 후보를 점수로 랭킹해 상위 N만 등록(0=전체, 기존 동작).
    //  점수는 정배열 검사에 이미 쓴 일봉을 재활용 → 추가 REST 0.
    //   • 추세강도 w_trend·(SMA5-SMA60)/SMA60   (정배열 기울기, 클수록 강한 상승)
    //   • 눌림깊이 w_pullback·-(price-SMA20)/SMA20 (SMA20 아래일수록 가점=과매도 우선)
    //   • 수급    w_supply·(수급틸트)  ← 기본 0.0. EOD 확정수급은 구조상 실시간 불가 →
    //     investor_flow_logger가 forward로 쌓인 뒤 ablation으로 켠다(지금은 데이터 없음).
    int    score_top_n      = 0;   // 0=비활성(전체 등록), N=상위 N만
    double score_w_trend    = 1.0;
    double score_w_pullback = 1.0;
    double score_w_supply   = 0.0; // 로거 데이터 확보 후 활성
};

// 시총 상위 ∪ 거래대금 상위 → 가격 필터 → (opt)정배열 프리필터. 티커 목록 반환.
//  초기 등록·주기적 재스캔이 공용으로 호출(cfg 값 복사 캡처라 std::function 저장 안전).
std::vector<std::string> scan_devscale(KisClient& kis, const DevScanCfg& cfg);

} // namespace universe
