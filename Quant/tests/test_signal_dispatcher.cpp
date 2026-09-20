// 신호 디스패처(core/SignalDispatcher.h) 단위 테스트. 주문 큐·ZMQ·종목 표기·청산 관리 여부를 std::function으로
//  대신해 Engine 없이 순번 부여, 비활성 전략·청산 관리 티커 차단, 교체 진입의 매도-보류-발주·만료·취소, 강제청산
//  잔량 계산과 스로틀, 한도 초과분 정리의 1회성, 전략 활성 플래그(국면·유니버스 AND), 유니버스 이탈·복귀 판정과
//  등록 상한 교체 후보 선택(core/UniverseExit.h)을 고정한다. OrderGate·Logger를 링크한다.
//  바스켓 슬롯 제외 종목은 강제청산·초과분 정리가 건드리지 않는 것도 본다.
//  관련 결정: D-019(교체 진입), D-038(순번), D-063(분리), D-077(유니버스 이탈), D-087(등록층 점수 교체), D-109(바스켓 슬리브).
// 빌드: cmake --build <directory> --target test_signal_dispatcher
#include "core/SignalDispatcher.h"
#include "core/UniverseExit.h"
#include "strategy/StrategyBase.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <vector>

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

using Clock = SignalDispatcher::Clock;

OrderSignal signal(const char* ticker, OrderSide side, int quantity, OrderAction action = OrderAction::NEW,
                const char* strategy = "T")
{
    OrderSignal signal;
    signal.ticker      = ticker;
    signal.side        = side;
    signal.type        = OrderType::LIMIT;
    signal.quantity    = quantity;
    signal.price       = 1000.0;
    signal.action      = action;
    signal.strategy_id = strategy;
    return signal;
}

OrderGate::Config open_config()
{
    OrderGate::Config config;
    config.max_quantity_per_ticker = 1000;
    config.max_orders_per_min = 1000;
    config.max_orders_per_sec = 1000;
    return config;
}

// 슬롯 2개가 찬 책. 교체 진입은 켜고 보유 시간 조건은 끈다(test_order_gate와 같은 설정).
OrderGate::Config displace_config()
{
    auto config                     = open_config();
    config.max_concurrent_positions = 2;
    config.displace_enabled         = true;
    config.displace_min_z_gap       = 0.5;
    config.displace_min_hold_sec    = 0;
    config.displace_slot_hold_sec   = 300;
    return config;
}

void seed_full_book(OrderGate& gate)
{
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{gate.intern_symbol("A"), 1, 0.9}, {gate.intern_symbol("B"), 2, -0.8}, {gate.intern_symbol("C"), 3, 1.5}}, 3);
}

struct Rig
{
    OrderGate                gate;
    std::vector<OrderSignal> out;
    Clock::time_point        start_time = Clock::now();
    SignalDispatcher         dispatcher;

    explicit Rig(OrderGate::Config config)
        : gate(config), dispatcher(gate, [this](const OrderSignal& signal) { out.push_back(signal); }, start_time)
    {
        dispatcher.set_label([](const std::string& ticker) { return "<" + ticker + ">"; });
    }
};

int test_stamp()
{
    Rig rig(open_config());
    rig.dispatcher.submit(signal("A", OrderSide::BUY, 1));
    rig.dispatcher.submit(signal("A", OrderSide::SELL, 2));
    CHECK(rig.out.size() == 2 && rig.out[0].sequence == 1 && rig.out[1].sequence == 2 && rig.dispatcher.sequence() == 2);
    CHECK(rig.out[1].side == OrderSide::SELL && rig.out[1].quantity == 2);

    // 로그 한 줄 — 취소는 동작을 앞에, 대상 주문을 뒤에 적는다.
    auto cancel_signal            = signal("A", OrderSide::BUY, 0, OrderAction::CANCEL);
    cancel_signal.original_client_order_id = "oid-7";
    cancel_signal.reason          = "재구성";
    const auto line   = dispatch::describe(cancel_signal, "<A>");
    CHECK(line == "[T] <A> 취소 BUY 0 대상=oid-7 | 근거: 재구성");
    CHECK(dispatch::describe(signal("A", OrderSide::SELL, 3), "A") == "[T] A SELL 3");
    return 0;
}

int test_strategy_gate()
{
    Rig rig(open_config());
    const symbol::SymbolId symbol_g = rig.gate.intern_symbol("G");
    rig.dispatcher.set_exit_managed_check([symbol_g](symbol::SymbolId symbol) { return symbol == symbol_g; });

    // 비활성 전략: 신규 매수만 막고 매도·취소는 통과.
    rig.dispatcher.from_strategy(false, false, signal("A", OrderSide::BUY, 1));
    CHECK(rig.out.empty());
    rig.dispatcher.from_strategy(false, false, signal("A", OrderSide::SELL, 1));
    rig.dispatcher.from_strategy(false, false, signal("A", OrderSide::BUY, 0, OrderAction::CANCEL));
    CHECK(rig.out.size() == 2 && rig.out[1].action == OrderAction::CANCEL);

    // 청산 관리 티커: ITB_ 밖 전략의 신규는 매수·매도 다 막고, 취소·정정과 ITB_는 통과.
    rig.dispatcher.from_strategy(true, false, signal("G", OrderSide::BUY, 1));
    rig.dispatcher.from_strategy(true, false, signal("G", OrderSide::SELL, 1));
    CHECK(rig.out.size() == 2);
    rig.dispatcher.from_strategy(true, false, signal("G", OrderSide::SELL, 0, OrderAction::REPLACE));
    rig.dispatcher.from_strategy(true, true, signal("G", OrderSide::BUY, 1));
    CHECK(rig.out.size() == 4 && rig.out[3].strategy_id == "T");
    // 청산 관리 밖 티커는 어느 전략이든 통과.
    rig.dispatcher.from_strategy(true, false, signal("H", OrderSide::BUY, 1));
    CHECK(rig.out.size() == 5);

    // 전략이 디스패처에 주는 active는 국면 축과 유니버스 축의 AND다 — 어느 한쪽이 닫히면 신규매수가 막힌다 (D-077).
    struct Stub : StrategyBase
    {
        const std::string& id() const override
        {
            static const std::string kId = "S";

            return kId;
        }

        std::string describe() const override { return "S"; }
        std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }
    } stop_token;
    CHECK(stop_token.is_active() && stop_token.in_universe());
    stop_token.set_in_universe(false);
    CHECK(!stop_token.is_active());
    stop_token.set_active(false);
    stop_token.set_in_universe(true);
    CHECK(!stop_token.is_active() && stop_token.in_universe());
    stop_token.set_active(true);
    CHECK(stop_token.is_active());
    return 0;
}

// 운영단말 수동 매도 정지(D-095): 전략의 SELL NEW만 막고, 매수·취소는 통과. 끄면 다시 나간다.
int test_manual_sell_halt()
{
    Rig rig(open_config());
    rig.gate.set_manual_halt(OrderSide::SELL, true);
    CHECK(rig.gate.is_manual_sell_halted() && !rig.gate.is_manual_buy_halted() && !rig.gate.is_entry_halted());

    rig.dispatcher.from_strategy(true, false, signal("A", OrderSide::SELL, 1));
    CHECK(rig.out.empty());
    rig.dispatcher.from_strategy(true, false, signal("A", OrderSide::BUY, 1));
    rig.dispatcher.from_strategy(true, false, signal("A", OrderSide::SELL, 0, OrderAction::CANCEL));
    CHECK(rig.out.size() == 2 && rig.out[0].side == OrderSide::BUY && rig.out[1].action == OrderAction::CANCEL);

    rig.gate.set_manual_halt(OrderSide::SELL, false);
    rig.dispatcher.from_strategy(true, false, signal("A", OrderSide::SELL, 1));
    CHECK(rig.out.size() == 3 && rig.out[2].side == OrderSide::SELL);

    // 매수 정지는 is_entry_halted()로만 드러난다(전략이 신호를 안 만든다) — 디스패처는 매도를 막지 않는다.
    rig.gate.set_manual_halt(OrderSide::BUY, true);
    CHECK(rig.gate.is_entry_halted() && !rig.gate.is_manual_sell_halted());
    rig.dispatcher.from_strategy(true, false, signal("A", OrderSide::SELL, 1));
    CHECK(rig.out.size() == 4);
    return 0;
}

// 유니버스 이탈은 시계 하나에 임계값 둘 — 차단(block)이 먼저, 해제(drop)가 뒤. 복귀는 present 연속 횟수로 (D-077).
int test_universe_exit_judge()
{
    using namespace universe_exit;
    const Thresholds thread{40, 600, 2};
    // 부재 20초(첫 부재 스캔)는 아무것도 아니고, 40초(둘째 연속 부재)에 차단, 600초에 해제.
    CHECK(judge_absent(0, thread, true) == Absent::KEEP);
    CHECK(judge_absent(20, thread, true) == Absent::KEEP);
    CHECK(judge_absent(40, thread, true) == Absent::BLOCK);
    CHECK(judge_absent(599, thread, true) == Absent::BLOCK);
    CHECK(judge_absent(600, thread, true) == Absent::DROP);
    // 이미 차단된 종목에 BLOCK을 다시 내지 않는다 — 로그가 20초마다 반복되지 않게.
    CHECK(judge_absent(40, thread, false) == Absent::KEEP);
    CHECK(judge_absent(599, thread, false) == Absent::KEEP);
    CHECK(judge_absent(600, thread, false) == Absent::DROP);
    // block≤0이면 차단 없이 해제만, drop≤0이면 해제 없이 차단만.
    CHECK(judge_absent(1000, Thresholds{0, 600, 2}, true) == Absent::DROP);
    CHECK(judge_absent(300, Thresholds{0, 600, 2}, true) == Absent::KEEP);
    CHECK(judge_absent(1000, Thresholds{40, 0, 2}, true) == Absent::BLOCK);
    CHECK(judge_absent(1000, Thresholds{40, 0, 2}, false) == Absent::KEEP);
    // 복귀: 차단 중 present 2회 연속에 푼다. 열려 있는 종목은 판정 대상이 아니다. confirm≤1은 1회.
    CHECK(!judge_return(1, thread, false));
    CHECK(judge_return(2, thread, false));
    CHECK(!judge_return(5, thread, true));
    CHECK(judge_return(1, Thresholds{40, 600, 0}, false));
    // 차단이 해제보다 늦으면 해제 시각에 맞춘다. drop이 꺼져 있으면 그대로 둔다.
    CHECK(clamp_block(900, 600) == 600);
    CHECK(clamp_block(40, 600) == 40);
    CHECK(clamp_block(900, 0) == 900);
    return 0;
}

// 등록 상한이 찼을 때 오늘 순위 밖·미보유 종목에게만, 부재가 가장 긴 것부터 자리를 비운다 (D-087).
int test_universe_evict_pick()
{
    using namespace universe_exit;
    // 종목 A·B·C = id 1·2·3. 순회 목록은 일부러 등록 순서를 뒤섞어 순서에 기대지 않음을 같이 본다.
    constexpr symbol::SymbolId kA = 1;
    constexpr symbol::SymbolId kB = 2;
    constexpr symbol::SymbolId kC = 3;
    const std::vector<symbol::SymbolId> owned{kC, kA, kB};
    auto bits = [](std::initializer_list<symbol::SymbolId> symbols) {
        std::vector<bool> out(8, false);

        for (symbol::SymbolId symbol : symbols)
        {
            out[symbol] = true;
        }

        return out;
    };
    auto reserved0  = [](symbol::SymbolId) { return 0; };
    auto no_absence = [](symbol::SymbolId) -> long long { return 0; };

    // A만 오늘 top-N 밖(미보유) — A가 후보.
    CHECK(pick_evict_candidate(owned, bits({kB, kC}), bits({}), reserved0, no_absence) == kA);
    // 전부 오늘 top-N 안이면 내줄 게 없다.
    CHECK(pick_evict_candidate(owned, bits({kA, kB, kC}), bits({}), reserved0, no_absence) == symbol::kNone);
    // top-N 밖이어도 보유 중이면 대상 아님.
    CHECK(pick_evict_candidate(owned, bits({kB, kC}), bits({kA}), reserved0, no_absence) == symbol::kNone);
    // top-N 밖이어도 선점(reserved) 중이면 대상 아님.
    auto reserved_a = [](symbol::SymbolId symbol) { return symbol == kA ? 1 : 0; };
    CHECK(pick_evict_candidate(owned, bits({kB, kC}), bits({}), reserved_a, no_absence) == symbol::kNone);
    // 부재 시간이 다르면 가장 오래 밖에 있던 쪽(B)을 고른다 — owned 순회 순서와 무관.
    auto absence_b_longer = [](symbol::SymbolId symbol) -> long long { return symbol == kB ? 900 : 100; };
    CHECK(pick_evict_candidate(owned, bits({}), bits({}), reserved0, absence_b_longer) == kB);
    // 부재 시간이 전부 같으면(추적 없음 포함) id가 작은 쪽으로 고정 — 목록 순서(C·A·B)에 기대지 않는다.
    CHECK(pick_evict_candidate(owned, bits({}), bits({}), reserved0, no_absence) == kA);
    return 0;
}

int test_displace_hold_and_release()
{
    Rig rig(displace_config());
    seed_full_book(rig.gate);
    CHECK(rig.gate.capacity_full());

    // 꽉 찬 책에 C 매수 → 최약체 B 전량 매도가 나가고 C 매수는 보류.
    rig.dispatcher.submit(signal("C", OrderSide::BUY, 1));
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "B" && rig.out[0].side == OrderSide::SELL &&
          rig.out[0].type == OrderType::MARKET && rig.out[0].quantity == 10 && rig.out[0].reference_price == 1000.0 &&
          rig.out[0].strategy_id == "DISPLACE");
    CHECK(rig.dispatcher.held_ticker() == "C" && rig.dispatcher.held_count() == 1);

    // 같은 분할 매수의 다음 분할 단계도 보류에 붙는다. 다른 종목의 매도는 그대로 나간다.
    rig.dispatcher.submit(signal("C", OrderSide::BUY, 2));
    rig.dispatcher.submit(signal("A", OrderSide::SELL, 1));
    CHECK(rig.dispatcher.held_count() == 2 && rig.out.size() == 2 && rig.out[1].ticker == "A");

    // 자리가 안 났으면 flush는 아무것도 안 한다.
    rig.dispatcher.flush_held(Clock::now());
    CHECK(rig.out.size() == 2);

    // B 매도 체결 → 자리 → 보류 매수 둘이 순번을 이어 나간다.
    rig.gate.on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
    CHECK(!rig.gate.capacity_full());
    rig.dispatcher.flush_held(Clock::now());
    CHECK(rig.out.size() == 4 && rig.out[2].ticker == "C" && rig.out[2].quantity == 1 && rig.out[3].quantity == 2 &&
          rig.out[3].sequence == 4);
    CHECK(rig.dispatcher.held_count() == 0 && rig.dispatcher.held_ticker() == "C");

    // 자리가 있는 책에서는 매수가 곧장 나간다.
    rig.dispatcher.submit(signal("C", OrderSide::BUY, 3));
    CHECK(rig.out.size() == 5 && rig.out[4].ticker == "C");
    return 0;
}

int test_displace_cancel_and_expiry()
{
    {
        Rig rig(displace_config());
        seed_full_book(rig.gate);
        rig.dispatcher.submit(signal("C", OrderSide::BUY, 1));
        CHECK(rig.dispatcher.held_count() == 1);
        // 전략이 분할 매수를 다시 깐다 — 취소가 오면 들고 있던 분할 단계를 비운다(취소 자체는 나간다).
        rig.dispatcher.submit(signal("C", OrderSide::BUY, 0, OrderAction::CANCEL));
        CHECK(rig.dispatcher.held_count() == 0 && rig.out.size() == 2 && rig.out[1].action == OrderAction::CANCEL);
    }

    {
        Rig rig(displace_config());
        seed_full_book(rig.gate);
        rig.dispatcher.submit(signal("C", OrderSide::BUY, 1));
        // 예약 시한이 지나면 버린다 — 자리가 났어도.
        rig.gate.on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
        rig.dispatcher.flush_held(Clock::now() + std::chrono::seconds(301));
        CHECK(rig.dispatcher.held_count() == 0 && rig.dispatcher.held_ticker().empty() && rig.out.size() == 1);
    }

    {
        // 교체가 꺼져 있으면 꽉 찬 책이라도 매수는 그대로 나간다(거부는 게이트 몫).
        auto config             = displace_config();
        config.displace_enabled = false;
        Rig rig(config);
        seed_full_book(rig.gate);
        rig.dispatcher.submit(signal("C", OrderSide::BUY, 1));
        CHECK(rig.out.size() == 1 && rig.out[0].ticker == "C" && rig.dispatcher.held_ticker().empty());
    }

    return 0;
}

int test_force_liquidation_orders()
{
    // 종목 id는 스냅샷에 실려 온다 — 미체결 조회도 그 id로 묻는다.
    constexpr symbol::SymbolId kA = 1, kB = 2, kC = 3;
    std::vector<OrderGate::HeldPos> held = {{"", "A", 10, 100.0, kA}, {"", "B", 5, 200.0, kB}, {"", "C", 3, 300.0, kC}};
    const auto reserved                  = [](const std::string&, symbol::SymbolId symbol)
    {
        if (symbol == kA)
        {
            return -4; // 미체결 매도 4
        }

        if (symbol == kB)
        {
            return -5; // 전량 이미 매도 중
        }

        return 2; // 미체결 매수는 잔량에 영향 없음
    };
    const auto out = dispatch::force_liquidation_orders(held, reserved);
    CHECK(out.size() == 2);
    CHECK(out[0].ticker == "A" && out[0].quantity == 6 && out[0].reference_price == 100.0 &&
          out[0].strategy_id == "FORCE_LIQ" && out[0].type == OrderType::MARKET && out[0].side == OrderSide::SELL);
    CHECK(out[1].ticker == "C" && out[1].quantity == 3);
    CHECK(out[0].reason.find("미체결매도=4") != std::string::npos);
    return 0;
}

int test_trim_orders()
{
    constexpr symbol::SymbolId      kA = 1, kB = 2, kC = 3, kD = 4;
    std::vector<OrderGate::HeldPos> held = {
        {"", "A", 10, 100.0, kA}, // 한도수량 10 — 초과 없음
        {"", "B", 20, 100.0, kB}, // 초과 10, 미체결 매도 3 → 7
        {"", "C", 20, 0.0, kC},   // 평단 없음 — 건너뜀
        {"", "D", 15, 100.0, kD}, // 초과 5, 미체결 매도 10 → 0
    };
    const auto reserved = [](const std::string&, symbol::SymbolId symbol)
    {
        return symbol == kB ? -3 : (symbol == kD ? -10 : 0);
    };
    const auto out = dispatch::trim_orders(held, 1000.0, reserved);
    CHECK(out.size() == 1 && out[0].ticker == "B" && out[0].quantity == 7 && out[0].strategy_id == "LIMIT_TRIM" &&
          out[0].reference_price == 100.0);
    CHECK(out[0].reason.find("한도수량=10") != std::string::npos);
    CHECK(dispatch::trim_orders(held, 0.0, reserved).empty());
    return 0;
}

int test_force_liquidation_throttle()
{
    Rig rig(open_config());
    rig.gate.seed_position("", "A", 10, 100.0);

    // 기준 시각 직후에는 안 나가고(간격 미달), 간격이 차야 한 번, 다시 간격이 차야 또 한 번.
    rig.dispatcher.force_liquidate(rig.start_time);
    rig.dispatcher.force_liquidate(rig.start_time + std::chrono::milliseconds(1999));
    CHECK(rig.out.empty());
    rig.dispatcher.force_liquidate(rig.start_time + std::chrono::seconds(2));
    CHECK(rig.out.size() == 1 && rig.out[0].strategy_id == "FORCE_LIQ" && rig.out[0].quantity == 10);
    rig.dispatcher.force_liquidate(rig.start_time + std::chrono::seconds(3));
    CHECK(rig.out.size() == 1);
    rig.dispatcher.force_liquidate(rig.start_time + std::chrono::seconds(4));
    CHECK(rig.out.size() == 2 && rig.out[1].sequence == 2);

    // 간격을 줄이면 그만큼 자주.
    rig.dispatcher.set_liquidation_interval(std::chrono::milliseconds(500));
    rig.dispatcher.force_liquidate(rig.start_time + std::chrono::milliseconds(4500));
    CHECK(rig.out.size() == 3);
    return 0;
}

int test_trim_once()
{
    auto config                    = open_config();
    config.max_notional_per_ticker = 1000.0;
    Rig rig(config);
    rig.gate.seed_position("", "A", 20, 100.0);

    rig.dispatcher.trim_excess_once(rig.start_time + std::chrono::seconds(19));
    CHECK(rig.out.empty() && !rig.dispatcher.trim_done());
    rig.dispatcher.trim_excess_once(rig.start_time + std::chrono::seconds(20));
    CHECK(rig.out.size() == 1 && rig.out[0].strategy_id == "LIMIT_TRIM" && rig.out[0].quantity == 10 && rig.dispatcher.trim_done());
    rig.dispatcher.trim_excess_once(rig.start_time + std::chrono::seconds(60));
    CHECK(rig.out.size() == 1);

    // 시각을 바꾸면 그때부터. 한도가 0이면 정리 없이 끝난 것으로 표시한다.
    Rig second_rig(open_config());
    second_rig.gate.seed_position("", "A", 20, 100.0);
    second_rig.dispatcher.set_trim_at(second_rig.start_time + std::chrono::seconds(1));
    second_rig.dispatcher.trim_excess_once(second_rig.start_time + std::chrono::seconds(1));
    CHECK(second_rig.out.empty() && second_rig.dispatcher.trim_done());
    return 0;
}

// 바스켓 슬리브 종목(D-109)은 장중 스캔 슬리브의 것이 아니다 — 15:15 강제청산과 명목 한도 초과분 정리가 건너뛴다.
int test_sleeve_scan_skips_basket()
{
    auto config                    = open_config();
    config.max_notional_per_ticker = 1000.0;
    Rig rig(config);
    rig.gate.set_slot_exempt({"BK"});
    rig.gate.seed_position("", "BK", 50, 100.0); // 명목 5,000 > 한도 1,000이지만 바스켓 것
    rig.gate.seed_position("", "A", 20, 100.0);

    const auto sleeve = rig.dispatcher.scan_sleeve_positions();
    CHECK(sleeve.size() == 1 && sleeve[0].ticker == "A");

    rig.dispatcher.trim_excess_once(rig.start_time + std::chrono::seconds(20));
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "A" && rig.out[0].quantity == 10);

    rig.dispatcher.force_liquidate(rig.start_time + std::chrono::seconds(22));
    CHECK(rig.out.size() == 2 && rig.out[1].ticker == "A" && rig.out[1].strategy_id == "FORCE_LIQ");
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

    if (test_stamp() || test_strategy_gate() || test_manual_sell_halt() || test_universe_exit_judge() || test_universe_evict_pick() ||
        test_displace_hold_and_release() || test_displace_cancel_and_expiry() || test_force_liquidation_orders() ||
        test_trim_orders() || test_force_liquidation_throttle() || test_trim_once() || test_sleeve_scan_skips_basket())
    {
        return 1;
    }

    std::cout << "test_signal_dispatcher: " << g_checks << " checks passed\n";
    return 0;
}
