#include "strategy/SurgeHoldPlan.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace surge
{
namespace
{
std::optional<Action> action_of(const std::string& text)
{
    if (text == "BUY_OPEN")
    {
        return Action::BuyOpen;
    }

    if (text == "HOLD")
    {
        return Action::Hold;
    }

    if (text == "EXIT_CLOSE")
    {
        return Action::ExitClose;
    }

    return std::nullopt;
}

double number_or(const nlohmann::json& node, const char* key, double fallback)
{
    const auto found = node.find(key);
    return found != node.end() && found->is_number() ? found->get<double>() : fallback;
}

std::string text_or_empty(const nlohmann::json& node, const char* key)
{
    const auto found = node.find(key);
    return found != node.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::chrono::sys_days days_of(int yyyymmdd)
{
    const std::chrono::year_month_day date{std::chrono::year(yyyymmdd / 10000), std::chrono::month(static_cast<unsigned>(yyyymmdd / 100 % 100)),
                                           std::chrono::day(static_cast<unsigned>(yyyymmdd % 100))};
    return std::chrono::sys_days(date);
}
} // namespace

int date_number(const std::string& text)
{
    std::string digits;

    for (const char character : text)
    {
        if (character >= '0' && character <= '9')
        {
            digits.push_back(character);
        }
        else if (character != '-')
        {
            return 0;
        }
    }

    if (digits.size() != 8)
    {
        return 0;
    }

    return std::stoi(digits);
}

std::optional<Plan> parse_plan(const nlohmann::json& document, std::string& reason)
{
    if (!document.is_object())
    {
        reason = "최상위가 객체가 아님";
        return std::nullopt;
    }

    Plan plan;
    plan.schema       = static_cast<int>(number_or(document, "schema", 0.0));
    plan.as_of        = text_or_empty(document, "as_of");
    plan.generated_at = text_or_empty(document, "generated_at");
    plan.rule         = text_or_empty(document, "rule");

    if (plan.schema != 1)
    {
        reason = "schema가 1이 아님";
        return std::nullopt;
    }

    if (date_number(plan.as_of) == 0)
    {
        reason = "as_of 날짜 형식이 틀림: " + plan.as_of;
        return std::nullopt;
    }

    const auto rows = document.find("rows");

    if (rows == document.end() || !rows->is_array())
    {
        reason = "rows 배열 없음";
        return std::nullopt;
    }

    for (const auto& node : *rows)
    {
        PlanRow row;
        row.ticker = text_or_empty(node, "ticker");
        row.name   = text_or_empty(node, "name");
        const auto action = action_of(text_or_empty(node, "action"));

        if (row.ticker.empty() || !action)
        {
            reason = "종목 또는 action이 틀린 행: " + node.dump();
            return std::nullopt;
        }

        row.action    = *action;
        row.held_days = static_cast<int>(number_or(node, "held_days", 0.0));

        if (row.action == Action::BuyOpen)
        {
            row.signal_date     = text_or_empty(node, "signal_date");
            row.stop_basis_low  = number_or(node, "stop_basis_low", 0.0);
            row.stop_percent    = number_or(node, "stop_pct", 0.0);
            row.take_percent    = number_or(node, "take_pct", 0.0);
            row.reference_close = number_or(node, "reference_close", 0.0);
            row.d0              = text_or_empty(node, "d0");
            row.turnover_share  = number_or(node, "turnover_share", 0.0);

            if (row.stop_basis_low <= 0.0 || row.stop_percent <= 0.0 || row.take_percent <= 0.0 || row.reference_close <= 0.0)
            {
                reason = "BUY_OPEN 행의 손절 기준·손절%·익절%·기준가 중 0 이하: " + row.ticker;
                return std::nullopt;
            }

            const int d0_number     = date_number(row.d0);
            const int signal_number = date_number(row.signal_date);

            if (d0_number == 0 || signal_number == 0 || d0_number > signal_number)
            {
                reason = "BUY_OPEN 행의 d0·signal_date가 없거나 d0가 신호일 뒤: " + row.ticker;
                return std::nullopt;
            }
        }

        plan.rows.push_back(std::move(row));
    }

    // 매수 후보는 거래대금 비중 큰 순 — 파이썬이 같은 순서로 쓰지만 손으로 고친 파일도 같게 집행한다.
    std::stable_sort(plan.rows.begin(), plan.rows.end(), [](const PlanRow& left, const PlanRow& right)
    {
        const bool left_buy  = left.action == Action::BuyOpen;
        const bool right_buy = right.action == Action::BuyOpen;

        if (left_buy != right_buy)
        {
            return left_buy;
        }

        return left_buy && left.turnover_share > right.turnover_share;
    });
    return plan;
}

bool plan_fresh(int as_of_yyyymmdd, int today_yyyymmdd)
{
    if (as_of_yyyymmdd <= 0 || today_yyyymmdd <= 0 || as_of_yyyymmdd >= today_yyyymmdd)
    {
        return false;
    }

    const auto gap = (days_of(today_yyyymmdd) - days_of(as_of_yyyymmdd)).count();
    return gap >= 1 && gap <= 5;
}

double stop_price(double stop_basis_low, double stop_percent)
{
    return stop_basis_low * (1.0 - stop_percent / 100.0);
}

double take_price(double entry_price, double take_percent)
{
    return entry_price * (1.0 + take_percent / 100.0);
}

Exit exit_check(double price, double stop, double take)
{
    if (price <= 0.0)
    {
        return Exit::None;
    }

    if (stop > 0.0 && price <= stop)
    {
        return Exit::Stop;
    }

    if (take > 0.0 && price >= take)
    {
        return Exit::Take;
    }

    return Exit::None;
}

Skip entry_skip(const EntryCheck& check)
{
    if (!check.active)
    {
        return Skip::RiskOff;
    }

    if (check.entry_halted)
    {
        return Skip::EntryHalted;
    }

    if (check.excluded)
    {
        return Skip::Excluded;
    }

    if (check.same_signal)
    {
        return Skip::SameSignal;
    }

    if (check.account_position > 0 || check.account_reserved > 0)
    {
        return Skip::Overlap;
    }

    if (check.open_positions >= check.max_positions)
    {
        return Skip::MaxPositions;
    }

    if (check.bought_today >= check.max_daily_buys)
    {
        return Skip::MaxDailyBuys;
    }

    if (check.quantity <= 0)
    {
        return Skip::NoPrice;
    }

    return Skip::None;
}

const char* skip_text(Skip skip)
{
    switch (skip)
    {
    case Skip::None:
        return "없음";
    case Skip::RiskOff:
        return "약세장(국면 게이트 꺼짐)";
    case Skip::EntryHalted:
        return "계좌 신규매수 차단";
    case Skip::Overlap:
        return "다른 슬리브 보유";
    case Skip::Excluded:
        return "바스켓 소유 종목";
    case Skip::MaxPositions:
        return "보유 종목 수 상한";
    case Skip::MaxDailyBuys:
        return "하루 매수 건수 상한";
    case Skip::NoPrice:
        return "기준가 없음 또는 금액 부족";
    case Skip::SameSignal:
        return "같은 급등 신호를 이미 삼";
    }

    return "?";
}

int buy_quantity(double amount_krw, double reference_price)
{
    if (reference_price <= 0.0 || amount_krw <= 0.0)
    {
        return 0;
    }

    return static_cast<int>(std::floor(amount_krw / reference_price));
}

int weekdays_between(int from_yyyymmdd, int to_yyyymmdd)
{
    if (from_yyyymmdd <= 0 || to_yyyymmdd <= from_yyyymmdd)
    {
        return 0;
    }

    int count = 0;

    for (auto day = days_of(from_yyyymmdd) + std::chrono::days(1); day <= days_of(to_yyyymmdd); day += std::chrono::days(1))
    {
        const unsigned weekday = std::chrono::weekday(day).c_encoding(); // 0 = 일요일

        if (weekday != 0 && weekday != 6)
        {
            ++count;
        }
    }

    return count;
}
} // namespace surge
