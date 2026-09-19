"""DART 전 상장사 재무 주요계정 과거분 채우기 — 발효일(rcept_no 앞 8자리)이 붙은 시점 고정 표.

출처(모두 https://opendart.fss.or.kr/api/):
  corpCode.xml?crtfc_key=…                       zip 안 CORPCODE.xml. 항목 키 corp_code, corp_name,
                                                 corp_eng_name, stock_code, modify_date. 2026-09-19 실호출 119,391건 중
                                                 stock_code가 있는 것 3,990건.
  fnlttMultiAcnt.json?crtfc_key=…&corp_code=<최대 100개 콤마>&bsns_year=YYYY&reprt_code=<11013|11012|11014|11011>
                                                 다중회사 주요계정. 응답 status/message/list. 행 키(2026-09-19 005930 실호출):
                                                 rcept_no, reprt_code, bsns_year, corp_code, stock_code, fs_div(CFS·OFS),
                                                 fs_nm, sj_div(BS·IS), sj_nm, account_nm, thstrm_nm, thstrm_dt,
                                                 thstrm_amount, thstrm_add_amount(IS 분기 누계, BS·연간엔 없음),
                                                 frmtrm_nm, frmtrm_dt, frmtrm_amount, bfefrmtrm_nm, bfefrmtrm_dt,
                                                 bfefrmtrm_amount(연간만), ord, currency.
                                                 계정 14개: 매출액·영업이익·당기순이익(손실)·법인세차감전 순이익·총포괄손익·
                                                 자산총계·부채총계·자본총계·유동자산·비유동자산·유동부채·비유동부채·자본금·이익잉여금.
                                                 이 엔드포인트에는 rcept_dt가 없다 — rcept_no 앞 8자리가 접수일(YYYYMMDD)이라
                                                 그것을 effective_date로 쓴다.
  상태코드: 000 정상, 013 조회된 데이타가 없습니다(빈 결과로 캐시), 020 요청 제한 초과(즉시 중단),
            010·011·012 키 문제, 800 점검 중. 일일 한도 20,000건.

키는 _private/keys.json의 "dart"(gitignore). 키 값은 어디에도 찍지 않는다.

호출 수: 연도 12(2015~2026) × 보고서 4 × 묶음 40(3,990종목/100) = 1,920건 — 한도의 1/10.
캐시는 PYQuant/data/cache/dart_fin/<year>_<reprt>_<batch>.parquet(013이면 빈 파일), 재실행하면 있는 묶음은 건너뛴다.
묶음의 종목 구성은 같은 폴더 _batches.json에 적어 두고, 상장사 목록이 바뀌어 구성이 달라진 묶음만 다시 받는다.
실패 묶음은 _failed.txt에 적고 계속 간다.

출력:
  PYQuant/data/fin/corp_code.parquet          corp_code, corp_name, stock_code, modify_date (stock_code 있는 것만)
  PYQuant/data/fin/dart_fs_raw.parquet        응답 행 그대로 + fetched_at (append-only, 이번 실행에 새로 받은 묶음만 덧붙임)
  PYQuant/data/fin/fin_point_in_time.parquet  한 행 = (stock_code, bsns_year, reprt_code, rcept_no, fs_div, effective_date)
                                              + 계정별 열(revenue, operating_income, … , 아래 ACCOUNT_COLUMNS).
                                              같은 (종목,연도,보고서)에 정정공시가 여럿이면 rcept_no마다 별도 행.
                                              CFS가 있으면 CFS, 없으면 OFS 한 벌만.
                                              손익 계정은 <열>(당기 구간 값)과 <열>_ytd(누계, 연간은 당기와 같음) 두 열.
등급: 발효일이 있어 시점 고정은 되지만 corpCode.xml은 현재 목록이라 오래전 상장폐지 종목은 일부 빠진다(PIT-B).

실행(저장소 루트에서):
  py PYQuant/tools/dart_fin_history_fill.py --from 2024 --to 2024 --limit-batches 3   # 검증용
  py PYQuant/tools/dart_fin_history_fill.py --from 2015 --to 2026 --sleep 0.2         # 전량
  py PYQuant/tools/dart_fin_history_fill.py --rebuild-clean                            # 정리본만 다시
  py PYQuant/tools/dart_fin_history_fill.py --rebuild-raw                              # 캐시에서 원본 parquet 다시
"""

import argparse
import datetime as dt
import io
import json
import math
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[2]
KEYS_PATH = REPO_ROOT / "_private" / "keys.json"
FIN_DIR = REPO_ROOT / "PYQuant" / "data" / "fin"
CACHE_DIR = REPO_ROOT / "PYQuant" / "data" / "cache" / "dart_fin"
CORP_CODE_PATH = FIN_DIR / "corp_code.parquet"
RAW_PATH = FIN_DIR / "dart_fs_raw.parquet"
CLEAN_PATH = FIN_DIR / "fin_point_in_time.parquet"
BATCH_MANIFEST_PATH = CACHE_DIR / "_batches.json"
FAILED_PATH = CACHE_DIR / "_failed.txt"

API_BASE = "https://opendart.fss.or.kr/api/"
BATCH_SIZE = 100
REPORT_CODES = ("11011", "11013", "11012", "11014")   # 사업보고서를 먼저, 그다음 1분기·반기·3분기
STATUS_OK = "000"
STATUS_NO_DATA = "013"
STATUS_STOP = {"020": "일일 한도 초과", "800": "시스템 점검", "010": "미등록 키", "011": "만료된 키", "012": "접근 불가 IP"}
BACKOFF_SECONDS = (1, 4, 16)
CORP_CODE_MAX_AGE_DAYS = 7

RAW_COLUMNS = ["rcept_no", "reprt_code", "bsns_year", "corp_code", "stock_code", "fs_div", "fs_nm", "sj_div", "sj_nm",
               "account_nm", "thstrm_nm", "thstrm_dt", "thstrm_amount", "thstrm_add_amount", "frmtrm_nm", "frmtrm_dt",
               "frmtrm_amount", "frmtrm_add_amount", "bfefrmtrm_nm", "bfefrmtrm_dt", "bfefrmtrm_amount", "ord", "currency",
               "fetched_at"]

# account_nm을 공백·괄호 꼬리를 뗀 뒤 맞춘다. 순서는 정리본 열 순서.
ACCOUNT_COLUMNS = {
    "매출액": "revenue",
    "영업이익": "operating_income",
    "당기순이익": "net_income",
    "법인세차감전순이익": "pretax_income",
    "총포괄손익": "comprehensive_income",
    "자산총계": "total_assets",
    "부채총계": "total_liabilities",
    "자본총계": "total_equity",
    "유동자산": "current_assets",
    "비유동자산": "noncurrent_assets",
    "유동부채": "current_liabilities",
    "비유동부채": "noncurrent_liabilities",
    "자본금": "capital_stock",
    "이익잉여금": "retained_earnings",
}
INCOME_STATEMENT_COLUMNS = ("revenue", "operating_income", "net_income", "pretax_income", "comprehensive_income")
CLEAN_KEY_COLUMNS = ["stock_code", "corp_code", "bsns_year", "reprt_code", "rcept_no", "fs_div", "effective_date",
                     "period_end"]


class RateLimitStop(Exception):
    """020 등 계속 가면 안 되는 상태코드. 어디까지 했는지 메시지에 담는다."""


def log(message: str) -> None:
    print(time.strftime("%H:%M:%S"), message, flush=True)


def load_api_key() -> str:
    with open(KEYS_PATH, encoding="utf-8") as handle:
        return json.load(handle)["dart"]


def parse_amount(text) -> float:
    """"227,062,266,000,000" → 2.27e14, "-1,234" → -1234.0, ""·"-"·None → nan."""
    if text is None:
        return math.nan

    cleaned = str(text).replace(",", "").strip()

    if cleaned in ("", "-"):
        return math.nan

    try:
        return float(cleaned)

    except ValueError:
        return math.nan


def normalize_account_name(name: str) -> str:
    """"당기순이익(손실)" → "당기순이익", "법인세차감전 순이익" → "법인세차감전순이익"."""
    cleaned = re.sub(r"\s+", "", str(name))
    cleaned = re.sub(r"\([^)]*\)$", "", cleaned)
    return cleaned


def http_get(url: str, timeout: int = 60) -> bytes:
    """429·5xx·네트워크 오류는 1·4·16초 backoff 3회 뒤 예외."""
    last_error = None

    for attempt in range(len(BACKOFF_SECONDS) + 1):
        try:
            with urllib.request.urlopen(url, timeout=timeout) as response:
                return response.read()

        except urllib.error.HTTPError as error:
            last_error = error

            if error.code != 429 and error.code < 500:
                raise

        except (urllib.error.URLError, TimeoutError) as error:
            last_error = error

        if attempt < len(BACKOFF_SECONDS):
            time.sleep(BACKOFF_SECONDS[attempt])

    raise RuntimeError(f"HTTP 실패: {last_error}")


def fetch_corp_codes(api_key: str) -> pd.DataFrame:
    url = API_BASE + "corpCode.xml?" + urllib.parse.urlencode({"crtfc_key": api_key})
    archive = zipfile.ZipFile(io.BytesIO(http_get(url)))
    root = ElementTree.fromstring(archive.read(archive.namelist()[0]))
    records = []

    for item in root:
        stock_code = (item.findtext("stock_code") or "").strip()

        if not stock_code:
            continue

        records.append({
            "corp_code": (item.findtext("corp_code") or "").strip(),
            "corp_name": (item.findtext("corp_name") or "").strip(),
            "stock_code": stock_code.zfill(6),
            "modify_date": (item.findtext("modify_date") or "").strip(),
        })

    frame = pd.DataFrame(records).drop_duplicates("corp_code").sort_values("stock_code").reset_index(drop=True)
    return frame


def load_corp_codes(api_key: str, refresh: bool) -> pd.DataFrame:
    """7일 안에 받은 파일이 있으면 그것을 쓴다(묶음 구성이 흔들리지 않게)."""
    if CORP_CODE_PATH.exists() and not refresh:
        age_days = (time.time() - CORP_CODE_PATH.stat().st_mtime) / 86400

        if age_days < CORP_CODE_MAX_AGE_DAYS:
            return pd.read_parquet(CORP_CODE_PATH)

    frame = fetch_corp_codes(api_key)
    FIN_DIR.mkdir(parents=True, exist_ok=True)
    frame.to_parquet(CORP_CODE_PATH, index=False)
    log(f"corp_code 갱신: 상장사 {len(frame):,}건 → {CORP_CODE_PATH.relative_to(REPO_ROOT)}")
    return frame


def make_batches(corp_codes: list) -> list:
    return [corp_codes[start:start + BATCH_SIZE] for start in range(0, len(corp_codes), BATCH_SIZE)]


def load_batch_manifest() -> dict:
    if BATCH_MANIFEST_PATH.exists():
        with open(BATCH_MANIFEST_PATH, encoding="utf-8") as handle:
            return json.load(handle)

    return {}


def save_batch_manifest(manifest: dict) -> None:
    with open(BATCH_MANIFEST_PATH, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False)


def cache_path(year: int, report_code: str, batch_index: int) -> Path:
    return CACHE_DIR / f"{year}_{report_code}_{batch_index:03d}.parquet"


def fetch_batch(api_key: str, corp_codes: list, year: int, report_code: str) -> pd.DataFrame:
    """묶음 하나(≤100종목) 호출. 013은 빈 표, 020 등은 RateLimitStop, 그 밖의 오류 코드는 RuntimeError."""
    query = urllib.parse.urlencode({
        "crtfc_key": api_key,
        "corp_code": ",".join(corp_codes),
        "bsns_year": str(year),
        "reprt_code": report_code,
    })
    body = json.loads(http_get(API_BASE + "fnlttMultiAcnt.json?" + query).decode("utf-8"))
    status = str(body.get("status", ""))

    if status in STATUS_STOP:
        raise RateLimitStop(f"status {status} ({STATUS_STOP[status]}): {body.get('message', '')}")

    if status == STATUS_NO_DATA:
        return pd.DataFrame(columns=RAW_COLUMNS)

    if status != STATUS_OK:
        raise RuntimeError(f"status {status}: {body.get('message', '')}")

    frame = pd.DataFrame(body.get("list", []))

    for column in RAW_COLUMNS:
        if column not in frame.columns:
            frame[column] = None

    frame["fetched_at"] = pd.Timestamp.now(tz="Asia/Seoul").tz_localize(None)
    frame = frame[RAW_COLUMNS].astype({column: "string" for column in RAW_COLUMNS if column != "fetched_at"})
    return frame


def append_raw(frames: list) -> int:
    """이번 실행에서 새로 받은 묶음만 원본 parquet 뒤에 덧붙인다(append-only)."""
    frames = [frame for frame in frames if not frame.empty]

    if not frames:
        return 0

    new_rows = pd.concat(frames, ignore_index=True)

    if RAW_PATH.exists():
        existing = pd.read_parquet(RAW_PATH)
        combined = pd.concat([existing, new_rows], ignore_index=True)

    else:
        combined = new_rows

    FIN_DIR.mkdir(parents=True, exist_ok=True)
    combined.to_parquet(RAW_PATH, index=False)
    return len(new_rows)


def rebuild_raw_from_cache() -> int:
    frames = [pd.read_parquet(path) for path in sorted(CACHE_DIR.glob("*.parquet"))]
    frames = [frame for frame in frames if not frame.empty]

    if not frames:
        return 0

    combined = pd.concat(frames, ignore_index=True)
    FIN_DIR.mkdir(parents=True, exist_ok=True)
    combined.to_parquet(RAW_PATH, index=False)
    return len(combined)


def run_fetch(api_key: str, corp_frame: pd.DataFrame, year_from: int, year_to: int, sleep_seconds: float,
              limit_batches: int) -> tuple:
    """(받은 묶음 수, 건너뛴 묶음 수, 실패 묶음 수, 새로 받은 원본 행 수). 020이면 그때까지 받은 것을 저장한 뒤 예외."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    batches = make_batches(corp_frame["corp_code"].tolist())
    manifest = load_batch_manifest()
    jobs = [(year, report_code, batch_index)
            for year in range(year_from, year_to + 1)
            for report_code in REPORT_CODES
            for batch_index in range(len(batches))]

    if limit_batches > 0:
        jobs = jobs[:limit_batches]

    log(f"상장사 {len(corp_frame):,} → 묶음 {len(batches)}개, 작업 {len(jobs)}건({year_from}~{year_to}), sleep={sleep_seconds}")
    fetched = 0
    skipped = 0
    failed = 0
    new_frames = []
    last_job = None

    try:
        for job_index, (year, report_code, batch_index) in enumerate(jobs, start=1):
            last_job = (year, report_code, batch_index)
            batch_key = str(batch_index)
            batch_codes = batches[batch_index]
            path = cache_path(year, report_code, batch_index)
            composition_changed = manifest.get(batch_key) not in (None, batch_codes)

            if path.exists() and not composition_changed:
                skipped += 1
                continue

            try:
                frame = fetch_batch(api_key, batch_codes, year, report_code)

            except RateLimitStop:
                raise

            except Exception as error:   # 한 묶음 실패는 기록만 하고 계속
                failed += 1

                with open(FAILED_PATH, "a", encoding="utf-8") as handle:
                    handle.write(f"{year}\t{report_code}\t{batch_index}\t{error}\n")

                time.sleep(sleep_seconds)
                continue

            frame.to_parquet(path, index=False)
            manifest[batch_key] = batch_codes
            new_frames.append(frame)
            fetched += 1
            time.sleep(sleep_seconds)

            if job_index % 100 == 0 or job_index == len(jobs):
                log(f"진행 {job_index}/{len(jobs)} 받음 {fetched} 건너뜀 {skipped} 실패 {failed}")

    except RateLimitStop as error:
        log(f"중단: {error} — 마지막 작업 {last_job}, 받음 {fetched} 건너뜀 {skipped} 실패 {failed}")
        save_batch_manifest(manifest)
        new_rows = append_raw(new_frames)
        raise RuntimeError(f"한도 중단, 원본에 {new_rows:,}행 덧붙임. 같은 명령으로 재실행하면 이어서 받는다.") from error

    save_batch_manifest(manifest)
    new_rows = append_raw(new_frames)
    return fetched, skipped, failed, new_rows


def build_clean(raw: pd.DataFrame) -> pd.DataFrame:
    """원본(길게) → 시점 고정 정리본(넓게). rcept_no마다 한 행, CFS 우선."""
    if raw.empty:
        return pd.DataFrame(columns=CLEAN_KEY_COLUMNS + list(ACCOUNT_COLUMNS.values()))

    frame = raw.copy()
    frame["stock_code"] = frame["stock_code"].astype(str).str.strip().str.zfill(6)
    frame["account_key"] = frame["account_nm"].map(normalize_account_name)
    frame["column"] = frame["account_key"].map(ACCOUNT_COLUMNS)
    # 표준 14계정 밖의 이름은 버리지 않고 account_<이름> 열로 남긴다
    unmapped = frame["column"].isna()
    frame.loc[unmapped, "column"] = "account_" + frame.loc[unmapped, "account_key"]

    frame["amount"] = frame["thstrm_amount"].map(parse_amount)
    frame["amount_ytd"] = frame["thstrm_add_amount"].map(parse_amount)
    # 연간 보고서엔 누계 열이 없다 — 당기 값이 곧 누계
    annual = frame["reprt_code"].astype(str) == "11011"
    frame.loc[annual & frame["amount_ytd"].isna(), "amount_ytd"] = frame.loc[annual & frame["amount_ytd"].isna(), "amount"]

    # CFS 우선: rcept_no마다 CFS 행이 하나라도 있으면 OFS는 버린다
    has_consolidated = frame["fs_div"].eq("CFS").groupby(frame["rcept_no"]).transform("any")
    frame = frame[(frame["fs_div"] == "CFS") | ~has_consolidated]

    key_columns = ["stock_code", "corp_code", "bsns_year", "reprt_code", "rcept_no", "fs_div"]
    frame = frame.drop_duplicates(key_columns + ["column"], keep="last")
    wide = frame.pivot(index=key_columns, columns="column", values="amount")
    ytd_source = frame[frame["column"].isin(INCOME_STATEMENT_COLUMNS)]
    wide_ytd = ytd_source.pivot(index=key_columns, columns="column", values="amount_ytd")
    wide_ytd.columns = [f"{column}_ytd" for column in wide_ytd.columns]
    wide = wide.join(wide_ytd, how="left").reset_index()

    # 기간 끝(thstrm_dt "2024.12.31 현재" 또는 "2024.01.01 ~ 2024.12.31")은 rcept_no마다 하나
    period_end = (frame.groupby(key_columns)["thstrm_dt"]
                  .agg(lambda values: max(re.findall(r"\d{4}\.\d{2}\.\d{2}", " ".join(values.dropna().astype(str))) or [""]))
                  .rename("period_end").reset_index())
    wide = wide.merge(period_end, on=key_columns, how="left")
    wide["period_end"] = pd.to_datetime(wide["period_end"], format="%Y.%m.%d", errors="coerce")
    wide["effective_date"] = pd.to_datetime(wide["rcept_no"].astype(str).str[:8], format="%Y%m%d", errors="coerce")
    wide["bsns_year"] = wide["bsns_year"].astype(int)
    wide["reprt_code"] = wide["reprt_code"].astype(str)

    ordered_columns = list(ACCOUNT_COLUMNS.values())
    ordered_columns += [f"{column}_ytd" for column in INCOME_STATEMENT_COLUMNS]
    extra_columns = sorted(column for column in wide.columns if column.startswith("account_"))

    for column in ordered_columns:
        if column not in wide.columns:
            wide[column] = math.nan

    wide = wide[CLEAN_KEY_COLUMNS + ordered_columns + extra_columns]
    wide = wide.sort_values(["stock_code", "effective_date", "bsns_year", "reprt_code"]).reset_index(drop=True)
    return wide


def rebuild_clean() -> pd.DataFrame:
    raw = pd.read_parquet(RAW_PATH) if RAW_PATH.exists() else pd.DataFrame(columns=RAW_COLUMNS)
    clean = build_clean(raw)
    FIN_DIR.mkdir(parents=True, exist_ok=True)
    clean.to_parquet(CLEAN_PATH, index=False)
    return clean


def report(clean: pd.DataFrame) -> None:
    """검증 수치: 원본·정리본 행 수, 005930 사업보고서 매출액·영업이익(공시값과 눈으로 대조용)."""
    raw_rows = len(pd.read_parquet(RAW_PATH, columns=["rcept_no"])) if RAW_PATH.exists() else 0
    print("=== 결과 ===")
    print(f"원본 {raw_rows:,}행  정리본 {len(clean):,}행  종목 {clean['stock_code'].nunique() if not clean.empty else 0}")

    if clean.empty:
        return

    print(f"effective_date {clean['effective_date'].min().date()} ~ {clean['effective_date'].max().date()}")
    samsung = clean[(clean["stock_code"] == "005930") & (clean["reprt_code"] == "11011")]

    if not samsung.empty:
        print("005930 사업보고서 (단위 억원, https://dart.fss.or.kr 사업보고서 연결재무제표와 대조):")
        view = samsung[["bsns_year", "rcept_no", "fs_div", "effective_date", "revenue", "operating_income", "net_income"]].copy()

        for column in ["revenue", "operating_income", "net_income"]:
            view[column] = (view[column] / 1e8).round(0)

        print(view.to_string(index=False))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="year_from", type=int, default=2015)
    parser.add_argument("--to", dest="year_to", type=int, default=dt.date.today().year)
    parser.add_argument("--sleep", type=float, default=0.2)
    parser.add_argument("--limit-batches", type=int, default=0, help="검증용, 앞에서 N묶음만")
    parser.add_argument("--refresh-corp-codes", action="store_true", help="corp_code.parquet를 7일 안이어도 다시 받는다")
    parser.add_argument("--rebuild-raw", action="store_true", help="캐시 폴더에서 원본 parquet를 다시 만든다(호출 없음)")
    parser.add_argument("--rebuild-clean", action="store_true", help="원본 parquet에서 정리본만 다시 만든다(호출 없음)")
    arguments = parser.parse_args()
    started = time.time()

    if arguments.rebuild_raw:
        log(f"원본 재구성 {rebuild_raw_from_cache():,}행 → {RAW_PATH.relative_to(REPO_ROOT)}")

    if not (arguments.rebuild_raw or arguments.rebuild_clean):
        api_key = load_api_key()
        corp_frame = load_corp_codes(api_key, arguments.refresh_corp_codes)

        try:
            fetched, skipped, failed, new_rows = run_fetch(api_key, corp_frame, arguments.year_from, arguments.year_to,
                                                           arguments.sleep, arguments.limit_batches)

        except RuntimeError as error:
            log(str(error))
            rebuild_clean()
            return 2

        log(f"받음 {fetched} 건너뜀 {skipped} 실패 {failed}, 원본에 {new_rows:,}행 덧붙임")

    clean = rebuild_clean()
    report(clean)
    log(f"완료 {time.time() - started:.0f}초, 정리본 {CLEAN_PATH.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
