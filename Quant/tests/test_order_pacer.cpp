// 발주 조절기(core/OrderPacer.h) 단위 테스트. 보유 수량 조회를 std::function으로 대신해 Engine·OrderGate 없이
//  발주 간격, 거부 분류(유량 한도·청산 SELL·40240000·BUY 제외·횟수 소진), 재시도 만기 순서와 청산 완료 폐기를
//  고정한다. Logger만 링크한다. 관련 결정: C-2(청산 SELL 재시도), D-065(분리).
// 빌드: cmake --build <dir> --target test_order_pacer
#include "core/OrderPacer.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <iostream>

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

using Clock = OrderPacer::Clock;
using std::chrono::milliseconds;
using pacing::Retry;

OrderSignal sig(const char* ticker, OrderSide side, int qty, OrderAction action = OrderAction::NEW)
{
    OrderSignal s;
    s.ticker      = ticker;
    s.account_id  = "ACC";
    s.side        = side;
    s.type        = OrderType::MARKET;
    s.quantity    = qty;
    s.action      = action;
    s.strategy_id = "T";
    return s;
}

const milliseconds kDelay(1200);

int test_classify()
{
    const auto sell = sig("A", OrderSide::SELL, 10);
    const auto buy  = sig("A", OrderSide::BUY, 10);
    const auto cxl  = sig("A", OrderSide::BUY, 0, OrderAction::CANCEL);

    // 접수·횟수 소진은 재시도 없음
    CHECK(pacing::classify(sell, 0, 3, OrderStatus::ACCEPTED, "", kDelay).kind == Retry::NONE);
    CHECK(pacing::classify(sell, 3, 3, OrderStatus::REJECTED, "x", kDelay).kind == Retry::NONE);

    // 유량 한도 — KIS EGW00201과 게이트 "Rate limit …" 둘 다, action 불문
    auto p = pacing::classify(cxl, 0, 3, OrderStatus::REJECTED, "EGW00201 초당 거래건수 초과", kDelay);
    CHECK(p.kind == Retry::RATE_LIMIT && p.delay == kDelay);
    p = pacing::classify(buy, 2, 3, OrderStatus::REJECTED, "Rate limit 초과 (초당 5)", kDelay);
    CHECK(p.kind == Retry::RATE_LIMIT && p.delay == kDelay);
    // 분당 한도는 20초로 물러난다
    p = pacing::classify(buy, 0, 3, OrderStatus::REJECTED, "Rate limit 초과 (분당 40)", kDelay);
    CHECK(p.kind == Retry::RATE_LIMIT && p.delay == milliseconds(20000));
    // "Rate limit"은 문자열 머리에 있을 때만 게이트 거부다
    CHECK(pacing::classify(buy, 0, 3, OrderStatus::REJECTED, "기타 Rate limit", kDelay).kind == Retry::NONE);

    // 청산 SELL 거부는 되쏜다. 40240000은 지속성 조건이라 제외
    p = pacing::classify(sell, 1, 3, OrderStatus::REJECTED, "APBK0013 모의투자 장종료", kDelay);
    CHECK(p.kind == Retry::SELL_REJECTED && p.delay == kDelay);
    CHECK(pacing::classify(sell, 0, 3, OrderStatus::REJECTED, "40240000 주문가능수량 없음", kDelay).kind ==
          Retry::NONE);
    // BUY·취소의 일반 거부는 되쏘지 않는다(빈-ODNO 중복주문 위험)
    CHECK(pacing::classify(buy, 0, 3, OrderStatus::REJECTED, "APBK0013", kDelay).kind == Retry::NONE);
    CHECK(pacing::classify(sig("A", OrderSide::SELL, 0, OrderAction::CANCEL), 0, 3, OrderStatus::REJECTED,
                           "APBK0013", kDelay)
              .kind == Retry::NONE);
    return 0;
}

int test_interval()
{
    const auto t0 = Clock::now();
    OrderPacer p({350, 3}, t0);
    // 첫 주문은 기다리지 않는다
    CHECK(p.wait_before_send(t0) == Clock::duration::zero());
    p.note_sent(t0);
    CHECK(p.wait_before_send(t0) == milliseconds(350));
    CHECK(p.wait_before_send(t0 + milliseconds(100)) == milliseconds(250));
    CHECK(p.wait_before_send(t0 + milliseconds(350)) == Clock::duration::zero());
    CHECK(p.wait_before_send(t0 + milliseconds(900)) == Clock::duration::zero());
    // 재시도 지연은 dedup 창(1.2s) 아래로 내려가지 않고, 간격이 더 길면 간격을 따른다
    CHECK(p.retry_delay() == milliseconds(1200));
    CHECK(OrderPacer({2000, 3}, t0).retry_delay() == milliseconds(2000));
    return 0;
}

int test_retry_queue()
{
    const auto t0 = Clock::now();
    OrderPacer p({350, 3}, t0);
    int held = 10;
    p.set_position([&](const std::string&, const std::string&) { return held; });

    // 예약 → 만기 전엔 없음 → 만기 뒤 attempts+1로 나온다
    CHECK(p.on_rejected({sig("A", OrderSide::SELL, 10), 0}, OrderStatus::REJECTED, "APBK0013", t0));
    CHECK(p.retry_count() == 1);
    CHECK(!p.take_due_retry(t0 + milliseconds(1199)));
    auto r = p.take_due_retry(t0 + milliseconds(1200));
    CHECK(r && r->sig.ticker == "A" && r->attempts == 1);
    CHECK(p.retry_count() == 0);

    // 접수·비대상 거부는 예약하지 않는다
    CHECK(!p.on_rejected({sig("A", OrderSide::SELL, 10), 0}, OrderStatus::ACCEPTED, "", t0));
    CHECK(!p.on_rejected({sig("A", OrderSide::BUY, 10), 0}, OrderStatus::REJECTED, "APBK0013", t0));
    CHECK(!p.on_rejected({sig("A", OrderSide::SELL, 10), 3}, OrderStatus::REJECTED, "APBK0013", t0));
    CHECK(p.retry_count() == 0);

    // 만기 순서는 FIFO — 분당 거부(20s)가 앞에 있으면 뒤의 1.2s 건도 앞이 만기될 때까지 기다린다
    CHECK(p.on_rejected({sig("B", OrderSide::BUY, 5), 0}, OrderStatus::REJECTED, "Rate limit 초과 (분당 40)", t0));
    CHECK(p.on_rejected({sig("C", OrderSide::SELL, 5), 0}, OrderStatus::REJECTED, "APBK0013", t0));
    CHECK(!p.take_due_retry(t0 + milliseconds(5000)));
    r = p.take_due_retry(t0 + milliseconds(20000));
    CHECK(r && r->sig.ticker == "B" && r->attempts == 1);
    r = p.take_due_retry(t0 + milliseconds(20000));
    CHECK(r && r->sig.ticker == "C");
    CHECK(!p.take_due_retry(t0 + milliseconds(20000)));

    // 청산 완료(보유 0)면 만기된 SELL 재시도를 버리고 다음 것을 준다. BUY 재시도는 보유와 무관
    CHECK(p.on_rejected({sig("D", OrderSide::SELL, 5), 0}, OrderStatus::REJECTED, "APBK0013", t0));
    CHECK(p.on_rejected({sig("E", OrderSide::BUY, 5), 0}, OrderStatus::REJECTED, "EGW00201", t0));
    held = 0;
    r    = p.take_due_retry(t0 + milliseconds(1200));
    CHECK(r && r->sig.ticker == "E");
    CHECK(p.retry_count() == 0);

    // 횟수는 재시도마다 이어진다: 2회째 거부 → attempts 2, 3회째 거부는 예약 없음
    CHECK(p.on_rejected({sig("F", OrderSide::SELL, 5), 0}, OrderStatus::REJECTED, "APBK0013", t0));
    held = 5;
    r    = p.take_due_retry(t0 + milliseconds(1200));
    CHECK(r && r->attempts == 1);
    CHECK(p.on_rejected(*r, OrderStatus::REJECTED, "APBK0013", t0));
    r = p.take_due_retry(t0 + milliseconds(1200));
    CHECK(r && r->attempts == 2);
    CHECK(p.on_rejected(*r, OrderStatus::REJECTED, "APBK0013", t0));
    r = p.take_due_retry(t0 + milliseconds(1200));
    CHECK(r && r->attempts == 3);
    CHECK(!p.on_rejected(*r, OrderStatus::REJECTED, "APBK0013", t0));
    CHECK(p.retry_count() == 0);
    return 0;
}
} // namespace

int main()
{
    // 산출물을 라이브 로그 폴더와 갈라 둔다(test_order_router와 같은 이유). QUANT_LOG_DIR이 있으면 존중.
    if (const char* env = std::getenv("QUANT_LOG_DIR"); !env || !*env)
    {
        Logger::instance().set_base_dir(Logger::executable_dir() / "logs_test");
    }

    if (test_classify() || test_interval() || test_retry_queue())
    {
        return 1;
    }

    Logger::instance().flush();
    std::cout << "test_order_pacer: " << g_checks << " checks passed\n";
    return 0;
}
