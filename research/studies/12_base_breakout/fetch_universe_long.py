# -*- coding: utf-8 -*-
"""KOSPI+KOSDAQ 보통주 전종목 일봉 수집 → raw/universe.parquet.

선택편향 방어용. 18종만 보면 사후선택이므로, 규칙은 전 종목에 걸어 검증한다.
한계: data.go.kr 스냅샷은 2026-09-07 시점 생존 종목 → 상장폐지 종목 누락(survivorship).
"""
import os, sys, json, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pandas as pd
import FinanceDataReader as fdr

sys.stdout.reconfigure(encoding="utf-8")
HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
RAW.mkdir(exist_ok=True)
sys.path.insert(0, str(HERE.parents[2] / "PYQuant"))
from data.datagokr_source import DataGoKrSource  # noqa: E402

START, END = "2005-01-01", "2026-09-09"

src = DataGoKrSource()
snap = src._snapshot("2026-09-07")
meta = pd.DataFrame(snap)
meta = meta[meta["market"].isin(["KOSPI", "KOSDAQ"])].drop_duplicates("code")
meta.to_csv(RAW / "meta.csv", index=False, encoding="utf-8-sig")
codes = meta["code"].tolist()
print(f"universe={len(codes)}", flush=True)

def one(code):
    for attempt in range(3):
        try:
            df = fdr.DataReader(code, START, END)
            if df is None or df.empty:
                return None
            df = df[["Open", "High", "Low", "Close", "Volume"]].copy()
            df["code"] = code
            return df.reset_index()
        except Exception:
            time.sleep(0.4 * (attempt + 1))
    return None

out, fail = [], 0
with ThreadPoolExecutor(max_workers=12) as ex:
    for i, r in enumerate(ex.map(one, codes)):
        if r is None:
            fail += 1
        else:
            out.append(r)
        if (i + 1) % 200 == 0:
            print(f"{i+1}/{len(codes)} fail={fail}", flush=True)

big = pd.concat(out, ignore_index=True)
big.columns = [c.lower() for c in big.columns]
big.to_parquet(RAW / "universe_long.parquet", index=False)
print(f"done rows={len(big):,} codes={big['code'].nunique()} fail={fail}", flush=True)
