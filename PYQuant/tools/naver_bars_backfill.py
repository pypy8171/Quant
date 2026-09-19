"""네이버 일봉(siseJson) 1990~ 전 기간 백필 → PYQuant/data/bars_all_pit_v2.parquet.

출처: https://api.finance.naver.com/siseJson.naver?symbol={code}&requestType=1&startTime=19900101
      &endTime=YYYYMMDD&timeframe=day — 종목당 1콜에 전 기간이 온다.
응답은 JSON이 아니라 파이썬 리터럴에 가깝다(작은따옴표, 외국인소진율이 빈 행은 끝에 쉼표가 남음)
→ ast.literal_eval로 읽는다. 헤더(2026-09-19 실호출): 날짜, 시가, 고가, 저가, 종가, 거래량, 외국인소진율.

알아둘 것(005930 실측):
- 가격은 수정주가(2018-05 액면분할 앞뒤가 이어짐), 거래량은 미수정.
- 거래정지일은 시·고·저·거래량이 0이고 종가만 남는다.
- 1990-01~02 두 달은 미수정 값이라 1990-03-02에 44,800→421 점프가 있다. 그 구간은 쓰지 않는 편이 맞다.

종목 목록·name·market·delisted는 v1(PYQuant/data/bars_all_pit.parquet)에서 그대로 가져온다.
출력 컬럼·dtype은 v1과 같고 foreign_hold_pct(float64)를 더한다. v1은 건드리지 않는다.
종목별 결과를 PYQuant/data/cache/bars_v2/<code>.parquet에 두고 마지막에 합친다(재실행 시 건너뜀).
끝에 v1과 겹치는 구간의 005930 종가 일치율을 검사해 출력한다.

실행(저장소 루트에서):
  py PYQuant/tools/naver_bars_backfill.py --workers 4 --sleep 0.15
  py PYQuant/tools/naver_bars_backfill.py --limit 30          # 검증용
"""

import argparse
import ast
import datetime as dt
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
CACHE_DIR = REPO_ROOT / "PYQuant" / "data" / "cache" / "bars_v2"
DEFAULT_OUT = REPO_ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"

HEADERS = {"User-Agent": "Mozilla/5.0", "Referer": "https://finance.naver.com/"}
BACKOFF_SECONDS = (1, 4, 16)
START_DATE = "19900101"

# v1 컬럼 그대로 + foreign_hold_pct
V1_COLUMNS = ["Date", "Open", "High", "Low", "Close", "Volume", "code", "name", "market", "delisted"]
OUTPUT_COLUMNS = V1_COLUMNS + ["foreign_hold_pct"]
KOREAN_TO_ENGLISH = {"날짜": "Date", "시가": "Open", "고가": "High", "저가": "Low",
                     "종가": "Close", "거래량": "Volume", "외국인소진율": "foreign_hold_pct"}

print_lock = threading.Lock()


def log(message: str) -> None:
    with print_lock:
        print(time.strftime("%H:%M:%S"), message, flush=True)


def fetch_text(code: str, end_date: str, sleep_seconds: float) -> str:
    """전 기간 원문. 429·5xx는 1·4·16초 backoff 3회 뒤 예외."""
    url = (f"https://api.finance.naver.com/siseJson.naver?symbol={code}&requestType=1"
           f"&startTime={START_DATE}&endTime={end_date}&timeframe=day")
    last_error = None

    for attempt in range(len(BACKOFF_SECONDS) + 1):
        try:
            request = urllib.request.Request(url, headers=HEADERS)

            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read().decode("utf-8").strip()

            time.sleep(sleep_seconds)
            return body

        except urllib.error.HTTPError as error:
            last_error = error

            if error.code != 429 and error.code < 500:
                raise

        except (urllib.error.URLError, TimeoutError) as error:
            last_error = error

        if attempt < len(BACKOFF_SECONDS):
            time.sleep(BACKOFF_SECONDS[attempt])

    raise RuntimeError(f"{code}: {last_error}")


def parse_bars(text: str) -> pd.DataFrame:
    """siseJson 원문 → Date..Volume, foreign_hold_pct 표. 행이 없으면 빈 표."""
    rows = ast.literal_eval(text)

    if len(rows) < 2:
        return pd.DataFrame(columns=["Date", "Open", "High", "Low", "Close", "Volume", "foreign_hold_pct"])

    header = [KOREAN_TO_ENGLISH.get(name, name) for name in rows[0]]
    records = []

    for row in rows[1:]:
        values = list(row)[:len(header)]
        values += [None] * (len(header) - len(values))   # 외국인소진율이 빈 행
        records.append(values)

    frame = pd.DataFrame(records, columns=header)
    frame["Date"] = pd.to_datetime(frame["Date"].astype(str), format="%Y%m%d")

    for column in ["Open", "High", "Low", "Close", "Volume"]:
        frame[column] = pd.to_numeric(frame[column], errors="coerce")

    if "foreign_hold_pct" not in frame.columns:
        frame["foreign_hold_pct"] = float("nan")

    frame["foreign_hold_pct"] = pd.to_numeric(frame["foreign_hold_pct"], errors="coerce").astype("float64")
    frame = frame.dropna(subset=["Open", "High", "Low", "Close", "Volume"])

    for column in ["Open", "High", "Low", "Close", "Volume"]:
        frame[column] = frame[column].astype("int64")

    frame = frame.drop_duplicates("Date").sort_values("Date").reset_index(drop=True)
    return frame[["Date", "Open", "High", "Low", "Close", "Volume", "foreign_hold_pct"]]


def load_ticker_meta(limit: int) -> pd.DataFrame:
    """v1에서 종목별 name·market·delisted 한 줄씩(마지막 행 기준)."""
    v1 = pd.read_parquet(V1_BARS, columns=["Date", "code", "name", "market", "delisted"])
    meta = v1.sort_values("Date").groupby("code").last().reset_index().sort_values("code")

    if limit > 0:
        selected = meta.head(limit)
        samsung = meta[meta["code"] == "005930"]   # 검증용 대조 종목은 항상 포함
        meta = pd.concat([selected, samsung]).drop_duplicates("code")

    return meta[["code", "name", "market", "delisted"]].reset_index(drop=True)


def run(meta: pd.DataFrame, workers: int, sleep_seconds: float) -> tuple:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    failed_path = CACHE_DIR / "_failed.txt"
    end_date = dt.date.today().strftime("%Y%m%d")
    pending = [code for code in meta["code"] if not (CACHE_DIR / f"{code}.parquet").exists()]
    log(f"종목 {len(meta)}개 중 미수집 {len(pending)}개, workers={workers}, sleep={sleep_seconds}")
    done = 0
    failed = 0
    empty = 0

    def work(code: str) -> tuple:
        frame = parse_bars(fetch_text(code, end_date, sleep_seconds))
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


def merge(meta: pd.DataFrame, out_path: Path) -> pd.DataFrame:
    frames = []

    for row in meta.itertuples(index=False):
        path = CACHE_DIR / f"{row.code}.parquet"

        if not path.exists():
            continue

        frame = pd.read_parquet(path)

        if frame.empty:
            continue

        frame["code"] = row.code
        frame["name"] = row.name
        frame["market"] = row.market
        frame["delisted"] = bool(row.delisted)
        frames.append(frame)

    if not frames:
        return pd.DataFrame(columns=OUTPUT_COLUMNS)

    merged = pd.concat(frames, ignore_index=True)[OUTPUT_COLUMNS]
    merged = merged.sort_values(["code", "Date"]).reset_index(drop=True)
    merged["delisted"] = merged["delisted"].astype(bool)

    for column in ["code", "name", "market"]:
        merged[column] = merged[column].astype(object)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    merged.to_parquet(out_path, index=False)
    return merged


def compare_with_v1(merged: pd.DataFrame, meta: pd.DataFrame) -> None:
    """v1과 겹치는 구간 005930 종가 일치율. 불일치면 원인 행 5개."""
    print("=== 결과 ===")
    print(f"행 {len(merged):,}  종목 {merged['code'].nunique() if not merged.empty else 0}/{len(meta)}")

    if merged.empty:
        return

    print(f"기간 {merged['Date'].min().date()} ~ {merged['Date'].max().date()}")
    print(f"foreign_hold_pct NaN {merged['foreign_hold_pct'].isna().mean():.2%}")
    delisted_covered = merged.loc[merged['delisted'], 'code'].nunique()
    delisted_total = int(meta['delisted'].sum())
    print(f"상폐 종목 커버 {delisted_covered}/{delisted_total}"
          + (f" = {delisted_covered / delisted_total:.1%}" if delisted_total else ""))

    v1 = pd.read_parquet(V1_BARS, columns=["Date", "Close", "code"])
    v1 = v1[v1["code"] == "005930"].set_index("Date")["Close"]
    v2 = merged[merged["code"] == "005930"].set_index("Date")["Close"]

    if v1.empty or v2.empty:
        print("005930 비교 생략(한쪽에 없음)")
        return

    overlap_start = max(v1.index.min(), v2.index.min())
    overlap_end = min(v1.index.max(), v2.index.max())
    v1_overlap = v1[(v1.index >= overlap_start) & (v1.index <= overlap_end)]
    joined = pd.DataFrame({"v1": v1_overlap}).join(pd.DataFrame({"v2": v2}), how="left")
    matched = (joined["v1"] == joined["v2"]).sum()
    print(f"005930 종가 겹침 구간 {overlap_start.date()}~{overlap_end.date()} v1 {len(joined)}행 중 "
          f"일치 {matched} = {matched / len(joined):.2%} (판정 기준 100%)")
    mismatch = joined[joined["v1"] != joined["v2"]]

    if not mismatch.empty:
        print("불일치 행(앞 5개):")
        print(mismatch.head(5).to_string())

    # v1 컬럼 dtype 동일성
    v1_types = pd.read_parquet(V1_BARS).dtypes

    for column in V1_COLUMNS:
        if str(v1_types[column]) != str(merged[column].dtype):
            print(f"dtype 다름 {column}: v1 {v1_types[column]} v2 {merged[column].dtype}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--sleep", type=float, default=0.15)
    parser.add_argument("--limit", type=int, default=0, help="검증용, 앞에서 N종목만")
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    arguments = parser.parse_args()

    meta = load_ticker_meta(arguments.limit)
    started = time.time()
    run(meta, arguments.workers, arguments.sleep)
    merged = merge(meta, Path(arguments.out))
    compare_with_v1(merged, meta)
    log(f"완료 {time.time() - started:.0f}초, 출력 {arguments.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
