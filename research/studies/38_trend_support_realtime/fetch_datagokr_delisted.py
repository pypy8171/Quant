"""스터디 38 3회차 — 상장폐지 종목의 무수정 일봉(2020-01 이후)을 data.go.kr 주식시세정보에서 종목별로 받는다.

왜: 다음(Daum) 차트는 상장폐지 종목에 403 을 준다(fetch_daum_unadjusted.py). data.go.kr 금융위원회 주식시세정보
(getStockPriceInfo)는 기준일마다 그날 상장 종목의 무수정 종가·거래량·거래대금을 주고 2020-01-02 부터 있다.
상장폐지일이 2020-01-02 이후인 종목만 받는다. 그 전 구간은 이 출처로 채울 수 없다(round3_factor_fix.py 가 따로 다룬다).

출처: https://apis.data.go.kr/1160100/service/GetStockSecuritiesInfoService/getStockPriceInfo
      (likeSrtnCd=<코드>, beginBasDt=20200101, endBasDt=20261008). 키는 PYQuant/data/keys.py load_key("datagokr") — 값은 찍지 않는다.
등급: B(받은 날 고정).
산출: data/datagokr_unadjusted/<코드>.parquet (date int, open, high, low, close, volume, trade_value — 무수정)
      data/datagokr_unadjusted_meta.json
재실행: py research/studies/38_trend_support_realtime/fetch_datagokr_delisted.py (있는 파일은 건너뜀)
"""
from __future__ import annotations

import datetime
import json
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.parse import unquote

import numpy as np
import pandas as pd
import requests

STUDY = Path(__file__).resolve().parent
REPOSITORY = STUDY.parents[2]
sys.path.insert(0, str(REPOSITORY / "PYQuant"))

from data.keys import load_key  # noqa: E402

BARS_PATH = REPOSITORY / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
OUTPUT_DIRECTORY = STUDY / "data" / "datagokr_unadjusted"
META = STUDY / "data" / "datagokr_unadjusted_meta.json"
URL = "https://apis.data.go.kr/1160100/service/GetStockSecuritiesInfoService/getStockPriceInfo"
BEGIN, END = "20200101", "20261008"
ROWS_PER_PAGE = 1000
WORKERS = 6            # 동시 요청 수(내가 정함 — 한 종목 응답이 10초 넘게 걸려 나눠 받는다)


def number(value) -> float:
    try:
        return float(str(value).replace(",", ""))
    except (TypeError, ValueError):
        return np.nan


def fetch_one(code: str, key: str) -> str:
    items = []
    page = 1

    while True:
        parameters = {"serviceKey": key, "resultType": "json", "numOfRows": ROWS_PER_PAGE, "pageNo": page,
                      "beginBasDt": BEGIN, "endBasDt": END, "likeSrtnCd": code}
        body = None

        for attempt in range(4):
            try:
                response = requests.get(URL, params=parameters, timeout=30)
                body = (response.json().get("response", {}) or {}).get("body", {}) or {}
                break
            except Exception:   # noqa: BLE001 — 502 는 HTML 이라 json 해석이 실패한다. 다시 시도한다.
                time.sleep(1.0 * (attempt + 1))

        if body is None:
            return "request_failed"

        raw = (body.get("items", {}) or {}).get("item", []) or []
        raw = [raw] if isinstance(raw, dict) else raw
        items.extend(raw)

        if not raw or len(items) >= int(number(body.get("totalCount", 0)) or 0) or len(raw) < ROWS_PER_PAGE:
            break

        page += 1

    rows = [item for item in items if str(item.get("srtnCd", "")).lstrip("A")[-6:] == code]

    if not rows:
        return "empty"

    frame = pd.DataFrame({"date": [int(item["basDt"]) for item in rows],
                          "open": [number(item.get("mkp")) for item in rows],
                          "high": [number(item.get("hipr")) for item in rows],
                          "low": [number(item.get("lopr")) for item in rows],
                          "close": [number(item.get("clpr")) for item in rows],
                          "volume": [number(item.get("trqu")) for item in rows],
                          "trade_value": [number(item.get("trPrc")) for item in rows]})
    frame = frame.drop_duplicates("date").sort_values("date").reset_index(drop=True)
    frame.to_parquet(OUTPUT_DIRECTORY / f"{code}.parquet", index=False)
    return "ok"


def main() -> None:
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
    key = unquote(load_key("datagokr"))
    bars = pd.read_parquet(BARS_PATH, columns=["code", "Date", "delisted"])
    summary = bars.groupby("code").agg(delisted=("delisted", "last"), last=("Date", "max"))
    targets = sorted(summary[summary["delisted"] & (summary["last"] >= pd.Timestamp("2020-01-02"))].index)
    results = {code: "ok" for code in targets if (OUTPUT_DIRECTORY / f"{code}.parquet").exists()}
    pending = [code for code in targets if code not in results]

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        for index, (code, status) in enumerate(zip(pending, pool.map(lambda code: fetch_one(code, key), pending))):
            results[code] = status

            if (index + 1) % 50 == 0:
                print(f"  {index + 1}/{len(pending)}", flush=True)

    meta = {"source": URL, "range": [BEGIN, END], "fetched_on": datetime.date.today().isoformat(), "targets": len(targets),
            "ok": sum(status == "ok" for status in results.values()),
            "failures": {code: status for code, status in results.items() if status != "ok"}, "grade": "B (받은 날 고정)"}
    META.write_bytes((json.dumps(meta, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))
    print(f"대상 {len(targets)}, 성공 {meta['ok']}, 실패 {len(meta['failures'])}", flush=True)


if __name__ == "__main__":
    main()
