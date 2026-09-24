// 후보 합집합 수집 — KIS 랭킹·업종 순위·유니버스 파일·전 종목 확장 축을 한 집합으로 모은다. 스캔에서 KIS REST를
//  쓰는 단계는 이것 하나라 union_refresh_sec 동안 지난 집합을 그대로 재사용한다.
//  스캔 스레드 전용. [why D-028]

#include "detail/Pipeline.h"
#include "utils/JsonNode.h"
#include "universe/MaAlign.h"
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

namespace universe::detail
{
namespace
{
// 업종 등락률 순위를 한 콜씩 낼 때 사이에 두는 시간. 26개 업종이면 6.5초가 된다. 업종 축은 후보 합집합
//  갱신 주기(union_refresh_sec)마다 돌아 그에 비하면 작다. 값을 줄이면 같은 시세 키를 쓰는 대시보드 조회와 겹쳐
//  초당 한도에 걸린다(09-23: 100ms 일 때 이 축에서만 되보냄 452건, 전날 0건).
constexpr int kSectorCallIntervalMs = 250;

CandidateSet g_candidate_cache;
std::mutex   g_candidate_mutex;

// ETF/ETN·리츠 배제(개별주만). ETF는 브랜드 접두사(경계검사)∪상품 토큰, 리츠는 접미사·정확일치다.
//  KIS 축은 KisClient에서 이미 걸러지지만 data.go.kr 축과 한 규칙으로 이중 차단한다.
//  리츠를 따로 보는 이유는 배당·NAV로 움직여 일봉 프리필터를 그대로 통과하기 때문이다(334890 유입).
bool excluded_by_name(const std::string& name, int& etf_drop, int& reit_drop)
{
    static const std::vector<std::string> kEtfPrefixes =
        etf_filter::load_list("etf_prefixes.json", etf_filter::default_prefixes());
    static const std::vector<std::string> kEtfTokens =
        etf_filter::load_list("etf_name_tokens.json", etf_filter::default_tokens());
    static const std::vector<std::string> kReitSuffixes =
        etf_filter::load_list("reit_name_suffixes.json", etf_filter::default_reit_suffixes());
    static const std::vector<std::string> kReitExacts =
        etf_filter::load_list("reit_names.json", etf_filter::default_reit_exacts());

    if (etf_filter::is_etf_like(name, kEtfPrefixes, kEtfTokens))
    {
        ++etf_drop;
        return true;
    }

    if (etf_filter::is_reit_like(name, kReitSuffixes, kReitExacts))
    {
        ++reit_drop;
        return true;
    }

    return false;
}

// 랭킹 응답을 후보 집합에 붓는다. 스냅샷가는 중복분에도 반영한다 — 표의 현재가를 최신으로 둔다.
void take_ranking(const std::vector<KisClient::RankingStock>& rank, const DevScanCfg& config, QuoteTable& quotes,
                  CandidateSet& candidates, symbol::SymbolTable& symbols)
{
    for (const auto& ranked : rank)
    {
        if (ranked.price < config.min_price)
        {
            continue;
        }

        if (config.max_price > 0.0 && ranked.price > config.max_price)
        {
            continue;
        }

        if (excluded_by_name(ranked.name, candidates.etf_drop, candidates.reit_drop))
        {
            continue;
        }

        const symbol::SymbolId symbol = symbols.intern(ranked.ticker); // REST 응답의 문자열 티커 — 여기서 id가 된다

        if (symbol == symbol::kNone)
        {
            continue;
        }

        if (ranked.price > 0.0)
        {
            quotes[symbol].price = ranked.price;
        }

        candidates.add(symbol, ranked.name);
    }
}

// data.go.kr 시총∪거래대금 유니버스 피드. KIS 30행캡·ETF 잠식을 우회한 개별주 깊은 집합이라
//  후보 집합 맨 앞에 넣어 일봉 조회 우선순위를 준다. 파일이 없으면 조용히 스킵한다(하위호환).
//  전종목 코드→시장 사전(market_map)을 top-N보다 먼저 적재해, KIS 랭킹축 티커의 시장도
//  해석되게 한다 — 없으면 kosdaq_enabled 게이트가 그쪽으로 샌다.
void take_universe_file(const DevScanCfg& config, CandidateSet& candidates, symbol::SymbolTable& symbols)
{
    if (config.universe_file.empty())
    {
        return;
    }

    std::ifstream file(config.universe_file);

    if (!file)
    {
        LOG_WARN("[Main] DEVSCALE 유니버스 파일 없음(" + config.universe_file +
                 ") — data.go.kr 축 스킵, KIS 랭킹 축만 사용");
        return;
    }

    try
    {
        nlohmann::json document;
        file >> document;
        const std::string basDt = document.value("basDt", std::string());

        if (document.contains("market_map") && document["market_map"].is_object())
        {
            for (auto iterator = document["market_map"].begin(); iterator != document["market_map"].end(); ++iterator)
            {
                // 사전은 코스피·코스닥 보통주 6자리만 — ETN 등 다른 길이는 종목 테이블에 넣지 않는다.
                if (!iterator.value().is_string() || iterator.key().size() != symbol::kKoreanTickerLength)
                {
                    continue;
                }

                const symbol::SymbolId symbol = symbols.intern(iterator.key());

                if (symbol != symbol::kNone)
                {
                    candidates.set_market(symbol, market_from_text(iterator.value().get_ref<const std::string&>()));
                }
            }

            candidates.have_market_map = !candidates.market_listed.empty();
        }

        const nlohmann::json& array = jsonx::array_or_empty(document, "universe");
        int added_file = 0, duplicate = 0;

        for (const auto& element : array)
        {
            const std::string ticker = element.value("ticker", std::string());

            if (ticker.empty())
            {
                continue;
            }

            std::string name = element.value("name", std::string());

            if (excluded_by_name(name, candidates.etf_drop, candidates.reit_drop))
            {
                continue;
            }

            const double price = element.value("close", 0.0);

            // close(0=미제공)면 가격필터는 뒤 정배열 프리필터의 일봉이 대신 검증한다.
            if (price > 0.0 && price < config.min_price)
            {
                continue;
            }

            if (config.max_price > 0.0 && price > config.max_price)
            {
                continue;
            }

            const symbol::SymbolId symbol = symbols.intern(ticker); // 파일의 문자열 티커 — 여기서 id가 된다

            if (symbol == symbol::kNone)
            {
                continue;
            }

            if (!candidates.add(symbol, std::move(name)))
            {
                ++duplicate;
                continue;
            }

            candidates.set_market(symbol, market_from_text(element.value("market", std::string())));   // 시장별 risk_off 게이트용
            ++added_file;
        }

        LOG_INFO("[Main] DEVSCALE data.go.kr 축(기준일 " + basDt + "): 파일 " +
                 std::to_string(array.size()) + "종목 → 신규 " + std::to_string(added_file) +
                 " union (중복 " + std::to_string(duplicate) + ")");
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[Main] DEVSCALE 유니버스 파일 파싱 실패(" + config.universe_file +
                 "): " + std::string(exception.what()) + " — data.go.kr 축 스킵");
    }
}

// 업종 등락률 축 — 다른 축과 data.go.kr 축이 전부 전일 이전 상태를 보는 것과 달리 이 축만
//  장중을 본다. 등락률 내림차순이라 상위 N행이 곧 지금 강한 종목이고, 업종을 순회하므로
//  한 섹터가 집합을 독식하지 않는다. 정배열·과확장 판정은 뒤 프리필터가 그대로 한다.
void take_sector_ranking(KisClient& kis, const DevScanCfg& config, QuoteTable& quotes, CandidateSet& candidates,
                         symbol::SymbolTable& symbols)
{
    if (config.sector_codes.empty())
    {
        return;
    }

    const std::size_t before = candidates.symbols.size();
    int sec_ok = 0, sec_weak = 0;

    for (const auto& sector_code : config.sector_codes)
    {
        auto rows = kis.fetch_sector_ranking(sector_code, config.sector_top_n);
        // 26콜을 쉬지 않고 내면 8.8콜/s로 나가 문서상 한도 20/s의 절반을 이 축 하나가
        //  버스트로 먹는다(09-08: ranking/fluctuation HTTP 500 70건). 지금은 업종 수 × 250ms
        //  (kSectorCallIntervalMs)라 26개면 약 6.5초다. 합집합 갱신 주기(union_refresh_sec)에 비하면 작다.
        //  100ms 로는 모자랐다 — 축마다 따로 쉬는 방식의 한계라, 요청 예산을 한곳에서 재는 것이 근본이다.
        std::this_thread::sleep_for(std::chrono::milliseconds(kSectorCallIntervalMs));

        if (rows.empty())
        {
            continue;
        }

        ++sec_ok;
        // 약세 행은 제자리에서 걷어낸다 — 통과 행을 새 벡터로 베끼지 않는다.
        std::erase_if(rows, [&](const KisClient::RankingStock& row)
        {
            if (row.change_rate < config.sector_min_change)
            {
                ++sec_weak;
                return true;
            }

            return false;
        });
        take_ranking(rows, config, quotes, candidates, symbols);
    }

    LOG_INFO("[Main] DEVSCALE 업종 등락률 축: " + std::to_string(sec_ok) + "/" +
             std::to_string(config.sector_codes.size()) + "업종 응답, 신규 " +
             std::to_string(candidates.symbols.size() - before) + "종목 union (약세컷 " +
             std::to_string(sec_weak) + ")");
}

// 전 종목 확장 — market_map(코스피+코스닥 전체)을 후보로 붓는다. 종목명은 시세 표에서
//  가져와 ETF·리츠 필터를 그대로 적용한다(이름이 없으면 버린다).
//  티커 문자열 순으로 순회한다. 종목 id는 intern 순서라 실행마다 달라지고, 그 순서로 돌면 같은 입력에서도
//  align_lookup_max로 잘리는 지점이 함께 바뀌어 유니버스가 재현되지 않는다. Ticker는 고정 길이라 비교가 싸다.
void take_full_market(const DevScanCfg& config, const QuoteTable& quotes, CandidateSet& candidates,
                      const symbol::SymbolTable& symbols)
{
    if (!config.full_market || !candidates.have_market_map)
    {
        return;
    }

    const std::size_t before_fm = candidates.symbols.size();
    int no_name = 0;
    std::vector<std::pair<symbol::Ticker, symbol::SymbolId>> listed;
    listed.reserve(candidates.market_listed.size());

    for (const symbol::SymbolId symbol : candidates.market_listed)
    {
        listed.emplace_back(symbols.name(symbol), symbol);
    }

    std::sort(listed.begin(), listed.end(),
              [](const auto& entry_a, const auto& entry_b) { return entry_a.first.view() < entry_b.first.view(); });

    for (const auto& [ticker, symbol] : listed)
    {
        const MarketQuote& quote = quotes[symbol];

        if (quote.price <= 0.0 || quote.name.empty())
        {
            ++no_name;
            continue;
        }

        if (excluded_by_name(quote.name, candidates.etf_drop, candidates.reit_drop))
        {
            continue;
        }

        if (quote.price < config.min_price)
        {
            continue;
        }

        if (config.max_price > 0.0 && quote.price > config.max_price)
        {
            continue;
        }

        candidates.add(symbol, quote.name);
    }

    LOG_INFO("[Main] 전 종목 확장: 신규 " + std::to_string(candidates.symbols.size() - before_fm) +
             "종목 union (시세없음 " + std::to_string(no_name) + ", 총 후보 " +
             std::to_string(candidates.symbols.size()) + ")");
}
} // namespace

Market market_from_text(std::string_view text)
{
    if (text == "KOSPI")
    {
        return Market::Kospi;
    }

    if (text == "KOSDAQ")
    {
        return Market::Kosdaq;
    }

    return Market::Unknown;
}

void CandidateSet::reserve_symbol(symbol::SymbolId symbol)
{
    if (symbol >= slot_of.size())
    {
        slot_of.resize(static_cast<size_t>(symbol) + 1, kNoSlot);
        market.resize(static_cast<size_t>(symbol) + 1, Market::Unlisted);
    }
}

bool CandidateSet::add(symbol::SymbolId symbol, std::string name)
{
    reserve_symbol(symbol);

    if (slot_of[symbol] != kNoSlot)
    {
        return false;
    }

    slot_of[symbol] = static_cast<uint32_t>(symbols.size());
    symbols.push_back(symbol);
    names.push_back(std::move(name));
    return true;
}

void CandidateSet::set_market(symbol::SymbolId symbol, Market value)
{
    reserve_symbol(symbol);

    if (market[symbol] == Market::Unlisted)
    {
        market_listed.push_back(symbol);
    }

    market[symbol] = value;
}

std::string_view CandidateSet::name_of(symbol::SymbolId symbol) const
{
    if (symbol >= slot_of.size() || slot_of[symbol] == kNoSlot)
    {
        return {};
    }

    return names[slot_of[symbol]];
}

Market CandidateSet::market_of(symbol::SymbolId symbol) const
{
    if (symbol < market.size() && market[symbol] != Market::Unlisted)
    {
        return market[symbol];
    }

    return have_market_map ? Market::Unknown : Market::Kospi;
}

void collect_candidates(KisClient& kis, const DevScanCfg& config, const std::string& date_yyyymmdd,
                        QuoteTable& quotes, CandidateSet& candidates, symbol::SymbolTable& symbols)
{
    if (config.union_refresh_sec > 0)
    {
        long long age = -1;
        {
            std::lock_guard<std::mutex> lock(g_candidate_mutex);

            if (g_candidate_cache.date_yyyymmdd == date_yyyymmdd && !g_candidate_cache.symbols.empty() &&
                std::time(nullptr) - g_candidate_cache.at < config.union_refresh_sec)
            {
                candidates = g_candidate_cache;   // 복사가 맞다 — 캐시는 락 아래 남고 호출자는 락 밖에서 자기 사본을 쓴다
                age  = static_cast<long long>(std::time(nullptr) - g_candidate_cache.at);
            }
        }

        if (age >= 0)
        {
            // 현재가·거래대금은 시세 표에서 방금 읽은 값을 쓴다. 랭킹 축이 실어오던
            //  스냅샷가는 재사용분에 없지만, 네이버 쪽이 더 최신이라 판정에는 그편이 낫다.
            LOG_INFO("[Main] DEVSCALE 후보 합집합 재사용: " + std::to_string(candidates.symbols.size()) +
                     "종목 (" + std::to_string(age) +
                     "초 전 수집, 갱신주기 " + std::to_string(config.union_refresh_sec) + "초)");
            return;
        }
    }

    // 정배열 프리필터로 상당수가 탈락하므로 여기선 max_register로 자르지 않고 넓게 모은다.
    take_universe_file(config, candidates, symbols);
    take_ranking(kis.fetch_kr_ranking(config.scan_top_n, "J"), config, quotes, candidates, symbols);            // 시총 상위
    take_ranking(kis.fetch_value_ranking(config.value_top_n, "J", "3"), config, quotes, candidates, symbols);   // 거래대금 상위
    // 랭킹 TR은 축마다 상위 30행 고정(연속조회 불가)이라 정렬축을 하나 더 union해 집합을 넓힌다.
    //  거래증가율(1)은 대형주에 편중된 시총·거래대금축과 겹침이 적어(중소형 모멘텀) 정배열 후보를 늘린다.
    take_ranking(kis.fetch_value_ranking(config.value_top_n, "J", "1"), config, quotes, candidates, symbols);   // 거래증가율 상위
    take_sector_ranking(kis, config, quotes, candidates, symbols);
    take_full_market(config, quotes, candidates, symbols);

    if (config.union_refresh_sec > 0)
    {
        std::lock_guard<std::mutex> lock(g_candidate_mutex);
        candidates.date_yyyymmdd = date_yyyymmdd;
        candidates.at  = std::time(nullptr);
        g_candidate_cache   = candidates;   // 복사가 맞다 — 호출자가 candidates를 계속 쓰고 캐시는 다음 재스캔까지 남는다
    }
}
} // namespace universe::detail
