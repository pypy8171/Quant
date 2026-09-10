# -*- coding: utf-8 -*-
"""사용자 전제(초중반 상승 -> 6~7월 급락 -> 바닥다지기 -> 최근 2주 재상승)를 데이터로 검증한다."""
import os, json, sys
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
RAW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "raw")
CODES = json.load(open(os.path.join(RAW, "codes.json"), encoding="utf-8"))

def load(code):
    df = pd.read_csv(os.path.join(RAW, f"{code}.csv"), index_col=0, parse_dates=True)
    return df

rows = []
for name, code in list(CODES.items()) + [("KOSPI", "KS11"), ("KOSDAQ", "KQ11")]:
    d = load(code)
    y = d.loc["2026-01-01":]
    if y.empty:
        continue
    pre = y.loc[:"2026-07-30"]              # 저점일까지를 고점 탐색 구간으로 (06-05 절단은 오류였음)
    peak_i = pre["Close"].idxmax()
    peak = pre["Close"].max()
    crash = y.loc["2026-06-01":"2026-08-05"]  # 급락~바닥 후보 구간
    low_i = crash["Close"].idxmin()
    low = crash["Close"].min()
    last = y["Close"].iloc[-1]
    d2w = y["Close"].iloc[-11]              # 약 2주 전(10영업일)
    ytd0 = y["Close"].iloc[0]
    rows.append(dict(
        종목=name, 시가=round(ytd0), 상반기고점=round(peak), 고점일=str(peak_i.date()),
        저점=round(low), 저점일=str(low_i.date()),
        연초대비고점=f"{peak/ytd0-1:+.0%}", 낙폭=f"{low/peak-1:+.0%}",
        저점대비현재=f"{last/low-1:+.0%}", 최근2주=f"{last/d2w-1:+.0%}",
        고점회복=f"{last/peak-1:+.0%}",
    ))

df = pd.DataFrame(rows)
print(df.to_string(index=False))
df.to_csv(os.path.join(RAW, "characterize.csv"), index=False, encoding="utf-8-sig")
