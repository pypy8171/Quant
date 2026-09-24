#include "core/PaperExecutor.h"

namespace feed
{
OrderAck PaperExecutor::submit_order_acknowledgement(const OrderSignal& signal)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (signal.quantity <= 0 || signal.side == OrderSide::NONE)
    {
        return OrderAck::fail("E_PAPER_ARG");
    }

    // 신호에 id가 안 찍힌 경로(수동 주문함·테스트)만 여기서 푼다 — 주문마다 한 번이라 비용은 상관없다.
    const symbol::SymbolId symbol_id =
        signal.symbol_id != symbol::kNone ? signal.symbol_id : symbols_.intern(signal.ticker);

    if (symbol_id == symbol::kNone)
    {
        return OrderAck::fail("E_PAPER_SYMBOL");
    }

    const double price = signal.price > 0.0 ? signal.price : signal.reference_price;

    if (signal.side == OrderSide::SELL && sellable_locked(symbol_id) < signal.quantity)
    {
        return OrderAck::fail(kis_error::kNoSellableQty);
    }

    if (signal.side == OrderSide::BUY && price > 0.0 && price * signal.quantity > cash_ - reserved_cash_locked())
    {
        return OrderAck::fail("E_PAPER_CASH");
    }

    Pending pending;
    pending.kis_order_no = next_odno_locked();
    pending.signal = signal;
    pending.signal.symbol_id = symbol_id;
    OrderAck acknowledgement{pending.kis_order_no, "PAPER", std::string()};
    pending_slot_locked(symbol_id).push_back(std::move(pending));
    ++open_orders_;
    return acknowledgement;
}

OrderAck PaperExecutor::cancel_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                     int quantity, bool all_remaining)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const symbol::SymbolId symbol_id = symbols_.lookup(ticker);
    Pending* pending = find_locked(symbol_id, orig_odno);

    if (!pending)
    {
        return OrderAck::fail("E_PAPER_NO_ORDER");
    }

    if (all_remaining || quantity >= pending->signal.quantity)
    {
        erase_locked(symbol_id, orig_odno);
    }
    else
    {
        pending->signal.quantity -= quantity;
    }

    return OrderAck{next_odno_locked(), "PAPER", std::string()};
}

OrderAck PaperExecutor::revise_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                     int new_quantity, double new_price)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Pending* pending = find_locked(symbols_.lookup(ticker), orig_odno);

    if (!pending || new_quantity <= 0)
    {
        return OrderAck::fail("E_PAPER_NO_ORDER");
    }

    pending->signal.quantity = new_quantity;
    pending->signal.price = new_price;
    pending->signal.type = new_price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
    pending->kis_order_no = next_odno_locked();
    return OrderAck{pending->kis_order_no, "PAPER", std::string()};
}

std::vector<OpenOrder> PaperExecutor::get_open_orders()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OpenOrder> out;

    for (const auto& list : pending_)
    {
        for (const auto& item : list)
        {
            OpenOrder open_order;
            open_order.ticker = item.signal.ticker;
            open_order.kis_order_no = item.kis_order_no;
            open_order.krx_forwarding_org_no = "PAPER";
            open_order.psbl_qty = item.signal.quantity;
            open_order.ord_unpr = item.signal.price;
            open_order.side = item.signal.side;
            out.push_back(std::move(open_order));
        }
    }

    return out;
}

void PaperExecutor::on_tick(const TradeData& trade)
{
    const symbol::SymbolId symbol_id =
        trade.symbol_id != symbol::kNone ? trade.symbol_id : symbols_.intern(trade.ticker);

    if (symbol_id == symbol::kNone)
    {
        return;
    }

    std::vector<FillNotification> fills;
    FillCb callback;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (symbol_id >= last_price_.size())
        {
            last_price_.resize(static_cast<size_t>(symbol_id) + 1, 0.0);
        }

        last_price_[symbol_id] = trade.price;

        if (symbol_id >= pending_.size() || pending_[symbol_id].empty())
        {
            return;
        }

        auto& list = pending_[symbol_id];

        for (auto begin = list.begin(); begin != list.end();)
        {
            if (!crosses(begin->signal, trade.price))
            {
                ++begin;
                continue;
            }

            apply_fill_locked(begin->signal, trade.price);
            FillNotification fill_notification;
            fill_notification.kis_order_no = begin->kis_order_no;
            fill_notification.ticker = trade.ticker.string();
            fill_notification.side = begin->signal.side;
            fill_notification.filled_quantity = begin->signal.quantity;
            fill_notification.filled_price = trade.price;
            fill_notification.fill_time = krx::hhmmss_string(trade.hhmmss);
            fill_notification.timestamp = std::chrono::system_clock::now();
            fills.push_back(std::move(fill_notification));
            begin = list.erase(begin);
            --open_orders_;
        }

        callback = on_fill_; // 락 밖에서 부르려 뜬 사본 — set_fill_callback이 사이에 갈아끼워도 안전하다
    }

    fills_ += fills.size();

    if (callback)
    {
        // 수신 스레드 여럿이 여기 같이 오면 한 번에 하나씩 넘긴다(delivery_mutex_ 설명).
        std::lock_guard<std::mutex> delivery_lock(delivery_mutex_);

        for (const auto& fill : fills)
        {
            callback(fill);
        }
    }
}

KisResult<AccountBalance> PaperExecutor::balance() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    AccountBalance balance;
    double evaluation = cash_;

    for (const auto& [symbol_id, book_entry] : book_)
    {
        const double last = symbol_id < last_price_.size() ? last_price_[symbol_id] : 0.0;
        const double price = last > 0.0 ? last : book_entry.average_price;
        Holding& out = balance.holdings.emplace_back(book_entry);
        out.evaluation_pnl = (price - book_entry.average_price) * book_entry.quantity;
        out.sellable_quantity = book_entry.quantity - pending_sell_locked(symbol_id);
        evaluation += price * book_entry.quantity;
    }

    balance.total_evaluation_amount = evaluation;
    balance.available_cash = cash_ - reserved_cash_locked();
    balance.previous_day_total_asset = initial_cash_;
    return balance;
}

bool PaperExecutor::crosses(const OrderSignal& signal, double price)
{
    if (signal.type == OrderType::MARKET || signal.price <= 0.0)
    {
        return true;
    }

    return signal.side == OrderSide::BUY ? price <= signal.price : price >= signal.price;
}

void PaperExecutor::apply_fill_locked(const OrderSignal& signal, double price)
{
    Holding& holding = book_[signal.symbol_id];
    holding.ticker = signal.ticker;

    if (signal.side == OrderSide::BUY)
    {
        const double cost = holding.average_price * holding.quantity + price * signal.quantity;
        holding.quantity += signal.quantity;
        holding.average_price = holding.quantity > 0 ? cost / holding.quantity : 0.0;
        cash_ -= price * signal.quantity;
    }
    else
    {
        holding.quantity -= signal.quantity;
        cash_ += price * signal.quantity;
    }

    if (holding.quantity <= 0)
    {
        book_.erase(signal.symbol_id);
    }
}

std::vector<PaperExecutor::Pending>& PaperExecutor::pending_slot_locked(symbol::SymbolId symbol_id)
{
    if (symbol_id >= pending_.size())
    {
        pending_.resize(static_cast<size_t>(symbol_id) + 1);
    }

    return pending_[symbol_id];
}

int PaperExecutor::pending_sell_locked(symbol::SymbolId symbol_id) const
{
    if (symbol_id >= pending_.size())
    {
        return 0;
    }

    int count = 0;

    for (const auto& pending_order : pending_[symbol_id])
    {
        if (pending_order.signal.side == OrderSide::SELL)
        {
            count += pending_order.signal.quantity;
        }
    }

    return count;
}

int PaperExecutor::sellable_locked(symbol::SymbolId symbol_id) const
{
    const auto found = book_.find(symbol_id);
    return (found == book_.end() ? 0 : found->second.quantity) - pending_sell_locked(symbol_id);
}

double PaperExecutor::reserved_cash_locked() const
{
    double sum = 0.0;

    if (open_orders_ == 0)
    {
        return sum;
    }

    for (const auto& list : pending_)
    {
        for (const auto& item : list)
        {
            if (item.signal.side == OrderSide::BUY)
            {
                const double price = item.signal.price > 0.0 ? item.signal.price : item.signal.reference_price;
                sum += price * item.signal.quantity;
            }
        }
    }

    return sum;
}

PaperExecutor::Pending* PaperExecutor::find_locked(symbol::SymbolId symbol_id, const std::string& kis_order_no)
{
    if (symbol_id == symbol::kNone || symbol_id >= pending_.size())
    {
        return nullptr;
    }

    for (auto& pending_order : pending_[symbol_id])
    {
        if (pending_order.kis_order_no == kis_order_no)
        {
            return &pending_order;
        }
    }

    return nullptr;
}

void PaperExecutor::erase_locked(symbol::SymbolId symbol_id, const std::string& kis_order_no)
{
    if (symbol_id >= pending_.size())
    {
        return;
    }

    auto& list = pending_[symbol_id];

    for (auto begin = list.begin(); begin != list.end(); ++begin)
    {
        if (begin->kis_order_no == kis_order_no)
        {
            list.erase(begin);
            --open_orders_;
            break;
        }
    }
}

std::string PaperExecutor::next_odno_locked()
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "9%09llu", static_cast<unsigned long long>(next_odno_++));
    return buffer;
}

} // namespace feed
