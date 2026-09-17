// 리플레이용 모의 체결기 — OrderRouter가 KIS 대신 주문을 넣는 IOrderExecutor. 주문은 다음 틱에 체결되고
//  체결통보는 라이브와 같은 콜백으로 나간다. 잔고 대조기에는 자기 장부를 돌려준다.
// 스레드: submit/cancel/revise는 주문 스레드, on_tick은 피드 스레드, balance는 제어 스레드. mutex_ 하나로 지킨다.
//  체결통보 콜백은 on_tick(피드 스레드)에서만 부른다 — fill_queue_의 생산자를 하나로 두기 위해. [why D-071]
#pragma once
#include "api/IOrderExecutor.h"
#include "api/KisErrorCodes.h"
#include "api/KisResult.h"
#include "api/KisTypes.h"
#include "core/MarketSession.h"
#include "core/Types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace feed
{

class PaperExecutor final : public IOrderExecutor
{
public:
    using FillCb = std::function<void(const FillNotification&)>;

    explicit PaperExecutor(double initial_cash) : cash_(initial_cash), initial_cash_(initial_cash) {}

    void set_fill_callback(FillCb callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_fill_ = std::move(callback);
    }

    // 접수만 한다. 체결은 그 종목의 다음 틱(on_tick)에서 — 시장가는 틱 가격, 지정가는 가격이 닿을 때.
    //  매도는 보유수량에서 대기 매도를 뺀 만큼만 받고(부족하면 KIS와 같은 40240000), 매수는 현금 한도.
    [[nodiscard]] OrderAck submit_order_acknowledgement(const OrderSignal& signal) override
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (signal.quantity <= 0 || signal.side == OrderSide::NONE)
        {
            return OrderAck::fail("E_PAPER_ARG");
        }

        const double price = signal.price > 0.0 ? signal.price : signal.reference_price;

        if (signal.side == OrderSide::SELL && sellable_locked(signal.ticker) < signal.quantity)
        {
            return OrderAck::fail(kis_error::kNoSellableQty);
        }

        if (signal.side == OrderSide::BUY && price > 0.0 && price * signal.quantity > cash_ - reserved_cash_locked())
        {
            return OrderAck::fail("E_PAPER_CASH");
        }

        Pending pending;
        pending.kis_order_no = next_odno_locked();
        pending.signal  = signal;
        pending_[signal.ticker].push_back(pending);
        return OrderAck{pending.kis_order_no, "PAPER", std::string()};
    }

    [[nodiscard]] OrderAck cancel_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int quantity, bool all_remaining) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Pending* pending = find_locked(ticker, orig_odno);

        if (!pending)
        {
            return OrderAck::fail("E_PAPER_NO_ORDER");
        }

        if (all_remaining || quantity >= pending->signal.quantity)
        {
            erase_locked(ticker, orig_odno);
        }
        else
        {
            pending->signal.quantity -= quantity;
        }

        return OrderAck{next_odno_locked(), "PAPER", std::string()};
    }

    [[nodiscard]] OrderAck revise_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int new_quantity, double new_price) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Pending* pending = find_locked(ticker, orig_odno);

        if (!pending || new_quantity <= 0)
        {
            return OrderAck::fail("E_PAPER_NO_ORDER");
        }

        pending->signal.quantity = new_quantity;
        pending->signal.price    = new_price;
        pending->signal.type     = new_price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
        pending->kis_order_no         = next_odno_locked();
        return OrderAck{pending->kis_order_no, "PAPER", std::string()};
    }

    [[nodiscard]] bool is_paper() const noexcept override
    {
        return true;
    }

    [[nodiscard]] std::vector<OpenOrder> get_open_orders() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<OpenOrder>      out;

        for (const auto& [ticker, list] : pending_)
        {
            for (const auto& item : list)
            {
                OpenOrder open_order;
                open_order.ticker    = ticker;
                open_order.kis_order_no      = item.kis_order_no;
                open_order.krx_forwarding_org_no = "PAPER";
                open_order.psbl_qty  = item.signal.quantity;
                open_order.ord_unpr  = item.signal.price;
                open_order.side      = item.signal.side;
                out.push_back(open_order);
            }
        }

        return out;
    }

    // 피드 스레드가 틱마다 부른다. 그 종목의 대기 주문을 접수 순서대로 보고 조건이 맞으면 체결·통보한다.
    //  콜백은 락을 놓고 부른다(콜백이 큐 push라 짧지만 락 안에서 남의 코드를 부르지 않는다).
    //  종목은 고정 배열 틱에서 오므로 string_view로 받고, 맵 키가 필요할 때만 문자열을 만든다(15자 이하라 SSO).
    void on_tick(std::string_view ticker_sv, double price, int32_t hhmmss)
    {
        std::vector<FillNotification> fills;
        FillCb                        callback;
        const std::string             ticker(ticker_sv);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_price_[ticker] = price;
            auto iterator          = pending_.find(ticker);

            if (iterator == pending_.end())
            {
                return;
            }

            auto& list = iterator->second;

            for (auto begin = list.begin(); begin != list.end();)
            {
                if (!crosses(begin->signal, price))
                {
                    ++begin;
                    continue;
                }

                apply_fill_locked(begin->signal, price);
                FillNotification fill_notification;
                fill_notification.kis_order_no         = begin->kis_order_no;
                fill_notification.ticker       = ticker;
                fill_notification.side         = begin->signal.side;
                fill_notification.filled_quantity   = begin->signal.quantity;
                fill_notification.filled_price = price;
                fill_notification.fill_time    = krx::hhmmss_string(hhmmss);
                fill_notification.timestamp    = std::chrono::system_clock::now();
                fills.push_back(std::move(fill_notification));
                begin = list.erase(begin);
            }

            if (list.empty())
            {
                pending_.erase(iterator);
            }

            callback = on_fill_;
        }

        fills_ += fills.size();

        if (callback)
        {
            for (const auto& fill : fills)
            {
                callback(fill);
            }
        }
    }

    // 잔고 대조기(LedgerReconciler::FetchBalance)용 — KIS 잔고 자리에 자기 장부를 준다.
    [[nodiscard]] KisResult<AccountBalance> balance() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AccountBalance              balance;
        double                      evaluation = cash_;

        for (const auto& [ticker, book_entry] : book_)
        {
            const auto last_price_iterator = last_price_.find(ticker);
            const double price = last_price_iterator != last_price_.end() ? last_price_iterator->second : book_entry.average_price;
            Holding      out = book_entry;
            out.evaluation_pnl     = (price - book_entry.average_price) * book_entry.quantity;
            out.sellable_quantity = book_entry.quantity - pending_sell_locked(ticker);
            evaluation += price * book_entry.quantity;
            balance.holdings.push_back(out);
        }

        balance.total_evaluation_amount       = evaluation;
        balance.available_cash       = cash_ - reserved_cash_locked();
        balance.previous_day_total_asset = initial_cash_;
        return balance;
    }

    [[nodiscard]] double cash() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return cash_;
    }

    [[nodiscard]] uint64_t fills() const noexcept
    {
        return fills_;
    }

    [[nodiscard]] size_t open_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t                      count = 0;

        for (const auto& [pending_entry, list] : pending_)
        {
            count += list.size();
        }

        return count;
    }

private:
    struct Pending
    {
        std::string kis_order_no;
        OrderSignal signal;
    };

    static bool crosses(const OrderSignal& signal, double price)
    {
        if (signal.type == OrderType::MARKET || signal.price <= 0.0)
        {
            return true;
        }

        return signal.side == OrderSide::BUY ? price <= signal.price : price >= signal.price;
    }

    void apply_fill_locked(const OrderSignal& signal, double price)
    {
        Holding& holding = book_[signal.ticker];
        holding.ticker   = signal.ticker;

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
            book_.erase(signal.ticker);
        }
    }

    int pending_sell_locked(const std::string& ticker) const
    {
        int  count  = 0;
        auto iterator = pending_.find(ticker);

        if (iterator == pending_.end())
        {
            return 0;
        }

        for (const auto& pending_order : iterator->second)
        {
            if (pending_order.signal.side == OrderSide::SELL)
            {
                count += pending_order.signal.quantity;
            }
        }

        return count;
    }

    int sellable_locked(const std::string& ticker) const
    {
        const auto found = book_.find(ticker);
        return (found == book_.end() ? 0 : found->second.quantity) - pending_sell_locked(ticker);
    }

    // 대기 매수의 명목 합. 시장가는 ref_price로 잰다(0이면 한도에 안 잡힌다 — 게이트가 먼저 거른다).
    double reserved_cash_locked() const
    {
        double sum = 0.0;

        for (const auto& [pending_entry, list] : pending_)
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

    Pending* find_locked(const std::string& ticker, const std::string& kis_order_no)
    {
        auto iterator = pending_.find(ticker);

        if (iterator == pending_.end())
        {
            return nullptr;
        }

        for (auto& pending_order : iterator->second)
        {
            if (pending_order.kis_order_no == kis_order_no)
            {
                return &pending_order;
            }
        }

        return nullptr;
    }

    void erase_locked(const std::string& ticker, const std::string& kis_order_no)
    {
        auto iterator = pending_.find(ticker);

        if (iterator == pending_.end())
        {
            return;
        }

        auto& list = iterator->second;

        for (auto begin = list.begin(); begin != list.end(); ++begin)
        {
            if (begin->kis_order_no == kis_order_no)
            {
                list.erase(begin);
                break;
            }
        }

        if (list.empty())
        {
            pending_.erase(iterator);
        }
    }

    std::string next_odno_locked()
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "P%09llu", static_cast<unsigned long long>(next_odno_++));
        return buffer;
    }

    mutable std::mutex mutex_;
    FillCb             on_fill_;

    std::unordered_map<std::string, std::vector<Pending>> pending_; // ticker → 접수 순서
    std::unordered_map<std::string, Holding>              book_;
    std::unordered_map<std::string, double>               last_price_;
    double                                                cash_;
    double                                                initial_cash_;
    uint64_t                                              next_odno_ = 1;
    std::atomic<uint64_t>                                 fills_{0};
};

} // namespace feed
