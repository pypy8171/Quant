// 주문 장부 — 주문 수명(선점·접수·거부·취소·체결)과 하루 만료. 저널·대조·조회는 PositionLedger*.cpp로 나눴다(D-113).
#include "risk/PositionLedger.h"

#include <algorithm>

namespace
{
// 국내 주식 체결 비용률 (KIS 실계좌 기준).
constexpr double kCommissionRate = 0.00015; // 위탁수수료 0.015% (매수·매도 공통)
constexpr double kSellTaxRate    = 0.0020;  // 증권거래세 0.20% (매도에만 부과. 2026년: 코스피 0.05%+농특세 0.15%, 코스닥 0.20%. 09-21까지 장부는 0.18%)

// 키가 없으면 fallback. count 뒤 []로 두 번 찾던 자리를 한 번 찾기로 모은다(CODE_REVIEW S-1).
template <typename Map>
typename Map::mapped_type find_or(const Map& map, const typename Map::key_type& key,
                                  typename Map::mapped_type fallback)
{
    const auto found = map.find(key);
    return found != map.end() ? found->second : fallback;
}
} // namespace

// ─── 주문 의도 — 전송 직전 선점 + INTENT 기록 (실체결 장부 positions_는 불변) ────────
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
        apply_reservation_delta(account, ticker, side, quantity, price);

        ledger_journal::Record record = make_order_record(ledger_journal::Kind::INTENT, side, quantity, reference);
        record.price                  = price;
        ledger_journal::put_string(record.strategy, sizeof(record.strategy), strategies_.name(strategy).view());
        sequence = journal_append(record, account, ticker);
    }

    // 디스크 쓰기는 잠금을 푼 뒤에 한다 — 체결 반영·장부 읽기가 이 쓰기를 기다리지 않는다(W-2). 주문은 여전히
    //  INTENT가 디스크에 남은 뒤에만 나간다(이 함수가 참을 돌려준 뒤).
    if (journal_written(sequence))
    {
        return true;
    }

    // 적히지 않은 선점은 되돌린다 — 파일에 없는 주문은 나가지 않는다. 선점가도 직전 값으로. 쓰는 동안 잠금을
    //  놓았으므로 그사이 다른 스레드가 이 선점을 봤을 수 있다 — 한 번 더 막혀 보였을 뿐 넘치게 내지는 않는다.
    std::lock_guard<std::mutex> lock(positions_mutex_);
    apply_reservation_delta(account, ticker, side, -quantity, 0.0);

    if (had_price && side == OrderSide::BUY)
    {
        const auto reservation_iterator = reserved_.find(key);

        if (reservation_iterator != reserved_.end() && reservation_iterator->second.buy > 0)
        {
            reserved_price_[key] = previous_price;
        }
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
    release_reservation(keys_.make(account, ticker), side, quantity);
    ledger_journal::Record record = make_order_record(ledger_journal::Kind::REJECT, side, quantity, reference);
    ledger_journal::put_string(record.reason, sizeof(record.reason), reason);
    journal_append(record, account, ticker);
}

// on_intent 본체 + 저널 리플레이(apply_record) 공용 — 재기동 복구가 실시간 경로와 같은 규칙을 탄다.
//  [inv] positions_mutex_를 잡고 부른다.
void PositionLedger::apply_reservation_delta(std::string_view account, std::string_view ticker, OrderSide side, int delta,
                                             double price)
{
    const PosKey key         = keys_.make(account, ticker);
    Reservation& reservation = reserved_[key];
    int&         quantity    = (side == OrderSide::BUY) ? reservation.buy : reservation.sell;
    quantity                 = std::max(0, quantity + delta);

    if (side == OrderSide::BUY)
    {
        if (reservation.buy == 0)
        {
            reserved_price_.erase(key);   // 매수 선점이 해소되면 선점가도 정리(§3d 명목이 남아 부풀지 않게)
        }
        else if (price > 0.0)
        {
            reserved_price_[key] = price; // 최신 선점가 기록. 시장가(0)면 유지(직전 값)해 총노출 근사 보존
        }
    }

    if (reservation.empty())
    {
        reserved_.erase(key);
    }
}

// ─── 미체결 취소/정정 축소 시 선점 해제 (C5) ────────────────────────────────
//  on_fill_confirmed의 reserved 해제와 같은 방향. positions_/average_price는 손대지 않는다
//  (취소는 체결이 아니므로 실보유·평단 불변). quantity<=0이면 no-op(방어).
// 선점 해제 한 곳 — 취소 통보와 체결 통보가 같은 규칙을 쓰게 모았다. 규칙이 갈라져 있던 동안
//  on_cancel에만 가드가 있고 on_fill_confirmed에는 없어, 선점을 잡은 적 없는 포지션의 체결이
//  없던 선점을 만들어 냈다. 매수·매도는 서로 다른 칸이라 한쪽 해제가 다른 쪽을 건드리지 않는다.
//  호출자가 positions_mutex_를 이미 쥐고 있다고 가정한다(여기서 다시 잡지 않는다).
void PositionLedger::release_reservation(const PosKey& key, OrderSide side, int quantity)
{
    // 잔고 대조가 reserved_를 비운 뒤 온 통보는 대상이 이미 없으므로 아무 것도 하지 않는다.
    auto iterator = reserved_.find(key);

    if (iterator == reserved_.end())
    {
        return;
    }

    int& current = (side == OrderSide::BUY) ? iterator->second.buy : iterator->second.sell;

    if (current == 0)
    {
        return;
    }

    // 과잉 해제는 0에서 정지 — 리셋·이중통보로 음수 선점이 남지 않게.
    current = std::max(0, current - quantity);

    if (side == OrderSide::BUY && current == 0)
    {
        reserved_price_.erase(key);
    }

    if (iterator->second.empty())
    {
        reserved_.erase(iterator);
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
    release_reservation(keys_.make(account, ticker), side, quantity);
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


// ─── 실현 손익 누적 ─────────────────────────────────────────────────────────
void PositionLedger::add_realized_pnl(double pnl)
{
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    daily_pnl_ += pnl;
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

        // 전략별 서브장부(D-089) — 위 종목단위 pre_quantity/current_average와 별개로 같은 락에서 갱신.
        //  전략 번호가 없으면(kNone) 건드리지 않는다(계산·판정에 영향 없음, 참고용 집계일 뿐).
        if (strategy != strategy_table::kNone)
        {
            apply_strategy_fill(StrategyKey{strategy, key.symbol}, side, quantity, price, result);
        }

        if (side == OrderSide::BUY)
        {
            apply_buy_fill(key, pre_quantity, current_average, quantity, price, result);
        }
        else // SELL
        {
            apply_sell_fill(key, pre_quantity, current_average, quantity, price, result);
        }

        // FILL 기록 — 같은 락 안에서 순번을 받아 파일 순서가 장부 갱신 순서와 같다. 실현손익은 참고용(리플레이는 다시 계산한다).
        ledger_journal::Record record = make_order_record(ledger_journal::Kind::FILL, side, quantity, reference);
        record.price                  = price;
        record.pnl                    = result.realized_pnl;
        ledger_journal::put_string(record.strategy, sizeof(record.strategy), strategies_.name(strategy).view());
        ledger_journal::put_fill_detail(record, {.commission    = result.commission,
                                                 .tax           = result.tax,
                                                 .average_price = result.average_price,
                                                 .net_quantity  = result.net_quantity,
                                                 .present       = 2,
                                                 .regime        = regime_code_.load(std::memory_order_relaxed)});
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

// 전략별 서브장부 한 줄 갱신. result.commission·tax는 부른 쪽이 먼저 채워 둔다(매도 몫을 같은 비율로 나눈다).
//  [inv] positions_mutex_를 쥐고 부른다.
void PositionLedger::apply_strategy_fill(const StrategyKey& strategy_key, OrderSide side, int quantity, double price,
                                         FillResult& result)
{
    auto         strategy_position_iterator = strategy_positions_.find(strategy_key);
    auto         strategy_average_iterator  = strategy_average_prices_.find(strategy_key);
    const int    strategy_pre_quantity =
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
        return;
    }

    // SELL
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

// 매수 체결의 종목 장부 갱신 — 평단·보유·열린 시각·매도가능, 그리고 매수 선점 해제.
//  [inv] positions_mutex_를 쥐고 부른다.
void PositionLedger::apply_buy_fill(const PosKey& key, int pre_quantity, double current_average, int quantity,
                                    double price, FillResult& result)
{
    // 실체결분만 장부에 반영 (부분체결도 정확) — 평단 분모는 실체결 수량
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

    // 매수 선점 해제. on_cancel과 같은 가드를 둔다 — 선점이 없는데 빼면 이후 한도·슬롯 계산이
    //  왜곡된다(잔고 재시드분처럼 게이트가 선점을 잡은 적 없는 포지션의 체결이 이 경로로 온다).
    release_reservation(key, OrderSide::BUY, quantity);
}

// 매도 체결의 종목 장부 갱신 — 실현손익(평단을 모르면 0과 플래그), 보유·매도가능 감소, 다 팔면 줄 삭제,
//  그리고 매도 선점 해제. result.commission·tax는 부른 쪽이 먼저 채워 둔다.
//  [inv] positions_mutex_를 쥐고 부른다.
void PositionLedger::apply_sell_fill(const PosKey& key, int pre_quantity, double current_average, int quantity,
                                     double price, FillResult& result)
{
    int new_quantity = pre_quantity - quantity;

    if (new_quantity < 0)
    {
        new_quantity = 0;  // 공매도 미지원 — 보유 초과 매도는 0으로 클램프
    }

    // 평단 미상(장부가 종목을 모름·재기동 후 미시드)이면 손익을 계산할 수 없다.
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

    // 매도 선점 해제. 가드가 없으면 선점이 없던 종목의 매도 체결이 선점을 만들어 내고,
    //  slots_full()이 포지션도 없는 종목의 슬롯을 점유로 세어 비운 자리가 그날 내내 열리지 않는다.
    release_reservation(key, OrderSide::SELL, quantity);
}

// check() 3절의 장부 키 — 처음 보는 계좌·종목은 등록한다(모르는 계좌끼리 중복 신호 키가 겹치지 않게).
PositionLedger::PosKey PositionLedger::register_signal(const OrderSignal& signal)
{
    std::lock_guard<std::mutex> lock(positions_mutex_);
    return keys_.register_signal(signal);
}

// ─── 일별 만료 (거래일 첫 회차) ─────────────────────────────────────────────────
void PositionLedger::expire_reservations(uint32_t trading_date_yyyymmdd)
{
    // 미체결 선점은 일일 만료 (KIS 당일 주문은 장 마감 소멸 → 다음날 잘못된 차단 방지).
    // C5(MM-1): 명시적 취소는 on_cancel()로 일원화. reserved_.clear()는 장 마감 안전망
    //   — 취소 없이 장 마감까지 미체결로 만료된 분의 선점을 청소한다.
    const JournalFlushAfter journal_flush_after{*this};
    std::lock_guard<std::mutex> lock(positions_mutex_);
    reserved_.clear();
    reserved_price_.clear();
    last_daily_reset_date_.store(trading_date_yyyymmdd, std::memory_order_relaxed);
    ledger_journal::Record record;
    record.kind      = static_cast<uint16_t>(ledger_journal::Kind::RESET_DAY);
    record.reserved0 = trading_date_yyyymmdd;
    journal_append(record, std::string_view(), std::string_view());
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
