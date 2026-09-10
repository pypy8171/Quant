# -*- coding: utf-8 -*-
"""T1 신호 안에서 종목선정 조합축 검정 + 손익 분포 구조 확인.

5분위 표에서 평균은 양수인데 중앙값이 음수인 축이 많았다. 소수 대박이 평균을 만드는
구조인지 확인해야 운용 방식(분산·손절·승자유지)이 정해진다.
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"
r = pd.read_csv(RAW / "signals_excess.csv", dtype={"code": str})
d = r[r["trig"] == "T1_ma20_reclaim"].dropna(subset=["x_asof"]).copy()
rng = np.random.default_rng(11)


def ci(x):
    bs = rng.choice(x, size=(4000, len(x)), replace=True).mean(axis=1)
    return np.percentile(bs, [2.5, 97.5])


def show(label, mask):
    s = d[mask]
    if len(s) < 8:
        print(f"{label:34s} n={len(s):3d} (표본부족)")
        return
    x = s["x_asof"].values
    lo, hi = ci(x)
    print(f"{label:34s} n={len(s):4d} 초과평균 {x.mean():+.1%} 중앙 {np.median(x):+.1%} "
          f"승률 {(x>0).mean():.0%} 95%CI[{lo:+.1%},{hi:+.1%}] 18종 {int(s['is18'].sum())}")


q = lambda c, p: d[c].quantile(p)
d["rs_top"] = d["rs_rank_raw"] >= q("rs_rank_raw", 0.8)
d["lead_top"] = d["h1_run"] >= q("h1_run", 0.8)
d["dd_mid"] = (d["dd_at_sig"] <= -0.43) & (d["dd_at_sig"] >= -0.60)   # 5분위 Q2~Q3 구간
d["liq"] = d["tv20"] >= 5e9

print("=== T1 안에서의 종목선정 조합 (초과수익 기준) ===")
show("전체 T1", d["x_asof"].notna())
show("상대강도 상위20%", d["rs_top"])
show("상반기주도 상위20%", d["lead_top"])
show("낙폭 -43~-60% 구간", d["dd_mid"])
show("상대강도상위 + 낙폭중간", d["rs_top"] & d["dd_mid"])
show("상대강도상위 + 상반기주도", d["rs_top"] & d["lead_top"])
show("상대강도상위 + 낙폭중간 + 거래대금50억", d["rs_top"] & d["dd_mid"] & d["liq"])

print("\n=== 손익 분포 구조 (전체 T1, 초과수익) ===")
x = d["x_asof"].values
print(f"승 {(x>0).sum()}건 평균 {x[x>0].mean():+.1%} / 패 {(x<=0).sum()}건 평균 {x[x<=0].mean():+.1%}")
print(f"손익비 {abs(x[x>0].mean()/x[x<=0].mean()):.2f}  기대값 {x.mean():+.1%}")
for p in [10, 25, 50, 75, 90, 95, 99]:
    print(f"  {p:2d}분위 {np.percentile(x, p):+.1%}", end="")
print()
top5 = np.sort(x)[-int(len(x)*0.05):]
print(f"상위 5%({len(top5)}건) 제거 시 평균 {x[x < top5.min()].mean():+.1%} "
      f"→ 평균의 {1 - x[x < top5.min()].mean()/x.mean():.0%}가 상위 5%에서 나온다")

print("\n=== 20일 보유 시 손절 효과 (초과수익 x20 기준) ===")
d2 = d.dropna(subset=["x20", "mae20"])
for stop in [-0.05, -0.08, -0.10, -0.15, None]:
    if stop is None:
        v = d2["x20"].values
        lab = "손절 없음"
    else:
        v = np.where(d2["mae20"] <= stop, stop, d2["x20"].values)
        lab = f"MAE {stop:.0%} 손절"
    print(f"{lab:14s} 평균 {v.mean():+.2%} 중앙 {np.median(v):+.2%} 승률 {(v>0).mean():.0%}")
