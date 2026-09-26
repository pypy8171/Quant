// tests/test_protective_orders.cpp
// 보호 주문 표 검증 (D-114 단계 1) — 전략 없이 표만으로 청산이 나가는가.
//
//   이 테스트에는 전략이 한 줄도 없다. 표에 규칙을 넣고, 원장 보유 스냅샷과 가격만 흘려 준다.
//   그래도 청산 주문이 나오면 "전략이 죽어도 보유분이 지켜진다"는 말의 증거가 된다.
//
//   ① off/shadow/owner 세 모드가 각각 무엇을 하는가
//   ② 손절·트레일 판정과 재발주 간격
//   ③ 이미 낸 미체결 매도만큼 빼고 내는가(이중 매도 금지)
//   ④ 보유가 0이 되면 최고가 기억을 비우는가(재진입 직후 오발주 금지)
//   ⑤ 리플레이 가격 경로 하나를 끝까지 흘렸을 때 딱 한 번 청산이 나가는가
//
//   사용법: test_protective_orders

#include "risk/ProtectiveOrders.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
int g_checks = 0;

void check(bool condition, const std::string& name)
{
    ++g_checks;

    if (!condition)
    {
        std::cout << "[FAIL] " << name << "\n";
        std::abort();
    }

    std::cout << "[PASS] " << name << "\n";
}

constexpr symbol::SymbolId kSymbol = 7;
const std::string          kAccount = "50204275";
const std::string          kTicker  = "005930";

std::vector<OrderGate::HeldPos> held(int quantity, double average_price)
{
    OrderGate::HeldPos holding;
    holding.account       = kAccount;
    holding.ticker        = kTicker;
    holding.quantity      = quantity;
    holding.average_price = average_price;
    holding.symbol        = kSymbol;
    return {holding};
}

risk::ProtectiveRule stop_rule(double stop_loss_percent)
{
    risk::ProtectiveRule rule;
    rule.account           = kAccount;
    rule.ticker            = kTicker;
    rule.symbol            = kSymbol;
    rule.stop_loss_percent = stop_loss_percent;
    rule.owner             = "DEVSCALE_005930";
    rule.owner_index       = 3;
    return rule;
}

// 값이 무엇이든 그 값을 현재가로 주는 가격원. 전략도 틱 콜백도 없다.
risk::ProtectiveOrderBook::PriceFn price_of(double price)
{
    return [price](symbol::SymbolId) { return price; };
}

// 미체결 잔량 — 음수가 이미 낸 매도다.
risk::ProtectiveOrderBook::SellPendingFn sell_pending_of(int sell_pending)
{
    return [sell_pending](const std::string&, symbol::SymbolId) { return sell_pending; };
}

using Clock = risk::ProtectiveOrderBook::Clock;
} // namespace

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const auto start = Clock::now();

    // ── 1. off 모드는 조건이 걸려도 아무것도 하지 않는다 ─────────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Off);
        book.arm(stop_rule(2.0));
        const auto orders = book.evaluate(held(10, 10000.0), price_of(9000.0), sell_pending_of(0), start);
        check(orders.empty(), "off 모드는 발주하지 않는다");
        check(book.statistics().shadow_hits == 0, "off 모드는 판정도 세지 않는다");
    }

    // ── 2. shadow 모드는 판정만 세고 발주는 전략에 맡긴다 ────────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Shadow);
        book.arm(stop_rule(2.0));
        const auto orders = book.evaluate(held(10, 10000.0), price_of(9000.0), sell_pending_of(0), start);
        check(orders.empty(), "shadow 모드는 발주하지 않는다");
        check(book.statistics().shadow_hits == 1, "shadow 모드는 팔았어야 할 판정을 센다");
        check(!book.owns(kAccount, kSymbol), "shadow 모드는 종목을 맡지 않는다(전략이 계속 본다)");
    }

    // ── 3. owner 모드는 전략 없이 전량 시장가 매도를 만든다 ──────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        book.arm(stop_rule(2.0));
        check(book.owns(kAccount, kSymbol), "owner 모드는 등록된 종목을 맡는다");

        const auto no_orders = book.evaluate(held(10, 10000.0), price_of(9900.0), sell_pending_of(0), start);
        check(no_orders.empty(), "평단 -1%는 손절선(-2%) 위라 팔지 않는다");

        const auto orders = book.evaluate(held(10, 10000.0), price_of(9800.0), sell_pending_of(0), start);
        check(orders.size() == 1, "평단 -2%에 닿으면 표가 청산을 만든다");
        check(orders[0].ticker == kTicker && orders[0].symbol_id == kSymbol, "종목이 규칙 그대로다");
        check(orders[0].account_id == kAccount, "계좌가 규칙 그대로다");
        check(orders[0].side == OrderSide::SELL && orders[0].type == OrderType::MARKET, "시장가 매도다");
        check(orders[0].quantity == 10, "보유 전량을 낸다");
        check(orders[0].reference_price == 9800.0, "시장가라 명목 평가용 참조가에 현재가를 찍는다");
        check(orders[0].strategy_index == 3, "손익 귀속은 등록한 전략 번호로 간다");
        check(orders[0].reason.find("보호주문:손절") == 0, "사유에 보호주문 손절이 적힌다");
        check(book.statistics().fired == 1, "발사 수를 센다");
    }

    // ── 4. 이미 낸 미체결 매도만큼 빼고 낸다 ─────────────────────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        book.arm(stop_rule(2.0));
        const auto partial = book.evaluate(held(10, 10000.0), price_of(9800.0), sell_pending_of(4), start);
        check(partial.size() == 1 && partial[0].quantity == 6, "미체결 매도 4주를 뺀 6주만 낸다");

        risk::ProtectiveOrderBook covered(risk::ProtectiveMode::Owner);
        covered.arm(stop_rule(2.0));
        const auto none = covered.evaluate(held(10, 10000.0), price_of(9800.0), sell_pending_of(10), start);
        check(none.empty(), "미체결 매도가 보유를 덮으면 내지 않는다");
        check(covered.statistics().skipped_sold == 1, "낼 것이 없던 판정을 따로 센다");
    }

    // ── 5. 재발주 간격 — 같은 청산을 매 주기 다시 내지 않는다 ────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        book.set_retry_interval(std::chrono::milliseconds(30000));
        book.arm(stop_rule(2.0));
        check(book.evaluate(held(10, 10000.0), price_of(9800.0), sell_pending_of(0), start).size() == 1, "첫 주기에 낸다");
        check(book.evaluate(held(10, 10000.0), price_of(9800.0), sell_pending_of(0), start + std::chrono::seconds(5)).empty(),
              "5초 뒤에는 다시 내지 않는다");
        check(book.evaluate(held(10, 10000.0), price_of(9800.0), sell_pending_of(0), start + std::chrono::seconds(31)).size() == 1,
              "간격이 지나면 다시 낸다(청산이 안 먹힌 경우)");
    }

    // ── 6. 트레일 — 무장 전에는 안 팔고, 무장 뒤 최고가에서 밀리면 판다 ──────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        risk::ProtectiveRule rule = stop_rule(0.0);
        rule.trail_arm_percent    = 1.0; // 평단 +1%에 닿으면 무장
        rule.trail_percent        = 0.5; // 무장 뒤 최고가 -0.5%면 청산
        book.arm(rule);

        const auto held_ten = held(10, 10000.0);
        check(book.evaluate(held_ten, price_of(10050.0), sell_pending_of(0), start).empty(), "무장선(+1%) 전에는 팔지 않는다");
        check(book.evaluate(held_ten, price_of(10000.0), sell_pending_of(0), start).empty(),
              "무장 전에 평단으로 돌아와도 트레일은 안 걸린다");
        check(book.evaluate(held_ten, price_of(10200.0), sell_pending_of(0), start).empty(), "무장은 됐지만 최고가라 안 판다");
        check(book.evaluate(held_ten, price_of(10160.0), sell_pending_of(0), start).empty(), "최고가 -0.39%는 아직 아니다");

        const auto orders = book.evaluate(held_ten, price_of(10140.0), sell_pending_of(0), start);
        check(orders.size() == 1, "최고가 10200의 -0.5%(10149) 아래로 밀리면 판다");
        check(orders[0].reason.find("보호주문:트레일") == 0, "사유에 트레일이 적힌다");
    }

    // ── 7. 보유가 0이 되면 최고가 기억을 비운다 ──────────────────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        risk::ProtectiveRule rule = stop_rule(0.0);
        rule.trail_arm_percent    = 1.0;
        rule.trail_percent        = 0.5;
        book.arm(rule);

        book.evaluate(held(10, 10000.0), price_of(10200.0), sell_pending_of(0), start); // 무장 + 최고가 10200
        book.evaluate({}, price_of(10200.0), sell_pending_of(0), start);                 // 전량 청산돼 보유 0

        // 새 평단 10100으로 재진입. 옛 최고가(10200)가 남아 있으면 이 가격에서 바로 트레일이 걸린다.
        const auto orders = book.evaluate(held(10, 10100.0), price_of(10100.0), sell_pending_of(0), start);
        check(orders.empty(), "재진입 직후 옛 최고가로 오발주하지 않는다");
    }

    // ── 8. 발사 사실은 전략이 한 번만 가져간다 ───────────────────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        book.arm(stop_rule(2.0));
        check(!book.consume_fired(kAccount, kSymbol), "발사 전에는 가져갈 것이 없다");
        book.evaluate(held(10, 10000.0), price_of(9800.0), sell_pending_of(0), start);
        check(book.consume_fired(kAccount, kSymbol), "발사 뒤 한 번은 가져간다");
        check(!book.consume_fired(kAccount, kSymbol), "같은 발사를 두 번 가져가지 않는다");
    }

    // ── 9. disarm하면 표에서 빠진다 ──────────────────────────────────────────
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        book.arm(stop_rule(2.0));
        check(book.size() == 1, "등록하면 표에 한 줄 선다");
        book.disarm(kAccount, kSymbol);
        check(book.size() == 0, "해제하면 표에서 빠진다");
        check(book.evaluate(held(10, 10000.0), price_of(9000.0), sell_pending_of(0), start).empty(), "해제 뒤에는 팔지 않는다");

        risk::ProtectiveRule empty_rule = stop_rule(0.0); // 조건이 하나도 없는 규칙은 등록이 아니라 해제다
        book.arm(empty_rule);
        check(book.size() == 0, "조건 없는 규칙은 등록하지 않는다");
    }

    // ── 10. 리플레이 — 가격 경로를 끝까지 흘려도 청산은 한 번이다 ────────────
    //    전략 코드가 한 줄도 안 도는 상태에서, 체결이 나기 전까지 보유가 그대로인 경우를 그린다.
    {
        risk::ProtectiveOrderBook book(risk::ProtectiveMode::Owner);
        book.set_retry_interval(std::chrono::milliseconds(30000));
        book.arm(stop_rule(2.0));

        const double prices[] = {10000.0, 10050.0, 9990.0, 9950.0, 9900.0, 9850.0, 9810.0, 9790.0, 9750.0, 9700.0};
        int          emitted  = 0;
        auto         now      = start;

        for (const double price : prices)
        {
            // 한 주기 200ms — 엔진 주문 쪽이 표를 보는 간격과 같다.
            now += std::chrono::milliseconds(200);
            emitted += static_cast<int>(book.evaluate(held(10, 10000.0), price_of(price), sell_pending_of(0), now).size());
        }

        check(emitted == 1, "손절선을 지난 뒤 주기가 여러 번 돌아도 청산은 한 번만 나간다");
        check(book.statistics().fired == 1, "발사 수도 한 번이다");
    }

    std::cout << "test_protective_orders: " << g_checks << " checks passed\n";
    return 0;
}
