"""스터디 38 2회차 — 코스피 지수 일봉을 받아 저장하고, 저장소에 있던 yfinance 캐시와 겹치는 날을 대조한다.

왜 새로 받나: 저장소의 코스피 일봉(PYQuant/.index_cache/idx__KS11_1985-01-01_2026-08-15_adj.parquet, yfinance ^KS11)은
2026-08-14 에서 끝나 종목 자료 끝(2026-10-08)까지 37 거래일이 빈다. FinanceDataReader(KRX)는 2026-09-17 까지만 준다.
네이버 siseJson(symbol=KOSPI)은 1990-01-04 – 2026-10-08 을 한 번에 준다 → 이것을 쓴다(시점 고정 등급 B: 지수는 고쳐 쓰지 않는 값,
받은 날 2026-10-10).

출처: https://api.finance.naver.com/siseJson.naver?symbol=KOSPI&requestType=1&startTime=19900101&endTime=20261010&timeframe=day
산출: data/kospi_daily_naver.parquet (date int YYYYMMDD, open, high, low, close, volume)
      data/kospi_daily_check.json     (yfinance 캐시와 겹치는 날 수·종가 차이 분포·한쪽에만 있는 날)
재실행: py research/studies/38_trend_support_realtime/fetch_kospi_daily.py
"""
from __future__ import annotations

import ast
import json
import urllib.request
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
REPOSITORY = STUDY.parents[2]
OUTPUT = STUDY / "data" / "kospi_daily_naver.parquet"
CHECK = STUDY / "data" / "kospi_daily_check.json"
YFINANCE_CACHE = REPOSITORY / "PYQuant" / ".index_cache" / "idx__KS11_1985-01-01_2026-08-15_adj.parquet"
URL = ("https://api.finance.naver.com/siseJson.naver?symbol=KOSPI&requestType=1"
       "&startTime=19900101&endTime=20261010&timeframe=day")
HEADERS = {"User-Agent": "Mozilla/5.0", "Referer": "https://finance.naver.com/"}


def fetch() -> pd.DataFrame:
    request = urllib.request.Request(URL, headers=HEADERS)

    with urllib.request.urlopen(request, timeout=60) as response:
        text = response.read().decode("utf-8", errors="replace").strip()

    rows = ast.literal_eval(text)[1:]   # 첫 줄은 머리글(한글이 cp949 라 깨져 오므로 열 순서로 읽는다: 날짜·시가·고가·저가·종가·거래량)
    frame = pd.DataFrame([list(row)[:6] for row in rows], columns=["date", "open", "high", "low", "close", "volume"])
    frame["date"] = frame["date"].astype(str).astype(np.int64)

    for column in ("open", "high", "low", "close", "volume"):
        frame[column] = pd.to_numeric(frame[column], errors="coerce").astype(np.float64)

    return frame.dropna(subset=["high", "low", "close"]).drop_duplicates("date").sort_values("date").reset_index(drop=True)


def compare(frame: pd.DataFrame) -> dict:
    cached = pd.read_parquet(YFINANCE_CACHE)
    cached["date"] = cached["date"].astype(str).str.replace("-", "").astype(np.int64)
    merged = frame.merge(cached[["date", "close", "high", "low"]], on="date", suffixes=("", "_yfinance"))
    difference = (merged["close"] / merged["close_yfinance"] - 1.0) * 100.0
    window = frame[(frame["date"] >= int(cached["date"].min())) & (frame["date"] <= int(cached["date"].max()))]
    only_naver = sorted(set(window["date"]) - set(cached["date"]))
    only_yfinance = sorted(set(cached["date"]) - set(frame["date"]))
    return {"naver_rows": len(frame), "naver_first": int(frame["date"].min()), "naver_last": int(frame["date"].max()),
            "yfinance_path": str(YFINANCE_CACHE.relative_to(REPOSITORY)).replace("\\", "/"),
            "yfinance_first": int(cached["date"].min()), "yfinance_last": int(cached["date"].max()),
            "overlap_days": len(merged),
            "close_difference_percent": {"mean": float(difference.mean()), "median": float(difference.median()),
                                         "absolute_p99": float(difference.abs().quantile(0.99)),
                                         "absolute_max": float(difference.abs().max()),
                                         "days_absolute_over_0_1": int((difference.abs() > 0.1).sum())},
            "only_in_naver_within_yfinance_span": len(only_naver), "only_in_naver_sample": only_naver[:20],
            "only_in_yfinance": len(only_yfinance), "only_in_yfinance_sample": only_yfinance[:20],
            "high_low_equal_days_since_2009": int(((frame["high"] == frame["low"]) & (frame["date"] >= 20090101)).sum())}


def main() -> None:
    frame = fetch()
    OUTPUT.parent.mkdir(exist_ok=True)
    frame.to_parquet(OUTPUT, index=False)
    check = compare(frame)

    with open(CHECK, "wb") as handle:
        handle.write(json.dumps(check, ensure_ascii=False, indent=1).encode("utf-8"))

    print(json.dumps(check, ensure_ascii=False))


if __name__ == "__main__":
    main()
