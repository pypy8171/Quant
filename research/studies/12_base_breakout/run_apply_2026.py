# -*- coding: utf-8 -*-
"""과거 10개 사건에서 정한 부호를 2026-08 사건에 그대로 적용한다.

2026 사건은 전방 60거래일이 아직 없어 IC 학습에 들어가지 못했다. 부호는 전부
2018~2024에서 나왔으므로 여기 적용은 표본 밖이다.
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from signals import add_features, add_market, t1_ma20_reclaim
from run_crosssec import feats_at, spearman

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"

A = pd.read_csv(RAW / "crosssec.csv", dtype={"code": str})
FE = [c for c in A.columns if c not in ("code", "event", "market", "x", "bench")]


def sp(a, b):
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 30:
        return np.nan
    ra = pd.Series(a[m]).rank().values
    rb = pd.Series(b[m]).rank().values
    if np.std(ra) == 0 or np.std(rb) == 0:
        return np.nan
    return float(np.corrcoef(ra, rb)[0, 1])


sign = {}
for f in FE:
    v = np.array([sp(g[f].values.astype(float), g["x"].values.astype(float))
                  for _, g in A.groupby("event")])
    v = v[np.isfinite(v)]
    sign[f] = float(np.sign(v.mean())) if len(v) >= 8 else 0.0
print("과거 10건에서 나온 부호(+면 높을수록 유리):")
print("  " + ", ".join(f"{f}{'+' if s > 0 else '−' if s < 0 else '0'}" for f, s in sign.items()))

EV = pd.Timestamp("2026-08-03")           # 지수 20MA 회복일
END = pd.Timestamp("2026-09-09")
big = pd.read_parquet(RAW / "universe.parquet")
big["date"] = pd.to_datetime(big["date"])
meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
mcap = dict(zip(meta["code"], meta["mktcap"].replace(0, np.nan)))
name = dict(zip(meta["code"], meta["name"]))
mk = dict(zip(meta["code"], meta["market"]))
idx = {m: pd.read_csv(RAW / f"{s}.csv", parse_dates=["Date"]).rename(columns={"Date": "date"})
       for m, s in (("KOSPI", "KS11"), ("KOSDAQ", "KQ11"))}

rows = []
for code, d in big.groupby("code", sort=False):
    market = mk.get(code)
    if market not in idx or len(d) < 300:
        continue
    ix = idx[market].rename(columns={"Close": "mclose"})[["date", "mclose"]]
    d = add_market(add_features(d.sort_values("date")), ix).reset_index(drop=True)
    w = (d["date"] >= EV) & (d["date"] <= EV + pd.Timedelta(days=22))
    if not w.any() or d.loc[w, "tv20"].median() < 5e8 or d.loc[w, "close"].median() < 1000:
        continue
    s = t1_ma20_reclaim(d).fillna(False) & w
    pos = np.flatnonzero(s.values)
    if len(pos) == 0 or pos[0] + 1 >= len(d):
        continue
    i = pos[0]
    e = d["open"].iloc[i + 1]
    if not np.isfinite(e) or e <= 0:
        continue
    r = feats_at(d, i)
    r.update(code=code, name=name.get(code, code), market=market,
             sig=d["date"].iloc[i].date(), ret=d["close"].iloc[-1] / e - 1)
    rows.append(r)

T = pd.DataFrame(rows)
T["시총로그"] = np.log(T["code"].map(mcap).fillna(T["code"].map(mcap).median()))
T["mktcap"] = T["code"].map(mcap)
sc = np.zeros(len(T))
for f in FE:
    sc += sign[f] * T[f].astype(float).rank(pct=True).fillna(0.5).values
T["score"] = sc / sum(1 for f in FE if sign[f] != 0)

for m in ("KOSPI", "KOSDAQ"):
    ix = idx[m]
    a = ix.loc[ix["date"] == EV, "Open"].iloc[0]
    b = ix.loc[ix["date"] <= END, "Close"].iloc[-1]
    T.loc[T["market"] == m, "bench"] = b / a - 1
T["x"] = T["ret"] - T["bench"]
T = T.sort_values("score", ascending=False).reset_index(drop=True)
T.to_csv(RAW / "apply_2026.csv", index=False, encoding="utf-8-sig")

n = len(T)
print(f"\n2026-08 사건: 신호 {n}종, 보유 {T['sig'].min()} 익일시가 → 09-09 (약 26거래일)")
for lab, s in (("상위20%", T.head(n // 5)), ("중간", T.iloc[n // 5:-(n // 5)]), ("하위20%", T.tail(n // 5))):
    print(f"  {lab:<7s} n={len(s):4d}  절대 {s['ret'].mean():+6.1%}  초과 {s['x'].mean():+6.1%}  "
          f"중앙초과 {s['x'].median():+6.1%}  +50%이상 {(s['ret'] > 0.5).mean():4.0%}")
print(f"  {'전체':<7s} n={n:4d}  절대 {T['ret'].mean():+6.1%}  초과 {T['x'].mean():+6.1%}")

print("\n=== 점수 상위 25종 (2026-08-03 시점 정보만으로 뽑힘) ===")
for i, r in enumerate(T.head(25).itertuples(), 1):
    print(f"{i:2d}. {r.name[:12]:<13s} {r.code} {r.market:<6s} 점수 {r.score:.3f}  "
          f"실현 {r.ret:+7.1%}  초과 {r.x:+7.1%}")
U18 = ["001440", "336260", "382900", "032820", "000500", "082740", "403870", "000990", "095610",
       "108490", "034020", "052690", "006360", "047040", "000720", "000660", "009150", "001820"]
T["rank"] = range(1, n + 1)
s = T[T["code"].isin(U18)]
print(f"\n지목 18종 중 {len(s)}종이 신호 발생. 점수 순위 중앙 {s['rank'].median():.0f}/{n} "
      f"(상위 {s['rank'].median() / n:.0%}), 실현 초과 중앙 {s['x'].median():+.1%}")
