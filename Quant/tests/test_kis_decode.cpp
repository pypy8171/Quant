// KIS REST 디코더(api/KisRestDecode.h) 단위 테스트. 분봉: 숫자 필드 실패 처리·시각 변환·페이지 병합(중복·날짜
// 필터·커서)·interval 집계(OHLC 병합·정렬·bar_index·count 상한). 잔고: 행 필터·주문가능수량 "모름"·요약 폴백·
// 배열/객체 output2. 전광판: 키 후보 순서·빈 코드. 헤더 전용이라 HTTP·인증 링크 없이 돈다.
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

#define CHECK(cond)                                                                                                  \
    do                                                                                                               \
    {                                                                                                                \
        if (!(cond))                                                                                                 \
        {                                                                                                            \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n";                            \
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

int test_num()
{
    json node = {{"a", "1234.5"}, {"b", ""}, {"c", "abc"}, {"d", 7}, {"e", "-3"}};
    CHECK(kis_rest::num(node, "a") == 1234.5);
    CHECK(kis_rest::num(node, "b") == 0.0);       // 빈 문자열
    CHECK(kis_rest::num(node, "c") == 0.0);       // 숫자 아님
    CHECK(kis_rest::num(node, "d") == 0.0);       // 문자열이 아닌 값 — value()가 type_error를 던진다
    CHECK(kis_rest::num(node, "e") == -3.0);
    CHECK(kis_rest::num(node, "missing") == 0.0); // 키 없음
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
    std::vector<kis_rest::RawMinute> raws;
    std::unordered_set<std::string> seen;
    int added = -1;

    // 페이지 1: 최신→과거. 짧은 시각 행은 버리고 커서에도 안 잡힌다.
    json p1 = json::array({row("20260911", "090200", "100", "110", "90", "105", "10"),
                           row("20260911", "090100", "99", "101", "98", "100", "20"),
                           row("20260911", "0900", "1", "1", "1", "1", "1")});
    std::string cur = kis_rest::parse_minute_page(p1, raws, seen, "", added);
    CHECK(cur == "090100");
    CHECK(added == 2);
    CHECK(raws.size() == 2);
    CHECK(raws[0].open == 100 && raws[0].high == 110 && raws[0].low == 90 && raws[0].close == 105 && raws[0].volume == 10);

    // 페이지 2: 경계 중복(090100)은 raws에 안 들어가고, 커서는 중복 여부와 무관하게 가장 이른 시각.
    json p2 = json::array({row("20260911", "090100", "99", "101", "98", "100", "20"),
                           row("20260911", "090000", "95", "99", "94", "99", "30")});
    cur = kis_rest::parse_minute_page(p2, raws, seen, "", added);
    CHECK(cur == "090000");
    CHECK(added == 1);
    CHECK(raws.size() == 3);

    // 날짜 필터: 다른 날 행은 버리지만 커서에는 반영된다(역페이징이 그 시각을 넘어가야 하므로).
    json p3 = json::array({row("20260910", "153000", "1", "1", "1", "1", "1"),
                           row("20260911", "085900", "1", "1", "1", "1", "1")});
    cur = kis_rest::parse_minute_page(p3, raws, seen, "20260911", added);
    CHECK(cur == "085900");
    CHECK(added == 1);
    CHECK(raws.back().date == "20260911" && raws.back().hour == "085900");

    // 빈 페이지: 커서 빈 문자열, added 0.
    cur = kis_rest::parse_minute_page(json::array(), raws, seen, "", added);
    CHECK(cur.empty());
    CHECK(added == 0);
    return 0;
}

int test_aggregate()
{
    // 1분봉 6개를 뒤섞어 넣는다(REST는 최신→과거, 페이지 순서는 임의). 3분 버킷: [09:00,09:03) [09:03,09:06).
    std::vector<kis_rest::RawMinute> raws = {
        {"20260911", "090400", 120, 125, 118, 121, 6},
        {"20260911", "090100", 101, 108, 99, 107, 2},
        {"20260911", "090500", 121, 130, 120, 128, 7},
        {"20260911", "090000", 100, 105, 98, 101, 1},
        {"20260911", "090300", 110, 115, 109, 120, 5},
        {"20260911", "090200", 107, 112, 100, 110, 3},
    };
    auto bars = kis_rest::aggregate_minutes(raws, "005930", 3, 10);
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
    auto one = kis_rest::aggregate_minutes(raws, "005930", 3, 1);
    CHECK(one.size() == 1 && one[0].close == 128);

    // interval 1은 원본 그대로(최신→과거).
    auto ident = kis_rest::aggregate_minutes(raws, "005930", 1, 10);
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
    CHECK(kis_rest::aggregate_minutes(raws, "005930", 0, 10).empty());
    CHECK(kis_rest::aggregate_minutes(raws, "005930", 3, 0).empty());
    return 0;
}

int test_opt_num()
{
    json node = {{"a", "12.5"}, {"b", ""}, {"c", "x"}, {"d", 3}};
    CHECK(kis_rest::opt_num(node, "a") && *kis_rest::opt_num(node, "a") == 12.5);
    CHECK(!kis_rest::opt_num(node, "b"));       // 빈 문자열은 "없음" — num()의 0과 다르다
    CHECK(!kis_rest::opt_num(node, "c"));
    CHECK(!kis_rest::opt_num(node, "d"));       // 문자열이 아닌 값
    CHECK(!kis_rest::opt_num(node, "missing"));
    return 0;
}

int test_decode_holding()
{
    json node = {{"pdno", "005930"}, {"prdt_name", "삼성전자"}, {"hldg_qty", "12"}, {"pchs_avg_pric", "71000.0000"},
              {"ord_psbl_qty", "10 "}, {"evlu_pfls_amt", "-1200"}};
    Holding decoded_holding = kis_rest::decode_holding(node);
    CHECK(decoded_holding.ticker == "005930" && decoded_holding.name == "삼성전자" && decoded_holding.quantity == 12 && decoded_holding.average_price == 71000.0);
    CHECK(decoded_holding.eval_pnl == -1200.0);
    CHECK(decoded_holding.sellable_qty && *decoded_holding.sellable_qty == 10); // 꼬리 공백 허용

    // ord_psbl_qty 없음·숫자 아님 → 비어 있음("모름"). 0은 "매도 가능 0주"로 남는다.
    CHECK(!kis_rest::decode_holding(json{{"pdno", "1"}, {"hldg_qty", "1"}}).sellable_qty);
    CHECK(!kis_rest::decode_holding(json{{"pdno", "1"}, {"hldg_qty", "1"}, {"ord_psbl_qty", "n/a"}}).sellable_qty);
    Holding zero_holding = kis_rest::decode_holding(json{{"pdno", "1"}, {"hldg_qty", "1"}, {"ord_psbl_qty", "0"}});
    CHECK(zero_holding.sellable_qty && *zero_holding.sellable_qty == 0);
    return 0;
}

int test_decode_balance_page()
{
    // 1페이지: 보유 2행 + 0주 행(매도 완료 뒤 남은 것) + pdno 없는 행 → 2행만. output2는 배열.
    json p1 = {{"rt_cd", "0"},
               {"output1", json::array({json{{"pdno", "005930"}, {"hldg_qty", "5"}, {"pchs_avg_pric", "70000"}},
                                        json{{"pdno", "000660"}, {"hldg_qty", "3"}, {"pchs_avg_pric", "200000"}},
                                        json{{"pdno", "035420"}, {"hldg_qty", "0"}, {"pchs_avg_pric", "1"}},
                                        json{{"hldg_qty", "9"}}})},
               {"output2", json::array({json{{"tot_evlu_amt", "1500000"}, {"prvs_rcdl_excc_amt", "300000"},
                                             {"dnca_tot_amt", "999"}, {"bfdy_tot_asst_evlu_amt", "1490000"}}})}};
    AccountBalance balance_b;
    kis_rest::decode_balance_page(p1, balance_b, /*first_page=*/true);
    CHECK(balance_b.holdings.size() == 2 && balance_b.holdings[0].ticker == "005930" && balance_b.holdings[1].quantity == 3);
    CHECK(balance_b.total_eval_amt && *balance_b.total_eval_amt == 1500000.0);
    CHECK(balance_b.available_cash && *balance_b.available_cash == 300000.0); // 가수도정산금 우선
    CHECK(balance_b.prev_day_total_asset && *balance_b.prev_day_total_asset == 1490000.0);

    // 2페이지: 보유 누적, output2는 첫 페이지 것을 유지(덮어쓰지 않는다).
    json p2 = {{"output1", json::array({json{{"pdno", "051910"}, {"hldg_qty", "1"}, {"pchs_avg_pric", "5"}}})},
               {"output2", json::array({json{{"tot_evlu_amt", "1"}}})}};
    kis_rest::decode_balance_page(p2, balance_b, /*first_page=*/false);
    CHECK(balance_b.holdings.size() == 3 && balance_b.holdings[2].ticker == "051910");
    CHECK(*balance_b.total_eval_amt == 1500000.0);

    // output2가 객체이고 tot_evlu_amt가 비면 nass_amt, 가수도정산금이 비면 예수금으로.
    json p3 = {{"output1", json::array()},
               {"output2", json{{"tot_evlu_amt", ""}, {"nass_amt", "77"}, {"dnca_tot_amt", "5"}}}};
    AccountBalance balance_c;
    kis_rest::decode_balance_page(p3, balance_c, true);
    CHECK(balance_c.holdings.empty());
    CHECK(balance_c.total_eval_amt && *balance_c.total_eval_amt == 77.0);
    CHECK(balance_c.available_cash && *balance_c.available_cash == 5.0);
    CHECK(!balance_c.prev_day_total_asset);

    // output1·output2 둘 다 없음(한도 초과 본문 모양) → 아무것도 채우지 않는다.
    AccountBalance balance_d;
    kis_rest::decode_balance_page(json{{"rt_cd", "1"}, {"msg_cd", "EGW00201"}}, balance_d, true);
    CHECK(balance_d.holdings.empty() && !balance_d.total_eval_amt && !balance_d.available_cash);
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

int test_kis_result()
{
    // 봉투(std::expected): 실패는 bool false·error_text, 성공은 값 접근. 실패 봉투에는 값이 없다.
    KisResult<AccountBalance> failed = kis_fail("EGW00201", "초당 거래건수 초과");
    CHECK(!failed && !failed.has_value() && failed.error().code == "EGW00201" && error_text(failed) == "EGW00201 초당 거래건수 초과");
    AccountBalance balance;
    balance.holdings.push_back(Holding{"005930", "삼성전자", 1, 70000.0, 0.0, std::nullopt});
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
    if (test_num() || test_parse_dt() || test_parse_minute_page() || test_aggregate() || test_opt_num() ||
        test_decode_holding() || test_decode_balance_page() || test_decode_future_board() || test_kis_result())
    {
        return 1;
    }

    std::cout << "test_kis_decode: " << g_pass << " checks passed\n";
    return 0;
}
