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
#include "universe/UniverseScanner.h"
#include "utils/Logger.h"
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

// ─── MA_CROSS ───────────────────────────────────────────────────────────────
static void load_ma_cross(StrategyLoadCtx& ctx, const json& s)
{
    Engine& engine = ctx.engine;
    int qty = s.value("quantity", 1);
    int sp = s["short_period"].get<int>();
    int lp = s["long_period"].get<int>();

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
            nlohmann::json bal = bal_kis.get_balance();
            int added = 0;
            if (bal.contains("output1"))
            {
                for (auto& h : bal["output1"])
                {
                    std::string code = h.value("pdno", "");
                    int hq           = std::atoi(h.value("hldg_qty", "0").c_str());
                    if (!code.empty() && hq > 0)
                    {
                        engine.add_strategy(std::make_unique<MACrossStrategy>(
                            code, sp, lp, hq, /*start_in_position=*/true));
                        LOG_INFO("[Main]   + MACross " + code + " 보유 " + std::to_string(hq) + "주 (in_position 시드)");
                        ++added;
                    }
                }
            }
            LOG_INFO("[Main] universe_from_balance: 보유 " + std::to_string(added) +
                     "종목 등록 (short=" + std::to_string(sp) + " long=" + std::to_string(lp) + ")");
        }
    }
    else
    {
        engine.add_strategy(std::make_unique<MACrossStrategy>(
            s["ticker"].get<std::string>(), sp, lp, qty));
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
                    engine.add_strategy(std::move(strat));
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
            nlohmann::json bal = bal_kis.get_balance();
            int added = 0;
            if (bal.contains("output1"))
            {
                for (auto& h : bal["output1"])
                {
                    std::string code = h.value("pdno", "");
                    std::string pname = h.value("prdt_name", "");
                    int hq           = std::atoi(h.value("hldg_qty", "0").c_str());
                    double avg_px    = std::atof(h.value("pchs_avg_pric", "0").c_str());
                    if (!code.empty() && hq > 0)
                    {
                        auto strat = std::make_unique<IntradayBreakoutStrategy>(
                            code, entry_qty, hq, /*start_in_position=*/true, channel_min, eps,
                            trail_pct, hard_pct, eod_hhmm, cooldown_sec, avg_px, avg_loss_pct,
                            seed_trail_pct, exit_near_avg_pct, no_new_entry_hhmm,
                            /*notional=*/0.0, /*day_open_px=*/0.0);
                        strat->set_name(pname);
                        engine.register_ticker_name(code, pname); // 로그 라벨(보유분 종목명)
                        engine.add_strategy(std::move(strat));
                        LOG_INFO("[Main]   + ITB " + code + " " + pname + " 보유 " + std::to_string(hq) +
                                 "주 (in_position 시드, 평단=" + std::to_string((long long)avg_px) + ")");
                        ++added;
                    }
                }
            }
            LOG_INFO("[Main] ITB universe_from_balance: 보유 " + std::to_string(added) + "종목 등록");
        }
    }
    else
    {
        engine.add_strategy(std::make_unique<IntradayBreakoutStrategy>(
            s["ticker"].get<std::string>(), entry_qty, /*hold_qty=*/0, /*start_in_position=*/false,
            channel_min, eps, trail_pct, hard_pct, eod_hhmm, cooldown_sec,
            /*avg_px=*/0.0, avg_loss_pct, seed_trail_pct, exit_near_avg_pct,
            no_new_entry_hhmm, notional_per_position, /*day_open_px=*/0.0));
    }
}

// ─── MOMENTUM ───────────────────────────────────────────────────────────────
static void load_momentum(StrategyLoadCtx& ctx, const json& s)
{
    int qty = s.value("quantity", 1);
    ctx.engine.add_strategy(
        std::make_unique<MomentumStrategy>(s["ticker"].get<std::string>(), s["period"].get<int>(), qty));
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
    ctx.engine.add_strategy(std::make_unique<ValueContraryStrategy>(market, exchange, pbr_max, qty, eod_hhmm));
}

// ─── FIXED_INTERVAL ─────────────────────────────────────────────────────────
static void load_fixed_interval(StrategyLoadCtx& ctx, const json& s)
{
    std::string ticker   = s["ticker"].get<std::string>();
    int buy_qty          = s.value("buy_qty", 1);
    int sell_qty         = s.value("sell_qty", 1);
    int interval_sec     = s.value("interval_sec", 300);
    ctx.engine.add_strategy(std::make_unique<FixedIntervalStrategy>(ticker, buy_qty, sell_qty, interval_sec));
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
            t.ticker       = pt["ticker"].get<std::string>();
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
            l.ticker   = lo["ticker"].get<std::string>();
            l.side     = (lo.value("side", "BUY") == "SELL") ? OrderSide::SELL : OrderSide::BUY;
            l.price    = lo.value("price",    0.0);
            l.quantity = lo.value("quantity", 1);
            limit_orders.push_back(l);
        }
    }
    ctx.engine.add_strategy(std::make_unique<PriceTargetStrategy>(
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
    ctx.engine.add_strategy(std::make_unique<SupplyDemandPullbackStrategy>(sp));
}

// ─── MARKET_MAKING ──────────────────────────────────────────────────────────
static void load_market_making(StrategyLoadCtx& ctx, const json& s)
{
    std::string ticker      = s["ticker"].get<std::string>();
    int mm_qty              = s.value("quantity", 1);
    int half_spread_ticks   = s.value("half_spread_ticks", 1);
    int requote_move_ticks  = s.value("requote_move_ticks", 1);
    int min_requote_ms      = s.value("min_requote_ms", 1000); // ≥1000 권장(초당 4건 rate 백스톱)
    ctx.engine.add_strategy(std::make_unique<MarketMakingStrategy>(
        ticker, mm_qty, half_spread_ticks, requote_move_ticks, min_requote_ms));
}

// ─── 보유분 청산 가디언 (DEVIATION_SCALE 보조) ───────────────────────────────
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

    KisClient bal_kis(ctx.kis_cfg);
    if (!bal_kis.authenticate())
    {
        LOG_ERROR("[Main] manage_holdings: 잔고조회 인증 실패 — 청산 가디언 건너뜀");
        return;
    }
    nlohmann::json bal = bal_kis.get_balance();
    if (!bal.contains("output1"))
    {
        LOG_WARN("[Main] manage_holdings: 잔고 output1 없음 — 부착할 보유분 없음");
        return;
    }
    int added = 0, skipped = 0;
    for (auto& h : bal["output1"])
    {
        std::string code  = h.value("pdno", "");
        std::string pname = h.value("prdt_name", "");
        int    hq = std::atoi(h.value("hldg_qty", "0").c_str());
        double av = std::atof(h.value("pchs_avg_pric", "0").c_str());
        if (code.empty() || hq <= 0)
            continue;
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
        strat->set_name(pname);
        engine.register_ticker_name(code, pname); // 로그 라벨(보유분 종목명)
        engine.add_strategy(std::move(strat));
        LOG_INFO("[Main]   + 청산가디언(ITB) " + code + " " + pname + " 보유 " +
                 std::to_string(hq) + "주 @평단 " + std::to_string((long long)av) +
                 " (trail=" + std::to_string(seed_trail_disp) + "% 본전탈출=" +
                 std::to_string(exit_near_avg_disp) + "% eod=" + std::to_string(eod_hhmm) + ")");
        ++added;
    }
    LOG_INFO("[Main] manage_holdings: 청산 가디언 " + std::to_string(added) +
             "종목 부착, 스캔중복 " + std::to_string(skipped) + "종목 스킵");
}

// ─── DEVIATION_SCALE ────────────────────────────────────────────────────────
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
    base.pullback_pct      = s.value("pullback_pct", 2.0);
    base.entry_upper_pct   = s.value("entry_upper_pct", 0.0);   // SMA20 위 진입 허용%(0=순수 눌림만)
    base.reprice_move_ticks = s.value("reprice_move_ticks", 2);
    base.eod_hhmm          = s.value("eod_exit_hhmm", 1515);
    base.interval_min      = s.value("interval_min", 3);
    base.min_action_ms     = s.value("min_action_ms", 3000);
    base.daily_lookback    = s.value("daily_lookback", 70);
    base.account           = s.value("account", std::string());

    // 스캔/단일로 실제 DeviationScale이 담당하는 티커 — 보유분 청산 가디언 중복 부착 방지.
    std::set<std::string> covered;

    // 티커 → DeviationScale 인스턴스 팩토리 (초기 스캔·주기적 재스캔 공용).
    auto factory = [base](const std::string& ticker) -> std::unique_ptr<StrategyBase>
    {
        DeviationScaleStrategy::Params dp = base;
        dp.ticker = ticker;
        return std::make_unique<DeviationScaleStrategy>(std::move(dp));
    };

    if (s.value("universe_from_scan", false))
    {
        // ── 전체 시장 자동 선정 ("둘 다": 시총 상위 ∪ 거래대금 상위) ─────────
        //  1단(스캐너): 시총 상위(넓은 유동 유니버스) + 거래대금 상위(장중 급변 종목)의
        //             합집합을 최소·최대가 필터로 압축 + 정배열 프리필터.
        //  2단(전략): 등록된 각 DeviationScale이 자기 일봉으로 정배열+눌림 존을 판정 →
        //             자격 종목만 실제 오실레이션. 시장 스캔 + 종목별 자리판정 = 2단 선정.
        universe::DevScanCfg sc;
        sc.scan_top_n      = s.value("scan_top_n", 80);   // 시총 상위 스캔 수(넓은 유니버스)
        sc.value_top_n     = s.value("value_top_n", 30);  // 거래대금 상위 스캔 수(장중 급변)
        sc.min_price       = s.value("min_price", 5000.0);
        sc.max_price       = s.value("max_price", 0.0);   // 0이면 상한 없음(고가주 포함)
        sc.max_register    = s.value("max_universe", 40);
        sc.risk_off_idx    = s.value("risk_off_index_pct", -0.02);
        sc.require_aligned = s.value("require_aligned", true);  // 정배열 프리필터 on/off
        sc.align_probe_max = s.value("align_probe_max", 60);    // 정배열 검사 후보 상한(일봉 조회 비용 캡)
        sc.align_daily_n   = base.daily_lookback;               // 정배열(SMA60) 판정용 일봉 개수(≥60)
        // 횡단면 스코어러(2026-08-09 회의 Task 4) — score_top_n>0이면 정배열 통과분을
        //  점수 랭킹해 상위 N만 등록(오너 원안 "점수 내고 5개"). 0=기존 동작(전체 등록).
        sc.score_top_n      = s.value("score_top_n", 0);
        sc.score_w_trend    = s.value("score_w_trend", 1.0);
        sc.score_w_pullback = s.value("score_w_pullback", 1.0);
        sc.score_w_supply   = s.value("score_w_supply", 0.0); // 수급 로거 데이터 확보 후 ablation
        int rescan_sec     = s.value("rescan_interval_sec", 600); // 주기적 재스캔 간격(초)

        // 유니버스 산출 콜백 — 초기 등록과 주기적 재스캔이 공용으로 사용(cfg 값 복사 캡처).
        //  &engine 참조 캡처: universe_fn은 엔진(set_universe_rescan)에 저장되어 엔진이 살아있는
        //  동안만 호출되므로 참조 수명 안전. 스캔 결과 종목명을 엔진 라벨 맵에 등록해 로그에 노출.
        auto universe_fn = [sc, &engine](KisClient& c)
        {
            std::unordered_map<std::string, std::string> nm;
            auto ts = universe::scan_devscale(c, sc, &nm);
            for (auto& kv : nm) engine.register_ticker_name(kv.first, kv.second);
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
                auto tickers = universe_fn(scan_kis);
                int  added   = 0;
                for (const auto& t : tickers)
                {
                    engine.add_strategy(factory(t));
                    covered.insert(t); // 청산 가디언 중복 부착 방지용
                    LOG_INFO("[Main]   + DEVSCALE 초기 " + t);
                    ++added;
                }
                LOG_INFO("[Main] DEVSCALE universe_from_scan: 초기 " + std::to_string(added) +
                         "종목 등록 (각자 정배열+눌림 존 게이트로 자체 선별)");
            }

            // 주기적 재스캔 등록(동적) — data_thread가 rescan_sec마다 universe_fn을 재호출해
            //  신규 티커만 런타임 add. 인증 실패해도 재스캔은 엔진 내부 시세 클라이언트로 시도.
            engine.set_universe_rescan(universe_fn, factory, rescan_sec);
            LOG_INFO("[Main] DEVSCALE 주기적 재스캔 활성: " + std::to_string(rescan_sec) + "초 간격");
        }
    }
    else
    {
        std::string t = s["ticker"].get<std::string>();
        engine.add_strategy(factory(t));
        covered.insert(t);
    }

    // 보유분 청산 가디언 — 스캔에 안 잡힌 잔고 보유분에 청산 전용 ITB 부착(옵션).
    if (s.contains("manage_holdings") && s["manage_holdings"].value("enabled", false))
        attach_holding_guardians(ctx, s["manage_holdings"], covered);
}

// ─── THEME ──────────────────────────────────────────────────────────────────
static void load_theme(StrategyLoadCtx& ctx, const json& s)
{
    int qty = s.value("quantity", 1);
    std::vector<std::string> sector_codes;
    if (s.contains("sector_codes") && s["sector_codes"].is_array())
        sector_codes = s["sector_codes"].get<std::vector<std::string>>();
    int top_n            = s.value("top_n_sectors", 2);
    double vol_surge     = s.value("volume_surge_mult", 2.0);
    bool inst_filter     = s.value("inst_filter", true);
    int eod_hhmm         = s.value("eod_exit_hhmm", 1520);
    ctx.engine.add_strategy(std::make_unique<ThemeStrategy>(
        sector_codes, top_n, vol_surge, inst_filter, qty, eod_hhmm));
}

// ─── 전략-국면 매핑 공통 후처리 ─────────────────────────────────────────────
//  config "active_regimes"(미지정 시 전 국면). 방금 추가된 전략에만 적용.
static void apply_active_regimes(Engine& engine, const json& s, const std::string& type, size_t n_before)
{
    if (engine.strategy_count() > n_before && s.contains("active_regimes") && s["active_regimes"].is_array())
    {
        std::vector<Regime> ar;
        for (const auto& r : s["active_regimes"])
        {
            std::string rs = r.get<std::string>();
            if (rs == "BULL")         ar.push_back(Regime::BULL);
            else if (rs == "NEUTRAL") ar.push_back(Regime::NEUTRAL);
            else if (rs == "BEAR")    ar.push_back(Regime::BEAR);
            else LOG_WARN("[Main] " + type + " 알 수 없는 active_regimes 값: '" + rs +
                          "' (BULL/NEUTRAL/BEAR만 유효) — 무시됨");   // G-2
        }
        if (!ar.empty())
        {
            engine.set_last_active_regimes(ar);
            LOG_INFO("[Main] " + type + " 활성국면: " + std::to_string(ar.size()) + "개");
        }
        else   // G-2: 키는 있는데 파싱 결과가 비면 게이트가 조용히 무력화됨 → 경고
            LOG_WARN("[Main] " + type + " active_regimes 파싱 결과 비어있음 — 전 국면 통과로 동작(게이트 무효)");
    }
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
        std::string type = s["type"];
        size_t n_before = ctx.engine.strategy_count();

        auto it = LOADERS.find(type);
        if (it == LOADERS.end())
        {
            LOG_WARN("[Main] 알 수 없는 전략: " + type);
            continue;
        }
        it->second(ctx, s);
        apply_active_regimes(ctx.engine, s, type, n_before);
    }
}
