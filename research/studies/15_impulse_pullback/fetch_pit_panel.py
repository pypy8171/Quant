# -*- coding: utf-8 -*-
"""15_impulse_pullback 입력 데이터 준비 — PIT 유니버스 단면 + 일봉 패널 + 지수.

재현 정보
  - 유니버스: PYQuant/data/datagokr_source.py `universe_top(on_date, ...)`를 캐시된
    스냅샷 날짜마다 그대로 호출한다(정적 리스트 금지). 네트워크는 쓰지 않는다 —
    `PYQuant/.datagokr_cache/univ_YYYYMMDD.parquet`가 있는 날짜만 단면으로 쓴다.
  - 일봉: research/studies/12_base_breakout/raw/universe_long.parquet(FDR, 2026-09-09 수집)을
    재사용하고, 거기 없는 코드(대부분 그 뒤 상폐·이관분)만 FDR로 추가 수집한다.
  - 지수: research/studies/12_base_breakout/raw/{KS11,KQ11}_long.csv(FDR, 2026-09-07 수집).

산출물
  raw/pit_universe.csv   snapshot_date, code, market   (평가일 t는 t 이하 최신 단면을 쓴다)
  raw/px.parquet         date, code, open, high, low, close, volume
  raw/KS11.csv, raw/KQ11.csv
  raw/fetch_manifest.json  재현용 메타(수집시각·행수·실패코드)
"""
import json
import os
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path

import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
RAW.mkdir(exist_ok=True)
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "PYQuant"))
from data.datagokr_source import DataGoKrSource  # noqa: E402

START, END = "2019-07-01", "2026-09-10"   # 신호 워밍업(60일 SMA) 여유를 두고 시작
UNIV_FROM, UNIV_TO = "2019-07-01", "2026-09-01"
TOP_KOSPI, TOP_KOSDAQ = 500, 500
MIN_TURNOVER = 1e8         # 단면 편입 최소 거래대금(사건 정의의 5억 하한은 별도로 또 건다)
S12 = ROOT / "research" / "studies" / "12_base_breakout" / "raw"


def cached_snapshot_dates(cache: Path) -> list[str]:
    out = []
    for p in cache.glob("univ_*.parquet"):
        m = re.fullmatch(r"univ_(\d{8})", p.stem)
        if not m:
            continue
        ymd = m.group(1)
        iso = f"{ymd[:4]}-{ymd[4:6]}-{ymd[6:]}"
        if UNIV_FROM <= iso <= UNIV_TO:
            out.append(iso)
    return sorted(out)


def build_pit_universe() -> pd.DataFrame:
    src = DataGoKrSource(market="ALL")
    dates = cached_snapshot_dates(src._cache)
    print(f"[univ] 캐시된 스냅샷 {len(dates)}개 ({dates[0]} ~ {dates[-1]})", flush=True)
    rows = []
    for iso in dates:
        codes = src.universe_top(iso, n=TOP_KOSPI,
                                 min_turnover=MIN_TURNOVER,
                                 sizes={"KOSPI": TOP_KOSPI, "KOSDAQ": TOP_KOSDAQ})
        mk = {r["code"]: r["market"] for r in src._snapshot(iso)}
        for c in codes:
            rows.append({"snapshot_date": iso, "code": c, "market": mk.get(c, "")})
    df = pd.DataFrame(rows)
    df.to_csv(RAW / "pit_universe.csv", index=False, encoding="utf-8-sig")
    print(f"[univ] 단면 {df['snapshot_date'].nunique()}개, 연인원 {len(df):,}행, "
          f"고유코드 {df['code'].nunique()}개", flush=True)
    return df


def fdr_one(code: str):
    import FinanceDataReader as fdr
    for attempt in range(3):
        try:
            d = fdr.DataReader(code, START, END)
            if d is None or d.empty:
                return code, None
            d = d[["Open", "High", "Low", "Close", "Volume"]].copy()
            d.columns = ["open", "high", "low", "close", "volume"]
            d["code"] = code
            return code, d.reset_index().rename(columns={"Date": "date", "index": "date"})
        except Exception:
            time.sleep(0.4 * (attempt + 1))
    return code, None


def build_prices(codes: set[str]) -> pd.DataFrame:
    big = pd.read_parquet(S12 / "universe_long.parquet")
    big = big[big["code"].isin(codes)]
    big = big[(big["date"] >= START) & (big["date"] <= END)]
    have = set(big["code"].unique())
    todo = sorted(codes - have)
    print(f"[px] 12번 스터디 패널 재사용 {len(have)}코드 / 추가수집 대상 {len(todo)}코드", flush=True)
    got, fail = [], []
    if todo:
        with ThreadPoolExecutor(max_workers=12) as ex:
            for i, (code, d) in enumerate(ex.map(fdr_one, todo)):
                if d is None:
                    fail.append(code)
                else:
                    got.append(d)
                if (i + 1) % 25 == 0:
                    print(f"  {i+1}/{len(todo)} 실패 {len(fail)}", flush=True)
    px = pd.concat([big] + got, ignore_index=True) if got else big.copy()
    px["date"] = pd.to_datetime(px["date"])
    px = px.sort_values(["code", "date"]).reset_index(drop=True)
    px = px[["date", "code", "open", "high", "low", "close", "volume"]]
    px.to_parquet(RAW / "px.parquet", index=False)
    print(f"[px] 행 {len(px):,} 코드 {px['code'].nunique()} "
          f"{px['date'].min().date()}~{px['date'].max().date()} 수집실패 {len(fail)}", flush=True)
    return px, fail


def copy_index():
    for f in ("KS11_long.csv", "KQ11_long.csv"):
        d = pd.read_csv(S12 / f, parse_dates=["Date"])
        d = d.rename(columns={"Date": "date"})[["date", "Open", "High", "Low", "Close"]]
        d.columns = ["date", "open", "high", "low", "close"]
        d = d[(d["date"] >= START) & (d["date"] <= END)].sort_values("date")
        out = RAW / f.replace("_long", "")
        d.to_csv(out, index=False, encoding="utf-8-sig")
        print(f"[idx] {out.name} {len(d)}행 {d['date'].min().date()}~{d['date'].max().date()}")


if __name__ == "__main__":
    univ = build_pit_universe()
    px, fail = build_prices(set(univ["code"]))
    copy_index()
    (RAW / "fetch_manifest.json").write_text(json.dumps({
        "built_at": datetime.now().isoformat(timespec="seconds"),
        "universe_source": "PYQuant/data/datagokr_source.py universe_top (캐시 스냅샷 전용)",
        "snapshot_dates": sorted(univ["snapshot_date"].unique().tolist()),
        "top_sizes": {"KOSPI": TOP_KOSPI, "KOSDAQ": TOP_KOSDAQ},
        "min_turnover": MIN_TURNOVER,
        "price_source": "12_base_breakout/raw/universe_long.parquet (FDR 2026-09-09) + FDR 추가수집",
        "index_source": "12_base_breakout/raw/{KS11,KQ11}_long.csv (FDR 2026-09-07)",
        "px_rows": int(len(px)), "px_codes": int(px["code"].nunique()),
        "fetch_failed_codes": fail,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print("[done] raw/fetch_manifest.json 기록")
