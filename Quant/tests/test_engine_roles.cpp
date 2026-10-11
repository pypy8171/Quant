// test_engine — 역할별 종목 번호·주문 스위치·수동 주문·역할대로 스레드 띄우기.
//  공용 준비는 test_engine_support.h, 실행 순서는 test_engine.cpp의 main.
#include "test_engine_support.h"

namespace engine_test
{

// D-114 단계 4 — 티커가 종목 번호가 되는 자리를 둘로 가른 것을 고정한다. 넣는 쪽(register_symbol)은
//  주문 프로세스 하나고, 잦은 자리(lookup_symbol)는 표에 없는 티커를 만들지 않는다.
int run_symbol_role_case()
{
    using namespace std::chrono_literals;
    std::cout << "case symbol role\n";

    // 1. 역할을 주기 전은 지금까지와 같다 — 넣는 쪽이라 그 자리에서 번호가 나고, 같은 티커는 같은 번호다.
    //  여기서는 스레드를 띄우지 않는다. 역할은 뜨기 전에 정해지고 뜬 뒤에는 바꾸지 않는다 — 어느 스레드를
    //  띄울지가 그 값으로 갈리고, 돌고 있는 스레드가 같은 값을 읽는다. [why D-114 단계 5]
    {
        Engine engine(KisConfig{});
        engine.set_zmq_enabled(false);
        engine.set_strategy_shards(1);
        engine.add_strategy(std::make_unique<BuyOnce>("005930"));
        engine.set_feed_source(std::make_unique<FakeFeed>(1), 1'000'000.0);

        const symbol::SymbolId samsung = engine.register_symbol("005930");
        CHECK(samsung != symbol::kNone);
        CHECK(engine.register_symbol("005930") == samsung);

        // 2. 잦은 자리는 없는 티커를 만들지 않는다 — kNone을 주고 센다(등록 경로가 빠진 것을 드러낸다).
        CHECK(engine.lookup_symbol("000660") == symbol::kNone);
        CHECK(engine.symbol_lookup_misses() == 1);
        CHECK(engine.lookup_symbol("000660") == symbol::kNone);
        CHECK(engine.symbol_lookup_misses() == 2);
    }

    // 3. 전략 역할은 표에 직접 넣지 않는다 — 제어 요청을 보내고, 주문 쪽이 넣은 번호를 같은 표에서 읽어
    //  온다. 답할 쪽이 있어야 하니 주문 역할을 따로 띄운다.
    Engine order_engine(KisConfig{});
    order_engine.set_zmq_enabled(false);
    order_engine.set_strategy_shards(1);
    order_engine.set_feed_source(std::make_unique<FakeFeed>(1), 1'000'000.0);
    order_engine.set_role(ProcessRole::Order);
    order_engine.start();
    CHECK(order_engine.is_running());

    Engine strategy_engine(KisConfig{});
    strategy_engine.set_zmq_enabled(false);
    strategy_engine.set_strategy_shards(1);
    strategy_engine.set_feed_source(std::make_unique<FakeFeed>(1), 1'000'000.0);
    strategy_engine.set_role(ProcessRole::Strategy);
    strategy_engine.start();
    CHECK(strategy_engine.is_running());

    const symbol::SymbolId samsung = order_engine.register_symbol("005930");
    CHECK(samsung != symbol::kNone);

    const symbol::SymbolId hynix = strategy_engine.register_symbol("000660");
    CHECK(hynix != symbol::kNone);
    CHECK(hynix != samsung);
    CHECK(strategy_engine.lookup_symbol("000660") == hynix);
    CHECK(strategy_engine.symbol_register_timeouts() == 0);

    // 4. 이미 표에 있으면 요청을 보내지 않고 바로 답한다.
    CHECK(strategy_engine.register_symbol("000660") == hynix);
    CHECK(strategy_engine.symbol_register_timeouts() == 0);

    // 5. 집어 갈 쪽이 멎으면 번호가 안 뜬다 — 기다리다 kNone을 주고 세고, 표에도 안 들어간다.
    //  부른 쪽이 그 줄을 접게 하려는 것이다(전략 쪽이 제 번호를 찍어 버리는 쪽이 훨씬 나쁘다).
    order_engine.stop();
    CHECK(!order_engine.is_running());

    CHECK(strategy_engine.register_symbol("373220") == symbol::kNone);
    CHECK(strategy_engine.symbol_register_timeouts() == 1);
    CHECK(strategy_engine.lookup_symbol("373220") == symbol::kNone);

    strategy_engine.stop();
    CHECK(!strategy_engine.is_running());

    return 0;
}

// D-114 단계 4 — 주문 쪽 스위치를 고치는 자리가 주문 스레드 하나인 것을 고정한다. Engine::request_* 는
//  제어 요청 한 줄로 바뀌고, 값은 주문 스레드가 그 줄을 집은 뒤에 바뀐다. Both 로 돌 때도 같은 통로다 —
//  전략 프로세스 안 여러 생산자가 앞 토막에 모이고, 전략 스레드가 경계 너머 제어 면으로 옮긴다.
int run_switch_role_case()
{
    using namespace std::chrono_literals;
    std::cout << "case switch role\n";

    auto feed_owned = std::make_unique<FakeFeed>(1);

    Engine engine(KisConfig{});
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(1);
    engine.add_strategy(std::make_unique<BuyOnce>("005930"));
    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);

    // 1. 역할을 주기 전에도 통로를 탄다 — 부른 그 자리에서는 바뀌지 않는다. Both 만 질러가게 두면
    //  그 통로가 갈라 띄운 날 처음 돈다.
    engine.request_entry_halt(true);
    CHECK(!engine.is_entry_halted());

    engine.start();
    CHECK(engine.is_running());

    // 주문 스레드가 제어 큐를 집어 갈 때까지 짧게 본다. 값이 바뀌는 시점이 부른 자리가 아니라는 것이 요점이다.
    const auto settled = [](auto&& reached) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (reached())
            {
                return true;
            }

            std::this_thread::sleep_for(200us);
        }

        return false;
    };

    // 2. 뜨기 전에 넣은 줄도 잃지 않는다 — 전략 스레드가 옮기고 주문 스레드가 건다.
    CHECK(settled([&engine]
    {
        return engine.is_entry_halted();
    }));

    // 3. 신규 진입 정지 — 끄고 다시 켠다. 뜬 뒤에는 역할을 바꾸지 않는다(스레드 구성이 그 값으로
    //  갈린다) — 전략 역할에서 같은 통로를 타는 것은 run_split_start_case 가 실제 두 프로세스로 본다.
    engine.request_entry_halt(false);
    CHECK(settled([&engine]
    {
        return !engine.is_entry_halted();
    }));
    engine.request_entry_halt(true);
    CHECK(settled([&engine]
    {
        return engine.is_entry_halted();
    }));

    // 4. 매수 비율 — 스위치가 아니라 값이라 칸을 따로 둔다.
    constexpr double kScaleUnderTest = 0.4;
    engine.request_entry_scale(kScaleUnderTest);
    CHECK(settled([&engine]
    {
        return std::fabs(engine.entry_scale() - kScaleUnderTest) < 1e-9;
    }));

    // 5. 수동 정지 — 방향 칸이 같이 건너간다(매도만 걸고 매수는 그대로).
    engine.request_manual_halt(OrderSide::SELL, true);
    CHECK(settled([&engine]
    {
        return engine.is_manual_sell_halted();
    }));

    // 6. 전방향 차단 — 마지막에 건다.
    engine.request_kill_switch(true);
    CHECK(settled([&engine]
    {
        return engine.is_killed();
    }));

    engine.stop();
    CHECK(!engine.is_running());

    // 7. 집어 갈 쪽이 멎으면 값이 안 바뀐다 — 요청은 통로에 남고, 여기서 제 손으로 고치지 않는다.
    engine.request_entry_halt(false);
    CHECK(engine.is_entry_halted());

    return 0;
}

// D-114 단계 4 — 역할대로 제 몫만 띄운다. 전략 역할은 샤드·전략 스레드만, 주문 역할은 주문·체결 스레드만이다.
//  둘 다 브로커 없이 뜨고 멎는다. 여기서 드러나는 빈자리가 --role 을 아직 막아 둔 이유다 — 전략 쪽은 종목
//  번호를 주는 주문 쪽이 없어 못 받고, 주문 쪽은 구독 목록이 전략 쪽에 있어 소켓을 열 게 없다.
int run_manual_order_case()
{
    using namespace std::chrono_literals;
    std::cout << "case manual order\n";

    // 전략을 하나도 올리지 않는다 — 수동주문이 전략 쪽을 안 거치고 주문 스레드에서 나가는지만 본다. [why D-114]
    auto feed_owned = std::make_unique<FakeFeed>(1);

    Engine engine(KisConfig{});
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(1);
    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
    engine.set_role(ProcessRole::Order);
    engine.start();

    CHECK(engine.is_running());

    // 값이 틀린 것은 인테이크에서 되돌린다 — 단말이 그 자리에서 사유를 본다.
    OpsOrderReq bad_ticker;
    bad_ticker.client_id = "manual-bad-1";
    bad_ticker.ticker    = "삼성전자";
    bad_ticker.side      = "BUY";
    bad_ticker.quantity  = 1;
    CHECK(!engine.accept_manual_order(bad_ticker).empty());

    OpsOrderReq bad_side  = bad_ticker;
    bad_side.client_id    = "manual-bad-2";
    bad_side.ticker       = "005930";
    bad_side.side         = "LONG";
    CHECK(!engine.accept_manual_order(bad_side).empty());

    // 보유가 없는 매도는 꺼내는 자리(주문 스레드)에서 끊긴다 — 인테이크는 값만 보므로 여기선 통과다.
    OpsOrderReq sell_without_position = bad_side;
    sell_without_position.client_id   = "manual-sell-1";
    sell_without_position.side        = "SELL";
    CHECK(engine.accept_manual_order(sell_without_position).empty());

    OpsOrderReq buy;
    buy.client_id       = "manual-buy-1";
    buy.ticker          = "005930";
    buy.side            = "BUY";
    buy.quantity        = 1;
    buy.price           = 70000.0;
    buy.reference_price = 70000.0;
    CHECK(engine.accept_manual_order(buy).empty());

    // 같은 cid 재전송은 한 번만 받는다.
    CHECK(!engine.accept_manual_order(buy).empty());

    // 지정가라 체결은 시세가 닿아야 난다 — 여기서 보는 것은 주문이 나갔는가까지다.
    const auto deadline = std::chrono::steady_clock::now() + 8s;

    while (engine.order_count() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }

    CHECK(engine.order_count() == 1);   // 매수 한 건만 나갔다 — 보유 없는 매도는 꺼내는 자리에서 끊겼다
    CHECK(engine.signal_count() == 0);  // 전략 쪽 순번은 찍히지 않았다 — 통로를 안 지났다

    engine.stop();
    CHECK(!engine.is_running());
    return 0;
}

int run_split_start_case()
{
    using namespace std::chrono_literals;
    std::cout << "case split start\n";

    // 세 프로세스를 한 프로세스 안에서 흉내낸다 — 주문 역할이 공유 쪽지를 만들고, 시세·전략 역할이 같은
    //  이름으로 붙는다. 붙는 쪽이 늦게 뜨는 순서까지 그대로다. 단계 5부터 소켓은 시세가 쥔다 — 앱키 하나에
    //  실시간 세션 하나라 소켓도 하나뿐이고 체결통보가 같은 세션에 실린다. 가르면 셋이 다 뜬다. [why D-114 단계 5]
    auto  order_feed_owned = std::make_unique<FakeFeed>(1);
    auto* order_feed       = order_feed_owned.get();
    auto  order_strategy   = std::make_unique<BuyOnce>("005930");
    auto* order_side_view  = order_strategy.get();

    Engine order_engine(KisConfig{});
    order_engine.set_zmq_enabled(false);
    order_engine.set_strategy_shards(1);
    order_engine.add_strategy(std::move(order_strategy));
    order_engine.set_feed_source(std::move(order_feed_owned), 1'000'000.0);
    order_engine.set_role(ProcessRole::Order);
    order_engine.start();

    // 1. 주문 역할 — 전략을 올리지 않는다. on_start 를 부르지 않으니 종목 번호를 달라는 요청도 없다.
    //  소켓도 열지 않는다(단계 4까지는 여기가 열었다). [why D-114 단계 5]
    CHECK(order_engine.is_running());
    CHECK(order_side_view->symbol_id() == symbol::kNone); // 전략을 올리지 않았다
    CHECK(order_engine.symbol_register_timeouts() == 0);
    CHECK(!order_feed->is_connected());                   // 소켓은 시세 쪽이 쥔다
    CHECK(order_engine.order_count() == 0);
    CHECK(order_engine.feed_channel_lanes() == order_engine.websocket_lanes() + 1);
    CHECK(order_engine.fill_channel_overflows() == 0); // 체결 통로는 비어 있다
    CHECK(order_engine.fill_channel_discarded() == 0);

    // 2. 시세 역할 — 소켓을 쥐고, 꺼낸 시세를 샤드가 아니라 통로에 넣는다. 전략도 샤드도 저쪽 프로세스에 있다.
    auto  feed_side_owned = std::make_unique<FakeFeed>(1);
    auto* feed_side       = feed_side_owned.get();

    Engine feed_engine(KisConfig{});
    feed_engine.set_zmq_enabled(false);
    feed_engine.set_strategy_shards(1);
    feed_engine.set_feed_source(std::move(feed_side_owned), 1'000'000.0);
    feed_engine.set_role(ProcessRole::Feed);
    feed_engine.start();

    CHECK(feed_engine.is_running());
    CHECK(feed_side->is_connected());    // 구독 목록이 비어도 연다 — 체결통보를 이 소켓이 듣는다
    CHECK(feed_engine.order_count() == 0);
    CHECK(feed_engine.feed_channel_lanes() == feed_engine.websocket_lanes() + 1);

    // 번호를 다는 쪽은 주문 하나다 — 시세는 표에 없는 티커가 오면 버리고 센다. 여기서 청하면 제어 줄의
    //  보내는 쪽이 둘이 되어 한줄 큐가 깨진다. 아무도 청하지 않은 종목이 세션에 실려 온 자리다. [why D-114 단계 5]
    feed_side->emit_trade(0, "000660", 70000.0, 93001);
    CHECK(feed_engine.unknown_ticker_dropped() == 1);
    CHECK(feed_engine.feed_channel_pending_trades(0) == 0);
    CHECK(feed_engine.feed_channel_overflows() == 0);

    // 3. 전략 역할 — 전략은 올라가지만 주문·소켓은 이 프로세스에 없다. 자리표는 주문 쪽이 만든 쪽지에 붙는다.
    {
        auto  feed_owned     = std::make_unique<FakeFeed>(1);
        auto* feed           = feed_owned.get();
        auto  strategy_owned = std::make_unique<BuyOnce>("005930");
        auto* strategy       = strategy_owned.get();

        Engine engine(KisConfig{});
        engine.set_zmq_enabled(false);
        engine.set_strategy_shards(1);
        engine.add_strategy(std::move(strategy_owned));
        engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
        engine.set_role(ProcessRole::Strategy);
        engine.start();

        CHECK(engine.is_running());
        CHECK(engine.shard_count() == 1);              // 틱 파이프라인 자리는 전략 쪽에 남는다
        CHECK(!feed->is_connected());                  // 시세 소켓은 시세 쪽이 쥔다
        CHECK(engine.order_count() == 0);              // 주문 스레드가 없다
        CHECK(engine.feed_channel_lanes() == engine.websocket_lanes() + 1); // 꺼내는 쪽도 같은 줄 수를 본다

        // 종목 표는 이제 한 장이다 — 전략이 on_start 에서 부탁한 번호를 주문 쪽이 그 표에 넣고, 전략은
        //  같은 표에서 읽는다. 셋이 같은 종목에 같은 번호를 본다. [why D-114]
        CHECK(engine.symbol_register_timeouts() == 0);
        const symbol::SymbolId shared_id = strategy->symbol_id();
        CHECK(shared_id != symbol::kNone);
        CHECK(order_engine.symbols().lookup("005930") == shared_id);

        // 전략이 보낸 구독 요청은 주문이 아니라 시세 쪽 제어 줄로 간다(ipc::routes_to_feed) — 소켓을 쥔
        //  쪽이 받아 소켓에 건다. 감시 스레드 바퀴가 5초라 그동안 기다린다. [why D-114 단계 5]
        WatchSpec expected;
        expected.ticker = "005930";

        const auto watch_deadline = std::chrono::steady_clock::now() + 15s;

        while (!feed_side->has_specification(expected) && std::chrono::steady_clock::now() < watch_deadline)
        {
            std::this_thread::sleep_for(50ms);
        }

        CHECK(feed_side->has_specification(expected));

        // 표에 번호가 생긴 뒤의 체결은 통로를 그대로 건넌다 — 번호가 한 표에서 나오니 꺼낸 쪽이 알아본다.
        feed_side->emit_trade(0, "005930", 70100.0, 93002);

        const auto deadline = std::chrono::steady_clock::now() + 5s;

        while (engine.data_count() == 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(10ms);
        }

        CHECK(engine.data_count() == 1);
        CHECK(engine.feed_channel_discarded() == 0);
        CHECK(feed_engine.unknown_ticker_dropped() == 1); // 더 늘지 않았다 — 표에서 번호를 찾았다

        // 전략 역할이 낸 스위치 요청은 제어 줄을 타고 주문 쪽에서 바뀐다 — 부른 자리에서는 안 바뀐다.
        //  값을 쥔 쪽이 주문 하나라는 것이 요점이다. [why D-114 단계 4]
        engine.request_entry_halt(true);
        CHECK(!engine.is_entry_halted());

        const auto halt_deadline = std::chrono::steady_clock::now() + 5s;

        while (!order_engine.is_entry_halted() && std::chrono::steady_clock::now() < halt_deadline)
        {
            std::this_thread::sleep_for(10ms);
        }

        CHECK(order_engine.is_entry_halted());

        engine.stop();
        CHECK(!engine.is_running());
    }

    feed_engine.stop();
    CHECK(!feed_engine.is_running());

    order_engine.stop();
    CHECK(!order_engine.is_running());

    return 0;
}

} // namespace engine_test
