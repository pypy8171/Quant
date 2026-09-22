#pragma once
// 보호 주문 표의 주문 쪽 구현 — 규칙(risk/ProtectiveRule.h)과 원장 보유 스냅샷·현재가만으로 청산 주문을 만든다.
//  전략은 이 파일을 모른다 — 전략이 보는 것은 등록 창구(ProtectiveOrderRegistry)뿐이다. [why D-114]
//
//  [lock-order] mutex_ → OrderGate. evaluate()는 mutex_를 쥔 채 reserved(미체결 매도)를 묻는다.
//   반대 방향(게이트를 쥐고 이 표를 부르는 길)은 없다.
#include "risk/OrderGate.h"
#include "risk/ProtectiveRule.h"
#include "utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace risk
{
class ProtectiveOrderBook : public ProtectiveOrderRegistry
{
public:
    using Clock      = std::chrono::steady_clock;
    using PriceFn    = std::function<double(symbol::SymbolId)>;                  // 현재가(0=모름)
    using ReservedFn = std::function<int(const std::string&, symbol::SymbolId)>; // 미체결 잔량(음수=매도)

    struct Stats
    {
        std::size_t armed        = 0; // 지금 등록된 규칙 수
        uint64_t    fired        = 0; // owner 모드에서 실제로 낸 청산 주문 수
        uint64_t    shadow_hits  = 0; // shadow 모드에서 "팔았어야 한다"고 판정한 수
        uint64_t    skipped_sold = 0; // 판정은 섰는데 이미 낸 미체결 매도로 낼 것이 없던 수
    };

    ProtectiveOrderBook() = default;

    explicit ProtectiveOrderBook(ProtectiveMode mode) : mode_(mode)
    {
    }

    ProtectiveOrderBook(const ProtectiveOrderBook&)            = delete;
    ProtectiveOrderBook& operator=(const ProtectiveOrderBook&) = delete;

    void set_mode(ProtectiveMode mode)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode;
    }

    ProtectiveMode mode() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mode_;
    }

    bool enabled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mode_ != ProtectiveMode::Off && !entries_.empty();
    }

    // 재발주 간격 — 청산이 안 먹히면(매도가능 0·거부) 이 간격으로 다시 낸다. 전략의 청산 백오프와 같은 뜻이다.
    void set_retry_interval(std::chrono::milliseconds interval)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retry_interval_ = interval;
    }

    void arm(const ProtectiveRule& rule) override
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

    void disarm(const std::string& account, symbol::SymbolId symbol) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remove_locked(account, symbol);
    }

    bool owns(const std::string& account, symbol::SymbolId symbol) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (mode_ != ProtectiveMode::Owner)
        {
            return false;
        }

        return find_locked(account, symbol) != nullptr;
    }

    bool consume_fired(const std::string& account, symbol::SymbolId symbol) override
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

    // 주문 쪽 한 주기. held는 원장 보유 스냅샷(OrderGate::snapshot_positions), price_of는 현재가,
    //  reserved는 미체결 잔량(음수가 이미 낸 매도)이다. 낼 것이 없으면 빈 벡터.
    //  shadow 모드는 판정만 세고 빈 벡터를 돌려준다.
    std::vector<OrderSignal> evaluate(const std::vector<OrderGate::HeldPos>& held, const PriceFn& price_of,
                                      const ReservedFn& reserved, Clock::time_point now)
    {
        std::vector<OrderSignal>    out;
        std::lock_guard<std::mutex> lock(mutex_);

        if (mode_ == ProtectiveMode::Off || entries_.empty())
        {
            return out;
        }

        for (Entry& entry : entries_)
        {
            const OrderGate::HeldPos* holding  = find_holding(held, entry.rule.account, entry.rule.symbol);
            const int                 position = holding != nullptr ? holding->quantity : 0;

            // 보유가 없으면 이 종목의 기억을 비운다 — 옛 최고가가 남으면 재진입 직후 바로 청산이 나간다
            //  (전략 쪽에서 같은 버그를 겪었다, 09-14 067290).
            if (position <= 0)
            {
                entry.peak_price = 0.0;
                entry.next_try   = Clock::time_point{};
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
                LOG_INFO("[보호주문] 그림자 판정 " + entry.rule.ticker + " " + reason + " 보유=" +
                         std::to_string(position) + " (발주는 전략이 한다)");
                continue;
            }

            const int reserved_quantity = reserved ? reserved(entry.rule.account, entry.rule.symbol) : 0;
            const int sell_pending      = reserved_quantity < 0 ? -reserved_quantity : 0;
            const int quantity          = position - sell_pending;

            if (quantity <= 0)
            {
                ++statistics_.skipped_sold;
                continue; // 이미 낸 매도가 보유를 덮는다 — 전략이 먼저 냈거나 직전 주기 것이 살아 있다
            }

            OrderSignal signal;
            signal.ticker          = entry.rule.ticker;
            signal.symbol_id       = entry.rule.symbol;
            signal.account_id      = entry.rule.account;
            signal.side            = OrderSide::SELL;
            signal.type            = OrderType::MARKET;
            signal.quantity        = quantity;
            signal.price           = 0.0;
            signal.reference_price = current_price;
            signal.strategy_id     = entry.rule.owner.empty() ? std::string("PROTECT") : entry.rule.owner;
            signal.strategy_index  = entry.rule.owner_index;
            signal.reason          = "보호주문:" + reason;
            out.push_back(std::move(signal));

            entry.fired_unread = true;
            ++entry.fired_count;
            ++statistics_.fired;
            LOG_WARN("[보호주문] 청산 " + entry.rule.ticker + " " + reason + " 보유=" + std::to_string(position) +
                     " 미체결매도=" + std::to_string(sell_pending) + " 발주=" + std::to_string(quantity));
        }

        return out;
    }

    Stats statistics() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats copy = statistics_;
        copy.armed = entries_.size();
        return copy;
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    struct Entry
    {
        ProtectiveRule    rule;
        double            peak_price = 0.0; // 보유 구간 최고가. 보유가 0이 되면 비운다
        Clock::time_point next_try{};       // 이 시각 전에는 다시 내지 않는다
        bool              fired_unread = false; // 전략이 아직 안 가져간 발사 사실
        uint64_t          fired_count  = 0;
    };

    // 팔 이유. 빈 문자열이면 팔 이유가 없다. 손절을 먼저 본다 — 둘 다 걸리면 더 아픈 쪽을 사유로 남긴다.
    std::string verdict(const Entry& entry, double average_price, double current_price) const
    {
        const ProtectiveRule& rule = entry.rule;

        if (rule.stop_loss_percent > 0.0 && current_price <= average_price * (1.0 - rule.stop_loss_percent / 100.0))
        {
            return "손절(평단 " + format_one_decimal(average_price) + " -" + format_one_decimal(rule.stop_loss_percent) + "%)";
        }

        if (rule.trail_arm_percent > 0.0 && rule.trail_percent > 0.0 &&
            entry.peak_price >= average_price * (1.0 + rule.trail_arm_percent / 100.0) &&
            current_price <= entry.peak_price * (1.0 - rule.trail_percent / 100.0))
        {
            return "트레일(최고 " + format_one_decimal(entry.peak_price) + " -" + format_one_decimal(rule.trail_percent) + "%)";
        }

        return std::string();
    }

    // 로그·사유 문구용 소수 한 자리. 사유는 주문 한 건을 낼 때만 만든다.
    static std::string format_one_decimal(double value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        return std::string(buffer);
    }

    static const OrderGate::HeldPos* find_holding(const std::vector<OrderGate::HeldPos>& held, const std::string& account,
                                                  symbol::SymbolId symbol)
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

    Entry* find_locked(const std::string& account, symbol::SymbolId symbol)
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

    const Entry* find_locked(const std::string& account, symbol::SymbolId symbol) const
    {
        return const_cast<ProtectiveOrderBook*>(this)->find_locked(account, symbol);
    }

    void remove_locked(const std::string& account, symbol::SymbolId symbol)
    {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& entry)
                                      {
                                          return entry.rule.symbol == symbol && entry.rule.account == account;
                                      }),
                       entries_.end());
    }

    // [inv] 규칙은 수십 건(보유 종목 수)이라 선형 탐색이다. 주기도 초 단위라 hot path가 아니다.
    mutable std::mutex        mutex_;
    std::vector<Entry>        entries_;
    ProtectiveMode            mode_ = ProtectiveMode::Off;
    std::chrono::milliseconds retry_interval_{30000};
    Stats                     statistics_;
};
} // namespace risk
