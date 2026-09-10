# -*- coding: utf-8 -*-
"""지수 타이밍 축의 과거 재현성.

2026년 사건 1건으로는 결론이 안 난다. 저점일이 전 종목 공통이었으므로 '언제 사느냐'는
지수 국면 문제다. 지수만 쓰면 2000년부터 표본을 늘릴 수 있다.
규칙은 사후정보를 쓰지 않는다 — 낙폭과 이평 회복은 당일까지의 정보로 계산된다.
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd
import FinanceDataReader as fdr

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"

for sym in ["KS11", "KQ11"]:
    f = RAW / f"{sym}_long.csv"
    if not f.exists():
        fdr.DataReader(sym, "1999-01-01", "2026-09-09").to_csv(f, encoding="utf-8")

rng = np.random.default_rng(3)
HZ = (20, 60, 120)


def analyze(sym, label, dd_thr):
    d = pd.read_csv(RAW / f"{sym}_long.csv", parse_dates=["Date"]).rename(columns={"Date": "date"})
    d = d.sort_values("date").reset_index(drop=True)
    c = d["Close"]
    d["ma20"] = c.rolling(20).mean()
    d["hi250"] = c.rolling(250, min_periods=120).max()
    d["dd"] = c / d["hi250"] - 1
    for h in HZ:
        d[f"f{h}"] = c.shift(-h) / d["Open"].shift(-1) - 1     # 익일 시가 진입

    reclaim = (c > d["ma20"]) & (c.shift(1) <= d["ma20"].shift(1))
    deep = d["dd"] <= dd_thr
    sig = reclaim & deep
    # 같은 국면에서 반복 발생하는 신호는 30거래일 쿨다운으로 1건만 센다
    keep, last = [], -999
    for i in np.flatnonzero(sig.values):
        if i - last >= 30:
            keep.append(i); last = i
    ev = d.iloc[keep]

    print(f"\n=== {label} | 250일고점대비 {dd_thr:.0%} 이하에서 20MA 회복 ===")
    print(f"사건 {len(ev)}건 ({d['date'].iloc[0].date()}~{d['date'].iloc[-1].date()})")
    print(f"발생일: {', '.join(str(x.date()) for x in ev['date'])}")
    for h in HZ:
        x = ev[f"f{h}"].dropna().values
        base = d[f"f{h}"].dropna().values                      # 무조건 진입 기저율
        if len(x) < 3:
            continue
        bs = rng.choice(x, size=(4000, len(x)), replace=True).mean(axis=1)
        lo, hi = np.percentile(bs, [2.5, 97.5])
        print(f"  +{h:3d}일  평균 {x.mean():+.1%} 중앙 {np.median(x):+.1%} 승률 {(x>0).mean():.0%} "
              f"95%CI[{lo:+.1%},{hi:+.1%}] | 무조건진입 평균 {base.mean():+.1%} 승률 {(base>0).mean():.0%}")
    return ev


for sym, label in [("KS11", "KOSPI"), ("KQ11", "KOSDAQ")]:
    for thr in (-0.15, -0.25, -0.35):
        analyze(sym, label, thr)
