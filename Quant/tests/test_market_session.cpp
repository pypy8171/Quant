// 정규장 시각 판정(core/MarketSession.h)과 KST 분해(core/KstTime.h) 단위 테스트. 틱마다 불리는 순수
//  함수라 경계값을 고정해 두고, KST 분해는 머신 TZ를 바꿔 가며 같은 값이 나오는지 본다. 관련 결정: D-037·D-070.
#include "core/KstTime.h"
#include "core/MarketSession.h"
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

namespace
{
void set_tz(const char* tz)
{
#ifdef _WIN32
    _putenv_s("TZ", tz);
    _tzset();
#else
    setenv("TZ", tz, 1);
    tzset();
#endif
}

// 2026-09-11(금) 09:00:00 KST = UTC 00:00:00. 같은 순간을 TZ마다 다시 분해해도 KST 값은 같아야 한다.
void check_kst_fixed()
{
    constexpr std::time_t kOpen = 1789084800;

    for (const char* tz : {"UTC0", "KST-9", "PST8PDT"})
    {
        set_tz(tz);
        assert(kst::ymd(kOpen) == "20260911");
        assert(kst::hhmmss(kOpen) == "090000");
        assert(kst::datetime(kOpen) == "2026-09-11 09:00:00");
        assert(kst::ymd(kOpen - 1) == "20260911");     // 08:59:59 KST
        assert(kst::hhmmss(kOpen - 1) == "085959");
        assert(kst::ymd(kOpen - 9 * 3600) == "20260911"); // 00:00:00 KST — 날짜 경계
        assert(kst::ymd(kOpen - 9 * 3600 - 1) == "20260910");
        assert(kst::hhmmss(kOpen - 9 * 3600 - 1) == "235959");
        assert(kst::hhmmss_int(kOpen) == 90000);
        assert(kst::hhmmss_int(kOpen - 1) == 85959);
        assert(kst::hhmmss_int(kOpen - 9 * 3600 - 1) == 235959);

        const struct tm t = kst::to_tm(kOpen);
        assert(t.tm_year == 126 && t.tm_mon == 8 && t.tm_mday == 11);
        assert(t.tm_hour == 9 && t.tm_min == 0 && t.tm_sec == 0);
        assert(t.tm_wday == 5);   // 금요일 — is_kr_market_open이 주말 판정에 쓴다
        assert(t.tm_yday == 253); // 1월 1일 = 0 — BarAggregator가 거래일 키에 쓴다
        assert(kst::to_tm(kOpen - 9 * 3600 - 1).tm_yday == 252);

        const auto tod = kst::time_of_day(kOpen + 6 * 3600 + 30 * 60 + 5);
        assert(tod.hours().count() == 15 && tod.minutes().count() == 30 && tod.seconds().count() == 5);
    }

    // 윤년 2월 29일 — 1월 1일부터 59일째.
    constexpr std::time_t kLeap = 1709164800 - 9 * 3600; // 2024-02-29 00:00:00 KST
    assert(kst::ymd(kLeap) == "20240229");
    assert(kst::to_tm(kLeap).tm_yday == 59);
    assert(kst::format_ymd(std::chrono::year{2024} / 2 / 29) == "20240229");

    // 자리값 눈금(옮기지 않음): parse_dt("20260911","090000")의 결과를 그대로 날짜로.
    assert(kst::format_ymd(kst::utc_date(1789117200)) == "20260911");
    assert(kst::decompose(std::chrono::sys_seconds{std::chrono::seconds{1789117200}}).tm_wday == 5);
}
} // namespace

int main()
{
    using namespace krx;

    check_kst_fixed();

    // parse_hhmm: 앞 4자리만 본다. HHMMSS도 HHMM으로 접힌다.
    assert(parse_hhmm("0930") == 930);
    assert(parse_hhmm("093015") == 930);
    assert(parse_hhmm("1530") == 1530);
    assert(parse_hhmm("0000") == 0);

    // 짧거나 숫자가 아니면 0 — 0은 장 밖이라 판정이 보수적으로 떨어진다.
    assert(parse_hhmm("") == 0);
    assert(parse_hhmm("930") == 0);
    assert(parse_hhmm("ab12") == 0);
    assert(parse_hhmm("09:3") == 0);
    assert(parse_hhmm("1a30") == 0);   // stoi였다면 130
    assert(parse_hhmm("+930") == 0);   // stoi였다면 930 — 장중으로 오판
    assert(parse_hhmm(" 930") == 0);

    // parse_hhmmss: 여섯 자리 숫자만. 뒤에 붙은 글자는 무시, 앞이 짧거나 숫자가 아니면 0.
    assert(parse_hhmmss("093001") == 93001);
    assert(parse_hhmmss("000000") == 0);
    assert(parse_hhmmss("153000123") == 153000);
    assert(parse_hhmmss("09300") == 0);
    assert(parse_hhmmss("09300a") == 0);
    assert(parse_hhmmss("") == 0);
    assert(hhmmss_str(93001) == "093001");
    assert(hhmmss_str(0) == "000000");
    assert(hhmmss_str(235959) == "235959");
    assert(parse_hhmmss(hhmmss_str(153000)) == 153000);

    // in_session: 09:00 포함, 15:30 제외.
    assert(!in_session(kSessionOpenHHMM - 1));
    assert(in_session(kSessionOpenHHMM));
    assert(in_session(1200));
    assert(in_session(kSessionCloseHHMM - 1));
    assert(!in_session(kSessionCloseHHMM));
    assert(!in_session(0));

    // 문자열 경로는 둘을 잇는다.
    assert(in_session_str("090000"));
    assert(!in_session_str("085959"));
    assert(!in_session_str("153000"));
    assert(!in_session_str(""));

    std::cout << "test_market_session: all passed" << std::endl;
    return 0;
}
