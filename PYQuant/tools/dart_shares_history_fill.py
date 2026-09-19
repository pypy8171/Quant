"""상장주식수 시점 고정 표 채우기 — `PYQuant/data/fin/shares_point_in_time.parquet`.

두 소스를 잇는다(시가총액·PBR을 과거 어느 날 값으로 만들려면 그날 알 수 있던 주식수가 필요하다).

  DART stockTotqySttus.json?crtfc_key=…&corp_code=<8자리>&bsns_year=YYYY&reprt_code=11011
      회사 하나·연도 하나에 한 번 호출. 응답 list의 행 키(2026-09-20 005930 실호출): rcept_no, se(보통주·우선주·합계·비고),
      isu_stock_totqy(발행할 주식수), istc_totqy(발행주식총수), tesstk_co(자기주식), distb_stock_co(유통주식수).
      발효일은 rcept_no 앞 8자리(접수일). 사업보고서만 받는다(연 1회, 유상증자·분할은 다음 보고서에서 잡힌다 → 등급 B).
      일일 한도 20,000건. 2015~2019 다섯 해 × 약 2,000사 ≈ 10,000건.
  data.go.kr 금융위 주식시세정보 getStockPriceInfo(basDt=YYYYMMDD)
      하루 전 종목이 한 번에 온다(lstgStCnt 상장주식수, mrktTotAmt 시가총액). 2020-01-02부터만 있다(2026-09-20 실측:
      2016·2018 기준일은 totalCount 0). 매월 마지막 거래일 하루씩 받는다 → 등급 A.

캐시: PYQuant/data/cache/dart_shares/<corp_code>_<year>_11011.json, PYQuant/data/cache/datagokr_shares/univ_<YYYYMMDD>.parquet.
있으면 건너뛴다. 실패는 각 폴더 _failed.txt에 적고 계속 간다. 020(한도 초과)이면 그 자리에서 멈추고 받은 것까지 저장한다.

출력 열: ticker, published_at, effective_date, shares_common_issued, shares_preferred_issued, treasury_common, rcept_no, source, grade
  source = "dart" | "datagokr". datagokr 행은 우선주·자기주식이 없다(결측).

실행(저장소 루트에서):
  py PYQuant/tools/dart_shares_history_fill.py --source dart --from 2015 --to 2019 --sleep 0.1
  py PYQuant/tools/dart_shares_history_fill.py --source datagokr --from 2020
  py PYQuant/tools/dart_shares_history_fill.py --build          # 캐시에서 parquet만 다시
키 값은 어디에도 찍지 않는다(`PYQuant/data/keys.py`).
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
from pathlib import Path
from urllib.parse import unquote

import pandas as pd
import requests

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from PYQuant.data.keys import load_key  # noqa: E402

FIN_PATH = REPO_ROOT / "PYQuant" / "data" / "fin" / "fin_point_in_time.parquet"
BARS_PATH = REPO_ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
OUTPUT_PATH = REPO_ROOT / "PYQuant" / "data" / "fin" / "shares_point_in_time.parquet"
DART_CACHE_DIR = REPO_ROOT / "PYQuant" / "data" / "cache" / "dart_shares"
DATAGOKR_CACHE_DIR = REPO_ROOT / "PYQuant" / "data" / "cache" / "datagokr_shares"

DART_URL = "https://opendart.fss.or.kr/api/stockTotqySttus.json"
DATAGOKR_URL = "https://apis.data.go.kr/1160100/service/GetStockSecuritiesInfoService/getStockPriceInfo"
ANNUAL_REPORT = "11011"
DART_STATUS_OK = "000"
DART_STATUS_NO_DATA = "013"
DART_STATUS_STOP = {"020": "일일 한도 초과", "800": "시스템 점검", "010": "미등록 키", "011": "만료된 키", "012": "접근 불가 IP"}
DATAGOKR_MIN_ROWS = 2_000
OUTPUT_COLUMNS = ["ticker", "published_at", "effective_date", "shares_common_issued", "shares_preferred_issued",
                  "treasury_common", "rcept_no", "source", "grade"]


class RateLimitStop(RuntimeError):
    """계속 가면 안 되는 상태코드."""


def log(message: str) -> None:
    print(f"{datetime.datetime.now():%H:%M:%S} {message}", flush=True)


def parse_count(text) -> float:
    if text is None:
        return float("nan")

    cleaned = str(text).replace(",", "").strip()

    if cleaned in ("", "-"):
        return float("nan")

    try:
        return float(cleaned)
    except ValueError:
        return float("nan")


def append_failed(folder: Path, line: str) -> None:
    folder.mkdir(parents=True, exist_ok=True)

    with (folder / "_failed.txt").open("a", encoding="utf-8") as handle:
        handle.write(line + "\n")


# ── DART ───────────────────────────────────────────────────────────────────────

def dart_jobs(year_from: int, year_to: int) -> pd.DataFrame:
    """재무 표에 사업보고서가 있는 (corp_code, stock_code, bsns_year)만 부른다 — 재무가 없으면 팩터도 없다."""
    fin = pd.read_parquet(FIN_PATH, columns=["stock_code", "corp_code", "bsns_year", "reprt_code"])
    annual = fin[(fin["reprt_code"] == ANNUAL_REPORT) & fin["bsns_year"].between(year_from, year_to)]
    return annual[["corp_code", "stock_code", "bsns_year"]].drop_duplicates().sort_values(["bsns_year", "stock_code"])


def dart_cache_path(corp_code: str, year: int) -> Path:
    return DART_CACHE_DIR / f"{corp_code}_{year}_{ANNUAL_REPORT}.json"


def fetch_dart_one(api_key: str, corp_code: str, year: int) -> list:
    """회사 하나·연도 하나. 013은 빈 목록, 020 등은 RateLimitStop."""
    response = requests.get(DART_URL, params={"crtfc_key": api_key, "corp_code": corp_code,
                                              "bsns_year": str(year), "reprt_code": ANNUAL_REPORT}, timeout=30)
    body = response.json()
    status = str(body.get("status", ""))

    if status in DART_STATUS_STOP:
        raise RateLimitStop(f"status {status} ({DART_STATUS_STOP[status]})")

    if status == DART_STATUS_NO_DATA:
        return []

    if status != DART_STATUS_OK:
        raise RuntimeError(f"status {status}: {body.get('message', '')}")

    return list(body.get("list", []))


def run_dart(year_from: int, year_to: int, sleep_seconds: float, limit: int) -> None:
    api_key = load_key("dart")
    DART_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    jobs = dart_jobs(year_from, year_to)
    pending = [row for row in jobs.itertuples(index=False) if not dart_cache_path(row.corp_code, row.bsns_year).exists()]

    if limit > 0:
        pending = pending[:limit]

    log(f"DART 주식수: 작업 {len(jobs):,}건 중 미수집 {len(pending):,}건 ({year_from}~{year_to}), sleep={sleep_seconds}")
    fetched = 0
    failed = 0

    for position, row in enumerate(pending, start=1):
        try:
            rows = fetch_dart_one(api_key, row.corp_code, row.bsns_year)
        except RateLimitStop as stop:
            log(f"중단: {stop} — {position - 1}건 받은 뒤. 내일 같은 명령으로 이어서 받는다")
            break
        except Exception as error:  # noqa: BLE001
            failed += 1
            append_failed(DART_CACHE_DIR, f"{row.corp_code}\t{row.bsns_year}\t{type(error).__name__}: {error}")
            time.sleep(sleep_seconds)
            continue

        payload = {"corp_code": row.corp_code, "stock_code": row.stock_code, "bsns_year": int(row.bsns_year),
                   "fetched_at": datetime.datetime.now().isoformat(timespec="seconds"), "list": rows}
        dart_cache_path(row.corp_code, row.bsns_year).write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
        fetched += 1

        if fetched % 200 == 0:
            log(f"  {fetched:,}/{len(pending):,} 받음, 실패 {failed}")

        time.sleep(sleep_seconds)

    log(f"DART 끝: 받음 {fetched:,}, 실패 {failed}")


def classify_share_kind(label: str) -> str:
    text = str(label).replace(" ", "")

    if "합계" in text:
        return "total"

    if "우선" in text:
        return "preferred"

    if "보통" in text:
        return "common"

    return "other"


def build_dart_rows() -> pd.DataFrame:
    """캐시 json → 보고서 하나에 한 행. 보통주 행이 없으면 합계를 보통주로 쓴다(우선주 없는 회사)."""
    records = []

    for path in sorted(DART_CACHE_DIR.glob("*_11011.json")):
        payload = json.loads(path.read_text(encoding="utf-8"))
        by_report: dict = {}

        for item in payload["list"]:
            kind = classify_share_kind(item.get("se", ""))

            if kind == "other":
                continue

            bucket = by_report.setdefault(str(item.get("rcept_no", "")), {})
            bucket[kind] = (parse_count(item.get("istc_totqy")), parse_count(item.get("tesstk_co")))

        for rcept_no, kinds in by_report.items():
            common = kinds.get("common", kinds.get("total", (float("nan"), float("nan"))))
            preferred = kinds.get("preferred", (float("nan"), float("nan")))

            if not rcept_no or len(rcept_no) < 8 or pd.isna(common[0]) or common[0] <= 0:
                continue

            published = pd.Timestamp(rcept_no[:8])
            records.append({"ticker": str(payload["stock_code"]).zfill(6), "published_at": published,
                            "effective_date": published, "shares_common_issued": common[0],
                            "shares_preferred_issued": preferred[0], "treasury_common": common[1],
                            "rcept_no": rcept_no, "source": "dart", "grade": "B"})

    return pd.DataFrame(records, columns=OUTPUT_COLUMNS)


# ── data.go.kr ─────────────────────────────────────────────────────────────────

def month_end_trading_days(year_from: int) -> list:
    dates = pd.read_parquet(BARS_PATH, columns=["Date"])["Date"].drop_duplicates().sort_values()
    dates = dates[dates >= pd.Timestamp(year=year_from, month=1, day=1)]
    month_key = dates.dt.to_period("M")
    return list(dates.groupby(month_key.values).max())


def datagokr_cache_path(day: pd.Timestamp) -> Path:
    return DATAGOKR_CACHE_DIR / f"univ_{day:%Y%m%d}.parquet"


def fetch_datagokr_day(service_key: str, day: pd.Timestamp, sleep_seconds: float) -> pd.DataFrame:
    items = []
    page = 1

    while True:
        response = requests.get(DATAGOKR_URL, params={"serviceKey": service_key, "resultType": "json", "numOfRows": 1000,
                                                      "pageNo": page, "basDt": f"{day:%Y%m%d}"}, timeout=30)
        body = response.json()["response"]["body"]
        raw = body.get("items", {}).get("item", []) or []

        if isinstance(raw, dict):
            raw = [raw]

        items.extend(raw)
        total = int(body.get("totalCount", 0) or 0)

        if not raw or len(items) >= total or len(raw) < 1000:
            break

        page += 1
        time.sleep(sleep_seconds)

    frame = pd.DataFrame([{"ticker": str(item.get("srtnCd", "")).replace("A", "").zfill(6),
                           "shares_listed": parse_count(item.get("lstgStCnt")),
                           "market_cap": parse_count(item.get("mrktTotAmt")),
                           "close": parse_count(item.get("clpr")), "market": str(item.get("mrktCtg", ""))} for item in items])
    frame["date"] = day
    return frame


def run_datagokr(year_from: int, sleep_seconds: float, limit: int) -> None:
    service_key = unquote(load_key("datagokr"))
    DATAGOKR_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    days = [day for day in month_end_trading_days(year_from) if not datagokr_cache_path(day).exists()]

    if limit > 0:
        days = days[:limit]

    log(f"data.go.kr 주식수: 월말 {len(days)}일 미수집 ({year_from}~)")

    for day in days:
        try:
            frame = fetch_datagokr_day(service_key, day, sleep_seconds)
        except Exception as error:  # noqa: BLE001
            append_failed(DATAGOKR_CACHE_DIR, f"{day:%Y%m%d}\t{type(error).__name__}: {error}")
            log(f"  {day:%Y-%m-%d} 실패 {type(error).__name__}")
            time.sleep(sleep_seconds)
            continue

        if len(frame) < DATAGOKR_MIN_ROWS:
            append_failed(DATAGOKR_CACHE_DIR, f"{day:%Y%m%d}\t행 {len(frame)} < {DATAGOKR_MIN_ROWS}")
            log(f"  {day:%Y-%m-%d} {len(frame)}행 — 부분 응답으로 보고 버림")
            continue

        frame.to_parquet(datagokr_cache_path(day), index=False)
        log(f"  {day:%Y-%m-%d} {len(frame):,}행")
        time.sleep(sleep_seconds)


def build_datagokr_rows() -> pd.DataFrame:
    frames = [pd.read_parquet(path) for path in sorted(DATAGOKR_CACHE_DIR.glob("univ_*.parquet"))]

    if not frames:
        return pd.DataFrame(columns=OUTPUT_COLUMNS)

    snapshot = pd.concat(frames, ignore_index=True)
    snapshot = snapshot[snapshot["shares_listed"] > 0]
    return pd.DataFrame({"ticker": snapshot["ticker"], "published_at": snapshot["date"], "effective_date": snapshot["date"],
                         "shares_common_issued": snapshot["shares_listed"], "shares_preferred_issued": float("nan"),
                         "treasury_common": float("nan"), "rcept_no": "", "source": "datagokr", "grade": "A"},
                        columns=OUTPUT_COLUMNS)


def build_output() -> pd.DataFrame:
    parts = [frame for frame in (build_dart_rows(), build_datagokr_rows()) if len(frame) > 0]
    combined = pd.concat(parts, ignore_index=True) if parts else pd.DataFrame(columns=OUTPUT_COLUMNS)
    combined = combined.sort_values(["ticker", "effective_date", "published_at"]).reset_index(drop=True)
    combined.to_parquet(OUTPUT_PATH, index=False)
    by_source = combined.groupby("source").agg(rows=("ticker", "size"), tickers=("ticker", "nunique"),
                                               first=("effective_date", "min"), last=("effective_date", "max"))
    log(f"저장 {OUTPUT_PATH.relative_to(REPO_ROOT)} {len(combined):,}행\n{by_source}")
    return combined


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", choices=["dart", "datagokr"], default=None)
    parser.add_argument("--from", dest="year_from", type=int, default=2015)
    parser.add_argument("--to", dest="year_to", type=int, default=2019)
    parser.add_argument("--sleep", type=float, default=0.1)
    parser.add_argument("--limit", type=int, default=0, help="검증용, 앞에서 N건만")
    parser.add_argument("--build", action="store_true", help="캐시에서 출력 parquet만 다시 만든다")
    arguments = parser.parse_args()

    if arguments.source == "dart":
        run_dart(arguments.year_from, arguments.year_to, arguments.sleep, arguments.limit)

    if arguments.source == "datagokr":
        run_datagokr(arguments.year_from, arguments.sleep, arguments.limit)

    build_output()
    return 0


if __name__ == "__main__":
    sys.exit(main())
