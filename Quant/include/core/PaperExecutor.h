// 리플레이용 모의 체결기 — OrderRouter가 KIS 대신 주문을 넣는 IOrderExecutor. 주문은 다음 틱에 체결되고
//  체결통보는 라이브와 같은 콜백으로 나간다. 잔고 대조기에는 자기 장부를 돌려준다.
// 스레드: submit/cancel/revise는 주문 스레드, on_tick은 피드 스레드, balance는 제어 스레드. mutex_ 하나로 지킨다.
//  체결통보 콜백은 on_tick(피드 스레드)에서만 부른다 — fill_queue_의 생산자를 하나로 두기 위해.
//  장부·대기 주문은 SymbolId로 인덱스한 배열이다 — on_tick은 수신 스레드에서 틱마다 도니 문자열 생성·해시가
//  없어야 한다(원칙 3·6). 문자열 티커는 주문·취소·잔고처럼 드문 경로에서만 SymbolTable로 푼다. 실측(09-20,
//  2,700종목·대기 주문 100건·무작위 틱): 문자열 키 맵 37 ns/틱 → id 배열 14.5 ns/틱, 남은 건 mutex다. [why D-071]
#pragma once
#include "api/IOrderExecutor.h"
#include "api/KisErrorCodes.h"
#include "api/KisResult.h"
#include "api/KisTypes.h"
#include "core/MarketSession.h"
#include "core/SymbolTable.h"
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
#include <utility>
#include <vector>

namespace feed
{

class PaperExecutor final : public IOrderExecutor
{
public:
    using FillCb = std::function<void(const FillNotification&)>;

    // symbols는 엔진의 테이블과 같은 것이어야 한다 — 틱에 찍힌 id와 주문 티커를 푼 id가 같은 번호 체계여야 장부가 맞는다.
    PaperExecutor(double initial_cash, symbol::SymbolTable& symbols)
        : symbols_(symbols), cash_(initial_cash), initial_cash_(initial_cash)
    {
    }

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

        // 신호에 id가 안 찍힌 경로(수동 주문함·테스트)만 여기서 푼다 — 주문마다 한 번이라 비용은 상관없다.
        const symbol::SymbolId symbol_id = signal.symbol_id != symbol::kNone ? signal.symbol_id : symbols_.intern(signal.ticker);

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
        pending.kis_order_no     = next_odno_locked();
        pending.signal           = signal;
        pending.signal.symbol_id = symbol_id;
        OrderAck acknowledgement{pending.kis_order_no, "PAPER", std::string()};
        pending_slot_locked(symbol_id).push_back(std::move(pending));
        ++open_orders_;
        return acknowledgement;
    }

    [[nodiscard]] OrderAck cancel_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int quantity, bool all_remaining) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const symbol::SymbolId      symbol_id = symbols_.lookup(ticker);
        Pending*                    pending   = find_locked(symbol_id, orig_odno);

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

    [[nodiscard]] OrderAck revise_order(const std::string& ticker, const std::string& orig_odno, const std::string&,
                                        int new_quantity, double new_price) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Pending*                    pending = find_locked(symbols_.lookup(ticker), orig_odno);

        if (!pending || new_quantity <= 0)
        {
            return OrderAck::fail("E_PAPER_NO_ORDER");
        }

        pending->signal.quantity = new_quantity;
        pending->signal.price    = new_price;
        pending->signal.type     = new_price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
        pending->kis_order_no    = next_odno_locked();
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

        for (const auto& list : pending_)
        {
            for (const auto& item : list)
            {
                OpenOrder open_order;
                open_order.ticker                = item.signal.ticker;
                open_order.kis_order_no          = item.kis_order_no;
                open_order.krx_forwarding_org_no = "PAPER";
                open_order.psbl_qty              = item.signal.quantity;
                open_order.ord_unpr              = item.signal.price;
                open_order.side                  = item.signal.side;
                out.push_back(std::move(open_order));
            }
        }

        return out;
    }

    // 피드 스레드가 틱마다 부른다. 그 종목의 대기 주문을 접수 순서대로 보고 조건이 맞으면 체결·통보한다.
    //  콜백은 락을 놓고 부른다(콜백이 큐 push라 짧지만 락 안에서 남의 코드를 부르지 않는다).
    //  대기 주문이 없는 종목은 배열 한 칸 보고 돌아간다 — 문자열도 해시도 만들지 않는다. id가 안 찍힌 틱
    //  (테스트·옛 경로)만 테이블로 푼다.
    void on_tick(const TradeData& trade)
    {
        const symbol::SymbolId symbol_id = trade.symbol_id != symbol::kNone ? trade.symbol_id : symbols_.intern(trade.ticker);

        if (symbol_id == symbol::kNone)
        {
            return;
        }

        std::vector<FillNotification> fills;
        FillCb                        callback;

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
                fill_notification.kis_order_no    = begin->kis_order_no;
                fill_notification.ticker          = trade.ticker.string();
                fill_notification.side            = begin->signal.side;
                fill_notification.filled_quantity = begin->signal.quantity;
                fill_notification.filled_price    = trade.price;
                fill_notification.fill_time       = krx::hhmmss_string(trade.hhmmss);
                fill_notification.timestamp       = std::chrono::system_clock::now();
                fills.push_back(std::move(fill_notification));
                begin = list.erase(begin);
                --open_orders_;
            }

            callback = on_fill_; // 락 밖에서 부르려 뜬 사본 — set_fill_callback이 사이에 갈아끼워도 안전하다
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

        for (const auto& [symbol_id, book_entry] : book_)
        {
            const double last  = symbol_id < last_price_.size() ? last_price_[symbol_id] : 0.0;
            const double price = last > 0.0 ? last : book_entry.average_price;
            Holding&     out   = balance.holdings.emplace_back(book_entry);
            out.evaluation_pnl    = (price - book_entry.average_price) * book_entry.quantity;
            out.sellable_quantity = book_entry.quantity - pending_sell_locked(symbol_id);
            evaluation += price * book_entry.quantity;
        }

        balance.total_evaluation_amount  = evaluation;
        balance.available_cash           = cash_ - reserved_cash_locked();
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
        return open_orders_;
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
        Holding& holding = book_[signal.symbol_id];
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
            book_.erase(signal.symbol_id);
        }
    }

    // 그 종목의 대기 목록. 없으면 배열을 늘려 만든다 — 주문 경로에서만 부른다(락 안).
    std::vector<Pending>& pending_slot_locked(symbol::SymbolId symbol_id)
    {
        if (symbol_id >= pending_.size())
        {
            pending_.resize(static_cast<size_t>(symbol_id) + 1);
        }

        return pending_[symbol_id];
    }

    int pending_sell_locked(symbol::SymbolId symbol_id) const
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

    int sellable_locked(symbol::SymbolId symbol_id) const
    {
        const auto found = book_.find(symbol_id);
        return (found == book_.end() ? 0 : found->second.quantity) - pending_sell_locked(symbol_id);
    }

    // 대기 매수의 명목 합. 시장가는 ref_price로 잰다(0이면 한도에 안 잡힌다 — 게이트가 먼저 거른다).
    double reserved_cash_locked() const
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

    Pending* find_locked(symbol::SymbolId symbol_id, const std::string& kis_order_no)
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

    void erase_locked(symbol::SymbolId symbol_id, const std::string& kis_order_no)
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

    // 모의 ODNO — 실전문처럼 자릿수 10개(라우터가 정수로 바꿔 색인한다, D-112). 앞자리 9는 모의 표시.
    std::string next_odno_locked()
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "9%09llu", static_cast<unsigned long long>(next_odno_++));
        return buffer;
    }

    symbol::SymbolTable& symbols_;
    mutable std::mutex   mutex_;
    FillCb               on_fill_;

    std::vector<std::vector<Pending>>             pending_;    // [symbol_id] → 접수 순서. 빈 칸이 대부분이다
    std::unordered_map<symbol::SymbolId, Holding> book_;       // 보유 종목만
    std::vector<double>                           last_price_; // [symbol_id]. 0이면 아직 틱 없음
    size_t                                        open_orders_ = 0; // [inv] pending_ 안 항목 수의 합
    double                                        cash_;
    double                                        initial_cash_;
    uint64_t                                      next_odno_ = 1;
    std::atomic<uint64_t>                         fills_{0};
};

} // namespace feed
