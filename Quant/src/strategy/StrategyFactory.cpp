#include "strategy/StrategyFactory.h"
#include "core/Engine.h"
#include "core/Types.h"
#include "strategy/DeviationScaleStrategy.h"
#include "strategy/FixedIntervalStrategy.h"
#include "strategy/IntradayBreakoutStrategy.h"
#include "strategy/MACrossStrategy.h"
#include "strategy/MarketMakingStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "strategy/PriceTargetStrategy.h"
#include "strategy/SupplyDemandPullbackStrategy.h"
#include "strategy/ThemeStrategy.h"
#include "strategy/ValueContraryStrategy.h"
#include "universe/ScoreWeight.h"
#include "universe/UniverseScanner.h"
#include "utils/Logger.h"
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

using json = nlohmann::json;

// ─── 활성 국면 부착 ─────────────────────────────────────────────────────────
//  Engine은 직전 전략 하나에만 국면을 붙일 수 있다(set_last_active_regimes). 한 config 항목이
//  N개를 등록하는 로더(유니버스·보유 전 종목)에서 N-1개가 빠지지 않도록, 추가 지점마다 붙인다.
//  s_pending_regimes는 load_strategies가 로더를 부르는 동안만 유효하다(그 밖에서는 nullptr).
static const std::vector<Regime>* s_pending_regimes = nullptr;

// 보유분 청산 관리 부착은 슬리브 하나가 아니라 전 슬리브가 등록된 뒤에 한 번만 한다.
//  s_scan_covered는 모든 DEVIATION_SCALE 슬리브가 담당하는 티커의 합집합이고, 청산 관리 설정은
//  마지막으로 manage_holdings.enabled를 켠 슬리브의 것을 쓴다(현재 구성은 하나만 켠다).
static std::set<std::string> s_scan_covered;
static json                  s_pending_guardians;
static bool                  s_guard_gated = false;
static std::vector<Regime>   s_guard_regimes;

static void add_gated(Engine& engine, std::unique_ptr<StrategyBase> strat)
{
    if (s_pending_regimes && strat)
    {
        strat->set_active_regimes(*s_pending_regimes);
    }

    engine.add_strategy(std::move(strat));
}

// 재스캔처럼 Engine이 나중에 factory를 직접 부르는 경로용 — 국면을 factory 안에 묶는다.
static std::function<std::unique_ptr<StrategyBase>(const std::string&)>
gate_factory(std::function<std::unique_ptr<StrategyBase>(const std::string&)> factory)
{
    if (!s_pending_regimes)
    {
        return factory;
    }

    std::vector<Regime> ar = *s_pending_regimes;
    return [factory = std::move(factory), ar](const std::string& t) {
        auto strat = factory(t);

        if (strat)
        {
            strat->set_active_regimes(ar);
        }

        return strat;
    };
}

// 필수 "ticker" 키. 없으면 예외로 기동이 죽는 대신 경고 후 그 항목만 건너뛴다.
static bool require_ticker(const json& s, const char* type, std::string& out)
{
    out = s.value("ticker", std::string());

    if (out.empty())
    {
        LOG_WARN(std::string("[Main] ") + type + " 설정에 ticker 없음 — 등록 건너뜀");
    }

    return !out.empty();
}

// ─── MA_CROSS ───────────────────────────────────────────────────────────────
static void load_ma_cross(StrategyLoadCtx& ctx, const json& s)
{
    Engine& engine = ctx.engine;
    int qty = s.value("quantity", 1);
    int sp = s.value("short_period", 0);
    int lp = s.value("long_period", 0);

    // calc_ma는 prices_(최대 lp개) 끝에서 sp/lp회 역참조한다. sp<1이거나 sp>=lp면
    // begin 이전 역참조(UB/크래시)가 되므로 등록을 건너뛴다.
    if (sp < 1 || sp >= lp)
    {
        LOG_ERROR("[Main] MACross 설정 오류: short_period(" + std::to_string(sp) +
                  ")는 1 이상이고 long_period(" + std::to_string(lp) + ") 미만이어야 함 — 등록 건너뜀");
        return;
    }

    if (s.value("universe_from_balance", false))
    {
        // 모의계좌 보유종목 전체를 유니버스로 — 종목마다 MACross 등록.
        // 보유분은 start_in_position=true 로 시드 → 데드크로스에 실제 보유수량 매도, 골든크로스에 재매수.
        KisClient bal_kis(ctx.kis_cfg);

        if (!bal_kis.authenticate())
        {
            LOG_ERROR("[Main] universe_from_balance: 잔고조회용 인증 실패 — 건너뜀");
        }
        else
        {
            const KisResult<AccountBalance> bal = bal_kis.get_balance();
            int added = 0;

            if (!bal)
            {
                LOG_WARN("[Main] universe_from_balance: 잔고 조회 실패(" + bal.error_text() + ")");
            }
            else
            {
                for (const Holding& h : bal->holdings)
                {
                    add_gated(engine, std::make_unique<MACrossStrategy>(
                        h.ticker, sp, lp, h.qty, /*start_in_position=*/true));
                    LOG_INFO("[Main]   + MACross " + h.ticker + " 보유 " + std::to_string(h.qty) + "주 (in_position 시드)");
                    ++added;
                }
            }

            LOG_INFO("[Main] universe_from_balance: 보유 " + std::to_string(added) +
                     "종목 등록 (short=" + std::to_string(sp) + " long=" + std::to_string(lp) + ")");
        }
    }
    else
    {
        std::string ticker;

        if (!require_ticker(s, "MACross", ticker))
        {
            return;
        }

        add_gated(engine, std::make_unique<MACrossStrategy>(ticker, sp, lp, qty));
    }
}

// ─── INTRADAY_BREAKOUT ──────────────────────────────────────────────────────
static void load_intraday_breakout(StrategyLoadCtx& ctx, const json& s)
{
    Engine& engine = ctx.engine;
    // 첫 장중 자동매매 기준(ITB) — WS 체결 틱 기반 채널돌파 + 트레일/하드 스탑.
    int channel_min   = s.value("channel_min", 10);
    double eps         = s.value("breakout_eps", 0.002);
    double trail_pct   = s.value("trail_pct", 0.010);
    double hard_pct    = s.value("hard_pct", 0.015);
    int eod_hhmm       = s.value("eod_hhmm", 1515);
    int cooldown_sec   = s.value("reentry_cooldown_sec", 60);
    int entry_qty      = s.value("entry_qty", 1); // 신규 돌파 진입 수량(명목 미지정 시)
    double avg_loss_pct = s.value("avg_loss_pct", 0.0); // 평단 대비 손절률(0=비활성)
    // ── v2 파라미터(strategies/ITB/SPEC.md §2/§3) ──
    double seed_trail_pct      = s.value("seed_trail_pct", 0.0);      // 물린분 앵커 트레일(넓게)
    double exit_near_avg_pct   = s.value("exit_near_avg_pct", 0.0);   // 물린분 본전탈출 임계
    int    no_new_entry_hhmm   = s.value("no_new_entry_hhmm", 0);     // 신규진입 금지 시각(0→eod)
    double notional_per_position = s.value("notional_per_position", 0.0); // 종목당 명목(원)

    if (s.value("universe_from_scan", false))
    {
        // ── 거래대금 상위 스캔 유니버스(ITB v2) ─────────────────────────
        //  실전 도메인 키로 거래대금 랭킹 → 등락률/가격 필터 → (opt)수급 → 레짐 게이트.
        universe::ItbScanCfg sc;
        sc.scan_top_n   = s.value("scan_top_n", 30);
        sc.chg_min      = s.value("chg_min", 0.02);
        sc.chg_max      = s.value("chg_max", 0.12);
        sc.min_price    = s.value("min_price", 3000.0);
        sc.sd_filter    = s.value("sd_filter", true);
        sc.risk_off_idx = s.value("risk_off_index_pct", -0.01);
        sc.risk_off_idx_resume = s.value("risk_off_resume_pct", sc.risk_off_idx);
        sc.risk_off_dwell_sec  = s.value("risk_off_dwell_sec", 0);
        sc.max_register = s.value("max_concurrent_positions", 3) * 2; // 후보는 상한의 2배까지 등록(경쟁)

        if (!ctx.has_quote_kis)
        {
            LOG_ERROR("[Main] universe_from_scan: quote_kis(실전 시세 키) 미설정 — 스캔 불가, 건너뜀");
        }
        else
        {
            KisClient scan_kis(ctx.quote_kis_cfg);

            if (!scan_kis.authenticate())
            {
                LOG_ERROR("[Main] universe_from_scan: 시세 키 인증 실패 — 건너뜀");
            }
            else
            {
                auto cands = universe::scan_itb(scan_kis, sc);

                for (const auto& c : cands)
                {
                    auto strat = std::make_unique<IntradayBreakoutStrategy>(
                        c.ticker, entry_qty, /*hold_qty=*/0, /*start_in_position=*/false,
                        channel_min, eps, trail_pct, hard_pct, eod_hhmm, cooldown_sec,
                        /*avg_px=*/0.0, avg_loss_pct, seed_trail_pct, exit_near_avg_pct,
                        no_new_entry_hhmm, notional_per_position, /*day_open_px=*/c.day_open);
                    strat->set_name(c.name);
                    add_gated(engine, std::move(strat));
                }
            }
        }
    }
    else if (s.value("universe_from_balance", false))
    {
        // 모의계좌 보유종목 전체를 유니버스로 — 종목마다 ITB 등록(보유분 in_position 시드).
        KisClient bal_kis(ctx.kis_cfg);

        if (!bal_kis.authenticate())
        {
            LOG_ERROR("[Main] ITB universe_from_balance: 잔고조회용 인증 실패 — 건너뜀");
        }
        else
        {
            const KisResult<AccountBalance> bal = bal_kis.get_balance();
            int added = 0;

            if (!bal)
            {
                LOG_WARN("[Main] ITB universe_from_balance: 잔고 조회 실패(" + bal.error_text() + ")");
            }
            else
            {
                for (const Holding& h : bal->holdings)
                {
                    auto strat = std::make_unique<IntradayBreakoutStrategy>(
                        h.ticker, entry_qty, h.qty, /*start_in_position=*/true, channel_min, eps,
                        trail_pct, hard_pct, eod_hhmm, cooldown_sec, h.avg_price, avg_loss_pct,
                        seed_trail_pct, exit_near_avg_pct, no_new_entry_hhmm,
                        /*notional=*/0.0, /*day_open_px=*/0.0);
                    strat->set_name(h.name);
                    engine.register_ticker_name(h.ticker, h.name); // 로그 라벨(보유분 종목명)
                    add_gated(engine, std::move(strat));
                    LOG_INFO("[Main]   + ITB " + h.ticker + " " + h.name + " 보유 " + std::to_string(h.qty) +
                             "주 (in_position 시드, 평단=" + std::to_string((long long)h.avg_price) + ")");
                    ++added;
                }
            }

            LOG_INFO("[Main] ITB universe_from_balance: 보유 " + std::to_string(added) + "종목 등록");
        }
    }
    else
    {
        std::string ticker;

        if (!require_ticker(s, "ITB", ticker))
        {
            return;
        }

        add_gated(engine, std::make_unique<IntradayBreakoutStrategy>(
            ticker, entry_qty, /*hold_qty=*/0, /*start_in_position=*/false,
            channel_min, eps, trail_pct, hard_pct, eod_hhmm, cooldown_sec,
            /*avg_px=*/0.0, avg_loss_pct, seed_trail_pct, exit_near_avg_pct,
            no_new_entry_hhmm, notional_per_position, /*day_open_px=*/0.0));
    }
}

// ─── MOMENTUM ───────────────────────────────────────────────────────────────
static void load_momentum(StrategyLoadCtx& ctx, const json& s)
{
    int qty = s.value("quantity", 1);
    std::string ticker;

    if (!require_ticker(s, "Momentum", ticker))
    {
        return;
    }

    add_gated(ctx.engine, std::make_unique<MomentumStrategy>(ticker, s.value("period", 20), qty));
}

// ─── VALUE_CONTRARY ─────────────────────────────────────────────────────────
static void load_value_contrary(StrategyLoadCtx& ctx, const json& s)
{
    int qty = s.value("quantity", 1);
    std::string market_str = s.value("market", "KR");
    Market market = (market_str == "US") ? Market::US : Market::KR;
    std::string exchange = s.value("exchange", "");
    double pbr_max = s.value("pbr_max", 1.0);
    int eod_hhmm = s.value("eod_exit_hhmm", 1520);
    add_gated(ctx.engine, std::make_unique<ValueContraryStrategy>(market, exchange, pbr_max, qty, eod_hhmm));
}

// ─── FIXED_INTERVAL ─────────────────────────────────────────────────────────
static void load_fixed_interval(StrategyLoadCtx& ctx, const json& s)
{
    std::string ticker;

    if (!require_ticker(s, "FixedInterval", ticker))
    {
        return;
    }

    int buy_qty          = s.value("buy_qty", 1);
    int sell_qty         = s.value("sell_qty", 1);
    int interval_sec     = s.value("interval_sec", 300);
    add_gated(ctx.engine, std::make_unique<FixedIntervalStrategy>(ticker, buy_qty, sell_qty, interval_sec));
}

// ─── PRICE_TARGET ───────────────────────────────────────────────────────────
static void load_price_target(StrategyLoadCtx& ctx, const json& s)
{
    std::vector<PriceTargetStrategy::PriceTarget> price_targets;

    if (s.contains("price_targets"))
    {
        for (const auto& pt : s["price_targets"])
        {
            PriceTargetStrategy::PriceTarget t;
            t.ticker       = pt.value("ticker", std::string());

            if (t.ticker.empty())
            {
                continue;
            }

            t.buy_price    = pt.value("buy_price",  0.0);
            t.sell_price   = pt.value("sell_price", 0.0);
            t.quantity     = pt.value("quantity",   1);
            t.cooldown_sec = pt.value("cooldown_sec", 60);
            price_targets.push_back(t);
        }
    }

    std::vector<PriceTargetStrategy::LimitOrder> limit_orders;

    if (s.contains("limit_orders"))
    {
        for (const auto& lo : s["limit_orders"])
        {
            PriceTargetStrategy::LimitOrder l;
            l.ticker   = lo.value("ticker", std::string());

            if (l.ticker.empty())
            {
                continue;
            }

            l.side     = (lo.value("side", "BUY") == "SELL") ? OrderSide::SELL : OrderSide::BUY;
            l.price    = lo.value("price",    0.0);
            l.quantity = lo.value("quantity", 1);
            limit_orders.push_back(l);
        }
    }

    add_gated(ctx.engine, std::make_unique<PriceTargetStrategy>(
        std::move(price_targets), std::move(limit_orders)));
}

// ─── SUPPLY_DEMAND_PULLBACK ─────────────────────────────────────────────────
static void load_supply_demand_pullback(StrategyLoadCtx& ctx, const json& s)
{
    SupplyDemandPullbackStrategy::Params sp;
    sp.market_div        = s.value("market_div",        "J");
    sp.universe_size     = s.value("universe_size",     50);
    sp.lookback_days     = s.value("lookback_days",     5);
    sp.min_dual_days     = s.value("min_dual_days",     3);
    sp.min_consec_days   = s.value("min_consec_days",   0);
    sp.net_buy_threshold = s.value("net_buy_threshold", (int64_t)0);
    sp.ma_period         = s.value("ma_period",         5);
    sp.pullback_band     = s.value("pullback_band",     0.01);
    sp.require_prev_above= s.value("require_prev_above",true);
    sp.quantity          = s.value("quantity",          10);
    sp.eod_exit_hhmm     = s.value("eod_exit_hhmm",    std::string("1500"));
    sp.stop_below_ma     = s.value("stop_below_ma",    0.0);
    std::string mode_str = s.value("entry_mode", "EOD");
    sp.mode = (mode_str == "INTRADAY")
            ? SupplyDemandPullbackStrategy::EntryMode::INTRADAY
            : SupplyDemandPullbackStrategy::EntryMode::EOD;
    add_gated(ctx.engine, std::make_unique<SupplyDemandPullbackStrategy>(sp));
}

// ─── MARKET_MAKING ──────────────────────────────────────────────────────────
static void load_market_making(StrategyLoadCtx& ctx, const json& s)
{
    std::string ticker;

    if (!require_ticker(s, "MarketMaking", ticker))
    {
        return;
    }

    int mm_qty              = s.value("quantity", 1);
    int half_spread_ticks   = s.value("half_spread_ticks", 1);
    int requote_move_ticks  = s.value("requote_move_ticks", 1);
    int min_requote_ms      = s.value("min_requote_ms", 1000); // ≥1000 권장(초당 4건 rate 백스톱)
    add_gated(ctx.engine, std::make_unique<MarketMakingStrategy>(
        ticker, mm_qty, half_spread_ticks, requote_move_ticks, min_requote_ms));
}

// ─── 보유분 청산 관리 (DEVIATION_SCALE 보조) ───────────────────────────────
//  스캔 유니버스가 잡지 못한 잔고 보유분(아침에 산 물린분 등)마다 "청산 전용" ITB를
//  붙인다. 신규진입은 no_new_entry_hhmm=1(항상 과거)로 영구 차단 → 오직 보호·청산만:
//    seed_trail_pct(넓은 앵커 트레일) + exit_near_avg_pct(본전근처 반등청산)
//    + avg_loss_pct(평단손절, 0=비활성) + EOD(eod_exit_hhmm, 기본 1600=장중 강제청산 안 함).
//  covered = 이미 스캔 전략이 담당하는 티커(중복 부착 방지). rest_price_feed 합성틱으로 on_trade 구동.
static void attach_holding_guardians(StrategyLoadCtx& ctx, const json& mh,
                                     const std::set<std::string>& covered)
{
    Engine& engine = ctx.engine;
    double seed_trail_disp    = mh.value("seed_trail_pct", 2.0);     // 표시용(%)
    double exit_near_avg_disp = mh.value("exit_near_avg_pct", 1.0);
    double seed_trail_pct     = seed_trail_disp / 100.0;             // %→비율
    double exit_near_avg_pct  = exit_near_avg_disp / 100.0;
    double avg_loss_pct       = mh.value("avg_loss_pct", 0.0) / 100.0; // 0=비활성
    int    eod_hhmm           = mh.value("eod_exit_hhmm", 1600);     // 1600=장중 강제청산 안 함(보호만)
    int    channel_min        = mh.value("channel_min", 10);
    int    cooldown_sec       = mh.value("reentry_cooldown_sec", 60);
    // 본전탈출 무장 깊이 — 평단 -arm%까지 실제로 밀려 본 적이 있어야 본전탈출이 켜진다.
    //  이게 없으면 평단 -0.2%로 보유한 종목이 재기동 첫 틱에 전량 청산된다(그건 물린 게
    //  아니라 그냥 본전이다). 부착유예는 재기동 첫 틱과 국면 배선 사이 경합을 막는다.
    double arm_disp           = mh.value("exit_near_avg_arm_pct", 3.0);
    double exit_arm_pct       = arm_disp / 100.0;
    int    guard_warmup_sec   = mh.value("guard_warmup_sec", 60);

    KisClient bal_kis(ctx.kis_cfg);

    if (!bal_kis.authenticate())
    {
        LOG_ERROR("[Main] manage_holdings: 잔고조회 인증 실패 — 청산 관리 건너뜀");
        return;
    }

    const KisResult<AccountBalance> bal = bal_kis.get_balance();

    if (!bal)
    {
        LOG_WARN("[Main] manage_holdings: 잔고 조회 실패(" + bal.error_text() + ") — 부착할 보유분 없음");
        return;
    }

    int added = 0, skipped = 0;

    for (const Holding& h : bal->holdings)
    {
        const std::string& code  = h.ticker;
        const std::string& pname = h.name;
        const int    hq = h.qty;
        const double av = h.avg_price;

        if (covered.count(code)) // 스캔 전략이 이미 담당 → 이중 부착 방지
        {
            ++skipped;
            continue;
        }

        auto strat = std::make_unique<IntradayBreakoutStrategy>(
            code, /*entry_qty=*/0, /*hold_qty=*/hq, /*start_in_position=*/true,
            channel_min, /*breakout_eps=*/0.002, /*trail_pct=*/0.010, /*hard_pct=*/0.015,
            eod_hhmm, cooldown_sec, /*avg_px=*/av, avg_loss_pct,
            seed_trail_pct, exit_near_avg_pct, /*no_new_entry_hhmm=*/1,
            /*notional=*/0.0, /*day_open_px=*/0.0);
        strat->set_exit_near_avg_arm(exit_arm_pct);
        strat->set_guard_warmup_sec(guard_warmup_sec);
        strat->set_name(pname);
        engine.register_ticker_name(code, pname); // 로그 라벨(보유분 종목명)
        engine.mark_guardian_ticker(code);        // 스캔 슬리브의 신규매수에서 제외
        add_gated(engine, std::move(strat));
        LOG_INFO("[Main]   + 청산 관리(ITB) " + code + " " + pname + " 보유 " +
                 std::to_string(hq) + "주 @평단 " + std::to_string((long long)av) +
                 " (trail=" + std::to_string(seed_trail_disp) + "% 본전탈출=" +
                 std::to_string(exit_near_avg_disp) + "%(무장 -" + std::to_string(arm_disp) + "%) 유예" +
                 std::to_string(guard_warmup_sec) + "s eod=" + std::to_string(eod_hhmm) + ")");
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
struct DevScaleScoreState
{
    std::mutex                              mu;
    std::unordered_map<std::string, double> mult;
    std::unordered_map<std::string, double> krw; // 종목당 명목 총액(원). 원 사이징이 켜진 슬리브만 채운다
};

// 점수 z → 종목당 명목(원). z≤0은 바닥, z≥cap_z는 천장, 사이는 직선.
//  [formula] krw = floor + (cap − floor) × clamp(z / cap_z, 0, 1)
//  천장은 "풀 안에서 확실히 강하다"(z)만 본다. 절대 산포 조건(모두 비슷한 장에서는 천장을 닫는
//  것)은 점수 이력이 쌓인 뒤 붙인다 — 지금은 이력이 없어 임계를 정할 근거가 없다.
static std::unordered_map<std::string, double>
score_to_krw(const std::unordered_map<std::string, double>& scores,
             double floor_krw, double cap_krw, double cap_z)
{
    std::unordered_map<std::string, double> out;
    auto z = universe::score_to_z(scores);

    for (const auto& kv : z)
    {
        double f = cap_z > 0.0 ? (std::max)(0.0, (std::min)(1.0, kv.second / cap_z)) : 0.0;
        out[kv.first] = floor_krw + (cap_krw - floor_krw) * f;
    }

    return out;
}

// 진입 우선순위 랭크는 슬리브 하나가 아니라 전 슬리브를 합쳐서 매겨야 한다. 슬리브마다
//  set_entry_priority를 부르면 나중에 스캔한 쪽이 앞 슬리브의 랭크 맵을 통째로 덮어쓰고,
//  랭크를 잃은 종목은 rank=0이 되어 우선순위 바를 건너뛴다. 두 슬리브가 20초 간격으로
//  번갈아 스캔하는 지금 구성에서는 바가 절반만 작동하는 셈이었다.
//  슬리브별 z는 각자의 풀 안에서 정규화된 값이라 슬리브를 넘는 비교는 근사다. 그래도
//  랭크가 통째로 사라지는 것보다는 낫다.
struct EntryPriorityMerger
{
    std::mutex                                                    mu;
    std::map<std::string, std::unordered_map<std::string, double>> by_sleeve;
};

static EntryPriorityMerger& priority_merger()
{
    static EntryPriorityMerger m;
    return m;
}

// 한 슬리브의 점수를 갱신하고, 전 슬리브를 합친 랭크를 엔진에 넣는다.
static void publish_entry_priority(Engine& engine, const std::string& sleeve,
                                   const std::unordered_map<std::string, double>& sco)
{
    std::unordered_map<std::string, double> merged;
    {
        std::lock_guard<std::mutex> lk(priority_merger().mu);
        priority_merger().by_sleeve[sleeve] = sco;

        for (const auto& sv : priority_merger().by_sleeve)
        {
            for (const auto& kv : sv.second)
            {
                auto it = merged.find(kv.first);

                // 같은 종목이 두 슬리브에 올라오면 높은 점수를 남긴다.
                if (it == merged.end() || kv.second > it->second)
                {
                    merged[kv.first] = kv.second;
                }
            }
        }
    }

    engine.set_entry_priority(universe::score_to_rank(merged), universe::score_to_z(merged),
                              static_cast<int>(merged.size()));
}

static void load_deviation_scale(StrategyLoadCtx& ctx, const json& s)
{
    Engine& engine = ctx.engine;
    // 공통 파라미터(티커 제외) — 스캔 유니버스/단일 종목이 함께 쓴다.
    DeviationScaleStrategy::Params base;
    base.base_pct          = s.value("base_pct", 0.05);        // 베이스 명목 = 자본의 5%
    base.max_pct           = s.value("max_pct", 0.10);         // 종목당 상한 명목 = 자본의 10%
    base.fallback_equity   = s.value("fallback_equity", 0.0);  // 잔고조회 실패 시 기준자본(원)
    base.base_qty          = s.value("base_qty", 10);          // (폴백) 주수
    base.step_qty          = s.value("step_qty", 5);           // (폴백) 주수
    base.sma_period        = s.value("sma_period", 20);
    base.dev_sell          = s.value("dev_sell_pct", 1.5);
    base.dev_buy           = s.value("dev_buy_pct", 0.8);
    base.n_rungs           = s.value("n_rungs", 2);
    base.add_below_sma_only = s.value("add_below_sma_only", true); // 점진 진입: 물타기는 기준선 아래(눌림)에서만
    // 개장 후 3분봉이 안 쌓인 구간(20봉×3분=60분)에 일봉 SMA20을 임시 기준선으로 쓴다.
    //  false면 예전대로 봉이 찰 때까지 발주하지 않는다(개장~10:00 발주 0).
    base.daily_basis_warmup = s.value("daily_basis_warmup", true);
    base.cross_guard       = s.value("ladder_cross_guard", true);  // 분할 매수 층이 현재가를 넘지 않게 앵커 클램프(D-006)
    base.pullback_pct      = s.value("pullback_pct", 2.0);
    base.entry_upper_pct   = s.value("entry_upper_pct", 0.0);   // SMA20 위 진입 허용%(0=순수 눌림만)
    base.zone_hyst_pct     = s.value("zone_hyst_pct", 4.0);     // 존 유지 여유폭(%) — 경계 진동 방지
    // 정배열 허용오차는 스캐너와 같은 값을 써야 등록·활성이 어긋나지 않아, 슬리브 설정에 없으면
    //  아래 유니버스 스캔 블록과 같은 기본값(0=엄격)을 쓴다.
    base.align_ma_tol_pct  = s.value("align_ma_tol_pct", 0.0);
    base.reprice_move_ticks = s.value("reprice_move_ticks", 2);
    base.min_rebuild_sec    = s.value("min_rebuild_sec", 0);
    base.id_prefix          = s.value("id_prefix", std::string("DEVSCALE"));
    base.entry_lower_pct    = s.value("entry_lower_pct", 0.0);
    base.anchor_on_price    = s.value("anchor_on_price", false);
    base.buy_rungs          = s.value("buy_rungs", -1);
    base.stop_loss_pct      = s.value("stop_loss_pct", 0.0);
    base.trail_sma_exit     = s.value("trail_sma_exit", false);
    base.trail_sma_tol_pct  = s.value("trail_sma_tol_pct", 1.0);
    base.stop_cooldown_sec  = s.value("stop_cooldown_sec", 900);
    base.sell_anchor_avg    = s.value("sell_anchor_avg", false);
    base.prefetch_jitter_pct = s.value("prefetch_jitter_pct", 50);
    base.eod_hhmm          = s.value("eod_exit_hhmm", 1515);
    base.interval_min      = s.value("interval_min", 3);
    base.min_action_ms     = s.value("min_action_ms", 3000);
    base.daily_lookback    = s.value("daily_lookback", 70);
    base.account           = s.value("account", std::string());

    // 스캔/단일로 실제 DeviationScale이 담당하는 티커 — 보유분 청산 관리 중복 부착 방지.
    std::set<std::string> covered;

    // 이미 보유 중인 종목은 DeviationScale 신규 스캔에서 제외 → 청산 관리가 전담(윈드다운).
    //  시드/전일 물린 보유분에 DevScale 분할 매수가 겹치면 종목당 명목상한(max_pct)을
    //  초과해 CANCEL 거부·과주문이 난다(073240 사례). 보유분=guardian, 신규만=DevScale로 분리.
    //  manage_holdings.enabled일 때만 적용(청산 관리가 있어야 보유분을 인수하므로).
    std::set<std::string> held;

    if (s.contains("manage_holdings") && s["manage_holdings"].value("enabled", false))
    {
        KisClient held_kis(ctx.kis_cfg);

        if (held_kis.authenticate())
        {
            const KisResult<AccountBalance> bal = held_kis.get_balance();

            if (!bal)
            {
                LOG_WARN("[Main] DEVSCALE: 보유분 조회 실패(" + bal.error_text() + ") — 스캔 제외 미적용(중복 위험)");
            }
            else
            {
                for (const Holding& h : bal->holdings)
                {
                    held.insert(h.ticker);
                }
            }

            LOG_INFO("[Main] DEVSCALE: 보유분 " + std::to_string(held.size()) +
                     "종목 스캔 제외(청산 관리 전담)");
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
    auto score_state = std::make_shared<DevScaleScoreState>();

    auto factory = [base, &engine, score_state](const std::string& ticker) -> std::unique_ptr<StrategyBase>
    {
        DeviationScaleStrategy::Params dp = base;
        dp.ticker = ticker;
        dp.name   = engine.ticker_name(ticker);
        {
            std::lock_guard<std::mutex> lk(score_state->mu);
            auto it = score_state->mult.find(ticker);

            if (it != score_state->mult.end() && it->second > 0.0)
            {
                dp.size_mult = it->second;
            }

            auto ik = score_state->krw.find(ticker);

            if (ik != score_state->krw.end() && ik->second > 0.0)
            {
                dp.notional_krw = ik->second;
            }
        }

        return std::make_unique<DeviationScaleStrategy>(std::move(dp));
    };

    if (s.value("universe_from_scan", false))
    {
        // ── 전체 시장 자동 선정 ("둘 다": 시총 상위 ∪ 거래대금 상위) ─────────
        //  1단(스캐너): 시총 상위(넓은 유동 유니버스) + 거래대금 상위(장중 급변 종목)의
        //             합집합을 최소·최대가 필터로 압축 + 정배열 프리필터.
        //  2단(전략): 등록된 각 DeviationScale이 자기 일봉으로 정배열+눌림 존을 판정 →
        //             자격 종목만 실제 오실레이션.
        universe::DevScanCfg sc;
        sc.scan_top_n      = s.value("scan_top_n", 80);   // 시총 상위 스캔 수(넓은 유니버스)
        sc.value_top_n     = s.value("value_top_n", 30);  // 거래대금 상위 스캔 수(장중 급변)
        sc.min_price       = s.value("min_price", 5000.0);
        sc.max_price       = s.value("max_price", 0.0);   // 0이면 상한 없음(고가주 포함)
        sc.max_register    = s.value("max_universe", 40);
        sc.risk_off_idx    = s.value("risk_off_index_pct", -0.02);
        sc.kosdaq_enabled      = s.value("kosdaq_enabled", false);              // 코스닥 참여(기본 off, 백테스트 통과 후 개방)
        sc.risk_off_idx_kosdaq = s.value("risk_off_index_pct_kosdaq", -0.015);  // 코스닥 지수 risk_off 임계(코스피보다 보수적)
        // 재개 임계와 최소 체류 — 차단 임계와 갈라 두어 경계 근처 토글을 없앤다. [why D-033]
        sc.risk_off_idx_resume        = s.value("risk_off_resume_pct", -0.012);
        sc.risk_off_idx_kosdaq_resume = s.value("risk_off_resume_pct_kosdaq", -0.009);
        sc.risk_off_dwell_sec         = s.value("risk_off_dwell_sec", 600);
        sc.require_aligned = s.value("require_aligned", true);  // 정배열 프리필터 on/off
        sc.align_probe_max = s.value("align_probe_max", 60);    // 정배열 검사 후보 상한(일봉 조회 비용 캡)

        // 장중 일봉 재조회 — 0이면 기존 동작(하루 한 번 조회 후 캐시 고정).
        for (const auto& e : s.value("sector_codes", nlohmann::json::array()))
        {
            if (e.is_string())
            {
                sc.sector_codes.push_back(e.get<std::string>());
            }
        }

        sc.sector_top_n   = s.value("sector_top_n", 10);
        sc.sector_min_chg = s.value("sector_min_chg", 0.0);
        sc.align_refresh_max = s.value("align_refresh_max", 0);
        sc.align_refresh_sec = s.value("align_refresh_sec", 600);
        // 후보 합집합(KIS 랭킹·업종 REST) 갱신 주기. 미지정이면 재스캔 주기와 같아
        //  기존 동작(재스캔마다 새로 수집)이 유지된다.
        sc.union_refresh_sec = s.value("union_refresh_sec", 0);
        sc.max_dev_pct     = s.value("max_dev_pct", 0.0);       // 과확장 컷(일봉 이격 상한, 0=비활성)
        sc.min_dev_pct     = s.value("min_dev_pct", 0.0);       // 과확장 하한(추세확장 슬리브용, 0=비활성)
        sc.universe_file   = s.value("universe_file", std::string()); // data.go.kr 유니버스 피드(ETF-free·30행캡 우회), 비면 KIS 랭킹만
        sc.prices_file     = s.value("prices_file", std::string());  // 전 종목 장중 시세 파일(네이버 벌크 보조 프로세스)
        sc.min_turnover    = s.value("min_turnover", 0.0);           // 거래대금 하한(원), 0=비활성
        sc.full_market     = s.value("full_market", false);          // 후보 풀을 전 종목으로
        sc.align_daily_n   = base.daily_lookback;               // 정배열(SMA60) 판정용 일봉 개수(≥60)
        // 횡단면 스코어러(2026-08-09 회의 Task 4) — score_top_n>0이면 정배열 통과분을
        //  점수 랭킹해 상위 N만 등록(오너 원안 "점수 내고 5개"). 0=기존 동작(전체 등록).
        sc.score_top_n      = s.value("score_top_n", 0);
        sc.score_w_trend    = s.value("score_w_trend", 1.0);
        sc.score_w_pullback = s.value("score_w_pullback", 1.0);
        sc.score_w_supply   = s.value("score_w_supply", 0.0); // 수급 로거 데이터 확보 후 제거실험
        // 변동성은 감점 축 — 같은 추세·눌림이면 덜 흔들리는 쪽에 비중을 준다.
        sc.score_w_vol      = s.value("score_w_vol", 0.5);
        // 거래대금 축(기본 0=비활성). 켜면 같은 조건에서 두꺼운 종목이 위로 올라온다.
        sc.score_w_liquidity = s.value("score_w_liquidity", 0.0);
        // 정배열 마지막 조건(SMA20>SMA60)의 허용오차. 기본 0=기존 엄격 판정.
        sc.align_ma_tol_pct  = s.value("align_ma_tol_pct", 0.0);
        int rescan_sec     = s.value("rescan_interval_sec", 600); // 주기적 재스캔 간격(초)
        // 스캔에서 이만큼 연속으로 빠진 종목의 전략을 뗀다(보유·선점 없을 때만). 0=안 뗌.
        int drop_after_sec = s.value("rescan_drop_after_sec", 1800);

        // 유니버스 산출 콜백 — 초기 등록·주기적 재스캔 공용(cfg 값 복사 캡처).
        //  &engine 캡처의 수명 안전은 위 factory와 동일. 스캔 결과 종목명을 엔진 라벨 맵에 등록해 로그에 노출.
        //  보유분 제외 필터는 초기 등록과 재스캔이 보는 잔고가 달라 호출자별로 쥌운다(아래 둘).
        // 비중 배분 파라미터 — spread는 최상위/최하위 배수 폭, target_pct는 베이스 명목 총합 목표.
        //  베이스 총합(base_pct x 슬롯)이 총노출 상한을 넘으면 매수가 무더기로 거부되므로
        //  스캔이 매회 슬롯 수 기준으로 배수를 재정규화한다.
        const double w_spread = s.value("weight_spread", 0.6);
        const double w_target = s.value("weight_target_pct", 0.80);
        const double w_base   = base.base_pct;
        // 원 단위 사이징 — 둘 다 0보다 크면 자본%·정규화 배수 대신 이 구간을 쓴다(D-036).
        const double krw_floor = s.value("notional_floor_krw", 0.0);
        const double krw_cap   = s.value("notional_cap_krw", 0.0);
        const double krw_cap_z = s.value("notional_cap_z", 1.5);
        const bool   krw_on    = krw_floor > 0.0 && krw_cap >= krw_floor;

        if (krw_on)
        {
            LOG_INFO("[Main] " + base.id_prefix + " 원 사이징: 종목당 " +
                     std::to_string(static_cast<long long>(krw_floor)) + "~" +
                     std::to_string(static_cast<long long>(krw_cap)) + "원, 천장 z>=" +
                     std::to_string(krw_cap_z));
        }

        const std::string sleeve_id = base.id_prefix;
        auto scan_fn = [sc, &engine, score_state, w_spread, w_target, w_base, sleeve_id,
                        krw_on, krw_floor, krw_cap, krw_cap_z](KisClient& c)
        {
            std::unordered_map<std::string, std::string> nm;
            std::unordered_map<std::string, double>      sco;
            auto ts = universe::scan_devscale(c, sc, &nm, &sco);

            for (auto& kv : nm)
            {
                engine.register_ticker_name(kv.first, kv.second);
            }

            // 점수의 두 가지 용도 — (a) 누가 먼저 슬롯을 차지하는가(랭크), (b) 얼마를 사는가(배수).
            auto mult = universe::score_to_mult(sco, w_spread, w_target, w_base,
                                                engine.risk_max_positions());
            {
                std::lock_guard<std::mutex> lk(score_state->mu);

                // 팩토리는 전략 생성 시점에 한 번만 읽으므로, 여기서 값을 덮어써도
                //  이미 분할 매수를 타는 전략의 예산은 흔들리지 않는다(신규 등록분에만 반영).
                for (auto& kv : mult)
                {
                    score_state->mult[kv.first] = kv.second;
                }

                if (krw_on)
                {
                    for (auto& kv : score_to_krw(sco, krw_floor, krw_cap, krw_cap_z))
                    {
                        score_state->krw[kv.first] = kv.second;
                    }
                }
            }

            publish_entry_priority(engine, sleeve_id, sco);
            return ts;
        };
        auto drop_held = [](std::vector<std::string>& ts, const std::set<std::string>& h)
        {
            if (h.empty())
            {
                return;
            }

            std::vector<std::string> keep;

            for (auto& t : ts)
            {
                if (!h.count(t))
                {
                    keep.push_back(t);
                }
            }

            ts.swap(keep);
        };

        // 초기 등록용 — load_strategies는 engine.start()(bootstrap_ledger 포함) 전에 돌아
        //  OrderGate 원장이 아직 비어 있다. 기동 시 직접 조회한 잔고 스냅샷을 쓴다.
        auto universe_init = [scan_fn, drop_held, held](KisClient& c)
        {
            auto ts = scan_fn(c);
            drop_held(ts, held);
            return ts;
        };
        // 주기적 재스캔용 — 매회 OrderGate 원장에서 현재 보유를 다시 읽는다. 청산 관리가 청산한
        //  종목은 그 시점부터 다시 후보가 된다(기동 스냅샷 고정이 유니버스를 굳히던 문제).
        //  이미 등록된 종목은 재스캔이 추가만 하므로 자기 보유분으로 등록이 풀리진 않는다.
        auto universe_rescan = [scan_fn, drop_held, &engine](KisClient& c)
        {
            auto ts = scan_fn(c);
            std::set<std::string> cur;

            for (const auto& h : engine.held_positions())
            {
                cur.insert(h.ticker);
            }

            drop_held(ts, cur);
            return ts;
        };

        if (!ctx.has_quote_kis)
        {
            LOG_ERROR("[Main] DEVSCALE universe_from_scan: quote_kis(실전 시세 키) 미설정 — 스캔 불가, 건너뜀");
        }
        else
        {
            KisClient scan_kis(ctx.quote_kis_cfg);

            if (!scan_kis.authenticate())
            {
                LOG_ERROR("[Main] DEVSCALE universe_from_scan: 시세 키 인증 실패 — 건너뜀");
            }
            else
            {
                auto tickers = universe_init(scan_kis);
                int  added   = 0;

                for (const auto& t : tickers)
                {
                    add_gated(engine, factory(t));
                    covered.insert(t); // 청산 관리 중복 부착 방지용
                    LOG_INFO("[Main]   + " + base.id_prefix + " 초기 " + t);
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
            engine.set_universe_rescan(universe_rescan, gate_factory(factory), rescan_sec,
                                       static_cast<size_t>(sc.max_register), drop_after_sec);
            LOG_INFO("[Main] " + base.id_prefix + " 주기적 재스캔 활성: " + std::to_string(rescan_sec) +
                     "초 간격, 이탈 해제 " + std::to_string(drop_after_sec) + "초");
        }
    }
    else
    {
        std::string t;

        if (!require_ticker(s, "DEVSCALE", t))
        {
            return;
        }

        add_gated(engine, factory(t));
        covered.insert(t);
    }

    // 보유분 청산 관리 — 스캔에 안 잡힌 잔고 보유분에 청산 전용 ITB 부착(옵션).
    //  여기서 바로 붙이지 않고 전 슬리브 로드가 끝난 뒤에 붙인다. 이 슬리브의 covered만 보면
    //  뒤에 로드되는 슬리브(TRENDX)가 방금 산 종목이 "스캔 밖 보유분"으로 보여 청산 관리가
    //  겹쳐 붙고, 그 청산 관리의 seed-trail 매도가 슬리브의 잔여 매도와 같은 주식을 두고
    //  경합한다(09-11 09:26~ ITB_112610·267250·014530 매도가능수량 0 거부 반복).
    s_scan_covered.insert(covered.begin(), covered.end());

    if (s.contains("manage_holdings") && s["manage_holdings"].value("enabled", false))
    {
        s_pending_guardians = s["manage_holdings"];
        s_guard_gated       = (s_pending_regimes != nullptr);
        s_guard_regimes     = s_guard_gated ? *s_pending_regimes : std::vector<Regime>{};
    }
}

// ─── THEME ──────────────────────────────────────────────────────────────────
static void load_theme(StrategyLoadCtx& ctx, const json& s)
{
    int qty = s.value("quantity", 1);
    std::vector<std::string> sector_codes;

    if (s.contains("sector_codes") && s["sector_codes"].is_array())
    {
        sector_codes = s["sector_codes"].get<std::vector<std::string>>();
    }

    int top_n            = s.value("top_n_sectors", 2);
    double vol_surge     = s.value("volume_surge_mult", 2.0);
    bool inst_filter     = s.value("inst_filter", true);
    int eod_hhmm         = s.value("eod_exit_hhmm", 1520);
    add_gated(ctx.engine, std::make_unique<ThemeStrategy>(
        sector_codes, top_n, vol_surge, inst_filter, qty, eod_hhmm));
}

// ─── 전략-국면 매핑 ────────────────────────────────────────────────────────
//  config "active_regimes"(미지정 시 전 국면)를 파싱한다. 적용은 add_gated/gate_factory가
//  로더 실행 중 추가되는 전략 전부에 한다. 반환 false = 키 없음 또는 비어 있음(전 국면).
static bool parse_active_regimes(const json& s, const std::string& type, std::vector<Regime>& ar)
{
    if (!s.contains("active_regimes") || !s["active_regimes"].is_array())
    {
        return false;
    }

    for (const auto& r : s["active_regimes"])
    {
        std::string rs = r.is_string() ? r.get<std::string>() : std::string();

        if (rs == "BULL")
        {
            ar.push_back(Regime::BULL);
        }
        else if (rs == "NEUTRAL")
        {
            ar.push_back(Regime::NEUTRAL);
        }
        else if (rs == "BEAR")
        {
            ar.push_back(Regime::BEAR);
        }
        else
        {
            LOG_WARN("[Main] " + type + " 알 수 없는 active_regimes 값: '" + rs +
                      "' (BULL/NEUTRAL/BEAR만 유효) — 무시됨");   // G-2
        }
    }

    if (ar.empty())   // G-2: 키는 있는데 파싱 결과가 비면 게이트가 조용히 무력화됨 → 경고
    {
        LOG_WARN("[Main] " + type + " active_regimes 파싱 결과 비어있음 — 전 국면 통과로 동작(게이트 무효)");
        return false;
    }

    return true;
}

// ─── 디스패치 ───────────────────────────────────────────────────────────────
void load_strategies(StrategyLoadCtx& ctx, const json& strategies)
{
    static const std::map<std::string, void (*)(StrategyLoadCtx&, const json&)> LOADERS = {
        {"MA_CROSS", load_ma_cross},
        {"INTRADAY_BREAKOUT", load_intraday_breakout},
        {"MOMENTUM", load_momentum},
        {"VALUE_CONTRARY", load_value_contrary},
        {"FIXED_INTERVAL", load_fixed_interval},
        {"PRICE_TARGET", load_price_target},
        {"SUPPLY_DEMAND_PULLBACK", load_supply_demand_pullback},
        {"MARKET_MAKING", load_market_making},
        {"DEVIATION_SCALE", load_deviation_scale},
        {"THEME", load_theme},
    };

    for (auto& s : strategies)
    {
        std::string type = s.value("type", std::string());
        size_t n_before = ctx.engine.strategy_count();

        auto it = LOADERS.find(type);

        if (it == LOADERS.end())
        {
            LOG_WARN("[Main] 알 수 없는 전략: '" + type + "'");
            continue;
        }

        std::vector<Regime> ar;
        const bool gated = parse_active_regimes(s, type, ar);
        s_pending_regimes = gated ? &ar : nullptr;
        it->second(ctx, s);
        s_pending_regimes = nullptr;

        if (gated && ctx.engine.strategy_count() > n_before)
        {
            LOG_INFO("[Main] " + type + " 활성국면: " + std::to_string(ar.size()) + "개 × 전략 " +
                     std::to_string(ctx.engine.strategy_count() - n_before) + "개");
        }
    }

    // 전 슬리브의 초기 유니버스가 확정된 뒤에야 "스캔 밖 보유분"을 가릴 수 있다.
    if (!s_pending_guardians.is_null())
    {
        s_pending_regimes = s_guard_gated ? &s_guard_regimes : nullptr;
        attach_holding_guardians(ctx, s_pending_guardians, s_scan_covered);
        s_pending_regimes = nullptr;
        s_pending_guardians = json();
    }
}
