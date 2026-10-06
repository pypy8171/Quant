// 급등 뒤 되돌림 보유 슬리브(strategy/SurgeHoldPlan.h·SurgeHoldStrategy.h) 단위 테스트 — 엔진 없이 원장·매도가능·국면·
//  신규매수 차단·종목 id·시각을 std::function으로 대신한다. Logger를 링크한다. 관련 결정: D-157.
// 빌드: cmake --build <directory> --target test_surge_hold
//
// 테스트 항목:
//   1. 계획 파일 파싱 — 정상, schema·action·BUY_OPEN 필드(d0 포함)가 틀리면 파일 전체를 버림, 거래대금 비중 큰 순 정렬
//   2. 계획 신선도 — 신호일이 오늘 앞이고 달력 5일 안. 평일 수 세기
//   3. 손절·익절 가격과 판정 — 손절 = 기준 저가 × 0.97, 익절 = 매수가 × 1.15
//   4. 건너뜀 판정 — 약세장·차단·같은 신호·겹침(보유·매수 중)·바스켓·보유 상한·하루 상한·금액 부족
//   5. 매수 창 — 08:49 없음, 08:50 동시호가 매수(표시·수량·주문 번호), 겹침 종목 건너뜀, 하루 3건 상한
//   6. 보유 상한 4 — 이미 3종목이면 1건만
//   7. 약세장(국면 게이트 꺼짐) — 매수 없음. 국면 파일을 모르면 보류했다가 알게 되면 산다
//   8. 체결 → 실제 매수가로 익절 재계산, 손절·익절 틱에 매도 한 번, 장부 0이면 청산 완료
//   9. 재기동 — 상태 파일로 보유·오늘 판정 복원, 같은 매수를 다시 내지 않음
//  10. 만기(EXIT_CLOSE) — 15:20 종가 동시호가 매도. 계획이 없으면 평일 130일 백스톱
//  11. dry_run — 신호 0, 상태 파일 보유 0, 슬롯 면제 없음
//  12. 같은 계획 다음 날 — 다시 사지 않음(C-1), 같은 급등 신호 재매수 막음
//  13. 매도 창 밖 손절 — 다음 날 09:00 뒤 매도, 어제 다 못 판 매도 재개(C-2), 시가 갭 하락 즉시 매도(W-1)
//  14. 09:30 미체결 — 취소 주문, 선점이 0이 될 때까지 PENDING 유지(W-5)
//  15. 상태 파일 깨짐 — 매수 없음, 파일 그대로(W-4). 슬롯 면제 전송 실패 — 다음 판에 다시 보냄(W-6)
#include "strategy/SurgeHoldStrategy.h"
#include "core/KstTime.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace
{
int g_checks = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        ++g_checks;                                                                             \
        if (!(condition))                                                                       \
        {                                                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition << "\n";     \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

const std::filesystem::path kDirectory = std::filesystem::temp_directory_path() / "quant_test_surge_hold";
const std::string           kPlan      = (kDirectory / "plan.json").string();
const std::string           kState     = (kDirectory / "state.json").string();

// 오늘(KST) 날짜의 hh:mm에 해당하는 time_t.
std::time_t today_at(int hour, int minute)
{
    const std::time_t now         = std::time(nullptr);
    const auto        time_of_day = kst::time_of_day(now);
    const std::time_t midnight    = now - static_cast<std::time_t>(time_of_day.to_duration().count());
    return midnight + hour * 3600 + minute * 60;
}

std::string dashed(const std::string& yyyymmdd)
{
    return yyyymmdd.substr(0, 4) + "-" + yyyymmdd.substr(4, 2) + "-" + yyyymmdd.substr(6, 2);
}

std::string yesterday()
{
    return kst::date_yyyymmdd(today_at(12, 0) - 86400);
}

nlohmann::json buy_row(const std::string& ticker)
{
    return {{"ticker", ticker}, {"action", "BUY_OPEN"}, {"signal_date", dashed(yesterday())}, {"d0", dashed(yesterday())}, {"stop_basis_low", 9000.0},
            {"stop_pct", 3}, {"take_pct", 15}, {"horizon_days", 120}, {"reference_close", 10000.0}};
}

void write_plan(const std::string& as_of, const std::vector<nlohmann::json>& rows)
{
    nlohmann::json document;
    document["schema"]       = 1;
    document["generated_at"] = "t1";
    document["as_of"]        = dashed(as_of);
    document["rule"]         = "c1";
    document["count"]        = rows.size();
    document["rows"]         = rows;
    std::ofstream(kPlan) << document.dump(2);
}

struct Rig
{
    std::map<std::string, int>         positions; // 원장 보유(테스트가 직접 바꿔 체결을 흉내 낸다)
    std::map<std::string, double>      averages;
    std::map<std::string, int>         reserved;  // 미체결 선점 순값(매수 − 매도)
    std::vector<std::string>           owned;
    bool                               halted     = false;
    bool                               sink_fails = false; // 슬롯 면제 전송 실패를 흉내 낸다
    std::time_t                        now    = 0;
    int                                day    = 0; // 오늘에서 며칠 뒤인가(다음 거래일을 흉내 낸다)
    std::vector<OrderSignal>           out;
    std::unique_ptr<SurgeHoldStrategy> strategy;

    explicit Rig(bool dry_run = false, int max_positions = 4)
    {
        now = today_at(8, 0); // on_start가 오늘 날짜로 하루를 연다
        SurgeHoldStrategy::Params parameters;
        parameters.plan_file        = kPlan;
        parameters.state_file       = kState;
        parameters.amount_krw       = 5'000'000.0;
        parameters.max_positions    = max_positions;
        parameters.max_daily_buys   = 3;
        parameters.reload_sec       = 0;
        parameters.dry_run          = dry_run;
        parameters.excluded_tickers = {"BASKET1"};
        strategy = std::make_unique<SurgeHoldStrategy>(parameters, [this](const std::vector<std::string>& tickers)
        {
            if (sink_fails)
            {
                return false;
            }

            owned = tickers;
            return true;
        });
        strategy->set_clock([this]
        {
            return now;
        });
        strategy->set_position_provider([this](const std::string&, const std::string& ticker)
        {
            const auto found = positions.find(ticker);
            return found == positions.end() ? 0 : found->second;
        });
        strategy->set_sellable_provider([this](const std::string&, const std::string& ticker)
        {
            StrategyBase::SellableInfo info;
            const auto found   = positions.find(ticker);
            const auto average = averages.find(ticker);
            info.sellable      = found == positions.end() ? 0 : found->second;
            info.average_price = average == averages.end() ? 0.0 : average->second;
            const auto pending = reserved.find(ticker);
            info.reserved      = pending == reserved.end() ? 0 : pending->second;
            return info;
        });
        strategy->set_entry_halt_provider([this]
        {
            return halted;
        });
        strategy->set_symbol_resolver([](std::string_view ticker)
        {
            return static_cast<symbol::SymbolId>(std::hash<std::string_view>{}(ticker) % 1000 + 1);
        });
        strategy->load_state();
        strategy->on_start();
    }

    void clock_at(int hour, int minute)
    {
        now = today_at(hour, minute) + static_cast<std::time_t>(day) * 86400;
        strategy->on_clock(out);
    }

    void trade(const std::string& ticker, double price)
    {
        TradeData data;
        data.symbol_id = static_cast<symbol::SymbolId>(std::hash<std::string_view>{}(ticker) % 1000 + 1);
        data.price     = price;
        strategy->on_trade_batch(data, out);
    }

    const SurgeHoldStrategy::Position* position(const std::string& ticker) const
    {
        for (const auto& held : strategy->positions())
        {
            if (held.ticker == ticker)
            {
                return &held;
            }
        }

        return nullptr;
    }
};

void reset_files()
{
    std::filesystem::remove_all(kDirectory);
    std::filesystem::create_directories(kDirectory);
}

int test_parse()
{
    std::string reason;
    nlohmann::json good = {{"schema", 1}, {"as_of", "2026-10-05"}, {"rule", "c1"},
                           {"rows", nlohmann::json::array({buy_row("A"), {{"ticker", "H"}, {"action", "EXIT_CLOSE"}, {"held_days", 120}}})}};
    const auto plan = surge::parse_plan(good, reason);
    CHECK(plan && plan->rows.size() == 2);
    CHECK(plan->rows[0].action == surge::Action::BuyOpen && plan->rows[0].stop_basis_low == 9000.0 && plan->rows[0].take_percent == 15.0);
    CHECK(plan->rows[1].action == surge::Action::ExitClose && plan->rows[1].held_days == 120);

    auto bad_schema      = good;
    bad_schema["schema"] = 2;
    CHECK(!surge::parse_plan(bad_schema, reason));

    auto bad_action                 = good;
    bad_action["rows"][1]["action"] = "SELL_NOW";
    CHECK(!surge::parse_plan(bad_action, reason));

    auto bad_buy                         = good;
    bad_buy["rows"][0]["stop_basis_low"] = 0;
    CHECK(!surge::parse_plan(bad_buy, reason)); // 행 하나가 틀리면 파일 전체를 버린다

    auto no_d0 = good;
    no_d0["rows"][0].erase("d0");
    CHECK(!surge::parse_plan(no_d0, reason));

    auto late_d0             = good;
    late_d0["rows"][0]["d0"] = "2099-01-01";
    CHECK(!surge::parse_plan(late_d0, reason)); // 급등일이 신호일 뒤

    // 매수 행은 앞으로, 거래대금 비중 큰 순으로 — 상한을 넘으면 앞에서부터 산다(파이썬 emit_plan과 같은 순서).
    auto small                = buy_row("S");
    small["turnover_share"]   = 0.1;
    auto large                = buy_row("L");
    large["turnover_share"]   = 0.5;
    nlohmann::json shuffled   = good;
    shuffled["rows"]          = nlohmann::json::array({{{"ticker", "H"}, {"action", "EXIT_CLOSE"}, {"held_days", 120}}, small, large});
    const auto ordered        = surge::parse_plan(shuffled, reason);
    CHECK(ordered && ordered->rows[0].ticker == "L" && ordered->rows[1].ticker == "S" && ordered->rows[2].ticker == "H");

    CHECK(surge::date_number("2026-10-05") == 20261005 && surge::date_number("20261005") == 20261005 && surge::date_number("26-10-05") == 0);
    return 0;
}

int test_fresh()
{
    CHECK(surge::plan_fresh(20261002, 20261005));  // 금 → 월
    CHECK(!surge::plan_fresh(20261005, 20261005)); // 같은 날(장 마감 뒤 쓴 계획은 다음 날 몫)
    CHECK(!surge::plan_fresh(20260928, 20261005)); // 일주일 묵음
    CHECK(surge::plan_fresh(20261231, 20270102)); // 해 넘김
    CHECK(surge::weekdays_between(20261002, 20261005) == 1);  // 금 → 월: 월요일 하루
    CHECK(surge::weekdays_between(20261005, 20261005) == 0);
    CHECK(surge::weekdays_between(20261005, 20261012) == 5);
    CHECK(surge::weekdays_between(20261230, 20270104) == 3);  // 해 넘김: 31·1·4
    return 0;
}

int test_prices()
{
    CHECK(surge::stop_price(10000.0, 3.0) == 9700.0);
    CHECK(std::abs(surge::take_price(10500.0, 15.0) - 12075.0) < 1e-6);
    CHECK(surge::exit_check(9700.0, 9700.0, 12075.0) == surge::Exit::Stop);
    CHECK(surge::exit_check(12075.0, 9700.0, 12075.0) == surge::Exit::Take);
    CHECK(surge::exit_check(10000.0, 9700.0, 12075.0) == surge::Exit::None);
    CHECK(surge::exit_check(0.0, 9700.0, 12075.0) == surge::Exit::None);
    CHECK(surge::buy_quantity(5'000'000.0, 10000.0) == 500 && surge::buy_quantity(5'000'000.0, 0.0) == 0);
    return 0;
}

int test_skip()
{
    surge::EntryCheck check;
    check.max_positions  = 4;
    check.max_daily_buys = 3;
    check.quantity       = 10;
    CHECK(surge::entry_skip(check) == surge::Skip::None);

    auto risk_off   = check;
    risk_off.active = false;
    CHECK(surge::entry_skip(risk_off) == surge::Skip::RiskOff);

    auto halted         = check;
    halted.entry_halted = true;
    CHECK(surge::entry_skip(halted) == surge::Skip::EntryHalted);

    auto overlap             = check;
    overlap.account_position = 7;
    CHECK(surge::entry_skip(overlap) == surge::Skip::Overlap);

    auto buying             = check;
    buying.account_reserved = 30; // 다른 슬리브가 사는 중(미체결)
    CHECK(surge::entry_skip(buying) == surge::Skip::Overlap);

    auto same        = check;
    same.same_signal = true;
    CHECK(surge::entry_skip(same) == surge::Skip::SameSignal);

    auto excluded     = check;
    excluded.excluded = true;
    CHECK(surge::entry_skip(excluded) == surge::Skip::Excluded);

    auto full           = check;
    full.open_positions = 4;
    CHECK(surge::entry_skip(full) == surge::Skip::MaxPositions);

    auto daily         = check;
    daily.bought_today = 3;
    CHECK(surge::entry_skip(daily) == surge::Skip::MaxDailyBuys);

    auto poor     = check;
    poor.quantity = 0;
    CHECK(surge::entry_skip(poor) == surge::Skip::NoPrice);
    return 0;
}

int test_buy_window()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A"), buy_row("B"), buy_row("BASKET1"), buy_row("C"), buy_row("D"), buy_row("E")});
    Rig rig;
    rig.positions["B"] = 10; // 다른 슬리브가 보유
    CHECK(rig.owned.size() == 5); // 후보 다섯(바스켓 종목 제외)이 슬롯 면제로 올라간다

    rig.clock_at(8, 49);
    CHECK(rig.out.empty());

    rig.clock_at(8, 50);
    CHECK(rig.out.size() == 3); // A·C·D — B 겹침, BASKET1 바스켓, E 하루 3건 상한
    CHECK(rig.out[0].ticker == "A" && rig.out[1].ticker == "C" && rig.out[2].ticker == "D");

    for (const auto& signal : rig.out)
    {
        CHECK(signal.side == OrderSide::BUY && signal.type == OrderType::MARKET && signal.quantity == 500);
        CHECK(signal.opening_auction && signal.exempt_from_age_limit && signal.reference_price == 10000.0);
        CHECK(signal.strategy_id == "SURGE_MAIN" && signal.client_order_number != 0);
    }

    rig.out.clear();
    rig.clock_at(8, 55);
    CHECK(rig.out.empty()); // 판정은 하루 한 번
    CHECK(rig.strategy->positions().size() == 3 && rig.position("A")->status == SurgeHoldStrategy::Status::Pending);
    return 0;
}

int test_max_positions()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A"), buy_row("B"), buy_row("C")});
    {
        Rig rig(false, 2);
        rig.clock_at(8, 50);
        CHECK(rig.out.size() == 2); // 상한 2에서 두 건만
    }

    reset_files();
    write_plan(yesterday(), {buy_row("A"), buy_row("B")});
    {
        // 보유 3종목이 상태 파일에 있으면 상한 4에서 한 건만.
        nlohmann::json state = {{"schema", 1}, {"positions", nlohmann::json::array()}};

        for (const char* ticker : {"H1", "H2", "H3"})
        {
            state["positions"].push_back({{"ticker", ticker}, {"entry_date", "20260901"}, {"quantity", 10}, {"entry_price", 10000.0},
                                          {"stop_price", 9000.0}, {"take_price", 11500.0}, {"status", "OPEN"}});
        }

        std::ofstream(kState) << state.dump(2);
        Rig rig;
        rig.positions = {{"H1", 10}, {"H2", 10}, {"H3", 10}};
        rig.clock_at(8, 50);
        CHECK(rig.out.size() == 1 && rig.out[0].ticker == "A");
    }

    return 0;
}

int test_risk_off()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig rig;
    rig.strategy->set_active(false); // RISK_OFF — regime_strategies에 SURGE_*가 없다
    rig.clock_at(8, 50);
    CHECK(rig.out.empty() && rig.strategy->positions().empty());

    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig halted_rig;
    halted_rig.halted = true;
    halted_rig.clock_at(8, 50);
    CHECK(halted_rig.out.empty());
    return 0;
}

int test_fill_and_exits()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A"), buy_row("C")});
    Rig rig;
    rig.clock_at(8, 50);
    CHECK(rig.out.size() == 2);
    rig.out.clear();

    // 09:00 시가 체결 — 장부에 보유·평단이 잡힌다.
    rig.positions["A"] = 500;
    rig.averages["A"]  = 9500.0;
    rig.positions["C"] = 500;
    rig.averages["C"]  = 10200.0;
    rig.clock_at(9, 1);
    const auto* filled = rig.position("A");
    CHECK(filled && filled->status == SurgeHoldStrategy::Status::Open && filled->quantity == 500 && filled->entry_price == 9500.0);
    CHECK(std::abs(filled->stop - 8730.0) < 1e-6);                 // 9000 × 0.97
    CHECK(std::abs(filled->take - 10925.0) < 1e-6);                // 9500 × 1.15

    rig.trade("A", 8800.0);
    CHECK(rig.out.empty());
    rig.trade("A", 8730.0);
    CHECK(rig.out.size() == 1 && rig.out[0].side == OrderSide::SELL && rig.out[0].quantity == 500);
    CHECK(rig.out[0].reason.find("손절") != std::string::npos);
    rig.trade("A", 8700.0);
    CHECK(rig.out.size() == 1); // 같은 손절을 두 번 내지 않는다

    rig.trade("C", 11800.0); // 익절가 10200 × 1.15 = 11730 위
    CHECK(rig.out.size() == 2 && rig.out[1].ticker == "C" && rig.out[1].reason.find("익절") != std::string::npos);

    // 매도 체결 — 장부 0이면 청산 끝, 보유에서 빠진다.
    rig.positions["A"] = 0;
    rig.clock_at(9, 30);
    CHECK(rig.position("A") == nullptr && rig.position("C") != nullptr);

    // 매도가 남았으면 60초 뒤 다시 낸다.
    rig.out.clear();
    rig.now += 61;
    rig.strategy->on_clock(rig.out);
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "C");
    return 0;
}

int test_restart()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    {
        Rig rig;
        rig.clock_at(8, 50);
        CHECK(rig.out.size() == 1);
        rig.positions["A"] = 500;
        rig.averages["A"]  = 10100.0;
        rig.clock_at(9, 2);
    }

    Rig restarted;
    restarted.positions["A"] = 500;
    restarted.averages["A"]  = 10100.0;
    restarted.clock_at(8, 51);
    CHECK(restarted.out.empty()); // 오늘 판정을 끝낸 종목은 다시 안 산다
    restarted.clock_at(9, 5);
    const auto* restored = restarted.position("A");
    CHECK(restored && restored->status == SurgeHoldStrategy::Status::Open && restored->entry_price == 10100.0 && std::abs(restored->stop - 8730.0) < 1e-6);
    CHECK(restarted.owned.size() == 1 && restarted.owned[0] == "A");
    restarted.trade("A", 8000.0);
    CHECK(restarted.out.size() == 1 && restarted.out[0].side == OrderSide::SELL);
    return 0;
}

int test_exit_close()
{
    reset_files();
    nlohmann::json state = {{"schema", 1},
                            {"positions", nlohmann::json::array({{{"ticker", "H"}, {"entry_date", "20260401"}, {"quantity", 300},
                                                                  {"entry_price", 10000.0}, {"stop_price", 8000.0},
                                                                  {"take_price", 11500.0}, {"status", "OPEN"}}})}};
    std::ofstream(kState) << state.dump(2);
    write_plan(yesterday(), {{{"ticker", "H"}, {"action", "EXIT_CLOSE"}, {"held_days", 120}, {"entry_date", "2026-04-01"}}});
    Rig rig;
    rig.positions["H"] = 300;
    rig.clock_at(15, 19);
    CHECK(rig.out.empty());
    rig.clock_at(15, 20);
    CHECK(rig.out.size() == 1 && rig.out[0].side == OrderSide::SELL && rig.out[0].quantity == 300);
    CHECK(rig.out[0].reason.find("만기") != std::string::npos);
    return 0;
}

int test_dry_run()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig rig(true);
    rig.clock_at(8, 50);
    CHECK(rig.out.empty() && rig.strategy->positions().empty());
    CHECK(rig.owned.empty()); // 주문을 안 내니 다른 슬리브의 슬롯 계산을 건드리지 않는다
    return 0;
}

// C-1 같은 as_of 계획이 이틀 이어지면(17:00 계획 생성이 실패한 날) 둘째 날은 사지 않는다. 새 as_of면 다시 산다.
int test_same_plan_next_day()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig rig;
    rig.clock_at(8, 50);
    CHECK(rig.out.size() == 1);
    rig.clock_at(9, 30); // 체결 없음, 선점도 0 — 미체결로 지운다
    CHECK(rig.strategy->positions().empty());

    rig.out.clear();
    rig.day = 1;
    rig.clock_at(8, 50);
    CHECK(rig.out.empty()); // 이미 집행한 계획

    write_plan(kst::date_yyyymmdd(today_at(12, 0)), {buy_row("B")});
    rig.clock_at(8, 51);
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "B");
    return 0;
}

// C-2 장 마감 뒤(15:31) 체결로 손절선을 밟으면 그 자리에서는 주문을 내지 않고 다음 날 09:00 뒤에 판다.
int test_exit_outside_window()
{
    reset_files();
    nlohmann::json state = {{"schema", 1},
                            {"positions", nlohmann::json::array({{{"ticker", "H"}, {"entry_date", "20260901"}, {"quantity", 300},
                                                                  {"entry_price", 10000.0}, {"stop_basis_low", 8247.0},
                                                                  {"stop_pct", 3}, {"take_pct", 15}, {"stop_price", 8000.0},
                                                                  {"take_price", 11500.0}, {"status", "OPEN"}}})}};
    std::ofstream(kState) << state.dump(2);
    write_plan(yesterday(), {{{"ticker", "H"}, {"action", "HOLD"}, {"held_days", 20}, {"entry_date", "2026-09-01"}}});
    Rig rig;
    rig.positions["H"] = 300;
    rig.clock_at(15, 31);
    rig.trade("H", 7900.0);
    CHECK(rig.out.empty());
    rig.clock_at(15, 40);
    CHECK(rig.out.empty());

    rig.day = 1;
    rig.clock_at(8, 55);
    CHECK(rig.out.empty()); // 장 시작 전에도 내지 않는다
    rig.clock_at(9, 1);
    CHECK(rig.out.size() == 1 && rig.out[0].side == OrderSide::SELL && rig.out[0].quantity == 300);
    return 0;
}

// C-2 매도를 세 번 내도 남으면 그날은 멈추고, 다음 날 시도 횟수를 0으로 되돌려 다시 판다.
int test_exiting_next_day()
{
    reset_files();
    nlohmann::json state = {{"schema", 1},
                            {"positions", nlohmann::json::array({{{"ticker", "H"}, {"entry_date", "20260901"}, {"quantity", 300},
                                                                  {"entry_price", 10000.0}, {"stop_basis_low", 8247.0},
                                                                  {"stop_pct", 3}, {"take_pct", 15}, {"stop_price", 8000.0},
                                                                  {"take_price", 11500.0}, {"status", "OPEN"}}})}};
    std::ofstream(kState) << state.dump(2);
    write_plan(yesterday(), {});
    Rig rig;
    rig.positions["H"] = 300;
    rig.clock_at(10, 0);
    rig.trade("H", 7900.0);
    CHECK(rig.out.size() == 1);

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        rig.now += 61;
        rig.strategy->on_clock(rig.out);
    }

    CHECK(rig.out.size() == 3); // 최대 3회
    rig.out.clear();
    rig.day = 1;
    rig.clock_at(9, 1);
    CHECK(rig.out.size() == 1 && rig.out[0].side == OrderSide::SELL);
    return 0;
}

// W-1 시가가 손절선 아래로 갭 하락해 체결되면 09:00 뒤 바로 판다(백테스트는 그날 손절로 친다).
int test_gap_below_stop()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig rig;
    rig.clock_at(8, 50);
    CHECK(rig.out.size() == 1);
    rig.out.clear();
    rig.positions["A"] = 500;
    rig.averages["A"]  = 8600.0; // 손절선 9000 × 0.97 = 8730 아래
    rig.clock_at(9, 0);
    const auto* filled = rig.position("A");
    CHECK(filled && std::abs(filled->stop - 8730.0) < 1e-6);
    CHECK(rig.out.size() == 1 && rig.out[0].side == OrderSide::SELL && rig.out[0].quantity == 500);
    return 0;
}

// 국면 파일: 08:50에는 엔진의 국면 선택이 아직 어제 값이라 파일을 직접 본다. 모르면 보류, 알게 되면 산다.
int test_regime_file()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    std::optional<bool> regime;
    Rig rig;
    rig.strategy->set_regime_check([&regime]
    {
        return regime;
    });
    rig.clock_at(8, 50);
    CHECK(rig.out.empty() && rig.strategy->positions().empty()); // 모름 — 판정하지 않고 보류
    regime = true;
    rig.clock_at(8, 52);
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "A");

    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig risk_off;
    risk_off.strategy->set_regime_check([]
    {
        return std::optional<bool>(false);
    });
    risk_off.clock_at(8, 50);
    CHECK(risk_off.out.empty() && risk_off.strategy->positions().empty());
    return 0;
}

// 같은 급등 신호(종목·d0)는 청산 뒤에도 다시 사지 않는다. 다른 슬리브가 사는 중(미체결)인 종목도 건너뛴다.
int test_same_signal_and_reserved()
{
    reset_files();
    nlohmann::json state = {{"schema", 1}, {"positions", nlohmann::json::array()},
                            {"closed", nlohmann::json::array({{{"ticker", "A"}, {"d0", dashed(yesterday())}}})}};
    std::ofstream(kState) << state.dump(2);
    write_plan(yesterday(), {buy_row("A"), buy_row("B"), buy_row("C")});
    Rig rig;
    rig.reserved["B"] = 40;
    rig.clock_at(8, 50);
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "C");
    return 0;
}

// 09:30까지 안 잡힌 매수는 취소 주문을 내고, 선점이 0이 될 때까지 PENDING을 지우지 않는다.
int test_unfilled_cancel()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig rig;
    rig.clock_at(8, 50);
    CHECK(rig.out.size() == 1);
    const uint64_t order_number = rig.out[0].client_order_number;
    rig.out.clear();
    rig.reserved["A"] = 500;
    rig.clock_at(9, 30);
    CHECK(rig.out.size() == 1 && rig.out[0].action == OrderAction::CANCEL && rig.out[0].original_client_order_number == order_number);
    CHECK(rig.position("A") && rig.position("A")->status == SurgeHoldStrategy::Status::Pending);
    rig.clock_at(9, 31);
    CHECK(rig.out.size() == 1); // 취소는 한 번
    rig.reserved["A"] = 0;
    rig.clock_at(9, 32);
    CHECK(rig.position("A") == nullptr);
    return 0;
}

// 상태 파일이 있는데 못 읽으면 매수를 막고 그 파일을 덮어쓰지 않는다.
int test_broken_state()
{
    reset_files();
    const std::string broken = "{\"positions\": [ {\"ticker\": ";
    std::ofstream(kState) << broken;
    write_plan(yesterday(), {buy_row("A")});
    {
        Rig rig;
        rig.clock_at(8, 50);
        rig.clock_at(8, 55);
        CHECK(rig.out.empty() && rig.strategy->positions().empty());
        rig.strategy->on_stop();
    }

    std::ifstream     stream(kState);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    CHECK(buffer.str() == broken);
    return 0;
}

// 슬롯 면제 전송이 실패하면 지난 목록을 그대로 두고 다음 판에 다시 보낸다.
int test_owned_sink_retry()
{
    reset_files();
    write_plan(yesterday(), {buy_row("A")});
    Rig rig;
    CHECK(rig.owned.size() == 1 && rig.owned[0] == "A"); // 오늘 매수 후보
    rig.reserved["A"] = 10; // 다른 슬리브가 사는 중 — 건너뛰면 후보에서 빠진다
    rig.sink_fails    = true;
    rig.clock_at(8, 50);
    CHECK(rig.out.empty() && rig.owned.size() == 1); // 못 보냄
    rig.sink_fails = false;
    rig.clock_at(8, 51);
    CHECK(rig.owned.empty()); // 다음 판에 다시 보냄
    return 0;
}

// 계획이 없는 날 — 매수일부터 평일 130일이 지난 보유는 종가 창에 판다.
int test_backstop()
{
    reset_files();
    nlohmann::json state = {{"schema", 1},
                            {"positions", nlohmann::json::array({{{"ticker", "H"}, {"entry_date", "20260102"}, {"quantity", 300},
                                                                  {"entry_price", 10000.0}, {"stop_price", 5000.0},
                                                                  {"take_price", 20000.0}, {"status", "OPEN"}},
                                                                 {{"ticker", "N"}, {"entry_date", kst::date_yyyymmdd(today_at(12, 0) - 7 * 86400)},
                                                                  {"quantity", 100}, {"entry_price", 10000.0}, {"stop_price", 5000.0},
                                                                  {"take_price", 20000.0}, {"status", "OPEN"}}})}};
    std::ofstream(kState) << state.dump(2);
    Rig rig; // 계획 파일 없음
    rig.positions = {{"H", 300}, {"N", 100}};
    rig.clock_at(15, 10);
    CHECK(rig.out.empty());
    rig.clock_at(15, 20);
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "H" && rig.out[0].reason.find("백스톱") != std::string::npos);
    return 0;
}
} // namespace

int main()
{
    if (const char* environment = std::getenv("QUANT_LOG_DIR"); !environment || !*environment)
    {
        Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");
    }

    const int failed = test_parse() || test_fresh() || test_prices() || test_skip() || test_buy_window() || test_max_positions() ||
                       test_risk_off() || test_fill_and_exits() || test_restart() || test_exit_close() || test_dry_run() || test_same_plan_next_day() ||
                       test_exit_outside_window() || test_exiting_next_day() || test_gap_below_stop() || test_regime_file() ||
                       test_same_signal_and_reserved() || test_unfilled_cancel() || test_broken_state() || test_owned_sink_retry() ||
                       test_backstop();
    std::filesystem::remove_all(kDirectory);

    if (failed)
    {
        return EXIT_FAILURE;
    }

    std::cout << "[PASS] test_surge_hold (" << g_checks << " checks)\n";
    return EXIT_SUCCESS;
}
