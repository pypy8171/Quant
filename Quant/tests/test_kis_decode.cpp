// KIS REST 분봉 디코더(api/KisRestDecode.h) 단위 테스트. 숫자 필드 실패 처리·시각 변환·페이지 병합(중복·날짜
// 필터·커서)·interval 집계(OHLC 병합·정렬·bar_index·count 상한)를 고정한다. 헤더 전용이라 HTTP·인증 링크 없이 돈다.
// 관련 결정: D-051.
#include "api/KisRestDecode.h"

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
json row(const char* date, const char* hour, const char* o, const char* h, const char* l, const char* c, const char* v)
{
    return json{{"stck_bsop_date", date}, {"stck_cntg_hour", hour}, {"stck_oprc", o},
                {"stck_hgpr", h},         {"stck_lwpr", l},         {"stck_prpr", c},
                {"cntg_vol", v}};
}

int test_num()
{
    json o = {{"a", "1234.5"}, {"b", ""}, {"c", "abc"}, {"d", 7}, {"e", "-3"}};
    CHECK(kis_rest::num(o, "a") == 1234.5);
    CHECK(kis_rest::num(o, "b") == 0.0);       // 빈 문자열
    CHECK(kis_rest::num(o, "c") == 0.0);       // 숫자 아님
    CHECK(kis_rest::num(o, "d") == 0.0);       // 문자열이 아닌 값 — value()가 type_error를 던진다
    CHECK(kis_rest::num(o, "e") == -3.0);
    CHECK(kis_rest::num(o, "missing") == 0.0); // 키 없음
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
    CHECK(raws[0].o == 100 && raws[0].h == 110 && raws[0].l == 90 && raws[0].c == 105 && raws[0].v == 10);

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
    CHECK(bars[0].timestamp == std::chrono::system_clock::from_time_t(kis_rest::parse_dt("20260911", "090500")));
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
    auto d = kis_rest::aggregate_minutes(two_days, "005930", 3, 10);
    CHECK(d.size() == 2 && d[0].close == 2 && d[1].close == 1);

    // 시각이 깨진 행은 건너뛴다. 빈 입력·interval 0·count 0은 빈 결과(0으로 나누지 않는다).
    std::vector<kis_rest::RawMinute> bad = {{"20260911", "xx", 1, 1, 1, 1, 1}};
    CHECK(kis_rest::aggregate_minutes(bad, "005930", 3, 10).empty());
    std::vector<kis_rest::RawMinute> none;
    CHECK(kis_rest::aggregate_minutes(none, "005930", 3, 10).empty());
    CHECK(kis_rest::aggregate_minutes(raws, "005930", 0, 10).empty());
    CHECK(kis_rest::aggregate_minutes(raws, "005930", 3, 0).empty());
    return 0;
}

} // namespace

int main()
{
    if (test_num() || test_parse_dt() || test_parse_minute_page() || test_aggregate())
    {
        return 1;
    }

    std::cout << "test_kis_decode: " << g_pass << " checks passed\n";
    return 0;
}
