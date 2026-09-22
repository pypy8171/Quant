#include "strategy/StrategyBase.h"

// placeholder

int StrategyBase::confirmed_position(const std::string& account, symbol::SymbolId symbol,
                                     const std::string& ticker) const
{
    if (symbol != symbol::kNone && position_provider_by_id_)
    {
        return position_provider_by_id_(account, symbol);
    }

    return confirmed_position(account, ticker);
}

std::optional<StrategyBase::SellableInfo> StrategyBase::ledger_sellable(const std::string& account,
                                                                        const std::string& ticker) const
{
    if (!sellable_provider_)
    {
        return std::nullopt;
    }

    return sellable_provider_(account, ticker);
}

void StrategyBase::arm_protective(const std::string& account, const std::string& ticker, symbol::SymbolId symbol,
                                  double stop_loss_percent, double trail_arm_percent, double trail_percent)
{
    if (protective_registry_ == nullptr)
    {
        return;
    }

    risk::ProtectiveRule rule;
    rule.account = account;
    rule.ticker = ticker;
    rule.symbol = symbol;
    rule.stop_loss_percent = stop_loss_percent;
    rule.trail_arm_percent = trail_arm_percent;
    rule.trail_percent = trail_percent;
    rule.owner = id();
    rule.owner_index = strategy_index_;
    protective_registry_->arm(rule);
}

void StrategyBase::disarm_protective(const std::string& account, symbol::SymbolId symbol)
{
    if (protective_registry_ != nullptr)
    {
        protective_registry_->disarm(account, symbol);
    }
}

bool StrategyBase::same_symbol(symbol::SymbolId symbol_id_a, std::string_view a_ticker, symbol::SymbolId symbol_id_b,
                               std::string_view b_ticker)
{
    if (symbol_id_a != symbol::kNone && symbol_id_b != symbol::kNone)
    {
        return symbol_id_a == symbol_id_b;
    }

    return a_ticker == b_ticker;
}
