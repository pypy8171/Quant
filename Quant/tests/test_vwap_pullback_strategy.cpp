// 강한 종목 첫 VWAP 눌림 전략·로더(strategy/VwapPullbackStrategy.h, VwapPullbackLoader.cpp) 시험 — 꺼짐이면 아무것도
//  등록하지 않는지, 그림자 모드가 주문을 하나도 내지 않는지, 다른 슬리브 보유·청산 관리 종목에서는 신호를 막힘으로
//  적는지, 재기동 재인수가 Full만 표시하고 Partial은 청산 관리에 넘기는지, 15:10 매도 수량이 자기 몫까지인지를 고정한다.
//  시계·분봉 조회·소유 판정은 주입으로 바꾸고, 그림자 행은 임시 폴더의 csv에서 읽는다.
//  관련 결정: D-109(슬리브 소유권).
// 빌드: cmake --build <directory> --target test_vwap_pullback_strategy
#include "strategy/StrategyLoadPass.h"

#include "core/Engine.h"
#include "strategy/StrategyFactory.h"
#include "strategy/VwapPullbackStrategy.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

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

constexpr symbol::SymbolId kSymbol = 7;
const std::string          kTicker = "123450";

// 2026-10-01(목) KST 시각 → UTC epoch.
std::time_t kst_time(int hours, int minutes, int seconds = 0)
{
    using namespace std::chrono;
    constexpr int kKstOffsetHours = 9;
    const sys_seconds at = sys_days{year{2026} / October / 1} + hours * 1h + minutes * 1min + seconds * 1s -
                           kKstOffsetHours * 1h;
    return system_clock::to_time_t(at);
}

MarketData minute_bar(int hhmm, double open, double high, double low, double close, int64_t volume)
{
    constexpr int kShift = 100;
    MarketData    bar;
    bar.ticker    = kTicker;
    bar.open      = open;
    bar.high      = high;
    bar.low       = low;
    bar.close     = close;
    bar.volume    = volume;
    bar.timestamp = std::chrono::system_clock::from_time_t(kst_time(hhmm / kShift, hhmm % kShift));
    return bar;
}

// 규칙 시험(test_vwap_pullback_rules.cpp warmed_machine)과 같은 하루 — 09:31 무장, 09:32 신호(진입 10,640·손절 10,460).
std::vector<MarketData> signal_day_bars()
{
    std::vector<MarketData> bars;
    bars.push_back(minute_bar(900, 10000, 11000, 10000, 10600, 100));

    for (int minute = 1; minute <= 30; ++minute)
    {
        bars.push_back(minute_bar(900 + minute, 10550, 10600, 10450, 10550, 1000));
    }

    bars.push_back(minute_bar(931, 10550, 10600, 10500, 10520, 1000));
    bars.push_back(minute_bar(932, 10520, 10650, 10510, 10620, 1000));
    std::reverse(bars.begin(), bars.end()); // REST처럼 [0]=최신
    return bars;
}

TradeData tick_at(std::time_t when, double price)
{
    TradeData trade;
    trade.ticker      = kTicker;
    trade.symbol_id   = kSymbol;
    trade.price       = price;
    trade.quantity    = 1;
    trade.timestamp   = std::chrono::system_clock::from_time_t(when);
    trade.hhmmss      = kst::hhmmss_int(when);
    trade.received_ns = 1; // WS 틱
    return trade;
}

std::filesystem::path shadow_file(const std::string& name)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("vwappb_test_" + name + ".csv");
    std::error_code             error;
    std::filesystem::remove(path, error);
    return path;
}

std::vector<std::string> rows_with(const std::filesystem::path& path, const std::string& event)
{
    std::ifstream            file(path, std::ios::binary);
    std::vector<std::string> rows;
    std::string              line;

    while (std::getline(file, line))
    {
        if (line.find("," + event + ",") != std::string::npos)
        {
            rows.push_back(line);
        }
    }

    return rows;
}

// 쉼표로 나눈 index번째 칸.
std::string column(const std::string& line, size_t index)
{
    std::stringstream stream(line);
    std::string       field;

    for (size_t current = 0; std::getline(stream, field, ','); ++current)
    {
        if (current == index)
        {
            return field;
        }
    }

    return std::string();
}

struct Harness
{
    std::time_t                           now = kst_time(9, 33);
    std::filesystem::path                 path;
    std::unique_ptr<VwapPullbackStrategy> strategy;

    explicit Harness(const std::string& name, long long own_net_quantity = 0)
        : path(shadow_file(name))
    {
        VwapPullbackStrategy::Params parameters;
        parameters.account          = "TEST";
        parameters.ticker           = kTicker;
        parameters.name             = "시험종목";
        parameters.previous_close   = 10000.0;
        parameters.own_net_quantity = own_net_quantity;
        parameters.shadow_file      = path.string();
        strategy                    = std::make_unique<VwapPullbackStrategy>(std::move(parameters));
        strategy->set_symbol_resolver([](std::string_view)
        {
            return kSymbol;
        });
        strategy->set_clock([this]
        {
            return now;
        });
        strategy->set_minute_fetcher([](const std::string&, int)
        {
            return signal_day_bars();
        });
        strategy->set_board_vwap([](const std::string&)
        {
            return 10540.0;
        });
    }

    // 프리페치 한 번 + 틱 한 개 — 시드한 봉이 상태 기계로 들어간다. 낸 주문 수를 돌려준다.
    size_t step(double price = 10630.0)
    {
        strategy->run_prefetch_once();
        std::vector<OrderSignal> out;
        strategy->on_trade_batch(tick_at(now, price), out);
        return out.size();
    }
};

int disabled_registers_nothing()
{
    Engine          engine(KisConfig{});
    KisConfig       config;
    StrategyLoadCtx context{engine, config, config, false};
    const json      strategies = json::parse(R"([{"type": "VWAP_PULLBACK", "enabled": false, "account": "TEST"}])");
    load_strategies(context, strategies);
    CHECK(engine.strategy_count() == 0);

    // 키가 아예 없어도 꺼짐이다
    const json no_key = json::parse(R"([{"type": "VWAP_PULLBACK"}])");
    load_strategies(context, no_key);
    CHECK(engine.strategy_count() == 0);
    return 0;
}

int shadow_emits_no_order()
{
    Harness harness("shadow");
    harness.strategy->on_start();
    CHECK(harness.step() == 0);
    CHECK(harness.strategy->phase() == vwap_pullback::Phase::Done);

    const std::vector<std::string> signals = rows_with(harness.path, "SIGNAL");
    CHECK(signals.size() == 1);
    CHECK(column(signals[0], 3) == "932");
    CHECK(column(signals[0], 9) == "10640.00");  // 진입
    CHECK(column(signals[0], 10) == "10460.00"); // 손절
    CHECK(column(signals[0], 12).empty());       // 막힘 없음
    CHECK(rows_with(harness.path, "ARM").size() == 1);

    // 15:10 뒤에도 주문은 없다 — 그림자 청산 행만 남는다
    harness.now = kst_time(15, 11);
    CHECK(harness.step(10700.0) == 0);
    const std::vector<std::string> exits = rows_with(harness.path, "EXIT");
    CHECK(exits.size() == 1);
    CHECK(column(exits[0], 4) == "time");
    CHECK(rows_with(harness.path, "SUMMARY").size() == 1);

    // 15:10 뒤 REST 재확인 — 같은 봉이라 신호 분이 같다
    CHECK(harness.step(10700.0) == 0);
    const std::vector<std::string> rechecks = rows_with(harness.path, "RECHECK");
    CHECK(rechecks.size() == 1);
    CHECK(column(rechecks[0], 15) == "1");
    return 0;
}

int no_entry_when_other_holds()
{
    Harness harness("held");
    harness.strategy->set_position_provider_by_id([](const std::string&, symbol::SymbolId)
    {
        return 5; // 다른 슬리브가 이 종목을 들고 있다
    });
    harness.strategy->on_start();
    CHECK(harness.step() == 0);

    const std::vector<std::string> signals = rows_with(harness.path, "SIGNAL");
    CHECK(signals.size() == 1);
    CHECK(column(signals[0], 12) == "held");

    // 막힌 신호는 그림자 진입도 없다 — 15:10에 청산 행이 없다
    harness.now = kst_time(15, 11);
    harness.step();
    CHECK(rows_with(harness.path, "EXIT").empty());
    return 0;
}

int no_entry_on_exit_managed()
{
    Harness harness("exit_managed");
    harness.strategy->set_owner_block([](symbol::SymbolId)
    {
        return std::string("exit_managed");
    });
    harness.strategy->on_start();
    CHECK(harness.step() == 0);

    const std::vector<std::string> signals = rows_with(harness.path, "SIGNAL");
    CHECK(signals.size() == 1);
    CHECK(column(signals[0], 12) == "exit_managed");
    return 0;
}

int load_first_marks_owned_tickers()
{
    Engine          engine(KisConfig{});
    KisConfig       config;
    StrategyLoadCtx base{engine, config, config, false};
    strategy_load::LoadPass pass(base);

    strategy_load::apply_vwap_pullback_holdings(pass, {{"005930", 10}}, {{"005930", 10}});
    const symbol::SymbolId symbol = engine.symbols().lookup("005930");
    CHECK(symbol != symbol::kNone);
    CHECK(strategy_load::has_symbol(pass.basket_owned, symbol)); // DEVSCALE 유니버스에서 빠진다
    CHECK(strategy_load::has_symbol(pass.scan_covered, symbol)); // 청산 관리 부착에서 빠진다
    CHECK(pass.vwap_pullback_owned.at("005930") == 10);
    return 0;
}

int partial_holding_goes_to_itb()
{
    Engine          engine(KisConfig{});
    KisConfig       config;
    StrategyLoadCtx base{engine, config, config, false};
    strategy_load::LoadPass pass(base);

    // 장부 순수량 5주 < 잔고 10주 — 남의 몫이 섞였다
    strategy_load::apply_vwap_pullback_holdings(pass, {{"000660", 5}}, {{"000660", 10}});
    const symbol::SymbolId symbol = engine.symbols().lookup("000660");
    CHECK(!strategy_load::has_symbol(pass.basket_owned, symbol));
    CHECK(!strategy_load::has_symbol(pass.scan_covered, symbol)); // 표시하지 않아 청산 관리가 붙는다
    CHECK(pass.vwap_pullback_owned.empty());

    // 잔고가 없으면(이미 다 팔림) 아무것도 하지 않는다
    strategy_load::apply_vwap_pullback_holdings(pass, {{"035720", 5}}, {});
    CHECK(pass.vwap_pullback_owned.empty());
    return 0;
}

int exit_sells_only_own_quantity()
{
    Harness harness("own_quantity", 30);
    harness.strategy->set_position_provider_by_id([](const std::string&, symbol::SymbolId)
    {
        return 50; // 잔고 50주 중 VWAPPB 몫은 30주
    });
    harness.strategy->on_start();
    CHECK(harness.strategy->own_exit_quantity() == 30);

    harness.now = kst_time(15, 9);
    CHECK(harness.step() == 0);
    CHECK(rows_with(harness.path, "EXIT").empty()); // 15:10 전에는 팔지 않는다

    harness.now = kst_time(15, 10);
    CHECK(harness.step() == 0);
    const std::vector<std::string> exits = rows_with(harness.path, "EXIT");
    CHECK(exits.size() == 1);
    CHECK(column(exits[0], 4) == "own_holding");
    CHECK(column(exits[0], 11) == "30");
    return 0;
}

int exit_after_restart_past_1510()
{
    Harness harness("restart_late", 30);
    harness.now = kst_time(15, 20); // 15:10 뒤에 재기동했다
    harness.strategy->set_position_provider_by_id([](const std::string&, symbol::SymbolId)
    {
        return 20; // 일부는 이미 팔렸다
    });
    harness.strategy->on_start();
    CHECK(harness.step() == 0);

    const std::vector<std::string> exits = rows_with(harness.path, "EXIT");
    CHECK(exits.size() == 1);
    CHECK(column(exits[0], 11) == "20");

    // 같은 날 다시 재기동해도 청산 행을 두 번 적지 않는다
    Harness again("restart_late_again", 30);
    again.path = harness.path;
    VwapPullbackStrategy::Params parameters = harness.strategy->parameters();
    again.strategy = std::make_unique<VwapPullbackStrategy>(parameters);
    again.strategy->set_clock([]
    {
        return kst_time(15, 25);
    });
    again.strategy->set_symbol_resolver([](std::string_view)
    {
        return kSymbol;
    });
    again.strategy->on_start();
    std::vector<OrderSignal> out;
    again.strategy->on_trade_batch(tick_at(kst_time(15, 25), 10600.0), out);
    CHECK(out.empty());
    CHECK(rows_with(harness.path, "EXIT").size() == 1);
    return 0;
}

int restart_keeps_one_signal_per_day()
{
    Harness first("once_per_day");
    first.strategy->on_start();
    first.step();
    CHECK(rows_with(first.path, "SIGNAL").size() == 1);

    // 같은 그림자 파일로 재기동 — 오늘 신호는 이미 냈다
    VwapPullbackStrategy::Params parameters = first.strategy->parameters();
    auto                         second     = std::make_unique<VwapPullbackStrategy>(parameters);
    second->set_clock([]
    {
        return kst_time(9, 40);
    });
    second->set_symbol_resolver([](std::string_view)
    {
        return kSymbol;
    });
    second->set_minute_fetcher([](const std::string&, int)
    {
        return signal_day_bars();
    });
    second->on_start();
    second->run_prefetch_once();
    std::vector<OrderSignal> out;
    second->on_trade_batch(tick_at(kst_time(9, 40), 10630.0), out);
    CHECK(out.empty());
    CHECK(rows_with(first.path, "SIGNAL").size() == 1);
    return 0;
}
} // namespace

int main()
{
    int failed = 0;
    failed |= disabled_registers_nothing();
    failed |= shadow_emits_no_order();
    failed |= no_entry_when_other_holds();
    failed |= no_entry_on_exit_managed();
    failed |= load_first_marks_owned_tickers();
    failed |= partial_holding_goes_to_itb();
    failed |= exit_sells_only_own_quantity();
    failed |= exit_after_restart_past_1510();
    failed |= restart_keeps_one_signal_per_day();

    if (failed != 0)
    {
        return 1;
    }

    std::cout << "test_vwap_pullback_strategy: " << g_checks << " checks passed\n";
    return 0;
}
