#pragma once
#include "api/KisClient.h"
#include "core/SymbolTable.h"
#include "universe/ScoreWeight.h"

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// 유니버스 스캐너 — config 스캔 파라미터로 "오늘 어떤 종목을 볼지" 티커 목록을 산출.
//  전략 생성(파라미터·인스턴스화)은 호출자(StrategyFactory)가 담당하고, 여기서는
//  레짐 게이트·랭킹·필터만 수행한다. 인증된 시세 클라이언트(KisClient&)를 받는다.
//  스캔 스레드 전용. 단계 순서는 `UniverseScanner.cpp`, 단계별 선정 규칙은 같은 폴더의 단계 파일
//  (UniverseRiskGate·UniverseCandidates·UniverseFeatures·UniverseScoring, 선언은 detail/Pipeline.h)이 정본이다.
// ─────────────────────────────────────────────────────────────────────────────
namespace universe
{

// ── ITB(장중 돌파) 스캔 ────────────────────────────────────────────────────
struct ItbScanCfg
{
    int    scan_top_n   = 30;
    double change_min      = 0.02;
    double change_max      = 0.12;
    double min_price    = 3000.0;
    bool   standard_deviation_filter    = true;
    double risk_off_index = -0.01;
    // 재개 임계·체류는 DevScale 쪽(ScanCfg)과 같은 뜻이다. 기본값은 차단 임계와 같고 체류 0이라
    //  옛 단일 임계 동작과 같다 — 값을 가르는 것은 config 몫이다. [inv] resume >= risk_off_index
    double risk_off_index_resume = -0.01;
    int    risk_off_dwell_sec  = 0;
    int    max_register = 6;
};

struct ItbCandidate
{
    std::string ticker;
    std::string name;
    double      day_open = 0.0; // 원, 당일 시가 기준점. 0이면 랭킹 스냅샷가로 폴백된다
};

// 레짐 위험회피면 빈 목록을 돌려준다(신규 미등록). 실패도 예외가 아니라 빈 목록이다.
//  kis는 인증된 실전 시세 클라이언트여야 한다.
std::vector<ItbCandidate> scan_itb(KisClient& kis, const ItbScanCfg& config);

// ── DeviationScale 스캔 ─────────────────────────────────────────────────────
struct DevScanCfg
{
    int    value_top_n     = 30;
    int    turnover_top_n  = 0;      // 시세 표 당일 거래대금 상위 N을 후보로. 0=끄기 [why D-146]
    double min_price       = 5000.0;
    double max_price       = 0.0; // 원, 0이면 상한 없음
    int    max_register    = 40;
    double risk_off_index    = -0.02;
    bool   require_aligned = true;
    int    align_lookup_max = 60;   // 재스캔당 일봉 REST 상한
    int    align_daily_n   = 70;   // 봉, 한 종목당 받는 일봉 길이
    int    union_refresh_sec = 0;    // 초, 후보 합집합 재수집 주기. 0=매 재스캔 새로 수집 [why D-028]
    double max_deviation_percent     = 0.0;    // 이격 (price-SMA20)/SMA20 상한. 0=비활성 [why D-022]
    // 정배열 마지막 조건(SMA10>SMA20)의 허용오차. 0=엄격(기존). tol을 주면 SMA10이 SMA20보다
    //  tol만큼 아래인 종목까지 통과한다 — 경계에서 판정이 진동하는 것을 막을 때 쓴다.
    //  1.0 이상이면 이 조건 자체가 사라져 2조건(SMA5>SMA10)만 남는다.
    double align_moving_average_tolerance_percent = 0.0;
    std::string universe_file;       // data.go.kr 거래대금 상위 피드 경로. 비면 KIS 랭킹 축만 [why D-015]
    // 엔진 안 시세판(universe/MarketBoard.h)에서 시세·유니버스를 받는다. 켜져 있으면 universe_file은 시세판이
    //  아직 첫 판을 못 받았을 때만 읽는다(장 전 기동 직후). 꺼져 있으면 전 종목 시세 없이 KIS 랭킹 축만 쓴다 [why D-147]
    bool   market_board = false;
    // 장 전 일봉 캐시 데우기 마감(KST HHMM). 0=끄기. 시세판 목록의 전 종목(직전 세션 거래대금 순)을 이 시각까지
    //  받아 둔다 — 장 중 첫 스캔이 일봉 REST 수백 건으로 밀리지 않게 [why D-147]
    int    daily_warm_until_hhmm = 0;
    double min_turnover = 0.0;       // 원, 거래대금 하한. 0=비활성 [why D-029]
    bool   full_market = false;      // 후보 풀을 시세 파일의 전 종목으로 넓힌다 [why D-015]
    std::vector<std::string> sector_codes;   // 업종 등락률 축. 비면 끄기 [why D-029]
    int    sector_top_n      = 10;   // 업종당 상위 N행(등락률 내림차순)
    double sector_min_change    = 0.0;  // %, 이 등락률 미만은 버린다
    // 횡단면 점수는 정배열 검사에 이미 쓴 일봉을 재활용하므로 추가 REST가 없다. [why D-018]
    int    score_top_n      = 0;   // 0=비활성(전체 등록), N=상위 N만
    double score_weight_trend    = 1.0; // (SMA5-SMA20)/SMA20 의 z에 곱한다(D-141부터 20일 기준)
    double score_weight_pullback = 1.0; // -(price-SMA20)/SMA20 의 z에 곱한다
    double score_weight_volume      = 0.5; // ATR(14)/종가 z의 감점 가중
    // 거래대금 축 — log(거래대금)의 z에 곱한다. 0=비활성(기존). 추세·눌림이 비슷하면 더 두꺼운
    //  종목을 위로 올린다. 알파 축이 아니라 체결비용 축이다(얇은 종목의 청산 슬리피지 회피).
    double score_weight_liquidity = 0.0;
    bool   kosdaq_enabled      = false;  // [why D-030]
    double risk_off_index_kosdaq = -0.015; // 분수, 코스닥 지수 risk_off 임계 [why D-030]
    // 지수 게이트의 재개 임계와 최소 체류. 차단 임계 하나로만 매 재스캔(20초) 판정하면 지수가
    //  경계를 오갈 때 게이트가 같이 떤다(2026-08-21에 2분 53초 간격 토글). 차단은 risk_off_index,
    //  재개는 이 값 위로 올라와야 풀리고, 상태를 바꾼 뒤 dwell 초 동안은 다시 바꾸지 않는다.
    //  [inv] resume >= risk_off_index 여야 히스테리시스가 성립한다(같으면 옛 동작).
    double risk_off_index_resume        = -0.012; // 분수, 코스피 재개 임계 [why D-033]
    double risk_off_index_kosdaq_resume = -0.009; // 분수, 코스닥 재개 임계 [why D-033]
    int    risk_off_dwell_sec         = 600;    // 초, 상태 변경 후 최소 체류. 0=끄기 [why D-033]
};

// 스캔 결과 — 세 배열은 같은 순서(symbols[i]의 이름이 names[i], 점수가 scores[i].score).
//  응답의 문자열 티커는 스캐너 안에서 종목 테이블에 한 번 들어가고, 밖으로는 id만 나간다. [why D-112]
//  scores는 점수 경로(require_aligned)에서만 채워진다 — 프리필터만 쓴 스캔은 비어 있다.
struct ScanResult
{
    std::vector<symbol::SymbolId> symbols;
    std::vector<std::string>      names;
    ScoreList                     scores;
};

// 전 종목 장중 시세(시세판의 판) 한 줄. 종목 id가 칸 번호다.
struct MarketQuote
{
    double      price  = 0.0; // 원, 0이면 이번 판에 없는 종목
    double      value  = 0.0; // 원, 누적 거래대금
    double      volume = 0.0; // 주, 누적 거래량
    std::string name;
};

using QuoteTable = std::vector<MarketQuote>;

// 표를 비운다 — 시세판이 꺼져 있거나 첫 판 전이면 스캐너가 이쪽을 부른다. 표는 호출자가 재스캔 사이에 들고
//  있다(매번 새로 잡지 않고, 이름 문자열 버퍼는 남긴다).
void clear_quote_table(QuoteTable& quotes, const symbol::SymbolTable& symbols);

struct BoardSnapshot;

// 시세판(universe/MarketBoard.h)의 판으로 quotes의 칸을 다시 채운다. 이번 판에 없는 종목의 가격·거래대금·거래량은
//  0으로 비우고, 이름은 바뀐 때만 다시 복사한다.
void load_quote_table(const BoardSnapshot& board, QuoteTable& quotes, symbol::SymbolTable& symbols);

// 장 전 일봉 캐시 데우기 스레드를 띄운다. 프로세스에 한 번만 뜨고, 마감 시각이 이미 지났으면 띄우지 않는다.
//  kis_config는 시세 키(실전 도메인)다. 받은 요약은 다음 스캔이 캐시로 옮긴다.
void start_daily_warm(const KisConfig& kis_config, const DevScanCfg& config);

// 초기 등록·주기적 재스캔이 공용으로 호출한다(config는 값 복사 캡처라 std::function 저장이 안전).
//  레짐 위험회피면 빈 결과를 돌려준다. 실패도 예외가 아니라 빈 결과다.
//  등록 순서가 곧 진입 우선순위다. 점수 → 비중 배수 변환은 `ScoreWeight.h`가 한다. [why D-018]
//  quotes는 시세 파일을 담아 두는 표로, 같은 슬리브의 스캔끼리 이어 쓴다(스캔은 한 번에 하나씩 돈다).
ScanResult scan_devscale(KisClient& kis, const DevScanCfg& config, symbol::SymbolTable& symbols,
                         QuoteTable& quotes);

} // namespace universe
