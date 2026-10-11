// 주문 장부 — 조회, 열린 슬롯 세기, 장부 사본 발행(D-114 단계 2.5), 슬롯 계산 밖 종목.
#include "risk/PositionLedger.h"
#include "ipc/LedgerSnapshot.h"
#include <algorithm>
#include <cstring>

size_t PositionLedger::open_slot_count() const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    size_t open = 0;

    for (const auto& entry : positions_)
    {
        if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
        {
            ++open;
        }
    }

    for (const auto& entry : reserved_)
    {
        if (entry.second.buy > 0 && !slot_exempt_.contains(entry.first.symbol))
        {
            auto iterator = positions_.find(entry.first);

            if (iterator == positions_.end() || iterator->second <= 0)
            {
                ++open;
            }
        }
    }

    return open;
}


// ─── 조회 (계좌별) ───────────────────────────────────────────────────────────
int PositionLedger::position(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = positions_.find(keys_.lookup(account, ticker));
    return (iterator != positions_.end()) ? iterator->second : 0;
}

int PositionLedger::position(const std::string& account, symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = positions_.find(keys_.lookup(account, symbol));
    return (iterator != positions_.end()) ? iterator->second : 0;
}

int PositionLedger::reserved(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = reserved_.find(keys_.lookup(account, ticker));
    return (iterator != reserved_.end()) ? iterator->second.net() : 0;
}

int PositionLedger::reserved(const std::string& account, symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = reserved_.find(keys_.lookup(account, symbol));
    return (iterator != reserved_.end()) ? iterator->second.net() : 0;
}

int PositionLedger::reserved_sell(const std::string& account, symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = reserved_.find(keys_.lookup(account, symbol));
    return (iterator != reserved_.end()) ? iterator->second.sell : 0;
}

double PositionLedger::average_price(const std::string& account, const std::string& ticker) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = average_prices_.find(keys_.lookup(account, ticker));
    return (iterator != average_prices_.end()) ? iterator->second : 0.0;
}

double PositionLedger::daily_pnl() const
{
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    return daily_pnl_;
}

// ─── 보유 포지션 스냅샷 (G3 강제청산) ────────────────────────────────────────
std::vector<PositionLedger::HeldPos> PositionLedger::snapshot_positions() const
{
    std::vector<HeldPos> out;
    std::lock_guard<std::mutex> lock(positions_mutex_);
    out.reserve(positions_.size());

    for (const auto& entry : positions_)
    {
        if (entry.second <= 0)
        {
            continue; // 롱 보유분만 청산 대상
        }

        const PosKey& key = entry.first;
        HeldPos held_position;
        held_position.account = keys_.account_of(key);
        held_position.ticker  = keys_.ticker_of(key).string();
        held_position.symbol  = key.symbol;
        held_position.quantity     = entry.second;
        auto average_price_iterator = average_prices_.find(key);
        held_position.average_price = (average_price_iterator != average_prices_.end()) ? average_price_iterator->second : 0.0;
        held_position.slot_exempt   = slot_exempt_.contains(key.symbol);
        out.push_back(std::move(held_position));
    }

    return out;
}

// ─── 장부 사본 발행 (D-114 단계 2.5) ─────────────────────────────────────────
//  전략 쪽이 OrderGate를 직접 부르는 자리를 이 사본 하나로 바꾸기 위한 채우기다. 전략 스레드
//  (SignalDispatcher·전략 provider)가 이 사본을 읽는다(D-114). 한 바퀴에 한 번 부르므로 사본을 읽는 쪽이 밀리지 않는다.
//
//  [inv] 판 안에서는 사본을 읽지 않는다. 판 번호가 홀수인 동안 읽는 쪽 함수(collect_rows·row)는
//  짝수가 될 때까지 도는데, 그 짝수를 만드는 것이 자기 자신이라 영영 안 끝난다.
//  그래서 미체결(reserved_)을 먼저 싣고 보유(positions_)를 돌 때 매도가능을 같이 셈한다.
void PositionLedger::publish(ipc::LedgerSnapshot& snapshot, const std::function<void(ipc::LedgerGlobals&)>& fill_globals,
                             int max_concurrent_positions, double max_gross_exposure_percent) const
{
    // 발행끼리 줄을 세운다. 판 번호를 둘이 동시에 뒤집으면 짝수인 순간이 안 와 읽는 쪽이 밀린다.
    std::lock_guard<std::mutex> publish_lock(ledger_publish_mutex_);

    // 자기 원자변수가 지키는 값들은 positions_mutex_ 밖에서 읽는다 — 잠금을 쥔 구간을 좁게 둔다.
    const double equity = equity_.load(std::memory_order_relaxed);

    snapshot.begin_publish();

    ipc::LedgerGlobals& globals      = snapshot.globals_for_write();
    fill_globals(globals); // 게이트 쪽 전역값(국면 플래그·한도) — 채우는 쪽은 OrderGate::publish_ledger

    PublishTally tally;

    {
        std::lock_guard<std::mutex> lock(positions_mutex_);

        const uint32_t account = publish_account_locked();

        // 이번 판의 계좌 이름. 실린 줄이 하나도 없으면(기동 직후) 0번 = ""을 쓴다 — 단일 계좌에서
        //  장부 키가 쓰는 이름이 그것이고, 이름을 비워 두면 전략 쪽이 강제청산 주문에 계좌를 못 적는다.
        {
            const std::string& account_name = keys_.account_name(account);
            const size_t       copied       = (account_name.size() < sizeof(globals.account)) ? account_name.size()
                                                                                              : sizeof(globals.account) - 1;
            std::memcpy(globals.account, account_name.data(), copied);
            globals.account[copied] = '\0';
        }

        // ① 미체결 선점 먼저. 보유를 돌 때 매도가능(상한 - 미체결 매도)을 한 번에 셈하기 위해서다.
        publish_reserved_rows_locked(snapshot, account, tally);
        // ② 보유.
        publish_position_rows_locked(snapshot, account, tally);

        // 자리(슬롯)와 예산(총노출) 중 하나만 막혀도 신규 종목을 열 여력이 없다. capacity_full()과 같은 셈이다.
        //  [inv] 슬롯 면제 종목은 자리를 먹지 않는다 — check()·open_slot_count()·entry_snapshot()과 같은
        //  규칙이다. 자리 세는 곳이 다섯인데 셈이 하나라도 다르면 전략과 게이트가 딴 판단을 한다. [why D-109]
        const bool slots_are_full = max_concurrent_positions > 0
                                    && tally.open_slots >= static_cast<size_t>(max_concurrent_positions);
        const bool budget_is_full = max_gross_exposure_percent > 0.0 && equity > 0.0
                                    && tally.gross >= max_gross_exposure_percent * equity * 0.95;

        globals.open_slot_count = static_cast<int32_t>(tally.open_slots);
        globals.capacity_full   = (slots_are_full || budget_is_full) ? 1 : 0;
    }

    snapshot.end_publish();

    if (tally.foreign_rows != 0)
    {
        ledger_foreign_account_rows_.fetch_add(tally.foreign_rows, std::memory_order_relaxed);
    }
}

// 어느 계좌를 싣는가 — 한 프로세스는 한 계좌만 다룬다. 가장 작은 계좌 번호를 이번 판의 계좌로
//  삼고(같은 장부면 판마다 같은 답이 나온다), 다른 계좌 줄은 싣지 않고 센다.
//  [inv] positions_mutex_를 쥐고 부른다.
uint32_t PositionLedger::publish_account_locked() const
{
    uint32_t account = kUnknownAccount;

    for (const auto& entry : positions_)
    {
        if (entry.second != 0 && entry.first.account < account)
        {
            account = entry.first.account;
        }
    }

    for (const auto& entry : reserved_)
    {
        if (!entry.second.empty() && entry.first.account < account)
        {
            account = entry.first.account;
        }
    }

    return account;
}

// 사본 ① — 미체결 선점 줄. 보유 없이 매수 선점만 있는 종목의 자리와 매수 선점 명목을 tally에 더한다.
//  [inv] positions_mutex_를 쥐고 부른다.
void PositionLedger::publish_reserved_rows_locked(ipc::LedgerSnapshot& snapshot, uint32_t account,
                                                  PublishTally& tally) const
{
    for (const auto& entry : reserved_)
    {
        const PosKey& key = entry.first;

        if (entry.second.empty())
        {
            continue;
        }

        if (key.account != account)
        {
            ++tally.foreign_rows;
            continue;
        }

        const bool exempt = slot_exempt_.contains(key.symbol);

        ipc::LedgerRow& row = snapshot.row_for_write(key.symbol);
        row.reserved        = entry.second.net();
        row.reserved_sell   = entry.second.sell;
        row.slot_exempt     = exempt ? 1 : 0;

        if (entry.second.buy > 0)
        {
            const auto position_iterator = positions_.find(key);

            // 보유 없이 매수 선점만 있는 종목도 자리를 하나 문다.
            if (!exempt && (position_iterator == positions_.end() || position_iterator->second <= 0))
            {
                ++tally.open_slots;
            }

            const auto reserved_price_iterator = reserved_price_.find(key);
            tally.gross += entry.second.buy * ((reserved_price_iterator != reserved_price_.end()) ? reserved_price_iterator->second : 0.0);
        }
    }
}

// 사본 ② — 보유 줄. 매도가능은 sellable_view()와 같은 셈이다 — 보유와 KIS 상한 중 작은 쪽에서 미체결 매도를 뺀다.
//  미체결 매도는 ①이 같은 줄에 먼저 적어 둔 row.reserved_sell을 읽는다.
//  [inv] positions_mutex_를 쥐고 부른다.
void PositionLedger::publish_position_rows_locked(ipc::LedgerSnapshot& snapshot, uint32_t account,
                                                  PublishTally& tally) const
{
    for (const auto& entry : positions_)
    {
        const PosKey& key = entry.first;

        if (entry.second == 0)
        {
            continue; // 닫힌 자리는 사본에 없는 것과 같다
        }

        if (key.account != account)
        {
            ++tally.foreign_rows;
            continue;
        }

        const auto   average_price_iterator = average_prices_.find(key);
        const double average_price = (average_price_iterator != average_prices_.end()) ? average_price_iterator->second : 0.0;
        const bool   exempt        = slot_exempt_.contains(key.symbol);

        ipc::LedgerRow& row = snapshot.row_for_write(key.symbol);
        row.position        = entry.second;
        row.average_price   = average_price;
        row.slot_exempt     = exempt ? 1 : 0;

        if (entry.second > 0)
        {
            if (!exempt)
            {
                ++tally.open_slots;
            }

            const auto mark_iterator = mark_prices_.find(key); // 총노출은 게이트 gross_exposure()와 같이 현재가 우선
            tally.gross += entry.second * ((mark_iterator != mark_prices_.end()) ? mark_iterator->second : average_price);

            int        sellable_limit    = entry.second;
            const auto sellable_iterator = sellable_.find(key);

            if (sellable_iterator != sellable_.end() && sellable_iterator->second < sellable_limit)
            {
                sellable_limit = sellable_iterator->second;
            }

            const int pending_sell = row.reserved_sell;
            row.sellable = (sellable_limit > pending_sell) ? (sellable_limit - pending_sell) : 0;
        }
    }
}

void PositionLedger::set_slot_exempt(strategy_table::StrategyId owner, const std::vector<std::string>& tickers)
{
    std::vector<symbol::SymbolId> symbols;
    symbols.reserve(tickers.size());

    for (const auto& ticker : tickers)
    {
        symbols.push_back(keys_.symbols().intern(ticker));
    }

    set_slot_exempt_by_id(owner, symbols);
}

void PositionLedger::set_slot_exempt_by_id(strategy_table::StrategyId owner, const std::vector<symbol::SymbolId>& symbols)
{
    std::unordered_set<symbol::SymbolId> owned;
    owned.reserve(symbols.size());

    for (const symbol::SymbolId symbol : symbols)
    {
        if (symbol != symbol::kNone)
        {
            owned.insert(symbol);
        }
    }

    // 소유자 하나의 목록만 바꾸고 판정용 집합은 전 소유자의 합집합으로 다시 만든다 — 바스켓과 SURGE_HOLD가
    //  같이 걸면 나중에 건 쪽이 앞의 목록을 지우던 문제(10-06 발견)를 막는다. [why D-157]
    std::lock_guard<std::mutex> lock(positions_mutex_);
    slot_exempt_by_owner_[owner] = std::move(owned);
    slot_exempt_.clear();

    for (const auto& owner_entry : slot_exempt_by_owner_)
    {
        slot_exempt_.insert(owner_entry.second.begin(), owner_entry.second.end());
    }
}

bool PositionLedger::is_slot_exempt(symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    return slot_exempt_.contains(symbol);
}

std::vector<symbol::SymbolId> PositionLedger::slot_exempt_symbols() const
{
    std::vector<symbol::SymbolId> out;
    std::lock_guard<std::mutex>   lock(positions_mutex_);
    out.assign(slot_exempt_.begin(), slot_exempt_.end());
    std::sort(out.begin(), out.end()); // 집합 순서는 해시 순 — 호출자가 같은 입력에 같은 순서를 받게 한다
    return out;
}
