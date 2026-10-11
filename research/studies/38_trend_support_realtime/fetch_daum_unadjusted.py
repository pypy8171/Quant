"""스터디 38 3회차 — 상장 중인 종목의 무수정 일봉(종가·거래량·거래대금)을 다음(Daum) 금융 차트에서 받아 종목별로 저장한다.

왜 받나: 공용 일봉 PYQuant/data/bars_all_pit_v2.parquet 의 가격은 수정주가라, 스터디 33 load_inputs 가
월말 무수정 종가(data.go.kr, 2020-01 부터)로 factor 를 만들어 곱한다. 2020 이전은 첫 월말 factor 를 뒤로 늘려 쓰고,
월 중간에 생긴 감자·분할은 다음 월말까지 반영되지 않는다 → 2회차 turnover_outliers.csv 의 25 종목처럼 거래대금이
수십 배 부풀려진 날이 생긴다. 다음 차트는 adjusted=false 로 그날 실제 종가와 그날 거래대금(candleAccTradePrice)을 준다.

출처: https://finance.daum.net/api/charts/A<코드>/days?limit=5000&adjusted=false  (Referer 필요, 한 번에 최대 5000 거래일 —
      2026-10-08 기준 2006-06 부터). 상장폐지 종목은 403 을 돌려준다 → 이 파일은 상장 중 종목만 받고 실패 목록을 남긴다.
등급: B(받은 날 고정 — 과거 무수정 값은 고쳐 쓰지 않는 값이지만 사이트가 정정할 수 있다). 받은 날은 메타 파일에 적는다.
산출: data/daum_unadjusted/<코드>.parquet  (date int, open, high, low, close, volume, trade_value — 모두 무수정)
      data/daum_unadjusted_meta.json        (받은 날·종목 수·실패 목록)
재실행: py research/studies/38_trend_support_realtime/fetch_daum_unadjusted.py  (있는 파일은 건너뜀, --refresh 로 다시 받음)
"""
from __future__ import annotations

import argparse
import datetime
import json
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
REPOSITORY = STUDY.parents[2]
BARS_PATH = REPOSITORY / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
OUTPUT_DIRECTORY = STUDY / "data" / "daum_unadjusted"
META = STUDY / "data" / "daum_unadjusted_meta.json"
URL = "https://finance.daum.net/api/charts/A{code}/days?limit=5000&adjusted=false"
WORKERS = 3            # 동시 요청 수(내가 정함 — 사이트 부담을 줄이려 작게)
PAUSE_SECONDS = 0.3    # 요청 사이 쉬는 시간(내가 정함)
RETRIES = 3


def fetch_one(code: str) -> tuple[str, str]:
    path = OUTPUT_DIRECTORY / f"{code}.parquet"
    headers = {"User-Agent": "Mozilla/5.0", "Referer": f"https://finance.daum.net/quotes/A{code}"}
    last_error = ""

    for attempt in range(RETRIES):
        try:
            request = urllib.request.Request(URL.format(code=code), headers=headers)

            with urllib.request.urlopen(request, timeout=60) as response:
                payload = json.loads(response.read().decode("utf-8"))

            rows = payload.get("data") or []

            if not rows:
                return code, "empty"

            frame = pd.DataFrame({
                "date": [int(row["date"].replace("-", "")) for row in rows],
                "open": [float(row["openingPrice"]) for row in rows],
                "high": [float(row["highPrice"]) for row in rows],
                "low": [float(row["lowPrice"]) for row in rows],
                "close": [float(row["tradePrice"]) for row in rows],
                "volume": [float(row["candleAccTradeVolume"]) for row in rows],
                "trade_value": [float(row["candleAccTradePrice"]) for row in rows]})
            frame = frame.drop_duplicates("date").sort_values("date").reset_index(drop=True)
            frame.to_parquet(path, index=False)
            time.sleep(PAUSE_SECONDS)
            return code, "ok"
        except urllib.error.HTTPError as error:
            last_error = f"http {error.code}"

            if error.code == 403:
                return code, last_error
        except Exception as error:   # noqa: BLE001 — 네트워크 오류는 다시 시도하고 마지막 사유만 남긴다
            last_error = type(error).__name__

        time.sleep(2.0 * (attempt + 1))

    return code, last_error


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--refresh", action="store_true")
    arguments = parser.parse_args()
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
    bars = pd.read_parquet(BARS_PATH, columns=["code", "delisted"])
    listed = sorted(bars.groupby("code")["delisted"].last().loc[lambda series: ~series].index)
    targets = [code for code in listed if arguments.refresh or not (OUTPUT_DIRECTORY / f"{code}.parquet").exists()]
    print(f"상장 중 {len(listed)} 종목, 받을 종목 {len(targets)}", flush=True)
    results = {}

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        for index, (code, status) in enumerate(pool.map(fetch_one, targets)):
            results[code] = status

            if (index + 1) % 200 == 0:
                print(f"  {index + 1}/{len(targets)}", flush=True)

    previous = json.loads(META.read_text(encoding="utf-8")) if META.exists() else {}
    failures = {code: status for code, status in {**previous.get("failures", {}), **results}.items() if status != "ok"}
    failures = {code: status for code, status in failures.items() if not (OUTPUT_DIRECTORY / f"{code}.parquet").exists()}
    meta = {"source": URL, "fetched_on": datetime.date.today().isoformat(), "listed_codes": len(listed),
            "files": len(list(OUTPUT_DIRECTORY.glob("*.parquet"))), "failures": failures,
            "grade": "B (받은 날 고정)"}
    META.write_bytes((json.dumps(meta, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))
    print(f"파일 {meta['files']}, 실패 {len(failures)}", flush=True)


if __name__ == "__main__":
    np.seterr(all="ignore")
    main()
