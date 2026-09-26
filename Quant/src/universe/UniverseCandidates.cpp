// 후보 합집합 수집 — KIS 랭킹·업종 순위·유니버스 파일·전 종목 확장 축을 한 집합으로 모은다. 스캔에서 KIS REST를
//  쓰는 단계는 이것 하나라 KIS 축만 union_refresh_sec 동안 재사용하고, 유니버스 파일은 다시 쓰일 때마다 읽는다.
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

// 캐시는 둘로 나눈다. KIS 축(랭킹 2개·업종 23콜, 약 6.5초)은 union_refresh_sec마다 새로 받고, 유니버스 파일 축은
//  파일이 다시 쓰일 때마다(감시견이 1분마다, D-142) 읽는다. 둘을 한 캐시에 묶으면 파일의 1분 재랭킹이
//  KIS 주기(120초)만큼 늦게 들어온다.
CandidateSet g_kis_axes_cache;
CandidateSet g_file_axis_cache;
std::filesystem::file_time_type g_file_axis_written{};
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

// data.go.kr 종목 목록 기반 거래대금 상위 유니버스 피드(시총 축은 D-146부터 0). KIS 30행캡·ETF 잠식을 우회한 개별주 깊은 집합이라
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

// 유니버스 파일 축을 붓되, 파일이 지난번 파싱 뒤로 다시 쓰이지 않았으면 그때 결과를 쓴다.
//  재스캔(20초)이 파일 갱신(1분)보다 잦아 세 번 중 두 번은 같은 내용을 다시 파싱하게 되기 때문이다.
//  candidates는 비어 있어야 한다 — 파일 축이 맨 앞 축이다.
void take_universe_file_cached(const DevScanCfg& config, CandidateSet& candidates, symbol::SymbolTable& symbols)
{
    std::error_code error;
    const auto written = config.universe_file.empty()
                             ? std::filesystem::file_time_type{}
                             : std::filesystem::last_write_time(config.universe_file, error);

    if (config.universe_file.empty() || error)
    {
        take_universe_file(config, candidates, symbols);   // 없을 때의 경고·스킵은 그쪽이 한다
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_candidate_mutex);

        if (written == g_file_axis_written && !g_file_axis_cache.symbols.empty())
        {
            candidates = g_file_axis_cache;   // 복사가 맞다 — 호출자가 이 위에 다른 축을 더 붓는다
            return;
        }
    }

    take_universe_file(config, candidates, symbols);
    std::lock_guard<std::mutex> lock(g_candidate_mutex);
    g_file_axis_cache   = candidates;   // 복사가 맞다 — 호출자가 candidates에 다른 축을 계속 붓는다
    g_file_axis_written = written;
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

// 거래대금 상위 축 — 시세 표(재스캔마다 새로 읽는 전 종목 장중 시세)에서 당일 누적 거래대금 상위
//  turnover_top_n종목을 붓는다. KIS 랭킹은 한 번에 30행, 가격 구간을 갈라도 60행이 끝이라 그보다 깊은
//  거래대금 순위는 여기서만 나온다. 추가 조회는 없다. 시가총액 축은 대형주가 거래 없이도 자리를
//  먹어 뺐다 [why D-146].
//  market_map이 있으면 코스피·코스닥 상장 종목만 본다(ETN·ETF 코드는 사전에 없다). 순위가 같으면
//  티커 순으로 자른다 — 같은 시세 파일이면 같은 집합이 나오게.
void take_turnover_top(const DevScanCfg& config, const QuoteTable& quotes, CandidateSet& candidates,
                       const symbol::SymbolTable& symbols)
{
    if (config.turnover_top_n <= 0)
    {
        return;
    }

    struct Ranked
    {
        double           value;
        symbol::Ticker   ticker;
        symbol::SymbolId symbol;
    };

    std::vector<Ranked> ranked;

    for (std::size_t index = 0; index < quotes.size(); ++index)
    {
        const symbol::SymbolId symbol = static_cast<symbol::SymbolId>(index);
        const MarketQuote& quote = quotes[index];

        if (quote.price <= 0.0 || quote.value <= 0.0 || quote.name.empty())
        {
            continue;
        }

        if (candidates.have_market_map &&
            (index >= candidates.market.size() || candidates.market[index] == Market::Unlisted))
        {
            continue;
        }

        if (quote.price < config.min_price || (config.max_price > 0.0 && quote.price > config.max_price))
        {
            continue;
        }

        if (quote.value < config.min_turnover)
        {
            continue;
        }

        ranked.push_back({quote.value, symbols.name(symbol), symbol});
    }

    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right)
    {
        if (left.value != right.value)
        {
            return left.value > right.value;
        }

        return left.ticker.view() < right.ticker.view();
    });

    const std::size_t before = candidates.symbols.size();
    int taken = 0;

    for (const Ranked& entry : ranked)
    {
        if (taken >= config.turnover_top_n)
        {
            break;
        }

        const std::string& name = quotes[entry.symbol].name;

        if (excluded_by_name(name, candidates.etf_drop, candidates.reit_drop))
        {
            continue;
        }

        ++taken;
        candidates.add(entry.symbol, name);
    }

    LOG_INFO("[Main] DEVSCALE 거래대금 상위 축: 상위 " + std::to_string(taken) + "종목 중 신규 " +
             std::to_string(candidates.symbols.size() - before) + " union (시세 " +
             std::to_string(ranked.size()) + "종목에서)");
}

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
    // 정배열 프리필터로 상당수가 탈락하므로 여기선 max_register로 자르지 않고 넓게 모은다.
    //  축 순서(파일 → 거래대금 상위 → KIS 랭킹 → 업종 → 전 종목)가 일봉 조회 우선순위라 캐시를 나눠도 이 순서로 합친다.
    take_universe_file_cached(config, candidates, symbols);
    take_turnover_top(config, quotes, candidates, symbols);

    CandidateSet kis_axes(symbols.capacity());
    long long age = -1;

    if (config.union_refresh_sec > 0)
    {
        std::lock_guard<std::mutex> lock(g_candidate_mutex);

        if (g_kis_axes_cache.date_yyyymmdd == date_yyyymmdd && !g_kis_axes_cache.symbols.empty() &&
            std::time(nullptr) - g_kis_axes_cache.at < config.union_refresh_sec)
        {
            kis_axes = g_kis_axes_cache;   // 복사가 맞다 — 캐시는 락 아래 남고 호출자는 락 밖에서 자기 사본을 쓴다
            age = static_cast<long long>(std::time(nullptr) - g_kis_axes_cache.at);
        }
    }

    if (age >= 0)
    {
        // 현재가·거래대금은 시세 표에서 방금 읽은 값을 쓴다. 랭킹 축이 실어오던
        //  스냅샷가는 재사용분에 없지만, 네이버 쪽이 더 최신이라 판정에는 그편이 낫다.
        LOG_INFO("[Main] DEVSCALE KIS 축 재사용: " + std::to_string(kis_axes.symbols.size()) +
                 "종목 (" + std::to_string(age) +
                 "초 전 수집, 갱신주기 " + std::to_string(config.union_refresh_sec) + "초)");
    }
    else
    {
        take_ranking(kis.fetch_value_ranking(config.value_top_n, "J", "3"), config, quotes, kis_axes, symbols);   // 거래대금 상위
        // 랭킹 TR은 축마다 상위 30행 고정(연속조회 불가)이라 정렬축을 하나 더 union해 집합을 넓힌다.
        //  거래증가율(1)은 대형주에 편중된 거래대금축과 겹침이 적어(중소형 모멘텀) 정배열 후보를 늘린다.
        take_ranking(kis.fetch_value_ranking(config.value_top_n, "J", "1"), config, quotes, kis_axes, symbols);   // 거래증가율 상위
        take_sector_ranking(kis, config, quotes, kis_axes, symbols);

        if (config.union_refresh_sec > 0)
        {
            std::lock_guard<std::mutex> lock(g_candidate_mutex);
            kis_axes.date_yyyymmdd = date_yyyymmdd;
            kis_axes.at = std::time(nullptr);
            g_kis_axes_cache = kis_axes;   // 복사가 맞다 — 아래에서 kis_axes를 계속 읽고 캐시는 다음 갱신까지 남는다
        }
    }

    for (std::size_t index = 0; index < kis_axes.symbols.size(); ++index)
    {
        candidates.add(kis_axes.symbols[index], std::move(kis_axes.names[index]));
    }

    candidates.etf_drop  += kis_axes.etf_drop;
    candidates.reit_drop += kis_axes.reit_drop;
    // 전 종목 확장은 REST가 없어 매번 다시 한다 — 방금 읽은 시세 표로 가격·이름 필터를 건다.
    take_full_market(config, quotes, candidates, symbols);
}
} // namespace universe::detail
