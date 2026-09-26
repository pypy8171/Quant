// exchange::SymbolBook·MatchingEngine 구현 — 선언과 각 함수의 계약은 include/exchange/MatchingEngine.h.
#include "exchange/MatchingEngine.h"

#include "core/TickSize.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace exchange
{
namespace
{
// [formula] 국내 주식 가격제한폭 ±30%(유가증권시장 업무규정 제20조). 격자를 이 폭으로 만들어 두면
//  그날 나올 수 있는 모든 호가가 배열 안에 들어온다.
constexpr double kPriceLimitRatio = 0.30;

// 가격을 호가단위 격자 위로 올린다. 격자 첫 칸을 잡을 때만 쓴다.
PriceKrw align_up_to_tick(PriceKrw price_krw)
{
    const PriceKrw tick = static_cast<PriceKrw>(krx::tick_size(static_cast<double>(price_krw)));

    if (tick <= 1)
    {
        return price_krw;
    }

    const PriceKrw remainder = price_krw % tick;

    return remainder == 0 ? price_krw : price_krw + (tick - remainder);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SymbolBook
// ─────────────────────────────────────────────────────────────────────────────

void SymbolBook::configure(PriceKrw reference_price_krw)
{
    level_price_.clear();
    buy_levels_.clear();
    sell_levels_.clear();
    clear_orders();

    reference_price_krw_ = reference_price_krw;

    if (reference_price_krw <= 0)
    {
        return;
    }

    const double   reference    = static_cast<double>(reference_price_krw);
    const PriceKrw lower_limit  = static_cast<PriceKrw>(std::floor(reference * (1.0 - kPriceLimitRatio)));
    const PriceKrw upper_limit  = static_cast<PriceKrw>(std::ceil(reference * (1.0 + kPriceLimitRatio)));
    PriceKrw       price        = align_up_to_tick(std::max<PriceKrw>(lower_limit, 1));

    while (price <= upper_limit)
    {
        level_price_.push_back(price);
        const PriceKrw tick = static_cast<PriceKrw>(krx::tick_size(static_cast<double>(price)));
        price += std::max<PriceKrw>(tick, 1);
    }

    buy_levels_.resize(level_price_.size());
    sell_levels_.resize(level_price_.size());
}

PriceKrw SymbolBook::price_of_level(size_t level_index) const
{
    return level_index < level_price_.size() ? level_price_[level_index] : 0;
}

size_t SymbolBook::level_of_price(PriceKrw price_krw) const
{
    const auto found = std::lower_bound(level_price_.begin(), level_price_.end(), price_krw);

    if (found == level_price_.end() || *found != price_krw)
    {
        return kNoLevel;
    }

    return static_cast<size_t>(found - level_price_.begin());
}

OrderSlot SymbolBook::new_slot(uint64_t order_id, int32_t quantity)
{
    if (order_slot_count_ >= static_cast<size_t>(kNoSlot))
    {
        return kNoSlot; // 자리 번호가 32비트라 42억 건이 한계다. 부하시험 회차는 종목당 10만 건 수준이다
    }

    if ((order_slot_count_ & kOrderBlockMask) == 0)
    {
        order_blocks_.push_back(std::make_unique<RestingOrder[]>(kOrderBlockSize));
    }

    const OrderSlot slot = static_cast<OrderSlot>(order_slot_count_);
    ++order_slot_count_;

    RestingOrder& made      = slot_at(slot);
    made.order_id           = order_id;
    made.remaining_quantity = quantity;
    made.next_slot          = kNoSlot;

    return slot;
}

bool SymbolBook::push_resting(PriceLevel& level, uint64_t order_id, int32_t quantity)
{
    const OrderSlot slot = new_slot(order_id, quantity);

    if (slot == kNoSlot)
    {
        return false;
    }

    if (level.last_slot == kNoSlot)
    {
        level.first_slot = slot;
    }
    else
    {
        slot_at(level.last_slot).next_slot = slot;
    }

    level.last_slot = slot;
    level.total_quantity += quantity;

    return true;
}

bool SymbolBook::accumulate(const IncomingOrder& order)
{
    if (!ready() || order.quantity <= 0)
    {
        return false;
    }

    const bool is_buy = order.side == OrderSide::BUY;

    if (!is_buy && order.side != OrderSide::SELL)
    {
        return false;
    }

    if (order.price_krw == kMarketOrderPrice)
    {
        if (!push_resting(is_buy ? buy_market_ : sell_market_, order.order_id, order.quantity))
        {
            return false;
        }

        ++resting_count_;

        return true;
    }

    const size_t level_index = level_of_price(order.price_krw);

    if (level_index == kNoLevel)
    {
        return false;
    }

    PriceLevel& level = is_buy ? buy_levels_[level_index] : sell_levels_[level_index];

    if (!push_resting(level, order.order_id, order.quantity))
    {
        return false;
    }

    if (is_buy)
    {
        if (highest_buy_level_ == kNoLevel || level_index > highest_buy_level_)
        {
            highest_buy_level_ = level_index;
        }
    }
    else
    {
        if (lowest_sell_level_ == kNoLevel || level_index < lowest_sell_level_)
        {
            lowest_sell_level_ = level_index;
        }
    }

    ++resting_count_;
    resting_limit_quantity_ += order.quantity;

    return true;
}

int64_t SymbolBook::cumulative_buy_at(size_t level_index) const
{
    // 가격 P 이상에 걸린 매수는 P에 체결돼도 손해가 아니라 전부 살 의향이 있다. 시장가는 가격을 안 가리니 항상 포함.
    int64_t total = buy_market_.total_quantity;

    for (size_t level = level_index; level < buy_levels_.size(); ++level)
    {
        total += buy_levels_[level].total_quantity;
    }

    return total;
}

int64_t SymbolBook::cumulative_sell_at(size_t level_index) const
{
    int64_t total = sell_market_.total_quantity;

    for (size_t level = 0; level <= level_index && level < sell_levels_.size(); ++level)
    {
        total += sell_levels_[level].total_quantity;
    }

    return total;
}

PriceKrw SymbolBook::find_auction_price() const
{
    if (level_price_.empty())
    {
        return 0;
    }

    // 누적을 매 레벨마다 다시 더하면 레벨 수의 제곱이 된다 — 한 번씩 훑어 배열로 만들어 둔다.
    const size_t         level_count = level_price_.size();
    std::vector<int64_t> buy_cumulative(level_count, 0);
    std::vector<int64_t> sell_cumulative(level_count, 0);

    int64_t running = buy_market_.total_quantity;

    for (size_t offset = 0; offset < level_count; ++offset)
    {
        const size_t level = level_count - 1 - offset;
        running += buy_levels_[level].total_quantity;
        buy_cumulative[level] = running;
    }

    running = sell_market_.total_quantity;

    for (size_t level = 0; level < level_count; ++level)
    {
        running += sell_levels_[level].total_quantity;
        sell_cumulative[level] = running;
    }

    int64_t  best_matched    = 0;
    int64_t  best_imbalance  = 0;
    PriceKrw best_price      = 0;

    for (size_t level = 0; level < level_count; ++level)
    {
        const int64_t matched = std::min(buy_cumulative[level], sell_cumulative[level]);

        if (matched <= 0)
        {
            continue;
        }

        const int64_t  imbalance = std::llabs(buy_cumulative[level] - sell_cumulative[level]);
        const PriceKrw price     = level_price_[level];

        if (best_price == 0 || matched > best_matched)
        {
            best_matched   = matched;
            best_imbalance = imbalance;
            best_price     = price;

            continue;
        }

        if (matched < best_matched)
        {
            continue;
        }

        if (imbalance < best_imbalance)
        {
            best_imbalance = imbalance;
            best_price     = price;

            continue;
        }

        if (imbalance == best_imbalance &&
            std::llabs(price - reference_price_krw_) < std::llabs(best_price - reference_price_krw_))
        {
            best_price = price;
        }
    }

    return best_price;
}

int64_t SymbolBook::run_auction(const std::function<void(const Execution&)>& on_execution)
{
    const PriceKrw auction_price = find_auction_price();

    if (auction_price == 0)
    {
        return 0;
    }

    const size_t auction_level = level_of_price(auction_price);

    if (auction_level == kNoLevel)
    {
        return 0;
    }

    int64_t remaining = std::min(cumulative_buy_at(auction_level), cumulative_sell_at(auction_level));

    if (remaining <= 0)
    {
        return 0;
    }

    const int64_t total_matched = remaining;

    // 체결 순서는 가격 우선·시간 우선이다. 매수는 시장가 먼저 그다음 높은 가격부터, 매도는 시장가 먼저
    //  그다음 낮은 가격부터 꺼낸다. 두 커서를 나란히 밀며 짝을 짓는다.
    size_t buy_level_cursor  = buy_levels_.empty() ? 0 : buy_levels_.size() - 1;
    size_t sell_level_cursor = 0;

    // 한 줄에서 아직 수량이 남은 맨 앞 주문. 다 체결된 앞자리는 지나가며 버린다.
    auto next_in_level = [&](PriceLevel& level) -> RestingOrder*
    {
        while (level.first_slot != kNoSlot)
        {
            RestingOrder& candidate = slot_at(level.first_slot);

            if (candidate.remaining_quantity > 0)
            {
                return &candidate;
            }

            level.first_slot = candidate.next_slot;
        }

        level.last_slot = kNoSlot;

        return nullptr;
    };

    auto next_buy = [&]() -> RestingOrder*
    {
        if (RestingOrder* from_market = next_in_level(buy_market_))
        {
            return from_market;
        }

        while (true)
        {
            if (RestingOrder* found = next_in_level(buy_levels_[buy_level_cursor]))
            {
                return found;
            }

            if (buy_level_cursor <= auction_level || buy_level_cursor == 0)
            {
                return nullptr;
            }

            --buy_level_cursor;
        }
    };

    auto next_sell = [&]() -> RestingOrder*
    {
        if (RestingOrder* from_market = next_in_level(sell_market_))
        {
            return from_market;
        }

        while (sell_level_cursor < sell_levels_.size())
        {
            if (RestingOrder* found = next_in_level(sell_levels_[sell_level_cursor]))
            {
                return found;
            }

            if (sell_level_cursor >= auction_level)
            {
                return nullptr;
            }

            ++sell_level_cursor;
        }

        return nullptr;
    };

    while (remaining > 0)
    {
        RestingOrder* buy_order  = next_buy();
        RestingOrder* sell_order = next_sell();

        if (buy_order == nullptr || sell_order == nullptr)
        {
            break;
        }

        const int32_t quantity = static_cast<int32_t>(
            std::min<int64_t>({static_cast<int64_t>(buy_order->remaining_quantity),
                               static_cast<int64_t>(sell_order->remaining_quantity), remaining}));

        buy_order->remaining_quantity -= quantity;
        sell_order->remaining_quantity -= quantity;
        remaining -= quantity;

        if (on_execution)
        {
            on_execution(Execution{buy_order->order_id, sell_order->order_id, symbol::kNone, auction_price, quantity});
        }
    }

    // 단일가가 지나간 뒤의 잔량 집계는 다시 세는 편이 안전하다 — 커서가 지나간 자리에 부분 체결이 섞여 있다.
    resting_count_          = 0;
    resting_limit_quantity_ = 0;
    highest_buy_level_      = kNoLevel;
    lowest_sell_level_      = kNoLevel;

    // 줄 하나를 다시 세어 total_quantity를 맞추고 그 값을 돌려준다.
    const auto recount = [&](PriceLevel& level) -> int64_t
    {
        int64_t total = 0;

        for (OrderSlot slot = level.first_slot; slot != kNoSlot; slot = slot_at(slot).next_slot)
        {
            const int32_t left = slot_at(slot).remaining_quantity;

            if (left > 0)
            {
                ++resting_count_;
                total += left;
            }
        }

        level.total_quantity = total;

        return total;
    };

    recount(buy_market_);
    recount(sell_market_);

    for (size_t level = 0; level < buy_levels_.size(); ++level)
    {
        const int64_t buy_total = recount(buy_levels_[level]);
        resting_limit_quantity_ += buy_total;

        if (buy_total > 0 && (highest_buy_level_ == kNoLevel || level > highest_buy_level_))
        {
            highest_buy_level_ = level;
        }

        const int64_t sell_total = recount(sell_levels_[level]);
        resting_limit_quantity_ += sell_total;

        if (sell_total > 0 && lowest_sell_level_ == kNoLevel)
        {
            lowest_sell_level_ = level;
        }
    }

    return total_matched - remaining;
}

int64_t SymbolBook::take_from_level(PriceLevel& level, int32_t wanted_quantity, uint64_t counterparty_order_id,
                                    OrderSide resting_side, PriceKrw execution_price,
                                    const std::function<void(const Execution&)>& on_execution)
{
    int64_t taken = 0;

    while (taken < wanted_quantity && level.first_slot != kNoSlot)
    {
        RestingOrder& resting = slot_at(level.first_slot);

        if (resting.remaining_quantity <= 0)
        {
            level.first_slot = resting.next_slot;

            continue;
        }

        const int32_t quantity =
            static_cast<int32_t>(std::min<int64_t>(resting.remaining_quantity, wanted_quantity - taken));

        resting.remaining_quantity -= quantity;
        level.total_quantity -= quantity;
        taken += quantity;

        if (resting.remaining_quantity == 0)
        {
            --resting_count_;
        }

        if (on_execution)
        {
            const uint64_t buy_order_id  = resting_side == OrderSide::BUY ? resting.order_id : counterparty_order_id;
            const uint64_t sell_order_id = resting_side == OrderSide::BUY ? counterparty_order_id : resting.order_id;
            on_execution(Execution{buy_order_id, sell_order_id, symbol::kNone, execution_price, quantity});
        }
    }

    if (level.first_slot == kNoSlot)
    {
        level.last_slot = kNoSlot;
    }

    return taken;
}

int64_t SymbolBook::match(const IncomingOrder& order, const std::function<void(const Execution&)>& on_execution)
{
    if (!ready() || order.quantity <= 0)
    {
        return 0;
    }

    const bool is_buy = order.side == OrderSide::BUY;

    if (!is_buy && order.side != OrderSide::SELL)
    {
        return 0;
    }

    int32_t remaining = order.quantity;

    // 상대편에 시장가가 남아 있으면 먼저 맞춘다 — 가격을 안 가린 주문이라 어느 가격에도 응한다.
    //  체결가는 들어온 쪽 가격을 쓰되, 들어온 쪽도 시장가면 기준가로 잡는다(둘 다 가격이 없는 경우).
    {
        PriceLevel&     opposite_queue = is_buy ? sell_market_ : buy_market_;
        const OrderSide resting_side   = is_buy ? OrderSide::SELL : OrderSide::BUY;
        const PriceKrw  price = order.price_krw == kMarketOrderPrice ? reference_price_krw_ : order.price_krw;

        remaining -= static_cast<int32_t>(
            take_from_level(opposite_queue, remaining, order.order_id, resting_side, price, on_execution));
    }

    // 지정가 상대편을 최우선호가부터 훑는다. 매수가 들어왔으면 낮은 매도부터, 매도가 들어왔으면 높은 매수부터.
    while (remaining > 0)
    {
        const size_t level_index = is_buy ? lowest_sell_level_ : highest_buy_level_;

        if (level_index == kNoLevel)
        {
            break;
        }

        const PriceKrw level_price = level_price_[level_index];

        if (order.price_krw != kMarketOrderPrice)
        {
            const bool crosses = is_buy ? order.price_krw >= level_price : order.price_krw <= level_price;

            if (!crosses)
            {
                break;
            }
        }

        PriceLevel&     level        = is_buy ? sell_levels_[level_index] : buy_levels_[level_index];
        const OrderSide resting_side = is_buy ? OrderSide::SELL : OrderSide::BUY;
        const int64_t   taken =
            take_from_level(level, remaining, order.order_id, resting_side, level_price, on_execution);

        resting_limit_quantity_ -= taken;
        remaining -= static_cast<int32_t>(taken);

        if (level.total_quantity > 0)
        {
            break;
        }

        // 이 레벨이 비었으니 다음 최우선호가로 경계를 민다.
        if (is_buy)
        {
            size_t next_level = level_index + 1;

            while (next_level < sell_levels_.size() && sell_levels_[next_level].total_quantity == 0)
            {
                ++next_level;
            }

            lowest_sell_level_ = next_level < sell_levels_.size() ? next_level : kNoLevel;
        }
        else
        {
            if (level_index == 0)
            {
                highest_buy_level_ = kNoLevel;
            }
            else
            {
                size_t next_level = level_index - 1;

                while (next_level > 0 && buy_levels_[next_level].total_quantity == 0)
                {
                    --next_level;
                }

                highest_buy_level_ = buy_levels_[next_level].total_quantity > 0 ? next_level : kNoLevel;
            }
        }
    }

    // 남은 수량은 오더북에 쌓는다. 시장가는 상대가 없으면 남기지 않는다(취소) — 가격 없는 주문을 장부에
    //  남겨 두면 다음에 들어오는 아무 주문과나 맞아 버린다.
    if (remaining > 0 && order.price_krw != kMarketOrderPrice)
    {
        IncomingOrder leftover = order;
        leftover.quantity      = remaining;
        accumulate(leftover);
    }

    return order.quantity - remaining;
}

PriceKrw SymbolBook::best_bid_krw() const
{
    return highest_buy_level_ == kNoLevel ? 0 : level_price_[highest_buy_level_];
}

PriceKrw SymbolBook::best_ask_krw() const
{
    return lowest_sell_level_ == kNoLevel ? 0 : level_price_[lowest_sell_level_];
}

void SymbolBook::clear_orders()
{
    for (PriceLevel& level : buy_levels_)
    {
        level = PriceLevel{};
    }

    for (PriceLevel& level : sell_levels_)
    {
        level = PriceLevel{};
    }

    buy_market_  = PriceLevel{};
    sell_market_ = PriceLevel{};

    // 자리는 줄에서 떼는 것으로 끝나지 않는다 — 저장소를 통째로 놓아야 메모리가 돌아온다.
    order_blocks_.clear();
    order_slot_count_ = 0;

    highest_buy_level_      = kNoLevel;
    lowest_sell_level_      = kNoLevel;
    resting_count_          = 0;
    resting_limit_quantity_ = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// MatchingEngine
// ─────────────────────────────────────────────────────────────────────────────

void MatchingEngine::reserve(size_t symbol_count)
{
    books_.resize(symbol_count + 1);
}

void MatchingEngine::configure_symbol(symbol::SymbolId symbol_id, PriceKrw reference_price_krw)
{
    if (symbol_id == symbol::kNone)
    {
        return;
    }

    if (symbol_id >= books_.size())
    {
        books_.resize(symbol_id + 1);
    }

    books_[symbol_id].configure(reference_price_krw);
}

SymbolBook* MatchingEngine::book_of(symbol::SymbolId symbol_id)
{
    if (symbol_id == symbol::kNone || symbol_id >= books_.size())
    {
        return nullptr;
    }

    return &books_[symbol_id];
}

const SymbolBook* MatchingEngine::book_of(symbol::SymbolId symbol_id) const
{
    if (symbol_id == symbol::kNone || symbol_id >= books_.size())
    {
        return nullptr;
    }

    return &books_[symbol_id];
}

bool MatchingEngine::accumulate(const IncomingOrder& order)
{
    SymbolBook* book = book_of(order.symbol_id);

    if (book == nullptr || !book->accumulate(order))
    {
        ++rejected_count_;

        return false;
    }

    ++accepted_count_;

    return true;
}

int64_t MatchingEngine::run_auction(symbol::SymbolId symbol_id)
{
    SymbolBook* book = book_of(symbol_id);

    if (book == nullptr)
    {
        return 0;
    }

    // 체결에 종목을 실어 준다 — SymbolBook은 자기가 어느 종목인지 모른다(배열 인덱스가 곧 종목이라).
    const auto stamp_symbol = [&](const Execution& execution)
    {
        ++execution_count_;

        if (!on_execution_)
        {
            return;
        }

        Execution stamped = execution;
        stamped.symbol_id = symbol_id;
        on_execution_(stamped);
    };

    const int64_t matched = book->run_auction(stamp_symbol);
    executed_quantity_ += matched;

    return matched;
}

int64_t MatchingEngine::run_auction_all()
{
    int64_t total = 0;

    for (symbol::SymbolId symbol_id = 1; symbol_id < books_.size(); ++symbol_id)
    {
        if (books_[symbol_id].ready())
        {
            total += run_auction(symbol_id);
        }
    }

    return total;
}

int64_t MatchingEngine::match(const IncomingOrder& order)
{
    SymbolBook* book = book_of(order.symbol_id);

    if (book == nullptr)
    {
        ++rejected_count_;

        return 0;
    }

    const auto stamp_symbol = [&](const Execution& execution)
    {
        ++execution_count_;

        if (!on_execution_)
        {
            return;
        }

        Execution stamped = execution;
        stamped.symbol_id = order.symbol_id;
        on_execution_(stamped);
    };

    const int64_t matched = book->match(order, stamp_symbol);
    executed_quantity_ += matched;
    ++accepted_count_;

    return matched;
}

int64_t MatchingEngine::resting_count() const
{
    int64_t total = 0;

    for (const SymbolBook& book : books_)
    {
        total += book.resting_count();
    }

    return total;
}

} // namespace exchange
