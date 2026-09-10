# -*- coding: utf-8 -*-
"""2026년에서 나온 축이 과거 폭락에서도 재현되는지 검정.

2026년 1건으로 고른 축(상대강도 상위·낙폭 -43~-60%)은 그 데이터에서 고른 것이라
같은 데이터로는 검증이 안 된다. 과거 폭락 사건이 홀드아웃 역할을 한다.

사건 정의는 기계적이다: 지수가 250일 고점 대비 -25% 이하인 상태에서 20MA를 회복한 날.
2026-08의 사건과 정확히 같은 규칙이므로 사후 선택이 들어가지 않는다.
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from signals import add_features, add_market, t1_ma20_reclaim

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"
rng = np.random.default_rng(23)
HZ = 60                      # 보유 60거래일
WINDOW = 15                  # 사건일로부터 15거래일 안의 최초 신호만


def index_events(sym, thr):
    d = pd.read_csv(RAW / f"{sym}_long.csv", parse_dates=["Date"]).rename(columns={"Date": "date"})
    d = d.sort_values("date").reset_index(drop=True)
    c = d["Close"]
    ma20 = c.rolling(20).mean()
    dd = c / c.rolling(250, min_periods=120).max() - 1
    sig = (c > ma20) & (c.shift(1) <= ma20.shift(1)) & (dd <= thr)
    keep, last = [], -999
    for i in np.flatnonzero(sig.values):
        if i - last >= 60:                       # 같은 국면 중복 제거
            keep.append(i); last = i
    return d, [d["date"].iloc[i] for i in keep]


def run(sym, market_label, thr):
    big = pd.read_parquet(RAW / "universe_long.parquet")
    meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
    codes = set(meta.loc[meta["market"] == market_label, "code"])
    big = big[big["code"].isin(codes)]
    idx, events = index_events(sym, thr)
    idx_s = idx.rename(columns={"Close": "mclose"})[["date", "mclose", "Open"]]
    print(f"\n########## {market_label}: 사건 {len(events)}건 ##########")

    feat = {}
    for code, d in big.groupby("code", sort=False):
        if len(d) < 300:
            continue
        feat[code] = add_market(add_features(d), idx_s[["date", "mclose"]]).reset_index(drop=True)

    allrows = []
    for ev in events:
        rows = []
        ipos = idx.index[idx["date"] == ev]
        if len(ipos) == 0 or ipos[0] + 1 >= len(idx) or ipos[0] + 1 + HZ >= len(idx):
            continue
        i0 = ipos[0]
        bench = idx["Close"].iloc[i0 + 1 + HZ] / idx["Open"].iloc[i0 + 1] - 1
        for code, d in feat.items():
            w = (d["date"] >= ev) & (d["date"] <= idx["date"].iloc[min(i0 + WINDOW, len(idx) - 1)])
            if not w.any():
                continue
            if d.loc[w, "tv20"].median() < 5e8 or d.loc[w, "close"].median() < 1000:
                continue
            s = (t1_ma20_reclaim(d).fillna(False)) & w
            pos = np.flatnonzero(s.values)
            if len(pos) == 0 or pos[0] + 1 + HZ >= len(d):
                continue
            i = pos[0]
            e = d["open"].iloc[i + 1]
            if not np.isfinite(e) or e <= 0:
                continue
            rows.append(dict(event=ev.date(), code=code,
                             x=d["close"].iloc[i + 1 + HZ] / e - 1 - bench,
                             dd=d["dd"].iloc[i],
                             rs=d["rs"].iloc[i] / d["rs"].iloc[max(0, i - 120)] - 1,
                             h1=d["close"].iloc[i] / d["close"].iloc[max(0, i - 150)] - 1))
        if len(rows) < 20:
            continue
        r = pd.DataFrame(rows)
        allrows.append(r)
        rs_top = r["rs"] >= r["rs"].quantile(0.8)
        dd_mid = r["dd"] <= -0.43
        print(f"{ev.date()} n={len(r):4d} 전체 {r['x'].mean():+6.1%} | "
              f"RS상위20% {r.loc[rs_top,'x'].mean():+6.1%}(n={rs_top.sum():3d}) | "
              f"낙폭깊음 {r.loc[dd_mid,'x'].mean():+6.1%}(n={dd_mid.sum():3d}) | 지수 {bench:+.1%}")

    if not allrows:
        return
    A = pd.concat(allrows, ignore_index=True)
    A.to_csv(RAW / f"replicate2_{market_label}.csv", index=False, encoding="utf-8-sig")
    print(f"\n--- {market_label} 사건 평균으로 집계(사건 동일가중) ---")
    per = []
    for ev, g in A.groupby("event"):
        rs_top = g["rs"] >= g["rs"].quantile(0.8)
        dd_mid = g["dd"] <= -0.43
        per.append(dict(event=ev, 전체=g["x"].mean(),
                        RS상위=g.loc[rs_top, "x"].mean(),
                        낙폭깊음=g.loc[dd_mid, "x"].mean() if dd_mid.sum() >= 5 else np.nan))
    P = pd.DataFrame(per)
    for col in ["전체", "RS상위", "낙폭깊음"]:
        x = P[col].dropna().values
        if len(x) < 3:
            continue
        bs = rng.choice(x, size=(4000, len(x)), replace=True).mean(axis=1)
        lo, hi = np.percentile(bs, [2.5, 97.5])
        print(f"  {col:6s} 사건 {len(x)}건 평균 {x.mean():+.1%} 중앙 {np.median(x):+.1%} "
              f"양수사건 {(x>0).sum()}/{len(x)} 95%CI[{lo:+.1%},{hi:+.1%}]")


for sym, lab, thr in [("KS11", "KOSPI", -0.20), ("KQ11", "KOSDAQ", -0.25)]:
    run(sym, lab, thr)
