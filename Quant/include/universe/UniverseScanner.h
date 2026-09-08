#pragma once
#include "api/KisClient.h"
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// 유니버스 스캐너 — config 스캔 파라미터로 "오늘 어떤 종목을 볼지" 티커 목록을 산출.
//  전략 생성(파라미터·인스턴스화)은 호출자(StrategyFactory)가 담당하고, 여기서는
//  레짐 게이트·랭킹·필터만 수행한다. 인증된 시세 클라이언트(KisClient&)를 받는다.
//  스캔 스레드 전용. 선정 규칙과 단계 순서의 정본은 `UniverseScanner.cpp` 하나다.
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
    double      day_open = 0.0; // 원, 당일 시가 앵커. 0이면 랭킹 스냅샷가로 폴백된다
};

// 레짐 위험회피면 빈 목록을 돌려준다(신규 미등록). 실패도 예외가 아니라 빈 목록이다.
//  kis는 인증된 실전 시세 클라이언트여야 한다.
std::vector<ItbCandidate> scan_itb(KisClient& kis, const ItbScanCfg& cfg);

// ── DeviationScale 스캔 ─────────────────────────────────────────────────────
struct DevScanCfg
{
    int    scan_top_n      = 80;
    int    value_top_n     = 30;
    double min_price       = 5000.0;
    double max_price       = 0.0; // 원, 0이면 상한 없음
    int    max_register    = 40;
    double risk_off_idx    = -0.02;
    bool   require_aligned = true;
    int    align_probe_max = 60;   // 재스캔당 일봉 REST 상한
    int    align_daily_n   = 70;   // 봉, 한 종목당 받는 일봉 길이
    // 장중 일봉 재조회. 기본은 꺼 둔다 — 일봉은 전일 확정치라 재조회해도 같은 값이 온다. [why D-029]
    int    align_refresh_max = 0;    // 재스캔당 재조회 상한(0=끄기, 기본)
    int    align_refresh_sec = 600;  // 초, 이 시간이 지난 조회분만 재조회 대상
    int    union_refresh_sec = 0;    // 초, 후보 합집합 재수집 주기. 0=매 재스캔 새로 수집 [why D-028]
    double max_dev_pct     = 0.0;    // 이격 (price-SMA20)/SMA20 상한. 0=비활성 [why D-022]
    double min_dev_pct     = 0.0;    // 같은 이격의 하한. max와 짝지어 슬리브 밴드를 만든다 [why D-022]
    std::string universe_file;       // data.go.kr 시총∪거래대금 피드 경로. 비면 KIS 랭킹 축만 [why D-015]
    std::string prices_file;         // 전 종목 장중 시세 파일(`scripts/live_prices_feed.py` 산출) [why D-029]
    double min_turnover = 0.0;       // 원, 거래대금 하한. 0=비활성 [why D-029]
    bool   full_market = false;      // 후보 풀을 시세 파일의 전 종목으로 넓힌다 [why D-015]
    std::vector<std::string> sector_codes;   // 업종 등락률 축. 비면 끄기 [why D-029]
    int    sector_top_n      = 10;   // 업종당 상위 N행(등락률 내림차순)
    double sector_min_chg    = 0.0;  // %, 이 등락률 미만은 버린다
    // 횡단면 점수는 정배열 검사에 이미 쓴 일봉을 재활용하므로 추가 REST가 없다. [why D-018]
    int    score_top_n      = 0;   // 0=비활성(전체 등록), N=상위 N만
    double score_w_trend    = 1.0; // (SMA5-SMA60)/SMA60 의 z에 곱한다
    double score_w_pullback = 1.0; // -(price-SMA20)/SMA20 의 z에 곱한다
    double score_w_supply   = 0.0; // 로거 데이터 확보 후 활성 (D-014)
    double score_w_vol      = 0.5; // ATR(14)/종가 z의 감점 가중
    bool   kosdaq_enabled      = false;  // [why D-030]
    double risk_off_idx_kosdaq = -0.015; // 분수, 코스닥 지수 risk_off 임계 [why D-030]
};

// 초기 등록·주기적 재스캔이 공용으로 호출한다(cfg는 값 복사 캡처라 std::function 저장이 안전).
//  레짐 위험회피면 빈 목록을 돌려준다. 실패도 예외가 아니라 빈 목록이다.
//  out_names·out_scores(옵션)를 주면 등록 티커의 종목명과 종합점수를 채운다.
//  등록 순서가 곧 진입 우선순위다. 점수 → 비중 배수 변환은 `ScoreWeight.h`가 한다. [why D-018]
std::vector<std::string> scan_devscale(KisClient& kis, const DevScanCfg& cfg,
                                       std::unordered_map<std::string, std::string>* out_names = nullptr,
                                       std::unordered_map<std::string, double>* out_scores = nullptr);

} // namespace universe
