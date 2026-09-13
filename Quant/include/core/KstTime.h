#pragma once
// UTC 초 → KST 분해 시각. 거래일(YYYYMMDD)·틱 시각(HHMMSS)·장 시간 판정·원장 날짜가 같은 변환을 쓴다. [why D-062]
//  달력 산술은 <chrono>로만 한다 — gmtime·localtime 계열을 부르지 않으므로 머신 TZ와 무관하다. [why D-070]
#include <chrono>
#include <ctime>
#include <format>
#include <string>

namespace kst
{
inline constexpr int                kOffsetSec = 9 * 3600;
inline constexpr std::chrono::hours kOffset{9};

// KST 벽시계를 UTC 눈금에 얹은 값. floor<days>·year_month_day·hh_mm_ss 분해의 입력이다.
inline std::chrono::sys_seconds wall(std::time_t now_utc)
{
    return std::chrono::sys_seconds{std::chrono::seconds{now_utc}} + kOffset;
}

inline std::chrono::year_month_day date(std::time_t now_utc)
{
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(wall(now_utc))};
}

// 초를 옮기지 않고 그대로 날짜로 — KST 자리값을 UTC로 읽어 둔 초(parse_dt 결과, KIS 일봉 조회창)용.
inline std::chrono::year_month_day utc_date(std::time_t t)
{
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{std::chrono::seconds{t}})};
}

inline std::chrono::hh_mm_ss<std::chrono::seconds> time_of_day(std::time_t now_utc)
{
    const auto w = wall(now_utc);
    return std::chrono::hh_mm_ss{w - std::chrono::floor<std::chrono::days>(w)};
}

// YYYYMMDD. 날짜만 있는 값(지수 일봉의 UTC 날짜 등)을 같은 모양으로 찍을 때도 쓴다.
inline std::string format_ymd(std::chrono::year_month_day d)
{
    return std::format("{:04}{:02}{:02}", static_cast<int>(d.year()), static_cast<unsigned>(d.month()),
                       static_cast<unsigned>(d.day()));
}

// 주어진 초를 옮기지 않고 struct tm으로 분해한다. tm_isdst는 0.
inline struct tm decompose(std::chrono::sys_seconds w)
{
    using namespace std::chrono;
    const sys_days       d = floor<days>(w);
    const year_month_day ymd{d};
    const hh_mm_ss       hms{w - d};
    struct tm            out{};
    out.tm_year = static_cast<int>(ymd.year()) - 1900;
    out.tm_mon  = static_cast<int>(static_cast<unsigned>(ymd.month())) - 1;
    out.tm_mday = static_cast<int>(static_cast<unsigned>(ymd.day()));
    out.tm_hour = static_cast<int>(hms.hours().count());
    out.tm_min  = static_cast<int>(hms.minutes().count());
    out.tm_sec  = static_cast<int>(hms.seconds().count());
    out.tm_wday = static_cast<int>(weekday{d}.c_encoding());
    out.tm_yday = static_cast<int>((d - sys_days{ymd.year() / January / 1}).count());
    return out;
}

// struct tm 모양이 필요한 곳(봉 슬롯의 tm_yday, 장 시간의 tm_wday)을 위해 남긴다.
inline struct tm to_tm(std::time_t now_utc)
{
    return decompose(wall(now_utc));
}

// 거래일 YYYYMMDD. 손익 기준선 파일·날짜별 표식 파일·원장 CSV 파일명이 쓴다.
inline std::string ymd(std::time_t now_utc)
{
    return format_ymd(date(now_utc));
}

// 틱 시각 HHMMSS. REST 대체 틱의 TradeData.time이 WS 체결(H0STCNT0)과 같은 모양을 갖게 한다.
inline std::string hhmmss(std::time_t now_utc)
{
    const auto t = time_of_day(now_utc);
    return std::format("{:02}{:02}{:02}", t.hours().count(), t.minutes().count(), t.seconds().count());
}

// 원장 CSV 행 시각 "YYYY-MM-DD HH:MM:SS".
inline std::string datetime(std::time_t now_utc)
{
    const auto d = date(now_utc);
    const auto t = time_of_day(now_utc);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(d.year()),
                       static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()), t.hours().count(),
                       t.minutes().count(), t.seconds().count());
}
} // namespace kst
