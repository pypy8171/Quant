// KIS REST 디코더(api/KisRestDecode.h) 단위 테스트. 분봉: 숫자 필드 실패 처리·시각 변환·페이지 병합(중복·날짜
// 필터·커서)·interval 집계(OHLC 병합·정렬·bar_index·count 상한). 잔고: 행 필터·주문가능수량 "모름"·요약 폴백·
// 배열/객체 output2. 전광판: 키 후보 순서·빈 코드. 미체결: 한도 초과·빈 본문은 실패, 행 필터·커서. 헤더 전용이라 HTTP·인증 링크 없이 돈다.
// 관련 결정: D-051(분봉), D-059(잔고·전광판·KisResult).
#include "api/KisRestDecode.h"
#include "core/KstTime.h"
#include "api/KisResult.h"

#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

using nlohmann::json;

namespace
{

int g_pass = 0;

#define CHECK(condition)                                                                                                  \
    do                                                                                                               \
    {                                                                                                                \
        if (!(condition))                                                                                                 \
        {                                                                                                            \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #condition << "\n";                            \
            return 1;                                                                                                \
        }                                                                                                            \
        ++g_pass;                                                                                                    \
    } while (0)

// output2 한 행. KIS는 숫자도 문자열로 준다.
json row(const char* date, const char* hour, const char* output, const char* header, const char* line, const char* code, const char* value)
{
    return json{{"stck_bsop_date", date}, {"stck_cntg_hour", hour}, {"stck_oprc", output},
                {"stck_hgpr", header},         {"stck_lwpr", line},         {"stck_prpr", code},
                {"cntg_vol", value}};
}

int test_number()
{
    json node = {{"a", "1234.5"}, {"b", ""}, {"c", "abc"}, {"d", 7}, {"e", "-3"}};
    CHECK(kis_rest::number(node, "a") == 1234.5);
    CHECK(kis_rest::number(node, "b") == 0.0);       // 빈 문자열
    CHECK(kis_rest::number(node, "c") == 0.0);       // 숫자 아님
    CHECK(kis_rest::number(node, "d") == 0.0);       // 문자열이 아닌 값 — value()가 type_error를 던진다
    CHECK(kis_rest::number(node, "e") == -3.0);
    CHECK(kis_rest::number(node, "missing") == 0.0); // 키 없음
    return 0;
}

int test_parse_dt()
{
    // 2026-09-11 09:00:00을 UTC 벽시계로 읽는다(서버 TZ 무관). 1789117200 = calendar.timegm((2026,9,11,9,0,0)).
    CHECK(kis_rest::parse_dt("20260911", "090000") == static_cast<time_t>(1789117200));
    CHECK(kis_rest::parse_dt("20260911", "153000") == static_cast<time_t>(1789140600));
    CHECK(kis_rest::parse_dt("20260911", "0900") == 0);    // 시각 자릿수 부족
    CHECK(kis_rest::parse_dt("2026091", "090000") == 0);   // 날짜 자릿수 부족
    CHECK(kis_rest::parse_dt("2026ab11", "090000") == 0);  // 숫자 아님
    CHECK(kis_rest::parse_dt("20260911", "09000099") != 0); // 6자 넘는 꼬리는 무시
    CHECK(kis_rest::parse_dt("20261301", "090000") == 0);   // 달력에 없는 날짜 — _mkgmtime은 정규화했고 chrono는 0
    CHECK(kis_rest::parse_dt("20260230", "090000") == 0);
    CHECK(kis_rest::parse_dt("20240229", "000000") == static_cast<time_t>(1709164800)); // 윤년
    return 0;
}

int test_parse_minute_page()
{
    std::vector<kis_rest::RawMinute> raw_minutes;
    std::unordered_set<uint64_t> seen;
    int added = -1;

    // 페이지 1: 최신→과거. 짧은 시각 행은 버리고 커서에도 안 잡힌다.
    json page_a = json::array({row("20260911", "090200", "100", "110", "90", "105", "10"),
                           row("20260911", "090100", "99", "101", "98", "100", "20"),
                           row("20260911", "0900", "1", "1", "1", "1", "1")});
    std::string current = kis_rest::parse_minute_page(page_a, raw_minutes, seen, "", added);
    CHECK(current == "090100");
    CHECK(added == 2);
    CHECK(raw_minutes.size() == 2);
    CHECK(raw_minutes[0].open == 100 && raw_minutes[0].high == 110 && raw_minutes[0].low == 90 && raw_minutes[0].close == 105 && raw_minutes[0].volume == 10);

    // 페이지 2: 경계 중복(090100)은 raws에 안 들어가고, 커서는 중복 여부와 무관하게 가장 이른 시각.
    json page_b = json::array({row("20260911", "090100", "99", "101", "98", "100", "20"),
                           row("20260911", "090000", "95", "99", "94", "99", "30")});
    current = kis_rest::parse_minute_page(page_b, raw_minutes, seen, "", added);
    CHECK(current == "090000");
    CHECK(added == 1);
    CHECK(raw_minutes.size() == 3);

    // 날짜 필터: 다른 날 행은 버리지만 커서에는 반영된다(역페이징이 그 시각을 넘어가야 하므로).
    json page_c = json::array({row("20260910", "153000", "1", "1", "1", "1", "1"),
                           row("20260911", "085900", "1", "1", "1", "1", "1")});
    current = kis_rest::parse_minute_page(page_c, raw_minutes, seen, "20260911", added);
    CHECK(current == "085900");
    CHECK(added == 1);
    CHECK(raw_minutes.back().date == "20260911" && raw_minutes.back().hour == "085900");

    // 빈 페이지: 커서 빈 문자열, added 0.
    current = kis_rest::parse_minute_page(json::array(), raw_minutes, seen, "", added);
    CHECK(current.empty());
    CHECK(added == 0);
    return 0;
}

int test_aggregate()
{
    // 1분봉 6개를 뒤섞어 넣는다(REST는 최신→과거, 페이지 순서는 임의). 3분 버킷: [09:00,09:03) [09:03,09:06).
    std::vector<kis_rest::RawMinute> raw_minutes = {
        {"20260911", "090400", 120, 125, 118, 121, 6},
        {"20260911", "090100", 101, 108, 99, 107, 2},
        {"20260911", "090500", 121, 130, 120, 128, 7},
        {"20260911", "090000", 100, 105, 98, 101, 1},
        {"20260911", "090300", 110, 115, 109, 120, 5},
        {"20260911", "090200", 107, 112, 100, 110, 3},
    };
    auto bars = kis_rest::aggregate_minutes(raw_minutes, "005930", 3, 10);
    CHECK(bars.size() == 2);
    CHECK(bars[0].bar_index == 0 && bars[1].bar_index == 1);
    CHECK(bars[0].ticker == "005930" && bars[0].market == Market::KR);

    // bars[0] = 최신 버킷 09:03~09:05: open=첫 봉 open, high=max, low=min, close=마지막 close, volume=합.
    CHECK(bars[0].open == 110 && bars[0].high == 130 && bars[0].low == 109 && bars[0].close == 128);
    CHECK(bars[0].volume == 18);
    CHECK(bars[0].timestamp == std::chrono::system_clock::from_time_t(kis_rest::parse_dt("20260911", "090500") - kst::kOffsetSec)); // 진짜 UTC(D-072)
    // bars[1] = 09:00~09:02
    CHECK(bars[1].open == 100 && bars[1].high == 112 && bars[1].low == 98 && bars[1].close == 110);
    CHECK(bars[1].volume == 6);

    // count 상한은 최신 쪽에서 센다.
    auto one = kis_rest::aggregate_minutes(raw_minutes, "005930", 3, 1);
    CHECK(one.size() == 1 && one[0].close == 128);

    // interval 1은 원본 그대로(최신→과거).
    auto ident = kis_rest::aggregate_minutes(raw_minutes, "005930", 1, 10);
    CHECK(ident.size() == 6 && ident[0].close == 128 && ident[5].close == 101);

    // 날짜가 다르면 같은 버킷 번호라도 합치지 않는다.
    std::vector<kis_rest::RawMinute> two_days = {
        {"20260910", "090000", 1, 1, 1, 1, 1},
        {"20260911", "090000", 2, 2, 2, 2, 2},
    };
    auto aggregated = kis_rest::aggregate_minutes(two_days, "005930", 3, 10);
    CHECK(aggregated.size() == 2 && aggregated[0].close == 2 && aggregated[1].close == 1);

    // 시각이 깨진 행은 건너뛴다. 빈 입력·interval 0·count 0은 빈 결과(0으로 나누지 않는다).
    std::vector<kis_rest::RawMinute> bad = {{"20260911", "xx", 1, 1, 1, 1, 1}};
    CHECK(kis_rest::aggregate_minutes(bad, "005930", 3, 10).empty());
    std::vector<kis_rest::RawMinute> none;
    CHECK(kis_rest::aggregate_minutes(none, "005930", 3, 10).empty());
    CHECK(kis_rest::aggregate_minutes(raw_minutes, "005930", 0, 10).empty());
    CHECK(kis_rest::aggregate_minutes(raw_minutes, "005930", 3, 0).empty());
    return 0;
}

int test_option_number()
{
    json node = {{"a", "12.5"}, {"b", ""}, {"c", "x"}, {"d", 3}};
    CHECK(kis_rest::option_number(node, "a") && *kis_rest::option_number(node, "a") == 12.5);
    CHECK(!kis_rest::option_number(node, "b"));       // 빈 문자열은 "없음" — number()의 0과 다르다
    CHECK(!kis_rest::option_number(node, "c"));
    CHECK(!kis_rest::option_number(node, "d"));       // 문자열이 아닌 값
    CHECK(!kis_rest::option_number(node, "missing"));
    return 0;
}

int test_decode_holding()
{
    json node = {{"pdno", "005930"}, {"prdt_name", "삼성전자"}, {"hldg_qty", "12"}, {"pchs_avg_pric", "71000.0000"},
              {"ord_psbl_qty", "10 "}, {"evlu_pfls_amt", "-1200"}, {"prpr", "70900"}};
    Holding decoded_holding = kis_rest::decode_holding(node);
    CHECK(decoded_holding.ticker == "005930" && decoded_holding.name == "삼성전자" && decoded_holding.quantity == 12 && decoded_holding.average_price == 71000.0);
    CHECK(decoded_holding.evaluation_pnl == -1200.0);
    CHECK(decoded_holding.current_price == 70900.0);
    CHECK(decoded_holding.sellable_quantity && *decoded_holding.sellable_quantity == 10); // 꼬리 공백 허용

    // ord_psbl_qty 없음·숫자 아님 → 비어 있음("모름"). 0은 "매도 가능 0주"로 남는다.
    CHECK(!kis_rest::decode_holding(json{{"pdno", "1"}, {"hldg_qty", "1"}}).sellable_quantity);
    CHECK(!kis_rest::decode_holding(json{{"pdno", "1"}, {"hldg_qty", "1"}, {"ord_psbl_qty", "n/a"}}).sellable_quantity);
    Holding zero_holding = kis_rest::decode_holding(json{{"pdno", "1"}, {"hldg_qty", "1"}, {"ord_psbl_qty", "0"}});
    CHECK(zero_holding.sellable_quantity && *zero_holding.sellable_quantity == 0);
    return 0;
}

int test_decode_balance_page()
{
    // 1페이지: 보유 2행 + 0주 행(매도 완료 뒤 남은 것) + pdno 없는 행 → 2행만. output2는 배열.
    json page_a = {{"rt_cd", "0"},
               {"output1", json::array({json{{"pdno", "005930"}, {"hldg_qty", "5"}, {"pchs_avg_pric", "70000"}},
                                        json{{"pdno", "000660"}, {"hldg_qty", "3"}, {"pchs_avg_pric", "200000"}},
                                        json{{"pdno", "035420"}, {"hldg_qty", "0"}, {"pchs_avg_pric", "1"}},
                                        json{{"hldg_qty", "9"}}})},
               {"output2", json::array({json{{"tot_evlu_amt", "1500000"}, {"prvs_rcdl_excc_amt", "300000"},
                                             {"dnca_tot_amt", "999"}, {"bfdy_tot_asst_evlu_amt", "1490000"}}})}};
    AccountBalance balance_b;
    kis_rest::decode_balance_page(page_a, balance_b, /*first_page=*/true);
    CHECK(balance_b.holdings.size() == 2 && balance_b.holdings[0].ticker == "005930" && balance_b.holdings[1].quantity == 3);
    CHECK(balance_b.total_evaluation_amount && *balance_b.total_evaluation_amount == 1500000.0);
    CHECK(balance_b.available_cash && *balance_b.available_cash == 300000.0); // 가수도정산금 우선
    CHECK(balance_b.previous_day_total_asset && *balance_b.previous_day_total_asset == 1490000.0);

    // 2페이지: 보유 누적, output2는 첫 페이지 것을 유지(덮어쓰지 않는다).
    json page_b = {{"output1", json::array({json{{"pdno", "051910"}, {"hldg_qty", "1"}, {"pchs_avg_pric", "5"}}})},
               {"output2", json::array({json{{"tot_evlu_amt", "1"}}})}};
    kis_rest::decode_balance_page(page_b, balance_b, /*first_page=*/false);
    CHECK(balance_b.holdings.size() == 3 && balance_b.holdings[2].ticker == "051910");
    CHECK(*balance_b.total_evaluation_amount == 1500000.0);

    // output2가 객체이고 tot_evlu_amt가 비면 nass_amt, 가수도정산금이 비면 예수금으로.
    json page_c = {{"output1", json::array()},
               {"output2", json{{"tot_evlu_amt", ""}, {"nass_amt", "77"}, {"dnca_tot_amt", "5"}}}};
    AccountBalance balance_three;
    kis_rest::decode_balance_page(page_c, balance_three, true);
    CHECK(balance_three.holdings.empty());
    CHECK(balance_three.total_evaluation_amount && *balance_three.total_evaluation_amount == 77.0);
    CHECK(balance_three.available_cash && *balance_three.available_cash == 5.0);
    CHECK(!balance_three.previous_day_total_asset);

    // output1·output2 둘 다 없음(한도 초과 본문 모양) → 아무것도 채우지 않는다.
    AccountBalance balance_d;
    kis_rest::decode_balance_page(json{{"rt_cd", "1"}, {"msg_cd", "EGW00201"}}, balance_d, true);
    CHECK(balance_d.holdings.empty() && !balance_d.total_evaluation_amount && !balance_d.available_cash);
    return 0;
}

int test_decode_future_board()
{
    // output1이 비어 있으면 output2, 그것도 없으면 output. 코드 없는 행은 버린다.
    json document = {{"output1", json::array()},
              {"output2", json::array({json{{"futs_shrn_iscd", "101W09"}, {"hts_kor_isnm", "KOSPI200 F 202609"}},
                                       json{{"hts_kor_isnm", "코드 없음"}},
                                       json{{"futs_shrn_iscd", "101WC0"}, {"hts_kor_isnm", "KOSPI200 F 202612"}}})},
              {"output", json::array({json{{"futs_shrn_iscd", "ZZZ"}}})}};
    auto rows = kis_rest::decode_future_board(document);
    CHECK(rows.size() == 2 && rows[0].issue_code == "101W09" && rows[1].issue_code == "101WC0");
    CHECK(rows[0].name == "KOSPI200 F 202609");
    CHECK(kis_rest::decode_future_board(json::object()).empty());
    CHECK(kis_rest::decode_future_board(json{{"output1", "not-an-array"}}).empty());
    return 0;
}

int test_decode_open_order_page()
{
    // 초당 한도(EGW00201) — rt_cd "1"에 빈 배열. 급락장 재기동에서 실제로 오는 모양이라 "미체결 없음"이 아니라
    //  실패여야 한다. 모의(output1)·실거래(output) 둘 다. (전수조사 B1-2)
    const std::string throttled_paper =
        R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다.","output1":[],"output2":{}})";
    const auto paper_failed = kis_rest::decode_open_order_page(throttled_paper, /*paper=*/true);
    CHECK(!paper_failed && paper_failed.error().code == "EGW00201");
    const std::string throttled_live =
        R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다.","output":[]})";
    const auto live_failed = kis_rest::decode_open_order_page(throttled_live, /*paper=*/false);
    CHECK(!live_failed && live_failed.error().code == "EGW00201");

    // 빈 본문(전송 실패)·JSON 아님(게이트웨이 오류 페이지)도 실패다.
    CHECK(kis_rest::decode_open_order_page("", false).error().code == "transport");
    CHECK(kis_rest::decode_open_order_page("<html>502</html>", false).error().code == "parse");

    // 브로커가 "없다"고 답한 것만 빈 목록 성공이다.
    const auto none = kis_rest::decode_open_order_page(R"({"rt_cd":"0","output":[],"ctx_area_nk100":"   "})", false);
    CHECK(none && none->rows.empty() && none->next_key.empty());

    // 실거래: psbl_qty 0 행·종목 없는 행은 버리고, 매도/매수 코드를 읽고, 커서의 끝 공백을 뗀다.
    const std::string live_page = R"({"rt_cd":"0","output":[
        {"pdno":"005930","odno":"0000007886","ord_gno_brno":"06010","psbl_qty":"18","ord_unpr":"70100","sll_buy_dvsn_cd":"01"},
        {"pdno":"000660","odno":"0000007887","psbl_qty":"0","ord_unpr":"1","sll_buy_dvsn_cd":"02"},
        {"odno":"0000007888","psbl_qty":"3","sll_buy_dvsn_cd":"02"},
        {"pdno":"035420","ODNO":"0000007889","psbl_qty":"2","ord_unpr":"150000","sll_buy_dvsn_cd":"02"}],
        "ctx_area_fk100":"FK1  ","ctx_area_nk100":"NK1  "})"; // [wire] KIS 응답 필드명
    const auto live = kis_rest::decode_open_order_page(live_page, false);
    CHECK(live && live->rows.size() == 2);
    CHECK(live->rows[0].kis_order_no == "0000007886" && live->rows[0].psbl_qty == 18 && live->rows[0].side == OrderSide::SELL);
    CHECK(live->rows[0].krx_forwarding_org_no == "06010" && live->rows[0].ord_unpr == 70100.0);
    CHECK(live->rows[1].kis_order_no == "0000007889" && live->rows[1].side == OrderSide::BUY);
    CHECK(live->forward_key == "FK1" && live->next_key == "NK1");

    // 모의: output1·rmn_qty(잔여)를 읽고 취소된 행(cncl_yn=Y)은 버린다.
    const std::string paper_page = R"({"rt_cd":"0","output1":[
        {"pdno":"021240","odno":"1","rmn_qty":"4","psbl_qty":"99","cncl_yn":"N","sll_buy_dvsn_cd":"02"},
        {"pdno":"021240","odno":"2","rmn_qty":"5","cncl_yn":"Y","sll_buy_dvsn_cd":"02"}]})";
    const auto paper = kis_rest::decode_open_order_page(paper_page, true);
    CHECK(paper && paper->rows.size() == 1 && paper->rows[0].psbl_qty == 4);
    return 0;
}

int test_kis_result()
{
    // 봉투(std::expected): 실패는 bool false·error_text, 성공은 값 접근. 실패 봉투에는 값이 없다.
    KisResult<AccountBalance> failed = kis_fail("EGW00201", "초당 거래건수 초과");
    CHECK(!failed && !failed.has_value() && failed.error().code == "EGW00201" && error_text(failed) == "EGW00201 초당 거래건수 초과");
    AccountBalance balance;
    balance.holdings.push_back(Holding{"005930", "삼성전자", 1, 70000.0, 0.0, 0.0, std::nullopt});
    KisResult<AccountBalance> moved = std::move(balance);
    CHECK(moved && error_text(moved).empty() && moved->holdings.size() == 1 && (*moved).holdings[0].ticker == "005930");
    // 실패 → 값 대입으로 성공 봉투가 된다(LedgerReconciler 부트스트랩의 "init" 실패 → 재시도 루프 대입).
    failed = AccountBalance{};
    CHECK(failed.has_value() && failed->holdings.empty());
    return 0;
}

} // namespace

int main()
{
    if (test_number() || test_parse_dt() || test_parse_minute_page() || test_aggregate() || test_option_number() ||
        test_decode_holding() || test_decode_balance_page() || test_decode_future_board() || test_decode_open_order_page() ||
        test_kis_result())
    {
        return 1;
    }

    std::cout << "test_kis_decode: " << g_pass << " checks passed\n";
    return 0;
}
