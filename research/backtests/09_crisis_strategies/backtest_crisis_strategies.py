#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""위기 대응·수익추구 전략 10종 인과 백테스트 (BT-09) — 엔트리·오케스트레이션.

목적: BT-08의 5개 방어 대응법(M1~M5)을 넘어, 시장 도메인 지식(지표·이벤트·크로스에셋 연관)에서
      우리 에이전트들이 논의해 도출한 **손실축소(방어 5) + 수익추구(공세 5) 전략 10종**을
      동일한 인과 규율로 검증한다.

모듈 구성(가독성·기능분리):
 - bt09_signals.py     : 정렬(align_ff)·NaN내성 지표·신호 컨텍스트(build_ctx) + 신호 상수
 - bt09_strategies.py  : 방어 5(C1~C5)+공세 5(O1~O5) 익스포저 함수 + STRATS 레지스트리
 - bt09_report.py      : 평가 결과 → README.md 직렬화(write_readme)
 - 본 파일             : 벤치마크 구성·평가·스윕 오케스트레이션 + main.
   (하위호환: gen_trade_journal.py 가 쓰는 build_ctx/STRATS/build_benchmark 등을 재-export)

절대 경계(BT-07/08 계승):
 1. 엔진(PYQuant/backtest/engine.py)·yfinance_source.py 절대 미수정.
    데이터는 IndexSource().get_historical_ohlcv 만. BT-08 엔진 헬퍼는 import 재사용(단일 소스).
 2. 지수·매크로 시계열만(종목레벨 breadth/dispersion 금지 — 생존편향).
 3. 룩어헤드 차단:
    - 익스포저 e[t]는 오직 close t 까지의 정보로 산출, 수익 = e[t-1]×지수수익(run_curve).
    - **US→KR 시차**: KR(^KS11) 벤치마크에선 US/글로벌 신호(^GSPC·^IXIC·^VIX·^TNX·^SOX·CL=F·DXY)를
      strict=True(직전 세션값)로 정렬한다. 미국장은 한국장 이후 마감 → 같은 날짜 US종가를 KR신호로
      쓰면 1일 룩어헤드. strict=True + run_curve e[t-1] 로 이중 차단.
    - 매크로(TNX/oil/DXY/KRW)는 **신호전용** — 수익곡선 ret[t]는 오직 거래대상 지수. (금리·유가로
      '수익곡선'을 만드는 범주오류 금지.)
 4. 정직성: 1차 지표 = 단일 연결곡선 Calmar 1개(검정 1회). 이벤트별은 진단·중앙값만.
    사전등록 임계값 + 전체 스윕 공개(best 셀 보고 금지). 비용 0.21/0.5/1.0% 감도.
    홀드아웃 2022 잠금(train=2022제외, holdout=2022). 결측신호 → 중립(e=1) + 커버리지 명시.

단독 실행:
    python research/backtests/09_crisis_strategies/backtest_crisis_strategies.py
콘솔 요약 + README.md 생성.
"""
import sys
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]                        # .../Quant
_PYQ = _REPO / "PYQuant"
_BT08 = _REPO / "research" / "backtests" / "08_crisis_response"
for _p in (str(_PYQ), str(_BT08)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from data.index_source import IndexSource                    # noqa: E402
import backtest_crisis_response as bt08                       # noqa: E402  (엔진 헬퍼 재사용)

# BT-08에서 그대로 재사용(오케스트레이션이 직접 호출)
load_series   = bt08.load_series
daily_returns = bt08.daily_returns
run_curve     = bt08.run_curve
curve_stats   = bt08.curve_stats
max_drawdown  = bt08.max_drawdown
turnover_toggles = bt08.turnover_toggles
slice_window  = bt08.slice_window
window_maxdd_anchor = bt08.window_maxdd_anchor
median        = bt08.median
fmt           = bt08.fmt
EVENTS        = bt08.EVENTS
BEHAVIOR      = bt08.BEHAVIOR
ANNUAL        = bt08.ANNUAL
COST_BASE     = bt08.COST_BASE
COST_GRID     = bt08.COST_GRID
WARMUP        = bt08.WARMUP

# BT-09 계층 재-export(하위호환: gen_trade_journal.py 는 build_ctx·STRATS 만 사용)
from bt09_signals import (                                    # noqa: E402
    TODAY, STALE_CAP_DAYS, SIGNAL_TICKERS, US_SESSION,
    align_ff, build_ctx,
)
from bt09_strategies import (                                 # noqa: E402
    OFF_CAP, STRATS,
    expo_c1_supplyshock, expo_c2_riskoff_triple, expo_c3_vix_shock,
    expo_c4_rateshock, expo_c5_overnight_gap,
    expo_o1_regime_switch, expo_o2_vol_breakout, expo_o3_sox_lead,
    expo_o4_demandpull, expo_o5_crossasset_rebound,
)
from bt09_report import write_readme                          # noqa: E402

OUT_DIR = _HERE.parent
README_PATH = OUT_DIR / "README.md"
HOLDOUT = ("2022-01-01", "2022-12-31")


# ════════════════════════════════════════════════════════════════════════════
# 평가
# ════════════════════════════════════════════════════════════════════════════
def _mask_by_dates(dates, d0, d1, inside=True):
    m = np.array([(d0 <= d <= d1) for d in dates], dtype=bool)
    return m if inside else ~m


def _stats_on_subset(net, mask):
    sub = net[mask]
    if len(sub) < 20:
        return None
    eq = np.cumprod(1.0 + sub)
    return curve_stats(sub, eq)


def build_benchmark(src, name, ticker, start, is_kr):
    dates, closes = load_series(src, ticker, start, TODAY)
    if len(closes) < WARMUP + 20:
        return None
    ret = daily_returns(closes)
    ctx = build_ctx(src, dates, is_kr)
    years = (int(dates[-1][:4]) - int(dates[0][:4])) + 1
    return dict(name=name, ticker=ticker, dates=dates, closes=closes, ret=ret,
                ctx=ctx, span=f"{dates[0]}~{dates[-1]}", nbars=len(dates),
                bpy=len(dates) / max(years, 1), is_kr=is_kr)


def eval_benchmark(bm):
    closes, ret, ctx, dates = bm["closes"], bm["ret"], bm["ctx"], bm["dates"]
    bh_net, bh_eq = run_curve(closes, ret, np.ones(len(closes)), 0.0)
    bh = curve_stats(bh_net, bh_eq)
    hold_mask = _mask_by_dates(dates, HOLDOUT[0], HOLDOUT[1], inside=True)
    train_mask = ~hold_mask

    rows, per_e = [], {}
    for code, label, fn, side, desc, needs in STRATS:
        e = fn(closes, ret, ctx, {})
        per_e[code] = e
        by_cost = {}
        for c in COST_GRID:
            net, eq = run_curve(closes, ret, e, c)
            by_cost[c] = curve_stats(net, eq)
        base = by_cost[COST_BASE]
        net_b, _ = run_curve(closes, ret, e, COST_BASE)
        tr = _stats_on_subset(net_b, train_mask)
        ho = _stats_on_subset(net_b, hold_mask)
        mdd_red = base["mdd"] - bh["mdd"]                 # +면 덜 빠짐
        cagr_delta = base["cagr"] - bh["cagr"]           # +면 BH초과수익
        # 활성 커버리지(신호 발동 비율): e != 1.0 인 날 비중
        active = float(np.mean(np.abs(e - 1.0) > 1e-9)) * 100.0
        rows.append(dict(code=code, label=label, side=side, desc=desc, needs=needs,
                         by_cost=by_cost, base=base, mdd_red=mdd_red,
                         cagr_delta=cagr_delta, active=active,
                         toggles=turnover_toggles(e),
                         train=tr, hold=ho))

    # 이벤트별 진단(방어=낙폭축소, 공세=초과수익)
    ev_rows = []
    for eid, cause, rep, d0, d1 in EVENTS:
        lo, hi = slice_window(dates, d0, d1)
        if lo is None or hi - lo < 10:
            continue
        wc = closes[lo:hi]
        wr = ret[lo:hi].copy()
        wr[0] = 0.0
        _, _, bh_dd = window_maxdd_anchor(wc)
        bh_wnet = np.zeros(len(wc))
        for t in range(1, len(wc)):
            bh_wnet[t] = wr[t]
        bh_wtot = float(np.prod(1.0 + bh_wnet) - 1.0)
        row = dict(eid=eid, cause=cause, behavior=BEHAVIOR.get(eid, "?"),
                   window=f"{d0[:7]}~{d1[:7]}", bh_dd=bh_dd, bh_tot=bh_wtot * 100.0,
                   methods={})
        for code, label, fn, side, desc, needs in STRATS:
            e = per_e[code][lo:hi]
            net = np.zeros(len(wc))
            prev2 = e[0]
            for t in range(1, len(wc)):
                pos = e[t - 1]
                net[t] = pos * wr[t] - abs(pos - prev2) * (COST_BASE / 2.0)
                prev2 = pos
            eq = np.cumprod(1.0 + net)
            m_dd = max_drawdown(eq)
            m_tot = float(eq[-1] - 1.0) * 100.0
            row["methods"][code] = dict(mdd_red=m_dd - bh_dd, excess=m_tot - bh_wtot * 100.0)
        ev_rows.append(row)

    return dict(bh=bh, rows=rows, ev_rows=ev_rows,
                cov=ctx["_cov"], hold_bh=_stats_on_subset(bh_net, hold_mask),
                train_bh=_stats_on_subset(bh_net, train_mask))


def run_sweep(bm):
    """사전등록 임계값 그리드 전량 → full-curve Calmar. + 절제(ablation) 쌍."""
    closes, ret, ctx = bm["closes"], bm["ret"], bm["ctx"]
    out = []

    def cal(fn, p):
        e = fn(closes, ret, ctx, p)
        net, eq = run_curve(closes, ret, e, COST_BASE)
        st = curve_stats(net, eq)
        return st["calmar"], st["mdd"], st["total"]

    # C3 cool
    for cl in (3, 5, 10):
        c, m, t = cal(expo_c3_vix_shock, {"cool": cl})
        out.append(("C3", f"cool={cl}", c, m, t))
    # C4 thr
    for thr in (0.40, 0.60, 0.80):
        c, m, t = cal(expo_c4_rateshock, {"thr": thr})
        out.append(("C4", f"thr={thr:.2f}%p", c, m, t))
    return out


# ════════════════════════════════════════════════════════════════════════════
# 엔트리
# ════════════════════════════════════════════════════════════════════════════
def main():
    src = IndexSource()
    print("[load] 벤치마크·신호 구축 중...")
    us = build_benchmark(src, "US(^GSPC)", "^GSPC", "1985-01-01", is_kr=False)
    kr = build_benchmark(src, "KR(^KS11)", "^KS11", "1996-01-01", is_kr=True)
    bms = [b for b in (us, kr) if b is not None]
    results, sweeps = {}, {}
    for bm in bms:
        print(f"[eval] {bm['name']} {bm['span']} bars={bm['nbars']}")
        results[bm["name"]] = eval_benchmark(bm)
        sweeps[bm["name"]] = run_sweep(bm)
    write_readme(bms, results, sweeps, README_PATH)
    print(f"[done] README → {README_PATH}")
    # 콘솔 요약
    for bm in bms:
        r = results[bm["name"]]
        print(f"\n=== {bm['name']} (BH Calmar {fmt(r['bh']['calmar'],2)}, "
              f"CAGR {fmt(r['bh']['cagr'],2)}%, MDD {fmt(r['bh']['mdd'])}%) ===")
        for row in r["rows"]:
            b = row["base"]
            print(f"  {row['code']} {row['side']} Calmar {fmt(b['calmar'],2):>6} "
                  f"CAGR {fmt(b['cagr'],2):>6} MDD {fmt(b['mdd']):>6} "
                  f"낙폭축소 {fmt(row['mdd_red']):>6} 초과CAGR {fmt(row['cagr_delta'],2):>6} "
                  f"활성 {fmt(row['active'],0)}% 토글 {row['toggles']}")


if __name__ == "__main__":
    main()
