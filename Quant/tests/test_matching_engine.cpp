// exchange::MatchingEngine 단위 테스트 — 호가 격자, 동시호가 적재, 단일가 균형가 3단 판정,
//  단일가 일괄 체결, 연속매매(지정가 교차·시장가·부분 체결·잔량 적재).
// 빌드: cmake --build <directory> --target test_matching_engine
#include "exchange/MatchingEngine.h"

#include <iostream>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        ++g_checks;                                                                             \
        if (!(condition))                                                                       \
        {                                                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                           \
        }                                                                                       \
    } while (false)

constexpr symbol::SymbolId kTestSymbol    = 1;
constexpr exchange::PriceKrw kReferencePrice = 10000;

exchange::IncomingOrder make_order(uint64_t order_id, exchange::PriceKrw price_krw, int32_t quantity, OrderSide side)
{
    exchange::IncomingOrder order;
    order.order_id  = order_id;
    order.symbol_id = kTestSymbol;
    order.price_krw = price_krw;
    order.quantity  = quantity;
    order.side      = side;

    return order;
}

} // namespace

int main()
{
    // ── 호가 격자 ────────────────────────────────────────────────────────────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);

        CHECK(book.ready());
        CHECK(book.reference_price_krw() == kReferencePrice);
        CHECK(book.level_of_price(kReferencePrice) != exchange::SymbolBook::kNoLevel);

        // 1만원대 호가단위는 10원이다 — 격자에 없는 가격은 레벨이 없다.
        CHECK(book.level_of_price(10005) == exchange::SymbolBook::kNoLevel);
        CHECK(book.level_of_price(10010) != exchange::SymbolBook::kNoLevel);

        // 상하한가 ±30% 밖은 격자에 없다.
        CHECK(book.level_of_price(20000) == exchange::SymbolBook::kNoLevel);
        CHECK(book.level_of_price(1000) == exchange::SymbolBook::kNoLevel);

        // 격자는 오름차순이라 인덱스가 커지면 가격도 커진다.
        CHECK(book.price_of_level(0) < book.price_of_level(book.level_count() - 1));
    }

    // ── 동시호가 적재: 맞추지 않고 쌓기만 한다 ───────────────────────────────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);

        CHECK(book.accumulate(make_order(1, 10100, 100, OrderSide::BUY)));
        CHECK(book.accumulate(make_order(2, 9900, 100, OrderSide::SELL)));

        // 매수 10,100 > 매도 9,900 으로 교차하지만 accumulate는 체결하지 않는다.
        CHECK(book.resting_count() == 2);
        CHECK(book.resting_quantity() == 200);
        CHECK(book.best_bid_krw() == 10100);
        CHECK(book.best_ask_krw() == 9900);

        // 격자 밖 가격·0 이하 수량은 받지 않는다.
        CHECK(!book.accumulate(make_order(3, 10005, 10, OrderSide::BUY)));
        CHECK(!book.accumulate(make_order(4, 10000, 0, OrderSide::BUY)));
        CHECK(book.resting_count() == 2);
    }

    // ── 균형가 판정 ③: 체결량·잔량이 같으면 기준가에 가장 가까운 가격 ───────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);
        book.accumulate(make_order(1, 10100, 100, OrderSide::BUY));
        book.accumulate(make_order(2, 9900, 100, OrderSide::SELL));

        // 9,900~10,100 어느 가격에서도 100주가 맞고 잔량은 0이다 — 기준가 10,000이 남는다.
        CHECK(book.find_auction_price() == kReferencePrice);
    }

    // ── 균형가 판정 ①: 체결 가능 수량이 가장 큰 가격이 먼저다 ───────────────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);

        // 매수는 10,050에 300주뿐, 매도는 10,050에 300주·10,100에 500주.
        //  10,100에서는 매수 누적이 0이라 안 맞고, 10,050에서 300주가 맞는다.
        book.accumulate(make_order(1, 10050, 300, OrderSide::BUY));
        book.accumulate(make_order(2, 10050, 300, OrderSide::SELL));
        book.accumulate(make_order(3, 10100, 500, OrderSide::SELL));

        CHECK(book.find_auction_price() == 10050);
    }

    // ── 단일가 일괄 체결 ─────────────────────────────────────────────────────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);
        book.accumulate(make_order(1, 10100, 100, OrderSide::BUY));
        book.accumulate(make_order(2, 9900, 60, OrderSide::SELL));
        book.accumulate(make_order(3, 9900, 40, OrderSide::SELL));

        std::vector<exchange::Execution> executions;
        const int64_t matched = book.run_auction([&](const exchange::Execution& execution)
                                                 {
                                                     executions.push_back(execution);
                                                 });

        CHECK(matched == 100);
        CHECK(executions.size() == 2);

        // 단일가는 한 가격이다 — 매도 호가가 9,900이어도 체결가는 균형가 하나뿐.
        CHECK(executions[0].price_krw == kReferencePrice);
        CHECK(executions[1].price_krw == kReferencePrice);

        // 시간 우선 — 먼저 들어온 2번 매도가 먼저 맞는다.
        CHECK(executions[0].sell_order_id == 2);
        CHECK(executions[0].quantity == 60);
        CHECK(executions[1].sell_order_id == 3);
        CHECK(executions[1].quantity == 40);
        CHECK(executions[0].buy_order_id == 1);

        // 양쪽이 다 맞았으니 잔량이 없다.
        CHECK(book.resting_count() == 0);
        CHECK(book.resting_quantity() == 0);
    }

    // ── 단일가 부분 체결: 못 맞은 잔량은 오더북에 남는다 ─────────────────────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);
        book.accumulate(make_order(1, 10100, 100, OrderSide::BUY));
        book.accumulate(make_order(2, 9900, 30, OrderSide::SELL));

        const int64_t matched = book.run_auction(nullptr);

        CHECK(matched == 30);
        CHECK(book.resting_count() == 1);
        CHECK(book.resting_quantity() == 70);
        CHECK(book.best_bid_krw() == 10100);
        CHECK(book.best_ask_krw() == 0);
    }

    // ── 연속매매: 지정가 교차·부분 체결·잔량 적재 ───────────────────────────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);
        book.accumulate(make_order(1, 10010, 50, OrderSide::SELL));

        std::vector<exchange::Execution> executions;
        const auto collect = [&](const exchange::Execution& execution)
        {
            executions.push_back(execution);
        };

        // 30주만 사면 매도 20주가 남는다.
        CHECK(book.match(make_order(2, 10010, 30, OrderSide::BUY), collect) == 30);
        CHECK(executions.size() == 1);
        CHECK(executions[0].price_krw == 10010);
        CHECK(book.best_ask_krw() == 10010);
        CHECK(book.resting_quantity() == 20);

        // 값이 안 맞는 매수는 체결되지 않고 매수호가로 쌓인다.
        executions.clear();
        CHECK(book.match(make_order(3, 10000, 10, OrderSide::BUY), collect) == 0);
        CHECK(executions.empty());
        CHECK(book.best_bid_krw() == kReferencePrice);

        // 남은 매도 20주를 먹고 30주는 매수호가로 쌓인다 — 매도가 비면 최우선 매도호가가 사라진다.
        executions.clear();
        CHECK(book.match(make_order(4, 10010, 50, OrderSide::BUY), collect) == 20);
        CHECK(executions.size() == 1);
        CHECK(executions[0].quantity == 20);
        CHECK(book.best_ask_krw() == 0);
        CHECK(book.best_bid_krw() == 10010);
    }

    // ── 연속매매: 시장가는 가격을 안 가리고 최우선호가부터 훑는다 ────────────
    {
        exchange::SymbolBook book;
        book.configure(kReferencePrice);
        book.accumulate(make_order(1, 10010, 10, OrderSide::SELL));
        book.accumulate(make_order(2, 10020, 10, OrderSide::SELL));

        std::vector<exchange::Execution> executions;
        const int64_t matched =
            book.match(make_order(3, exchange::kMarketOrderPrice, 15, OrderSide::BUY),
                       [&](const exchange::Execution& execution)
                       {
                           executions.push_back(execution);
                       });

        CHECK(matched == 15);
        CHECK(executions.size() == 2);
        CHECK(executions[0].price_krw == 10010);
        CHECK(executions[0].quantity == 10);
        CHECK(executions[1].price_krw == 10020);
        CHECK(executions[1].quantity == 5);

        // 시장가는 상대가 없어도 오더북에 남기지 않는다 — 가격 없는 주문이 장부에 남으면 다음 주문과 무조건 맞는다.
        executions.clear();
        CHECK(book.match(make_order(4, exchange::kMarketOrderPrice, 100, OrderSide::BUY), nullptr) == 5);
        CHECK(book.best_bid_krw() == 0);
    }

    // ── MatchingEngine: 종목 id로 오더북을 고르고 체결에 종목을 실어 준다 ────
    {
        exchange::MatchingEngine engine;
        engine.reserve(2);
        engine.configure_symbol(kTestSymbol, kReferencePrice);
        engine.configure_symbol(2, kReferencePrice);

        std::vector<exchange::Execution> executions;
        engine.set_execution_callback([&](const exchange::Execution& execution)
                                      {
                                          executions.push_back(execution);
                                      });

        CHECK(engine.accumulate(make_order(1, 10100, 100, OrderSide::BUY)));
        CHECK(engine.accumulate(make_order(2, 9900, 100, OrderSide::SELL)));
        CHECK(engine.accepted_count() == 2);
        CHECK(engine.resting_count() == 2);

        // 격자를 안 만든 종목은 거부한다.
        exchange::IncomingOrder unknown = make_order(3, kReferencePrice, 10, OrderSide::BUY);
        unknown.symbol_id               = 9;
        CHECK(!engine.accumulate(unknown));
        CHECK(engine.rejected_count() == 1);

        CHECK(engine.run_auction_all() == 100);
        CHECK(engine.executed_quantity() == 100);
        CHECK(engine.execution_count() == 1);
        CHECK(executions.size() == 1);
        CHECK(executions[0].symbol_id == kTestSymbol);
        CHECK(executions[0].price_krw == kReferencePrice);
        CHECK(engine.resting_count() == 0);

        // 연속매매도 같은 오더북을 쓴다.
        executions.clear();
        CHECK(engine.match(make_order(4, 10050, 10, OrderSide::SELL)) == 0);
        CHECK(engine.match(make_order(5, 10050, 10, OrderSide::BUY)) == 10);
        CHECK(engine.execution_count() == 2);
        CHECK(executions.size() == 1);
        CHECK(executions[0].symbol_id == kTestSymbol);
    }

    std::cout << "OK test_matching_engine  checks=" << g_checks << "\n";

    return 0;
}
