#pragma once
#include "api/KisClient.h"
#include <string>
#include <unordered_map>
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
    // 장중 일봉 재조회 — 지금은 꺼두는 게 맞다. 일봉은 include_today=false로 받아
    //  전일 확정치에서 멈춰 있고(SMA·수익률·ATR 전부 과거값), 오늘 가격은 시세 파일의
    //  현재가를 최신 봉 자리에 접어 넣어 반영하므로 재조회해도 같은 값이 온다.
    //  종목이 신규상장 60봉을 채우는 등 캐시 자체를 갱신할 사유가 생겼을 때만 쓴다.
    //  align_refresh_max>0이면 매 재스캔마다 "가장 오래 안 본" 후보부터 그 수만큼 다시 조회한다.
    int    align_refresh_max = 0;    // 재스캔당 재조회 상한(0=끄기, 기본)
    int    align_refresh_sec = 600;  // 이 시간이 지난 조회분만 재조회 대상
    // 후보 합집합 갱신 주기(초) — 랭킹·업종 축은 KIS REST 23콜이라 재스캔마다 돌리면
    //  초당 한도를 이 축이 먹는다. 반면 정배열·이격·점수 재판정은 일봉 캐시와 시세 파일만
    //  보므로 REST가 0이다. 이 값을 재스캔 주기보다 크게 두면 "후보 수집"은 느리게,
    //  "스코어링"은 재스캔 주기대로 빠르게 돈다. 0=매 재스캔 새로 수집(기존 동작).
    int    union_refresh_sec = 0;
    // 과확장 컷 — 정배열이어도 일봉 이격 (price-SMA20)/SMA20 이 값을 넘으면 제외.
    //  0=비활성(기존 동작). >0이면 존 밴드(entry_upper_pct)를 벗어나 진입 불가한
    //  폭등주(이격 33~104%)를 유니버스에서 미리 제거해 슬롯 낭비를 막는다.
    double max_dev_pct     = 0.0;
    // 과확장 "하한" — 이격이 이 값 미만이면 제외. 0=비활성(기존 동작).
    //  max_dev_pct와 짝을 이뤄 밴드를 만든다. 추세확장 슬리브가 눌림 슬리브가 버리는
    //  구간(min=0.05, max=0.30)만 골라 가져가는 데 쓴다. 두 슬리브의 유니버스는
    //  이 밴드로 상호배타라 같은 티커를 두 전략이 잡는 일이 없다.
    double min_dev_pct     = 0.0;
    // data.go.kr 시총∪거래대금 top-N 유니버스 피드 파일(ETF-free·30행캡 우회). 비면 KIS 랭킹 축만.
    //  Python universe_feed.py가 T-1 스냅샷으로 {ticker,name,close} 리스트를 이 경로에 기록하고,
    //  scan_devscale가 후보 풀의 "맨 앞"(기동 점검 우선) 4번째 축으로 union한다. KIS 랭킹 TR의
    //  30행 하드캡·장중 ETF 잠식을 우회해 개별주 깊은 풀을 확보한다.
    std::string universe_file;
    // 전 종목 장중 시세 파일(scripts/live_prices_feed.py 산출). 네이버 벌크에서
    //  코스피+코스닥 전 종목의 현재가·거래량·거래대금을 2분마다 받아 떨괴 것.
    //  KIS 초당 한도를 안 쓰므로 후보 전체의 정배열·이격을 매 재스캔마다 다시 판정할 수 있다.
    std::string prices_file;
    // 거래대금 하한(원). 유동성 없는 종목이 등락률 축으로 유니버스에 들어오는 것을 막는다.
    //  0=비활성. 2026-09-08 000215(DL우)·010955(S-Oil우)가 이 필터 부재로 등록됐다.
    double min_turnover = 0.0;
    // 후보 풀을 market_map 전체(코스피+코스닥 전 종목)로 넓힌다. 종목명은 시세 파일에서 가져온다.
    bool   full_market = false;
    // 업종별 등락률 축 — 장중에 갱신되는 유일한 후보 소스.
    //  data.go.kr 축은 장 마감 스냅샷이라(09-08 실행의 기준일이 09-04) 장중에 재생성해도 같은 목록이고,
    //  KIS 랭킹 3축은 축마다 30행 하드캡이라 시장 전체가 아니라 대형주 언저리만 본다. 그래서 오늘
    //  달아오른 섹터(전력·원전·반도체)가 후보에 들어올 경로가 아예 없었다 — 09-08 실측으로
    //  업종 28코드를 훑으면 524종목이 잡히고 그중 441개가 아침 유니버스에 없다(한전기술 +14.8%,
    //  우진 +10.8% 등). 재스캔마다 이 축을 다시 긁어 후보 풀을 장중 상태로 되돌린다.
    //  비면 끄기(기존 동작). 호출 비용은 코드당 1회 REST — 28코드/600초는 0.05 calls/s.
    std::vector<std::string> sector_codes;
    int    sector_top_n      = 10;   // 업종당 상위 N행(등락률 내림차순). 기동 점검 예산을 지키는 고삐
    double sector_min_chg    = 0.0;  // 이 등락률(%) 미만은 버린다. 강세만 담아 풀 오염을 막는다
    // 횡단면 스코어러(2026-08-09 회의 Task 4) — 오너 원안 "점수 내고 5개 골라서".
    //  score_top_n>0이면 정배열 통과 후보를 점수로 랭킹해 상위 N만 등록(0=전체, 기존 동작).
    //  점수는 정배열 검사에 이미 쓴 일봉을 재활용 → 추가 REST 0.
    //   • 추세강도 w_trend·(SMA5-SMA60)/SMA60   (정배열 기울기, 클수록 강한 상승)
    //   • 눌림깊이 w_pullback·-(price-SMA20)/SMA20 (SMA20 아래일수록 가점=과매도 우선)
    //   • 수급    w_supply·(수급틸트)  ← 기본 0.0. 장 마감 확정수급은 구조상 실시간 불가 →
    //     investor_flow_logger가 forward로 쌓인 뒤 ablation으로 켠다(지금은 데이터 없음).
    int    score_top_n      = 0;   // 0=비활성(전체 등록), N=상위 N만
    double score_w_trend    = 1.0;
    double score_w_pullback = 1.0;
    double score_w_supply   = 0.0; // 로거 데이터 확보 후 활성
    // 변동성 페널티 — ATR(14)/종가의 횡단면 z를 점수에서 뺀다(같은 추세·눌림이면 덜 흔들리는 쪽).
    //  1.0(동등)이면 변동성만으로 순위가 크게 뒤집혀 저변동 대형주로 책이 쏠린다. 0.5는 순위를
    //  뒤집기보다 동점자를 가르는 정도로 작용한다. (회의 2026-09-07)
    double score_w_vol      = 0.5;
    // ── 코스닥 참여(2026-08-19 strategist·data-sourcer 회의) ──────────────────
    //  기본 false: universe_scan.json에 코스닥("market"=="KOSDAQ") 종목이 섞여 있어도 전량 드롭
    //  → 코스피 동작을 오늘과 바이트 단위로 동일 유지(라이브 무위험). 코스닥 장 마감 백테스트가 LAB
    //  게이트(PF≥1.3/Sharpe≥1.0/MDD≤15%/표본≥200)를 통과한 뒤에만 true로 개방한다.
    bool   kosdaq_enabled      = false;
    // 코스닥 지수(get_index_price("1001")) risk_off 임계(분수). 코스닥 신규진입은 이중 AND —
    //  (코스피 정상 AND 코스닥 정상)일 때만 통과. 전이 함정(코스피 급락→코스닥 후행하락) 회피용.
    //  코스닥이 먼저 무너지면 더 빨리 차단하도록 코스피(risk_off_idx)보다 보수적(덜 음수) 권장.
    double risk_off_idx_kosdaq = -0.015;
};

// 시총 상위 ∪ 거래대금 상위 → 가격 필터 → (opt)정배열 프리필터. 티커 목록 반환.
//  초기 등록·주기적 재스캔이 공용으로 호출(cfg 값 복사 캡처라 std::function 저장 안전).
//  out_names(옵션)를 주면 등록 티커→종목명(hts_kor_isnm)을 채워 로그 라벨에 쓴다.
//  out_scores(옵션)를 주면 등록 티커→종합점수를 채운다. 호출자가 이 점수로 비중 배수를
//  만들고(ScoreWeight.h), 등록 순서가 곧 진입 우선순위가 된다.
std::vector<std::string> scan_devscale(KisClient& kis, const DevScanCfg& cfg,
                                       std::unordered_map<std::string, std::string>* out_names = nullptr,
                                       std::unordered_map<std::string, double>* out_scores = nullptr);

} // namespace universe
