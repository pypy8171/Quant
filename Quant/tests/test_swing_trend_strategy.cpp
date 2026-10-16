// 스윙 추세 슬리브 본체(strategy/SwingTrendStrategy.h) 단위 테스트 — 엔진 없이 장부 보유·미체결 선점·신규매수 차단·시각·
//  시가·현재가를 std::function으로 대신한다. 근접 발주(2026-10-16 후속, D-160)를 고정한다.
// 빌드: cmake --build <directory> --target test_swing_trend_strategy
//
// 테스트 항목:
//   1. 시가 통과 — 주문 0건, 매수 대기에 들어감. 시가가 매수가 이하인 종목은 대기에도 넣지 않음
//   2. 현재가가 발주선 이하로 다가옴 — 매수가 지정가 1건, 대기에서 빠짐
//   3. 멀어짐 — 취소 1건(같은 주문 번호), 선점이 풀리기 전에는 다시 내지 않음, 풀리면 대기 복귀, 다시 다가오면 다시 냄
//   4. 부분 체결 — 멀어져도 취소하지 않음
//   5. 재기동 — 상태 파일로 대기 복원, 이전 exe가 낸 오늘 매수가 멀면 취소
//   6. 15:19 — 다가와도 내지 않음, 대기 해제, 남은 매수는 미체결 취소
//   7. 계좌 신규매수 차단 — 다가와도 내지 않고 풀리면 냄
#include "strategy/SwingTrendStrategy.h"
#include "utils/Logger.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

const std::filesystem::path kDirectory = std::filesystem::temp_directory_path() / "quant_test_swing_trend_strategy";
const std::string           kPlan      = (kDirectory / "plan.json").string();
const std::string           kState     = (kDirectory / "state.json").string();
constexpr int64_t           kEntryPrice = 10'000; // 발주선 10,100(+1%)·취소선 10,200(+2%)
constexpr int               kQuantity   = 50;     // 500,000원 / 10,000원

// 2026-10-16(금) KST hh:mm — 평일로 고정해 주말에 돌려도 prefetch가 멈추지 않는다.
std::time_t friday_at(int hour, int minute)
{
    constexpr std::chrono::sys_days kDay{std::chrono::year{2026} / std::chrono::October / std::chrono::day{16}};
    const auto                      utc_midnight = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point{kDay});
    return utc_midnight + hour * 3600 + minute * 60 - 9 * 3600;
}

void reset_files()
{
    std::filesystem::remove_all(kDirectory);
    std::filesystem::create_directories(kDirectory);
    nlohmann::json document;
    document["as_of"]   = "2026-10-15";
    document["entries"] = nlohmann::json::array({{{"ticker", "111111"}, {"name", "가"}, {"entry_price", kEntryPrice}, {"cancel_below", 9'000}},
                                                 {{"ticker", "222222"}, {"name", "나"}, {"entry_price", kEntryPrice}, {"cancel_below", 9'000}}});
    std::ofstream(kPlan) << document.dump(2);
}

struct Rig
{
    std::map<std::string, int>          positions; // 장부 보유(시험이 바꿔 체결을 흉내 낸다)
    std::map<std::string, int>          reserved;  // 미체결 선점(매수 주문 접수·취소 확인을 흉내 낸다)
    std::map<std::string, double>       opens;
    std::map<std::string, double>       prices;
    bool                                halted = false;
    std::time_t                         now    = friday_at(8, 50);
    std::vector<OrderSignal>            out;
    std::unique_ptr<SwingTrendStrategy> strategy;

    Rig()
    {
        SwingTrendStrategy::Params parameters;
        parameters.account    = "ACC";
        parameters.plan_file  = kPlan;
        parameters.state_file = kState;
        parameters.reload_sec = 0;
        strategy = std::make_unique<SwingTrendStrategy>(parameters, [](const std::vector<std::string>&)
        {
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
            const auto                 held    = positions.find(ticker);
            const auto                 pending = reserved.find(ticker);
            info.sellable                      = held == positions.end() ? 0 : held->second;
            info.reserved                      = pending == reserved.end() ? 0 : pending->second;
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
        SwingTrendStrategy::QuoteSource source;
        source.open = [this](const std::string& ticker)
        {
            const auto found = opens.find(ticker);
            return found == opens.end() ? 0.0 : found->second;
        };
        source.prices = [this](const std::vector<std::string>& tickers)
        {
            std::vector<double> result;

            for (const auto& ticker : tickers)
            {
                const auto found = prices.find(ticker);
                result.push_back(found == prices.end() ? 0.0 : found->second);
            }

            return std::optional<std::vector<double>>(std::move(result));
        };
        strategy->set_quote_source(std::move(source));
        strategy->load_state();
        strategy->on_start();
    }

    // 엔진의 한 주기를 흉내 낸다 — 요청 게시(on_clock) → REST 받기(prefetch) → 판단(on_clock).
    void step(int hour, int minute)
    {
        now = friday_at(hour, minute);
        out.clear();
        strategy->on_clock(out);
        strategy->prefetch_once();
        strategy->on_clock(out);
    }

    int count(OrderAction action, OrderSide side) const
    {
        int total = 0;

        for (const auto& signal : out)
        {
            if (signal.action == action && signal.side == side)
            {
                ++total;
            }
        }

        return total;
    }

    const SwingTrendStrategy::Position* position(const std::string& ticker) const
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

int test_arm_then_near_order()
{
    reset_files();
    Rig rig;
    rig.opens  = {{"111111", 10'500.0}, {"222222", 9'900.0}};
    rig.prices = {{"111111", 10'500.0}};
    rig.step(9, 0);
    CHECK(rig.out.empty());                           // 시가가 매수가 위여도 바로 내지 않는다
    CHECK(rig.strategy->armed_tickers().count("111111") == 1);
    CHECK(rig.strategy->armed_tickers().count("222222") == 0); // 시가 9,900 ≤ 매수가 — 오늘은 사지 않음
    CHECK(rig.strategy->positions().empty());

    rig.step(9, 1);
    CHECK(rig.out.empty());                           // 10,500 > 발주선 10,100

    rig.prices["111111"] = 10'100.0;
    rig.step(9, 2);
    CHECK(rig.out.size() == 1);
    CHECK(rig.count(OrderAction::NEW, OrderSide::BUY) == 1);
    CHECK(rig.out[0].type == OrderType::LIMIT);
    CHECK(rig.out[0].price == static_cast<double>(kEntryPrice));
    CHECK(rig.out[0].quantity == kQuantity);
    CHECK(rig.strategy->armed_tickers().empty());
    CHECK(rig.position("111111") != nullptr);
    CHECK(rig.position("111111")->status == SwingTrendStrategy::Status::Pending);
    return 0;
}

int test_far_cancel_and_rearm()
{
    reset_files();
    Rig rig;
    rig.opens  = {{"111111", 10'500.0}};
    rig.prices = {{"111111", 10'050.0}};
    rig.step(9, 0);
    rig.step(9, 1);
    CHECK(rig.count(OrderAction::NEW, OrderSide::BUY) == 1);
    const uint64_t first_order = rig.out[0].client_order_number;
    rig.reserved["111111"] = kQuantity; // 접수

    rig.prices["111111"] = 10'200.0;    // 취소선과 같은 값은 두고 본다
    rig.step(9, 5);
    CHECK(rig.out.empty());

    rig.prices["111111"] = 10'300.0;
    rig.step(9, 6);
    CHECK(rig.out.size() == 1);
    CHECK(rig.out[0].action == OrderAction::CANCEL);
    CHECK(rig.out[0].original_client_order_number == first_order);

    rig.prices["111111"] = 10'000.0;    // 취소 확인 전에 다시 다가와도 새 매수를 내지 않는다
    rig.step(9, 7);
    CHECK(rig.out.empty());
    CHECK(rig.strategy->armed_tickers().empty());

    rig.reserved["111111"] = 0;          // 취소 확인 — 선점이 풀렸다
    rig.prices["111111"]   = 10'300.0;
    rig.step(9, 8);
    CHECK(rig.out.empty());
    CHECK(rig.position("111111") == nullptr);
    CHECK(rig.strategy->armed_tickers().count("111111") == 1); // 대기 복귀

    rig.prices["111111"] = 10'090.0;
    rig.step(9, 9);
    CHECK(rig.count(OrderAction::NEW, OrderSide::BUY) == 1);
    CHECK(rig.out[0].client_order_number != first_order);
    return 0;
}

int test_partial_fill_not_cancelled()
{
    reset_files();
    Rig rig;
    rig.opens  = {{"111111", 10'500.0}};
    rig.prices = {{"111111", 10'000.0}};
    rig.step(9, 0);
    rig.step(9, 1);
    CHECK(rig.count(OrderAction::NEW, OrderSide::BUY) == 1);
    rig.positions["111111"] = 10;        // 10주 체결
    rig.reserved["111111"]  = kQuantity - 10;
    rig.prices["111111"]    = 11'000.0;
    rig.step(9, 30);
    CHECK(rig.out.empty());
    CHECK(rig.position("111111") != nullptr);
    CHECK(rig.position("111111")->status == SwingTrendStrategy::Status::Open);
    CHECK(rig.position("111111")->quantity == 10);
    return 0;
}

int test_restart_keeps_armed_and_cancels_far()
{
    reset_files();
    uint64_t order_number = 0;
    {
        Rig first;
        first.opens  = {{"111111", 10'500.0}, {"222222", 10'600.0}};
        first.prices = {{"111111", 10'000.0}, {"222222", 10'600.0}};
        first.step(9, 0);
        first.step(9, 1);
        CHECK(first.count(OrderAction::NEW, OrderSide::BUY) == 1);
        CHECK(first.out[0].ticker == "111111");
        order_number = first.out[0].client_order_number;
        CHECK(first.strategy->armed_tickers().count("222222") == 1);
    }

    Rig second;
    second.now                = friday_at(10, 0);
    second.reserved["111111"] = kQuantity; // 이전 exe의 매수가 아직 걸려 있다
    second.opens              = {{"111111", 10'500.0}, {"222222", 10'600.0}};
    second.prices             = {{"111111", 10'500.0}, {"222222", 10'600.0}};
    CHECK(second.strategy->armed_tickers().count("222222") == 1); // 상태 파일 "armed"
    CHECK(second.position("111111") != nullptr);
    second.step(10, 0);
    CHECK(second.out.size() == 1);
    CHECK(second.out[0].action == OrderAction::CANCEL);
    CHECK(second.out[0].original_client_order_number == order_number);
    return 0;
}

int test_cancel_window_releases()
{
    reset_files();
    Rig rig;
    rig.opens  = {{"111111", 10'500.0}, {"222222", 10'500.0}};
    rig.prices = {{"111111", 10'000.0}, {"222222", 10'500.0}};
    rig.step(9, 0);
    rig.step(9, 1);
    CHECK(rig.count(OrderAction::NEW, OrderSide::BUY) == 1);
    rig.reserved["111111"] = kQuantity;
    CHECK(rig.strategy->armed_tickers().count("222222") == 1);

    rig.prices["222222"] = 10'000.0;
    rig.step(15, 19);
    CHECK(rig.count(OrderAction::NEW, OrderSide::BUY) == 0);   // 15:19부터는 내지 않는다
    CHECK(rig.count(OrderAction::CANCEL, OrderSide::BUY) == 1); // 남은 매수는 미체결 취소
    CHECK(rig.strategy->armed_tickers().empty());              // 대기 해제

    rig.step(15, 20);
    CHECK(rig.out.empty());
    return 0;
}

int test_entry_halt_waits()
{
    reset_files();
    Rig rig;
    rig.opens  = {{"111111", 10'500.0}};
    rig.prices = {{"111111", 10'500.0}};
    rig.step(9, 0);
    CHECK(rig.strategy->armed_tickers().count("111111") == 1);
    rig.halted           = true;
    rig.prices["111111"] = 10'000.0;
    rig.step(9, 10);
    CHECK(rig.out.empty());
    CHECK(rig.strategy->armed_tickers().count("111111") == 1);
    rig.halted = false;
    rig.step(9, 11);
    CHECK(rig.count(OrderAction::NEW, OrderSide::BUY) == 1);
    return 0;
}
} // namespace

int main()
{
    if (test_arm_then_near_order() != 0 || test_far_cancel_and_rearm() != 0 || test_partial_fill_not_cancelled() != 0 ||
        test_restart_keeps_armed_and_cancels_far() != 0 || test_cancel_window_releases() != 0 || test_entry_halt_waits() != 0)
    {
        return 1;
    }

    std::filesystem::remove_all(kDirectory);
    std::cout << "test_swing_trend_strategy: " << g_checks << " checks passed\n";
    return 0;
}
