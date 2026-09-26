#include "universe/UniverseScanner.h"
#include "detail/Pipeline.h"
#include "utils/JsonNode.h"
#include "universe/MaAlign.h"
#include "universe/MarketBoard.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "utils/EtfFilter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace universe
{
using namespace detail;

namespace
{
// 거래일 YYYYMMDD — KST 고정(머신 TZ 무관). 이름은 호출부와 맞춰 둔다.
std::string local_ymd()
{
    return kst::date_yyyymmdd(std::time(nullptr));
}
} // namespace

// DeviationScale 유니버스 선정. 단계는 넷이고 비용이 다르다 — 후보 합집합 수집만 KIS REST를
//  쓰고(D-028로 주기 분리), 정배열·이격·점수 재판정은 일봉 캐시와 시세 표만 본다.
//  스캔 스레드에서만 부른다. 실패는 예외 대신 빈 목록으로 돌려준다.
ScanResult scan_devscale(KisClient& kis, const DevScanCfg& config, symbol::SymbolTable& symbols,
                         QuoteTable& quotes)
{
    // 계측(문항 2): 단계별 경과를 요약 로그에 붙인다 — "일봉조회=0인데 40초"가 어느 단계인지 가르기 위해.
    using scan_clock = std::chrono::steady_clock;
    const auto scan_start = scan_clock::now();
    auto ms_since = [](scan_clock::time_point from) -> long long
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(scan_clock::now() - from).count();
    };

    const std::string date_yyyymmdd = local_ymd();   // 일봉 캐시·후보 집합 캐시의 거래일 키
    g_lookup_cache.load_today(date_yyyymmdd, symbols); // 장중 재기동 시 일봉 재조회를 막는다

    // 장 전 데우기 스레드가 받아 둔 일봉을 옮긴다. 옮긴 게 있으면 파일에도 남겨 재기동 때 다시 받지 않는다.
    if (drain_daily_warm(date_yyyymmdd, symbols) > 0)
    {
        g_lookup_cache.save_today(date_yyyymmdd, symbols);
    }

    const std::shared_ptr<const BoardSnapshot> board =
        config.market_board ? MarketBoard::instance().snapshot() : nullptr;

    if (board)
    {
        load_quote_table(*board, quotes, symbols);
    }
    else
    {
        clear_quote_table(quotes, symbols);   // 시세판 꺼짐, 또는 첫 판 받기 전 — 랭킹 축 가격만 쓴다
    }

    const MarketGate gate = build_market_gate(kis, config);
    const long long gate_ms = ms_since(scan_start);   // 시세 표 적재 + 지수 조회(REST)

    if (gate.closed())
    {
        // 모든 시장이 위험회피 상태다. 후보 수집·일봉 조회를 전부 생략한다.
        LOG_WARN("[Main] DEVSCALE 스캔: 레짐 위험회피(코스피 " + std::to_string(gate.kospi_change * 100.0) +
                 "%" + (config.kosdaq_enabled ? ", 코스닥 " + std::to_string(gate.kosdaq_change * 100.0) + "%" : "") +
                 ") — 신규 유니버스 스킵");
        return {};
    }

    CandidateSet candidates(symbols.capacity());
    const auto   collect_start = scan_clock::now();
    collect_candidates(kis, config, date_yyyymmdd, quotes, candidates, symbols);
    const long long collect_ms = ms_since(collect_start);   // KIS 랭킹 REST + 파일 union

    if (!config.require_aligned)
    {
        return take_first_n(config, candidates, gate);
    }

    LookupStats statistics;
    const auto lookup_start = scan_clock::now();
    std::vector<Features> passed = lookup_and_filter(kis, config, date_yyyymmdd, candidates, quotes, gate, statistics, symbols);
    const long long lookup_ms = ms_since(lookup_start);
    const auto score_start = scan_clock::now();
    score_cross_section(config, passed);
    ScanResult out = rank_and_truncate(config, passed, candidates, symbols);
    const long long score_ms = ms_since(score_start);

    // 새로 받은 일봉이 있을 때만 파일을 갱신한다. 히트만 났으면 내용이 같다.
    const auto save_start = scan_clock::now();

    if (statistics.fetched > 0)
    {
        g_lookup_cache.save_today(date_yyyymmdd, symbols);
    }

    const long long save_ms = ms_since(save_start);

    LOG_INFO("[Main] DEVSCALE 정배열 프리필터: 후보=" + std::to_string(candidates.symbols.size()) +
             " ETF드롭=" + std::to_string(candidates.etf_drop) +
             " 리츠드롭=" + std::to_string(candidates.reit_drop) +
             " 검사=" + std::to_string(statistics.looked_up) +
             " (일봉조회=" + std::to_string(statistics.fetched) +
             " 캐시=" + std::to_string(statistics.cache_hit) + ")" +
             " 정배열=" + std::to_string(statistics.aligned) +
             " 역배열컷=" + std::to_string(statistics.misaligned) +
             " 데이터부족(<20봉)=" + std::to_string(statistics.short_bars) +
             " 과확장컷=" + std::to_string(statistics.overext) +
             " 거래대금미달=" + std::to_string(statistics.illiquid) +
             " 예산소진=" + std::to_string(statistics.budget_skipped) +
             " 등록=" + std::to_string(out.symbols.size()));
    // 단계별 경과 — 검사는 REST 시간과 버킷 대기를 따로 보여 "REST가 아니라면 어디서 새는지" 가른다.
    LOG_INFO("[Main] DEVSCALE 스캔 계측: 전체=" + std::to_string(ms_since(scan_start)) +
             "ms 게이트=" + std::to_string(gate_ms) + "ms 수집=" + std::to_string(collect_ms) +
             "ms 검사=" + std::to_string(lookup_ms) + "ms(REST=" + std::to_string(statistics.rest_ms) +
             "ms 버킷대기=" + std::to_string(statistics.wait_ms) + "ms 간격sleep=" +
             std::to_string(statistics.sleep_ms) +
             "ms) 점수·순위=" + std::to_string(score_ms) + "ms 캐시저장=" + std::to_string(save_ms) + "ms");
    return out;
}
} // namespace universe
