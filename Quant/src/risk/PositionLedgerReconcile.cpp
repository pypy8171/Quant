// 주문 장부 — 잔고 대조 쪽 갱신: 시드, 당일 손익·자본·현금·시가 주입, 유령 슬롯 정리, 매도가능·놓친 체결 맞추기.
#include "risk/PositionLedger.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

void PositionLedger::set_daily_pnl(double pnl)
{
    const Won pnl_won = money::round_won(pnl);
    {
        std::lock_guard<std::mutex> lock(pnl_mutex_);
        daily_pnl_ = pnl_won;
    }

    ledger_journal::Record record;
    record.kind = static_cast<uint16_t>(ledger_journal::Kind::DAILY_PNL);
    record.pnl  = static_cast<double>(pnl_won);
    journal_append(record, std::string_view(), std::string_view());
    journal_flush();
}

void PositionLedger::set_equity(double equity)
{
    const Won equity_won = money::round_won(equity);
    equity_.store(equity_won, std::memory_order_relaxed);
    ledger_journal::Record record;
    record.kind   = static_cast<uint16_t>(ledger_journal::Kind::CASH);
    record.cash   = static_cast<double>(available_cash_.load(std::memory_order_relaxed));
    record.equity = static_cast<double>(equity_won);
    journal_append(record, std::string_view(), std::string_view());
    journal_flush();
}

void PositionLedger::replace_mark_prices(const std::string& account, const std::vector<std::pair<std::string, double>>& marks)
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const uint32_t              account_id = keys_.account_index(account, true);

    for (auto iterator = rows_.begin(); iterator != rows_.end();)
    {
        if (iterator->first.account == account_id)
        {
            iterator->second.mark_price = 0;
        }

        iterator = iterator->second.empty() ? rows_.erase(iterator) : std::next(iterator);
    }

    for (const auto& [ticker, price] : marks)
    {
        const Won mark = money::round_won(price);

        if (mark > 0)
        {
            rows_[keys_.make(account, ticker)].mark_price = mark;
        }
    }
}

void PositionLedger::set_available_cash(double available_cash)
{
    const Won cash_won = money::round_won(available_cash);
    available_cash_.store(cash_won, std::memory_order_relaxed);
    available_cash_known_.store(true, std::memory_order_relaxed);
    ledger_journal::Record record;
    record.kind   = static_cast<uint16_t>(ledger_journal::Kind::CASH);
    record.cash   = static_cast<double>(cash_won);
    record.equity = static_cast<double>(equity_.load(std::memory_order_relaxed));
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

    for (auto iterator = rows_.begin(); iterator != rows_.end();)
    {
        PositionRow& row = iterator->second;

        if (row.position <= 0 || live[iterator->first.symbol])
        {
            ++iterator;
            continue;
        }

        // 방금 열린 포지션은 남긴다. 잔고 조회가 체결보다 먼저 떠난 왕복이면 잔고에 아직
        //  안 보이는데, 그걸 지우면 전략이 미보유로 읽고 같은 종목을 또 산다.
        if (row.opened_at && min_age_sec > 0 && now - *row.opened_at < std::chrono::seconds(min_age_sec))
        {
            ++iterator;
            continue;
        }

        const PosKey key = iterator->first;
        gone.push_back(key.symbol);
        row.position      = 0;
        row.average_price = 0;
        row.sellable      = PositionRow::kUnknown;
        row.opened_at.reset();
        journal_adjust(key, "prune_positions");
        iterator = row.empty() ? rows_.erase(iterator) : std::next(iterator);
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

    for (auto iterator = rows_.begin(); iterator != rows_.end();)
    {
        PositionRow& row = iterator->second;

        if (row.reserved.empty() || (iterator->first.symbol < live.size() && live[iterator->first.symbol]))
        {
            ++iterator;
            continue;
        }

        const PosKey key = iterator->first;
        gone.emplace_back(keys_.ticker_of(key).view());
        row.reserved       = {};
        row.reserved_price = 0;
        journal_adjust(key, "prune_reservations");
        iterator = row.empty() ? rows_.erase(iterator) : std::next(iterator);
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
    PositionRow* row = find_row_locked(key);

    if (row == nullptr || row->position <= 0)
    {
        return;   // 장부가 모르는 보유는 손대지 않는다 — 없는 매도가능수량을 만들어 낼 이유가 없다
    }

    // 매도가능수량을 모르면 0에서 더한다(보유수량으로 읽지 않는다 — 되돌릴 몫만 연다).
    const int restored = row->sellable_or(0) + quantity;
    row->sellable      = std::min(restored, row->position);
    journal_adjust(key, "restore_sellable");
}

PositionLedger::SellableView PositionLedger::sellable_view(const std::string& account, const std::string& ticker) const
{
    SellableView sellable_view;
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const PositionRow& row = row_locked(keys_.lookup(account, ticker));

    if (row.position <= 0)
    {
        return sellable_view;
    }

    sellable_view.held                  = row.position;
    sellable_view.possible_quantity_cap = std::min(row.sellable_or(row.position), row.position);
    sellable_view.pending               = row.reserved.sell;
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
    PositionRow* row = find_row_locked(key);

    if (row == nullptr || row->position <= 0)
    {
        return;
    }

    const int sellable = ord_psbl_qty + row->reserved.sell;
    row->sellable      = std::min(sellable, row->position);
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
    PositionRow* row = find_row_locked(key);

    if (row == nullptr)
    {
        return 0;
    }

    const int difference = row->position - balance_quantity;

    // 장부가 잔고보다 많지 않거나, 미체결 매도보다 큰 차이는 놓친 체결로 설명되지 않는다 — 손대지 않고 로그 관찰에 맡긴다.
    if (difference <= 0 || difference > row->reserved.sell)
    {
        row->missed_sell_seen = PositionRow::kUnknown;
        erase_if_empty_locked(key);
        return 0;
    }

    if (row->missed_sell_seen != balance_quantity)
    {
        row->missed_sell_seen = balance_quantity; // 첫 관측 — 다음 대조에서 같으면 맞춘다
        return 0;
    }

    row->missed_sell_seen = PositionRow::kUnknown;
    row->position         = balance_quantity;
    row->reserved.sell   -= difference;

    if (row->sellable > balance_quantity)
    {
        row->sellable = balance_quantity;
    }

    journal_adjust(key, "absorb_missed_sell");
    erase_if_empty_locked(key);
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
    PositionRow* row = find_row_locked(key);

    if (row == nullptr)
    {
        return 0; // 장부에 줄이 없으면 미체결 매수도 없다 — 놓친 체결로 설명되지 않는다
    }

    const int ledger_quantity = row->position;
    const int difference      = balance_quantity - ledger_quantity;

    // 장부가 잔고보다 적지 않거나, 미체결 매수보다 큰 차이(밖에서 산 것 등)는 손대지 않는다.
    if (difference <= 0 || difference > row->reserved.buy)
    {
        row->missed_buy_seen = PositionRow::kUnknown;
        erase_if_empty_locked(key);
        return 0;
    }

    if (row->missed_buy_seen != balance_quantity)
    {
        row->missed_buy_seen = balance_quantity; // 첫 관측 — 다음 대조에서 같으면 맞춘다
        return 0;
    }

    row->missed_buy_seen = PositionRow::kUnknown;
    row->position        = balance_quantity;
    const Won average    = money::round_won(balance_average);

    if (average > 0)
    {
        row->average_price = average;
    }

    if (ledger_quantity <= 0)
    {
        row->opened_at = Clock::now();
    }

    row->reserved.buy -= difference;

    if (row->reserved.buy == 0)
    {
        row->reserved_price = 0;
    }

    // 당일 매수분은 당일 매도 가능하다(on_fill_confirmed BUY와 같다).
    row->sellable = row->sellable_or(ledger_quantity) + difference;
    journal_adjust(key, "absorb_missed_buy");
    return difference;
}


// ─── 장부 부트스트랩 — 실계좌 보유분 시드 ──────────────────────────────
//  체결이 아니므로 선점·당일 손익은 두고 그 줄의 보유·평단·매도가능·열린 시각을 설정한다.
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
    PositionRow& row      = rows_[keys_.make(account, ticker)];
    const Won average_won = money::round_won(average);
    row.position          = quantity;
    row.average_price     = average_won;
    // 매도가능수량. 모르면(-1) 보유수량으로 둔다 - 모르는 것을 0으로 두면 정당한 청산이 막힌다.
    row.sellable = (sellable >= 0 && sellable < quantity) ? sellable : quantity;
    // 기동 시드는 "오늘 산 것"이 아니다. 최소 보유 시간 판정에서 즉시 교체 대상이 되도록
    //  과거 시각으로 찍는다(전일 물린 보유분을 15분 붙잡아 둘 이유가 없다).
    row.opened_at = Clock::now() - PositionLedger::kSeededOpenAge;

    ledger_journal::Record record;
    record.kind     = static_cast<uint16_t>(ledger_journal::Kind::SEED);
    record.quantity = quantity;
    record.price    = static_cast<double>(average_won);
    record.sellable = sellable;
    journal_append(record, account, ticker);
}
