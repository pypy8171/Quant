// 리플레이용 모의 체결기 — OrderRouter가 KIS 대신 주문을 넣는 IOrderExecutor. 주문은 다음 틱에 체결되고
//  체결통보는 라이브와 같은 콜백으로 나간다. 잔고 대조기에는 자기 장부를 돌려준다.
// 스레드: submit/cancel/revise는 주문 스레드, on_tick은 피드 스레드, balance는 제어 스레드. mtx_ 하나로 지킨다.
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

    void set_fill_callback(FillCb cb)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        on_fill_ = std::move(cb);
    }

    // 접수만 한다. 체결은 그 종목의 다음 틱(on_tick)에서 — 시장가는 틱 가격, 지정가는 가격이 닿을 때.
    //  매도는 보유수량에서 대기 매도를 뺀 만큼만 받고(부족하면 KIS와 같은 40240000), 매수는 현금 한도.
    [[nodiscard]] OrderAck submit_order_ack(const OrderSignal& sig) override
    {
        std::lock_guard<std::mutex> lk(mtx_);

        if (sig.quantity <= 0 || sig.side == OrderSide::NONE)
        {
            return OrderAck::fail("E_PAPER_ARG");
        }

        const double px = sig.price > 0.0 ? sig.price : sig.ref_price;

        if (sig.side == OrderSide::SELL && sellable_locked(sig.ticker) < sig.quantity)
        {
            return OrderAck::fail(kis_err::kNoSellableQty);
        }

        if (sig.side == OrderSide::BUY && px > 0.0 && px * sig.quantity > cash_ - reserved_cash_locked())
        {
            return OrderAck::fail("E_PAPER_CASH");
        }

        Pending p;
        p.odno = next_odno_locked();
        p.sig  = sig;
        pending_[sig.ticker].push_back(p);
        return OrderAck{p.odno, "PAPER", std::string()};
    }

    [[nodiscard]] OrderAck cancel_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int qty, bool all_remaining) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Pending* p = find_locked(ticker, orig_odno);

        if (!p)
        {
            return OrderAck::fail("E_PAPER_NO_ORDER");
        }

        if (all_remaining || qty >= p->sig.quantity)
        {
            erase_locked(ticker, orig_odno);
        }
        else
        {
            p->sig.quantity -= qty;
        }

        return OrderAck{next_odno_locked(), "PAPER", std::string()};
    }

    [[nodiscard]] OrderAck revise_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int new_qty, double new_price) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Pending* p = find_locked(ticker, orig_odno);

        if (!p || new_qty <= 0)
        {
            return OrderAck::fail("E_PAPER_NO_ORDER");
        }

        p->sig.quantity = new_qty;
        p->sig.price    = new_price;
        p->sig.type     = new_price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
        p->odno         = next_odno_locked();
        return OrderAck{p->odno, "PAPER", std::string()};
    }

    [[nodiscard]] bool is_paper() const noexcept override
    {
        return true;
    }

    [[nodiscard]] std::vector<OpenOrder> get_open_orders() override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<OpenOrder>      out;

        for (const auto& [ticker, list] : pending_)
        {
            for (const auto& p : list)
            {
                OpenOrder o;
                o.ticker    = ticker;
                o.odno      = p.odno;
                o.krx_orgno = "PAPER";
                o.psbl_qty  = p.sig.quantity;
                o.ord_unpr  = p.sig.price;
                o.side      = p.sig.side;
                out.push_back(o);
            }
        }

        return out;
    }

    // 피드 스레드가 틱마다 부른다. 그 종목의 대기 주문을 접수 순서대로 보고 조건이 맞으면 체결·통보한다.
    //  콜백은 락을 놓고 부른다(콜백이 큐 push라 짧지만 락 안에서 남의 코드를 부르지 않는다).
    //  종목은 고정 배열 틱에서 오므로 string_view로 받고, 맵 키가 필요할 때만 문자열을 만든다(15자 이하라 SSO).
    void on_tick(std::string_view ticker_sv, double px, int32_t hhmmss)
    {
        std::vector<FillNotification> fills;
        FillCb                        cb;
        const std::string             ticker(ticker_sv);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            last_px_[ticker] = px;
            auto it          = pending_.find(ticker);

            if (it == pending_.end())
            {
                return;
            }

            auto& list = it->second;

            for (auto p = list.begin(); p != list.end();)
            {
                if (!crosses(p->sig, px))
                {
                    ++p;
                    continue;
                }

                apply_fill_locked(p->sig, px);
                FillNotification fn;
                fn.odno         = p->odno;
                fn.ticker       = ticker;
                fn.side         = p->sig.side;
                fn.filled_qty   = p->sig.quantity;
                fn.filled_price = px;
                fn.fill_time    = krx::hhmmss_str(hhmmss);
                fn.timestamp    = std::chrono::system_clock::now();
                fills.push_back(std::move(fn));
                p = list.erase(p);
            }

            if (list.empty())
            {
                pending_.erase(it);
            }

            cb = on_fill_;
        }

        fills_ += fills.size();

        if (cb)
        {
            for (const auto& f : fills)
            {
                cb(f);
            }
        }
    }

    // 잔고 대조기(LedgerReconciler::FetchBalance)용 — KIS 잔고 자리에 자기 장부를 준다.
    [[nodiscard]] KisResult<AccountBalance> balance() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        AccountBalance              b;
        double                      eval = cash_;

        for (const auto& [ticker, h] : book_)
        {
            const auto lp = last_px_.find(ticker);
            const double px = lp != last_px_.end() ? lp->second : h.avg_price;
            Holding      out = h;
            out.eval_pnl     = (px - h.avg_price) * h.qty;
            out.sellable_qty = h.qty - pending_sell_locked(ticker);
            eval += px * h.qty;
            b.holdings.push_back(out);
        }

        b.total_eval_amt       = eval;
        b.available_cash       = cash_ - reserved_cash_locked();
        b.prev_day_total_asset = initial_cash_;
        return b;
    }

    [[nodiscard]] double cash() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return cash_;
    }

    [[nodiscard]] uint64_t fills() const noexcept
    {
        return fills_;
    }

    [[nodiscard]] size_t open_count() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        size_t                      n = 0;

        for (const auto& [t, list] : pending_)
        {
            n += list.size();
        }

        return n;
    }

private:
    struct Pending
    {
        std::string odno;
        OrderSignal sig;
    };

    static bool crosses(const OrderSignal& sig, double px)
    {
        if (sig.type == OrderType::MARKET || sig.price <= 0.0)
        {
            return true;
        }

        return sig.side == OrderSide::BUY ? px <= sig.price : px >= sig.price;
    }

    void apply_fill_locked(const OrderSignal& sig, double px)
    {
        Holding& h = book_[sig.ticker];
        h.ticker   = sig.ticker;

        if (sig.side == OrderSide::BUY)
        {
            const double cost = h.avg_price * h.qty + px * sig.quantity;
            h.qty += sig.quantity;
            h.avg_price = h.qty > 0 ? cost / h.qty : 0.0;
            cash_ -= px * sig.quantity;
        }
        else
        {
            h.qty -= sig.quantity;
            cash_ += px * sig.quantity;
        }

        if (h.qty <= 0)
        {
            book_.erase(sig.ticker);
        }
    }

    int pending_sell_locked(const std::string& ticker) const
    {
        int  n  = 0;
        auto it = pending_.find(ticker);

        if (it == pending_.end())
        {
            return 0;
        }

        for (const auto& p : it->second)
        {
            if (p.sig.side == OrderSide::SELL)
            {
                n += p.sig.quantity;
            }
        }

        return n;
    }

    int sellable_locked(const std::string& ticker) const
    {
        const auto h = book_.find(ticker);
        return (h == book_.end() ? 0 : h->second.qty) - pending_sell_locked(ticker);
    }

    // 대기 매수의 명목 합. 시장가는 ref_price로 잰다(0이면 한도에 안 잡힌다 — 게이트가 먼저 거른다).
    double reserved_cash_locked() const
    {
        double sum = 0.0;

        for (const auto& [t, list] : pending_)
        {
            for (const auto& p : list)
            {
                if (p.sig.side == OrderSide::BUY)
                {
                    const double px = p.sig.price > 0.0 ? p.sig.price : p.sig.ref_price;
                    sum += px * p.sig.quantity;
                }
            }
        }

        return sum;
    }

    Pending* find_locked(const std::string& ticker, const std::string& odno)
    {
        auto it = pending_.find(ticker);

        if (it == pending_.end())
        {
            return nullptr;
        }

        for (auto& p : it->second)
        {
            if (p.odno == odno)
            {
                return &p;
            }
        }

        return nullptr;
    }

    void erase_locked(const std::string& ticker, const std::string& odno)
    {
        auto it = pending_.find(ticker);

        if (it == pending_.end())
        {
            return;
        }

        auto& list = it->second;

        for (auto p = list.begin(); p != list.end(); ++p)
        {
            if (p->odno == odno)
            {
                list.erase(p);
                break;
            }
        }

        if (list.empty())
        {
            pending_.erase(it);
        }
    }

    std::string next_odno_locked()
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "P%09llu", static_cast<unsigned long long>(next_odno_++));
        return buf;
    }

    mutable std::mutex mtx_;
    FillCb             on_fill_;

    std::unordered_map<std::string, std::vector<Pending>> pending_; // ticker → 접수 순서
    std::unordered_map<std::string, Holding>              book_;
    std::unordered_map<std::string, double>               last_px_;
    double                                                cash_;
    double                                                initial_cash_;
    uint64_t                                              next_odno_ = 1;
    std::atomic<uint64_t>                                 fills_{0};
};

} // namespace feed
