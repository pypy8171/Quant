// 발주 조절기(core/OrderPacer.h) 단위 테스트. 보유 수량 조회를 std::function으로 대신해 Engine·OrderGate 없이
//  발주 간격, 거부 분류(유량 한도·청산 SELL·40240000·BUY 제외·횟수 소진), 재시도 만기 순서와 청산 완료 폐기를
//  고정한다. Logger만 링크한다. 관련 결정: C-2(청산 SELL 재시도), D-065(분리).
// 빌드: cmake --build <directory> --target test_order_pacer
#include "core/OrderPacer.h"
#include "risk/GateReasons.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <iostream>

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

using Clock = OrderPacer::Clock;
using std::chrono::milliseconds;
using pacing::Retry;

OrderSignal signal(const char* ticker, OrderSide side, int quantity, OrderAction action = OrderAction::NEW)
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.account_id  = "ACC";
    signal.side        = side;
    signal.type        = OrderType::MARKET;
    signal.quantity    = quantity;
    signal.action      = action;
    signal.strategy_id = "T";
    return signal;
}

const milliseconds kDelay(1200);

int test_classify()
{
    const auto sell = signal("A", OrderSide::SELL, 10);
    const auto buy  = signal("A", OrderSide::BUY, 10);
    const auto cancel  = signal("A", OrderSide::BUY, 0, OrderAction::CANCEL);

    // 접수·횟수 소진은 재시도 없음
    CHECK(pacing::classify(sell, 0, 3, OrderStatus::ACCEPTED, "", kDelay).kind == Retry::NONE);
    CHECK(pacing::classify(sell, 3, 3, OrderStatus::REJECTED, "x", kDelay).kind == Retry::NONE);

    // 유량 한도 — KIS EGW00201과 게이트 "Rate limit …" 둘 다, action 불문
    auto plan = pacing::classify(cancel, 0, 3, OrderStatus::REJECTED, "EGW00201 초당 거래건수 초과", kDelay);
    CHECK(plan.kind == Retry::RATE_LIMIT && plan.delay == kDelay);
    plan = pacing::classify(buy, 2, 3, OrderStatus::REJECTED, gate_reason::rate_limit(false, 5), kDelay);
    CHECK(plan.kind == Retry::RATE_LIMIT && plan.delay == kDelay);
    // 분당 한도는 20초로 물러난다. 게이트가 만드는 문장 그대로다(계약은 risk/GateReasons.h 한 곳)
    CHECK(gate_reason::rate_limit(true, 40) == "Rate limit 초과 (분당 40건)");
    plan = pacing::classify(buy, 0, 3, OrderStatus::REJECTED, gate_reason::rate_limit(true, 40), kDelay);
    CHECK(plan.kind == Retry::RATE_LIMIT && plan.delay == milliseconds(20000));
    // "Rate limit"은 문자열 머리에 있을 때만 게이트 거부다
    CHECK(pacing::classify(buy, 0, 3, OrderStatus::REJECTED, "기타 Rate limit", kDelay).kind == Retry::NONE);

    // 청산 SELL 거부는 되쏜다. 40240000은 지속성 조건이라 제외
    plan = pacing::classify(sell, 1, 3, OrderStatus::REJECTED, "APBK0013 모의투자 장종료", kDelay);
    CHECK(plan.kind == Retry::SELL_REJECTED && plan.delay == kDelay);
    CHECK(pacing::classify(sell, 0, 3, OrderStatus::REJECTED, "40240000 주문가능수량 없음", kDelay).kind ==
          Retry::NONE);
    // BUY·취소의 일반 거부는 되쏘지 않는다(빈-ODNO 중복주문 위험)
    CHECK(pacing::classify(buy, 0, 3, OrderStatus::REJECTED, "APBK0013", kDelay).kind == Retry::NONE);
    CHECK(pacing::classify(signal("A", OrderSide::SELL, 0, OrderAction::CANCEL), 0, 3, OrderStatus::REJECTED,
                           "APBK0013", kDelay)
              .kind == Retry::NONE);
    return 0;
}

int test_interval()
{
    const auto start_time = Clock::now();
    OrderPacer order_pacer({350, 3}, start_time);
    // 첫 주문은 기다리지 않는다
    CHECK(order_pacer.wait_before_send(start_time) == Clock::duration::zero());
    order_pacer.note_sent(start_time);
    CHECK(order_pacer.wait_before_send(start_time) == milliseconds(350));
    CHECK(order_pacer.wait_before_send(start_time + milliseconds(100)) == milliseconds(250));
    CHECK(order_pacer.wait_before_send(start_time + milliseconds(350)) == Clock::duration::zero());
    CHECK(order_pacer.wait_before_send(start_time + milliseconds(900)) == Clock::duration::zero());
    // 재시도 지연은 deduplicate 창(1.2s) 아래로 내려가지 않고, 간격이 더 길면 간격을 따른다
    CHECK(order_pacer.retry_delay() == milliseconds(1200));
    CHECK(OrderPacer({2000, 3}, start_time).retry_delay() == milliseconds(2000));
    return 0;
}

int test_retry_queue()
{
    const auto start_time = Clock::now();
    OrderPacer order_pacer({350, 3}, start_time);
    int held = 10;
    order_pacer.set_position([&](const std::string&, const std::string&) { return held; });

    // 예약 → 만기 전엔 없음 → 만기 뒤 attempts+1로 나온다
    CHECK(order_pacer.on_rejected({signal("A", OrderSide::SELL, 10), 0}, OrderStatus::REJECTED, "APBK0013", start_time));
    CHECK(order_pacer.retry_count() == 1);
    CHECK(!order_pacer.take_due_retry(start_time + milliseconds(1199)));
    auto take_due_retry = order_pacer.take_due_retry(start_time + milliseconds(1200));
    CHECK(take_due_retry && take_due_retry->signal.ticker == "A" && take_due_retry->attempts == 1);
    CHECK(order_pacer.retry_count() == 0);

    // 접수·비대상 거부는 예약하지 않는다
    CHECK(!order_pacer.on_rejected({signal("A", OrderSide::SELL, 10), 0}, OrderStatus::ACCEPTED, "", start_time));
    CHECK(!order_pacer.on_rejected({signal("A", OrderSide::BUY, 10), 0}, OrderStatus::REJECTED, "APBK0013", start_time));
    CHECK(!order_pacer.on_rejected({signal("A", OrderSide::SELL, 10), 3}, OrderStatus::REJECTED, "APBK0013", start_time));
    CHECK(order_pacer.retry_count() == 0);

    // 만기 순서는 FIFO — 분당 거부(20s)가 앞에 있으면 뒤의 1.2s 건도 앞이 만기될 때까지 기다린다
    CHECK(order_pacer.on_rejected({signal("B", OrderSide::BUY, 5), 0}, OrderStatus::REJECTED, "Rate limit 초과 (분당 40)", start_time));
    CHECK(order_pacer.on_rejected({signal("C", OrderSide::SELL, 5), 0}, OrderStatus::REJECTED, "APBK0013", start_time));
    CHECK(!order_pacer.take_due_retry(start_time + milliseconds(5000)));
    take_due_retry = order_pacer.take_due_retry(start_time + milliseconds(20000));
    CHECK(take_due_retry && take_due_retry->signal.ticker == "B" && take_due_retry->attempts == 1);
    take_due_retry = order_pacer.take_due_retry(start_time + milliseconds(20000));
    CHECK(take_due_retry && take_due_retry->signal.ticker == "C");
    CHECK(!order_pacer.take_due_retry(start_time + milliseconds(20000)));

    // 청산 완료(보유 0)면 만기된 SELL 재시도를 버리고 다음 것을 준다. BUY 재시도는 보유와 무관
    CHECK(order_pacer.on_rejected({signal("D", OrderSide::SELL, 5), 0}, OrderStatus::REJECTED, "APBK0013", start_time));
    CHECK(order_pacer.on_rejected({signal("E", OrderSide::BUY, 5), 0}, OrderStatus::REJECTED, "EGW00201", start_time));
    held = 0;
    take_due_retry    = order_pacer.take_due_retry(start_time + milliseconds(1200));
    CHECK(take_due_retry && take_due_retry->signal.ticker == "E");
    CHECK(order_pacer.retry_count() == 0);

    // 횟수는 재시도마다 이어진다: 2회째 거부 → attempts 2, 3회째 거부는 예약 없음
    CHECK(order_pacer.on_rejected({signal("F", OrderSide::SELL, 5), 0}, OrderStatus::REJECTED, "APBK0013", start_time));
    held = 5;
    take_due_retry    = order_pacer.take_due_retry(start_time + milliseconds(1200));
    CHECK(take_due_retry && take_due_retry->attempts == 1);
    CHECK(order_pacer.on_rejected(*take_due_retry, OrderStatus::REJECTED, "APBK0013", start_time));
    take_due_retry = order_pacer.take_due_retry(start_time + milliseconds(1200));
    CHECK(take_due_retry && take_due_retry->attempts == 2);
    CHECK(order_pacer.on_rejected(*take_due_retry, OrderStatus::REJECTED, "APBK0013", start_time));
    take_due_retry = order_pacer.take_due_retry(start_time + milliseconds(1200));
    CHECK(take_due_retry && take_due_retry->attempts == 3);
    CHECK(!order_pacer.on_rejected(*take_due_retry, OrderStatus::REJECTED, "APBK0013", start_time));
    CHECK(order_pacer.retry_count() == 0);
    return 0;
}
} // namespace

int main()
{
    // 산출물을 라이브 로그 폴더와 갈라 둔다(test_order_router와 같은 이유). QUANT_LOG_DIR이 있으면 존중.
    if (const char* environment = std::getenv("QUANT_LOG_DIR"); !environment || !*environment)
    {
        Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");
    }

    if (test_classify() || test_interval() || test_retry_queue())
    {
        return 1;
    }

    Logger::instance().flush();
    std::cout << "test_order_pacer: " << g_checks << " checks passed\n";
    return 0;
}
