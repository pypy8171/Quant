# -*- coding: utf-8 -*-
"""바닥 사건 이후 무엇을 샀어야 하는가 — 신호시점 관측값의 예측력을 사건별 순위상관으로 검정.

임계값을 고르지 않는다. 각 사건에서 피처와 전방 60일 초과수익의 순위상관(IC)을 재고,
사건을 건너 부호가 유지되는지만 본다. 임계 적합이 없으므로 과최적화 여지가 작다.
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from signals import add_features, add_market, t1_ma20_reclaim

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"
rng = np.random.default_rng(11)
HZ, WINDOW = 60, 15


def spearman(a, b):
    a = pd.Series(a).rank().values
    b = pd.Series(b).rank().values
    if np.std(a) == 0 or np.std(b) == 0:
        return np.nan
    return float(np.corrcoef(a, b)[0, 1])


def index_events(sym, thr):
    d = pd.read_csv(RAW / f"{sym}_long.csv", parse_dates=["Date"]).rename(columns={"Date": "date"})
    d = d.sort_values("date").reset_index(drop=True)
    c = d["Close"]
    ma20 = c.rolling(20).mean()
    dd = c / c.rolling(250, min_periods=120).max() - 1
    sig = (c > ma20) & (c.shift(1) <= ma20.shift(1)) & (dd <= thr)
    keep, last = [], -999
    for i in np.flatnonzero(sig.values):
        if i - last >= 60:
            keep.append(i); last = i
    return d, keep


def feats_at(d, i):
    """신호 시점 i에서만 보이는 값들. 미래 봉을 참조하지 않는다."""
    c = d["close"].iloc[i]
    return dict(
        낙폭깊이     = -d["dd"].iloc[i],
        시총로그     = np.log(max(c * 1.0, 1)),
        거래대금로그  = np.log(max(d["tv20"].iloc[i], 1)),
        거래량급증   = d["volume"].iloc[i] / max(d["vol20"].iloc[i], 1),
        변동성      = d["vol20"].iloc[i],
        저점대비반등  = c / max(d["lo60"].iloc[i], 1) - 1,
        바닥후경과일  = d["days_since_lo60"].iloc[i],
        상대강도120 = d["rs"].iloc[i] / max(d["rs"].iloc[max(0, i - 120)], 1e-9) - 1,
        상대강도20  = d["rs"].iloc[i] / max(d["rs"].iloc[max(0, i - 20)], 1e-9) - 1,
        직전1년상승  = c / max(d["close"].iloc[max(0, i - 250)], 1) - 1,
        주가로그     = np.log(max(c, 1)),
        이평200이격 = c / max(d["ma200"].iloc[i], 1) - 1,
    )


def run(sym, market_label, thr, mcap):
    big = pd.read_parquet(RAW / "universe_long.parquet")
    meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
    codes = set(meta.loc[meta["market"] == market_label, "code"])
    big = big[big["code"].isin(codes)]
    idx, epos = index_events(sym, thr)
    idx_s = idx.rename(columns={"Close": "mclose"})[["date", "mclose"]]

    feat = {}
    for code, d in big.groupby("code", sort=False):
        if len(d) < 300:
            continue
        feat[code] = add_market(add_features(d), idx_s).reset_index(drop=True)

    out = []
    for i0 in epos:
        if i0 + 1 + HZ >= len(idx):
            continue
        ev = idx["date"].iloc[i0]
        bench = idx["Close"].iloc[i0 + 1 + HZ] / idx["Open"].iloc[i0 + 1] - 1
        rows = []
        for code, d in feat.items():
            w = (d["date"] >= ev) & (d["date"] <= idx["date"].iloc[min(i0 + WINDOW, len(idx) - 1)])
            if not w.any():
                continue
            if d.loc[w, "tv20"].median() < 5e8 or d.loc[w, "close"].median() < 1000:
                continue
            s = t1_ma20_reclaim(d).fillna(False) & w
            pos = np.flatnonzero(s.values)
            if len(pos) == 0 or pos[0] + 1 + HZ >= len(d):
                continue
            i = pos[0]
            e = d["open"].iloc[i + 1]
            if not np.isfinite(e) or e <= 0:
                continue
            r = feats_at(d, i)
            r.update(code=code, event=ev.date(), market=market_label,
                     x=d["close"].iloc[i + 1 + HZ] / e - 1 - bench, bench=bench)
            rows.append(r)
        if len(rows) >= 50:
            df = pd.DataFrame(rows)
            df["시총로그"] = np.log(df["code"].map(mcap).fillna(df["시총로그"].median()))
            out.append(df)
    return out


meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
mcap = dict(zip(meta["code"], meta["mktcap"].replace(0, np.nan)))
panels = run("KS11", "KOSPI", -0.20, mcap) + run("KQ11", "KOSDAQ", -0.25, mcap)
A = pd.concat(panels, ignore_index=True)
A.to_csv(RAW / "crosssec.csv", index=False, encoding="utf-8-sig")

FE = [c for c in A.columns if c not in ("code", "event", "market", "x", "bench")]
evs = sorted(A["event"].unique())
print(f"사건 {len(evs)}건: " + ", ".join(str(e) for e in evs))
print(f"\n{'피처':<12s} {'평균IC':>7s} {'중앙IC':>7s} {'부호일치':>8s} {'t':>6s}  사건별 IC")
res = []
for f in FE:
    ics = np.array([spearman(g[f].values, g["x"].values) for _, g in A.groupby("event")])
    ics = ics[np.isfinite(ics)]
    if len(ics) < 5:
        continue
    same = max((ics > 0).sum(), (ics < 0).sum())
    t = ics.mean() / (ics.std(ddof=1) / np.sqrt(len(ics)))
    res.append((abs(ics.mean()), f, ics, same, t))
for _, f, ics, same, t in sorted(res, reverse=True):
    bar = " ".join(f"{v:+.2f}" for v in ics)
    print(f"{f:<12s} {ics.mean():+7.3f} {np.median(ics):+7.3f} {same:>5d}/{len(ics):<3d} {t:+6.2f}  {bar}")
