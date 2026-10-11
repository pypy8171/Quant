// 전 종목 시세판의 응답 읽기 — 네이버 시총순 목록 한 쪽·시세 폴링 본문을 종목 줄로 바꾼다. 네트워크·파일 없음.
//  선언은 universe/MarketBoard.h. 스레드: 시세판 스레드(MarketBoard::refresh_listing·sweep)와 테스트가 부른다.
//  상태 없음. [why D-147]
#include "universe/MarketBoard.h"
#include "core/SymbolTable.h"

#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace universe
{
namespace
{

// Raw 필드는 "5504265000000"처럼 숫자만 든 문자열이다. 숫자로 오는 경우도 받아 둔다. 못 읽으면 0.
// [wire] 근거: 2026-09-27 실측 응답(polling.finance.naver.com, 공식 문서 없음) — "closePriceRaw":"286500",
//  "accumulatedTradingValueRaw":"5504265000000" 처럼 모두 따옴표 친 문자열로 왔다.
double raw_number(const nlohmann::json& row, const char* key)
{
    const auto found = row.find(key);

    if (found == row.end())
    {
        return 0.0;
    }

    if (found->is_number())
    {
        return found->get<double>();
    }

    if (!found->is_string())
    {
        return 0.0;
    }

    const std::string& text = found->get_ref<const std::string&>();
    char*              end  = nullptr;
    const double       value = std::strtod(text.c_str(), &end);
    return (end != text.c_str() && std::isfinite(value)) ? value : 0.0;
}

std::string text_field(const nlohmann::json& row, const char* key)
{
    const auto found = row.find(key);
    return (found != row.end() && found->is_string()) ? found->get<std::string>() : std::string();
}

// 표시용 숫자("310,500")를 읽는다. 시간외 칸에는 가격 Raw 필드가 없어 이것을 쓴다. 못 읽으면 0.
// [wire] 근거: 2026-10-01 실측 응답 — overMarketPriceInfo.overPrice "310,500", overPriceRaw 칸은 없다.
double comma_number(const nlohmann::json& row, const char* key)
{
    std::string text = text_field(row, key);
    std::erase(text, ',');
    char*        end   = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    return (!text.empty() && end != text.c_str() && std::isfinite(value)) ? value : 0.0;
}

// 정규장 전에는 맨 위(KRX) 칸의 누적 거래대금이 빈 문자열이라 0으로 읽힌다. 그때 NXT 프리마켓 칸에 거래가 있으면
//  가격·등락률·거래량·거래대금을 그 칸 값으로 바꾼다. KRX 값이 있으면(정규장 중·마감 뒤) 손대지 않는다.
//  애프터마켓(AFTER_MARKET) 칸은 쓰지 않는다 — 그때는 KRX 칸에 정규장 값이 있다.
// [wire] 근거: 2026-10-01 08:56 실측(005490) — marketStatus "PREOPEN", 맨 위 accumulatedTradingValueRaw "",
//  overMarketPriceInfo.tradingSessionType "PRE_MARKET"·overPrice "310,500"·fluctuationsRatio "1.47"·
//  accumulatedTradingValueRaw "8749000000". 같은 날 09:13 응답은 tradingSessionType "REGULAR_MARKET". 공식 문서 없음.
void use_premarket_when_regular_empty(const nlohmann::json& row, BoardQuote& quote)
{
    if (quote.value > 0.0)
    {
        return;
    }

    const auto over = row.find("overMarketPriceInfo");

    if (over == row.end() || !over->is_object() || text_field(*over, "tradingSessionType") != "PRE_MARKET")
    {
        return;
    }

    const double over_price = comma_number(*over, "overPrice");
    const double over_value = raw_number(*over, "accumulatedTradingValueRaw");

    if (over_price <= 0.0 || over_value <= 0.0)
    {
        return; // 프리마켓에서 거래가 없던 종목 — KRX 값(전일 종가, 거래대금 0)을 그대로 둔다
    }

    // 등락률이 부호 없이 올 때를 대비해 방향 코드(4 하한·5 하락)로 부호를 붙인다(프로토타입
    //  research/studies/27_premarket_leaders/premarket_leaders.py signed_ratio와 같은 규칙).
    double     change    = comma_number(*over, "fluctuationsRatio");
    const auto direction = over->find("compareToPreviousPrice");

    if (change > 0.0 && direction != over->end() && direction->is_object())
    {
        const std::string code = text_field(*direction, "code");

        if (code == "4" || code == "5")
        {
            change = -change;
        }
    }

    quote.price          = over_price;
    quote.value          = over_value;
    quote.volume         = raw_number(*over, "accumulatedTradingVolumeRaw");
    quote.change_percent = change;
    quote.premarket      = true;
}

} // namespace

std::vector<ListedStock> parse_listing_page(std::string_view body, const std::string& market, int& total_count)
{
    std::vector<ListedStock> listed;
    total_count = -1;
    const nlohmann::json document = nlohmann::json::parse(body, nullptr, false);

    if (!document.is_object())
    {
        return listed;
    }

    const auto total = document.find("totalCount");

    if (total != document.end() && total->is_number())
    {
        total_count = total->get<int>();
    }

    const auto stocks = document.find("stocks");

    if (stocks == document.end() || !stocks->is_array())
    {
        return listed;
    }

    listed.reserve(stocks->size());

    for (const nlohmann::json& row : *stocks)
    {
        // ETF·ETN은 여기서 뺀다 — 옛 파이썬 피드가 data.go.kr 주식 시세(개별주만)를 목록으로 쓴 것과 같은 범위다.
        if (!row.is_object() || text_field(row, "stockEndType") != "stock")
        {
            continue;
        }

        std::string code = text_field(row, "itemCode");

        if (code.size() != symbol::kKoreanTickerLength)
        {
            continue;
        }

        listed.push_back({std::move(code), text_field(row, "stockName"), market});
    }

    return listed;
}

std::vector<BoardQuote> parse_polling(std::string_view body)
{
    std::vector<BoardQuote> quotes;
    const nlohmann::json    document = nlohmann::json::parse(body, nullptr, false);

    if (!document.is_object())
    {
        return quotes;
    }

    const auto rows = document.find("datas");

    if (rows == document.end() || !rows->is_array())
    {
        return quotes;
    }

    quotes.reserve(rows->size());

    for (const nlohmann::json& row : *rows)
    {
        if (!row.is_object())
        {
            continue;
        }

        // 맨 위 칸(KRX)을 읽는다. 같은 이름의 Raw 필드가 시간외(overMarketPriceInfo)·통합(integratedPriceInfo)
        //  안에도 있어 본문을 문자열로 훑으면 그쪽 값을 잡는다. 시간외 칸은 정규장 전 프리마켓 값일 때만 따로 읽는다.
        //  [wire] 근거: 2026-09-27 실측 응답 2종목에서 accumulatedTradingVolumeRaw·accumulatedTradingValueRaw가 종목마다 세 번
        //  (맨 위·overMarketPriceInfo·integratedPriceInfo) 나왔다. 공식 문서 없음.
        BoardQuote quote;
        quote.code           = text_field(row, "itemCode");
        quote.price          = raw_number(row, "closePriceRaw");
        quote.volume         = raw_number(row, "accumulatedTradingVolumeRaw");
        quote.value          = raw_number(row, "accumulatedTradingValueRaw");
        quote.change_percent = raw_number(row, "fluctuationsRatioRaw");
        use_premarket_when_regular_empty(row, quote);

        if (quote.code.empty() || quote.price <= 0.0)
        {
            continue;
        }

        quote.name         = text_field(row, "stockName");
        quote.market_value = raw_number(row, "marketValueFullRaw");
        quotes.push_back(std::move(quote));
    }

    return quotes;
}

} // namespace universe
