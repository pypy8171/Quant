#include "strategy/StrategyFactory.h"
#include "core/Engine.h"
#include "core/KstTime.h"
#include "core/Types.h"
#include "core/UniverseExit.h"
#include "strategy/DevScaleRules.h"
#include "strategy/DeviationScaleStrategy.h"
#include "strategy/FixedIntervalStrategy.h"
#include "strategy/IntradayBreakoutStrategy.h"
#include "strategy/MACrossStrategy.h"
#include "strategy/MarketMakingStrategy.h"
#include "strategy/TargetBasketStrategy.h"
#include "strategy/ValueContraryStrategy.h"
#include "universe/MarketBoard.h"
#include "universe/ScoreWeight.h"
#include "universe/UniverseScanner.h"
#include "utils/JsonNode.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

struct EntryPriorityMerger;

// ─── 로드 한 번의 공용 상태 ─────────────────────────────────────────────────
//  로더끼리 넘겨야 하는 값을 파일 전역 대신 여기에 둔다. load_strategies가 만들어 로더에 넘기고,
//  돌아가면 사라진다 — 재스캔 람다가 나중에 쓰는 것(priority_merger)만 shared_ptr로 따로 산다.
//  스레드: 메인 스레드(load_strategies 안)만 쓴다.
struct LoadPass : StrategyLoadCtx
{
    explicit LoadPass(const StrategyLoadCtx& base) : StrategyLoadCtx(base) {}

    // 활성 국면 부착 — 한 config 항목이 N개를 등록하는 로더(유니버스·보유 전 종목)에서 하나도 빠지지 않도록,
    //  전략을 추가하는 지점마다 붙인다.
    //  로더를 부르는 동안만 값이 있다(그 밖에서는 nullptr).
    const std::vector<Regime>* pending_regimes = nullptr;

    // 보유분 청산 관리 부착은 슬리브 하나가 아니라 전 슬리브가 등록된 뒤에 한 번만 한다.
    //  scan_covered는 모든 DEVIATION_SCALE 슬리브가 담당하는 종목(id 인덱스 비트)의 합집합이고, 청산 관리 설정은
    //  마지막으로 manage_holdings.enabled를 켠 슬리브의 것을 쓴다(현재 구성은 하나만 켠다).
    std::vector<bool>   scan_covered;
    const json*         pending_exit_managers = nullptr; // config 노드를 가리킨다 — load_strategies 안에서만 유효
    bool                guard_gated = false;
    std::vector<Regime> guard_regimes;

    // 바스켓 슬리브(TARGET_BASKET)가 소유한 종목(id 인덱스 비트) — DEVSCALE 초기 유니버스와 청산 관리 부착에서 뺀다.
    //  바스켓 로더가 먼저 돌아 채운다 [why D-109].
    std::vector<bool> basket_owned;

    // 전 슬리브의 진입 우선순위 점수표. 재스캔 람다(데이터 스레드)가 들고 가 로드가 끝난 뒤에도 쓴다.
    std::shared_ptr<EntryPriorityMerger> priority_merger;
};

// 종목 id 인덱스 비트를 켠다 — 배열은 종목 테이블 용량만큼 한 번만 늘린다.
static void mark_symbol(std::vector<bool>& bits, symbol::SymbolId symbol, size_t capacity)
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

static bool has_symbol(const std::vector<bool>& bits, symbol::SymbolId symbol)
{
    return symbol != symbol::kNone && symbol < bits.size() && bits[symbol];
}

static void add_gated(const LoadPass& context, std::unique_ptr<StrategyBase> strategy)
{
    if (context.pending_regimes && strategy)
    {
        strategy->set_active_regimes(*context.pending_regimes);
    }

    context.engine.add_strategy(std::move(strategy));
}

// 재스캔처럼 Engine이 나중에 factory를 직접 부르는 경로용 — 국면을 factory 안에 묶는다.
static std::function<std::unique_ptr<StrategyBase>(symbol::SymbolId)>
gate_factory(const LoadPass& context, std::function<std::unique_ptr<StrategyBase>(symbol::SymbolId)> factory)
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
static bool require_ticker(const json& node, const char* type, std::string& out)
{
    out = node.value("ticker", std::string());

    if (out.empty())
    {
        LOG_WARN(std::string("[Main] ") + type + " 설정에 ticker 없음 — 등록 건너뜀");
    }

    return !out.empty();
}

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

// 이 슬리브(id_prefix)가 오늘부터 lookback_days일 전(달력일)까지 산 종목 — 체결 원장 logs/trades_YYYYMMDD.csv
//  (OrderRouter가 쓴다)의 FILL·BUY 행. lookback_days 0이면 오늘 원장 하나만 본다.
//  재기동 때 보유분을 전부 청산 관리(ITB)로 넘기면 당일 매수분도 익절선 없이 트레일에만 걸린다(09-04~18 승계 매도
//  725체결 −190만). 분할 매수가 없으면(buy_split_steps 0) 명목 상한 초과 위험이 없어 DevScale이 그대로 맡는다.
//  파일이 없거나(첫 기동) 못 읽으면 빈 집합 — 그때는 기존대로 청산 관리가 맡는다.
//  넘김 모드(market_close_exit_hhmm 2400)는 전날 산 것도 DevScale 보유라 호출자가 lookback_days 20을 준다.
static std::set<std::string> tickers_bought_recently(const std::string& id_prefix, int lookback_days)
{
    std::set<std::string> bought;
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    constexpr std::time_t kSecondsPerDay = 86400;

    for (int day_offset = 0; day_offset <= lookback_days; ++day_offset)
    {
        const std::string date = kst::date_yyyymmdd(now - static_cast<std::time_t>(day_offset) * kSecondsPerDay);
        std::ifstream ledger(Logger::instance().path_for("trades_" + date + ".csv"));
        bought.merge(devscale_rules::tickers_bought_from_ledger(ledger, id_prefix));
    }

    return bought;
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

// ─── DEVIATION_SCALE ────────────────────────────────────────────────────────
// 스캔이 매긴 종목별 비중 배수를 팩토리에 건네는 공유 상태.
//  스캔(초기=메인 스레드, 재스캔=데이터 스레드)이 쓰고 팩토리가 읽으므로 뮤텍스로 감싼다.
//  이미 등록된 전략의 배수는 갱신하지 않는다 — 분할 매수 도중에 예산이 바뀌면 남은 층 예산과
//  평단이 어긋난다. 재스캔으로 새로 붙는 종목만 최신 배수를 받는다.
//  두 배열 다 종목 id 인덱스(0=값 없음), 크기는 종목 테이블 용량. [why D-112]
struct DevScaleScoreState
{
    std::mutex          mutex;
    std::vector<double> multiplier;
    std::vector<double> krw; // 종목당 명목 총액(원). 원 사이징이 켜진 슬리브만 채운다

    explicit DevScaleScoreState(size_t capacity) : multiplier(capacity, 0.0), krw(capacity, 0.0) {}
};

// 한 슬리브의 스캔이 재스캔마다 다시 쓰는 버퍼. 매번 새로 잡지 않으려고 스캔 콜백이 들고 있다.
//  락이 없다 — 한 슬리브의 스캔은 한 번에 하나씩만 돈다(초기 스캔은 엔진 시작 전 메인 스레드, 재스캔은
//  그 뒤 데이터 스레드). [inv] 스캔 콜백 밖에서는 건드리지 않는다.
struct DevScaleScanBuffers
{
    universe::QuoteTable quotes;     // 전 종목 시세 표(종목 id 인덱스). 칸 비우기·다시 채우기는 load_quote_table
    std::vector<double>  multiplier; // scores 순서의 비중 배수
    std::vector<double>  sorted_raw; // score_to_mult 작업 버퍼
    std::vector<double>  krw;        // scores 순서의 종목당 명목(원)
    std::vector<bool>    held;       // 재스캔 때 보유·바스켓 종목 표시(종목 id 인덱스)
};

// 점수 z → 종목당 명목(원). z≤0은 바닥, z≥cap_z는 천장, 사이는 직선. 결과는 scores와 같은 순서.
//  [formula] krw = floor + (capture − floor) × clamp(z / cap_z, 0, 1)
//  천장은 "풀 안에서 확실히 강하다"(z)만 본다. 절대 산포 조건(모두 비슷한 장에서는 천장을 닫는
//  것)은 점수 이력이 쌓인 뒤 붙인다 — 지금은 이력이 없어 임계를 정할 근거가 없다.
static void score_to_krw(const universe::ScoreList& scores, double floor_krw, double cap_krw, double cap_z,
                         std::vector<double>& krw)
{
    universe::score_to_z(scores, krw);

    for (double& value : krw)
    {
        const double factor = cap_z > 0.0 ? (std::max)(0.0, (std::min)(1.0, value / cap_z)) : 0.0;
        value               = floor_krw + (cap_krw - floor_krw) * factor;
    }
}

// 진입 우선순위 랭크는 슬리브 하나가 아니라 전 슬리브를 합쳐서 매겨야 한다. 슬리브마다
//  set_entry_priority를 부르면 나중에 스캔한 쪽이 앞 슬리브의 랭크 맵을 통째로 덮어쓰고,
//  랭크를 잃은 종목은 rank=0이 되어 우선순위 바를 건너뛴다. 두 슬리브가 20초 간격으로
//  번갈아 스캔하는 지금 구성에서는 바가 절반만 작동하는 셈이었다.
//  슬리브별 z는 각자의 풀 안에서 정규화된 값이라 슬리브를 넘는 비교는 근사다. 그래도
//  랭크가 통째로 사라지는 것보다는 낫다.
//  슬리브 키는 설정의 id_prefix 문자열 그대로(둘뿐, 스캔당 한 번 찾는다). 종목은 id로 든다.
//  합치기부터 엔진에 넣기까지 한 락 안에서 한다. 두 슬리브가 겹쳐 부르면 먼저 합친 쪽이 늦게 넣어
//  최신 표를 옛 표로 덮을 수 있어서다. 아래 버퍼는 그 락 아래에서만 쓰고 매번 새로 잡지 않는다.
struct EntryPriorityMerger
{
    std::mutex                                 mutex;
    std::map<std::string, universe::ScoreList> by_sleeve;
    std::vector<int32_t>                       slot_by_symbol; // 합칠 때 종목 id → merged 위치(-1=아직 없음)
    universe::ScoreList                        merged;
    std::vector<int>                           rank;
    std::vector<size_t>                        rank_order;     // score_to_rank 작업 버퍼
    std::vector<double>                        z_score;
    std::vector<OrderGate::PriorityEntry>      entries;
};

// 한 슬리브의 점수를 갱신하고, 전 슬리브를 합친 랭크를 엔진에 넣는다.
//  scores는 sink — 슬리브 표에 옮겨 넣는다.
//  [lock-order] merger.mutex를 쥔 채 engine.set_entry_priority를 부른다. 그 안의 잠금은 파일 쓰기용 하나뿐이고
//  그쪽에서 merger를 다시 부르지 않으므로 잠금 순서가 뒤집히지 않는다.
static void publish_entry_priority(Engine& engine, EntryPriorityMerger& merger, const std::string& sleeve,
                                   universe::ScoreList scores)
{
    std::lock_guard<std::mutex> lock(merger.mutex);
    universe::ScoreList&        merged = merger.merged;
    merged.clear();
    merger.by_sleeve[sleeve] = std::move(scores);

    if (merger.slot_by_symbol.size() < engine.symbols().capacity())
    {
        merger.slot_by_symbol.assign(engine.symbols().capacity(), -1);
    }

    for (const auto& sleeve_entry : merger.by_sleeve)
    {
        for (const universe::SymbolScore& entry : sleeve_entry.second)
        {
            if (entry.symbol == symbol::kNone || entry.symbol >= merger.slot_by_symbol.size())
            {
                continue;
            }

            int32_t& slot = merger.slot_by_symbol[entry.symbol];

            // 같은 종목이 두 슬리브에 올라오면 높은 점수를 남긴다.
            if (slot < 0)
            {
                slot = static_cast<int32_t>(merged.size());
                merged.push_back(entry);
            }
            else if (entry.score > merged[static_cast<size_t>(slot)].score)
            {
                merged[static_cast<size_t>(slot)].score = entry.score;
            }
        }
    }

    for (const universe::SymbolScore& entry : merged) // 다음 호출을 위해 건드린 칸만 되돌린다
    {
        merger.slot_by_symbol[entry.symbol] = -1;
    }

    universe::score_to_rank(merged, engine.symbols(), merger.rank, merger.rank_order);
    universe::score_to_z(merged, merger.z_score);
    merger.entries.clear();

    for (size_t index = 0; index < merged.size(); ++index)
    {
        merger.entries.push_back({merged[index].symbol, merger.rank[index], merger.z_score[index]});
    }

    engine.set_entry_priority(merger.entries, static_cast<int>(merged.size()));
}

static void load_deviation_scale(LoadPass& context, const json& node)
{
    Engine& engine = context.engine;
    // 공통 파라미터(티커 제외) — 스캔 유니버스/단일 종목이 함께 쓴다.
    DeviationScaleStrategy::Params base;
    base.base_percent          = node.value("base_pct", 0.05);        // 베이스 명목 = 자본의 5%
    base.max_percent           = node.value("max_pct", 0.10);         // 종목당 상한 명목 = 자본의 10%
    base.fallback_equity   = node.value("fallback_equity", 0.0);  // 잔고조회 실패 시 기준자본(원)
    base.base_quantity          = node.value("base_qty", 10);          // (폴백) 주수
    base.step_quantity          = node.value("step_qty", 5);           // (폴백) 주수
    base.simple_moving_average_period        = node.value("sma_period", 20);
    base.deviation_sell          = node.value("dev_sell_pct", 1.5);
    base.deviation_buy           = node.value("dev_buy_pct", 0.8);
    base.split_step_count           = node.value("split_step_count", 2);
    base.add_below_simple_moving_average_only = node.value("add_below_sma_only", true); // 점진 진입: 물타기는 기준선 아래(눌림)에서만
    // 개장 후 3분봉이 안 쌓인 구간(20봉×3분=60분)에 일봉 SMA20을 임시 기준선으로 쓴다.
    //  false면 예전대로 봉이 찰 때까지 발주하지 않는다(개장~10:00 발주 0).
    base.daily_basis_warmup = node.value("daily_basis_warmup", true);
    base.cross_guard       = node.value("split_buy_cross_guard", true);  // 분할 매수 층이 현재가를 넘지 않게 기준점 클램프(D-006)
    base.pullback_percent      = node.value("pullback_pct", 2.0);
    base.entry_upper_percent   = node.value("entry_upper_pct", 0.0);   // SMA20 위 진입 허용%(0=순수 눌림만)
    base.zone_hysteresis_percent     = node.value("zone_hyst_pct", 4.0);     // 존 유지 여유폭(%) — 경계 진동 방지
    // 정배열 허용오차는 스캐너와 같은 값을 써야 등록·활성이 어긋나지 않아, 슬리브 설정에 없으면
    //  아래 유니버스 스캔 블록과 같은 기본값(0=엄격)을 쓴다.
    base.align_moving_average_tolerance_percent  = node.value("align_ma_tol_pct", 0.0);
    base.reprice_move_ticks = node.value("reprice_move_ticks", 2);
    base.min_rebuild_sec    = node.value("min_rebuild_sec", 0);
    base.id_prefix          = node.value("id_prefix", std::string("DEVSCALE"));
    base.buy_split_steps          = node.value("buy_split_steps", -1);
    base.stop_loss_percent      = node.value("stop_loss_pct", 0.0);
    base.stop_cooldown_sec  = node.value("stop_cooldown_sec", 900);
    base.reentry_cooldown_sec = node.value("reentry_cooldown_sec", 600); // 전량 청산 뒤 재진입 대기(0=끄기)
    base.dust_krw           = node.value("dust_krw", 250000.0);     // 평가금 이 아래 잔존 보유는 시장가 정리(0=끄기)
    base.sell_base_average    = node.value("sell_base_average", false);
    base.trail_arm_percent    = node.value("trail_arm_pct", 0.0);
    base.trail_percent        = node.value("trail_pct", 1.0);
    base.entry_atr_max_percent            = node.value("entry_atr_max_pct", 0.0);
    base.entry_open_deviation_min_percent = node.value("entry_open_dev_min_pct", -99.0);
    base.entry_open_deviation_max_percent = node.value("entry_open_dev_max_pct", 99.0);
    base.prefetch_jitter_percent = node.value("prefetch_jitter_pct", 50);
    base.bar_source        = node.value("bar_source", std::string("ws"));   // "ws"(기본)|"rest" (D-069·D-072)
    base.market_close_hhmm          = node.value("market_close_exit_hhmm", 1515);
    base.interval_min      = node.value("interval_min", 3);
    base.min_action_ms     = node.value("min_action_ms", 3000);
    base.daily_lookback    = node.value("daily_lookback", 70);
    base.account           = node.value("account", std::string());

    // 스캔/단일로 실제 DeviationScale이 담당하는 종목(id 인덱스 비트) — 보유분 청산 관리 중복 부착 방지.
    const size_t      symbol_capacity = engine.symbols().capacity();
    std::vector<bool> covered(symbol_capacity, false);

    // 이미 보유 중인 종목은 DeviationScale 신규 스캔에서 제외 → 청산 관리가 전담(윈드다운).
    //  시드/전일 물린 보유분에 DevScale 분할 매수가 겹치면 종목당 명목상한(max_percent)을
    //  초과해 CANCEL 거부·과주문이 난다(073240 사례). 보유분=청산 관리, 신규만=DevScale로 분리.
    //  manage_holdings.enabled일 때만 적용(청산 관리가 있어야 보유분을 인수하므로).
    std::vector<bool>             held = context.basket_owned; // 바스켓 소유 종목은 처음부터 이 슬리브의 후보가 아니다 [why D-109]
    size_t                        held_count = 0;
    std::vector<symbol::SymbolId> reinstated; // 보유 중인 최근 매수분 — 스캔에 없어도 등록하고 청산 관리는 안 붙인다
    held.resize(std::max(held.size(), symbol_capacity), false);

    if (node.contains("manage_holdings") && node["manage_holdings"].value("enabled", false))
    {
        KisClient held_kis(context.kis_config);

        if (held_kis.authenticate())
        {
            const KisResult<AccountBalance> balance = held_kis.get_balance();

            if (!balance)
            {
                LOG_WARN("[Main] DEVSCALE: 보유분 조회 실패(" + error_text(balance) + ") — 스캔 제외 미적용(중복 위험)");
            }
            else
            {
                for (const Holding& holding : balance->holdings)
                {
                    mark_symbol(held, engine.symbols().intern(holding.ticker), symbol_capacity); // 잔고 티커는 문자열
                    ++held_count;
                }
            }

            // 당일 매수분은 DevScale이 다시 맡는다(재인수). 분할 매수가 있으면 기존대로 청산 관리에 넘긴다.
            //  넘김 모드면 최근 20일 원장까지 봐서 전날 넘긴 보유도 되찾는다.
            if (base.buy_split_steps == 0)
            {
                const int lookback_days = base.market_close_hhmm >= devscale_rules::kNoMarketCloseHhmm ? 20 : 0;

                for (const std::string& ticker : tickers_bought_recently(base.id_prefix, lookback_days))
                {
                    const symbol::SymbolId symbol = engine.symbols().intern(ticker); // 원장 CSV의 문자열 티커 — 여기서 id가 된다

                    if (has_symbol(held, symbol) && !has_symbol(context.basket_owned, symbol)) // 바스켓 것은 바스켓이 인수한다 [why D-109]
                    {
                        held[symbol] = false;
                        --held_count;
                        reinstated.push_back(symbol);
                    }
                }
            }

            LOG_INFO("[Main] DEVSCALE: 보유분 " + std::to_string(held_count) +
                     "종목 스캔 제외(청산 관리 전담), " + (base.market_close_hhmm >= devscale_rules::kNoMarketCloseHhmm ? "최근 매수분 " : "당일 매수분 ") +
                     std::to_string(reinstated.size()) + "종목 재인수");
        }
        else
        {
            LOG_WARN("[Main] DEVSCALE: 보유분 조회 인증 실패 — 스캔 제외 미적용(중복 위험)");
        }
    }

    // 티커 → DeviationScale 인스턴스 팩토리 (초기 스캔·주기적 재스캔 공용).
    //  &engine 참조 캡처: factory는 engine에 저장(set_universe_rescan)되어 engine 생존 중에만
    //   호출되므로 참조 수명 안전. 스캔이 register_ticker_name으로 이름을 먼저 등록하므로
    //   여기서 조회해 전략에 주입 → 로그에 "티커(종목명)" 노출(id()·데이터키는 티커 그대로).
    auto score_state = std::make_shared<DevScaleScoreState>(symbol_capacity);

    auto factory = [base, &engine, score_state](symbol::SymbolId symbol) -> std::unique_ptr<StrategyBase>
    {
        DeviationScaleStrategy::Params deviation_parameters = base;
        deviation_parameters.ticker = engine.symbols().name(symbol).string(); // 전략 파라미터·id()는 아직 문자열
        deviation_parameters.name   = engine.ticker_name(symbol);

        if (symbol < score_state->multiplier.size())
        {
            std::lock_guard<std::mutex> lock(score_state->mutex);

            if (score_state->multiplier[symbol] > 0.0)
            {
                deviation_parameters.size_mult = score_state->multiplier[symbol];
            }

            if (score_state->krw[symbol] > 0.0)
            {
                deviation_parameters.notional_krw = score_state->krw[symbol];
            }
        }

        return std::make_unique<DeviationScaleStrategy>(std::move(deviation_parameters));
    };

    if (node.value("universe_from_scan", false))
    {
        // ── 전체 시장 자동 선정 (거래대금 상위 축들의 합집합) ─────────
        //  1단(스캐너): 시세 표 거래대금 상위 + KIS 거래대금·거래증가율 상위 + 업종 축의
        //             합집합을 최소·최대가 필터로 압축 + 정배열 프리필터. 시총 축은 D-146에서 뺐다.
        //  2단(전략): 등록된 각 DeviationScale이 자기 일봉으로 정배열+눌림 존을 판정 →
        //             자격 종목만 실제 오실레이션.
        universe::DevScanCfg scan_config;
        // 거래대금 상위 스캔 수(장중 급변). 30을 넘기면 가격 구간을 갈라 두 번 부르므로 REST 호출이
        //  하나 는다 — 그 대신 ETF를 뺀 개별주를 60종목까지 볼 수 있다.
        scan_config.value_top_n     = node.value("value_top_n", 30);
        scan_config.turnover_top_n  = node.value("turnover_top_n", 0);  // 시세 표 거래대금 상위 N, 0=끄기
        scan_config.min_price       = node.value("min_price", 5000.0);
        scan_config.max_price       = node.value("max_price", 0.0);   // 0이면 상한 없음(고가주 포함)
        scan_config.max_register    = node.value("max_universe", 40);
        scan_config.risk_off_index    = node.value("risk_off_index_pct", -0.02);
        scan_config.kosdaq_enabled      = node.value("kosdaq_enabled", false);              // 코스닥 참여(기본 off, 백테스트 통과 후 개방)
        scan_config.risk_off_index_kosdaq = node.value("risk_off_index_pct_kosdaq", -0.015);  // 코스닥 지수 risk_off 임계(코스피보다 보수적)

        // 재개 임계와 최소 체류 — 차단 임계와 갈라 두어 경계 근처 토글을 없앤다. [why D-033]
        scan_config.risk_off_index_resume        = node.value("risk_off_resume_pct", -0.012);
        scan_config.risk_off_index_kosdaq_resume = node.value("risk_off_resume_pct_kosdaq", -0.009);
        scan_config.risk_off_dwell_sec         = node.value("risk_off_dwell_sec", 600);
        scan_config.require_aligned = node.value("require_aligned", true);  // 정배열 프리필터 on/off
        scan_config.align_lookup_max = node.value("align_lookup_max", 60);    // 정배열 검사 후보 상한(일봉 조회 비용 캡)

        // 장중 일봉 재조회 — 0이면 기존 동작(하루 한 번 조회 후 캐시 고정).
        for (const auto& element : jsonx::array_or_empty(node, "sector_codes"))
        {
            if (element.is_string())
            {
                scan_config.sector_codes.push_back(element.get<std::string>());
            }
        }

        scan_config.sector_top_n   = node.value("sector_top_n", 10);
        scan_config.sector_min_change = node.value("sector_min_chg", 0.0);
        // 후보 합집합(KIS 랭킹·업종 REST) 갱신 주기. 미지정이면 재스캔 주기와 같아
        //  기존 동작(재스캔마다 새로 수집)이 유지된다.
        scan_config.union_refresh_sec = node.value("union_refresh_sec", 0);
        scan_config.max_deviation_percent     = node.value("max_dev_pct", 0.0);       // 과확장 컷(일봉 이격 상한, 0=비활성)
        scan_config.universe_file   = node.value("universe_file", std::string()); // data.go.kr 유니버스 피드(ETF-free·30행캡 우회), 비면 KIS 랭킹만
        scan_config.prices_file     = node.value("prices_file", std::string());  // 전 종목 장중 시세 파일(네이버 벌크 보조 프로세스)
        scan_config.market_board    = node.value("market_board", false);         // 엔진 안 시세판이 시세·재랭킹을 맡는다(D-147)

        if (scan_config.market_board)
        {
            universe::MarketBoard::Config board_config;
            board_config.period_sec     = node.value("market_board_period_sec", board_config.period_sec);
            board_config.rerank_sec     = node.value("market_board_rerank_sec", board_config.rerank_sec);
            board_config.n_market_value = node.value("market_board_n_market_value", board_config.n_market_value);
            board_config.n_turnover     = node.value("market_board_n_turnover", board_config.n_turnover);
            board_config.min_turnover   = node.value("market_board_min_turnover", board_config.min_turnover);
            board_config.universe_out   = scan_config.universe_file; // 알림·대시보드·백필 스크립트가 이 파일을 읽는다
            universe::MarketBoard::instance().start(board_config);   // 슬리브가 여럿이어도 한 번만 뜬다
        }

        scan_config.min_turnover    = node.value("min_turnover", 0.0);           // 거래대금 하한(원), 0=비활성
        scan_config.full_market     = node.value("full_market", false);          // 후보 풀을 전 종목으로
        scan_config.align_daily_n   = base.daily_lookback;               // 정배열 판정용 일봉 개수(≥20)
        // 장 전 일봉 캐시 데우기 — 이 시각(HHMM)까지 시세판 목록의 일봉을 미리 받는다. 0=끔. [why D-147]
        scan_config.daily_warm_until_hhmm = node.value("daily_warm_until_hhmm", 0);

        if (scan_config.market_board && scan_config.daily_warm_until_hhmm > 0 && context.has_quote_kis)
        {
            universe::start_daily_warm(context.quote_kis_config, scan_config); // 슬리브가 여럿이어도 한 번만 뜬다
        }

        // 횡단면 스코어러(2026-08-09 회의 Task 4) — score_top_n>0이면 정배열 통과분을
        //  점수 랭킹해 상위 N만 등록(오너 원안 "점수 내고 5개"). 0=기존 동작(전체 등록).
        scan_config.score_top_n      = node.value("score_top_n", 0);
        scan_config.score_weight_trend    = node.value("score_w_trend", 1.0);
        scan_config.score_weight_pullback = node.value("score_w_pullback", 1.0);
        // 변동성은 감점 축 — 같은 추세·눌림이면 덜 흔들리는 쪽에 비중을 준다.
        scan_config.score_weight_volume      = node.value("score_w_vol", 0.5);
        // 거래대금 축(기본 0=비활성). 켜면 같은 조건에서 두꺼운 종목이 위로 올라온다.
        scan_config.score_weight_liquidity = node.value("score_w_liquidity", 0.0);
        // 정배열 마지막 조건(SMA10>SMA20)의 허용오차. 기본 0=기존 엄격 판정.
        scan_config.align_moving_average_tolerance_percent  = node.value("align_ma_tol_pct", 0.0);
        int rescan_sec     = node.value("rescan_interval_sec", 600); // 주기적 재스캔 간격(초)
        // 스캔에서 이만큼 연속으로 빠진 종목의 전략을 뗀다(보유·선점 없을 때만). 0=안 뗌.
        int drop_after_sec = node.value("rescan_drop_after_sec", 1800);
        // 같은 시계로 이만큼 빠지면 떼기 전에 신규매수부터 막는다. 0=안 막음(기본 — 실제 값은 config가 준다).
        //  복귀는 present 스캔이 return_confirm회 연속일 때만. 판정은 core/UniverseExit.h [why D-077].
        int block_after_sec = universe_exit::clamp_block(node.value("rescan_block_after_sec", 0), drop_after_sec);
        int return_confirm  = node.value("rescan_return_confirm", 2);

        if (block_after_sec != node.value("rescan_block_after_sec", 0))
        {
            LOG_WARN("[Main] rescan_block_after_sec " + std::to_string(node.value("rescan_block_after_sec", 0)) +
                     "이 rescan_drop_after_sec " + std::to_string(drop_after_sec) + "보다 커서 해제 시각에 맞춘다");
        }

        // 유니버스 산출 콜백 — 초기 등록·주기적 재스캔 공용(config 값 복사 캡처).
        //  &engine 캡처의 수명 안전은 위 factory와 동일. 스캔 결과 종목명을 엔진 라벨 맵에 등록해 로그에 노출.
        //  보유분 제외 필터는 초기 등록과 재스캔이 보는 잔고가 달라 호출자별로 쥌운다(아래 둘).
        // 비중 배분 파라미터 — spread는 최상위/최하위 배수 폭, target_pct는 베이스 명목 총합 목표.
        //  베이스 총합(base_percent x 슬롯)이 총노출 상한을 넘으면 매수가 무더기로 거부되므로
        //  스캔이 매회 슬롯 수 기준으로 배수를 재정규화한다.
        //  spread는 [0, 1]로 자른다. 1을 넘으면 점수 하위 쪽 배수가 음수가 되고, 팩토리는 0 이하 배수를 버리고
        //  기본 배수 1.0으로 되돌아가 하위 종목을 도리어 크게 산다.
        const double weight_spread_config = node.value("weight_spread", 0.6);
        double       weight_spread        = weight_spread_config;

        if (weight_spread > 1.0)
        {
            weight_spread = 1.0;
        }
        else if (!(weight_spread >= 0.0)) // 음수와 NaN
        {
            weight_spread = 0.0;
        }

        if (weight_spread != weight_spread_config)
        {
            LOG_WARN("[Main] " + base.id_prefix + " weight_spread " + std::to_string(weight_spread_config) +
                     "는 0~1 밖이라 " + std::to_string(weight_spread) + "로 자른다");
        }

        const double weight_target = node.value("weight_target_pct", 0.80);
        const double weight_base   = base.base_percent;
        // 원 단위 사이징 — 둘 다 0보다 크면 자본%·정규화 배수 대신 이 구간을 쓴다(D-036).
        const double krw_floor = node.value("notional_floor_krw", 0.0);
        const double krw_cap   = node.value("notional_cap_krw", 0.0);
        const double krw_cap_z = node.value("notional_cap_z", 1.5);
        const bool   krw_on    = krw_floor > 0.0 && krw_cap >= krw_floor;

        if (krw_on)
        {
            LOG_INFO("[Main] " + base.id_prefix + " 원 사이징: 종목당 " +
                     std::to_string(static_cast<long long>(krw_floor)) + "~" +
                     std::to_string(static_cast<long long>(krw_cap)) + "원, 천장 z>=" +
                     std::to_string(krw_cap_z));
        }

        auto scan_buffers = std::make_shared<DevScaleScanBuffers>();
        auto scan_fn = [scan_config, &engine, score_state, scan_buffers, merger = context.priority_merger, weight_spread, weight_target, weight_base, sleeve_id = base.id_prefix,
                        krw_on, krw_floor, krw_cap, krw_cap_z](KisClient& kis)
        {
            // 스캐너가 응답의 문자열 티커를 종목 테이블에 한 번 넣고 id·이름·점수를 준다.
            universe::ScanResult scan =
                universe::scan_devscale(kis, scan_config, engine.symbols(), scan_buffers->quotes);

            for (size_t index = 0; index < scan.symbols.size(); ++index)
            {
                engine.register_ticker_name(scan.symbols[index], scan.names[index]);
            }

            // 점수의 두 가지 용도 — (a) 누가 먼저 슬롯을 차지하는가(랭크), (b) 얼마를 사는가(배수).
            universe::score_to_mult(scan.scores, weight_spread, weight_target, weight_base, engine.risk_max_positions(),
                                    scan_buffers->multiplier, scan_buffers->sorted_raw);
            const std::vector<double>& multiplier = scan_buffers->multiplier;
            {
                std::lock_guard<std::mutex> lock(score_state->mutex);

                // 팩토리는 전략 생성 시점에 한 번만 읽으므로, 여기서 값을 덮어써도
                //  이미 분할 매수를 타는 전략의 예산은 흔들리지 않는다(신규 등록분에만 반영).
                for (size_t index = 0; index < scan.scores.size(); ++index)
                {
                    const symbol::SymbolId symbol = scan.scores[index].symbol;

                    if (symbol < score_state->multiplier.size())
                    {
                        score_state->multiplier[symbol] = multiplier[index];
                    }
                }

                if (krw_on)
                {
                    std::vector<double>& krw = scan_buffers->krw;
                    score_to_krw(scan.scores, krw_floor, krw_cap, krw_cap_z, krw);

                    for (size_t index = 0; index < scan.scores.size(); ++index)
                    {
                        const symbol::SymbolId symbol = scan.scores[index].symbol;

                        if (symbol < score_state->krw.size())
                        {
                            score_state->krw[symbol] = krw[index];
                        }
                    }
                }
            }

            publish_entry_priority(engine, *merger, sleeve_id, std::move(scan.scores));
            return std::move(scan.symbols);
        };
        auto drop_held = [](std::vector<symbol::SymbolId>& scanned, const std::vector<bool>& parts)
        {
            std::erase_if(scanned, [&parts](symbol::SymbolId symbol) { return has_symbol(parts, symbol); });
        };

        // 초기 등록용 — load_strategies는 engine.start()(bootstrap_ledger 포함) 전에 돌아
        //  OrderGate 원장이 아직 비어 있다. 기동 시 직접 조회한 잔고 스냅샷을 쓴다.
        //  이 함수 안에서 한 번만 부르므로 참조로 잡는다(universe_rescan은 엔진에 저장되어 사본이 필요하다).
        auto universe_initialize = [&scan_fn, &drop_held, &held, &reinstated](KisClient& kis)
        {
            auto scanned = scan_fn(kis);
            drop_held(scanned, held);

            for (const symbol::SymbolId symbol : reinstated) // 오늘 스캔에서 빠졌어도 보유분이라 맡는다
            {
                if (std::find(scanned.begin(), scanned.end(), symbol) == scanned.end())
                {
                    scanned.push_back(symbol);
                }
            }

            return scanned;
        };
        // 주기적 재스캔용 — 매회 OrderGate 원장에서 현재 보유를 다시 읽는다. 청산 관리가 청산한
        //  종목은 그 시점부터 다시 후보가 된다(기동 스냅샷 고정이 유니버스를 굳히던 문제).
        //  이미 등록된 종목은 재스캔이 추가만 하므로 자기 보유분으로 등록이 풀리진 않는다.
        auto universe_rescan = [scan_fn, drop_held, scan_buffers, &engine](KisClient& kis)
        {
            auto               scanned = scan_fn(kis);
            std::vector<bool>& current = scan_buffers->held;
            current.assign(engine.symbols().capacity(), false);

            for (const auto& held_position : engine.held_positions())
            {
                mark_symbol(current, held_position.symbol, current.size());
            }

            for (const symbol::SymbolId basket_symbol : engine.slot_exempt_symbols()) // 바스켓 소유 종목(파일이 바뀌면 여기서 따라온다) [why D-109]
            {
                mark_symbol(current, basket_symbol, current.size());
            }

            drop_held(scanned, current);
            return scanned;
        };

        std::vector<symbol::SymbolId> seeded; // 기동 등록 종목 — 재스캔 슬리브의 소유로 넘겨 차단·해제 대상에 넣는다

        if (!context.has_quote_kis)
        {
            LOG_ERROR("[Main] DEVSCALE universe_from_scan: quote_kis(실전 시세 키) 미설정 — 스캔 불가, 건너뜀");
        }
        else
        {
            KisClient scan_kis(context.quote_kis_config);

            if (!scan_kis.authenticate())
            {
                LOG_ERROR("[Main] DEVSCALE universe_from_scan: 시세 키 인증 실패 — 건너뜀");
            }
            else
            {
                const std::vector<symbol::SymbolId> scanned = universe_initialize(scan_kis);
                int                                 added   = 0;

                for (symbol::SymbolId symbol : scanned)
                {
                    add_gated(context, factory(symbol));
                    mark_symbol(covered, symbol, symbol_capacity); // 청산 관리 중복 부착 방지용
                    LOG_INFO("[Main]   + " + base.id_prefix + " 초기 " + engine.symbols().name(symbol).string());
                    seeded.push_back(symbol);
                    ++added;
                }

                LOG_INFO("[Main] " + base.id_prefix + " universe_from_scan: 초기 " + std::to_string(added) +
                         "종목 등록 (각자 정배열+눌림 존 게이트로 자체 선별)");
            }

            // 주기적 재스캔 등록(동적) — data_thread가 rescan_sec마다 universe_fn을 재호출해
            //  신규 티커를 런타임 add하고, drop_after_sec 이상 빠져 있는 티커는 뗀다.
            //  인증 실패해도 재스캔은 엔진 내부 시세 클라이언트로 시도.
            // 등록 총수 상한은 스캔 1회 상한(max_universe)과 같게 둔다 — 해제가 느리게 따라오므로
            //  상한이 없으면 총수가 그 값을 넘어 는다.
            engine.set_universe_rescan(std::move(universe_rescan), gate_factory(context, factory), rescan_sec,
                                       static_cast<size_t>(scan_config.max_register), drop_after_sec, block_after_sec,
                                       return_confirm);
            engine.seed_universe_rescan(seeded);
            LOG_INFO("[Main] " + base.id_prefix + " 주기적 재스캔 활성: " + std::to_string(rescan_sec) +
                     "초 간격, 이탈 차단 " + std::to_string(block_after_sec) + "초(복귀 확인 " +
                     std::to_string(return_confirm) + "회), 해제 " + std::to_string(drop_after_sec) + "초");
        }
    }
    else
    {
        std::string ticker;

        if (!require_ticker(node, "DEVSCALE", ticker))
        {
            return;
        }

        const symbol::SymbolId symbol = engine.symbols().intern(ticker); // 설정의 문자열 티커는 여기서 id가 된다
        add_gated(context, factory(symbol));
        mark_symbol(covered, symbol, symbol_capacity);
    }

    // 보유분 청산 관리 — 스캔에 안 잡힌 잔고 보유분에 청산 전용 ITB 부착(옵션).
    //  여기서 바로 붙이지 않고 전 슬리브 로드가 끝난 뒤에 붙인다. 이 슬리브의 covered만 보면
    //  뒤에 로드되는 슬리브가 방금 산 종목이 "스캔 밖 보유분"으로 보여 청산 관리가
    //  겹쳐 붙고, 그 청산 관리의 seed-trail 매도가 슬리브의 잔여 매도와 같은 주식을 두고
    //  경합한다(09-11 09:26~ ITB_112610·267250·014530 매도가능수량 0 거부 반복).
    if (context.scan_covered.size() < covered.size())
    {
        context.scan_covered.resize(covered.size(), false);
    }

    for (size_t symbol = 0; symbol < covered.size(); ++symbol)
    {
        if (covered[symbol])
        {
            context.scan_covered[symbol] = true;
        }
    }

    if (node.contains("manage_holdings") && node["manage_holdings"].value("enabled", false))
    {
        context.pending_exit_managers = &node["manage_holdings"];
        context.guard_gated           = (context.pending_regimes != nullptr);
        context.guard_regimes         = context.guard_gated ? *context.pending_regimes : std::vector<Regime>{};
    }
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
        std::move(parameters), [&engine](const std::vector<std::string>& tickers) { engine.set_slot_exempt_tickers(tickers); });
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
    pass.priority_merger = std::make_shared<EntryPriorityMerger>();

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
