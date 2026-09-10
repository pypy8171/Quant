# -*- coding: utf-8 -*-
"""초과수익(소속 지수 차감) 기준 재측정.

진입일이 8월 초에 몰려 있어 절대수익은 시장수익이 지배한다. 같은 보유구간의
소속 지수(KOSPI/KOSDAQ) 수익을 빼야 트리거 자체의 기여가 보인다.
벤치마크도 종목과 동일하게 신호 익일 시가 진입으로 맞춘다.
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"

r = pd.read_csv(RAW / "signals_universe.csv", dtype={"code": str}, parse_dates=["sig_date"])
meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
r = r.merge(meta[["code", "market"]], on="code", how="left")

idx = {}
for f, m in [("KS11", "KOSPI"), ("KQ11", "KOSDAQ")]:
    d = pd.read_csv(RAW / f"{f}.csv", parse_dates=["Date"]).rename(columns={"Date": "date"})
    d = d.sort_values("date").reset_index(drop=True)
    idx[m] = d

def bench(row, hz):
    """지수도 신호 익일 시가 진입. hz=None이면 마지막 종가까지."""
    d = idx.get(row["market"])
    if d is None:
        return np.nan
    pos = d.index[d["date"] == row["sig_date"]]
    if len(pos) == 0 or pos[0] + 1 >= len(d):
        return np.nan
    i = pos[0]
    e = d["Open"].iloc[i + 1]
    j = len(d) - 1 if hz is None else min(i + hz, len(d) - 1)
    return d["Close"].iloc[j] / e - 1

for col, hz in [("b_asof", None), ("b10", 10), ("b20", 20)]:
    r[col] = r.apply(lambda x: bench(x, hz), axis=1)
r["x_asof"] = r["r_asof"] - r["b_asof"]
r["x10"] = r["r10"] - r["b10"]
r["x20"] = r["r20"] - r["b20"]
r.to_csv(RAW / "signals_excess.csv", index=False, encoding="utf-8-sig")

print("=== 트리거별 초과수익 (소속 지수 차감, 신호 익일 시가 진입) ===")
g = r.groupby("trig").agg(
    n=("code", "size"),
    절대평균=("r_asof", "mean"), 지수평균=("b_asof", "mean"),
    초과평균=("x_asof", "mean"), 초과중앙=("x_asof", "median"),
    초과승률=("x_asof", lambda s: (s > 0).mean()),
    초과20d=("x20", "mean"), 초과10d=("x10", "mean"))
print(g.round(3).to_string())

print("\n--- t검정(초과수익 평균 = 0) ---")
rng = np.random.default_rng(7)
for t, s in r.groupby("trig"):
    x = s["x_asof"].dropna().values
    tt = x.mean() / (x.std(ddof=1) / np.sqrt(len(x)))
    bs = rng.choice(x, size=(4000, len(x)), replace=True).mean(axis=1)  # 부트스트랩 95% 구간
    lo, hi = np.percentile(bs, [2.5, 97.5])
    print(f"{t:18s} n={len(x):4d} 평균 {x.mean():+.3f} t={tt:+.2f} 95%CI[{lo:+.3f},{hi:+.3f}]")

print("\n=== 18종 vs 나머지 (초과수익) ===")
print(r.groupby(["trig", "is18"])["x_asof"].agg(["size", "mean", "median"]).round(3).to_string())

print("\n=== T1 종목선정 축 5분위 (초과수익) ===")
d = r[r["trig"] == "T1_ma20_reclaim"].copy()
for col, label in [("dd_at_sig", "낙폭(깊을수록 Q1)"), ("h1_run", "직전150일 상승률"),
                   ("rs_rank_raw", "120일 상대강도"), ("tv20", "20일 거래대금"),
                   ("mktcap", "시가총액")]:
    x = d.dropna(subset=[col, "x_asof"])
    x = x.assign(b=pd.qcut(x[col], 5, labels=[f"Q{i+1}" for i in range(5)], duplicates="drop"))
    gg = x.groupby("b", observed=True).agg(
        n=("code", "size"), 초과평균=("x_asof", "mean"), 초과중앙=("x_asof", "median"),
        초과승률=("x_asof", lambda s: (s > 0).mean()), n18=("is18", "sum"))
    print(f"\n--- {label} ---")
    print(gg.round(3).to_string())
