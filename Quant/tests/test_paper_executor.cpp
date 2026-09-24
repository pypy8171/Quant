// PaperExecutor 단위 테스트 — 다음 틱 체결, 지정가 대기, 매도가능·현금 한도 거부, 취소·정정, 장부/잔고.
// 빌드: cmake --build <directory> --target test_paper_executor
#include "core/PaperExecutor.h"
#include "core/SymbolTable.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(condition))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

OrderSignal signal(const std::string& ticker, OrderSide side, int quantity, double price, double reference = 0.0)
{
    OrderSignal signal;
    signal.ticker    = ticker;
    signal.side      = side;
    signal.type      = price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
    signal.quantity  = quantity;
    signal.price     = price;
    signal.reference_price = reference;
    return signal;
}

// 테스트 틱 — id는 안 찍는다. 체결기가 테이블로 푸는 경로(옛 경로·주입 피드)를 같이 검사한다.
TradeData tick(std::string_view ticker, double price, int32_t hhmmss)
{
    TradeData trade;
    trade.ticker = ticker;
    trade.price  = price;
    trade.hhmmss = hhmmss;
    return trade;
}

bool near(double amount, double base)
{
    return amount > base - 1e-6 && amount < base + 1e-6;
}
} // namespace

int main()
{
    symbol::SymbolTable           symbols;
    feed::PaperExecutor           executor(1'000'000.0, symbols);
    std::vector<FillNotification> fills;
    executor.set_fill_callback([&fills](const FillNotification& fill_notification) { fills.push_back(fill_notification); });

    // 1. 시장가 매수: 접수 시점엔 체결 없음, 다음 틱 가격에 체결. 장부·현금 반영.
    {
        const auto acknowledgement = executor.submit_order_acknowledgement(signal("005930", OrderSide::BUY, 10, 0.0, 70000.0));
        CHECK(acknowledgement.ok() && acknowledgement.kis_order_no == "9000000001" && acknowledgement.krx_forwarding_org_no == "PAPER");
        CHECK(fills.empty() && executor.open_count() == 1);
        executor.on_tick(tick("000660", 100000.0, 90100)); // 다른 종목 틱은 무관
        CHECK(fills.empty());
        executor.on_tick(tick("005930", 70100.0, 90101));
        CHECK(fills.size() == 1 && fills[0].kis_order_no == "9000000001" && fills[0].filled_quantity == 10);
        CHECK(near(fills[0].filled_price, 70100.0) && fills[0].fill_time == "090101" &&
              fills[0].side == OrderSide::BUY);
        CHECK(executor.open_count() == 0 && executor.fills() == 1);
        CHECK(near(executor.cash(), 1'000'000.0 - 701'000.0));
        const auto balance = executor.balance();
        CHECK(balance.has_value() && balance->holdings.size() == 1 && balance->holdings[0].quantity == 10);
        CHECK(near(balance->holdings[0].average_price, 70100.0));
        CHECK(balance->total_evaluation_amount && near(*balance->total_evaluation_amount, 1'000'000.0));
    }

    // 2. 지정가 매도: 가격이 닿기 전엔 대기, 닿으면 틱 가격에 체결. 대기 중엔 매도가능수량이 준다.
    {
        const auto acknowledgement = executor.submit_order_acknowledgement(signal("005930", OrderSide::SELL, 4, 71000.0));
        CHECK(acknowledgement.ok());
        executor.on_tick(tick("005930", 70500.0, 90200));
        CHECK(fills.size() == 1);
        const auto balance = executor.balance();
        CHECK(balance.has_value() && balance->holdings[0].sellable_quantity && *balance->holdings[0].sellable_quantity == 6);
        const auto opens = executor.get_open_orders();
        CHECK(opens.size() == 1 && opens[0].kis_order_no == acknowledgement.kis_order_no && opens[0].psbl_qty == 4 &&
              opens[0].side == OrderSide::SELL);
        executor.on_tick(tick("005930", 71200.0, 90300));
        CHECK(fills.size() == 2 && fills[1].side == OrderSide::SELL && near(fills[1].filled_price, 71200.0));
        CHECK(near(executor.cash(), 1'000'000.0 - 701'000.0 + 4 * 71200.0));
    }

    // 3. 거부: 보유 초과 매도는 40240000, 현금 초과 매수는 E_PAPER_CASH, 수량 0은 E_PAPER_ARG.
    {
        const auto submit_order_acknowledgement_a = executor.submit_order_acknowledgement(signal("005930", OrderSide::SELL, 7, 0.0, 70000.0));
        CHECK(!submit_order_acknowledgement_a.ok() && submit_order_acknowledgement_a.error_code == kis_error::kNoSellableQty);
        const auto submit_order_acknowledgement_b = executor.submit_order_acknowledgement(signal("000660", OrderSide::BUY, 100, 100000.0));
        CHECK(!submit_order_acknowledgement_b.ok() && submit_order_acknowledgement_b.error_code == "E_PAPER_CASH");
        const auto submit_order_acknowledgement_c = executor.submit_order_acknowledgement(signal("000660", OrderSide::BUY, 0, 100000.0));
        CHECK(!submit_order_acknowledgement_c.ok() && submit_order_acknowledgement_c.error_code == "E_PAPER_ARG");
        CHECK(executor.open_count() == 0);
    }

    // 4. 대기 매수의 명목은 현금에서 미리 잡힌다.
    {
        const auto submit_order_acknowledgement_a = executor.submit_order_acknowledgement(signal("000660", OrderSide::BUY, 5, 100000.0));
        CHECK(submit_order_acknowledgement_a.ok());
        const auto submit_order_acknowledgement_b = executor.submit_order_acknowledgement(signal("000660", OrderSide::BUY, 2, 100000.0));
        CHECK(!submit_order_acknowledgement_b.ok() && submit_order_acknowledgement_b.error_code == "E_PAPER_CASH");
        const auto cancel = executor.cancel_order("000660", submit_order_acknowledgement_a.kis_order_no, "PAPER", 0, /*all_remaining=*/true);
        CHECK(cancel.ok() && executor.open_count() == 0);
    }

    // 5. 취소·정정: 부분 취소는 잔량을 줄이고, 정정은 새 접수번호로 가격·수량을 바꾼다. 없는 주문은 실패.
    {
        const auto submit_order_acknowledgement = executor.submit_order_acknowledgement(signal("005930", OrderSide::SELL, 6, 80000.0));
        CHECK(submit_order_acknowledgement.ok());
        const auto part = executor.cancel_order("005930", submit_order_acknowledgement.kis_order_no, "PAPER", 2, false);
        CHECK(part.ok() && executor.get_open_orders()[0].psbl_qty == 4);
        const auto revise_result = executor.revise_order("005930", submit_order_acknowledgement.kis_order_no, "PAPER", 3, 70000.0);
        CHECK(revise_result.ok() && revise_result.kis_order_no != submit_order_acknowledgement.kis_order_no);
        CHECK(executor.get_open_orders()[0].kis_order_no == revise_result.kis_order_no && executor.get_open_orders()[0].psbl_qty == 3);
        const auto gone = executor.cancel_order("005930", submit_order_acknowledgement.kis_order_no, "PAPER", 0, true);
        CHECK(!gone.ok() && gone.error_code == "E_PAPER_NO_ORDER");
        executor.on_tick(tick("005930", 70000.0, 90400));
        CHECK(fills.size() == 3 && fills[2].kis_order_no == revise_result.kis_order_no && fills[2].filled_quantity == 3);
        const auto balance = executor.balance();
        CHECK(balance.has_value() && balance->holdings.size() == 1 && balance->holdings[0].quantity == 3);
    }

    // 6. 전량 매도 뒤 장부에서 빠진다.
    {
        const auto submit_order_acknowledgement = executor.submit_order_acknowledgement(signal("005930", OrderSide::SELL, 3, 0.0, 70000.0));
        CHECK(submit_order_acknowledgement.ok());
        executor.on_tick(tick("005930", 69000.0, 90500));
        const auto balance = executor.balance();
        CHECK(balance.has_value() && balance->holdings.empty());
        CHECK(executor.is_paper());
    }

    // 7. 수신 스레드 둘이 서로 다른 종목 틱을 같이 넣어도 체결 전달은 한 번에 하나다(CODE_REVIEW W-6).
    //  받는 쪽 체결 큐가 SPSC라, 콜백이 겹쳐 불리면 실제 엔진에서는 체결이 사라진다.
    {
        symbol::SymbolTable concurrent_symbols;
        feed::PaperExecutor concurrent_executor(1e12, concurrent_symbols);
        std::atomic<bool>   delivering{false};
        std::atomic<int>    overlap_count{0};
        std::atomic<int>    delivered_count{0};
        concurrent_executor.set_fill_callback(
            [&](const FillNotification&)
            {
                if (delivering.exchange(true))
                {
                    overlap_count.fetch_add(1);
                }

                // 스레드마다 첫 전달에서 50ms 머문다 — 전달이 한 줄로 서 있지 않으면 그 사이 다른 스레드가 들어온다.
                thread_local bool first_delivery = true;

                if (first_delivery)
                {
                    first_delivery = false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                delivering.store(false);
                delivered_count.fetch_add(1);
            });

        constexpr int kOrdersPerTicker = 300;

        for (int order_index = 0; order_index < kOrdersPerTicker; ++order_index)
        {
            CHECK(concurrent_executor.submit_order_acknowledgement(signal("005930", OrderSide::BUY, 1, 0.0, 1000.0)).ok());
            CHECK(concurrent_executor.submit_order_acknowledgement(signal("000660", OrderSide::BUY, 1, 0.0, 1000.0)).ok());
        }

        std::atomic<bool> start{false};
        std::thread first([&] { while (!start.load()) {} concurrent_executor.on_tick(tick("005930", 1000.0, 90600)); });
        std::thread second([&] { while (!start.load()) {} concurrent_executor.on_tick(tick("000660", 1000.0, 90600)); });
        start.store(true);
        first.join();
        second.join();
        CHECK(delivered_count.load() == 2 * kOrdersPerTicker);
        CHECK(overlap_count.load() == 0);
    }

    std::cout << "test_paper_executor: " << g_checks << " checks passed\n";
    return 0;
}
