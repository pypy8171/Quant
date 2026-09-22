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

} // namespace kst
