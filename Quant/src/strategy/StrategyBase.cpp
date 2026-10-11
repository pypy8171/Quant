#include "strategy/StrategyBase.h"
#include "core/KstTime.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace
{
constexpr int kHourToHhmm = 100; // HHMM 정수에서 시가 차지하는 자리값
} // namespace

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

std::optional<StrategyBase::SellableInfo> StrategyBase::ledger_sellable(const std::string& account, symbol::SymbolId symbol,
                                                                        const std::string& ticker) const
{
    if (symbol != symbol::kNone && sellable_provider_by_id_)
    {
        return sellable_provider_by_id_(account, symbol);
    }

    return ledger_sellable(account, ticker);
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

std::string StrategyBase::make_order_id(std::string_view tag, uint64_t sequence) const
{
    constexpr size_t kSequenceDigitsMax = 20; // uint64_t 최댓값의 자릿수
    constexpr size_t kSeparatorCount    = 2;  // ':' 두 개
    char             digits[kSequenceDigitsMax];
    const auto       converted = std::to_chars(digits, digits + kSequenceDigitsMax, sequence);

    const std::string& strategy_id = id();
    std::string        order_id;
    order_id.reserve(strategy_id.size() + tag.size() + kSeparatorCount + static_cast<size_t>(converted.ptr - digits));
    order_id.append(strategy_id).append(1, ':').append(tag).append(1, ':').append(digits, converted.ptr);
    return order_id;
}

OrderSignal StrategyBase::make_signal(std::string_view ticker, symbol::SymbolId symbol, OrderSide side, OrderType type,
                                      int quantity, std::chrono::system_clock::time_point timestamp) const
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.symbol_id   = symbol;
    signal.side        = side;
    signal.type        = type;
    signal.quantity    = quantity;
    signal.strategy_id = id();
    signal.market      = Market::KR;
    signal.timestamp   = timestamp;
    return signal;
}

int StrategyBase::kst_hhmm(std::time_t now_utc)
{
    const auto time_of_day = kst::time_of_day(now_utc);
    return static_cast<int>(time_of_day.hours().count()) * kHourToHhmm + static_cast<int>(time_of_day.minutes().count());
}
