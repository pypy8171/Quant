// DeviationScale 유니버스 스캔의 단계 사이 계약 — 단계 파일(UniverseRiskGate·UniverseQuotes·UniverseCandidates·
//  UniverseFeatures·UniverseScoring)이 주고받는 자료형과 함수 선언이다. 밖에 드러내는 것은 universe/UniverseScanner.h
//  하나고, 이 파일은 src/universe 안에서만 읽는다. 단계 순서는 scan_devscale(UniverseScanner.cpp)이 쥔다.
//  스캔 스레드 전용. [why D-028·D-112]
#pragma once
#include "universe/UniverseScanner.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace universe::detail
{
// ── 일봉 정배열 판정 캐시 ───────────────────────────────────────────────
//  캐시하는 것은 정배열 판정이 아니라 그 재료인 확정된 과거 일봉이다.
//  KIS 일봉을 include_today=false로 받으므로(D-005) d[0]은 전일 확정봉이고 장중에
//  바뀔 일이 없다. 당일봉은 따로 받지 않고 현재가를 SMA에 직접 접어 넣는다 —
//  s_n_live = (s_n*n - r_n + price_live) / n. 그래서 정배열·이격 판정은 재스캔마다
//  새 값으로 다시 나오고, REST만 하루 1회로 줄어든다.
//  px_live는 랭킹 축이 실어오는 전 종목 시세 파일(네이버 벌크)에서 온다. 이 파일이
//  끊기면 price가 전일 종가로 돌아가 판정이 정말로 얼어붙는다 — 나이를 경고로 내보낸다.
//  캐시가 없던 때는 재스캔마다 후보 전체의 일봉을 다시 받았고, 그 비용이
//  rescan_interval에 반비례해 후보 집합을 넓히는 것 자체가 막혔다(align_lookup_max가 그 캡).
//  ATR·봉수처럼 확정봉만 쓰는 값은 그대로 하루 고정이다.
struct DailyLookup
{
    std::string date_yyyymmdd;                       // 조회 시각의 로컬 날짜(YYYYMMDD)
    int    bars  = 0;                      // 확보 봉수(<20이면 판정 불가)
    double average_5 = 0.0, average_10 = 0.0, average_20 = 0.0;
    double close = 0.0;                    // 최신 종가(d[0])
    // [formula] SMA에 오늘 가격을 접어 넣을 때 빠지는 봉의 종가.
    //  s_n_live = (s_n*n - roll_n + price_live) / n — REST 없이 정배열을 장중 갱신한다.
    double r5 = 0.0, r10 = 0.0, r20 = 0.0;
    double atr_percent = 0.0;                  // ATR(14)/종가. 정배열 판정용 일봉 재활용(추가 REST 0)
};

// 일봉 요약 캐시 — 종목 id 인덱스 배열(date_yyyymmdd가 비면 없음). 표는 락으로 감싼다. [inv] 프로세스 안 종목 테이블은 하나다(Engine의 symbols_.table, OrderGate에도 주입된다) — id는 지워지지
//  않으므로 전역 캐시가 id를 들어도 된다. 파일은 문자열 티커로 쓰고 읽을 때 intern한다. 디스크 사본은 장중 재기동 대비다 — 메모리 캐시가 비면 후보
//  수백 건의 일봉을 실계좌 150ms, 모의 600ms 간격으로 다시 받아야 하고 그동안 발주 경로의 REST까지 밀린다.
//  확정된 과거 일봉이라 같은 거래일 안에서는 그대로 재사용해도 된다. 파일은 거래일별로
//  나누므로 날짜가 바뀌면 자연히 무시된다.
class DailyLookupCache
{
public:
    // 프로세스당 거래일 1회. 읽기 실패는 캐시 미스와 결과가 같으므로 경고만 남긴다.
    void load_today(const std::string& date_yyyymmdd, symbol::SymbolTable& symbols);

    // 쓰다 만 파일을 다음 기동이 읽지 않도록 임시 파일에 쓰고 바꿔치운다.
    void save_today(const std::string& date_yyyymmdd, const symbol::SymbolTable& symbols) const;

    // 오늘치가 있으면 채우고 true. 날짜가 다르면 미스로 본다.
    bool get(symbol::SymbolId symbol, const std::string& date_yyyymmdd, DailyLookup& out) const;

    void put(symbol::SymbolId symbol, const DailyLookup& daily_lookup);

private:
    static std::string cache_path(const std::string& date_yyyymmdd);
    void reserve_locked(size_t size);

    mutable std::mutex       mutex_;
    std::vector<DailyLookup> by_symbol_; // 종목 id 인덱스
    std::string              loaded_;
};
extern DailyLookupCache g_lookup_cache;

// 시장 구분 — 파일의 "KOSPI"/"KOSDAQ" 문자열은 읽는 자리에서 한 번 이 값이 된다.
enum class Market : uint8_t
{
    Unlisted = 0, // 사전(market_map)에 없음
    Unknown,      // 사전에 있으나 코스피·코스닥 보통주가 아님(ETN 등) — 코스닥과 같은 보수 판정
    Kospi,
    Kosdaq,
};

Market market_from_text(std::string_view text);

// 후보 합집합 — 수집 축들이 공유하는 누적기이자 그대로 재사용 캐시의 몸통이다.
//  [why D-028] 랭킹·업종 축은 KIS REST 랭킹 3콜 + 업종 코드 수만큼(sector_codes, 250ms 간격)이라 재스캔을 20초로
//  당기면 이 축만으로 초당 한도를 먹는다. 반면 정배열·이격·점수를 다시 매기는 데 필요한 건
//  일봉 캐시와 시세 표뿐이라 REST가 0이다. 그래서 "누가 후보인가"(비싼 축)와
//  "그 중 누가 좋은가"(싼 축)의 주기를 분리한다.
//  종목은 id로 든다 — 응답·파일의 문자열 티커는 붓는 자리(take_*)에서 intern한다. [why D-112]
struct CandidateSet
{
    static constexpr uint32_t kNoSlot = UINT32_MAX;

    std::string date_yyyymmdd;
    std::time_t at = 0;
    std::vector<symbol::SymbolId> symbols;       // 등록 순서 = 일봉 점검 우선순위
    std::vector<std::string>      names;         // symbols와 같은 순서
    std::vector<uint32_t>         slot_of;       // 종목 id → symbols 자리. kNoSlot=미등록(옛 seen)
    std::vector<Market>           market;        // 종목 id → 시장. Unlisted=사전에 없음
    std::vector<symbol::SymbolId> market_listed; // 사전에 올라온 종목(전 종목 확장 축의 원천)
    bool have_market_map = false;
    int  etf_drop  = 0;
    int  reit_drop = 0;

    explicit CandidateSet(size_t capacity = 0) : slot_of(capacity, kNoSlot), market(capacity, Market::Unlisted) {}

    void reserve_symbol(symbol::SymbolId symbol);

    // 중복이면 false. 이름은 로그 라벨과 ScanResult.names에 쓴다. name은 sink — 값으로 받아 옮겨 넣는다.
    bool add(symbol::SymbolId symbol, std::string name);

    void set_market(symbol::SymbolId symbol, Market value);

    // [inv] 반환 뷰는 candidates 수명 안, names에 삽입이 없는 구간에서만 유효하다(재할당이 뷰를 끊는다).
    std::string_view name_of(symbol::SymbolId symbol) const;

    // market_map이 있는데도 사전에 없는 티커는 코스피·코스닥 보통주가 아니다(ETN 등).
    //  사전이 없는 구 파일에서는 태그 없는 후보를 KOSPI로 간주해 기존 동작을 유지한다.
    Market market_of(symbol::SymbolId symbol) const;
};

// 지수 게이트의 래치. 축(코스피·코스닥)마다 현재 차단 여부와 마지막 전환 시각을 들고 있는다.
//  재스캔 스레드가 유일한 호출자지만 g_candidate_mutex가 지키는 후보 풀과 같은 규약으로 뮤텍스를 둔다.
//  [inv] 프로세스 전역이라 슬리브 여럿이 같은 래치를 공유한다. 슬리브마다 임계가 다르면
//   먼저 발화한 쪽 판정이 나머지에도 걸린다 — 임계가 갈리는 순간 1회 경고한다. [why D-033]
struct IdxGateLatch
{
    bool risk_off = false;                              // [inv] 현재 차단 상태(래치된 값)
    bool primed = false;                           // [inv] since가 유효한가 — 첫 전환 전에는 false
    std::chrono::steady_clock::time_point since{}; // 마지막 전환 시각
    double seen_trip = 0.0;                        // 직전 호출이 준 차단 임계(공유 감지용)
    double seen_resume = 0.0;                      // 직전 호출이 준 재개 임계
    bool config_warned = false;                       // 설정 경고를 이미 냈나(도배 방지)
};

// 지수 게이트 래치를 지키는 뮤텍스. DevScale 래치(UniverseRiskGate.cpp)와 ITB 래치(UniverseItb.cpp)가 같이 쓴다.
extern std::mutex g_index_latch_mutex;

// 히스테리시스 한 축. 등락률이 trip 아래로 내려가면 차단, resume 위로 올라오면 재개하고,
//  그 사이 중립대에서는 직전 상태를 유지한다. 차단 임계 하나로 20초마다 다시 재던 옛 판정은
//  지수가 경계를 오갈 때 게이트도 같이 떨었다(2026-08-21 최소 2분 53초 간격 토글).
//  observed=false는 조회 실패다 — 판정도 타이머도 건드리지 않는다. 반환은 "지금 차단인가".
//  [why D-033]
bool latch_risk_off(IdxGateLatch& latch, double change, bool observed, double trip, double resume,
                    int dwell_sec, const char* label);

// 시장별 risk_off 게이트(2026-08-19 회의). 코스피 급락은 전이 회피를 위해 코스피·코스닥
//  신규진입 모두에 영향을 준다(코스닥은 하루 늦게 따라오는 전이 지연이 잦다).
//  코스닥 종목은 이중 AND — 코스피 정상 AND 코스닥 정상일 때만 통과.
struct MarketGate
{
    double kospi_change  = 0.0;
    double kosdaq_change = 0.0;   // [inv] kosdaq_enabled=false면 미관측이라 0.0 — 표시에 쓰지 않는다
    bool   kospi_observation  = false; // [inv] 이번 조회가 성공했나. false면 kospi_change는 의미 없다 [why D-033]
    bool   kosdaq_observation = false;
    bool   kospi_pass  = false;
    bool   kosdaq_pass = false;

    bool closed() const { return !kospi_pass && !kosdaq_pass; }

    // 시장 미상은 코스닥과 같은 보수 판정(닫혀 있으면 드롭).
    bool allows(Market market) const;
};

// 지수 등락률 조회 2콜. kosdaq_enabled=false면 코스닥 지수 조회조차 생략한다.
//  판정은 래치를 거친다(히스테리시스·최소 체류) — 시세 조회를 먼저 끝내고 락을 잡는다.
//  [lock-order] g_index_latch_mutex는 REST 호출 밖에서만 잡는다. g_candidate_mutex와 겹치지 않는다.
MarketGate build_market_gate(KisClient& kis, const DevScanCfg& config);

// 후보 합집합을 채운다. union_refresh_sec 안에 다시 불리면 수집을 통째로 건너뛰고
//  지난 집합을 그대로 쓴다 — 이 단계만 KIS REST 랭킹 3콜 + 업종 코드 수만큼(sector_codes, 250ms 간격)이고 이후 재판정은 0콜이다(D-028).
//  0이면 매 호출 새로 모은다(기존 동작).
void collect_candidates(KisClient& kis, const DevScanCfg& config, const std::string& date_yyyymmdd,
                        QuoteTable& quotes, CandidateSet& candidates, symbol::SymbolTable& symbols);

//  점수는 원자료를 바로 더하지 않는다. 추세·눌림·변동성은 단위도 일별 분산도 달라서 그대로
//   더하면 그날 우연히 많이 벌어진 축이 점수를 지배한다. 통과 집합 안에서 각각 z-score로
//   정규화하고 ±2σ에서 자른 뒤 가중합한다(스케일-프리 + 이상치 1종목 지배 차단).
struct Features
{
    symbol::SymbolId symbol;
    double           trend, pull, atr_percent, turnover, score; // atr_percent는 ATR(14)/종가(변동성 축)
};

struct LookupStats
{
    int looked_up = 0, aligned = 0, short_bars = 0, overext = 0;
    int fetched = 0, cache_hit = 0;
    int illiquid = 0;         // 거래대금 하한 미달로 버린 수
    int misaligned = 0;       // 정배열 조건 미충족으로 버린 수(진단용)
    int budget_skipped = 0;   // 일봉 조회 예산이 끝났고 캐시도 없어 판정 못 한 수
    long long rest_ms = 0;    // 계측: fetch_daily_lookup(REST 일봉) 안에서 보낸 시간 합. 실계좌 150ms, 모의 600ms 간격 sleep은 뺀 값
    long long wait_ms = 0;    // 계측: 그중 KIS 토큰버킷 대기 합 — 크면 다른 소비자와 경합
    long long sleep_ms = 0;   // 계측: 일봉 조회 사이에 실제로 쉰 간격 합(계좌 종류에 따라 150ms 또는 600ms씩)
};

// 2단: 정배열 프리필터 — 후보를 일봉으로 검사해 정배열=Y(≥20봉)만 통과시킨다.
//  데이터부족(신규상장 <20봉)은 여기서 자동 제외된다. 일봉 조회 비용은 align_lookup_max로
//  캡하되 캐시 히트는 예산을 쓰지 않는다. 정배열 규칙은 MaAlign.h의 quant::moving_average::aligned 하나를 전략과 같이 쓴다.
std::vector<Features> lookup_and_filter(KisClient& kis, const DevScanCfg& config, const std::string& date_yyyymmdd,
                                   const CandidateSet& candidates, const QuoteTable& quotes,
                                   const MarketGate& gate, LookupStats& statistics, symbol::SymbolTable& symbols);

// 2.5단: 횡단면 정규화로 종합 점수 하나를 만든다. 이 점수가 등록 순서(=진입 우선순위)와
//  종목별 비중 배수 두 가지를 모두 정한다.
//  [formula] S = weight_trend·z(추세) + weight_pull·z(-눌림) - weight_volume·z(ATR%) + weight_liquidity·z(log 거래대금).
//   ATR%는 ATR(14)/종가, 곧 변동성이다(설정 키 이름 score_w_vol의 vol은 거래량이 아니라 이것이다). 변동성은 뺀다 — 추세·눌림이 같다면 덜 흔들리는 쪽이 낫다.
//   거래대금은 더한다 — 같은 조건이면 두꺼운 쪽이 청산 슬리피지가 작다. 기본값 0(비활성)이다.
void score_cross_section(const DevScanCfg& config, std::vector<Features>& passed);

// 3단: 점수 내림차순으로 등록한다. score_top_n>0이면 상위 N만 남긴다.
//  절단이 없어도 정렬은 한다 — 등록 순서가 그대로 진입 우선순위라, 안 정렬하면
//  유니버스 파일 순서(시총·거래대금)가 우선순위를 먹는다.
//  동점은 티커 사전순으로 가른다(`ranks_before`, 순위 계산과 같은 규칙) — 같은 입력이면 늘 같은 종목이 잘린다.
ScanResult rank_and_truncate(const DevScanCfg& config, std::vector<Features>& passed, const CandidateSet& candidates,
                             const symbol::SymbolTable& symbols);

// 프리필터 risk_off — 기존 동작(후보 앞에서부터 max_register개). 점수는 없다.
ScanResult take_first_n(const DevScanCfg& config, const CandidateSet& candidates, const MarketGate& gate);
} // namespace universe::detail
