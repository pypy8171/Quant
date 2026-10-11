// test_order_gate 공용 준비의 몸통.
#include "test_order_gate_support.h"

OrderSignal make_signal(const std::string& ticker, OrderSide side, int quantity)
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.quantity    = quantity;
    signal.price       = 100000.0;
    signal.strategy_id = "TEST";
    signal.market      = Market::KR;
    return signal;
}

void PASS(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl; // 뒤 테스트가 assert로 죽어도 여기까지는 남게 flush
}

OrderGate::Config displace_config()
{
    OrderGate::Config config;
    config.max_concurrent_positions = 2;
    config.max_quantity_per_ticker       = 1000;
    config.max_orders_per_min       = 1000;
    config.max_orders_per_sec       = 1000;
    config.displace_enabled         = true;
    config.displace_min_z_gap       = 0.5;
    config.displace_min_hold_sec    = 0; // 테스트에선 보유시간 조건을 끈다(별도 케이스에서 검증)
    return config;
}
