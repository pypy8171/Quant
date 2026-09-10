# -*- coding: utf-8 -*-
"""같은 트리거를 전 종목에 걸어 기저율(base rate)을 잰다.

18종만 보면 신호가 좋아 보이는 게 당연하다. 전 종목에 걸었을 때
같은 신호가 얼마나 자주 실패하는지가 진짜 정보다.
"""
import sys, json
from pathlib import Path
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from signals import add_features, add_market, TRIGGERS

sys.stdout.reconfigure(encoding="utf-8")
RAW = HERE / "raw"
WIN0, WIN1 = "2026-08-01", "2026-08-20"      # 신호 관측 창(바닥 직후)
ASOF = "2026-09-09"

big = pd.read_parquet(RAW / "universe.parquet")
meta = pd.read_csv(RAW / "meta.csv", dtype={"code": str})
name_of = dict(zip(meta["code"], meta["name"]))
mktcap_of = dict(zip(meta["code"], meta["mktcap"]))
ks = pd.read_csv(RAW / "KS11.csv", parse_dates=["Date"]).rename(
    columns={"Date": "date", "Close": "mclose"})[["date", "mclose"]]
CODES18 = set(json.load(open(RAW / "codes.json", encoding="utf-8")).values())

use = {k: v for k, v in TRIGGERS.items() if k != "T4_double_bottom"}  # 창 내 미발생·비용 큼
rows = []
for code, d in big.groupby("code", sort=False):
    if len(d) < 260:
        continue
    d = add_market(add_features(d), ks).reset_index(drop=True)
    win = (d["date"] >= WIN0) & (d["date"] <= WIN1)
    if not win.any():
        continue
    liq = d.loc[win, "tv20"].median()
    px = d.loc[win, "close"].median()
    if not np.isfinite(liq) or liq < 1e9 or px < 1000:   # 일평균 거래대금 10억·주가 1천원 하한
        continue
    last = d["close"].iloc[-1]
    ref = d.loc[win].iloc[0]
    for tname, fn in use.items():
        sig = (fn(d).fillna(False)) & win
        pos = np.flatnonzero(sig.values)
        if len(pos) == 0:
            continue
        i = pos[0]                                       # 창 내 최초 신호만
        if i + 1 >= len(d):
            continue
        entry = d["open"].iloc[i + 1]
        if not np.isfinite(entry) or entry <= 0:
            continue
        j10, j20 = min(i + 10, len(d) - 1), min(i + 20, len(d) - 1)
        rows.append(dict(
            code=code, name=name_of.get(code, ""), trig=tname,
            sig_date=d["date"].iloc[i].date(), entry=entry,
            r_asof=last / entry - 1,
            r10=d["close"].iloc[j10] / entry - 1,
            r20=d["close"].iloc[j20] / entry - 1,
            mae20=d["low"].iloc[i + 1:j20 + 1].min() / entry - 1,
            dd_at_sig=d["dd"].iloc[i],                   # 250일 고점 대비 낙폭
            h1_run=ref["close"] / d["close"].iloc[max(0, i - 150)] - 1,   # 직전 150일 상승률
            rs_rank_raw=d["rs"].iloc[i] / d["rs"].iloc[max(0, i - 120)] - 1,
            tv20=liq, mktcap=mktcap_of.get(code, np.nan),
            is18=code in CODES18))

r = pd.DataFrame(rows)
r.to_csv(RAW / "signals_universe.csv", index=False, encoding="utf-8-sig")
print(f"신호 표본 {len(r):,}건 / 종목 {r['code'].nunique():,}개 (유동성 필터 통과)\n")

print("=== 트리거별 기저율 (2026-08-01~08-20 최초신호, 익일시가 진입, 09-09 평가) ===")
g = r.groupby("trig").agg(
    n=("code", "size"),
    승률=("r_asof", lambda x: (x > 0).mean()),
    평균=("r_asof", "mean"), 중앙=("r_asof", "median"),
    상위10p=("r_asof", lambda x: x.quantile(0.9)),
    하위10p=("r_asof", lambda x: x.quantile(0.1)),
    평균MAE20=("mae20", "mean"))
print((g * 1).round(3).to_string())

print("\n=== 같은 트리거에서 18종 vs 나머지 ===")
cmp = r.groupby(["trig", "is18"])["r_asof"].agg(["size", "mean", "median"]).round(3)
print(cmp.to_string())
