#include "strategy/MarketMakingStrategy.h"

std::string MarketMakingStrategy::describe() const
{
    return "MarketMaking | " + ticker_ + " | qty=" + std::to_string(quantity_) +
           " half_spread=" + std::to_string(half_spread_ticks_) + "tk" +
           " requote_move=" + std::to_string(requote_move_ticks_) + "tk" +
           " min_requote=" + std::to_string(min_requote_.count()) + "ms";
}

void MarketMakingStrategy::on_start()
{
    symbol_id_ = symbol_of(ticker_);
    bid_order_id_.clear();
    ask_order_id_.clear();
    bid_order_number_ = 0;
    ask_order_number_ = 0;
    last_mid_ = 0.0;
    last_requote_ = std::chrono::steady_clock::time_point{};
    sequence_ = 0;
}

void MarketMakingStrategy::on_order_book_batch(const OrderBook& order_book, std::vector<OrderSignal>& out)
{
    if (!order_book.ticker.empty() && !same_symbol(symbol_id_, ticker_, order_book.symbol_id, order_book.ticker))
    {
        return;
    }

    const double best_bid = order_book.bids[0].price;
    const double best_ask = order_book.asks[0].price;

    // 장전·비어있는 호가(구독 직후/휴장) → 견적 안 냄.
    // TODO(호가 소싱 폴백, 기본 비활성): best_bid/ask1가 지속 0이면 kis_->get_current_price()로
    //   임시 mid_price 구성 가능하나 REST 저지연 아님 → 견적 억제가 원칙. config 플래그로만 노출 예정.
    if (best_bid <= 0.0 || best_ask <= 0.0 || best_ask < best_bid)
    {
        return;
    }

    const double mid_price = (best_bid + best_ask) / 2.0;
    const double tick = tick_size(mid_price);

    const bool have_live = !bid_order_id_.empty() || !ask_order_id_.empty();
    const auto now = std::chrono::steady_clock::now();

    // ── 재호가 게이트 ──────────────────────────────────────────────────
    if (have_live)
    {
        // (a) 최소 간격 미달 → skip
        if (now - last_requote_ < min_requote_)
        {
            return;
        }

        // (b) mid_price 이동이 requote_move_ticks 미만 → skip (폭주 방지)
        if (std::fabs(mid_price - last_mid_) < requote_move_ticks_ * tick)
        {
            return;
        }
    }

    // ── 목표 견적가 (스프레드 보존: 매수 내림 / 매도 올림) ───────────────
    const double raw_bid = mid_price - half_spread_ticks_ * tick;
    const double raw_ask = mid_price + half_spread_ticks_ * tick;
    const double desired_bid = round_to_tick(raw_bid, OrderSide::BUY);
    const double desired_ask = round_to_tick(raw_ask, OrderSide::SELL);

    if (desired_bid <= 0.0 || desired_ask <= desired_bid)
    {
        return; // 비정상 가격(교차/음수) → 이번 틱 skip
    }

    // ── 기존 견적 취소 (있으면) ─────────────────────────────────────────
    if (!bid_order_id_.empty())
    {
        out.push_back(make_cancel(std::move(bid_order_id_), bid_order_number_, OrderSide::BUY));
        bid_order_id_.clear();
        bid_order_number_ = 0;
    }

    if (!ask_order_id_.empty())
    {
        out.push_back(make_cancel(std::move(ask_order_id_), ask_order_number_, OrderSide::SELL));
        ask_order_id_.clear();
        ask_order_number_ = 0;
    }

    // ── 신규 양방향 지정가 ──────────────────────────────────────────────
    bid_order_id_ = next_order_id("B");
    ask_order_id_ = next_order_id("A");
    bid_order_number_ = next_client_order_number();
    ask_order_number_ = next_client_order_number();
    out.push_back(make_new(bid_order_id_, bid_order_number_, OrderSide::BUY, desired_bid));
    out.push_back(make_new(ask_order_id_, ask_order_number_, OrderSide::SELL, desired_ask));

    last_mid_ = mid_price;
    last_requote_ = now;
}

OrderSignal MarketMakingStrategy::make_new(const std::string& order_id, uint64_t order_number, OrderSide side,
                                           double price)
{
    OrderSignal signal;
    signal.ticker = ticker_;
    signal.symbol_id = symbol_id_;
    signal.side = side;
    signal.type = OrderType::LIMIT;
    signal.quantity = quantity_;
    signal.price = price;
    signal.strategy_id = id();
    signal.market = Market::KR;
    signal.action = OrderAction::NEW;
    signal.client_order_id = order_id;
    signal.client_order_number = order_number;
    signal.timestamp = std::chrono::system_clock::now();
    return signal;
}

OrderSignal MarketMakingStrategy::make_cancel(std::string original_order_id, uint64_t original_order_number,
                                              OrderSide side)
{
    OrderSignal signal;
    signal.ticker = ticker_;
    signal.symbol_id = symbol_id_;
    signal.side = side; // 참고용(취소 라우팅은 원주문 정보 사용). Engine NONE 가드는 action으로 우회.
    signal.type = OrderType::LIMIT;
    signal.quantity = 0;
    signal.strategy_id = id();
    signal.market = Market::KR;
    signal.action = OrderAction::CANCEL;
    signal.original_client_order_id = std::move(original_order_id);
    signal.original_client_order_number = original_order_number;
    signal.timestamp = std::chrono::system_clock::now();
    return signal;
}
