// 시세판(universe/MarketBoard.h)의 순수 함수 검사 — 네이버 응답 읽기·시장별 재랭킹·파일 문서.
//  네트워크는 쓰지 않는다. 응답 본문은 09-26 실측 응답에서 필요한 칸만 남긴 것이다. [why D-147]
#include "universe/MarketBoard.h"

#include <cassert>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>

using namespace universe;

namespace
{

// 맨 위 Raw 값과 시간외·통합 안의 같은 이름 칸 값을 일부러 다르게 둔다 — 안쪽 값을 읽으면 실패한다.
void check_polling_reads_top_level_only()
{
    const std::string body = R"({"pollingInterval":7000,"datas":[
        {"itemCode":"005930","stockName":"삼성전자",
         "overMarketPriceInfo":{"closePriceRaw":"1","accumulatedTradingVolumeRaw":"2","accumulatedTradingValueRaw":"3"},
         "integratedPriceInfo":{"closePriceRaw":"4","accumulatedTradingVolumeRaw":"5","accumulatedTradingValueRaw":"6"},
         "closePriceRaw":"84500","accumulatedTradingVolumeRaw":"12000000",
         "accumulatedTradingValueRaw":"1014000000000","marketValueFullRaw":"504000000000000"},
        {"itemCode":"000660","stockName":"SK하이닉스","closePriceRaw":0,"marketValueFullRaw":"1"},
        {"itemCode":"035420","stockName":"NAVER","closePriceRaw":210000,"accumulatedTradingValueRaw":"garbage"}
    ]})";
    const std::vector<BoardQuote> quotes = parse_polling(body);
    assert(quotes.size() == 2); // 가격 0은 뺀다
    assert(quotes[0].code == "005930");
    assert(quotes[0].name == "삼성전자");
    assert(quotes[0].price == 84500.0);
    assert(quotes[0].volume == 12000000.0);
    assert(quotes[0].value == 1014000000000.0);
    assert(quotes[0].market_value == 504000000000000.0);
    assert(quotes[1].price == 210000.0); // 숫자로 와도 읽는다
    assert(quotes[1].value == 0.0);      // 못 읽는 값은 0

    assert(parse_polling("").empty());
    assert(parse_polling("<html>").empty());
    assert(parse_polling(R"({"datas":{}})").empty());
}

void check_listing_keeps_stocks_only()
{
    const std::string body = R"({"totalCount":2412,"stocks":[
        {"stockEndType":"stock","itemCode":"005930","stockName":"삼성전자"},
        {"stockEndType":"etf","itemCode":"069500","stockName":"KODEX 200"},
        {"stockEndType":"stock","itemCode":"0096B0","stockName":"새코드"},
        {"stockEndType":"stock","itemCode":"12345","stockName":"짧은코드"}
    ]})";
    int total_count = 0;
    const std::vector<ListedStock> listed = parse_listing_page(body, "KOSPI", total_count);
    assert(total_count == 2412);
    assert(listed.size() == 2);
    assert(listed[0].code == "005930" && listed[0].market == "KOSPI");
    assert(listed[1].code == "0096B0");

    parse_listing_page("not json", "KOSDAQ", total_count);
    assert(total_count == -1); // 실패 표시
}

BoardQuote quote_of(const char* code, double value, double market_value)
{
    return BoardQuote{code, std::string("n") + code, 1000.4, 1.0, value, market_value};
}

// 시장별로 시총 상위 → 거래대금 상위 순으로 싣고, 겹치면 한 번만. 하한 미달·시총 0·판에 없는 종목은 후보가 아니다.
void check_rank_per_market_union()
{
    const std::vector<ListedStock> listing = {
        {"000001", "가", "KOSPI"},  {"000002", "나", "KOSPI"}, {"000003", "", "KOSPI"},
        {"000004", "라", "KOSPI"},  {"000005", "마", "KOSPI"}, {"100001", "A", "KOSDAQ"},
        {"100002", "B", "KOSDAQ"}, {"100003", "C", "KOSDAQ"},
    };
    BoardSnapshot board;
    board.quotes = {
        quote_of("000001", 5e9, 900e9), // 시총 1위
        quote_of("000002", 9e9, 100e9), // 거래대금 1위
        quote_of("000003", 8e9, 800e9), // 시총 2위·거래대금 2위 — 한 번만
        quote_of("000004", 0.5e9, 999e12), // 거래대금 하한 미달
        quote_of("000005", 7e9, 0.0),   // 시총 0
        quote_of("100001", 3e9, 50e9),
        quote_of("100002", 4e9, 40e9),
        // 100003은 판에 없다
    };

    const std::vector<RankedStock> ranked = rank_universe(listing, board, 2, 2, 1e9);
    std::vector<std::string> codes;

    for (const RankedStock& stock : ranked)
    {
        codes.push_back(stock.code);
    }

    const std::vector<std::string> expected = {"000001", "000003", "000002", "100001", "100002"};
    assert(codes == expected);
    assert(ranked[0].market == "KOSPI" && ranked[3].market == "KOSDAQ");
    assert(ranked[0].close == 1000.0);    // 반올림
    assert(ranked[1].name == "n000003");  // 목록 이름이 비면 시세 쪽 이름

    // 시총 축 0 = 끔. 거래대금 순서만 남는다.
    const std::vector<RankedStock> turnover_only = rank_universe(listing, board, 0, 1, 1e9);
    assert(turnover_only.size() == 2);
    assert(turnover_only[0].code == "000002");
    assert(turnover_only[1].code == "100002");
}

void check_turnover_filled_half_rule()
{
    BoardSnapshot board;
    assert(!turnover_filled(board)); // 빈 판
    board.quotes = {quote_of("000001", 1.0, 1.0), quote_of("000002", 0.0, 1.0), quote_of("000003", 0.0, 1.0)};
    assert(!turnover_filled(board)); // 1/3
    board.quotes[1].value = 2.0;
    assert(turnover_filled(board)); // 2/3
}

// 옛 파이썬 피드와 같은 스키마 — 알림·대시보드·백필 스크립트가 이 키를 읽는다.
void check_universe_file_schema()
{
    RankedUniverse ranked;
    ranked.ranked_at = 1790000000; // 2026-09-21 KST 무렵
    ranked.stocks    = {{"005930", "삼성전자", 84500.0, "KOSPI"}};
    ranked.listing   = {{"005930", "삼성전자", "KOSPI"}, {"100001", "", "KOSDAQ"}};

    const nlohmann::json document = nlohmann::json::parse(universe_file_text(ranked, "naver-live 0901"));
    assert(document["schema"] == 1);
    assert(document["market"] == "ALL");
    assert(document["count"] == 1);
    assert(document["basDt"].get<std::string>().size() == 8);
    assert(document["requested_date"] == document["basDt"]);
    assert(document["mktcap_source"] == "naver-live 0901");
    assert(document["universe"][0]["ticker"] == "005930");
    assert(document["universe"][0]["close"] == 84500);
    assert(document["universe"][0]["market"] == "KOSPI");
    assert(document["market_map"]["100001"] == "KOSDAQ");
    assert(document["name_map"].contains("005930"));
    assert(!document["name_map"].contains("100001")); // 빈 이름은 싣지 않는다
    assert(document.contains("scanned_at"));
}

} // namespace

int main()
{
    check_polling_reads_top_level_only();
    check_listing_keeps_stocks_only();
    check_rank_per_market_union();
    check_turnover_filled_half_rule();
    check_universe_file_schema();
    std::puts("test_market_board: ok");
    return 0;
}
