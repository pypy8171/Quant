#include "strategy/StrategyFactory.h"
#include "StrategyLoadPass.h"
#include "core/Engine.h"
#include "core/Types.h"
#include "strategy/FixedIntervalStrategy.h"
#include "strategy/IntradayBreakoutStrategy.h"
#include "strategy/MACrossStrategy.h"
#include "strategy/MarketMakingStrategy.h"
#include "strategy/TargetBasketStrategy.h"
#include "strategy/ValueContraryStrategy.h"
#include "universe/UniverseScanner.h"
#include "utils/Logger.h"
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace strategy_load
{
// 종목 id 인덱스 비트를 켠다 — 배열은 종목 테이블 용량만큼 한 번만 늘린다.
void mark_symbol(std::vector<bool>& bits, symbol::SymbolId symbol, size_t capacity)
{
    if (symbol == symbol::kNone)
    {
        return;
    }

    if (symbol >= bits.size())
    {
        bits.resize(std::max<size_t>(capacity, symbol + 1), false);
    }

    bits[symbol] = true;
}

bool has_symbol(const std::vector<bool>& bits, symbol::SymbolId symbol)
{
    return symbol != symbol::kNone && symbol < bits.size() && bits[symbol];
}

void add_gated(const LoadPass& context, std::unique_ptr<StrategyBase> strategy)
{
    if (context.pending_regimes && strategy)
    {
        strategy->set_active_regimes(*context.pending_regimes);
    }

    context.engine.add_strategy(std::move(strategy));
}

// 재스캔처럼 Engine이 나중에 factory를 직접 부르는 경로용 — 국면을 factory 안에 묶는다.
StrategyMaker gate_factory(const LoadPass& context, StrategyMaker factory)
{
    if (!context.pending_regimes)
    {
        return factory;
    }

    std::vector<Regime> active_regimes = *context.pending_regimes; // 람다가 이 스코프보다 오래 살아 사본이 필요하다
    return [factory = std::move(factory), active_regimes = std::move(active_regimes)](symbol::SymbolId symbol) {
        auto strategy = factory(symbol);

        if (strategy)
        {
            strategy->set_active_regimes(active_regimes);
        }

        return strategy;
    };
}

// 필수 "ticker" 키. 없으면 예외로 기동이 죽는 대신 경고 후 그 항목만 건너뛴다.
bool require_ticker(const json& node, const char* type, std::string& out)
{
    out = node.value("ticker", std::string());

    if (out.empty())
    {
        LOG_WARN(std::string("[Main] ") + type + " 설정에 ticker 없음 — 등록 건너뜀");
    }

    return !out.empty();
}
} // namespace strategy_load

using namespace strategy_load;

// ─── MA_CROSS ───────────────────────────────────────────────────────────────
static void load_moving_average_cross(LoadPass& context, const json& node)
{
    Engine& engine = context.engine;
    int quantity = node.value("quantity", 1);
    int short_period = node.value("short_period", 0);
    int long_period = node.value("long_period", 0);

    // calc_ma는 prices_(최대 lp개) 끝에서 short_period/lp회 역참조한다. short_period<1이거나 short_period>=lp면
    // begin 이전 역참조(UB/크래시)가 되므로 등록을 건너뛴다.
    if (short_period < 1 || short_period >= long_period)
    {
        LOG_ERROR("[Main] MACross 설정 오류: short_period(" + std::to_string(short_period) +
                  ")는 1 이상이고 long_period(" + std::to_string(long_period) + ") 미만이어야 함 — 등록 건너뜀");
        return;
    }

    if (node.value("universe_from_balance", false))
    {
        // 모의계좌 보유종목 전체를 유니버스로 — 종목마다 MACross 등록.
        // 보유분은 start_in_position=true 로 시드 → 데드크로스에 실제 보유수량 매도, 골든크로스에 재매수.
        KisClient balance_kis(context.kis_config);

        if (!balance_kis.authenticate())
        {
            LOG_ERROR("[Main] universe_from_balance: 잔고조회용 인증 실패 — 건너뜀");
        }
        else
        {
            const KisResult<AccountBalance> balance = balance_kis.get_balance();
            int added = 0;

            if (!balance)
            {
                LOG_WARN("[Main] universe_from_balance: 잔고 조회 실패(" + error_text(balance) + ")");
            }
            else
            {
                for (const Holding& holding : balance->holdings)
                {
                    add_gated(context, std::make_unique<MACrossStrategy>(
                        holding.ticker, short_period, long_period, holding.quantity, /*start_in_position=*/true));
                    LOG_INFO("[Main]   + MACross " + holding.ticker + " 보유 " + std::to_string(holding.quantity) + "주 (in_position 시드)");
                    ++added;
                }
            }

            LOG_INFO("[Main] universe_from_balance: 보유 " + std::to_string(added) +
                     "종목 등록 (short=" + std::to_string(short_period) + " long=" + std::to_string(long_period) + ")");
        }
    }
    else
    {
        std::string ticker;

        if (!require_ticker(node, "MACross", ticker))
        {
            return;
        }

        add_gated(context, std::make_unique<MACrossStrategy>(std::move(ticker), short_period, long_period, quantity));
    }
}

// ─── INTRADAY_BREAKOUT ──────────────────────────────────────────────────────
static void load_intraday_breakout(LoadPass& context, const json& node)
{
    Engine& engine = context.engine;
    // 첫 장중 자동매매 기준(ITB) — WS 체결 틱 기반 채널돌파 + 트레일/하드 스탑.
    int channel_min   = node.value("channel_min", 10);
    double epsilon         = node.value("breakout_eps", 0.002);
    double trail_percent   = node.value("trail_pct", 0.010);
    double hard_percent    = node.value("hard_pct", 0.015);
    int market_close_hhmm       = node.value("market_close_hhmm", 1515);
    int cooldown_sec   = node.value("reentry_cooldown_sec", 60);
    int entry_quantity      = node.value("entry_qty", 1); // 신규 돌파 진입 수량(명목 미지정 시)
    double average_loss_percent = node.value("avg_loss_pct", 0.0); // 평단 대비 손절률(0=비활성)
    // ── v2 파라미터(strategies/ITB/SPEC.md §2/§3) ──
    double seed_trail_percent      = node.value("seed_trail_pct", 0.0);      // 물린분 기준점 트레일(넓게)
    double exit_near_average_percent   = node.value("exit_near_avg_pct", 0.0);   // 물린분 본전탈출 임계
    int    no_new_entry_hhmm   = node.value("no_new_entry_hhmm", 0);     // 신규진입 금지 시각(0→장 마감)
    double notional_per_position = node.value("notional_per_position", 0.0); // 종목당 명목(원)

    if (node.value("universe_from_scan", false))
    {
        // ── 거래대금 상위 스캔 유니버스(ITB v2) ─────────────────────────
        //  실전 도메인 키로 거래대금 랭킹 → 등락률/가격 필터 → (option)수급 → 레짐 게이트.
        universe::ItbScanCfg scan_config;
        scan_config.scan_top_n   = node.value("scan_top_n", 30);
        scan_config.change_min      = node.value("chg_min", 0.02);
        scan_config.change_max      = node.value("chg_max", 0.12);
        scan_config.min_price    = node.value("min_price", 3000.0);
        scan_config.standard_deviation_filter    = node.value("sd_filter", true);
        scan_config.risk_off_index = node.value("risk_off_index_pct", -0.01);
        scan_config.risk_off_index_resume = node.value("risk_off_resume_pct", scan_config.risk_off_index);
        scan_config.risk_off_dwell_sec  = node.value("risk_off_dwell_sec", 0);
        scan_config.max_register = node.value("max_concurrent_positions", 3) * 2; // 후보는 상한의 2배까지 등록(경쟁)

        if (!context.has_quote_kis)
        {
            LOG_ERROR("[Main] universe_from_scan: quote_kis(실전 시세 키) 미설정 — 스캔 불가, 건너뜀");
        }
        else
        {
            KisClient scan_kis(context.quote_kis_config);

            if (!scan_kis.authenticate())
            {
                LOG_ERROR("[Main] universe_from_scan: 시세 키 인증 실패 — 건너뜀");
            }
            else
            {
                auto candidates = universe::scan_itb(scan_kis, scan_config);

                for (auto& candidate : candidates) // candidates는 이 반복 뒤 버려지므로 티커·이름을 옮긴다
                {
                    auto strategy = std::make_unique<IntradayBreakoutStrategy>(
                        std::move(candidate.ticker), entry_quantity, /*hold_quantity=*/0, /*start_in_position=*/false,
                        channel_min, epsilon, trail_percent, hard_percent, market_close_hhmm, cooldown_sec,
                        /*average_price=*/0.0, average_loss_percent, seed_trail_percent, exit_near_average_percent,
                        no_new_entry_hhmm, notional_per_position, /*day_open_price=*/candidate.day_open);
                    strategy->set_name(std::move(candidate.name));
                    add_gated(context, std::move(strategy));
                }
            }
        }
    }
    else if (node.value("universe_from_balance", false))
    {
        // 모의계좌 보유종목 전체를 유니버스로 — 종목마다 ITB 등록(보유분 in_position 시드).
        KisClient balance_kis(context.kis_config);

        if (!balance_kis.authenticate())
        {
            LOG_ERROR("[Main] ITB universe_from_balance: 잔고조회용 인증 실패 — 건너뜀");
        }
        else
        {
            const KisResult<AccountBalance> balance = balance_kis.get_balance();
            int added = 0;

            if (!balance)
            {
                LOG_WARN("[Main] ITB universe_from_balance: 잔고 조회 실패(" + error_text(balance) + ")");
            }
            else
            {
                for (const Holding& holding : balance->holdings)
                {
                    auto strategy = std::make_unique<IntradayBreakoutStrategy>(
                        holding.ticker, entry_quantity, holding.quantity, /*start_in_position=*/true, channel_min, epsilon,
                        trail_percent, hard_percent, market_close_hhmm, cooldown_sec, holding.average_price, average_loss_percent,
                        seed_trail_percent, exit_near_average_percent, no_new_entry_hhmm,
                        /*notional=*/0.0, /*day_open_price=*/0.0);
                    strategy->set_name(holding.name);
                    engine.register_ticker_name(holding.ticker, holding.name); // 로그 라벨(보유분 종목명)
                    add_gated(context, std::move(strategy));
                    LOG_INFO("[Main]   + ITB " + holding.ticker + " " + holding.name + " 보유 " + std::to_string(holding.quantity) +
                             "주 (in_position 시드, 평단=" + std::to_string(static_cast<long long>(holding.average_price)) + ")");
                    ++added;
                }
            }

            LOG_INFO("[Main] ITB universe_from_balance: 보유 " + std::to_string(added) + "종목 등록");
        }
    }
    else
    {
        std::string ticker;

        if (!require_ticker(node, "ITB", ticker))
        {
            return;
        }

        add_gated(context, std::make_unique<IntradayBreakoutStrategy>(
            std::move(ticker), entry_quantity, /*hold_quantity=*/0, /*start_in_position=*/false,
            channel_min, epsilon, trail_percent, hard_percent, market_close_hhmm, cooldown_sec,
            /*average_price=*/0.0, average_loss_percent, seed_trail_percent, exit_near_average_percent,
            no_new_entry_hhmm, notional_per_position, /*day_open_price=*/0.0));
    }
}

// ─── VALUE_CONTRARY ─────────────────────────────────────────────────────────
static void load_value_contrary(LoadPass& context, const json& node)
{
    int quantity = node.value("quantity", 1);
    std::string market_string = node.value("market", "KR");
    Market market = (market_string == "US") ? Market::US : Market::KR;
    std::string exchange = node.value("exchange", "");
    double pbr_max = node.value("pbr_max", 1.0);
    int market_close_hhmm = node.value("market_close_exit_hhmm", 1520);

    // 청산 시각이 정규장 밖이면 사고 나서 팔지 않는다(청산 분기가 세션 판정 뒤에 있다) — 등록하지 않는다.
    if (!ValueContraryStrategy::in_session(market, market_close_hhmm))
    {
        LOG_ERROR("[Main] ValueContrary 설정 오류: market_close_exit_hhmm " + std::to_string(market_close_hhmm) +
                  "이 정규장 밖이라 청산이 나가지 않는다 — 등록 건너뜀");
        return;
    }

    add_gated(context, std::make_unique<ValueContraryStrategy>(market, std::move(exchange), pbr_max, quantity, market_close_hhmm));
}

// ─── FIXED_INTERVAL ─────────────────────────────────────────────────────────
static void load_fixed_interval(LoadPass& context, const json& node)
{
    std::string ticker;

    if (!require_ticker(node, "FixedInterval", ticker))
    {
        return;
    }

    int buy_quantity          = node.value("buy_qty", 1);
    int sell_quantity         = node.value("sell_qty", 1);
    int interval_sec     = node.value("interval_sec", 300);
    add_gated(context, std::make_unique<FixedIntervalStrategy>(std::move(ticker), buy_quantity, sell_quantity, interval_sec));
}

// ─── MARKET_MAKING ──────────────────────────────────────────────────────────
static void load_market_making(LoadPass& context, const json& node)
{
    std::string ticker;

    if (!require_ticker(node, "MarketMaking", ticker))
    {
        return;
    }

    int market_making_quantity              = node.value("quantity", 1);
    int half_spread_ticks   = node.value("half_spread_ticks", 1);
    int requote_move_ticks  = node.value("requote_move_ticks", 1);
    int min_requote_ms      = node.value("min_requote_ms", 1000); // ≥1000 권장(초당 4건 rate 백스톱)
    add_gated(context, std::make_unique<MarketMakingStrategy>(
        std::move(ticker), market_making_quantity, half_spread_ticks, requote_move_ticks, min_requote_ms));
}

// ─── 보유분 청산 관리 (DEVIATION_SCALE 보조) ───────────────────────────────
//  스캔 유니버스가 잡지 못한 잔고 보유분(아침에 산 물린분 등)마다 "청산 전용" ITB를
//  붙인다. 신규진입은 no_new_entry_hhmm=1(항상 과거)로 영구 차단 → 오직 보호·청산만:
//    seed_trail_percent(넓은 기준점 트레일) + exit_near_average_percent(본전근처 반등청산)
//    + average_loss_percent(평단손절, 0=비활성) + 장 마감(market_close_exit_hhmm, 기본 2100=장중 강제청산 안 함 — 엔진이 20:00까지 도니 1600은 애프터마켓 청산이 된다, D-097).
//  covered = 이미 스캔 전략이 담당하는 종목(id 인덱스 비트, 중복 부착 방지). rest_price_feed 합성틱으로 on_trade 구동.
static void attach_holding_exit_managers(LoadPass& context, const json& exit_manager_node,
                                     const std::vector<bool>& covered)
{
    Engine& engine = context.engine;
    double seed_trail_display    = exit_manager_node.value("seed_trail_pct", 2.0);     // 표시용(%)
    double exit_near_average_display = exit_manager_node.value("exit_near_avg_pct", 1.0);
    double seed_trail_percent     = seed_trail_display / 100.0;             // %→비율
    double exit_near_average_percent  = exit_near_average_display / 100.0;
    double average_loss_percent       = exit_manager_node.value("avg_loss_pct", 0.0) / 100.0; // 0=비활성
    int    market_close_hhmm           = exit_manager_node.value("market_close_exit_hhmm", 2100);     // 2100=장중 강제청산 안 함(보호만) [why D-097]
    int    channel_min        = exit_manager_node.value("channel_min", 10);
    int    cooldown_sec       = exit_manager_node.value("reentry_cooldown_sec", 60);
    // 본전탈출 무장 깊이 — 평단 -arm%까지 실제로 밀려 본 적이 있어야 본전탈출이 켜진다.
    //  이게 없으면 평단 -0.2%로 보유한 종목이 재기동 첫 틱에 전량 청산된다(그건 물린 게
    //  아니라 그냥 본전이다). 부착유예는 재기동 첫 틱과 국면 배선 사이 경합을 막는다.
    double arm_display           = exit_manager_node.value("exit_near_avg_arm_pct", 3.0);
    double exit_arm_percent       = arm_display / 100.0;
    int    guard_warmup_sec   = exit_manager_node.value("guard_warmup_sec", 60);
    // 이월분 평단 하드스톱 조합안(D-082): 평단 −seed_hard_percent(%) + seed_hard_from_hhmm 이후 + 1분봉 종가
    //  seed_hard_confirm_bars 연속 확인. seed_hard_skip_percent(%)보다 깊게 물린 구형 보유는 제외. 0=비활성.
    double seed_hard_percent      = exit_manager_node.value("seed_hard_pct", 0.0) / 100.0;
    double seed_hard_skip_percent = exit_manager_node.value("seed_hard_skip_pct", 15.0) / 100.0;
    int    seed_hard_from     = exit_manager_node.value("seed_hard_from_hhmm", 915);
    int    seed_hard_bars     = exit_manager_node.value("seed_hard_confirm_bars", 3);

    KisClient balance_kis(context.kis_config);

    if (!balance_kis.authenticate())
    {
        LOG_ERROR("[Main] manage_holdings: 잔고조회 인증 실패 — 청산 관리 건너뜀");
        return;
    }

    const KisResult<AccountBalance> balance = balance_kis.get_balance();

    if (!balance)
    {
        LOG_WARN("[Main] manage_holdings: 잔고 조회 실패(" + error_text(balance) + ") — 부착할 보유분 없음");
        return;
    }

    int added = 0, skipped = 0;

    for (const Holding& holding : balance->holdings)
    {
        const std::string& code  = holding.ticker;
        const std::string& pname = holding.name;
        const int    hq = holding.quantity;
        const double average_value = holding.average_price;

        if (has_symbol(covered, engine.symbols().lookup(code))) // 스캔 전략이 이미 담당 → 이중 부착 방지(잔고 티커는 문자열)
        {
            ++skipped;
            continue;
        }

        auto strategy = std::make_unique<IntradayBreakoutStrategy>(
            code, /*entry_quantity=*/0, /*hold_quantity=*/hq, /*start_in_position=*/true,
            channel_min, /*breakout_epsilon=*/0.002, /*trail_percent=*/0.010, /*hard_percent=*/0.015,
            market_close_hhmm, cooldown_sec, /*average_price=*/average_value, average_loss_percent,
            seed_trail_percent, exit_near_average_percent, /*no_new_entry_hhmm=*/1,
            /*notional=*/0.0, /*day_open_price=*/0.0);
        strategy->set_exit_near_average_arm(exit_arm_percent);
        strategy->set_guard_warmup_sec(guard_warmup_sec);
        strategy->set_seed_hard_stop(seed_hard_percent, seed_hard_skip_percent, seed_hard_from, seed_hard_bars);
        strategy->set_name(pname);
        engine.register_ticker_name(code, pname); // 로그 라벨(보유분 종목명)
        engine.mark_exit_managed_ticker(code);        // 스캔 슬리브의 신규매수에서 제외
        add_gated(context, std::move(strategy));
        LOG_INFO("[Main]   + 청산 관리(ITB) " + code + " " + pname + " 보유 " +
                 std::to_string(hq) + "주 @평단 " + std::to_string(static_cast<long long>(average_value)) +
                 " (trail=" + std::to_string(seed_trail_display) + "% 본전탈출=" +
                 std::to_string(exit_near_average_display) + "%(무장 -" + std::to_string(arm_display) + "%) 유예" +
                 std::to_string(guard_warmup_sec) + "s market_close=" + std::to_string(market_close_hhmm) + ")");
        ++added;
    }

    LOG_INFO("[Main] manage_holdings: 청산 관리 " + std::to_string(added) +
             "종목 부착, 스캔중복 " + std::to_string(skipped) + "종목 스킵");
}

// ─── 전략-국면 매핑 ────────────────────────────────────────────────────────
//  config "active_regimes"(미지정 시 전 국면)를 파싱한다. 적용은 add_gated/gate_factory가
//  로더 실행 중 추가되는 전략 전부에 한다. 반환 false = 키 없음 또는 비어 있음(전 국면).
static bool parse_active_regimes(const json& node, const std::string& type, std::vector<Regime>& active_regimes)
{
    if (!node.contains("active_regimes") || !node["active_regimes"].is_array())
    {
        return false;
    }

    static const std::string kEmptyRegimeName;

    for (const auto& regime_node : node["active_regimes"])
    {
        const std::string& rs = regime_node.is_string() ? regime_node.get_ref<const std::string&>() : kEmptyRegimeName;
        Regime             parsed = Regime::from_string(rs);

        if (parsed == Regime::UNKNOWN)
        {
            LOG_WARN("[Main] " + type + " 알 수 없는 active_regimes 값: '" + rs +
                      "' (BULL/NEUTRAL/BEAR만 유효) — 무시됨");   // G-2
        }
        else
        {
            active_regimes.push_back(parsed);
        }
    }

    if (active_regimes.empty())   // G-2: 키는 있는데 파싱 결과가 비면 게이트가 조용히 무력화됨 → 경고
    {
        LOG_WARN("[Main] " + type + " active_regimes 파싱 결과 비어있음 — 전 국면 통과로 동작(게이트 무효)");
        return false;
    }

    return true;
}

// ─── TARGET_BASKET ──────────────────────────────────────────────────────────
//  목표 비중표 바스켓. 소유 종목을 OrderGate 슬롯 계산 밖에 두고(set_slot_exempt_tickers) context.basket_owned·context.scan_covered에
//  넣어 DEVSCALE 유니버스와 청산 관리 부착에서 뺀다 — 그래서 load_strategies가 이 타입을 먼저 돈다. [why D-109]
static void load_target_basket(LoadPass& context, const json& node)
{
    TargetBasketStrategy::Params parameters;
    parameters.label                = node.value("label", std::string("MAIN"));
    parameters.account              = node.value("account", std::string());
    parameters.targets_file         = node.value("targets_file", parameters.targets_file);
    parameters.state_file           = node.value("state_file", parameters.state_file);
    parameters.capital_krw          = node.value("capital_krw", 0.0);
    parameters.band                 = node.value("band_pct", 10.0) / 100.0;
    parameters.window_start_hhmm    = node.value("window_start_hhmm", 1440);
    parameters.window_end_hhmm      = node.value("window_end_hhmm", 1500);
    parameters.buy_leg_delay_sec    = node.value("buy_leg_delay_sec", 90);
    parameters.max_signals_per_pass = node.value("max_signals_per_pass", 3);
    parameters.reload_sec           = node.value("reload_sec", 60);
    parameters.dry_run              = node.value("dry_run", false);

    if (parameters.capital_krw <= 0.0)
    {
        LOG_WARN("[Main] TARGET_BASKET capital_krw 없음 — 등록 건너뜀");
        return;
    }

    Engine& engine = context.engine;
    auto    strategy = std::make_unique<TargetBasketStrategy>(
        std::move(parameters), [&engine](const std::vector<std::string>& tickers)
        {
            engine.set_slot_exempt_tickers(tickers);
        });
    strategy->load_targets(); // 기동 때 파일이 있으면 소유 종목을 지금 확정한다(뒤에 도는 DEVSCALE 로더가 본다)

    const size_t symbol_capacity = engine.symbols().capacity();

    for (const auto& ticker : strategy->owned_tickers()) // 목표 비중표 파일의 문자열 티커 — 여기서 id가 된다
    {
        const symbol::SymbolId symbol = engine.symbols().intern(ticker);
        mark_symbol(context.basket_owned, symbol, symbol_capacity);
        mark_symbol(context.scan_covered, symbol, symbol_capacity);
    }

    LOG_INFO("[Main] TARGET_BASKET " + strategy->describe() + " 소유 종목 " + std::to_string(strategy->owned_tickers().size()) + "개");
    add_gated(context, std::move(strategy));
}

// ─── 디스패치 ───────────────────────────────────────────────────────────────
void load_strategies(StrategyLoadCtx& context, const json& strategies)
{
    // 운영 설정 현황(2026-09-25 확인): Quant/config/config_dev_paper.json과 config_live.json은 DEVIATION_SCALE 하나만
    //  등록한다. TARGET_BASKET(D-109)은 로더만 있고 지금 어느 설정에도 없다.
    // INTRADAY_BREAKOUT은 운영에서는 이 표가 아니라 attach_holding_exit_managers()가 승계 보유분에만 붙이는 청산 전용이다.
    // 나머지 유형은 시험용 모의 설정(config_mm_paper.json의 MARKET_MAKING 등)에서만 쓴다.
    static const std::map<StrategyType, void (*)(LoadPass&, const json&)> LOADERS = {
        {StrategyType::MA_CROSS, load_moving_average_cross},
        {StrategyType::INTRADAY_BREAKOUT, load_intraday_breakout},
        {StrategyType::VALUE_CONTRARY, load_value_contrary},
        {StrategyType::FIXED_INTERVAL, load_fixed_interval},
        {StrategyType::MARKET_MAKING, load_market_making},
        {StrategyType::DEVIATION_SCALE, load_deviation_scale},
        {StrategyType::TARGET_BASKET, load_target_basket},
    };

    LoadPass pass(context);
    pass.priority_merger = make_entry_priority_merger();

    // 바스켓 슬리브를 먼저 — 그 소유 종목을 DEVSCALE 유니버스·청산 관리 부착에서 빼야 하므로 config 순서와 무관하게 앞에 둔다 [why D-109].
    std::vector<const json*> ordered;

    for (const auto& strategy : strategies)
    {
        if (StrategyType::from_string(strategy.value("type", std::string())) == StrategyType::TARGET_BASKET)
        {
            ordered.push_back(&strategy);
        }
    }

    for (const auto& strategy : strategies)
    {
        if (StrategyType::from_string(strategy.value("type", std::string())) != StrategyType::TARGET_BASKET)
        {
            ordered.push_back(&strategy);
        }
    }

    for (const json* strategy_node : ordered)
    {
        const json& strategy = *strategy_node;
        std::string type = strategy.value("type", std::string());
        size_t n_before = context.engine.strategy_count();

        auto iterator = LOADERS.find(StrategyType::from_string(type));

        if (iterator == LOADERS.end())
        {
            LOG_WARN("[Main] 알 수 없는 전략: '" + type + "'");
            continue;
        }

        std::vector<Regime> active_regimes;
        const bool gated = parse_active_regimes(strategy, type, active_regimes);
        pass.pending_regimes = gated ? &active_regimes : nullptr;
        iterator->second(pass, strategy);
        pass.pending_regimes = nullptr;

        if (gated && context.engine.strategy_count() > n_before)
        {
            LOG_INFO("[Main] " + type + " 활성국면: " + std::to_string(active_regimes.size()) + "개 × 전략 " +
                     std::to_string(context.engine.strategy_count() - n_before) + "개");
        }
    }

    // 전 슬리브의 초기 유니버스가 확정된 뒤에야 "스캔 밖 보유분"을 가릴 수 있다.
    if (pass.pending_exit_managers != nullptr)
    {
        pass.pending_regimes = pass.guard_gated ? &pass.guard_regimes : nullptr;
        attach_holding_exit_managers(pass, *pass.pending_exit_managers, pass.scan_covered);
    }
}
