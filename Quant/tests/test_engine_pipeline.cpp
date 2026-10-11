// test_engine — 수신 스레드×샤드 한 바퀴와 다종목 전략 하나의 샤드 묶음.
//  공용 준비는 test_engine_support.h, 실행 순서는 test_engine.cpp의 main.
#include "test_engine_support.h"

namespace engine_test
{

// 다종목 전략 하나 + 샤드 여럿. 샤드 수가 config 그대로 서고(1 폴백 없음), 종목 전부의 마스크가 그 전략의 샤드 하나이며,
//  종목 수만큼 주문·보유가 잡히면 통과.
int run_spanning_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers)
{
    using namespace std::chrono_literals;
    std::cout << "case spanning lanes=" << lanes << " shards=" << shards << " tickers=" << tickers.size() << "\n";

    auto  feed_owned = std::make_unique<FakeFeed>(lanes);
    auto* feed       = feed_owned.get();
    auto  strategy_owned = std::make_unique<BuyEachOnce>(tickers);
    auto* strategy       = strategy_owned.get();

    Engine engine(KisConfig{});
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트의 합성 주문·체결을 물어 실거래 DB에 넣는다(09-22 실제로 났다).
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(shards);
    engine.add_strategy(std::move(strategy_owned));
    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
    engine.start();

    CHECK(engine.is_running());
    CHECK(engine.shard_count() == shards);
    CHECK(strategy->shard_index() < shards);

    for (const auto symbol_id : strategy->symbol_ids())
    {
        CHECK(engine.route_mask(symbol_id) == shard::mask_of(strategy->shard_index()));
    }

    for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
    {
        feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70000.0, 93001);
    }

    std::vector<OrderGate::HeldPos> held;
    const auto                      deadline = std::chrono::steady_clock::now() + 8s;

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
        {
            feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70100.0, 93002);
        }

        held = engine.held_positions();

        if (held.size() >= tickers.size())
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(held.size() == tickers.size());
    CHECK(engine.signal_count() == tickers.size());

    // 한 프로세스로 돌면 시세는 통로를 지나지 않는다 — 수신 스레드가 곧바로 샤드에 넣는다. [why D-114]
    for (uint32_t lane = 0; lane < lanes; ++lane)
    {
        CHECK(engine.feed_channel_pending_trades(lane) == 0);
    }

    engine.stop();
    CHECK(!engine.is_running());
    return 0;
}

// 종목 i는 수신 스레드 i % lanes에서 들어온다. 종목마다 전략 하나. 종목 수만큼 주문·체결·보유가 잡히면 통과.
int run_case(uint32_t lanes, uint32_t shards, const std::vector<std::string>& tickers)
{
    using namespace std::chrono_literals;
    std::cout << "case lanes=" << lanes << " shards=" << shards << " tickers=" << tickers.size() << "\n";

    auto  feed_owned = std::make_unique<FakeFeed>(lanes);
    auto* feed       = feed_owned.get();
    std::vector<BuyOnce*> strategies;

    Engine engine(KisConfig{});
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트의 합성 주문·체결을 물어 실거래 DB에 넣는다(09-22 실제로 났다).
    engine.set_zmq_enabled(false);
    engine.set_strategy_shards(shards);

    for (const auto& ticker : tickers)
    {
        auto stop_token = std::make_unique<BuyOnce>(ticker);
        strategies.push_back(stop_token.get());
        engine.add_strategy(std::move(stop_token));
    }

    engine.set_feed_source(std::move(feed_owned), 1'000'000.0);
    engine.start();

    // 1. 브로커 없이 떴고, 소스는 전략들의 구독 종목으로 연결됐고, 행렬은 요청한 수신 스레드×샤드 그대로다(샤드 1 폴백 없음).
    CHECK(engine.is_running());
    CHECK(feed->is_connected());
    CHECK(feed->specifications().size() == tickers.size());
    CHECK(engine.websocket_lanes() == lanes);
    CHECK(engine.shard_count() == shards);

    // 2. 전략들이 실제로 서로 다른 샤드에 배정됐고(등록 순 라운드로빈), 각 종목 틱은 그 전략의 샤드로만 간다 —
    //  같은 샤드면 N×M을 시험한 것이 아니다. [why D-110]
    {
        std::set<uint32_t> cols;

        for (auto* stop_token : strategies)
        {
            cols.insert(stop_token->shard_index());
            CHECK(engine.route_mask(stop_token->symbol_id()) == shard::mask_of(stop_token->shard_index()));
        }

        CHECK(cols.size() == std::min<size_t>(shards, tickers.size()));
    }

    // 3. 첫 틱이 각 전략까지 닿고 매수 신호가 주문 큐로 간다. 접수는 모의 체결기 — 체결은 다음 틱에서.
    for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
    {
        feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70000.0, 93001);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;

    while (engine.order_count() < tickers.size() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }

    CHECK(engine.order_count() == tickers.size());

    for (auto* stop_token : strategies)
    {
        CHECK(stop_token->ticks_seen.load() >= 1);
    }

    // 4. 다음 틱이 시장가를 그 가격에 체결시키고 체결 소비 스레드가 원장에 반영한다 — 종목마다 보유 1주, 평단 70100.
    std::vector<OrderGate::HeldPos> held;

    for (int index = 0; index < 400; ++index)
    {
        for (size_t ticker_index = 0; ticker_index < tickers.size(); ++ticker_index)
        {
            feed->emit_trade(static_cast<uint32_t>(ticker_index) % lanes, tickers[ticker_index], 70100.0, 93002 + index);
        }

        held = engine.held_positions();

        if (held.size() >= tickers.size())
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(held.size() == tickers.size());

    for (const auto& holding : held)
    {
        CHECK(std::find(tickers.begin(), tickers.end(), holding.ticker) != tickers.end());
        CHECK(holding.quantity == 1);
        CHECK(holding.average_price > 70099.0 && holding.average_price < 70101.0);
    }

    CHECK(engine.signal_count() == tickers.size());

    // 5. 정지가 장 외 대기(60초)를 기다리지 않는다.
    const auto start_time = std::chrono::steady_clock::now();
    engine.stop();
    CHECK(!engine.is_running());
    CHECK(std::chrono::steady_clock::now() - start_time < 10s);
    return 0;
}

} // namespace engine_test
