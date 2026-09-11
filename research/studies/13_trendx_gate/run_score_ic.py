#!/usr/bin/env python3
"""스캐너 점수 랭크 순위상관(IC) — 현행 점수가 5·20일 뒤 수익 순위를 맞히는지.

`Quant/src/universe/UniverseScanner.cpp`의 점수를 일봉으로 재현한다:
  trend=(SMA5−SMA60)/SMA60, pull=(Close−SMA20)/SMA20, vol=ATR14/Close,
  날짜별 횡단면 z(모집단 표준편차, ±2 클립), S = z(trend) − z(pull) − 0.5·z(vol).
라이브 TRENDX는 유동성 항(w_liquidity 0.7)을 더 쓰지만 지시서 정의대로 세 항만 잰다(감도로 한 줄 추가).
슬리브 필터: 정배열 ∧ TRENDX 0.05≤pull≤0.35 / DEVSCALE pull≤0.05. 날짜별 적격 ≥20일 때만 IC를 낸다.
fwd_k = Close[t+k]/Open[t+1] − 1 (k=5·20). 일별 Spearman → 월 평균 → 월 시계열 1표본 t.
판정(사전등록): t>2 AND IC>0 월 ≥60%, Bonferroni α=0.0125(슬리브 2 × 지평 2).

출력 ic_monthly.csv · ic_summary.tsv
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
from run_trendx_gate import PERIODS, add_features, load_bars  # noqa: E402
from stats_util import one_sample_t  # noqa: E402

SLEEVES = {"TRENDX": (0.05, 0.35), "DEVSCALE": (-np.inf, 0.05)}
HORIZONS = (5, 20)
MIN_ELIG = 20
TOP_N = 25
W_VOL = 0.5
W_LIQ = 0.7          # 감도 행에만 쓴다
BONF_ALPHA = 0.0125


def zclip(s: pd.Series) -> pd.Series:
    sd = s.std(ddof=0)
    if not np.isfinite(sd) or sd < 1e-12:
        return pd.Series(0.0, index=s.index)
    return ((s - s.mean()) / sd).clip(-2.0, 2.0)


def add_score_inputs(d: pd.DataFrame) -> pd.DataFrame:
    g = d.groupby("code", sort=False)
    prev_c = g["Close"].shift(1)
    tr = pd.concat([d["High"] - d["Low"], (d["High"] - prev_c).abs(), (d["Low"] - prev_c).abs()], axis=1).max(axis=1)
    d["atr14"] = tr.groupby(d["code"]).transform(lambda s: s.rolling(14).mean())
    d["vol"] = d["atr14"] / d["Close"]
    d["trend"] = (d["sma5"] - d["sma60"]) / d["sma60"]
    d["pull"] = d["pull_pct"] / 100.0
    d["liq"] = np.log(d["tv20"].where(d["tv20"] > 0))
    nxt_open = g["Open"].shift(-1)
    for k in HORIZONS:
        d[f"fwd{k}"] = g["Close"].shift(-k) / nxt_open - 1.0
    return d


def daily_ic(sub: pd.DataFrame, score_col: str, k: int) -> float:
    x = sub[score_col].rank()
    y = sub[f"fwd{k}"].rank()
    if x.std() == 0 or y.std() == 0:
        return np.nan
    return float(np.corrcoef(x, y)[0, 1])


def run_sleeve(d: pd.DataFrame, name: str, lo: float, hi: float, with_liq: bool) -> pd.DataFrame:
    elig = d["in_pool"] & d["aligned"] & (d["pull"] >= lo) & (d["pull"] <= hi) & d["vol"].notna() \
        & d["trend"].notna()
    e = d.loc[elig, ["Date", "code", "trend", "pull", "vol", "liq", "in_pool"] + [f"fwd{k}" for k in HORIZONS]].copy()
    cnt = e.groupby("Date")["code"].transform("size")
    e = e[cnt >= MIN_ELIG]
    gd = e.groupby("Date", sort=True)
    e["zt"] = gd["trend"].transform(zclip)
    e["zp"] = gd["pull"].transform(zclip)
    e["zv"] = gd["vol"].transform(zclip)
    e["S"] = e["zt"] - e["zp"] - W_VOL * e["zv"]
    if with_liq:
        e["zl"] = gd["liq"].transform(lambda s: zclip(s.fillna(s.median())))
        e["S"] = e["S"] + W_LIQ * e["zl"]
    pool = d.loc[d["in_pool"], ["Date"] + [f"fwd{k}" for k in HORIZONS]]
    pool_mean = pool.groupby("Date").mean()
    rows = []
    for day, sub in e.groupby("Date", sort=True):
        rec = {"Date": day, "n_elig": len(sub)}
        top = sub.nlargest(TOP_N, "S")
        for k in HORIZONS:
            s2 = sub[sub[f"fwd{k}"].notna()]
            rec[f"ic{k}"] = daily_ic(s2, "S", k) if len(s2) >= MIN_ELIG else np.nan
            pm = pool_mean.at[day, f"fwd{k}"] if day in pool_mean.index else np.nan
            rec[f"top{k}_ex_pct"] = (top[f"fwd{k}"].mean() - pm) * 100.0
            rec[f"elig{k}_ex_pct"] = (sub[f"fwd{k}"].mean() - pm) * 100.0
        rows.append(rec)
    out = pd.DataFrame(rows)
    out["sleeve"] = name + ("+liq" if with_liq else "")
    return out


def summarize(daily: pd.DataFrame, lo: str | None, hi: str | None, label: str) -> list[dict]:
    d = daily if lo is None else daily[(daily["Date"] >= lo) & (daily["Date"] <= hi)]
    out = []
    for k in HORIZONS:
        m = d.groupby(d["Date"].dt.to_period("M")).agg(ic=(f"ic{k}", "mean"), top_ex=(f"top{k}_ex_pct", "mean"),
                                                        elig_ex=(f"elig{k}_ex_pct", "mean"), n_days=("Date", "size"))
        m = m.dropna(subset=["ic"])
        ic_mean, ic_t, ic_p, nm = one_sample_t(m["ic"].to_numpy())
        ex_mean, ex_t, ex_p, _ = one_sample_t(m["top_ex"].to_numpy())
        el_mean, el_t, _, _ = one_sample_t(m["elig_ex"].to_numpy())
        pos = float((m["ic"] > 0).mean()) if nm else np.nan
        out.append({"sleeve": daily["sleeve"].iloc[0], "period": label, "k": k, "n_days": int(d[f"ic{k}"].notna().sum()),
                    "n_months": nm, "ic_mean": ic_mean, "ic_t": ic_t, "ic_p": ic_p, "ic_pos_share": pos,
                    "pass": bool(nm and ic_t > 2 and pos >= 0.6 and ic_p < BONF_ALPHA),
                    "top25_ex_pct": ex_mean, "top25_ex_r": ex_mean / 6.0, "top25_t": ex_t,
                    "elig_ex_pct": el_mean, "elig_t": el_t})
    return out


def main() -> int:
    t0 = time.time()
    d = add_features(load_bars(), sma_prev=False)
    d = add_score_inputs(d)
    print(f"[load] {time.time()-t0:.0f}s", flush=True)
    dailies, summ = [], []
    for name, (lo, hi) in SLEEVES.items():
        for with_liq in (False, True):
            ts = time.time()
            daily = run_sleeve(d, name, lo, hi, with_liq)
            dailies.append(daily)
            summ += summarize(daily, None, None, "full_2019-2026")
            for per, (plo, phi) in PERIODS.items():
                summ += summarize(daily, plo, phi, per)
            print(f"[ic] {daily['sleeve'].iloc[0]} {len(daily)}일 {time.time()-ts:.0f}s", flush=True)
    allday = pd.concat(dailies)
    allday["month"] = allday["Date"].dt.to_period("M").astype(str)
    monthly = allday.groupby(["sleeve", "month"]).agg(
        n_days=("Date", "size"), n_elig_mean=("n_elig", "mean"),
        ic5=("ic5", "mean"), ic20=("ic20", "mean"),
        top5_ex_pct=("top5_ex_pct", "mean"), top20_ex_pct=("top20_ex_pct", "mean")).reset_index()
    monthly.to_csv(_HERE / "ic_monthly.csv", index=False, float_format="%.5f")
    s = pd.DataFrame(summ)
    s.to_csv(_HERE / "ic_summary.tsv", sep="\t", index=False, float_format="%.4f")
    pd.set_option("display.width", 220)
    pd.set_option("display.max_rows", 200)
    print(s.to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    print(f"[done] {time.time()-t0:.0f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
