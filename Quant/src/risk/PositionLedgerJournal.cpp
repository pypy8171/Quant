// 주문 장부 — 저널 쓰기와 재기동 리플레이(레코드를 실시간 경로와 같은 함수로 다시 적용). [why D-113]
#include "risk/PositionLedger.h"
#include "utils/Logger.h"
#include <algorithm>
#include <format>

// ─── 저널 ────────────────────────────────────────────────────────────────────
bool PositionLedger::set_journal(const std::filesystem::path& directory, std::string_view date_yyyymmdd, bool fsync)
{
    journal_ = std::make_unique<ledger_journal::LedgerJournal>(directory, date_yyyymmdd, fsync);

    if (!journal_->ok())
    {
        LOG_ERROR(std::format("[PositionLedger] 장부 저널을 못 열었다 - path({})", journal_->path().string()));
        return false;
    }

    replaying_     = true;
    replay_result_ = ledger_journal::LedgerJournal::replay(
        journal_->path(), [this](const ledger_journal::Record& record)
        {
            apply_record(record);
        });
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

    ledger_journal::put_string(record.account, sizeof(record.account), account.empty() ? journal_account_ : account);
    ledger_journal::put_string(record.ticker, sizeof(record.ticker), ticker);
    return journal_->stage(record);
}

std::string PositionLedger::ledger_account_of(const char* journal_account) const
{
    const std::string_view account(journal_account);

    if (!journal_account_.empty() && account == journal_account_)
    {
        return std::string();
    }

    return std::string(account);
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
    LOG_ERROR(std::format("[PositionLedger] 장부 저널 기록 실패, 파일이 장부보다 뒤처졌다 - {}건 첫 kind({})",
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
    record.reserved_quantity = reserved_iterator != reserved_.end() ? reserved_iterator->second.net() : 0;
    record.reserved_sell     = reserved_iterator != reserved_.end() ? static_cast<uint32_t>(reserved_iterator->second.sell) : 0;
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
              [](const OpenIntent& left, const OpenIntent& right)
              {
                  return left.order_id < right.order_id;
              });
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

    highest_order_id_ = std::max(highest_order_id_, record.order_id);
    const Kind kind   = static_cast<Kind>(record.kind);

    if (kind == Kind::INTENT)
    {
        OpenIntent& intent   = open_intents_[record.order_id];
        intent.order_id      = record.order_id;
        intent.account       = ledger_account_of(record.account);
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
    const std::string account = ledger_account_of(record.account);
    const std::string ticker(record.ticker);
    const OrderSide   side = record.side == static_cast<uint8_t>(OrderSide::SELL) ? OrderSide::SELL : OrderSide::BUY;

    switch (static_cast<Kind>(record.kind))
    {
    case Kind::SEED:
        seed_position(account, ticker, record.quantity, record.price, record.sellable);
        break;

    case Kind::INTENT:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        apply_reservation_delta(account, ticker, side, record.quantity, record.price);
        break;
    }

    case Kind::ACCEPT:
        break; // 상태 변화 없음 — 미결 INTENT를 가리는 것은 재기동 대조(Engine) 몫

    case Kind::REJECT:
    case Kind::CANCEL:
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        release_reservation(keys_.make(account, ticker), side, record.quantity);
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

    case Kind::RESET_DAY:
        expire_reservations(record.reserved0);
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

    // 매도 칸이 0이면(칸이 생기기 전 파일 포함) 순값의 음수를 매도로 읽는다 — 옛 파일은 둘이 같이 걸린 경우를 모른다.
    Reservation reservation;
    reservation.sell = record.reserved_sell > 0 ? static_cast<int>(record.reserved_sell) : std::max(0, -record.reserved_quantity);
    reservation.buy  = std::max(0, record.reserved_quantity + reservation.sell);

    if (!reservation.empty())
    {
        reserved_[key] = reservation;
    }
    else
    {
        reserved_.erase(key);
    }

    if (reservation.buy == 0)
    {
        reserved_price_.erase(key);
    }

    missed_sell_seen_.erase(key);
    missed_buy_seen_.erase(key);
}
