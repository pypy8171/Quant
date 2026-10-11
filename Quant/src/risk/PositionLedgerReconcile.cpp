// 주문 장부 — 잔고 대조 쪽 갱신: 시드, 당일 손익·자본·현금·시가 주입, 유령 슬롯 정리, 매도가능·놓친 체결 맞추기.
#include "risk/PositionLedger.h"
#include <algorithm>

void PositionLedger::set_daily_pnl(double pnl)
{
    {
        std::lock_guard<std::mutex> lock(pnl_mutex_);
        daily_pnl_ = pnl;
    }

    ledger_journal::Record record;
    record.kind = static_cast<uint16_t>(ledger_journal::Kind::DAILY_PNL);
    record.pnl  = pnl;
    journal_append(record, std::string_view(), std::string_view());
    journal_flush();
}

void PositionLedger::set_equity(double equity)
{
    equity_.store(equity, std::memory_order_relaxed);
    ledger_journal::Record record;
    record.kind   = static_cast<uint16_t>(ledger_journal::Kind::CASH);
    record.cash   = available_cash_.load(std::memory_order_relaxed);
    record.equity = equity;
    journal_append(record, std::string_view(), std::string_view());
    journal_flush();
}

void PositionLedger::replace_mark_prices(const std::string& account, const std::vector<std::pair<std::string, double>>& marks)
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const uint32_t              account_id = keys_.account_index(account, true);
    std::erase_if(mark_prices_, [account_id](const auto& entry)
    {
        return entry.first.account == account_id;
    });

    for (const auto& [ticker, price] : marks)
    {
        if (price > 0.0)
        {
            mark_prices_[keys_.make(account, ticker)] = price;
        }
    }
}

void PositionLedger::set_available_cash(double available_cash)
{
    available_cash_.store(available_cash, std::memory_order_relaxed);
    ledger_journal::Record record;
    record.kind   = static_cast<uint16_t>(ledger_journal::Kind::CASH);
    record.cash   = available_cash;
    record.equity = equity_.load(std::memory_order_relaxed);
    journal_append(record, std::string_view(), std::string_view());
    journal_flush();
}

// ─── 유령 슬롯 정리 ─────────────────────────────────────────────────────────
std::vector<symbol::SymbolId> PositionLedger::prune_positions(const std::vector<std::string>& live_tickers, int min_age_sec)
{
    std::vector<symbol::SymbolId> gone;
    const std::vector<bool>  live = keys_.live_symbols(live_tickers);
    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const auto now = Clock::now();

    for (auto iterator = positions_.begin(); iterator != positions_.end();)
    {
        if (iterator->second <= 0 || live[iterator->first.symbol])
        {
            ++iterator;
            continue;
        }

        // 방금 열린 포지션은 남긴다. 잔고 조회가 체결보다 먼저 떠난 왕복이면 잔고에 아직
        //  안 보이는데, 그걸 지우면 전략이 미보유로 읽고 같은 종목을 또 산다.
        auto opened_iterator = opened_at_.find(iterator->first);

        if (opened_iterator != opened_at_.end() && min_age_sec > 0 &&
            now - opened_iterator->second < std::chrono::seconds(min_age_sec))
        {
            ++iterator;
            continue;
        }

        const PosKey key = iterator->first;
        gone.push_back(key.symbol);
        average_prices_.erase(key);
        opened_at_.erase(key);
        sellable_.erase(key);
        iterator = positions_.erase(iterator);
        journal_adjust(key, "prune_positions");
    }

    return gone;
}

std::vector<std::string> PositionLedger::prune_reservations(const std::vector<std::string>& live_tickers)
{
    return prune_reservations(keys_.live_symbols(live_tickers));
}

std::vector<std::string> PositionLedger::prune_reservations(const std::vector<bool>& live)
{
    std::vector<std::string>    gone;
    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);

    for (auto iterator = reserved_.begin(); iterator != reserved_.end();)
    {
        if (iterator->first.symbol < live.size() && live[iterator->first.symbol])
        {
            ++iterator;
            continue;
        }

        const PosKey key = iterator->first;
        gone.emplace_back(keys_.ticker_of(key).view());
        reserved_price_.erase(key);
        iterator = reserved_.erase(iterator);
        journal_adjust(key, "prune_reservations");
    }

    return gone;
}

void PositionLedger::restore_sellable(const std::string& account, const std::string& ticker, int quantity)
{
    if (quantity <= 0)
    {
        return;
    }

    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = keys_.make(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= 0)
    {
        return;   // 장부가 모르는 보유는 손대지 않는다 — 없는 매도가능수량을 만들어 낼 이유가 없다
    }

    auto strategy_iterator = sellable_.find(key);
    const int current = (strategy_iterator != sellable_.end()) ? strategy_iterator->second : 0;
    const int restored = current + quantity;
    sellable_[key] = (restored > position_iterator->second) ? position_iterator->second : restored;
    journal_adjust(key, "restore_sellable");
}

PositionLedger::SellableView PositionLedger::sellable_view(const std::string& account, const std::string& ticker) const
{
    SellableView sellable_view;
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = keys_.lookup(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= 0)
    {
        return sellable_view;
    }

    sellable_view.held     = position_iterator->second;
    sellable_view.possible_quantity_cap = sellable_view.held;
    auto strategy_iterator = sellable_.find(key);

    if (strategy_iterator != sellable_.end() && strategy_iterator->second < sellable_view.possible_quantity_cap)
    {
        sellable_view.possible_quantity_cap = strategy_iterator->second;
    }

    auto reserved_iterator = reserved_.find(key);
    sellable_view.pending = (reserved_iterator != reserved_.end()) ? reserved_iterator->second.sell : 0;
    return sellable_view;
}

void PositionLedger::refresh_sellable(const std::string& account, const std::string& ticker, int ord_psbl_qty)
{
    if (ord_psbl_qty < 0)
    {
        return;
    }

    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = keys_.make(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= 0)
    {
        return;
    }

    auto reserved_iterator = reserved_.find(key);
    const int sell_pending = (reserved_iterator != reserved_.end()) ? reserved_iterator->second.sell : 0;
    const int sellable     = ord_psbl_qty + sell_pending;
    sellable_[key] = (sellable > position_iterator->second) ? position_iterator->second : sellable;
    journal_adjust(key, "refresh_sellable");
}

int PositionLedger::absorb_missed_sell(const std::string& account, const std::string& ticker, int balance_quantity)
{
    if (balance_quantity < 0)
    {
        return 0;
    }

    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = keys_.make(account, ticker);
    auto position_iterator = positions_.find(key);

    if (position_iterator == positions_.end() || position_iterator->second <= balance_quantity)
    {
        missed_sell_seen_.erase(key);
        return 0;
    }

    const int difference = position_iterator->second - balance_quantity;
    auto reserved_iterator = reserved_.find(key);
    const int sell_pending = (reserved_iterator != reserved_.end()) ? reserved_iterator->second.sell : 0;

    // 미체결 매도보다 큰 차이는 놓친 체결로 설명되지 않는다 — 손대지 않고 로그 관찰에 맡긴다.
    if (difference > sell_pending)
    {
        missed_sell_seen_.erase(key);
        return 0;
    }

    auto seen = missed_sell_seen_.find(key);

    if (seen == missed_sell_seen_.end() || seen->second != balance_quantity)
    {
        missed_sell_seen_[key] = balance_quantity; // 첫 관측 — 다음 대조에서 같으면 맞춘다
        return 0;
    }

    missed_sell_seen_.erase(key);
    position_iterator->second = balance_quantity;
    reserved_iterator->second.sell -= difference;

    if (reserved_iterator->second.empty())
    {
        reserved_.erase(reserved_iterator);
    }

    auto sellable_iterator = sellable_.find(key);

    if (sellable_iterator != sellable_.end() && sellable_iterator->second > balance_quantity)
    {
        sellable_iterator->second = balance_quantity;
    }

    journal_adjust(key, "absorb_missed_sell");
    return difference;
}

int PositionLedger::absorb_missed_buy(const std::string& account, const std::string& ticker, int balance_quantity,
                                      double balance_average)
{
    if (balance_quantity <= 0)
    {
        return 0;
    }

    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = keys_.make(account, ticker);
    auto position_iterator = positions_.find(key);
    const int ledger_quantity = position_iterator != positions_.end() ? position_iterator->second : 0;

    if (ledger_quantity >= balance_quantity)
    {
        missed_buy_seen_.erase(key);
        return 0;
    }

    const int difference = balance_quantity - ledger_quantity;
    auto reserved_iterator = reserved_.find(key);
    const int buy_pending = (reserved_iterator != reserved_.end()) ? reserved_iterator->second.buy : 0;

    // 미체결 매수보다 큰 차이는 놓친 체결로 설명되지 않는다(밖에서 산 것 등) — 손대지 않는다.
    if (difference > buy_pending)
    {
        missed_buy_seen_.erase(key);
        return 0;
    }

    auto seen = missed_buy_seen_.find(key);

    if (seen == missed_buy_seen_.end() || seen->second != balance_quantity)
    {
        missed_buy_seen_[key] = balance_quantity; // 첫 관측 — 다음 대조에서 같으면 맞춘다
        return 0;
    }

    missed_buy_seen_.erase(key);
    positions_[key] = balance_quantity;

    if (balance_average > 0.0)
    {
        average_prices_[key] = balance_average;
    }

    if (ledger_quantity <= 0)
    {
        opened_at_[key] = Clock::now();
    }

    reserved_iterator->second.buy -= difference;

    if (reserved_iterator->second.buy == 0)
    {
        reserved_price_.erase(key);
    }

    if (reserved_iterator->second.empty())
    {
        reserved_.erase(reserved_iterator);
    }

    // 당일 매수분은 당일 매도 가능하다(on_fill_confirmed BUY와 같다).
    auto sellable_iterator = sellable_.find(key);
    sellable_[key] = (sellable_iterator != sellable_.end() ? sellable_iterator->second : ledger_quantity) + difference;

    journal_adjust(key, "absorb_missed_buy");
    return difference;
}


// ─── 장부 부트스트랩 (G5) — 실계좌 보유분 시드 ──────────────────────────────
//  체결이 아니므로 reserved_·daily_pnl_은 두고 positions_·average_prices_·sellable_·opened_at_을 설정한다.
//  기동 때와 데이터 스레드의 재동기(LedgerReconciler) 때 부른다.
void PositionLedger::seed_position(const std::string& account, const std::string& ticker, int quantity, double average,
                              int sellable)
{
    if (quantity <= 0)
    {
        return;
    }

    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PosKey key = keys_.make(account, ticker);
    positions_[key]  = quantity;
    average_prices_[key] = average;
    // 매도가능수량. 모르면(-1) 보유수량으로 둔다 - 모르는 것을 0으로 두면 정당한 청산이 막힌다.
    sellable_[key] = (sellable >= 0 && sellable < quantity) ? sellable : quantity;
    // 기동 시드는 "오늘 산 것"이 아니다. 최소 보유 시간 판정에서 즉시 교체 대상이 되도록
    //  과거 시각으로 찍는다(전일 물린 보유분을 15분 붙잡아 둘 이유가 없다).
    opened_at_[key] = Clock::now() - std::chrono::hours(24);

    ledger_journal::Record record;
    record.kind     = static_cast<uint16_t>(ledger_journal::Kind::SEED);
    record.quantity = quantity;
    record.price    = average;
    record.sellable = sellable;
    journal_append(record, account, ticker);
}
