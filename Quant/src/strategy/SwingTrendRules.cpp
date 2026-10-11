// strategy/SwingTrendRules.cpp — 스윙 추세 슬리브의 순수 판정(계획 검증·매수·손절선·매도·계획 날짜) 구현.
//  스레드: 상태가 없다. 관련 결정: D-160(일봉 신호 다일 보유 슬리브).
#include "strategy/SwingTrendRules.h"
#include "strategy/SurgeHoldPlan.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace swing_trend
{
namespace
{
// 정수 원 값을 읽는다. 숫자가 아니거나 소수부가 있으면 nullopt.
std::optional<int64_t> whole_won(const nlohmann::json& node, const char* key)
{
    const auto found = node.find(key);

    if (found == node.end() || !found->is_number())
    {
        return std::nullopt;
    }

    const double value = found->get<double>();

    if (std::floor(value) != value)
    {
        return std::nullopt;
    }

    return static_cast<int64_t>(value);
}

std::optional<std::chrono::sys_days> calendar_day(int yyyymmdd)
{
    if (yyyymmdd <= 0)
    {
        return std::nullopt;
    }

    const std::chrono::year_month_day day{std::chrono::year{yyyymmdd / 10000},
                                          std::chrono::month{static_cast<unsigned>(yyyymmdd / 100 % 100)},
                                          std::chrono::day{static_cast<unsigned>(yyyymmdd % 100)}};

    if (!day.ok())
    {
        return std::nullopt;
    }

    return std::chrono::sys_days{day};
}
} // namespace

std::optional<Plan> parse_plan(const nlohmann::json& document, std::string& reason)
{
    if (!document.is_object())
    {
        reason = "최상위가 객체가 아님";
        return std::nullopt;
    }

    Plan plan;
    const auto as_of = document.find("as_of");

    if (as_of == document.end() || !as_of->is_string() || surge::date_number(as_of->get<std::string>()) == 0)
    {
        reason = "as_of 날짜가 없거나 형식이 틀림";
        return std::nullopt;
    }

    plan.as_of = as_of->get<std::string>();
    const auto generated_at = document.find("generated_at");

    if (generated_at != document.end() && generated_at->is_string())
    {
        plan.generated_at = generated_at->get<std::string>();
    }

    const auto entries = document.find("entries");

    if (entries == document.end() || !entries->is_array())
    {
        reason = "entries 배열 없음";
        return std::nullopt;
    }

    for (const auto& node : *entries)
    {
        if (!node.is_object())
        {
            reason = "entries 행이 객체가 아님";
            return std::nullopt;
        }

        const auto ticker = node.find("ticker");

        if (ticker == node.end() || !ticker->is_string() || ticker->get_ref<const std::string&>().size() != 6)
        {
            reason = "종목 코드가 6자리 문자열이 아님 " + node.dump();
            return std::nullopt;
        }

        const auto entry_price  = whole_won(node, "entry_price");
        const auto cancel_below = whole_won(node, "cancel_below");

        if (!entry_price || !cancel_below || *entry_price <= 0 || *cancel_below <= 0 || *cancel_below >= *entry_price)
        {
            reason = "entry_price·cancel_below가 양의 정수가 아니거나 손절선이 매수가 이상 " + node.dump();
            return std::nullopt;
        }

        PlanEntry entry;
        entry.ticker       = ticker->get<std::string>();
        entry.entry_price  = *entry_price;
        entry.cancel_below = *cancel_below;
        const auto name    = node.find("name");

        if (name != node.end() && name->is_string())
        {
            entry.name = name->get<std::string>();
        }

        const auto signal_date = node.find("signal_date");

        if (signal_date != node.end() && signal_date->is_string())
        {
            entry.signal_date = signal_date->get<std::string>();
        }

        const bool duplicate = std::any_of(plan.entries.begin(), plan.entries.end(), [&entry](const PlanEntry& existing)
        {
            return existing.ticker == entry.ticker;
        });

        if (duplicate)
        {
            reason = "같은 종목이 두 번 나옴 " + entry.ticker;
            return std::nullopt;
        }

        plan.entries.push_back(std::move(entry));
    }

    return plan;
}

bool should_buy(double open_price, int64_t entry_price)
{
    return entry_price > 0 && open_price > static_cast<double>(entry_price);
}

int buy_quantity(int64_t notional_krw, int64_t entry_price)
{
    if (entry_price <= 0 || notional_krw <= 0)
    {
        return 0;
    }

    return static_cast<int>(notional_krw / entry_price);
}

bool is_extreme_low(const std::vector<int64_t>& lows, size_t day, int window)
{
    if (day >= lows.size() || lows[day] <= 0 || window < 0)
    {
        return false;
    }

    const int64_t center = lows[day];
    const size_t  width  = static_cast<size_t>(window);
    const size_t  first  = day >= width ? day - width : 0;
    const size_t  last   = std::min(lows.size() - 1, day + width);

    for (size_t index = first; index <= last; ++index)
    {
        if (lows[index] > 0 && lows[index] < center)
        {
            return false;
        }
    }

    return true;
}

int64_t raised_stop(const std::vector<int64_t>& lows, size_t entry_index, int window, int64_t current_stop)
{
    if (window <= 0)
    {
        return current_stop;
    }

    const int64_t width      = window;
    const int64_t bar_count  = static_cast<int64_t>(lows.size());
    const int64_t first      = std::max(static_cast<int64_t>(entry_index) - width, width);
    const int64_t last       = bar_count - 1 - width;
    int64_t       stop       = current_stop;

    for (int64_t candidate = first; candidate <= last; ++candidate)
    {
        const size_t day = static_cast<size_t>(candidate);

        if (is_extreme_low(lows, day, window) && lows[day] > stop)
        {
            stop = lows[day];
        }
    }

    return stop;
}

size_t entry_bar_index(const std::vector<int32_t>& session_dates, int entry_date)
{
    const auto found = std::lower_bound(session_dates.begin(), session_dates.end(), entry_date);
    return static_cast<size_t>(found - session_dates.begin());
}

bool should_sell(int hhmm, double price, int64_t stop, int sell_start_hhmm, int sell_end_hhmm)
{
    if (hhmm < sell_start_hhmm || hhmm > sell_end_hhmm || price <= 0.0 || stop <= 0)
    {
        return false;
    }

    return price < static_cast<double>(stop);
}

bool plan_stale(int as_of_yyyymmdd, int today_yyyymmdd, int max_days)
{
    const auto as_of = calendar_day(as_of_yyyymmdd);
    const auto today = calendar_day(today_yyyymmdd);

    if (!as_of || !today)
    {
        return true;
    }

    return (*today - *as_of).count() > max_days;
}
} // namespace swing_trend
