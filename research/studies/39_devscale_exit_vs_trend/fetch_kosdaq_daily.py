"""스터디 39 round4 — 코스닥 지수 일봉을 받아 저장하고, 저장소에 있던 사본과 겹치는 날을 대조한다.

왜 새로 받나: 저장소의 코스닥 일봉(research/studies/35_surge_box_breakout/halt_trigger_raw/naver_KOSDAQ.parquet)은
2026-10-06 에서 끝나 종목 자료 끝(2026-10-08)까지 빈다. 같은 출처(네이버 siseJson)로 2010 년부터 다시 받는다
(시점 고정 등급 B: 지수는 고쳐 쓰지 않는 값, 받은 날은 산출 json 의 fetched_at).

출처: https://api.finance.naver.com/siseJson.naver?symbol=KOSDAQ&requestType=1&startTime=20090101&endTime=20261010&timeframe=day
산출: data/kosdaq_daily_naver.parquet (date int YYYYMMDD, open, high, low, close, volume)
      data/kosdaq_daily_check.json     (저장소 사본과 겹치는 날 수·종가 차이·받은 시각)
재실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/fetch_kosdaq_daily.py
"""
from __future__ import annotations

import ast
import datetime
import json
import urllib.request
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
REPOSITORY = STUDY.parents[2]
OUTPUT = STUDY / "data" / "kosdaq_daily_naver.parquet"
CHECK = STUDY / "data" / "kosdaq_daily_check.json"
REPOSITORY_COPY = REPOSITORY / "research" / "studies" / "35_surge_box_breakout" / "halt_trigger_raw" / "naver_KOSDAQ.parquet"
URL = ("https://api.finance.naver.com/siseJson.naver?symbol=KOSDAQ&requestType=1"
       "&startTime=20090101&endTime=20261010&timeframe=day")
HEADERS = {"User-Agent": "Mozilla/5.0", "Referer": "https://finance.naver.com/"}


def fetch() -> pd.DataFrame:
    request = urllib.request.Request(URL, headers=HEADERS)

    with urllib.request.urlopen(request, timeout=60) as response:
        text = response.read().decode("utf-8", errors="replace").strip()

    rows = ast.literal_eval(text)[1:]   # 첫 줄은 머리글. 열 순서로 읽는다: 날짜·시가·고가·저가·종가·거래량
    frame = pd.DataFrame([list(row)[:6] for row in rows], columns=["date", "open", "high", "low", "close", "volume"])
    frame["date"] = frame["date"].astype(str).astype(np.int64)

    for column in ("open", "high", "low", "close", "volume"):
        frame[column] = pd.to_numeric(frame[column], errors="coerce").astype(np.float64)

    return frame.dropna(subset=["high", "low", "close"]).drop_duplicates("date").sort_values("date").reset_index(drop=True)


def compare(frame: pd.DataFrame) -> dict:
    copy = pd.read_parquet(REPOSITORY_COPY).reset_index()
    copy["date"] = copy["date"].dt.strftime("%Y%m%d").astype(np.int64)
    merged = frame.merge(copy[["date", "open", "close"]], on="date", suffixes=("", "_copy"))
    difference = (merged["close"] / merged["close_copy"] - 1.0) * 100.0
    open_difference = (merged["open"] / merged["open_copy"] - 1.0) * 100.0
    return {"source": URL, "fetched_at": datetime.datetime.now().isoformat(timespec="seconds"),
            "rows": len(frame), "first": int(frame["date"].min()), "last": int(frame["date"].max()),
            "repository_copy": str(REPOSITORY_COPY.relative_to(REPOSITORY)).replace("\\", "/"),
            "copy_first": int(copy["date"].min()), "copy_last": int(copy["date"].max()), "overlap_days": len(merged),
            "close_difference_percent_absolute_max": float(difference.abs().max()),
            "open_difference_percent_absolute_max": float(open_difference.abs().max()),
            "days_close_absolute_over_0_01": int((difference.abs() > 0.01).sum()),
            "open_missing_or_zero_days": int(((frame["open"] <= 0) | frame["open"].isna()).sum())}


def main() -> None:
    frame = fetch()
    OUTPUT.parent.mkdir(exist_ok=True)
    frame.to_parquet(OUTPUT, index=False)
    check = compare(frame)

    with open(CHECK, "wb") as handle:
        handle.write((json.dumps(check, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))

    print(json.dumps(check, ensure_ascii=False))


if __name__ == "__main__":
    main()
