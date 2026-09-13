// PaperExecutor 단위 테스트 — 다음 틱 체결, 지정가 대기, 매도가능·현금 한도 거부, 취소·정정, 장부/잔고.
// 빌드: cmake --build <dir> --target test_paper_executor
#include "core/PaperExecutor.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(cond)                                                                        \
    do                                                                                     \
    {                                                                                      \
        ++g_checks;                                                                        \
        if (!(cond))                                                                       \
        {                                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";     \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

OrderSignal sig(const std::string& ticker, OrderSide side, int qty, double price, double ref = 0.0)
{
    OrderSignal s;
    s.ticker    = ticker;
    s.side      = side;
    s.type      = price > 0.0 ? OrderType::LIMIT : OrderType::MARKET;
    s.quantity  = qty;
    s.price     = price;
    s.ref_price = ref;
    return s;
}

bool near(double a, double b)
{
    return a > b - 1e-6 && a < b + 1e-6;
}
} // namespace

int main()
{
    feed::PaperExecutor           ex(1'000'000.0);
    std::vector<FillNotification> fills;
    ex.set_fill_callback([&fills](const FillNotification& fn) { fills.push_back(fn); });

    // 1. 시장가 매수: 접수 시점엔 체결 없음, 다음 틱 가격에 체결. 장부·현금 반영.
    {
        const auto ack = ex.submit_order_ack(sig("005930", OrderSide::BUY, 10, 0.0, 70000.0));
        CHECK(ack.ok() && ack.odno == "P000000001" && ack.krx_orgno == "PAPER");
        CHECK(fills.empty() && ex.open_count() == 1);
        ex.on_tick("000660", 100000.0, "090100"); // 다른 종목 틱은 무관
        CHECK(fills.empty());
        ex.on_tick("005930", 70100.0, "090101");
        CHECK(fills.size() == 1 && fills[0].odno == "P000000001" && fills[0].filled_qty == 10);
        CHECK(near(fills[0].filled_price, 70100.0) && fills[0].fill_time == "090101" &&
              fills[0].side == OrderSide::BUY);
        CHECK(ex.open_count() == 0 && ex.fills() == 1);
        CHECK(near(ex.cash(), 1'000'000.0 - 701'000.0));
        const auto b = ex.balance();
        CHECK(b.has_value() && b->holdings.size() == 1 && b->holdings[0].qty == 10);
        CHECK(near(b->holdings[0].avg_price, 70100.0));
        CHECK(b->total_eval_amt && near(*b->total_eval_amt, 1'000'000.0));
    }

    // 2. 지정가 매도: 가격이 닿기 전엔 대기, 닿으면 틱 가격에 체결. 대기 중엔 매도가능수량이 준다.
    {
        const auto ack = ex.submit_order_ack(sig("005930", OrderSide::SELL, 4, 71000.0));
        CHECK(ack.ok());
        ex.on_tick("005930", 70500.0, "090200");
        CHECK(fills.size() == 1);
        const auto b = ex.balance();
        CHECK(b.has_value() && b->holdings[0].sellable_qty && *b->holdings[0].sellable_qty == 6);
        const auto opens = ex.get_open_orders();
        CHECK(opens.size() == 1 && opens[0].odno == ack.odno && opens[0].psbl_qty == 4 &&
              opens[0].side == OrderSide::SELL);
        ex.on_tick("005930", 71200.0, "090300");
        CHECK(fills.size() == 2 && fills[1].side == OrderSide::SELL && near(fills[1].filled_price, 71200.0));
        CHECK(near(ex.cash(), 1'000'000.0 - 701'000.0 + 4 * 71200.0));
    }

    // 3. 거부: 보유 초과 매도는 40240000, 현금 초과 매수는 E_PAPER_CASH, 수량 0은 E_PAPER_ARG.
    {
        const auto a = ex.submit_order_ack(sig("005930", OrderSide::SELL, 7, 0.0, 70000.0));
        CHECK(!a.ok() && a.err_code == kis_err::kNoSellableQty);
        const auto b = ex.submit_order_ack(sig("000660", OrderSide::BUY, 100, 100000.0));
        CHECK(!b.ok() && b.err_code == "E_PAPER_CASH");
        const auto c = ex.submit_order_ack(sig("000660", OrderSide::BUY, 0, 100000.0));
        CHECK(!c.ok() && c.err_code == "E_PAPER_ARG");
        CHECK(ex.open_count() == 0);
    }

    // 4. 대기 매수의 명목은 현금에서 미리 잡힌다.
    {
        const auto a = ex.submit_order_ack(sig("000660", OrderSide::BUY, 5, 100000.0));
        CHECK(a.ok());
        const auto b = ex.submit_order_ack(sig("000660", OrderSide::BUY, 2, 100000.0));
        CHECK(!b.ok() && b.err_code == "E_PAPER_CASH");
        const auto cxl = ex.cancel_order("000660", a.odno, "PAPER", 0, /*all_remaining=*/true);
        CHECK(cxl.ok() && ex.open_count() == 0);
    }

    // 5. 취소·정정: 부분 취소는 잔량을 줄이고, 정정은 새 접수번호로 가격·수량을 바꾼다. 없는 주문은 실패.
    {
        const auto a = ex.submit_order_ack(sig("005930", OrderSide::SELL, 6, 80000.0));
        CHECK(a.ok());
        const auto part = ex.cancel_order("005930", a.odno, "PAPER", 2, false);
        CHECK(part.ok() && ex.get_open_orders()[0].psbl_qty == 4);
        const auto rev = ex.revise_order("005930", a.odno, "PAPER", 3, 70000.0);
        CHECK(rev.ok() && rev.odno != a.odno);
        CHECK(ex.get_open_orders()[0].odno == rev.odno && ex.get_open_orders()[0].psbl_qty == 3);
        const auto gone = ex.cancel_order("005930", a.odno, "PAPER", 0, true);
        CHECK(!gone.ok() && gone.err_code == "E_PAPER_NO_ORDER");
        ex.on_tick("005930", 70000.0, "090400");
        CHECK(fills.size() == 3 && fills[2].odno == rev.odno && fills[2].filled_qty == 3);
        const auto b = ex.balance();
        CHECK(b.has_value() && b->holdings.size() == 1 && b->holdings[0].qty == 3);
    }

    // 6. 전량 매도 뒤 장부에서 빠진다.
    {
        const auto a = ex.submit_order_ack(sig("005930", OrderSide::SELL, 3, 0.0, 70000.0));
        CHECK(a.ok());
        ex.on_tick("005930", 69000.0, "090500");
        const auto b = ex.balance();
        CHECK(b.has_value() && b->holdings.empty());
        CHECK(ex.is_paper());
    }

    std::cout << "test_paper_executor: " << g_checks << " checks passed\n";
    return 0;
}
