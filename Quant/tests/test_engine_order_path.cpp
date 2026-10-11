// test_engine — 늦은 진입 버리기·보호 주문 한 주기 독점·틱 캡처 리플레이.
//  공용 준비는 test_engine_support.h, 실행 순서는 test_engine.cpp의 main.
#include "test_engine_support.h"

namespace engine_test
{

// 캡처 파일을 틀어 같은 한 바퀴를 돈다 — set_replay만으로 KIS 없이 뜨는지. 첫 틱 70000이 매수 신호를 내고 뒤따르는
//  70100 틱 중 하나가 시장가를 체결시킨다. 캡처 간격 20ms·speed 1이라 주문이 큐를 지나는 사이에도 틱이 계속 온다.
// 보호 주문 한 주기는 한 스레드만 잡는다. 평소 주인은 전략 스레드고 전략이 죽으면 주문 스레드가 이어받는데,
//  멈췄던 전략이 깨어나면 둘 다 같은 주기를 보게 된다 — 둘 다 잡으면 같은 청산이 두 번 나간다(A등급).
//  스레드 넷이 같은 시각으로 한꺼번에 달려들어도 참이 하나뿐인지 본다. [why D-114]
// ─── 큐에서 오래 기다린 신규 매수만 버린다 (D-127) ──────────────────────────
int run_stale_entry_case()
{
    const int64_t emitted_ns = 1'000'000'000;                        // 신호를 만든 시각
    const int64_t fresh_ns   = emitted_ns + kOrderSignalMaxAgeNs / 2; // 문턱의 절반만 기다렸다
    const int64_t stale_ns   = emitted_ns + kOrderSignalMaxAgeNs + 1; // 문턱을 막 넘겼다

    OrderSignal buy;
    buy.action       = OrderAction::NEW;
    buy.side         = OrderSide::BUY;
    buy.signal_at_ns = emitted_ns;

    CHECK(!is_stale_entry(buy, fresh_ns));
    CHECK(is_stale_entry(buy, stale_ns));

    // 매도는 손절·청산이라 늦어도 보낸다.
    OrderSignal sell = buy;
    sell.side        = OrderSide::SELL;
    CHECK(!is_stale_entry(sell, stale_ns));

    // 취소·정정도 늦어도 보낸다 — 원주문이 살아 있다.
    OrderSignal cancel = buy;
    cancel.action      = OrderAction::CANCEL;
    CHECK(!is_stale_entry(cancel, stale_ns));

    OrderSignal replace = buy;
    replace.action      = OrderAction::REPLACE;
    CHECK(!is_stale_entry(replace, stale_ns));

    // 시각을 안 찍은 신호는 나이를 모르니 버리지 않는다.
    OrderSignal unstamped = buy;
    unstamped.signal_at_ns = 0;
    CHECK(!is_stale_entry(unstamped, stale_ns));

    // 바스켓 계획 매수처럼 나이 제한 면제 표시가 켜진 매수는 2초를 기다려도 버리지 않는다 — 10-02 모의는 모의
    //  서버 왕복 4.4–7.4초에 바스켓 29건 중 19건이 버려졌다. 표시 없는 매수는 같은 나이에 버린다. [why D-155]
    const int64_t two_seconds_ns = emitted_ns + 2 * kOrderSignalMaxAgeNs;
    OrderSignal exempt_buy = buy;
    exempt_buy.exempt_from_age_limit = true;
    CHECK(!is_stale_entry(exempt_buy, two_seconds_ns));
    CHECK(is_stale_entry(buy, two_seconds_ns));
    CHECK(!is_stale_entry(sell, two_seconds_ns));
    CHECK(!is_stale_parked_entry(exempt_buy, emitted_ns, emitted_ns + kParkedOrderMaxWaitNs + 1));

    // 면제 표시는 프로세스를 갈라 띄워도 주문 요청 레코드를 지나 그대로 온다.
    exempt_buy.sequence = 1;
    CHECK(ipc::to_signal(ipc::to_request(exempt_buy), "BASKET").exempt_from_age_limit);
    CHECK(!ipc::to_signal(ipc::to_request(buy), "BASKET").exempt_from_age_limit);

    // 같은 종목 앞 주문(취소) 답을 기다린 시간은 1초 기준으로 재지 않는다 — 10-01 실계좌 082270은 취소 왕복
    //  1.1초 뒤 다시 까는 BUY 32가 버려졌다. 큐에서 꺼낸 시각부터 10초를 넘길 때만 버린다. [why D-151]
    const int64_t parked_at_ns = emitted_ns + kOrderSignalMaxAgeNs / 2;
    CHECK(!is_stale_parked_entry(buy, parked_at_ns, parked_at_ns + 1'100'000'000));
    CHECK(is_stale_parked_entry(buy, parked_at_ns, parked_at_ns + kParkedOrderMaxWaitNs + 1));
    CHECK(!is_stale_parked_entry(sell, parked_at_ns, parked_at_ns + kParkedOrderMaxWaitNs + 1));
    CHECK(!is_stale_parked_entry(cancel, parked_at_ns, parked_at_ns + kParkedOrderMaxWaitNs + 1));
    CHECK(!is_stale_parked_entry(buy, 0, parked_at_ns + kParkedOrderMaxWaitNs + 1));

    // 줄에서 꺼낸 주문은 같은 종목이 줄에 더 있어도 다시 세우지 않는다 — 10-01 모의 306200은 취소·매도 두 건이
    //  서로 뒤로 미루며 주문 스레드를 5시간 묶었다. 같은 종목이 답을 기다리는 중이면 어디서 왔든 세운다.
    CHECK(!should_park(false, true, true));
    CHECK(should_park(false, true, false));
    CHECK(should_park(true, false, true));
    CHECK(should_park(true, true, false));
    CHECK(!should_park(false, false, false));

    std::cout << "  stale_entry OK" << std::endl;
    return 0;
}

int run_protective_claim_case()
{
    std::cout << "case protective claim\n";

    Engine engine(KisConfig{});
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트를 물어 실거래 DB에 넣는다.
    engine.set_zmq_enabled(false);

    constexpr int  kThreads    = 4;
    constexpr int  kAttempts   = 2000;
    const auto     fixed_now   = std::chrono::steady_clock::now();
    std::atomic<int> claimed{0};
    std::atomic<int> ready{0};

    {
        std::vector<std::thread> racers;

        for (int index = 0; index < kThreads; ++index)
        {
            racers.emplace_back(
                [&]
                {
                    // 넷이 같은 자리에서 출발해야 경합이 실제로 겹친다.
                    ready.fetch_add(1);

                    while (ready.load() < kThreads)
                    {
                        std::this_thread::yield();
                    }

                    for (int attempt = 0; attempt < kAttempts; ++attempt)
                    {
                        if (engine.claim_protective_cycle(fixed_now))
                        {
                            claimed.fetch_add(1);
                        }
                    }
                });
        }

        for (auto& racer : racers)
        {
            racer.join();
        }
    }

    // 같은 시각으로 8,000번 달려들어도 참은 딱 한 번이다.
    CHECK(claimed.load() == 1);

    // 간격(기본 200ms)이 아직 안 찼으면 계속 거짓이다.
    CHECK(!engine.claim_protective_cycle(fixed_now + std::chrono::milliseconds(199)));

    // 간격이 차면 다시 한 번만 참이다.
    const auto next_now = fixed_now + std::chrono::milliseconds(200);
    CHECK(engine.claim_protective_cycle(next_now));
    CHECK(!engine.claim_protective_cycle(next_now));

    std::cout << "  보호 주기 잡기 OK (경합 " << (kThreads * kAttempts) << "회에 참 1회)\n";
    return 0;
}

int run_replay_case()
{
    using namespace std::chrono_literals;
    std::cout << "case replay\n";
    const auto path = std::filesystem::temp_directory_path() / "quant_test_engine_replay.bin";
    std::filesystem::remove(path);
    {
        feed::TickCapture capture(path);
        CHECK(capture.ok());

        for (int index = 0; index < 100; ++index)
        {
            TradeData trade;
            trade.ticker.assign("005930");
            trade.price     = index == 0 ? 70000.0 : 70100.0;
            trade.quantity  = 10;
            trade.direction = 1;
            trade.hhmmss    = 93001 + index;
            trade.received_ns   = 1'000'000'000LL + static_cast<int64_t>(index) * 20'000'000LL;
            capture.on_trade(trade);
        }

        capture.flush();
        CHECK(capture.written() == 100);
    }

    Engine engine(KisConfig{});
    // 발행 채널을 열지 않는다 — 열면 운영 리코더가 이 테스트의 합성 주문·체결을 물어 실거래 DB에 넣는다(09-22 실제로 났다).
    engine.set_zmq_enabled(false);
    auto   st_owned = std::make_unique<BuyOnce>("005930");
    auto*  stop_token       = st_owned.get();
    engine.add_strategy(std::move(st_owned));
    engine.set_replay(path.string(), 1.0, 1'000'000.0);
    engine.start();

    // 1. KIS 없이 떴고 수신 스레드은 하나다.
    CHECK(engine.is_running());
    CHECK(engine.websocket_lanes() == 1);

    // 2. 캡처가 흐르는 동안 주문 1건이 접수되고 다음 틱에 체결돼 보유 1주·평단 70100이 잡힌다.
    std::vector<OrderGate::HeldPos> held;
    const auto                      deadline = std::chrono::steady_clock::now() + 8s;

    while (std::chrono::steady_clock::now() < deadline)
    {
        held = engine.held_positions();

        // 체결은 수신 스레드의 모의 체결기가 내고 틱은 전략 스레드가 따로 본다 — 보유가 먼저 잡혀도 전략이
        //  둘째 틱을 아직 못 봤을 수 있어 둘 다 기다린다(병렬 ctest에서 5회 중 1회 어긋났다).
        if (!held.empty() && stop_token->ticks_seen.load() >= 2)
        {
            break;
        }

        std::this_thread::sleep_for(10ms);
    }

    CHECK(engine.order_count() == 1);
    CHECK(stop_token->ticks_seen.load() >= 2);
    CHECK(held.size() == 1);

    if (held.size() == 1)
    {
        CHECK(held[0].ticker == "005930");
        CHECK(held[0].quantity == 1);
        CHECK(held[0].average_price > 70099.0 && held[0].average_price < 70101.0);
    }

    CHECK(engine.signal_count() == 1);
    engine.stop();
    CHECK(!engine.is_running());
    std::filesystem::remove(path);
    return 0;
}

} // namespace engine_test
