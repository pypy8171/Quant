"""네이버 모바일 수급 이력(외인·기관·개인 순매수 주식수, 외인보유율) 백필.

출처: https://m.stock.naver.com/api/stock/{code}/trend?bizdate=YYYYMMDD&pageSize=60
응답은 bizdate **앞**(배타) 60행 — bizdate=20260625를 주면 첫 행이 20260624다.
마지막 행 날짜를 그대로 다음 bizdate로 넘겨 --since까지 감는다(하루 빼면 페이지 경계마다 하루씩 빠진다).
빈 배열이 오면 그 종목 이력의 끝이다(005930은 2009-09까지, 2005 커서는 빈 배열).

응답 키(2026-09-19 005930 실호출로 확인): itemCode, bizdate, foreignerPureBuyQuant,
foreignerHoldRatio, organPureBuyQuant, individualPureBuyQuant, closePrice,
compareToPreviousClosePrice, accumulatedTradingVolume. 미정산 행은 값이 "-"다.

종목 목록은 PYQuant/data/bars_all_pit.parquet의 code 유니크. 종목별 결과를
PYQuant/data/cache/investor_flow/<code>.parquet에 두고 마지막에 합친다 — 재실행하면 받은 종목은 건너뛴다.
실패 종목은 같은 폴더 _failed.txt에 적고 계속 간다.

실행(저장소 루트에서):
  py PYQuant/tools/naver_flow_backfill.py --since 2019-01-01 --workers 4 --sleep 0.15
  py PYQuant/tools/naver_flow_backfill.py --limit 30          # 검증용
"""

import argparse
import datetime as dt
import json
import math
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[2]
V1_BARS = REPO_ROOT / "PYQuant" / "data" / "bars_all_pit.parquet"
CACHE_DIR = REPO_ROOT / "PYQuant" / "data" / "cache" / "investor_flow"
DEFAULT_OUT = REPO_ROOT / "PYQuant" / "data" / "investor_flow_pit.parquet"

HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/128.0 Safari/537.36"),
    "Referer": "https://m.stock.naver.com/",
}
PAGE_SIZE = 60
BACKOFF_SECONDS = (1, 4, 16)

OUTPUT_COLUMNS = ["date", "ticker", "foreign_net_quantity", "institution_net_quantity",
                  "individual_net_quantity", "foreign_hold_pct", "close"]

print_lock = threading.Lock()


def log(message: str) -> None:
    with print_lock:
        print(time.strftime("%H:%M:%S"), message, flush=True)


def parse_number(text) -> float:
    """"+2,746,972" → 2746972.0, "46.48%" → 46.48, "-"·None → nan."""
    if text is None:
        return math.nan

    cleaned = str(text).replace(",", "").replace("+", "").replace("%", "").strip()

    if cleaned in ("", "-"):
        return math.nan

    try:
        return float(cleaned)

    except ValueError:
        return math.nan


def fetch_page(code: str, bizdate: str, sleep_seconds: float) -> list:
    """한 페이지(최대 60행). 429·5xx는 1·4·16초 backoff 3회 뒤 예외."""
    url = f"https://m.stock.naver.com/api/stock/{code}/trend?bizdate={bizdate}&pageSize={PAGE_SIZE}"
    last_error = None

    for attempt in range(len(BACKOFF_SECONDS) + 1):
        try:
            request = urllib.request.Request(url, headers=HEADERS)

            with urllib.request.urlopen(request, timeout=15) as response:
                body = response.read().decode("utf-8")

            time.sleep(sleep_seconds)
            return json.loads(body)

        except urllib.error.HTTPError as error:
            last_error = error

            if error.code != 429 and error.code < 500:
                raise

        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            last_error = error

        if attempt < len(BACKOFF_SECONDS):
            time.sleep(BACKOFF_SECONDS[attempt])

    raise RuntimeError(f"{code} {bizdate}: {last_error}")


def fetch_ticker(code: str, since: dt.date, sleep_seconds: float) -> pd.DataFrame:
    """종목 하나를 오늘부터 since까지 커서로 감아 표로."""
    cursor = dt.date.today()
    rows = []

    while cursor >= since:
        page = fetch_page(code, cursor.strftime("%Y%m%d"), sleep_seconds)

        if not page:
            break

        rows.extend(page)
        last_bizdate = dt.datetime.strptime(page[-1]["bizdate"], "%Y%m%d").date()

        if last_bizdate >= cursor:
            break   # 커서가 안 움직이면 무한 루프 방지

        cursor = last_bizdate   # 배타 경계라 그대로 넘긴다

    if not rows:
        return pd.DataFrame(columns=OUTPUT_COLUMNS)

    frame = pd.DataFrame({
        "date": pd.to_datetime([row["bizdate"] for row in rows], format="%Y%m%d"),
        "ticker": code,
        "foreign_net_quantity": [parse_number(row.get("foreignerPureBuyQuant")) for row in rows],
        "institution_net_quantity": [parse_number(row.get("organPureBuyQuant")) for row in rows],
        "individual_net_quantity": [parse_number(row.get("individualPureBuyQuant")) for row in rows],
        "foreign_hold_pct": [parse_number(row.get("foreignerHoldRatio")) for row in rows],
        "close": [parse_number(row.get("closePrice")) for row in rows],
    })
    frame = frame[frame["date"] >= pd.Timestamp(since)]
    frame = frame.drop_duplicates("date").sort_values("date")

    # 순매수 주식수가 비면(미정산 행) int64로 못 넣으니 그 행은 뺀다. 종가·보유율 결측은 남긴다.
    quantity_columns = ["foreign_net_quantity", "institution_net_quantity", "individual_net_quantity"]
    frame = frame.dropna(subset=quantity_columns)

    for column in quantity_columns:
        frame[column] = frame[column].astype("int64")

    return frame[OUTPUT_COLUMNS].reset_index(drop=True)


def load_codes(limit: int) -> list:
    codes = sorted(pd.read_parquet(V1_BARS, columns=["code"])["code"].unique().tolist())

    if limit > 0:
        selected = codes[:limit]

        if "005930" in codes and "005930" not in selected:
            selected.append("005930")   # 검증용 대조 종목은 항상 포함

        codes = selected

    return codes


def run(codes: list, since: dt.date, workers: int, sleep_seconds: float) -> tuple:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    failed_path = CACHE_DIR / "_failed.txt"
    pending = [code for code in codes if not (CACHE_DIR / f"{code}.parquet").exists()]
    log(f"종목 {len(codes)}개 중 미수집 {len(pending)}개, workers={workers}, sleep={sleep_seconds}")
    done = 0
    failed = 0
    empty = 0

    def work(code: str) -> tuple:
        frame = fetch_ticker(code, since, sleep_seconds)
        frame.to_parquet(CACHE_DIR / f"{code}.parquet", index=False)
        return code, len(frame)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(work, code): code for code in pending}

        for future in as_completed(futures):
            code = futures[future]
            done += 1

            try:
                _, row_count = future.result()

                if row_count == 0:
                    empty += 1

            except Exception as error:   # 한 종목 실패는 기록만 하고 계속
                failed += 1

                with print_lock, open(failed_path, "a", encoding="utf-8") as handle:
                    handle.write(f"{code}\t{error}\n")

            if done % 100 == 0 or done == len(pending):
                log(f"진행 {done}/{len(pending)} 실패 {failed} 빈종목 {empty}")

    return done, failed, empty


def merge(codes: list, out_path: Path) -> pd.DataFrame:
    frames = []

    for code in codes:
        path = CACHE_DIR / f"{code}.parquet"

        if path.exists():
            frames.append(pd.read_parquet(path))

    if not frames:
        return pd.DataFrame(columns=OUTPUT_COLUMNS)

    merged = pd.concat(frames, ignore_index=True)
    merged = merged.sort_values(["ticker", "date"]).reset_index(drop=True)
    merged["date"] = pd.to_datetime(merged["date"])
    merged["ticker"] = merged["ticker"].astype(str)

    for column in ["foreign_net_quantity", "institution_net_quantity", "individual_net_quantity"]:
        merged[column] = merged[column].astype("int64")

    for column in ["foreign_hold_pct", "close"]:
        merged[column] = merged[column].astype("float64")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    merged.to_parquet(out_path, index=False)
    return merged


def report(merged: pd.DataFrame, codes: list, since: dt.date) -> None:
    """검증 수치: 행·기간·종목 커버·결측률, 005930 최근 5일(네이버 모바일 페이지와 눈으로 대조용)."""
    print("=== 결과 ===")
    print(f"행 {len(merged):,}  종목 {merged['ticker'].nunique()}/{len(codes)}")

    if merged.empty:
        return

    print(f"기간 {merged['date'].min().date()} ~ {merged['date'].max().date()} (since {since})")

    # 결측: v1 일봉 거래일 대비 수급 행이 없는 비율(종목·날짜 기준)
    v1 = pd.read_parquet(V1_BARS, columns=["Date", "code"])
    v1 = v1[(v1["Date"] >= pd.Timestamp(since)) & (v1["code"].isin(merged["ticker"].unique()))]
    v1_keys = set(zip(v1["code"], v1["Date"]))
    flow_keys = set(zip(merged["ticker"], merged["date"]))
    missing = len(v1_keys - flow_keys)
    missing_ratio = missing / max(len(v1_keys), 1)
    # 1% ≈ 종목당 연 2~3일. 거래정지·상장 첫 주처럼 원래 숫자가 없는 날은 그 안에 들어오고, 그보다 크면 받는 쪽 실패로 본다
    #  (09-19 전량 실측 0.48%). 오너 결정 09-20: 2%→1%.
    verdict = "통과" if missing_ratio <= 0.01 else "실패"
    print(f"v1 일봉 (종목,날짜) {len(v1_keys):,} 중 수급 없는 조합 {missing:,} = {missing_ratio:.2%} (판정 기준 ≤1%: {verdict})")

    for column in ["foreign_hold_pct", "close"]:
        print(f"{column} NaN {merged[column].isna().mean():.2%}")

    samsung = merged[merged["ticker"] == "005930"].tail(5)

    if not samsung.empty:
        print("005930 최근 5일 (https://m.stock.naver.com/domestic/stock/005930/trend 와 대조):")
        print(samsung.to_string(index=False))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--since", default="2019-01-01")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--sleep", type=float, default=0.15)
    parser.add_argument("--limit", type=int, default=0, help="검증용, 앞에서 N종목만")
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    arguments = parser.parse_args()

    since = dt.date.fromisoformat(arguments.since)
    codes = load_codes(arguments.limit)
    started = time.time()
    run(codes, since, arguments.workers, arguments.sleep)
    merged = merge(codes, Path(arguments.out))
    report(merged, codes, since)
    log(f"완료 {time.time() - started:.0f}초, 출력 {arguments.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
