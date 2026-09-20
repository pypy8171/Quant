#include "strategy/TargetBasketPlan.h"
#include <algorithm>
#include <format>
#include <set>

namespace basket
{
std::vector<std::string> Targets::tickers() const
{
    std::set<std::string> unique;

    for (const auto& row : rows)
    {
        unique.insert(row.ticker);
    }

    return {unique.begin(), unique.end()};
}

static std::string normalize_date(std::string text)
{
    std::erase(text, '-');
    return text;
}

std::optional<Targets> parse_targets(const nlohmann::json& document, std::string& error)
{
    if (!document.is_object())
    {
        error = "최상위가 객체가 아님";
        return std::nullopt;
    }

    if (document.value("schema", 0) != 1)
    {
        error = "schema가 1이 아님";
        return std::nullopt;
    }

    Targets targets;
    targets.generated_at  = document.value("generated_at", std::string());
    targets.as_of         = normalize_date(document.value("as_of", std::string()));
    targets.count         = document.value("count", -1);
    targets.liquidate_all = document.value("liquidate_all", false);

    if (targets.as_of.size() != 8)
    {
        error = "as_of가 YYYY-MM-DD가 아님";
        return std::nullopt;
    }

    const auto sleeves_node = document.find("sleeves");

    if (sleeves_node == document.end() || !sleeves_node->is_object())
    {
        error = "sleeves 없음";
        return std::nullopt;
    }

    for (const auto& [name, node] : sleeves_node->items())
    {
        Sleeve sleeve;
        sleeve.share            = node.value("share", 0.0);
        sleeve.is_rebalance_day = node.value("is_rebalance_day", false);
        targets.sleeves.emplace(name, sleeve);
    }

    const auto rows_node = document.find("targets");

    if (rows_node == document.end() || !rows_node->is_array())
    {
        error = "targets 배열 없음";
        return std::nullopt;
    }

    std::map<std::string, double> weight_sum;

    for (const auto& node : *rows_node)
    {
        TargetRow row;
        row.ticker          = node.value("ticker", std::string());
        row.name            = node.value("name", std::string());
        row.sleeve          = node.value("sleeve", std::string());
        row.weight          = node.value("weight", 0.0);
        row.reference_price = node.value("reference_price", 0.0);
        row.action          = node.value("action", std::string("KEEP"));
        row.reason          = node.value("reason", std::string());

        if (row.ticker.empty())
        {
            error = "ticker 빈 행";
            return std::nullopt;
        }

        if (!targets.sleeves.contains(row.sleeve))
        {
            error = "모르는 슬리브 " + row.sleeve + " (" + row.ticker + ")";
            return std::nullopt;
        }

        if (row.action == "DROP")
        {
            row.weight = 0.0;
        }
        else
        {
            if (row.reference_price <= 0.0)
            {
                error = "reference_price 없음 (" + row.ticker + ")";
                return std::nullopt;
            }

            weight_sum[row.sleeve] += row.weight;
        }

        targets.rows.push_back(std::move(row));
    }

    if (targets.count != static_cast<int>(targets.rows.size()))
    {
        error = std::format("count {} != 행 수 {}", targets.count, targets.rows.size());
        return std::nullopt;
    }

    for (const auto& [sleeve, sum] : weight_sum)
    {
        if (std::abs(sum - 1.0) > 0.005)
        {
            error = std::format("{} weight 합 {:.4f} (1±0.005 밖)", sleeve, sum);
            return std::nullopt;
        }
    }

    return targets;
}

namespace
{
struct TickerTarget
{
    double                   target_krw      = 0.0;
    double                   reference_price = 0.0;
    bool                     drop            = false; // DROP 행이 하나라도 있고 목표가 0이면 판다
    bool                     rebalance_day   = false; // 어느 슬리브든 리밸 날이면 밴드 규칙으로 맞춘다
    std::vector<std::string> sleeves;
    std::string              reason;
};
} // namespace

Plan make_plan(const Targets& targets, const PlanInput& input)
{
    Plan plan;

    // 1. 순자산 — 보유 평가손익은 기준가로 잰다(틱이 없는 종목이 대부분).
    std::map<std::string, Holding> holdings;
    double unrealized = 0.0;

    for (const auto& ticker : targets.tickers())
    {
        Holding holding = input.holding ? input.holding(ticker) : Holding{};
        holdings.emplace(ticker, holding);
    }

    std::map<std::string, TickerTarget> per_ticker;

    for (const auto& row : targets.rows)
    {
        TickerTarget& target = per_ticker[row.ticker];

        if (row.reference_price > 0.0)
        {
            target.reference_price = row.reference_price;
        }

        // DROP 행의 슬리브도 리밸 날로 친다 — 한 슬리브가 빼고 다른 슬리브가 KEEP인 종목을 그날 줄이기 위해.
        target.rebalance_day = target.rebalance_day || targets.sleeves.at(row.sleeve).is_rebalance_day;

        if (row.action == "DROP")
        {
            target.drop = true;
        }
        else
        {
            target.sleeves.push_back(row.sleeve);
        }

        if (!row.reason.empty())
        {
            target.reason = row.reason;
        }
    }

    for (const auto& [ticker, holding] : holdings)
    {
        const double reference_price = per_ticker[ticker].reference_price;

        if (holding.quantity > 0 && reference_price > 0.0 && holding.average_price > 0.0)
        {
            unrealized += holding.quantity * (reference_price - holding.average_price);
        }
    }

    plan.nav = input.capital_krw + input.realized_pnl_krw + unrealized;

    // 2. 종목 목표 금액 = 순자산 × Σ(share × weight)
    for (const auto& row : targets.rows)
    {
        if (row.action == "DROP")
        {
            continue;
        }

        per_ticker[row.ticker].target_krw += plan.nav * targets.sleeves.at(row.sleeve).share * row.weight;
    }

    // 3. 차이 → 주문
    for (const auto& [ticker, target] : per_ticker)
    {
        const Holding& holding = holdings[ticker];
        std::string    strategy_id = "BASKET_";

        for (size_t index = 0; index < target.sleeves.size(); ++index)
        {
            strategy_id += (index > 0 ? "+" : "") + target.sleeves[index];
        }

        if (target.sleeves.empty())
        {
            strategy_id += "DROP";
        }

        if (targets.liquidate_all || (target.drop && target.target_krw <= 0.0))
        {
            if (holding.quantity <= 0)
            {
                continue;
            }

            const int quantity = std::min(holding.quantity, holding.sellable);

            if (quantity <= 0)
            {
                plan.notes.push_back(ticker + " 매도가능 0 — 매도 미룸");
                continue;
            }

            plan.sells.push_back({ticker, strategy_id, OrderSide::SELL, quantity, target.reference_price,
                                  targets.liquidate_all ? "liquidate_all" : "DROP " + target.reason});
            continue;
        }

        if (target.reference_price <= 0.0)
        {
            continue;
        }

        const int target_quantity = static_cast<int>(std::floor(target.target_krw / target.reference_price));
        const int difference      = target_quantity - holding.quantity;

        if (difference == 0)
        {
            continue;
        }

        const double drift_krw = std::abs(difference) * target.reference_price;
        bool         should_act = false;
        std::string  why;

        if (holding.quantity <= 0 && difference > 0)
        {
            should_act = true;
            why = "신규 0→목표";
        }
        else if (target.rebalance_day)
        {
            should_act = drift_krw >= input.band * target.target_krw;
            why = std::format("리밸 날 차이 {:.0f}원 ≥ 밴드 {:.0f}원", drift_krw, input.band * target.target_krw);
        }
        else if (difference > 0)
        {
            should_act = holding.quantity < target_quantity * (1.0 - input.band);
            why = "미충족 채우기";
        }

        if (!should_act)
        {
            plan.notes.push_back(std::format("{} 밴드 안(차이 {}주) — 유지", ticker, difference));
            continue;
        }

        if (difference > 0)
        {
            plan.buys.push_back({ticker, strategy_id, OrderSide::BUY, difference, target.reference_price, why});
        }
        else
        {
            const int quantity = std::min(-difference, holding.sellable);

            if (quantity <= 0)
            {
                plan.notes.push_back(ticker + " 매도가능 0 — 매도 미룸");
                continue;
            }

            plan.sells.push_back({ticker, strategy_id, OrderSide::SELL, quantity, target.reference_price, why});
        }
    }

    return plan;
}
} // namespace basket
