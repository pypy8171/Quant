# -*- coding: utf-8 -*-
"""타이밍(트리거)이 같을 때, 무엇이 승자와 패자를 갈랐나 — 종목선정 축 검정.

트리거는 언제 살지만 정한다. 어느 종목을 살지는 별개 축이고, 이 스크립트가 그 축을 잰다.
각 필터는 신호 시점까지의 정보만 쓴다(dd_at_sig·h1_run·tv20·mktcap).
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"
r = pd.read_csv(RAW / "signals_universe.csv", dtype={"code": str})

TRIG = "T1_ma20_reclaim"
d = r[r["trig"] == TRIG].copy()
print(f"[{TRIG}] 표본 {len(d):,}건, 전체 평균 {d['r_asof'].mean():+.1%} "
      f"중앙 {d['r_asof'].median():+.1%} 승률 {(d['r_asof']>0).mean():.0%}\n")


def buckets(col, q=5, label=None):
    x = d.dropna(subset=[col])
    try:
        x = x.assign(b=pd.qcut(x[col], q, labels=[f"Q{i+1}" for i in range(q)], duplicates="drop"))
    except ValueError:
        return
    g = x.groupby("b", observed=True).agg(
        n=("code", "size"), 평균=("r_asof", "mean"), 중앙=("r_asof", "median"),
        승률=("r_asof", lambda s: (s > 0).mean()), MAE=("mae20", "mean"),
        구간하한=(col, "min"), 구간상한=(col, "max"))
    print(f"--- {label or col} 5분위 ---")
    print(g.round(3).to_string())
    n18 = x.groupby("b", observed=True)["is18"].sum()
    print(f"    18종 분포: {dict(n18)}\n")


buckets("dd_at_sig", label="신호시점 250일고점대비 낙폭(깊을수록 Q1)")
buckets("h1_run", label="직전 150일 상승률(상반기 주도주 여부)")
buckets("rs_rank_raw", label="120일 상대강도 변화")
buckets("tv20", label="20일 평균 거래대금")
buckets("mktcap", label="시가총액")

print("=== 조합 필터 후보 ===")
d["deep"] = d["dd_at_sig"] < d["dd_at_sig"].quantile(0.4)          # 깊게 빠진 쪽
d["leader"] = d["h1_run"] > d["h1_run"].quantile(0.6)              # 상반기 강했던 쪽
d["liquid"] = d["tv20"] > 5e9
for name, mask in [
    ("깊은낙폭", d["deep"]),
    ("상반기주도", d["leader"]),
    ("깊은낙폭+상반기주도", d["deep"] & d["leader"]),
    ("깊은낙폭+상반기주도+거래대금50억", d["deep"] & d["leader"] & d["liquid"]),
]:
    s = d[mask]
    if len(s) == 0:
        continue
    print(f"{name:32s} n={len(s):4d} 평균 {s['r_asof'].mean():+.1%} "
          f"중앙 {s['r_asof'].median():+.1%} 승률 {(s['r_asof']>0).mean():.0%} "
          f"MAE20 {s['mae20'].mean():+.1%} 18종포함 {int(s['is18'].sum())}/18")
