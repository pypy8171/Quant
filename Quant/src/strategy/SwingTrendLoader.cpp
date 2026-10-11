// strategy/SwingTrendLoader.cpp — SWING_TREND(스윙 추세 슬리브) 로더: config 항목을 읽어 상태 파일을 잔고와 맞추고 엔진에 등록한다.
//  보유는 context.scan_covered에 넣어 청산 관리(ITB) 부착에서 빼고(재기동 때 승계 금지), 매수 후보는 context.basket_owned에 넣어
//  DEVSCALE 초기 유니버스에서 뺀다. 바스켓·SURGE_HOLD가 먼저 돌아 채운 basket_owned 종목은 매수하지 않는다.
//  스레드: 메인 스레드(load_strategies 안). 관련 결정: D-160.
#include "StrategyLoadPass.h"

#include "api/KisClient.h"
#include "core/Engine.h"
#include "strategy/SwingTrendStrategy.h"
#include "utils/Logger.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace strategy_load
{
namespace
{
// 기동 잔고와 상태 파일을 맞춘다. 잔고를 못 받으면 상태 파일을 그대로 쓴다(장중 track_fills가 장부로 다시 맞춘다).
void reconcile_held(const LoadPass& context, SwingTrendStrategy& strategy)
{
    if (strategy.held_tickers().empty())
    {
        return;
    }

    KisClient held_kis(context.kis_config);

    if (!held_kis.authenticate())
    {
        LOG_WARN("[Main] SWING_TREND 잔고조회 인증 실패 — 상태 파일 보유를 그대로 쓴다");
        return;
    }

    const KisResult<AccountBalance> balance = held_kis.get_balance();

    if (!balance)
    {
        LOG_WARN("[Main] SWING_TREND 잔고 조회 실패(" + error_text(balance) + ") — 상태 파일 보유를 그대로 쓴다");
        return;
    }

    std::map<std::string, int> held_quantity;

    for (const Holding& holding : balance->holdings)
    {
        held_quantity[holding.ticker] += holding.quantity;
    }

    const int dropped = strategy.reconcile_with_balance(held_quantity);

    if (dropped > 0)
    {
        LOG_WARN("[Main] SWING_TREND 잔고에 없는 보유 " + std::to_string(dropped) + "종목을 상태 파일에서 뺐다");
    }
}
} // namespace

void load_swing_trend(LoadPass& context, const nlohmann::json& node)
{
    SwingTrendStrategy::Params parameters;
    read_sleeve_common(node, parameters);
    read_or_keep(node, "plan_file", parameters.plan_file);
    read_or_keep(node, "notional_krw", parameters.notional_krw);
    read_or_keep(node, "buy_start_hhmm", parameters.buy_start_hhmm);
    read_or_keep(node, "buy_cancel_hhmm", parameters.buy_cancel_hhmm);
    read_or_keep(node, "sell_start_hhmm", parameters.sell_start_hhmm);
    read_or_keep(node, "sell_end_hhmm", parameters.sell_end_hhmm);
    read_or_keep(node, "stale_days", parameters.stale_days);
    read_or_keep(node, "pivot_window", parameters.pivot_window);
    read_or_keep(node, "daily_lookback", parameters.daily_lookback);
    read_or_keep(node, "exit_retry_sec", parameters.exit_retry_sec);
    read_or_keep(node, "max_exit_attempts", parameters.max_exit_attempts);

    if (parameters.notional_krw <= 0)
    {
        LOG_WARN("[Main] SWING_TREND notional_krw 없음 — 등록 건너뜀");
        return;
    }

    Engine&      engine          = context.engine;
    const size_t symbol_capacity = engine.symbols().capacity();

    // 바스켓·SURGE_HOLD가 먼저 돌아 basket_owned에 자기 종목을 켜 두었다 — 이 종목들은 매수 후보에서 뺀다.
    for (size_t index = 0; index < context.basket_owned.size() && index < symbol_capacity; ++index)
    {
        if (context.basket_owned[index])
        {
            parameters.excluded_tickers.push_back(engine.symbols().name(static_cast<symbol::SymbolId>(index)).string());
        }
    }

    const bool        dry_run  = parameters.dry_run;
    const std::string owner    = "SWING_" + parameters.label; // 전략 id와 같은 이름 — 슬롯 면제 소유자
    auto              strategy = std::make_unique<SwingTrendStrategy>(
        std::move(parameters), [&engine, owner](const std::vector<std::string>& tickers)
        {
            return engine.set_slot_exempt_tickers(owner, tickers);
        });
    strategy->load_state();
    strategy->load_plan();
    reconcile_held(context, *strategy);

    for (const auto& ticker : strategy->held_tickers())
    {
        mark_symbol(context.scan_covered, engine.symbols().intern(ticker), symbol_capacity);
    }

    // dry_run은 주문을 내지 않으니 보유·후보를 DEVSCALE 유니버스에서 빼지 않는다.
    if (!dry_run)
    {
        for (const auto& ticker : strategy->held_tickers())
        {
            mark_symbol(context.basket_owned, engine.symbols().intern(ticker), symbol_capacity);
        }

        for (const auto& ticker : strategy->candidate_tickers())
        {
            mark_symbol(context.basket_owned, engine.symbols().intern(ticker), symbol_capacity);
        }
    }

    LOG_INFO("[Main] SWING_TREND " + strategy->describe() + " 보유 " + std::to_string(strategy->held_tickers().size()) + "종목, 매수 후보 " +
             std::to_string(strategy->candidate_tickers().size()) + "종목");
    add_gated(context, std::move(strategy));
}
} // namespace strategy_load
