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

// 정규장 전 응답 — KRX 칸 거래대금이 빈 문자열이고 시간외 칸이 NXT 프리마켓이다. 본문은 2026-10-01 실측 응답
//  (08:56 005490 값, 09:13 응답의 칸 구성)에서 필요한 칸만 남겼다. 프리마켓에 거래가 있는 종목만 그 값으로 바뀐다.
void check_polling_uses_premarket_before_open()
{
    const std::string body = R"({"pollingInterval":7000,"datas":[
        {"itemCode":"005490","stockName":"POSCO홀딩스","marketStatus":"PREOPEN",
         "closePrice":"306,000","fluctuationsRatioRaw":"0","accumulatedTradingVolumeRaw":"",
         "accumulatedTradingValueRaw":"","marketValueFullRaw":"24168665735000","closePriceRaw":"306000",
         "overMarketPriceInfo":{"tradingSessionType":"PRE_MARKET","overMarketStatus":"OPEN","overPrice":"310,500",
            "compareToPreviousPrice":{"code":"2","text":"상승","name":"RISING"},"fluctuationsRatio":"1.47",
            "accumulatedTradingVolume":"28,180","accumulatedTradingValue":"87억",
            "accumulatedTradingVolumeRaw":"28180","accumulatedTradingValueRaw":"8749000000"},
         "integratedPriceInfo":{"accumulatedTradingVolumeRaw":"28180","accumulatedTradingValueRaw":"8749000000"}},
        {"itemCode":"000001","stockName":"부호없는하락","closePriceRaw":"10000","accumulatedTradingValueRaw":"",
         "overMarketPriceInfo":{"tradingSessionType":"PRE_MARKET","overPrice":"9,800",
            "compareToPreviousPrice":{"code":"5"},"fluctuationsRatio":"2.00",
            "accumulatedTradingVolumeRaw":"100","accumulatedTradingValueRaw":"980000"}},
        {"itemCode":"000002","stockName":"프리마켓거래없음","closePriceRaw":"50000","accumulatedTradingValueRaw":"",
         "overMarketPriceInfo":{"tradingSessionType":"PRE_MARKET","overPrice":"50,000","fluctuationsRatio":"0.00",
            "accumulatedTradingVolumeRaw":"","accumulatedTradingValueRaw":""}},
        {"itemCode":"000003","stockName":"정규장첫체결전","closePriceRaw":"7000","accumulatedTradingValueRaw":"",
         "overMarketPriceInfo":{"tradingSessionType":"REGULAR_MARKET","overPrice":"7,100","fluctuationsRatio":"1.43",
            "accumulatedTradingVolumeRaw":"10","accumulatedTradingValueRaw":"71000"}},
        {"itemCode":"000004","stockName":"애프터마켓","closePriceRaw":"20000","fluctuationsRatioRaw":"-1.00",
         "accumulatedTradingVolumeRaw":"500","accumulatedTradingValueRaw":"10000000",
         "overMarketPriceInfo":{"tradingSessionType":"AFTER_MARKET","overPrice":"21,000","fluctuationsRatio":"3.95",
            "accumulatedTradingVolumeRaw":"9","accumulatedTradingValueRaw":"189000"}},
        {"itemCode":"000005","stockName":"KRX값있음","closePriceRaw":"3000","accumulatedTradingVolumeRaw":"1",
         "accumulatedTradingValueRaw":"3000",
         "overMarketPriceInfo":{"tradingSessionType":"PRE_MARKET","overPrice":"3,100","fluctuationsRatio":"3.33",
            "accumulatedTradingVolumeRaw":"1000","accumulatedTradingValueRaw":"3100000"}}
    ]})";
    const std::vector<BoardQuote> quotes = parse_polling(body);
    assert(quotes.size() == 6);

    const BoardQuote& posco = quotes[0];
    assert(posco.code == "005490" && posco.premarket);
    assert(posco.price == 310500.0);              // 쉼표 든 표시용 가격을 읽는다
    assert(posco.value == 8749000000.0);
    assert(posco.volume == 28180.0);
    assert(posco.change_percent == 1.47);
    assert(posco.market_value == 24168665735000.0); // 시총은 맨 위 칸 그대로

    assert(quotes[1].premarket && quotes[1].price == 9800.0);
    assert(quotes[1].change_percent == -2.0);     // 방향 코드 5(하락)면 부호를 붙인다

    assert(!quotes[2].premarket);                 // 프리마켓 거래가 없으면 KRX 값(전일 종가) 그대로
    assert(quotes[2].price == 50000.0 && quotes[2].value == 0.0);

    assert(!quotes[3].premarket);                 // 정규장 세션 칸은 대신 쓰지 않는다
    assert(quotes[3].price == 7000.0 && quotes[3].value == 0.0);

    assert(!quotes[4].premarket);                 // 애프터마켓은 기존 동작 — KRX 정규장 값
    assert(quotes[4].price == 20000.0 && quotes[4].value == 10000000.0 && quotes[4].change_percent == -1.0);

    assert(!quotes[5].premarket);                 // KRX 값이 있으면 프리마켓 칸이 있어도 KRX
    assert(quotes[5].price == 3000.0 && quotes[5].value == 3000.0);
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

// 1분 표본 판정 — 매매 시간(09:00~20:00 KST, 마감 분 포함)이고 같은 분에 두 번 쓰지 않는다.
void check_minute_due_window()
{
    const std::time_t open_kst = 1790812800; // 2026-10-01 09:00:00 KST(= 00:00:00 UTC)
    const std::int64_t open_minute = static_cast<std::int64_t>(open_kst) / 60;

    assert(board_minute_due(open_kst, -1));                    // 개장 분, 아직 저장 없음
    assert(!board_minute_due(open_kst + 55, open_minute));     // 같은 분 안의 다음 판
    assert(board_minute_due(open_kst + 60, open_minute));      // 다음 분
    assert(!board_minute_due(open_kst - 1, -1));               // 08:59:59
    assert(board_minute_due(open_kst + 8 * 3600, -1));         // 17:00 애프터마켓도 담는다
    assert(board_minute_due(open_kst + 660 * 60 + 59, -1));    // 20:00:59 — 마감 분까지 담는다
    assert(!board_minute_due(open_kst + 661 * 60, -1));        // 20:01:00
    assert(board_minute_file_name(open_kst) == "board_20261001.csv");
    assert(board_minute_file_name(open_kst - 9 * 3600) == "board_20261001.csv"); // 00:00 KST도 같은 날
}

// 줄 형식 — 종목마다 한 줄, 판의 종목 수·실패 묶음·전체 묶음을 끝 열에 반복한다.
void check_minute_rows_format()
{
    BoardSnapshot board;
    board.received_at     = 1790812800 + 3 * 60 + 7; // 09:03:07 KST
    board.request_count   = 3;
    board.failed_requests = 1;
    board.quotes          = {BoardQuote{"005930", "삼성전자", 84500.0, 12000000.0, 1014000000000.0, 504000000000000.0, 1.234},
                             BoardQuote{"0096B0", "새코드", 1234.4, 0.0, 0.0, 0.0, -29.999}};
    const std::string rows = board_minute_rows(board);
    assert(rows == "09:03:07,005930,84500,12000000,1014000000000,1.23,504000000000000,2,1,3\n"
                   "09:03:07,0096B0,1234,0,0,-30.00,0,2,1,3\n");

    const std::string header = board_minute_header();
    assert(header.find("고가") != std::string::npos); // 고가·저가가 없다는 것을 머리에 적는다
    assert(header.substr(header.rfind('\n', header.size() - 2) + 1) ==
           "kst_time,code,price,volume,turnover,change_pct,market_cap,board_count,failed_batches,batches\n");
    assert(board_minute_rows(BoardSnapshot{}).empty());
}

int main()
{
    check_polling_reads_top_level_only();
    check_polling_uses_premarket_before_open();
    check_listing_keeps_stocks_only();
    check_rank_per_market_union();
    check_turnover_filled_half_rule();
    check_universe_file_schema();
    check_minute_due_window();
    check_minute_rows_format();
    std::puts("test_market_board: ok");
    return 0;
}
