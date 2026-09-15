#!/usr/bin/env python3
"""검증 1 — TRENDX 손절을 고정 −2.5%에서 일봉 ATR14 배수로 바꾸면 달라지나(일봉 근사).

선정 층은 13_trendx_gate와 같은 셀(정배열 ∧ 이격 5~35 ∧ PIT 풀, 저항 off, 익절 touch)로 고정하고
**청산 규칙만** 바꾼다. 같은 신호 집합에 손절 규칙 6가지를 태워 짝지은 차이를 본다.

  손절 규칙: none · 2.5%(현행 라이브) · 6% · ATR14×1.5 · ATR14×2.0 · ATR14×2.5
  ATR14   = 참범위 max(H−L, |H−전일C|, |L−전일C|)의 14일 단순평균. **신호일 종가까지**로만 만든다.
            손절가 = 진입가(익일 시가) − 배수 × ATR14[신호일]. 진입 뒤에 정해지는 값이라 look-ahead 아님.
  판정    = 짝지은 차이(같은 (종목,신호일)에서 규칙별 r 차이) → 날짜 평균 → 월 평균 → 1표본 t.
            기저는 stop=2.5(라이브 현행). 참고로 풀 대비 초과R(13번 정본 방식)도 같이 낸다.

시뮬레이터는 13_trendx_gate/run_trendx_gate.py를 그대로 불러 쓴다(규칙을 다시 구현하지 않는다).

    py research/studies/16_trendx_execution/run_atr_stop.py

입력  PYQuant/data/bars_all_pit.parquet (2019-01-02~2026-09-04, 상폐 포함)
출력  research/studies/16_trendx_execution/atr_stop_results.tsv · atr_stop_monthly.csv
seed  없음(난수 미사용). 결정론은 입력 parquet 스냅샷과 파라미터만으로 결정된다.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parents[2]
_GATE = _ROOT / "research" / "studies" / "13_trendx_gate"
sys.path.insert(0, str(_GATE))

import run_trendx_gate as G  # noqa: E402
from stats_util import one_sample_t  # noqa: E402

ATR_PERIOD = 14
STOP_RULES = {                       # 이름 → (고정 %, ATR 배수)
    "none":     (0.0, None),
    "pct2.5":   (2.5, None),         # 현행 라이브 TRENDX stop_loss_pct
    "pct6":     (6.0, None),
    "atr1.5":   (0.0, 1.5),
    "atr2.0":   (0.0, 2.0),
    "atr2.5":   (0.0, 2.5),
}
BASELINE = "pct2.5"
BAND = "5~35"


def add_atr(d: pd.DataFrame) -> pd.DataFrame:
    """ATR14와 익일 시가. 둘 다 종목별로 계산한다(그룹 경계 넘김 없음)."""
    g = d.groupby("code", sort=False)
    prev_close = g["Close"].shift(1)
    tr = pd.concat([(d["High"] - d["Low"]).abs(),
                    (d["High"] - prev_close).abs(),
                    (d["Low"] - prev_close).abs()], axis=1).max(axis=1)
    d["atr14"] = tr.groupby(d["code"], sort=False).transform(
        lambda s: s.rolling(ATR_PERIOD, min_periods=ATR_PERIOD).mean())
    d["next_open"] = g["Open"].shift(-1)
    return d


def monthly_paired(diff: pd.Series, dates: pd.Series) -> pd.Series:
    """짝지은 차이의 월 시계열. 날짜 평균 → 월 평균(13번 excess_r와 같은 일 정합 집계)."""
    dd = pd.DataFrame({"Date": dates, "d": diff}).dropna()
    if dd.empty:
        return pd.Series(dtype=float)
    daily = dd.groupby("Date")["d"].mean()
    return daily.groupby(daily.index.to_period("M")).mean()


def main() -> int:
    t0 = time.time()
    d = G.load_bars()
    d = G.add_features(d, sma_prev=False)
    d = add_atr(d)
    print(f"[load] {len(d):,} rows · {d['code'].nunique()} codes · {time.time()-t0:.0f}s", flush=True)

    lo_b, hi_b = G.BANDS[BAND]
    in_pool = d["in_pool"].to_numpy()
    gate_sel = in_pool & d["aligned"].to_numpy() & (d["pull_pct"].to_numpy() >= lo_b) & (d["pull_pct"].to_numpy() <= hi_b)

    keep_cols = ["Date"]
    gate_r: dict[str, pd.DataFrame] = {}
    pool_r: dict[str, pd.DataFrame] = {}
    eff_stop: dict[str, float] = {}
    for name, (pct, mult) in STOP_RULES.items():
        ts = time.time()
        stop_col = None
        if mult is not None:
            d["_stop"] = d["next_open"] - mult * d["atr14"]
            stop_col = "_stop"
            eff_stop[name] = float(((mult * d.loc[gate_sel, "atr14"]) / d.loc[gate_sel, "next_open"] * 100.0).median())
        else:
            eff_stop[name] = pct
        s = G.simulate_all(d, pct, tp_close=False, stop_col=stop_col)
        ok = s["valid"].to_numpy() & in_pool
        cols = ["r", "net", "gross", "mae", "hold", "why"]
        gate_r[name] = pd.concat([d.loc[gate_sel & ok, keep_cols], s.loc[gate_sel & ok, cols]], axis=1)
        pool_r[name] = pd.concat([d.loc[ok, keep_cols], s.loc[ok, cols]], axis=1)
        print(f"[sim] stop={name} 손절가중앙 {eff_stop[name]:.2f}% · gate {len(gate_r[name]):,}행 "
              f"· {time.time()-ts:.0f}s", flush=True)

    periods = dict(G.PERIODS)
    rows, mrows = [], []
    base = gate_r[BASELINE]
    for name in STOP_RULES:
        t_all = gate_r[name]
        diff_all = t_all["r"] - base["r"].reindex(t_all.index)
        for per, (lo, hi) in list(periods.items()) + [("all4", (None, None))]:
            if per == "all4":
                mask = pd.Series(False, index=t_all.index)
                for plo, phi in periods.values():
                    mask |= (t_all["Date"] >= plo) & (t_all["Date"] <= phi)
                bmask = pd.Series(False, index=pool_r[name].index)
                for plo, phi in periods.values():
                    bmask |= (pool_r[name]["Date"] >= plo) & (pool_r[name]["Date"] <= phi)
            else:
                mask = (t_all["Date"] >= lo) & (t_all["Date"] <= hi)
                bmask = (pool_r[name]["Date"] >= lo) & (pool_r[name]["Date"] <= hi)
            t = t_all[mask]
            b = pool_r[name][bmask]
            if t.empty:
                continue
            ex = G.daily_aligned_excess(t, b)
            ex_mean, ex_t, ex_p, ex_n = one_sample_t(ex.to_numpy())
            dm = monthly_paired(diff_all[mask], t["Date"])
            dmean, dt, dp, dn = one_sample_t(dm.to_numpy())
            rows.append({
                "stop": name, "stop_pct_med": eff_stop[name], "period": per,
                "holdout": per == G.HOLDOUT, "n": int(len(t)), "n_days": int(t["Date"].nunique()),
                "n_months": dn, "gate_r": float(t["r"].mean()), "pool_r": float(b["r"].mean()),
                "excess_r": ex_mean, "excess_t": ex_t, "excess_p": ex_p, "excess_n_months": ex_n,
                "paired_dr": dmean, "paired_t": dt, "paired_p": dp,
                "paired_beat": float((dm > 0).mean()) if dn else float("nan"),
                "win_rate": float((t["net"] > 0).mean()),
                "mae_med": float(t["mae"].median()), "hold_mean": float(t["hold"].mean()),
                "tp_rate": float((t["why"] == "tp").mean()),
                "stop_rate": float(t["why"].isin(["stop", "gap"]).mean()),
                "trunc_rate": float((t["why"] == "trunc").mean()),
            })
            if per == "all4":
                for m, v in dm.items():
                    mrows.append({"stop": name, "month": str(m), "paired_dr": v})

    res = pd.DataFrame(rows)
    res.to_csv(_HERE / "atr_stop_results.tsv", sep="\t", index=False, float_format="%.4f")
    pd.DataFrame(mrows).to_csv(_HERE / "atr_stop_monthly.csv", index=False, float_format="%.5f")
    pd.set_option("display.width", 260)
    pd.set_option("display.max_rows", 200)
    view = res[["stop", "stop_pct_med", "period", "n", "n_months", "gate_r", "pool_r", "excess_r",
                "paired_dr", "paired_t", "paired_p", "paired_beat", "win_rate", "mae_med",
                "hold_mean", "tp_rate", "stop_rate"]]
    print(view.to_string(index=False, float_format=lambda x: f"{x:.4f}"))
    print(f"[done] {_HERE / 'atr_stop_results.tsv'} {time.time()-t0:.0f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
