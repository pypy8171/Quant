# -*- coding: utf-8 -*-
"""지목된 18종에 트리거를 걸어 '언제 신호가 났는지'를 본다.

역할 한정: 이 18종은 사후선택 표본이므로 **가설 생성 전용**이다. 성과 주장 근거로 쓰지 않는다.
"""
import json, sys
from pathlib import Path
import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from signals import add_features, add_market, TRIGGERS

sys.stdout.reconfigure(encoding="utf-8")
RAW = Path(__file__).resolve().parent / "raw"
CODES = json.load(open(RAW / "codes.json", encoding="utf-8"))

ks = pd.read_csv(RAW / "KS11.csv", parse_dates=["Date"]).rename(
    columns={"Date": "date", "Close": "mclose"})[["date", "mclose"]]

rows = []
for name, code in CODES.items():
    d = pd.read_csv(RAW / f"{code}.csv", parse_dates=["Date"]).rename(columns=str.lower)
    d = d.rename(columns={"date": "date"})
    d = add_market(add_features(d), ks)
    d = d.reset_index(drop=True)
    win = (d["date"] >= "2026-07-25") & (d["date"] <= "2026-09-09")
    last = d["close"].iloc[-1]
    for tname, fn in TRIGGERS.items():
        sig = fn(d).fillna(False) & win
        for i in np.flatnonzero(sig.values):
            if i + 1 >= len(d):
                continue
            entry = d["open"].iloc[i + 1]
            rows.append(dict(종목=name, 트리거=tname, 신호일=d["date"].iloc[i].date(),
                             진입가=entry, 현재=last, 수익률=last / entry - 1,
                             저점대비진입=entry / d["close"].loc[win].min() - 1))

r = pd.DataFrame(rows)
r.to_csv(RAW / "signals_18.csv", index=False, encoding="utf-8-sig")

print("=== 트리거별 요약 (07-25~09-09, 18종) ===")
g = r.groupby("트리거").agg(
    신호수=("종목", "size"), 커버종목=("종목", "nunique"),
    첫신호=("신호일", "min"), 최종신호=("신호일", "max"),
    평균수익=("수익률", "mean"), 중앙수익=("수익률", "median"),
    최저수익=("수익률", "min"))
print(g.to_string())

print("\n=== 종목별 최초 신호일 (트리거별) ===")
first = r.sort_values("신호일").groupby(["종목", "트리거"]).first().reset_index()
pv = first.pivot(index="종목", columns="트리거", values="신호일")
print(pv.to_string())
