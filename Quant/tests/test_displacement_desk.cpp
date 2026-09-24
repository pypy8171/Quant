// 교체 진입 창구(risk/DisplacementDesk.h) 단위 테스트 — 자리가 꽉 찬 책에 새 종목 매수가 왔을 때
//  최약체 매도를 앞세우고 그 매수를 들고 있다가 자리가 나면 내는 흐름을 고정한다.
//  전략 쪽(SignalDispatcher)에 있던 것을 주문 쪽으로 옮기면서 시험도 같이 옮겼다 — 최약체를 고르는 읽기와
//  자리를 예약하는 쓰기가 한 덩어리라 쪼갤 수 없다. 관련 결정: D-019(교체 진입), D-114(프로세스 분리).
// 빌드: cmake --build <directory> --target test_displacement_desk
#include "risk/DisplacementDesk.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                    \
    do                                                                                      \
    {                                                                                       \
        ++g_checks;                                                                         \
        if (!(condition))                                                                   \
        {                                                                                   \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n"; \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

using Clock = risk::DisplacementDesk::Clock;

// 슬롯 2개가 찬 책. 교체 진입은 켜고 보유 시간 조건은 끈다(test_order_gate와 같은 설정).
OrderGate::Config displace_config()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker  = 1000;
    config.max_orders_per_min       = 1000;
    config.max_orders_per_sec       = 1000;
    config.max_concurrent_positions = 2;
    config.displace_enabled         = true;
    config.displace_min_z_gap       = 0.5;
    config.displace_min_hold_sec    = 0;
    config.displace_slot_hold_sec   = 300;
    return config;
}

struct Rig
{
    OrderGate              gate;
    risk::DisplacementDesk desk;

    explicit Rig(OrderGate::Config config) : gate(config), desk(gate)
    {
        desk.set_label([](const std::string& ticker) { return "<" + ticker + ">"; });
    }

    // 슬롯 2개가 다 찬 책을 만든다.
    void seed_full_book()
    {
        gate.ledger().seed_position("", "A", 10, 1000.0);
        gate.ledger().seed_position("", "B", 10, 1000.0);
        gate.set_entry_priority({{gate.ledger().intern_symbol("A"), 1, 0.9},
                                 {gate.ledger().intern_symbol("B"), 2, -0.8},
                                 {gate.ledger().intern_symbol("C"), 3, 1.5}},
                                3);
    }

    // 창구는 종목 번호로 판단한다 — 신호를 내는 쪽(디스패처)이 이미 찍어 둔 것을 시험이 손으로 찍는다.
    OrderSignal signal(const char* ticker, OrderSide side, int quantity, OrderAction action = OrderAction::NEW)
    {
        OrderSignal made;
        made.ticker      = ticker;
        made.symbol_id   = gate.ledger().intern_symbol(ticker);
        made.side        = side;
        made.type        = OrderType::LIMIT;
        made.quantity    = quantity;
        made.price       = 1000.0;
        made.action      = action;
        made.strategy_id = "T";
        return made;
    }
};

int test_hold_and_release()
{
    Rig rig(displace_config());
    rig.seed_full_book();
    CHECK(rig.gate.capacity_full());

    // 꽉 찬 책에 C 매수 → 신호 자리에 최약체 B 전량 매도가 들어오고 C 매수는 창구가 든다.
    OrderSignal first = rig.signal("C", OrderSide::BUY, 1);
    CHECK(rig.desk.consider(first, Clock::now()) == risk::DisplacementDesk::Verdict::kSellFirst);
    CHECK(first.ticker == "B" && first.side == OrderSide::SELL && first.type == OrderType::MARKET &&
          first.quantity == 10 && first.reference_price == 1000.0 && first.strategy_id == "DISPLACE");
    CHECK(rig.desk.held_symbol() == rig.gate.ledger().intern_symbol("C") && rig.desk.held_count() == 1);

    // 같은 분할 매수의 다음 분할 단계도 보류에 붙는다. 다른 종목의 매도는 그대로 지나간다.
    OrderSignal second = rig.signal("C", OrderSide::BUY, 2);
    CHECK(rig.desk.consider(second, Clock::now()) == risk::DisplacementDesk::Verdict::kHold);
    OrderSignal sell_a = rig.signal("A", OrderSide::SELL, 1);
    CHECK(rig.desk.consider(sell_a, Clock::now()) == risk::DisplacementDesk::Verdict::kPass);
    CHECK(sell_a.ticker == "A" && rig.desk.held_count() == 2);

    // 자리가 안 났으면 꺼내 주지 않는다.
    OrderSignal taken;
    CHECK(!rig.desk.take_ready(taken));

    // B 매도 체결 → 자리 → 보류 매수 둘이 차례로 나온다.
    rig.gate.ledger().on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
    CHECK(!rig.gate.capacity_full());
    CHECK(rig.desk.take_ready(taken) && taken.ticker == "C" && taken.quantity == 1);
    CHECK(rig.desk.take_ready(taken) && taken.quantity == 2);
    CHECK(!rig.desk.take_ready(taken));

    // 시한까지는 예약 종목을 기억한다 — 보류 목록이 비어도 그렇다.
    CHECK(rig.desk.held_count() == 0 && rig.desk.held_symbol() == rig.gate.ledger().intern_symbol("C"));

    // 자리가 있는 책에서는 매수가 곧장 지나간다.
    OrderSignal third = rig.signal("C", OrderSide::BUY, 3);
    CHECK(rig.desk.consider(third, Clock::now()) == risk::DisplacementDesk::Verdict::kPass);
    CHECK(third.ticker == "C" && third.side == OrderSide::BUY);
    return 0;
}

int test_cancel_and_expiry()
{
    {
        Rig rig(displace_config());
        rig.seed_full_book();
        OrderSignal buy = rig.signal("C", OrderSide::BUY, 1);
        CHECK(rig.desk.consider(buy, Clock::now()) == risk::DisplacementDesk::Verdict::kSellFirst);
        CHECK(rig.desk.held_count() == 1);

        // 전략이 분할 매수를 다시 깐다 — 취소가 오면 들고 있던 분할 단계를 비운다(취소 자체는 지나간다).
        OrderSignal cancel = rig.signal("C", OrderSide::BUY, 0, OrderAction::CANCEL);
        CHECK(rig.desk.consider(cancel, Clock::now()) == risk::DisplacementDesk::Verdict::kPass);
        CHECK(rig.desk.held_count() == 0 && cancel.action == OrderAction::CANCEL);
    }

    {
        Rig rig(displace_config());
        rig.seed_full_book();
        OrderSignal buy = rig.signal("C", OrderSide::BUY, 1);
        CHECK(rig.desk.consider(buy, Clock::now()) == risk::DisplacementDesk::Verdict::kSellFirst);

        // 예약 시한이 지나면 버린다 — 자리가 났어도. 버린 것은 부른 쪽이 받아 그 순번에 답한다.
        rig.gate.ledger().on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
        std::vector<OrderSignal> expired;
        rig.desk.expire(Clock::now() + std::chrono::seconds(301), expired);
        CHECK(expired.size() == 1 && expired[0].ticker == "C" && expired[0].quantity == 1);
        CHECK(rig.desk.held_count() == 0 && rig.desk.held_symbol() == symbol::kNone);

        // 시한 전에는 버리지 않는다.
        Rig other(displace_config());
        other.seed_full_book();
        OrderSignal buy_other = other.signal("C", OrderSide::BUY, 1);
        CHECK(other.desk.consider(buy_other, Clock::now()) == risk::DisplacementDesk::Verdict::kSellFirst);
        expired.clear();
        other.desk.expire(Clock::now(), expired);
        CHECK(expired.empty() && other.desk.held_count() == 1);
    }

    {
        // 교체가 꺼져 있으면 꽉 찬 책이라도 매수는 그대로 지나간다(거부는 게이트 몫).
        auto config             = displace_config();
        config.displace_enabled = false;
        Rig rig(config);
        rig.seed_full_book();
        OrderSignal buy = rig.signal("C", OrderSide::BUY, 1);
        CHECK(rig.desk.consider(buy, Clock::now()) == risk::DisplacementDesk::Verdict::kPass);
        CHECK(buy.ticker == "C" && rig.desk.held_symbol() == symbol::kNone);
    }

    {
        // 이미 들고 있거나 선점이 걸린 종목은 교체를 일으키지 않는다 — 분할 매수 2회차가 남을 또 팔면 안 된다.
        Rig rig(displace_config());
        rig.seed_full_book();
        rig.gate.ledger().seed_position("", "C", 5, 1000.0);
        OrderSignal buy = rig.signal("C", OrderSide::BUY, 1);
        CHECK(rig.desk.consider(buy, Clock::now()) == risk::DisplacementDesk::Verdict::kPass);
        CHECK(buy.ticker == "C" && rig.desk.held_symbol() == symbol::kNone);
    }

    return 0;
}
} // namespace

int main()
{
    if (test_hold_and_release() || test_cancel_and_expiry())
    {
        return 1;
    }

    std::cout << "test_displacement_desk: " << g_checks << " checks passed\n";
    return 0;
}
