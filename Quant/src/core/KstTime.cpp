#include "core/KstTime.h"

namespace
{
// struct tm 의 연도 기준점.
constexpr int kTmYearBase = 1900;

constexpr int kSecondsPerHour = 3600;
} // namespace

namespace kst
{
std::chrono::hh_mm_ss<std::chrono::seconds> time_of_day(std::time_t now_utc)
{
    const auto wall_time = wall(now_utc);
    return std::chrono::hh_mm_ss{wall_time - std::chrono::floor<std::chrono::days>(wall_time)};
}

std::string format_ymd(std::chrono::year_month_day year_month_day)
{
    return std::format("{:04}{:02}{:02}", static_cast<int>(year_month_day.year()),
                       static_cast<unsigned>(year_month_day.month()), static_cast<unsigned>(year_month_day.day()));
}

struct tm decompose(std::chrono::sys_seconds sys_seconds)
{
    using namespace std::chrono;
    const sys_days day_start = floor<days>(sys_seconds);
    const year_month_day date_yyyymmdd{day_start};
    const hh_mm_ss time_of_day{sys_seconds - day_start};
    struct tm out{};
    out.tm_year = static_cast<int>(date_yyyymmdd.year()) - kTmYearBase;
    out.tm_mon = static_cast<int>(static_cast<unsigned>(date_yyyymmdd.month())) - 1;
    out.tm_mday = static_cast<int>(static_cast<unsigned>(date_yyyymmdd.day()));
    out.tm_hour = static_cast<int>(time_of_day.hours().count());
    out.tm_min = static_cast<int>(time_of_day.minutes().count());
    out.tm_sec = static_cast<int>(time_of_day.seconds().count());
    out.tm_wday = static_cast<int>(weekday{day_start}.c_encoding());
    out.tm_yday = static_cast<int>((day_start - sys_days{date_yyyymmdd.year() / January / 1}).count());
    return out;
}

std::string hhmmss(std::time_t now_utc)
{
    const auto now_time_of_day = time_of_day(now_utc);
    return std::format("{:02}{:02}{:02}", now_time_of_day.hours().count(), now_time_of_day.minutes().count(),
                       now_time_of_day.seconds().count());
}

int32_t hhmmss_int(std::time_t now_utc)
{
    const auto now_time_of_day = time_of_day(now_utc);
    return static_cast<int32_t>(now_time_of_day.hours().count() * 10000 + now_time_of_day.minutes().count() * 100 +
                                now_time_of_day.seconds().count());
}

std::string datetime(std::time_t now_utc)
{
    const auto today = date(now_utc);
    const auto now_time_of_day = time_of_day(now_utc);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(today.year()),
                       static_cast<unsigned>(today.month()), static_cast<unsigned>(today.day()),
                       now_time_of_day.hours().count(), now_time_of_day.minutes().count(),
                       now_time_of_day.seconds().count());
}

int sec_of_day(std::time_t now_utc)
{
    const struct tm local_time = to_tm(now_utc);
    return local_time.tm_hour * kSecondsPerHour + local_time.tm_min * 60 + local_time.tm_sec;
}

bool kr_market_open(std::time_t now_utc)
{
    const struct tm local_time = to_tm(now_utc);

    if (local_time.tm_wday == 0 || local_time.tm_wday == 6)
    {
        return false;
    }

    // 09:00~20:00 KST — 정규장 09:00~15:30, 장후 종가 15:30~16:00, 애프터마켓 16:00~20:00(2026-09-14 개장).
    const int minute = minute_of_day(local_time);
    return minute >= kKrMarketOpenMinute && minute < kKrAfterMarketCloseMinute;
}

bool us_market_open(std::time_t now_utc)
{
    const struct tm local_time = to_tm(now_utc);
    const int       minute     = minute_of_day(local_time);

    // 자정을 넘는 창이라 요일을 둘로 나눠 본다 — 22:30 이후는 그날(월~금)이 여는 장이고, 05:00 이전은 전날 연 장의
    //  뒷부분이라 화~토 새벽이 열려 있다. 요일 하나로 자르면 금요일 장의 토요일 새벽을 닫힘으로, 월요일 새벽(미국 일요일)을
    //  열림으로 보게 된다.
    const bool opening_weekday = local_time.tm_wday >= 1 && local_time.tm_wday <= 5; // 월~금
    const bool closing_weekday = local_time.tm_wday >= 2 && local_time.tm_wday <= 6; // 화~토
    return (minute >= kUsMarketOpenMinute && opening_weekday) || (minute < kUsMarketCloseMinute && closing_weekday);
}

bool any_market_open(std::time_t now_utc)
{
    return kr_market_open(now_utc) || us_market_open(now_utc);
}

} // namespace kst
