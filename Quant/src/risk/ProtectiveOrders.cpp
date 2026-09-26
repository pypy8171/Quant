#include "risk/ProtectiveOrders.h"

namespace risk
{
void ProtectiveOrderBook::arm(const ProtectiveRule& rule)
{
    if (rule.symbol == symbol::kNone)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!rule.watches_anything())
    {
        remove_locked(rule.account, rule.symbol);
        return;
    }

    if (Entry* existing = find_locked(rule.account, rule.symbol))
    {
        existing->rule = rule; // 조건만 갈아끼운다 — 최고가·재발주 시각은 보유가 이어지는 동안 유지한다
        return;
    }

    Entry entry;
    entry.rule = rule;
    entries_.push_back(std::move(entry));
}

void ProtectiveOrderBook::disarm(const std::string& account, symbol::SymbolId symbol)
{
    std::lock_guard<std::mutex> lock(mutex_);
    remove_locked(account, symbol);
}

bool ProtectiveOrderBook::owns(const std::string& account, symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (mode_ != ProtectiveMode::Owner)
    {
        return false;
    }

    return find_locked(account, symbol) != nullptr;
}

bool ProtectiveOrderBook::consume_fired(const std::string& account, symbol::SymbolId symbol)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = find_locked(account, symbol);

    if (entry == nullptr || !entry->fired_unread)
    {
        return false;
    }

    entry->fired_unread = false;
    return true;
}

std::vector<OrderSignal> ProtectiveOrderBook::evaluate(const std::vector<OrderGate::HeldPos>& held,
                                                       const PriceFn& price_of, const SellPendingFn& sell_pending_of,
                                                       Clock::time_point now)
{
    std::vector<OrderSignal> out;
    std::lock_guard<std::mutex> lock(mutex_);

    if (mode_ == ProtectiveMode::Off || entries_.empty())
    {
        return out;
    }

    for (Entry& entry : entries_)
    {
        const OrderGate::HeldPos* holding = find_holding(held, entry.rule.account, entry.rule.symbol);
        const int position = holding != nullptr ? holding->quantity : 0;

        // 보유가 없으면 이 종목의 기억을 비운다 — 옛 최고가가 남으면 재진입 직후 바로 청산이 나간다
        //  (전략 쪽에서 같은 버그를 겪었다, 09-14 067290).
        if (position <= 0)
        {
            entry.peak_price = 0.0;
            entry.next_try = Clock::time_point{};
            continue;
        }

        const double average_price = holding->average_price;
        const double current_price = price_of ? price_of(entry.rule.symbol) : 0.0;

        if (average_price <= 0.0 || current_price <= 0.0)
        {
            continue; // 평단이나 현재가를 모르면 판단하지 않는다 — 모르는 채 파는 것이 더 나쁘다
        }

        entry.peak_price = std::max(entry.peak_price, current_price);

        const std::string reason = verdict(entry, average_price, current_price);

        if (reason.empty())
        {
            entry.next_try = Clock::time_point{};
            continue;
        }

        if (entry.next_try != Clock::time_point{} && now < entry.next_try)
        {
            continue; // 재발주 간격 안 — 같은 청산을 매 주기 다시 내지 않는다
        }

        entry.next_try = now + retry_interval_;

        if (mode_ == ProtectiveMode::Shadow)
        {
            ++statistics_.shadow_hits;
            LOG_INFO("[보호주문] 그림자 판정 " + entry.rule.ticker + " " + reason +
                     " 보유=" + std::to_string(position) + " (발주는 전략이 한다)");
            continue;
        }

        const int sell_pending = sell_pending_of ? sell_pending_of(entry.rule.account, entry.rule.symbol) : 0;
        const int quantity = position - sell_pending;

        if (quantity <= 0)
        {
            ++statistics_.skipped_sold;
            continue; // 이미 낸 매도가 보유를 덮는다 — 전략이 먼저 냈거나 직전 주기 것이 살아 있다
        }

        OrderSignal signal;
        signal.ticker = entry.rule.ticker;
        signal.symbol_id = entry.rule.symbol;
        signal.account_id = entry.rule.account;
        signal.side = OrderSide::SELL;
        signal.type = OrderType::MARKET;
        signal.quantity = quantity;
        signal.price = 0.0;
        signal.reference_price = current_price;
        signal.strategy_id = entry.rule.owner.empty() ? std::string("PROTECT") : entry.rule.owner;
        signal.strategy_index = entry.rule.owner_index;
        signal.reason = "보호주문:" + reason;
        out.push_back(std::move(signal));

        entry.fired_unread = true;
        ++entry.fired_count;
        ++statistics_.fired;
        LOG_WARN("[보호주문] 청산 " + entry.rule.ticker + " " + reason + " 보유=" + std::to_string(position) +
                 " 미체결매도=" + std::to_string(sell_pending) + " 발주=" + std::to_string(quantity));
    }

    return out;
}

ProtectiveOrderBook::Stats ProtectiveOrderBook::statistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Stats copy = statistics_;
    copy.armed = entries_.size();
    return copy;
}

std::string ProtectiveOrderBook::verdict(const Entry& entry, double average_price, double current_price) const
{
    const ProtectiveRule& rule = entry.rule;

    if (rule.stop_loss_percent > 0.0 && current_price <= average_price * (1.0 - rule.stop_loss_percent / 100.0))
    {
        return "손절(평단 " + format_one_decimal(average_price) + " -" + format_one_decimal(rule.stop_loss_percent) +
               "%)";
    }

    if (rule.trail_arm_percent > 0.0 && rule.trail_percent > 0.0 &&
        entry.peak_price >= average_price * (1.0 + rule.trail_arm_percent / 100.0) &&
        current_price <= entry.peak_price * (1.0 - rule.trail_percent / 100.0))
    {
        return "트레일(최고 " + format_one_decimal(entry.peak_price) + " -" + format_one_decimal(rule.trail_percent) +
               "%)";
    }

    return std::string();
}

std::string ProtectiveOrderBook::format_one_decimal(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return std::string(buffer);
}

const OrderGate::HeldPos* ProtectiveOrderBook::find_holding(const std::vector<OrderGate::HeldPos>& held,
                                                            const std::string& account, symbol::SymbolId symbol)
{
    for (const OrderGate::HeldPos& holding : held)
    {
        if (holding.symbol == symbol && holding.account == account)
        {
            return &holding;
        }
    }

    return nullptr;
}

ProtectiveOrderBook::Entry* ProtectiveOrderBook::find_locked(const std::string& account, symbol::SymbolId symbol)
{
    for (Entry& entry : entries_)
    {
        if (entry.rule.symbol == symbol && entry.rule.account == account)
        {
            return &entry;
        }
    }

    return nullptr;
}

void ProtectiveOrderBook::remove_locked(const std::string& account, symbol::SymbolId symbol)
{
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& entry)
                                  { return entry.rule.symbol == symbol && entry.rule.account == account; }),
                   entries_.end());
}

} // namespace risk
