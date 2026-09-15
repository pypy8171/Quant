// 신호 디스패처(core/SignalDispatcher.h) 단위 테스트. 주문 큐·ZMQ·종목 표기·청산 관리 여부를 std::function으로
//  대신해 Engine 없이 순번 부여, 비활성 전략·청산 관리 티커 차단, 교체 진입의 매도-보류-발주·만료·취소, 강제청산
//  잔량 계산과 스로틀, 한도 초과분 정리의 1회성, 전략 활성 플래그(국면·유니버스 AND), 유니버스 이탈·복귀 판정과
//  등록 상한 교체 후보 선택(core/UniverseExit.h)을 고정한다. OrderGate·Logger를 링크한다.
//  관련 결정: D-019(교체 진입), D-038(순번), D-063(분리), D-077(유니버스 이탈), D-087(등록층 점수 교체).
// 빌드: cmake --build <dir> --target test_signal_dispatcher
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

using Clock = SignalDispatcher::Clock;

OrderSignal sig(const char* ticker, OrderSide side, int qty, OrderAction action = OrderAction::NEW,
                const char* strategy = "T")
{
    OrderSignal s;
    s.ticker      = ticker;
    s.side        = side;
    s.type        = OrderType::LIMIT;
    s.quantity    = qty;
    s.price       = 1000.0;
    s.action      = action;
    s.strategy_id = strategy;
    return s;
}

OrderGate::Config open_cfg()
{
    OrderGate::Config cfg;
    cfg.max_qty_per_ticker = 1000;
    cfg.max_orders_per_min = 1000;
    cfg.max_orders_per_sec = 1000;
    return cfg;
}

// 슬롯 2개가 찬 책. 교체 진입은 켜고 보유 시간 조건은 끈다(test_order_gate와 같은 설정).
OrderGate::Config displace_cfg()
{
    auto cfg                     = open_cfg();
    cfg.max_concurrent_positions = 2;
    cfg.displace_enabled         = true;
    cfg.displace_min_z_gap       = 0.5;
    cfg.displace_min_hold_sec    = 0;
    cfg.displace_slot_hold_sec   = 300;
    return cfg;
}

void seed_full_book(OrderGate& gate)
{
    gate.seed_position("", "A", 10, 1000.0);
    gate.seed_position("", "B", 10, 1000.0);
    gate.set_entry_priority({{"A", 1}, {"B", 2}, {"C", 3}}, {{"A", 0.9}, {"B", -0.8}, {"C", 1.5}}, 3);
}

struct Rig
{
    OrderGate                gate;
    std::vector<OrderSignal> out;
    Clock::time_point        t0 = Clock::now();
    SignalDispatcher         d;

    explicit Rig(OrderGate::Config cfg)
        : gate(cfg), d(gate, [this](const OrderSignal& s) { out.push_back(s); }, t0)
    {
        d.set_label([](const std::string& t) { return "<" + t + ">"; });
    }
};

int test_stamp()
{
    Rig r(open_cfg());
    r.d.submit(sig("A", OrderSide::BUY, 1));
    r.d.submit(sig("A", OrderSide::SELL, 2));
    CHECK(r.out.size() == 2 && r.out[0].seq == 1 && r.out[1].seq == 2 && r.d.seq() == 2);
    CHECK(r.out[1].side == OrderSide::SELL && r.out[1].quantity == 2);

    // 로그 한 줄 — 취소는 동작을 앞에, 대상 주문을 뒤에 적는다.
    auto c            = sig("A", OrderSide::BUY, 0, OrderAction::CANCEL);
    c.orig_client_oid = "oid-7";
    c.reason          = "재구성";
    const auto line   = dispatch::describe(c, "<A>");
    CHECK(line == "[T] <A> 취소 BUY 0 대상=oid-7 | 근거: 재구성");
    CHECK(dispatch::describe(sig("A", OrderSide::SELL, 3), "A") == "[T] A SELL 3");
    return 0;
}

int test_strategy_gate()
{
    Rig r(open_cfg());
    r.d.set_guardian([](const std::string& t) { return t == "G"; });

    // 비활성 전략: 신규 매수만 막고 매도·취소는 통과.
    r.d.from_strategy(false, "DEV_1", sig("A", OrderSide::BUY, 1));
    CHECK(r.out.empty());
    r.d.from_strategy(false, "DEV_1", sig("A", OrderSide::SELL, 1));
    r.d.from_strategy(false, "DEV_1", sig("A", OrderSide::BUY, 0, OrderAction::CANCEL));
    CHECK(r.out.size() == 2 && r.out[1].action == OrderAction::CANCEL);

    // 청산 관리 티커: ITB_ 밖 전략의 신규는 매수·매도 다 막고, 취소·정정과 ITB_는 통과.
    r.d.from_strategy(true, "DEV_1", sig("G", OrderSide::BUY, 1));
    r.d.from_strategy(true, "DEV_1", sig("G", OrderSide::SELL, 1));
    CHECK(r.out.size() == 2);
    r.d.from_strategy(true, "DEV_1", sig("G", OrderSide::SELL, 0, OrderAction::REPLACE));
    r.d.from_strategy(true, "ITB_1", sig("G", OrderSide::BUY, 1));
    CHECK(r.out.size() == 4 && r.out[3].strategy_id == "T");
    // 청산 관리 밖 티커는 어느 전략이든 통과.
    r.d.from_strategy(true, "DEV_1", sig("H", OrderSide::BUY, 1));
    CHECK(r.out.size() == 5);

    // 전략이 디스패처에 주는 active는 국면 축과 유니버스 축의 AND다 — 어느 한쪽이 닫히면 신규매수가 막힌다 (D-077).
    struct Stub : StrategyBase
    {
        std::string id() const override { return "S"; }
        std::string describe() const override { return "S"; }
        std::optional<OrderSignal> on_data(const MarketData&) override { return std::nullopt; }
    } st;
    CHECK(st.is_active() && st.in_universe());
    st.set_in_universe(false);
    CHECK(!st.is_active());
    st.set_active(false);
    st.set_in_universe(true);
    CHECK(!st.is_active() && st.in_universe());
    st.set_active(true);
    CHECK(st.is_active());
    return 0;
}

// 유니버스 이탈은 시계 하나에 임계값 둘 — 차단(block)이 먼저, 해제(drop)가 뒤. 복귀는 present 연속 횟수로 (D-077).
int test_universe_exit_judge()
{
    using namespace universe_exit;
    const Thresholds th{40, 600, 2};
    // 부재 20초(첫 부재 스캔)는 아무것도 아니고, 40초(둘째 연속 부재)에 차단, 600초에 해제.
    CHECK(judge_absent(0, th, true) == Absent::KEEP);
    CHECK(judge_absent(20, th, true) == Absent::KEEP);
    CHECK(judge_absent(40, th, true) == Absent::BLOCK);
    CHECK(judge_absent(599, th, true) == Absent::BLOCK);
    CHECK(judge_absent(600, th, true) == Absent::DROP);
    // 이미 차단된 종목에 BLOCK을 다시 내지 않는다 — 로그가 20초마다 반복되지 않게.
    CHECK(judge_absent(40, th, false) == Absent::KEEP);
    CHECK(judge_absent(599, th, false) == Absent::KEEP);
    CHECK(judge_absent(600, th, false) == Absent::DROP);
    // block≤0이면 차단 없이 해제만, drop≤0이면 해제 없이 차단만.
    CHECK(judge_absent(1000, Thresholds{0, 600, 2}, true) == Absent::DROP);
    CHECK(judge_absent(300, Thresholds{0, 600, 2}, true) == Absent::KEEP);
    CHECK(judge_absent(1000, Thresholds{40, 0, 2}, true) == Absent::BLOCK);
    CHECK(judge_absent(1000, Thresholds{40, 0, 2}, false) == Absent::KEEP);
    // 복귀: 차단 중 present 2회 연속에 푼다. 열려 있는 종목은 판정 대상이 아니다. confirm≤1은 1회.
    CHECK(!judge_return(1, th, false));
    CHECK(judge_return(2, th, false));
    CHECK(!judge_return(5, th, true));
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
    std::map<std::string, int> owned{{"A", 1}, {"B", 2}, {"C", 3}};
    auto reserved0  = [](const std::string&) { return 0; };
    auto no_absence = [](const std::string&) -> long long { return 0; };

    // A만 오늘 top-N 밖(미보유) — A가 후보.
    CHECK(pick_evict_candidate(owned, std::set<std::string>{"B", "C"}, std::set<std::string>{}, reserved0,
                                no_absence) == "A");
    // 전부 오늘 top-N 안이면 내줄 게 없다.
    CHECK(pick_evict_candidate(owned, std::set<std::string>{"A", "B", "C"}, std::set<std::string>{}, reserved0,
                                no_absence)
              .empty());
    // top-N 밖이어도 보유 중이면 대상 아님.
    CHECK(pick_evict_candidate(owned, std::set<std::string>{"B", "C"}, std::set<std::string>{"A"}, reserved0,
                                no_absence)
              .empty());
    // top-N 밖이어도 선점(reserved) 중이면 대상 아님.
    auto reserved_a = [](const std::string& t) { return t == "A" ? 1 : 0; };
    CHECK(pick_evict_candidate(owned, std::set<std::string>{"B", "C"}, std::set<std::string>{}, reserved_a,
                                no_absence)
              .empty());
    // 부재 시간이 다르면 가장 오래 밖에 있던 쪽(B)을 고른다 — owned 순회 순서와 무관.
    auto absence_b_longer = [](const std::string& t) -> long long { return t == "B" ? 900 : 100; };
    CHECK(pick_evict_candidate(owned, std::set<std::string>{}, std::set<std::string>{}, reserved0,
                                absence_b_longer) == "B");
    // 부재 시간이 전부 같으면(추적 없음 포함) 티커 문자열 순으로 고정 — 맵 순회 순서에 기대지 않는다.
    CHECK(pick_evict_candidate(owned, std::set<std::string>{}, std::set<std::string>{}, reserved0, no_absence) ==
          "A");
    return 0;
}

int test_displace_hold_and_release()
{
    Rig r(displace_cfg());
    seed_full_book(r.gate);
    CHECK(r.gate.capacity_full());

    // 꽉 찬 책에 C 매수 → 최약체 B 전량 매도가 나가고 C 매수는 보류.
    r.d.submit(sig("C", OrderSide::BUY, 1));
    CHECK(r.out.size() == 1 && r.out[0].ticker == "B" && r.out[0].side == OrderSide::SELL &&
          r.out[0].type == OrderType::MARKET && r.out[0].quantity == 10 && r.out[0].ref_price == 1000.0 &&
          r.out[0].strategy_id == "DISPLACE");
    CHECK(r.d.held_ticker() == "C" && r.d.held_count() == 1);

    // 같은 분할 매수의 다음 rung도 보류에 붙는다. 다른 종목의 매도는 그대로 나간다.
    r.d.submit(sig("C", OrderSide::BUY, 2));
    r.d.submit(sig("A", OrderSide::SELL, 1));
    CHECK(r.d.held_count() == 2 && r.out.size() == 2 && r.out[1].ticker == "A");

    // 자리가 안 났으면 flush는 아무것도 안 한다.
    r.d.flush_held(Clock::now());
    CHECK(r.out.size() == 2);

    // B 매도 체결 → 자리 → 보류 매수 둘이 순번을 이어 나간다.
    r.gate.on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
    CHECK(!r.gate.capacity_full());
    r.d.flush_held(Clock::now());
    CHECK(r.out.size() == 4 && r.out[2].ticker == "C" && r.out[2].quantity == 1 && r.out[3].quantity == 2 &&
          r.out[3].seq == 4);
    CHECK(r.d.held_count() == 0 && r.d.held_ticker() == "C");

    // 자리가 있는 책에서는 매수가 곧장 나간다.
    r.d.submit(sig("C", OrderSide::BUY, 3));
    CHECK(r.out.size() == 5 && r.out[4].ticker == "C");
    return 0;
}

int test_displace_cancel_and_expiry()
{
    {
        Rig r(displace_cfg());
        seed_full_book(r.gate);
        r.d.submit(sig("C", OrderSide::BUY, 1));
        CHECK(r.d.held_count() == 1);
        // 전략이 분할 매수를 다시 깐다 — 취소가 오면 들고 있던 rung을 비운다(취소 자체는 나간다).
        r.d.submit(sig("C", OrderSide::BUY, 0, OrderAction::CANCEL));
        CHECK(r.d.held_count() == 0 && r.out.size() == 2 && r.out[1].action == OrderAction::CANCEL);
    }

    {
        Rig r(displace_cfg());
        seed_full_book(r.gate);
        r.d.submit(sig("C", OrderSide::BUY, 1));
        // 예약 시한이 지나면 버린다 — 자리가 났어도.
        r.gate.on_fill_confirmed("", "B", OrderSide::SELL, 10, 1000.0);
        r.d.flush_held(Clock::now() + std::chrono::seconds(301));
        CHECK(r.d.held_count() == 0 && r.d.held_ticker().empty() && r.out.size() == 1);
    }

    {
        // 교체가 꺼져 있으면 꽉 찬 책이라도 매수는 그대로 나간다(거부는 게이트 몫).
        auto cfg             = displace_cfg();
        cfg.displace_enabled = false;
        Rig r(cfg);
        seed_full_book(r.gate);
        r.d.submit(sig("C", OrderSide::BUY, 1));
        CHECK(r.out.size() == 1 && r.out[0].ticker == "C" && r.d.held_ticker().empty());
    }

    return 0;
}

int test_force_liq_orders()
{
    std::vector<OrderGate::HeldPos> held = {{"", "A", 10, 100.0}, {"", "B", 5, 200.0}, {"", "C", 3, 300.0}};
    const auto reserved                  = [](const std::string&, const std::string& t)
    {
        if (t == "A")
        {
            return -4; // 미체결 매도 4
        }

        if (t == "B")
        {
            return -5; // 전량 이미 매도 중
        }

        return 2; // 미체결 매수는 잔량에 영향 없음
    };
    const auto out = dispatch::force_liq_orders(held, reserved);
    CHECK(out.size() == 2);
    CHECK(out[0].ticker == "A" && out[0].quantity == 6 && out[0].ref_price == 100.0 &&
          out[0].strategy_id == "FORCE_LIQ" && out[0].type == OrderType::MARKET && out[0].side == OrderSide::SELL);
    CHECK(out[1].ticker == "C" && out[1].quantity == 3);
    CHECK(out[0].reason.find("미체결매도=4") != std::string::npos);
    return 0;
}

int test_trim_orders()
{
    std::vector<OrderGate::HeldPos> held = {
        {"", "A", 10, 100.0}, // 한도수량 10 — 초과 없음
        {"", "B", 20, 100.0}, // 초과 10, 미체결 매도 3 → 7
        {"", "C", 20, 0.0},   // 평단 없음 — 건너뜀
        {"", "D", 15, 100.0}, // 초과 5, 미체결 매도 10 → 0
    };
    const auto reserved = [](const std::string&, const std::string& t)
    {
        return t == "B" ? -3 : (t == "D" ? -10 : 0);
    };
    const auto out = dispatch::trim_orders(held, 1000.0, reserved);
    CHECK(out.size() == 1 && out[0].ticker == "B" && out[0].quantity == 7 && out[0].strategy_id == "LIMIT_TRIM" &&
          out[0].ref_price == 100.0);
    CHECK(out[0].reason.find("한도수량=10") != std::string::npos);
    CHECK(dispatch::trim_orders(held, 0.0, reserved).empty());
    return 0;
}

int test_force_liq_throttle()
{
    Rig r(open_cfg());
    r.gate.seed_position("", "A", 10, 100.0);

    // 기준 시각 직후에는 안 나가고(간격 미달), 간격이 차야 한 번, 다시 간격이 차야 또 한 번.
    r.d.force_liquidate(r.t0);
    r.d.force_liquidate(r.t0 + std::chrono::milliseconds(1999));
    CHECK(r.out.empty());
    r.d.force_liquidate(r.t0 + std::chrono::seconds(2));
    CHECK(r.out.size() == 1 && r.out[0].strategy_id == "FORCE_LIQ" && r.out[0].quantity == 10);
    r.d.force_liquidate(r.t0 + std::chrono::seconds(3));
    CHECK(r.out.size() == 1);
    r.d.force_liquidate(r.t0 + std::chrono::seconds(4));
    CHECK(r.out.size() == 2 && r.out[1].seq == 2);

    // 간격을 줄이면 그만큼 자주.
    r.d.set_liq_interval(std::chrono::milliseconds(500));
    r.d.force_liquidate(r.t0 + std::chrono::milliseconds(4500));
    CHECK(r.out.size() == 3);
    return 0;
}

int test_trim_once()
{
    auto cfg                    = open_cfg();
    cfg.max_notional_per_ticker = 1000.0;
    Rig r(cfg);
    r.gate.seed_position("", "A", 20, 100.0);

    r.d.trim_excess_once(r.t0 + std::chrono::seconds(19));
    CHECK(r.out.empty() && !r.d.trim_done());
    r.d.trim_excess_once(r.t0 + std::chrono::seconds(20));
    CHECK(r.out.size() == 1 && r.out[0].strategy_id == "LIMIT_TRIM" && r.out[0].quantity == 10 && r.d.trim_done());
    r.d.trim_excess_once(r.t0 + std::chrono::seconds(60));
    CHECK(r.out.size() == 1);

    // 시각을 바꾸면 그때부터. 한도가 0이면 정리 없이 끝난 것으로 표시한다.
    Rig r2(open_cfg());
    r2.gate.seed_position("", "A", 20, 100.0);
    r2.d.set_trim_at(r2.t0 + std::chrono::seconds(1));
    r2.d.trim_excess_once(r2.t0 + std::chrono::seconds(1));
    CHECK(r2.out.empty() && r2.d.trim_done());
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

    if (test_stamp() || test_strategy_gate() || test_universe_exit_judge() || test_universe_evict_pick() ||
        test_displace_hold_and_release() || test_displace_cancel_and_expiry() || test_force_liq_orders() ||
        test_trim_orders() || test_force_liq_throttle() || test_trim_once())
    {
        return 1;
    }

    std::cout << "test_signal_dispatcher: " << g_checks << " checks passed\n";
    return 0;
}
