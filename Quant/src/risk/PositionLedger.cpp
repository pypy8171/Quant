#include "risk/PositionLedger.h"

#include "ipc/LedgerSnapshot.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cstring>
#include <format>

namespace
{
// 국내 주식 체결 비용률 (KIS 실계좌 기준).
constexpr double kCommissionRate = 0.00015; // 위탁수수료 0.015% (매수·매도 공통)
constexpr double kSellTaxRate    = 0.0020;  // 증권거래세 0.20% (매도에만 부과. 2026년: 코스피 0.05%+농특세 0.15%, 코스닥 0.20%. 09-21까지 원장은 0.18%)

// 키가 없으면 fallback. count 뒤 []로 두 번 찾던 자리를 한 번 찾기로 모은다(CODE_REVIEW S-1).
template <typename Map>
typename Map::mapped_type find_or(const Map& map, const typename Map::key_type& key,
                                  typename Map::mapped_type fallback)
{
    const auto found = map.find(key);
    return found != map.end() ? found->second : fallback;
}
} // namespace

// ─── 주문 의도 — 전송 직전 선점 + INTENT 기록 (실체결 원장 positions_는 불변) ────────
namespace
{
ledger_journal::Record make_order_record(ledger_journal::Kind kind, OrderSide side, int quantity,
                                         const PositionLedger::OrderRef& reference)
{
    ledger_journal::Record record;
    record.kind             = static_cast<uint16_t>(kind);
    record.side             = static_cast<uint8_t>(static_cast<OrderSide::Value>(side));
    record.order_type       = static_cast<uint8_t>(reference.type);
    record.order_id         = reference.order_id;
    record.kis_order_number = reference.kis_order_number;
    record.quantity         = quantity;
    return record;
}
} // namespace

bool PositionLedger::on_intent(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          double price, const OrderRef& reference, strategy_table::StrategyId strategy)
{
    const int delta          = (side == OrderSide::BUY) ? quantity : -quantity; // BUY 선점 +, SELL 선점 -
    PosKey    key            = {};
    bool      had_price      = false;
    double    previous_price = 0.0;
    uint64_t  sequence       = 0;
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        key                                = keys_.make(account, ticker);
        const auto previous_price_iterator = reserved_price_.find(key);
        had_price                          = previous_price_iterator != reserved_price_.end();
        previous_price                     = had_price ? previous_price_iterator->second : 0.0;
        apply_reservation_delta(account, ticker, delta, price);

        ledger_journal::Record record = make_order_record(ledger_journal::Kind::INTENT, side, quantity, reference);
        record.price                  = price;
        ledger_journal::put_string(record.strategy, sizeof(record.strategy), strategies_.name(strategy).view());
        sequence = journal_append(record, account, ticker);
    }

    // 디스크 쓰기는 잠금을 푼 뒤에 한다 — 체결 반영·원장 읽기가 이 쓰기를 기다리지 않는다(W-2). 주문은 여전히
    //  INTENT가 디스크에 남은 뒤에만 나간다(이 함수가 참을 돌려준 뒤).
    if (journal_written(sequence))
    {
        return true;
    }

    // 적히지 않은 선점은 되돌린다 — 파일에 없는 주문은 나가지 않는다. 선점가도 직전 값으로. 쓰는 동안 잠금을
    //  놓았으므로 그사이 다른 스레드가 이 선점을 봤을 수 있다 — 한 번 더 막혀 보였을 뿐 넘치게 내지는 않는다.
    std::lock_guard<std::mutex> lock(positions_mutex_);
    apply_reservation_delta(account, ticker, -delta, 0.0);

    if (had_price && reserved_.count(key))
    {
        reserved_price_[key] = previous_price;
    }

    return false;
}

void PositionLedger::adopt_strategy_table(const strategy_table::TableSlots&                          slots,
                                    std::function<strategy_table::StrategyId(std::string_view)> register_hook)
{
    strategies_.adopt(slots, std::move(register_hook));
}

void PositionLedger::on_accepted(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                            const OrderRef& reference)
{
    ledger_journal::Record record = make_order_record(ledger_journal::Kind::ACCEPT, side, quantity, reference);
    journal_append(record, account, ticker);
    journal_flush();
}

void PositionLedger::on_reject(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          const OrderRef& reference, std::string_view reason)
{
    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    release_reservation(keys_.make(account, ticker), (side == OrderSide::BUY) ? -quantity : quantity);
    ledger_journal::Record record = make_order_record(ledger_journal::Kind::REJECT, side, quantity, reference);
    ledger_journal::put_string(record.reason, sizeof(record.reason), reason);
    journal_append(record, account, ticker);
}

// on_intent 본체 + 저널 리플레이(apply_record) 공용 — 재기동 복구가 실시간 경로와 같은 규칙을 탄다.
//  [inv] positions_mutex_를 잡고 부른다.
void PositionLedger::apply_reservation_delta(std::string_view account, std::string_view ticker, int delta, double price)
{
    const PosKey key  = keys_.make(account, ticker);
    int          next = find_or(reserved_, key, 0) + delta;

    if (next == 0)
    {
        reserved_.erase(key);
        reserved_price_.erase(key);      // 선점이 해소되면 선점가도 정리(§3d 명목이 남아 부풀지 않게)
    }
    else
    {
        reserved_[key] = next;

        if (price > 0.0)
        {
            reserved_price_[key] = price; // 최신 선점가 기록. 시장가(0)면 유지(직전 값)해 총노출 근사 보존
        }
    }
}

// ─── 미체결 취소/정정 축소 시 선점 해제 (C5) ────────────────────────────────
//  on_fill_confirmed의 reserved 해제와 같은 방향. positions_/average_price는 손대지 않는다
//  (취소는 체결이 아니므로 실보유·평단 불변). quantity<=0이면 no-op(방어).
// 선점 해제 한 곳 — 취소 통보와 체결 통보가 같은 규칙을 쓰게 모았다. 규칙이 갈라져 있던 동안
//  on_cancel에만 가드가 있고 on_fill_confirmed에는 없어, 선점을 잡은 적 없는 포지션의 체결이
//  없던 선점을 만들어 냈다. delta는 해제 방향(BUY 선점 +는 -quantity, SELL 선점 -는 +quantity).
//  호출자가 positions_mutex_를 이미 쥐고 있다고 가정한다(여기서 다시 잡지 않는다).
void PositionLedger::release_reservation(const PosKey& key, int delta)
{
    // 잔고 대조가 reserved_를 비운 뒤 온 통보는 대상이 이미 없으므로 아무 것도 하지 않는다.
    //  (없는 키를 갱신하면 부호가 뒤집힌 선점이 생겨 이후 한도·슬롯 계산이 왜곡됨)
    int current = find_or(reserved_, key, 0);

    if (current == 0)
    {
        return;
    }

    int result = current + delta;

    // 과잉 해제(부호 역전) 시 0에서 정지 — 리셋·이중통보로 음수 선점이 남지 않게.
    if ((current > 0 && result < 0) || (current < 0 && result > 0))
    {
        result = 0;
    }

    if (result == 0)
    {
        reserved_.erase(key);
        reserved_price_.erase(key);
    }
    else
    {
        reserved_[key] = result;
    }
}

void PositionLedger::on_cancel(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          const OrderRef& reference)
{
    if (quantity <= 0)
    {
        return;
    }

    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    // BUY 선점은 +였으므로 -quantity, SELL 선점은 -였으므로 +quantity (해제 = 반대부호 가산)
    release_reservation(keys_.make(account, ticker), (side == OrderSide::BUY) ? -quantity : quantity);
    ledger_journal::Record record = make_order_record(ledger_journal::Kind::CANCEL, side, quantity, reference);
    journal_append(record, account, ticker);
}

// ─── 선점 전면 초기화 (REST 잔고 대조 전용) ──────────────────────────────────
void PositionLedger::reset_reserved()
{
    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    reserved_.clear();
    reserved_price_.clear();
    ledger_journal::Record record;
    record.kind = static_cast<uint16_t>(ledger_journal::Kind::RESET_RESERVED);
    journal_append(record, std::string_view(), std::string_view());
}

// ─── 저널 ────────────────────────────────────────────────────────────────────
bool PositionLedger::set_journal(const std::filesystem::path& directory, std::string_view date_yyyymmdd, bool fsync)
{
    journal_ = std::make_unique<ledger_journal::LedgerJournal>(directory, date_yyyymmdd, fsync);

    if (!journal_->ok())
    {
        LOG_ERROR(std::format("[PositionLedger] 원장 저널을 못 열었다 - path({})", journal_->path().string()));
        return false;
    }

    replaying_     = true;
    replay_result_ = ledger_journal::LedgerJournal::replay(
        journal_->path(), [this](const ledger_journal::Record& record) { apply_record(record); });
    replaying_ = false;
    // 꼬리를 잘랐는지는 파일을 열 때만 알 수 있다 — 자르고 난 뒤 다시 읽으면 멀쩡해 보인다.
    replay_result_.truncated_tail = journal_->opened().truncated_tail;
    return true;
}

uint64_t PositionLedger::journal_append(ledger_journal::Record& record, std::string_view account, std::string_view ticker)
{
    if (replaying_ || !journal_)
    {
        return 0;
    }

    ledger_journal::put_string(record.account, sizeof(record.account), account);
    ledger_journal::put_string(record.ticker, sizeof(record.ticker), ticker);
    return journal_->stage(record);
}

void PositionLedger::journal_flush()
{
    if (replaying_ || !journal_)
    {
        return;
    }

    const ledger_journal::LedgerJournal::FlushResult result = journal_->flush();

    if (result.failed == 0)
    {
        return;
    }

    journal_failures_.fetch_add(result.failed, std::memory_order_relaxed);
    LOG_ERROR(std::format("[PositionLedger] 원장 저널 기록 실패, 파일이 원장보다 뒤처졌다 - {}건 첫 kind({})",
                          result.failed, result.first_failed_kind));
}

bool PositionLedger::journal_written(uint64_t sequence)
{
    if (replaying_ || !journal_)
    {
        return true;
    }

    journal_flush();
    return journal_->written(sequence);
}

PositionLedger::JournalFlushAfter::~JournalFlushAfter()
{
    ledger.journal_flush();
}

void PositionLedger::journal_adjust(const PosKey& key, std::string_view reason)
{
    ledger_journal::Record record;
    record.kind = static_cast<uint16_t>(ledger_journal::Kind::ADJUST);
    const auto position_iterator = positions_.find(key);
    const auto average_iterator  = average_prices_.find(key);
    const auto sellable_iterator = sellable_.find(key);
    const auto reserved_iterator = reserved_.find(key);
    record.quantity          = position_iterator != positions_.end() ? position_iterator->second : 0;
    record.price             = average_iterator != average_prices_.end() ? average_iterator->second : 0.0;
    record.sellable          = sellable_iterator != sellable_.end() ? sellable_iterator->second : -1;
    record.reserved_quantity = reserved_iterator != reserved_.end() ? reserved_iterator->second : 0;
    ledger_journal::put_string(record.reason, sizeof(record.reason), reason);
    journal_append(record, keys_.account_of(key), keys_.ticker_of(key).view());
}

std::vector<PositionLedger::OpenIntent> PositionLedger::open_intents() const
{
    std::vector<OpenIntent> intents;
    intents.reserve(open_intents_.size());

    for (const auto& [order_id, intent] : open_intents_)
    {
        intents.push_back(intent);
    }

    std::sort(intents.begin(), intents.end(),
              [](const OpenIntent& left, const OpenIntent& right) { return left.order_id < right.order_id; });
    return intents;
}

// INTENT가 열고 FILL·CANCEL·REJECT가 닫는다 — 남은 것이 "보냈는데 결말을 못 본" 주문이다. 주문번호가 0인
//  레코드(시드·대조·현금)와 라우터가 번호를 못 붙인 주문은 셈에서 뺀다(닫을 방법이 없어 영원히 남는다).
void PositionLedger::track_open_intent(const ledger_journal::Record& record)
{
    using ledger_journal::Kind;

    if (record.order_id == 0)
    {
        return;
    }

    const Kind kind = static_cast<Kind>(record.kind);

    if (kind == Kind::INTENT)
    {
        OpenIntent& intent   = open_intents_[record.order_id];
        intent.order_id      = record.order_id;
        intent.account       = record.account;
        intent.ticker        = record.ticker;
        intent.strategy_name = record.strategy;
        intent.side  = record.side == static_cast<uint8_t>(OrderSide::SELL) ? OrderSide::SELL : OrderSide::BUY;
        intent.type  = record.order_type == static_cast<uint8_t>(OrderType::LIMIT) ? OrderType::LIMIT : OrderType::MARKET;
        intent.price = record.price;
        intent.remaining += record.quantity;
        return;
    }

    auto iterator = open_intents_.find(record.order_id);

    if (iterator == open_intents_.end())
    {
        return;
    }

    if (kind == Kind::ACCEPT)
    {
        iterator->second.accepted         = true;
        iterator->second.kis_order_number = record.kis_order_number;
        return;
    }

    if (kind != Kind::FILL && kind != Kind::CANCEL && kind != Kind::REJECT)
    {
        return;
    }

    iterator->second.remaining -= record.quantity;

    if (iterator->second.remaining <= 0)
    {
        open_intents_.erase(iterator);
    }
}

void PositionLedger::apply_record(const ledger_journal::Record& record)
{
    using ledger_journal::Kind;
    track_open_intent(record);
    const std::string account(record.account);
    const std::string ticker(record.ticker);
    const OrderSide   side = record.side == static_cast<uint8_t>(OrderSide::SELL) ? OrderSide::SELL : OrderSide::BUY;
    const int         release = (side == OrderSide::BUY) ? -record.quantity : record.quantity;

    switch (static_cast<Kind>(record.kind))
    {
    case Kind::SEED:
        seed_position(account, ticker, record.quantity, record.price, record.sellable);
        break;

    case Kind::INTENT:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        apply_reservation_delta(account, ticker, (side == OrderSide::BUY) ? record.quantity : -record.quantity,
                                record.price);
        break;
    }

    case Kind::ACCEPT:
        break; // 상태 변화 없음 — 미결 INTENT를 가리는 것은 재기동 대조(Engine) 몫

    case Kind::REJECT:
    case Kind::CANCEL:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        release_reservation(keys_.make(account, ticker), release);
        break;
    }

    case Kind::FILL:
        on_fill_confirmed(account, ticker, side, record.quantity, record.price,
                          record.strategy[0] != '\0' ? strategies_.intern(record.strategy) : strategy_table::kNone);
        break;

    case Kind::ADJUST:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        apply_adjust_locked(keys_.make(account, ticker), record);
        break;
    }

    case Kind::RESET_RESERVED:
        reset_reserved();
        break;

    case Kind::CASH:
        available_cash_.store(record.cash, std::memory_order_relaxed);
        equity_.store(record.equity, std::memory_order_relaxed);
        break;

    case Kind::DAILY_PNL:
        set_daily_pnl(record.pnl);
        break;
    }
}

void PositionLedger::apply_adjust_locked(const PosKey& key, const ledger_journal::Record& record)
{
    if (record.quantity > 0)
    {
        positions_[key]      = record.quantity;
        average_prices_[key] = record.price;
        sellable_[key]       = (record.sellable >= 0 && record.sellable < record.quantity) ? record.sellable : record.quantity;

        if (!opened_at_.count(key))
        {
            opened_at_[key] = Clock::now() - std::chrono::hours(24);
        }
    }
    else
    {
        positions_.erase(key);
        average_prices_.erase(key);
        sellable_.erase(key);
        opened_at_.erase(key);
    }

    if (record.reserved_quantity != 0)
    {
        reserved_[key] = record.reserved_quantity;
    }
    else
    {
        reserved_.erase(key);
        reserved_price_.erase(key);
    }

    missed_sell_seen_.erase(key);
    missed_buy_seen_.erase(key);
}

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
        return;   // 원장이 모르는 보유는 손대지 않는다 — 없는 매도가능수량을 만들어 낼 이유가 없다
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
    sellable_view.pending = (reserved_iterator != reserved_.end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;
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
    const int sell_pending = (reserved_iterator != reserved_.end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;
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
    const int sell_pending = (reserved_iterator != reserved_.end() && reserved_iterator->second < 0) ? -reserved_iterator->second : 0;

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
    reserved_iterator->second += difference;

    if (reserved_iterator->second == 0)
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
    const int buy_pending = (reserved_iterator != reserved_.end() && reserved_iterator->second > 0) ? reserved_iterator->second : 0;

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

    reserved_iterator->second -= difference;

    if (reserved_iterator->second == 0)
    {
        reserved_.erase(reserved_iterator);
        reserved_price_.erase(key);
    }

    // 당일 매수분은 당일 매도 가능하다(on_fill_confirmed BUY와 같다).
    auto sellable_iterator = sellable_.find(key);
    sellable_[key] = (sellable_iterator != sellable_.end() ? sellable_iterator->second : ledger_quantity) + difference;

    journal_adjust(key, "absorb_missed_buy");
    return difference;
}

// ─── 실현 손익 누적 ─────────────────────────────────────────────────────────
void PositionLedger::add_realized_pnl(double pnl)
{
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    daily_pnl_ += pnl;
}

// ─── 원장 부트스트랩 (G5) — 실계좌 보유분 시드 ──────────────────────────────
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

// ─── 체결 확인 — average_price 재계산 + 실현손익 적립 ──────────────────────────
PositionLedger::FillResult PositionLedger::on_fill_confirmed(
    const std::string& account, const std::string& ticker, OrderSide side, int quantity, double price,
    strategy_table::StrategyId strategy, const OrderRef& reference)
{
    FillResult result;
    result.commission = price * quantity * kCommissionRate;                              // 수수료 0.015%
    result.tax        = (side == OrderSide::SELL) ? price * quantity * kSellTaxRate : 0.0; // 거래세 매도만

    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        const PosKey key = keys_.make(account, ticker);
        int pre_quantity    = find_or(positions_, key, 0); // 체결 전 실보유
        double current_average = find_or(average_prices_, key, 0.0);

        // 전략별 서브원장(D-089) — 위 종목단위 pre_quantity/current_average와 별개로 같은 락에서 갱신.
        //  전략 번호가 없으면(kNone) 건드리지 않는다(계산·판정에 영향 없음, 참고용 집계일 뿐).
        if (strategy != strategy_table::kNone)
        {
            const StrategyKey strategy_key{strategy, key.symbol};
            auto              strategy_position_iterator = strategy_positions_.find(strategy_key);
            auto              strategy_average_iterator  = strategy_average_prices_.find(strategy_key);
            const int         strategy_pre_quantity =
                strategy_position_iterator != strategy_positions_.end() ? strategy_position_iterator->second : 0;
            const double strategy_current_average =
                strategy_average_iterator != strategy_average_prices_.end() ? strategy_average_iterator->second : 0.0;

            if (side == OrderSide::BUY)
            {
                int strategy_new_quantity = strategy_pre_quantity + quantity;
                strategy_average_prices_[strategy_key] = (strategy_new_quantity > 0)
                    ? (strategy_pre_quantity * strategy_current_average + quantity * price) / strategy_new_quantity
                    : price;
                strategy_positions_[strategy_key] = strategy_new_quantity;
            }
            else // SELL
            {
                if (strategy_pre_quantity <= 0 || strategy_current_average <= 0.0)
                {
                    result.strategy_basis_unknown = true;
                }
                else
                {
                    // 이 전략이 가진 만큼만 이 전략 몫이다. 넘는 수량(다른 전략·기동 시드 보유분)까지
                    //  이 전략 평단으로 셈하면 전략별 손익이 틀어진다. 수수료·세금도 같은 비율로 나눈다. [why CODE_REVIEW W-4]
                    const int    attributed_quantity = std::min(quantity, strategy_pre_quantity);
                    const double attributed_share    = static_cast<double>(attributed_quantity) / quantity;
                    result.strategy_realized_pnl = (price - strategy_current_average) * attributed_quantity
                                                    - (result.commission + result.tax) * attributed_share;
                }

                int strategy_new_quantity = strategy_pre_quantity - quantity;

                if (strategy_new_quantity <= 0)
                {
                    strategy_positions_.erase(strategy_key);
                    strategy_average_prices_.erase(strategy_key);
                }
                else
                {
                    strategy_positions_[strategy_key] = strategy_new_quantity;
                }
            }
        }

        if (side == OrderSide::BUY)
        {
            // 실체결분만 원장에 반영 (부분체결도 정확) — 평단 분모는 실체결 수량
            int new_quantity = pre_quantity + quantity;
            average_prices_[key] = (new_quantity > 0)
                ? (pre_quantity * current_average + quantity * price) / new_quantity
                : price;
            positions_[key] = new_quantity;

            if (pre_quantity <= 0)
            {
                opened_at_[key] = Clock::now(); // 0에서 열린 시각 — 교체 최소 보유 판정 기준
            }

            result.average_price = average_prices_[key];
            result.net_quantity   = new_quantity;

            // 당일 매수분은 당일 매도 가능하다.
            sellable_[key] = find_or(sellable_, key, pre_quantity) + quantity;

            // 선점 해제 (BUY 선점은 +였으므로 -quantity). on_cancel과 같은 가드를 둔다 —
            //  선점이 없는데 빼면 음수 선점이 생겨 이후 한도·슬롯 계산이 왜곡된다
            //  (잔고 재시드분처럼 게이트가 선점을 잡은 적 없는 포지션의 체결이 이 경로로 온다).
            release_reservation(key, -quantity);
        }
        else // SELL
        {
            int new_quantity = pre_quantity - quantity;

            if (new_quantity < 0)
            {
                new_quantity = 0;  // 공매도 미지원 — 보유 초과 매도는 0으로 클램프
            }

            // 평단 미상(원장이 종목을 모름·재기동 후 미시드)이면 손익을 계산할 수 없다.
            //  0으로 곱하면 매도대금 전액이 이익으로 적립되므로 0을 두고 플래그로 알린다.
            if (!average_prices_.count(key) || current_average <= 0.0)
            {
                result.basis_unknown = true;
                result.realized_pnl  = 0.0;
            }
            else
            {
                result.realized_pnl = (price - current_average) * quantity
                                      - result.commission - result.tax;
            }

            result.average_price = current_average; // SELL 후 평균단가 불변
            result.net_quantity   = new_quantity;

            if (new_quantity == 0)
            {
                positions_.erase(key);
                average_prices_.erase(key); // 포지션 청산 시 평균단가 초기화
                opened_at_.erase(key);
                sellable_.erase(key);
            }
            else
            {
                positions_[key] = new_quantity;
                int sellable_after = find_or(sellable_, key, pre_quantity) - quantity;
                sellable_[key] = sellable_after > 0 ? sellable_after : 0;
            }

            // 선점 해제 (SELL 선점은 -였으므로 +quantity). 가드가 없으면 선점이 없던 종목의
            //  매도 체결이 reserved_[k] = +quantity를 만들어 내고, slots_full()이 포지션도 없는
            //  종목의 슬롯을 점유로 세어 비운 자리가 그날 내내 열리지 않는다.
            release_reservation(key, quantity);
        }

        // FILL 기록 — 같은 락 안에서 순번을 받아 파일 순서가 원장 갱신 순서와 같다. 실현손익은 참고용(리플레이는 다시 계산한다).
        ledger_journal::Record record = make_order_record(ledger_journal::Kind::FILL, side, quantity, reference);
        record.price                  = price;
        record.pnl                    = result.realized_pnl;
        ledger_journal::put_string(record.strategy, sizeof(record.strategy), strategies_.name(strategy).view());
        journal_append(record, account, ticker);
    }

    journal_flush(); // 잠금을 푼 뒤에 쓴다(W-2)

    if (side == OrderSide::BUY)
    {
        // 매수 수수료도 발생 즉시 비용으로 인식한다 — average_price에 얹으면 평단 표시가
        //  실제 체결가와 어긋나므로, daily_pnl_에서 바로 뺀다.
        add_realized_pnl(-result.commission);
    }
    else if (side == OrderSide::SELL && !result.basis_unknown)
    {
        add_realized_pnl(result.realized_pnl);
    }

    return result;
}

// check() 3절의 원장 키 — 처음 보는 계좌·종목은 등록한다(모르는 계좌끼리 중복 신호 키가 겹치지 않게).
PositionLedger::PosKey PositionLedger::register_signal(const OrderSignal& signal)
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    return keys_.register_signal(signal);
}

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
        if (entry.second > 0 && !slot_exempt_.contains(entry.first.symbol))
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

// ─── 일별 만료 (장 시작 시) ─────────────────────────────────────────────────
void PositionLedger::expire_reservations()
{
    // 미체결 선점은 일일 만료 (KIS 당일 주문은 장 마감 소멸 → 다음날 잘못된 차단 방지).
    // C5(MM-1): 명시적 취소는 on_cancel()로 일원화. reserved_.clear()는 장 마감 안전망
    //   — 취소 없이 장 마감까지 미체결로 만료된 분의 선점을 청소한다.
    std::lock_guard<std::mutex> lock(positions_mutex_);
    reserved_.clear();
    reserved_price_.clear();
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
    return (iterator != reserved_.end()) ? iterator->second : 0;
}

int PositionLedger::reserved(const std::string& account, symbol::SymbolId symbol) const
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto iterator = reserved_.find(keys_.lookup(account, symbol));
    return (iterator != reserved_.end()) ? iterator->second : 0;
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

    uint64_t foreign_rows = 0;

    {
        std::lock_guard<std::mutex> lock(positions_mutex_);

        // 어느 계좌를 싣는가 — 한 프로세스는 한 계좌만 다룬다. 가장 작은 계좌 번호를 이번 판의 계좌로
        //  삼고(같은 원장이면 판마다 같은 답이 나온다), 다른 계좌 줄은 싣지 않고 센다.
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
            if (entry.second != 0 && entry.first.account < account)
            {
                account = entry.first.account;
            }
        }

        // 이번 판의 계좌 이름. 실린 줄이 하나도 없으면(기동 직후) 0번 = ""을 쓴다 — 단일 계좌에서
        //  원장 키가 쓰는 이름이 그것이고, 이름을 비워 두면 전략 쪽이 강제청산 주문에 계좌를 못 적는다.
        {
            const std::string& account_name = keys_.account_name(account);
            const size_t       copied       = (account_name.size() < sizeof(globals.account)) ? account_name.size()
                                                                                              : sizeof(globals.account) - 1;
            std::memcpy(globals.account, account_name.data(), copied);
            globals.account[copied] = '\0';
        }

        size_t open_slots = 0;
        double gross      = 0.0;

        // ① 미체결 선점 먼저. 보유를 돌 때 매도가능(상한 - 미체결 매도)을 한 번에 셈하기 위해서다.
        for (const auto& entry : reserved_)
        {
            const PosKey& key = entry.first;

            if (entry.second == 0)
            {
                continue;
            }

            if (key.account != account)
            {
                ++foreign_rows;
                continue;
            }

            const bool exempt = slot_exempt_.contains(key.symbol);

            ipc::LedgerRow& row = snapshot.row_for_write(key.symbol);
            row.reserved        = entry.second;
            row.slot_exempt     = exempt ? 1 : 0;

            if (entry.second > 0)
            {
                const auto position_iterator = positions_.find(key);

                // 보유 없이 매수 선점만 있는 종목도 자리를 하나 문다.
                if (!exempt && (position_iterator == positions_.end() || position_iterator->second <= 0))
                {
                    ++open_slots;
                }

                const auto reserved_price_iterator = reserved_price_.find(key);
                gross += entry.second * ((reserved_price_iterator != reserved_price_.end()) ? reserved_price_iterator->second : 0.0);
            }
        }

        // ② 보유. 매도가능은 sellable_view()와 같은 셈이다 — 보유와 KIS 상한 중 작은 쪽에서 미체결 매도를 뺀다.
        for (const auto& entry : positions_)
        {
            const PosKey& key = entry.first;

            if (entry.second == 0)
            {
                continue; // 닫힌 자리는 사본에 없는 것과 같다
            }

            if (key.account != account)
            {
                ++foreign_rows;
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
                    ++open_slots;
                }

                gross += entry.second * average_price;

                int        sellable_limit    = entry.second;
                const auto sellable_iterator = sellable_.find(key);

                if (sellable_iterator != sellable_.end() && sellable_iterator->second < sellable_limit)
                {
                    sellable_limit = sellable_iterator->second;
                }

                const int pending_sell = (row.reserved < 0) ? -row.reserved : 0;
                row.sellable = (sellable_limit > pending_sell) ? (sellable_limit - pending_sell) : 0;
            }
        }

        // 자리(슬롯)와 예산(총노출) 중 하나만 막혀도 신규 종목을 열 여력이 없다. capacity_full()과 같은 셈이다.
        //  [inv] 슬롯 면제 종목은 자리를 먹지 않는다 — check()·open_slot_count()·entry_snapshot()과 같은
        //  규칙이다. 자리 세는 곳이 다섯인데 셈이 하나라도 다르면 전략과 게이트가 딴 판단을 한다. [why D-109]
        const bool slots_are_full = max_concurrent_positions > 0
                                    && open_slots >= static_cast<size_t>(max_concurrent_positions);
        const bool budget_is_full = max_gross_exposure_percent > 0.0 && equity > 0.0
                                    && gross >= max_gross_exposure_percent * equity * 0.95;

        globals.open_slot_count = static_cast<int32_t>(open_slots);
        globals.capacity_full   = (slots_are_full || budget_is_full) ? 1 : 0;
    }

    snapshot.end_publish();

    if (foreign_rows != 0)
    {
        ledger_foreign_account_rows_.fetch_add(foreign_rows, std::memory_order_relaxed);
    }
}

void PositionLedger::set_slot_exempt(const std::vector<std::string>& tickers)
{
    std::unordered_set<symbol::SymbolId> next;
    next.reserve(tickers.size());

    for (const auto& ticker : tickers)
    {
        const symbol::SymbolId symbol = keys_.symbols().intern(ticker);

        if (symbol != symbol::kNone)
        {
            next.insert(symbol);
        }
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    slot_exempt_ = std::move(next);
}

void PositionLedger::set_slot_exempt_by_id(const std::vector<symbol::SymbolId>& symbols)
{
    std::unordered_set<symbol::SymbolId> next;
    next.reserve(symbols.size());

    for (const symbol::SymbolId symbol : symbols)
    {
        if (symbol != symbol::kNone)
        {
            next.insert(symbol);
        }
    }

    std::lock_guard<std::mutex> lock(positions_mutex_);
    slot_exempt_ = std::move(next);
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

void PositionLedger::on_accept(const std::string& account, const std::string& ticker, OrderSide side, int quantity,
                          double price)
{
    (void)on_intent(account, ticker, side, quantity, price, OrderRef{});
}

void PositionLedger::on_accept(const std::string& ticker, OrderSide side, int quantity, double price)
{
    on_accept(std::string(), ticker, side, quantity, price);
}

void PositionLedger::seed_position(const std::string& account, const std::string& ticker, int quantity, double average_price)
{
    seed_position(account, ticker, quantity, average_price, -1);
}

void PositionLedger::seed_position(const std::string& ticker, int quantity, double average_price)
{
    seed_position(std::string(), ticker, quantity, average_price, -1);
}

void PositionLedger::on_cancel(const std::string& account, const std::string& ticker, OrderSide side, int quantity)
{
    on_cancel(account, ticker, side, quantity, OrderRef{});
}

void PositionLedger::on_cancel(const std::string& ticker, OrderSide side, int quantity)
{
    on_cancel(std::string(), ticker, side, quantity);
}

size_t PositionLedger::StrategyKeyHash::operator()(const StrategyKey& key) const noexcept
{
    const uint64_t packed = (static_cast<uint64_t>(key.strategy) << 32) | key.symbol;
    const uint64_t mixed = packed * 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(mixed ^ (mixed >> 29));
}
