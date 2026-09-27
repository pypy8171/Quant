// 후보 합집합 수집 — 시세판 재랭킹(없으면 유니버스 파일)·거래대금·거래증가율·등락률 상위·전 종목 확장 축을 한 집합으로
//  모은다. 전부 시세판 표와 일봉 캐시만 보므로 KIS 조회가 없다(D-153). 스캔 스레드 전용. [why D-028]

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
#include <memory>
#include <optional>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace universe::detail
{
namespace
{
// 파일 축 캐시 — 시세판 결과나 유니버스 파일이 바뀌지 않았으면 지난번 사본을 쓴다.
CandidateSet g_file_axis_cache;
std::filesystem::file_time_type g_file_axis_written{};
std::shared_ptr<const RankedUniverse> g_board_axis_source; // g_file_axis_cache가 시세판의 어느 결과로 만든 것인가
std::mutex   g_candidate_mutex;

// ETF/ETN·리츠 배제(개별주만). ETF는 브랜드 접두사(경계검사)∪상품 토큰, 리츠는 접미사·정확일치다.
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

// 시세 표에서 metric이 큰 순으로 count종목을 candidates에 붓는다. metric이 값을 안 주면 그 종목은 순위에 없다.
//  가격·거래대금 하한·상장 여부·ETF·리츠 필터를 같이 건다. 값이 같으면 티커 순으로 자른다 — 같은 시세면 같은 집합이
//  나오게. 순위에 오른 종목 수를 돌려준다(로그용).
template <typename Metric>
std::size_t take_top_by(const DevScanCfg& config, const QuoteTable& quotes, int count, CandidateSet& candidates,
                        const symbol::SymbolTable& symbols, Metric metric)
{
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

        const std::optional<double> value = metric(symbol, quote);

        if (value)
        {
            ranked.push_back({*value, symbols.name(symbol), symbol});
        }
    }

    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right)
    {
        if (left.value != right.value)
        {
            return left.value > right.value;
        }

        return left.ticker.view() < right.ticker.view();
    });

    int taken = 0;

    for (const Ranked& entry : ranked)
    {
        if (taken >= count)
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

    return ranked.size();
}

// 유니버스 한 줄을 후보에 붓는다 — 파일 축과 시세판 축이 같은 규칙(ETF·리츠 이름 필터·가격 구간)을 쓴다.
//  결과: 1=새로 넣음, 0=걸러짐, -1=중복.
int add_universe_entry(const DevScanCfg& config, const std::string& ticker, std::string name, double price,
                       std::string_view market, CandidateSet& candidates, symbol::SymbolTable& symbols)
{
    if (ticker.empty() || excluded_by_name(name, candidates.etf_drop, candidates.reit_drop))
    {
        return 0;
    }

    // close(0=미제공)면 가격필터는 뒤 정배열 프리필터의 일봉이 대신 검증한다.
    if (price > 0.0 && price < config.min_price)
    {
        return 0;
    }

    if (config.max_price > 0.0 && price > config.max_price)
    {
        return 0;
    }

    const symbol::SymbolId symbol = symbols.intern(ticker); // 문자열 티커 — 여기서 id가 된다

    if (symbol == symbol::kNone)
    {
        return 0;
    }

    if (!candidates.add(symbol, std::move(name)))
    {
        return -1;
    }

    candidates.set_market(symbol, market_from_text(market));   // 시장별 risk_off 게이트용
    return 1;
}

// 유니버스 파일 축 — 거래대금 상위 종목 파일(시총 축은 D-146부터 0). `market_board`면 엔진 안 시세판이 네이버 시세로
//  1분마다 쓰고(D-147), 끄면 universe_feed.py가 쓴다. 개별주 깊은 집합이라 후보 집합 맨 앞에 넣어 일봉 조회
//  우선순위를 준다. 파일이 없으면 경고하고 건너뛴다.
//  전종목 코드→시장 사전(market_map)을 top-N보다 먼저 적재해, 뒤 축(거래대금·거래증가율·등락률) 종목의 시장도
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
                 ") — 유니버스 파일 축 스킵");
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
            const int outcome = add_universe_entry(config, element.value("ticker", std::string()),
                                                   element.value("name", std::string()), element.value("close", 0.0),
                                                   element.value("market", std::string()), candidates, symbols);
            added_file += outcome > 0 ? 1 : 0;
            duplicate  += outcome < 0 ? 1 : 0;
        }

        LOG_INFO("[Main] DEVSCALE 유니버스 파일 축(기준일 " + basDt + "): 파일 " +
                 std::to_string(array.size()) + "종목 → 신규 " + std::to_string(added_file) +
                 " union (중복 " + std::to_string(duplicate) + ")");
    }
    catch (const std::exception& exception)
    {
        LOG_WARN("[Main] DEVSCALE 유니버스 파일 파싱 실패(" + config.universe_file +
                 "): " + std::string(exception.what()) + " — 유니버스 파일 축 스킵");
    }
}

// 시세판 재랭킹 축 — 파일 축과 같은 자리·같은 규칙이다. 코드→시장 사전은 시세판의 전 종목 목록이다. [why D-147]
void take_board_axis(const DevScanCfg& config, const RankedUniverse& ranked, CandidateSet& candidates,
                     symbol::SymbolTable& symbols)
{
    for (const ListedStock& listed : ranked.listing)
    {
        const symbol::SymbolId symbol = symbols.intern(listed.code);

        if (symbol != symbol::kNone)
        {
            candidates.set_market(symbol, market_from_text(listed.market));
        }
    }

    candidates.have_market_map = !candidates.market_listed.empty();
    int added = 0, duplicate = 0;

    for (const RankedStock& stock : ranked.stocks)
    {
        const int outcome = add_universe_entry(config, stock.code, stock.name, stock.close, stock.market, candidates, symbols);
        added     += outcome > 0 ? 1 : 0;
        duplicate += outcome < 0 ? 1 : 0;
    }

    LOG_INFO("[Main] DEVSCALE 시세판 재랭킹 축(" + kst::hhmmss(ranked.ranked_at) + " 산출): " +
             std::to_string(ranked.stocks.size()) + "종목 → 신규 " + std::to_string(added) + " union (중복 " +
             std::to_string(duplicate) + ")");
}

// 유니버스 파일 축을 붓되, 파일이 지난번 파싱 뒤로 다시 쓰이지 않았으면 그때 결과를 쓴다.
//  재스캔(20초)이 파일 갱신(1분)보다 잦아 세 번 중 두 번은 같은 내용을 다시 파싱하게 되기 때문이다.
//  candidates는 비어 있어야 한다 — 파일 축이 맨 앞 축이다.
void take_universe_file_cached(const DevScanCfg& config, CandidateSet& candidates, symbol::SymbolTable& symbols)
{
    // 시세판이 켜져 있고 한 번이라도 뽑았으면 그 결과를 쓴다. 결과가 같은 판이면(1분에 한 번 바뀐다) 지난번 사본을 쓴다.
    //  아직 못 뽑았으면(장 전 기동 직후) 아래 파일 축으로 간다 — 전날 시세판이 써 둔 파일이다.
    if (config.market_board)
    {
        const std::shared_ptr<const RankedUniverse> ranked = MarketBoard::instance().ranked();

        if (ranked)
        {
            {
                std::lock_guard<std::mutex> lock(g_candidate_mutex);

                if (ranked == g_board_axis_source && !g_file_axis_cache.symbols.empty())
                {
                    candidates = g_file_axis_cache;   // 복사가 맞다 — 호출자가 이 위에 다른 축을 더 붓는다
                    return;
                }
            }

            take_board_axis(config, *ranked, candidates, symbols);
            std::lock_guard<std::mutex> lock(g_candidate_mutex);
            g_file_axis_cache   = candidates;   // 복사가 맞다 — 호출자가 candidates에 다른 축을 계속 붓는다
            g_board_axis_source = ranked;
            g_file_axis_written = {};
            return;
        }
    }

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
    g_board_axis_source.reset();
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
              [](const auto& entry_a, const auto& entry_b)
              {
                  return entry_a.first.view() < entry_b.first.view();
              });

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
//  turnover_top_n종목을 붓는다. 추가 조회는 없다. 시가총액 축은 대형주가 거래 없이도 자리를 먹어 뺐다 [why D-146].
//  market_map이 있으면 코스피·코스닥 상장 종목만 본다(ETN·ETF 코드는 사전에 없다).
void take_turnover_top(const DevScanCfg& config, const QuoteTable& quotes, CandidateSet& candidates,
                       const symbol::SymbolTable& symbols)
{
    if (config.turnover_top_n <= 0)
    {
        return;
    }

    const std::size_t before = candidates.symbols.size();
    const std::size_t ranked = take_top_by(config, quotes, config.turnover_top_n, candidates, symbols,
                                           [](symbol::SymbolId, const MarketQuote& quote) -> std::optional<double>
                                           {
                                               return quote.value;
                                           });
    LOG_INFO("[Main] DEVSCALE 거래대금 상위 축: 신규 " + std::to_string(candidates.symbols.size() - before) +
             " union (시세 " + std::to_string(ranked) + "종목에서)");
}

// 거래증가율 상위 축 — 당일 누적 거래량 ÷ 전일 거래량. 옛 KIS 거래증가율 순위(30행 상한)를 대신한다.
//  전일 거래량은 일봉 캐시에서 읽으므로 장 전 데우기나 지난 스캔이 일봉을 받아 둔 종목만 순위에 오른다 [why D-153].
void take_volume_surge(const DevScanCfg& config, const QuoteTable& quotes, const std::string& date_yyyymmdd,
                       CandidateSet& candidates, const symbol::SymbolTable& symbols)
{
    if (config.value_top_n <= 0)
    {
        return;
    }

    const std::size_t before = candidates.symbols.size();
    const std::size_t ranked = take_top_by(config, quotes, config.value_top_n, candidates, symbols,
                                           [&date_yyyymmdd](symbol::SymbolId symbol, const MarketQuote& quote)
                                               -> std::optional<double>
                                           {
                                               const double previous = g_lookup_cache.previous_volume(symbol, date_yyyymmdd);

                                               if (previous <= 0.0 || quote.volume <= 0.0)
                                               {
                                                   return std::nullopt;
                                               }

                                               return quote.volume / previous;
                                           });
    LOG_INFO("[Main] DEVSCALE 거래증가율 상위 축: 신규 " + std::to_string(candidates.symbols.size() - before) +
             " union (전일 거래량이 있는 " + std::to_string(ranked) + "종목에서)");
}

// 등락률 상위 축 — 지금 강한 종목. 옛 KIS 업종 등락률 축(업종 26개 × 10행, 조회 26건)을 대신한다.
//  시세판이 전 종목 등락률을 주므로 업종을 돌 필요가 없다. change_min_percent 미만은 넣지 않는다 [why D-029·D-153].
void take_change_top(const DevScanCfg& config, const QuoteTable& quotes, CandidateSet& candidates,
                     const symbol::SymbolTable& symbols)
{
    if (config.change_top_n <= 0)
    {
        return;
    }

    const std::size_t before = candidates.symbols.size();
    const std::size_t ranked = take_top_by(config, quotes, config.change_top_n, candidates, symbols,
                                           [&config](symbol::SymbolId, const MarketQuote& quote) -> std::optional<double>
                                           {
                                               if (quote.change_percent < config.change_min_percent)
                                               {
                                                   return std::nullopt;
                                               }

                                               return quote.change_percent;
                                           });
    LOG_INFO("[Main] DEVSCALE 등락률 상위 축: 신규 " + std::to_string(candidates.symbols.size() - before) +
             " union (" + std::to_string(config.change_min_percent) + "% 이상 " + std::to_string(ranked) + "종목에서)");
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

void collect_candidates(const DevScanCfg& config, const std::string& date_yyyymmdd, const QuoteTable& quotes,
                        CandidateSet& candidates, symbol::SymbolTable& symbols)
{
    // 정배열 프리필터로 상당수가 탈락하므로 여기선 max_register로 자르지 않고 넓게 모은다.
    //  축 순서(재랭킹·파일 → 거래대금 → 거래증가율 → 등락률 → 전 종목)가 일봉 조회 우선순위다.
    take_universe_file_cached(config, candidates, symbols);
    take_turnover_top(config, quotes, candidates, symbols);
    take_volume_surge(config, quotes, date_yyyymmdd, candidates, symbols);
    take_change_top(config, quotes, candidates, symbols);
    take_full_market(config, quotes, candidates, symbols);
}
} // namespace universe::detail
