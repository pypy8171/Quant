// test_order_router 공용 준비의 몸통 — 스텁의 응답 흉내와 시험 도우미.
#include "test_order_router_support.h"

// ─── KIS 응답 시뮬레이션 Stub ─────────────────────────────────────────────────
StubOrderExecutor::StubOrderExecutor(bool flag, std::string output)
    : succeed(flag)
{
    kis_order_no = std::move(output);
    orgno        = std::string("ORG000001");
}

bool StubOrderExecutor::is_paper() const noexcept
{
    return paper;
}

KisResult<std::vector<OpenOrder>> StubOrderExecutor::get_open_orders()
{
    ++open_order_calls;

    if (open_orders_fail)
    {
        return kis_fail("EGW00201", "초당 거래건수를 초과하였습니다");
    }

    return open_orders.copy();
}

KisResult<std::vector<DailyOrderFill>> StubOrderExecutor::get_daily_order_fills()
{
    ++daily_fill_calls;

    if (daily_fills_fail)
    {
        return kis_fail("EGW00201", "초당 거래건수를 초과하였습니다");
    }

    return daily_fills.copy();
}

OrderAck StubOrderExecutor::submit_order_acknowledgement(const OrderSignal&)
{
    ++call_count;

    // 남은 실패 수를 비교·교환 한 번으로 줄인다 — 두 스레드가 같이 줄여 음수가 되지 않게 한다.
    int remaining_failures = fail_next.load();

    while (remaining_failures > 0 && !fail_next.compare_exchange_weak(remaining_failures, remaining_failures - 1))
    {
    }

    if (remaining_failures > 0)
    {
        const std::string code = error_code.copy();
        return OrderAck::fail(code.empty() ? std::string("E_TEST") : code);
    }

    return succeed ? OrderAck{kis_order_no.copy(), orgno.copy(), std::string()} : OrderAck::fail("E_TEST");
}

OrderAck StubOrderExecutor::cancel_order(const std::string&, const std::string&, const std::string&,
                                         int quantity, bool)
{
    ++cancel_calls;
    last_cancel_quantity = quantity;
    return cancel_ok ? OrderAck{"C000000001", std::string(), std::string()} : OrderAck::fail("E_TEST");
}

OrderAck StubOrderExecutor::revise_order(const std::string&, const std::string&, const std::string&,
                                         int, double)
{
    ++revise_calls;
    return revise_ok ? OrderAck{"0000000001", std::string(), std::string()} : OrderAck::fail("E_TEST");
}

// ─── 헬퍼 ─────────────────────────────────────────────────────────────────────
OrderSignal make_signal(const std::string& ticker, OrderSide side, int quantity)
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.quantity    = quantity;
    signal.price       = 75000.0;
    signal.strategy_id = "TEST";
    signal.market      = Market::KR;
    return signal;
}

OrderGate::Config relaxed_config()
{
    OrderGate::Config config;
    config.max_orders_per_min = 100;
    config.max_orders_per_sec = 100;
    config.deduplicate_window_sec   = 0.0;
    return config;
}

// 오늘 원장 CSV의 마지막 n줄(헤더 제외). 행 검증용.
std::vector<std::string> tail_trade_rows(size_t count)
{
    std::time_t now_time = std::time(nullptr);
    std::tm     local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif
    char buffer[9];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &local_time);
    std::ifstream in(Logger::instance().path_for(std::string("trades_") + buffer + ".csv"));
    std::vector<std::string> rows;

    for (std::string line; std::getline(in, line); )
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (!line.empty() && line.rfind("ts_kst,", 0) != 0)
        {
            rows.push_back(line);
        }
    }

    if (rows.size() > count)
    {
        rows.erase(rows.begin(), rows.end() - static_cast<long>(count));
    }

    return rows;
}

std::vector<std::string> split_csv(const std::string& line)
{
    std::vector<std::string> out;
    std::string current;

    for (char character : line)
    {
        if (character == ',')
        {
            out.push_back(current);
            current.clear();
        }
        else
        {
            current += character;
        }
    }

    out.push_back(current);
    return out;
}

void PASS(const std::string& name)
{
    std::cout << "[PASS] " << name << "\n";
}

OpenOrder make_open_order(const std::string& ticker, OrderSide side, int quantity, double price,
                          const std::string& kis_order_no)
{
    OpenOrder open;
    open.ticker                = ticker;
    open.side                  = side;
    open.psbl_qty              = quantity;
    open.ord_unpr              = price;
    open.kis_order_no          = kis_order_no;
    open.krx_forwarding_org_no = "ORG000777";
    return open;
}

// INTENT 하나를 선점과 함께 세운다 — 리플레이가 되쌓는 모양과 같게.
OrderGate::OpenIntent reserve_intent(OrderGate& gate, uint64_t order_id, uint64_t kis_order_number, int quantity)
{
    const OrderGate::OrderRef reference{order_id, kis_order_number, OrderType::LIMIT};
    const bool                written = gate.ledger().on_intent("", "005930", OrderSide::BUY, quantity, 75000.0, reference);
    assert(written);

    OrderGate::OpenIntent intent;
    intent.order_id         = order_id;
    intent.kis_order_number = kis_order_number;
    intent.ticker           = "005930";
    intent.strategy_name    = "DEVSCALE";
    intent.side             = OrderSide::BUY;
    intent.type             = OrderType::LIMIT;
    intent.remaining        = quantity;
    intent.price            = 75000.0;
    intent.accepted         = kis_order_number != 0;
    return intent;
}
