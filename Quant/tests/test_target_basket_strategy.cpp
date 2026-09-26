// 목표 비중표 바스켓 전략(strategy/TargetBasketStrategy.h) 단위 테스트 — 엔진 없이 원장·매도가능·신규매수 차단·종목 id·시각을
//  std::function으로 대신해 집행 창, 두 레그, 하루 한 번 규칙(상태 파일), 낡은 파일 무시, dry_run을 고정한다. Logger를 링크한다.
//  관련 결정: D-109(바스켓 슬리브).
// 빌드: cmake --build <directory> --target test_target_basket_strategy
//
// 테스트 항목:
//   1. 창 전(14:39) — 신호 없음. as_of가 어제면 창 안에서도 없음
//   2. 창 안 — 매도 레그(DROP)가 먼저, 상태 파일에 적힌다. 같은 패스에서 매수는 안 나간다
//   3. 매수 레그 — 매도 잔량이 남아 있으면 delay 동안 대기, 지나면 예산(3)씩 낸다
//   4. 재기동 — 같은 상태 파일로 새로 만든 전략은 보낸 종목:방향을 다시 안 낸다
//   5. 창 끝(15:00) — 잔량은 안 내고 두 레그 끝 표시
//   6. dry_run — 신호 0, 상태 파일의 보낸 주문 0
//   7. 소유 종목 sink — 파일의 모든 종목(DROP 포함)이 넘어간다
#include "strategy/TargetBasketStrategy.h"
#include "core/KstTime.h"
#include "utils/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

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

const std::filesystem::path kDirectory = std::filesystem::temp_directory_path() / "quant_test_target_basket";
const std::string           kTargets   = (kDirectory / "targets.json").string();
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

void write_targets(const std::string& as_of_yyyymmdd, const std::string& generated_at = "t1")
{
    nlohmann::json document;
    document["schema"]       = 1;
    document["generated_at"] = generated_at;
    document["as_of"]        = dashed(as_of_yyyymmdd);
    document["count"]        = 5;
    document["sleeves"]      = {{"VALUE", {{"share", 0.5}, {"is_rebalance_day", true}}}, {"MOMENTUM", {{"share", 0.5}, {"is_rebalance_day", true}}}};
    document["targets"]      = nlohmann::json::array({
        {{"ticker", "V1"}, {"sleeve", "VALUE"}, {"weight", 0.5}, {"reference_price", 10000.0}, {"action", "NEW"}},
        {{"ticker", "V2"}, {"sleeve", "VALUE"}, {"weight", 0.5}, {"reference_price", 10000.0}, {"action", "NEW"}},
        {{"ticker", "M1"}, {"sleeve", "MOMENTUM"}, {"weight", 0.5}, {"reference_price", 10000.0}, {"action", "NEW"}},
        {{"ticker", "M2"}, {"sleeve", "MOMENTUM"}, {"weight", 0.5}, {"reference_price", 10000.0}, {"action", "NEW"}},
        {{"ticker", "D1"}, {"sleeve", "MOMENTUM"}, {"weight", 0.0}, {"reference_price", 10000.0}, {"action", "DROP"}, {"reason", "이탈"}},
    });
    std::ofstream(kTargets) << document.dump(2);
}

struct Rig
{
    std::map<std::string, int>      positions;   // 원장 보유(테스트가 직접 바꿔 체결을 흉내 낸다)
    std::vector<std::string>        owned;
    bool                            halted = false;
    double                          scale  = 1.0; // 국면 매수 비율
    std::time_t                     now    = 0;
    std::vector<OrderSignal>        out;
    std::unique_ptr<TargetBasketStrategy> strategy;

    explicit Rig(bool dry_run = false)
    {
        TargetBasketStrategy::Params parameters;
        parameters.targets_file = kTargets;
        parameters.state_file   = kState;
        parameters.capital_krw  = 1'000'000.0;
        parameters.dry_run      = dry_run;
        parameters.pass_interval_ms = 0; // 틱 스로틀은 steady_clock이라 시험에선 끈다 — 패스마다 돈다
        strategy = std::make_unique<TargetBasketStrategy>(parameters, [this](const std::vector<std::string>& tickers)
        {
            owned = tickers;
        });
        strategy->set_clock([this]
        {
            return now;
        });
        strategy->set_position_provider([this](const std::string&, const std::string& ticker)
        {
            auto iterator = positions.find(ticker);
            return iterator == positions.end() ? 0 : iterator->second;
        });
        strategy->set_sellable_provider([this](const std::string&, const std::string& ticker)
        {
            StrategyBase::SellableInfo info;
            auto iterator = positions.find(ticker);
            info.sellable      = iterator == positions.end() ? 0 : iterator->second;
            info.average_price = 9000.0;
            return info;
        });
        strategy->set_entry_halt_provider([this]
        {
            return halted;
        });
        strategy->set_entry_scale_provider([this]
        {
            return scale;
        });
        strategy->set_symbol_resolver([](std::string_view)
        {
            return symbol::SymbolId{1};
        });
        strategy->on_start();
    }

    // 한 패스 — 틱 하나.
    void pass()
    {
        TradeData trade;
        strategy->on_trade_batch(trade, out);
    }
};

int test_all()
{
    std::filesystem::remove_all(kDirectory);
    std::filesystem::create_directories(kDirectory);
    const std::time_t at_1445 = today_at(14, 45);
    const std::string today   = kst::date_yyyymmdd(at_1445);

    // 1. 창 전 / 낡은 파일
    write_targets(today);
    {
        Rig rig;
        rig.now = today_at(14, 39);
        rig.pass();
        CHECK(rig.out.empty());
        CHECK(!std::filesystem::exists(kState));
    }

    {
        write_targets(kst::date_yyyymmdd(at_1445 - 86400));
        Rig rig;
        rig.now = at_1445 + 1;
        rig.pass();
        CHECK(rig.out.empty());
    }

    // 2. 매도 레그 먼저
    write_targets(today);
    Rig rig;
    rig.positions["D1"] = 7;
    rig.now             = at_1445;
    rig.pass();
    CHECK(rig.out.size() == 1 && rig.out[0].ticker == "D1" && rig.out[0].side == OrderSide::SELL && rig.out[0].quantity == 7);
    CHECK(rig.out[0].type == OrderType::MARKET && rig.out[0].reference_price == 10000.0 && rig.out[0].strategy_id == "BASKET_DROP");
    CHECK(std::filesystem::exists(kState));
    {
        std::ifstream  input(kState);
        nlohmann::json state = nlohmann::json::parse(input);
        CHECK(state["day"]["date"] == today && state["day"]["sell_leg_done"] == true && state["day"]["buy_leg_done"] == false);
        CHECK(state["day"]["sent"].size() == 1 && state["day"]["sent"][0] == "D1:SELL");
    }

    // 3. 매수 레그 — 매도가 원장에 남아 있으면 delay 대기
    rig.out.clear();
    rig.now += 2;
    rig.pass();
    CHECK(rig.out.empty());
    rig.positions.erase("D1"); // 체결 반영
    rig.now += 2;
    rig.scale = 0.5; // 국면 매수 비율 — 매수 수량이 반으로
    rig.pass();
    CHECK(rig.out.size() == 3); // 예산 3
    rig.now += 2;
    rig.pass();
    CHECK(rig.out.size() == 4);

    for (const auto& signal : rig.out)
    {
        CHECK(signal.side == OrderSide::BUY && signal.quantity == 12); // 1,000,000 × 0.5 × 0.5 / 10,000 = 25주 × 0.5 → 12주
    }

    rig.scale = 1.0;

    rig.now += 2;
    rig.pass();
    CHECK(rig.out.size() == 4); // 더 없음

    // 4. 재기동 — 같은 상태 파일이면 다시 안 낸다(원장은 아직 비어 있어 계획상 4건 매수가 그대로인 상황)
    {
        Rig restarted;
        restarted.now = at_1445 + 60;
        restarted.pass();
        CHECK(restarted.out.empty());
    }

    // 5. 창 끝 — 새 파일(다른 key)로 리셋돼도 15:00 뒤면 안 낸다
    {
        write_targets(today, "t2");
        Rig late;
        late.now = today_at(15, 0);
        late.pass();
        CHECK(late.out.empty());
        std::ifstream  input(kState);
        nlohmann::json state = nlohmann::json::parse(input);
        CHECK(state["day"]["targets_key"] == "t2/5" && state["day"]["buy_leg_done"] == true);
    }

    // 6. dry_run
    {
        std::filesystem::remove(kState);
        write_targets(today, "t3");
        Rig dry(true);
        dry.now = at_1445;
        dry.pass();
        dry.now += 2;
        dry.pass();
        CHECK(dry.out.empty());
        std::ifstream  input(kState);
        nlohmann::json state = nlohmann::json::parse(input);
        CHECK(state["day"]["sent"].empty()); // dry-run은 보낸 주문을 상태 파일에 안 적는다
    }

    // 7. 소유 종목 sink
    {
        Rig rig_owned;
        CHECK(rig_owned.owned.size() == 5);
        CHECK(std::find(rig_owned.owned.begin(), rig_owned.owned.end(), "D1") != rig_owned.owned.end());
    }

    std::filesystem::remove_all(kDirectory);
    return 0;
}
} // namespace

int main()
{
    if (const char* environment = std::getenv("QUANT_LOG_DIR"); !environment || !*environment)
    {
        Logger::instance().set_base_directory(Logger::executable_directory() / "logs_test");
    }

    if (test_all())
    {
        return 1;
    }

    std::cout << "test_target_basket_strategy: " << g_checks << " checks passed\n";
    return 0;
}
