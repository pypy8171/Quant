#pragma once
// 보호 주문 표의 주문 쪽 구현 — 규칙(risk/ProtectiveRule.h)과 원장 보유 스냅샷·현재가만으로 청산 주문을 만든다.
//  전략은 이 파일을 모른다 — 전략이 보는 것은 등록 창구(ProtectiveOrderRegistry)뿐이다. [why D-114]
//
//  [lock-order] mutex_ → OrderGate. evaluate()는 mutex_를 쥔 채 sell_pending_of(미체결 매도)를 묻는다.
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
    using SellPendingFn = std::function<int(const std::string&, symbol::SymbolId)>; // 미체결 매도 수량(0 이상)

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

    void arm(const ProtectiveRule& rule) override;

    void disarm(const std::string& account, symbol::SymbolId symbol) override;

    bool owns(const std::string& account, symbol::SymbolId symbol) const override;

    bool consume_fired(const std::string& account, symbol::SymbolId symbol) override;

    // 주문 쪽 한 주기. held는 원장 보유 스냅샷(OrderGate::snapshot_positions), price_of는 현재가,
    //  sell_pending_of는 이미 낸 미체결 매도 수량이다. 낼 것이 없으면 빈 벡터.
    //  shadow 모드는 판정만 세고 빈 벡터를 돌려준다.
    std::vector<OrderSignal> evaluate(const std::vector<OrderGate::HeldPos>& held, const PriceFn& price_of,
                                      const SellPendingFn& sell_pending_of, Clock::time_point now);

    Stats statistics() const;

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
    std::string verdict(const Entry& entry, double average_price, double current_price) const;

    // 로그·사유 문구용 소수 한 자리. 사유는 주문 한 건을 낼 때만 만든다.
    static std::string format_one_decimal(double value);

    static const OrderGate::HeldPos* find_holding(const std::vector<OrderGate::HeldPos>& held, const std::string& account,
                                                  symbol::SymbolId symbol);

    Entry* find_locked(const std::string& account, symbol::SymbolId symbol);

    const Entry* find_locked(const std::string& account, symbol::SymbolId symbol) const
    {
        return const_cast<ProtectiveOrderBook*>(this)->find_locked(account, symbol);
    }

    void remove_locked(const std::string& account, symbol::SymbolId symbol);

    // [inv] 규칙은 수십 건(보유 종목 수)이라 선형 탐색이다. 주기도 초 단위라 hot path가 아니다.
    mutable std::mutex        mutex_;
    std::vector<Entry>        entries_;
    ProtectiveMode            mode_ = ProtectiveMode::Off;
    std::chrono::milliseconds retry_interval_{30000};
    Stats                     statistics_;
};
} // namespace risk
