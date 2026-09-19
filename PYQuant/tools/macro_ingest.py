"""거시 시계열(FRED·ECOS·관세청) 시점 고정 적재.

세 출처의 시계열을 한 스키마로 받아 PYQuant/data/macro/<source>_<series>.parquet 에 쌓는다.
append-only — 같은 (series_id, observation_date, published_at) 행은 먼저 적재된 것을 남기고 새 행만 붙인다.
한 시리즈가 실패하면 그 파일은 손대지 않고(마지막 성공분 유지) 다음 시리즈로 간다.

공통 스키마
  observation_date  관측 기간의 시작일(월간은 1일, 분기는 분기 첫날, 관세청 10일 잠정치는 구간 끝날)
  published_at      발표·발효 시각. 모르면 NaT. 시각까지는 모르니 날짜 00:00 이다 — KST 백테스트에서는
                    published_at 다음 날부터 보이는 것으로 조인한다(미국 발표는 KST 밤·새벽에 나온다).
  value             float
  source            "fred" | "ecos" | "tradedata"
  series_id         FRED 시리즈 id, ECOS는 아래 표의 이름, 관세청은 export_10d 같은 이름
  fetched_at        이 스크립트가 받은 시각(UTC)
  grade             "A" 발효일 있는 이력 / "B" 발표일 근사·재작성 가능 / "C" 현재 스냅샷의 과거 투영

등급은 research/COUNCIL_CHARTER.md 2절을 따른다.

FRED (등급 A — ALFRED 판본 이력. T10Y2Y·VIXCLS의 ALFRED 이전 구간은 현재값으로 채우고 그 행만 등급 B)
  https://api.stlouisfed.org/fred/series/observations
    ?series_id=CPIAUCSL&api_key=…&file_type=json&observation_start=2005-01-01
    &realtime_start=1776-07-04&realtime_end=9999-12-31&limit=100000&offset=0
  realtime_start를 ALFRED 최초일(1776-07-04)로 주면 각 관측치의 모든 판본이 오고, 판본의 realtime_start가
  그 값이 처음 공개된 날이다 → published_at. 값 "."은 결측(휴장)이라 버린다.
  응답 키(2026-09-19 CPIAUCSL 실호출): count, offset, limit, observations[{realtime_start, realtime_end, date, value}].
  같은 관측치가 판본마다 한 행씩 온다(CPIAUCSL 2024-01-01 → 2024-02-13·2025-02-12·2026-02-13 세 판본).
  기존 PYQuant/tools/macro_regime_feed.py는 FinanceDataReader의 FRED 경유(판본 없음·키 없음)라 겹치지 않는다.

ECOS 한국은행 (등급 B — 판본 없음, 월간 발표일은 공표일정을 안 받아 NaT)
  https://ecos.bok.or.kr/api/StatisticSearch/{key}/json/kr/{start_row}/{end_row}/{stat}/{cycle}/{start}/{end}/{item}
  응답 키(2026-09-19 722Y001 실호출): StatisticSearch.list_total_count, StatisticSearch.row[{STAT_CODE, STAT_NAME,
  ITEM_CODE1, ITEM_NAME1, …, UNIT_NAME, TIME, DATA_VALUE}]. 오류는 RESULT.CODE/RESULT.MESSAGE 로 온다.
  TIME 표기: D=YYYYMMDD, M=YYYYMM, Q=YYYYQn, A=YYYY. 일간(금리·환율)은 그날 공개되니 published_at=observation_date.
  통계표·항목 코드(2026-09-19 StatisticTableList·StatisticItemList 실호출로 확인):
    base_rate       722Y001 D 0101000    한국은행 기준금리 (1999-05-06~)
    ktb_3y          817Y002 D 010200000  국고채(3년) (1998-11-13~)
    ktb_10y         817Y002 D 010210000  국고채(10년) (2000-12-18~)
    usdkrw          731Y001 D 0000001    원/미국달러(매매기준율) (1964-05-04~)
    cpi             901Y009 M 0          소비자물가지수 총지수 2020=100 (1965-01~)
    exports         901Y118 M T002       수출금액(통관, 천불) (2000-01~)   ※ 301Y013은 표 목록에 없다
    imports         901Y118 M T004       수입금액(통관, 천불) (2000-01~)
    m2              161Y006 M BBHA00     M2 평잔 원계열(십억원) (2003-10~) ※ 101Y004는 2004-09에서 끝난 옛 표
    leading_index   901Y067 M I16A       선행종합지수 2020=100 (1970-01~)
    leading_cycle   901Y067 M I16E       선행지수순환변동치
    coincident_cycle 901Y067 M I16D      동행지수순환변동치

관세청 수출입무역통계 data.go.kr (등급 B — 10·20일 잠정치, 월확정은 다음 달 1일 발표)
  https://apis.data.go.kr/1220000/prlstMmUtPrviExpAcrs/getPrlstMmUtPrviExpAcrs?serviceKey=…&strtYymm=202607&endYymm=202609
  https://apis.data.go.kr/1220000/prlstMmUtPrviImpAcrs/getPrlstMmUtPrviImpAcrs?…  (수입)
  응답은 XML: response.header.resultCode("00"), body.items.item[{priodMon(YYYYMM), priodDt("YYYYMM.01~10" 꼴),
  itemUsdAmt00(총액, 천 USD), itemUsdAmt01(반도체) …}]. 같은 달에 1~10·1~20·월전체 행이 함께 오므로 구간별로
  시리즈를 나눈다(export_10d·export_20d·export_month, import_*). published_at은 관세청 발표 관행(11일·21일·다음 달 1일).
  키는 data.go.kr 일반 인증키(Encoding)이며 서비스마다 활용신청이 따로 필요하다. 미승인이면 HTTP 403
  SERVICE_KEY_IS_NOT_REGISTERED_ERROR 가 오고 이 스크립트는 그 사실만 로그에 남긴다.
    수출 주요품목별 10일 잠정치  https://www.data.go.kr/data/15157908/openapi.do
    수입 주요품목별 10일 잠정치  https://www.data.go.kr/data/15157901/openapi.do
    (참고) 수출 주요국가별 15157941, 수입 주요국가별 15157909, 품목별 수출입실적 15101609, 수출입총괄 15102108

키: _private/keys.json 의 "fred"·"ecos"·"datagokr". 키 값은 어디에도 찍지 않는다.

실행(저장소 루트에서):
  py PYQuant/tools/macro_ingest.py --source all --since 2005-01-01
  py PYQuant/tools/macro_ingest.py --source fred --since 2020-01-01
"""

import argparse
import datetime as dt
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[2]
KEYS_PATH = REPO_ROOT / "_private" / "keys.json"
DEFAULT_OUTPUT_DIRECTORY = REPO_ROOT / "PYQuant" / "data" / "macro"

OUTPUT_COLUMNS = ["observation_date", "published_at", "value", "source", "series_id", "fetched_at", "grade"]
DEDUP_KEYS = ["series_id", "observation_date", "published_at"]
BACKOFF_SECONDS = (1, 4, 16)
ALFRED_FIRST_DAY = "1776-07-04"
ALFRED_LAST_DAY = "9999-12-31"
FRED_PAGE_LIMIT = 100000
ECOS_PAGE_ROWS = 10000

FRED_SERIES = ["DGS10", "DGS2", "T10Y2Y", "FEDFUNDS", "CPIAUCSL", "PPIACO", "UNRATE", "GDPC1",
               "VIXCLS", "DTWEXBGS", "DEXKOUS", "BAMLH0A0HYM2", "ICSA", "UMCSENT"]

# 이름 → (통계표, 주기, 항목코드). 코드 근거는 모듈 독스트링.
ECOS_SERIES = {
    "base_rate": ("722Y001", "D", "0101000"),
    "ktb_3y": ("817Y002", "D", "010200000"),
    "ktb_10y": ("817Y002", "D", "010210000"),
    "usdkrw": ("731Y001", "D", "0000001"),
    "cpi": ("901Y009", "M", "0"),
    "exports": ("901Y118", "M", "T002"),
    "imports": ("901Y118", "M", "T004"),
    "m2": ("161Y006", "M", "BBHA00"),
    "leading_index": ("901Y067", "M", "I16A"),
    "leading_cycle": ("901Y067", "M", "I16E"),
    "coincident_cycle": ("901Y067", "M", "I16D"),
}

TRADEDATA_ENDPOINTS = {
    "export": "https://apis.data.go.kr/1220000/prlstMmUtPrviExpAcrs/getPrlstMmUtPrviExpAcrs",
    "import": "https://apis.data.go.kr/1220000/prlstMmUtPrviImpAcrs/getPrlstMmUtPrviImpAcrs",
}
TRADEDATA_APPLY_URLS = {
    "export": "https://www.data.go.kr/data/15157908/openapi.do",
    "import": "https://www.data.go.kr/data/15157901/openapi.do",
}


def log(message: str) -> None:
    print(time.strftime("%H:%M:%S"), message, flush=True)


def load_keys() -> dict:
    with open(KEYS_PATH, encoding="utf-8") as handle:
        return json.load(handle)


def http_get(url: str, timeout: float = 60.0) -> bytes:
    """GET 본문. 5xx·네트워크 오류는 세 번 물러섰다 다시 시도하고, 4xx는 그대로 올린다(키·코드 오류라 재시도 무의미)."""
    last_error: Exception | None = None

    for wait_seconds in (*BACKOFF_SECONDS, None):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "quant-macro-ingest/1.0"})

            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()

        except urllib.error.HTTPError as error:
            if 400 <= error.code < 500:
                body = error.read().decode("utf-8", "replace")
                raise RuntimeError(f"HTTP {error.code} {summarize_error_body(body)}") from None

            last_error = error

        except (urllib.error.URLError, TimeoutError, OSError) as error:
            last_error = error

        if wait_seconds is None:
            break

        time.sleep(wait_seconds)

    raise RuntimeError(f"요청 실패: {type(last_error).__name__}: {str(last_error)[:120]}")


def summarize_error_body(body: str) -> str:
    """data.go.kr 오류 XML의 errMsg·returnAuthMsg 만 뽑는다. 다른 본문은 앞 120자."""
    try:
        root = ElementTree.fromstring(body)
        pieces = [root.findtext(".//errMsg") or "", root.findtext(".//returnAuthMsg") or ""]
        joined = " ".join(piece for piece in pieces if piece)

        if joined:
            return joined

    except ElementTree.ParseError:
        pass

    return body[:120].replace("\n", " ")


# ── FRED ─────────────────────────────────────────────────────────────────────

def fetch_fred_vintages(series_id: str, api_key: str, since: str, realtime_start: str | None,
                        realtime_end: str | None) -> list:
    """한 실시간 구간의 판본 행 [(date, realtime_start, value)]. 페이지는 offset으로 넘긴다.
    realtime 구간을 None으로 주면 FRED 현재값(오늘 판본)만 온다 — 서버가 미국 날짜라 '오늘'을 직접 넣지 않는다."""
    rows = []
    offset = 0

    while True:
        parameters = {
            "series_id": series_id, "api_key": api_key, "file_type": "json",
            "observation_start": since, "limit": FRED_PAGE_LIMIT, "offset": offset,
        }

        if realtime_start is not None:
            parameters["realtime_start"] = realtime_start
            parameters["realtime_end"] = realtime_end

        query = urllib.parse.urlencode(parameters)
        payload = json.loads(http_get(f"https://api.stlouisfed.org/fred/series/observations?{query}"))
        observations = payload.get("observations", [])

        for observation in observations:
            if observation["value"] == ".":
                continue

            rows.append((observation["date"], observation["realtime_start"], float(observation["value"])))

        offset += len(observations)

        if not observations or offset >= int(payload.get("count", 0)):
            break

    return rows


def fetch_fred_series(series_id: str, api_key: str, since: str) -> pd.DataFrame:
    """ALFRED 판본 전체 — 관측치 한 개가 판본 수만큼 행이 되고 published_at=realtime_start, 등급 A.

    일간 시리즈(DGS10·VIXCLS 등)는 판본 날짜가 2,000개를 넘어 한 번에 못 받는다(HTTP 400 "There are N vintage
    dates"). 그때는 실시간 구간을 1년씩 끊는다. 구간 앞에서 이미 공개돼 있던 관측치는 realtime_start가 구간
    시작일로 잘려 오므로, 첫 구간을 뺀 나머지에서 realtime_start==구간 시작일인 행은 버린다(진짜 발표가 1월 1일에
    있을 일은 없다).

    ALFRED 이력이 없는 구간(VIXCLS·T10Y2Y는 늦게 ALFRED에 들어와 앞 연도가 HTTP 400 "does not exist in ALFRED")은
    FRED 현재값으로 채운다 — 그날 공개되는 시장 지표라 published_at=observation_date 로 두되 판본이 없으니 그
    행만 등급 B. 프레임에 grade 열이 같이 온다.
    """
    alfred_rows = []
    alfred_gap = False

    try:
        alfred_rows = fetch_fred_vintages(series_id, api_key, since, ALFRED_FIRST_DAY, ALFRED_LAST_DAY)

    except RuntimeError as error:
        if "does not exist in ALFRED" in str(error):
            alfred_gap = True

        elif "vintage dates" in str(error):
            first_year = dt.date.fromisoformat(since).year
            last_year = dt.date.today().year

            for year in range(first_year, last_year + 1):
                window_start = f"{year}-01-01"
                window_end = ALFRED_LAST_DAY if year == last_year else f"{year}-12-31"

                try:
                    window_rows = fetch_fred_vintages(series_id, api_key, since, window_start, window_end)

                except RuntimeError as window_error:
                    if "does not exist in ALFRED" not in str(window_error):
                        raise

                    alfred_gap = True
                    continue

                if year != first_year:
                    window_rows = [row for row in window_rows if row[1] != window_start]

                alfred_rows.extend(window_rows)

        else:
            raise

    frame = pd.DataFrame(alfred_rows, columns=["observation_date", "published_at", "value"])
    frame["grade"] = "A"

    if alfred_gap:
        current_rows = fetch_fred_vintages(series_id, api_key, since, None, None)
        covered = set(frame["observation_date"])
        filler = pd.DataFrame([(row[0], row[0], row[2]) for row in current_rows if row[0] not in covered],
                              columns=["observation_date", "published_at", "value"])
        filler["grade"] = "B"
        frame = pd.concat([frame, filler], ignore_index=True)

    frame["observation_date"] = pd.to_datetime(frame["observation_date"])
    frame["published_at"] = pd.to_datetime(frame["published_at"])
    return frame


# ── ECOS ─────────────────────────────────────────────────────────────────────

def ecos_time_bounds(cycle: str, since: str) -> tuple[str, str]:
    """주기별 TIME 표기로 시작·끝. 끝은 오늘."""
    start = dt.date.fromisoformat(since)
    today = dt.date.today()

    if cycle == "D":
        return start.strftime("%Y%m%d"), today.strftime("%Y%m%d")

    if cycle == "M":
        return start.strftime("%Y%m"), today.strftime("%Y%m")

    if cycle == "Q":
        return f"{start.year}Q{(start.month - 1) // 3 + 1}", f"{today.year}Q{(today.month - 1) // 3 + 1}"

    return str(start.year), str(today.year)


def parse_ecos_time(text: str, cycle: str) -> pd.Timestamp:
    """TIME → 기간 시작일."""
    if cycle == "D":
        return pd.Timestamp(dt.datetime.strptime(text, "%Y%m%d"))

    if cycle == "M":
        return pd.Timestamp(dt.datetime.strptime(text, "%Y%m"))

    if cycle == "Q":
        year, quarter = text.split("Q")
        return pd.Timestamp(year=int(year), month=(int(quarter) - 1) * 3 + 1, day=1)

    return pd.Timestamp(year=int(text), month=1, day=1)


def fetch_ecos_series(stat_code: str, cycle: str, item_code: str, api_key: str, since: str) -> pd.DataFrame:
    start, end = ecos_time_bounds(cycle, since)
    rows = []
    first_row = 1

    while True:
        last_row = first_row + ECOS_PAGE_ROWS - 1
        url = (f"https://ecos.bok.or.kr/api/StatisticSearch/{api_key}/json/kr/{first_row}/{last_row}/"
               f"{stat_code}/{cycle}/{start}/{end}/{item_code}")
        payload = json.loads(http_get(url))

        if "RESULT" in payload:
            result = payload["RESULT"]
            raise RuntimeError(f"ECOS {result.get('CODE')} {result.get('MESSAGE')}")

        body = payload.get("StatisticSearch", {})
        page_rows = body.get("row", [])

        for row in page_rows:
            value_text = (row.get("DATA_VALUE") or "").strip()

            if not value_text:
                continue

            rows.append((parse_ecos_time(row["TIME"], cycle), float(value_text.replace(",", ""))))

        total = int(body.get("list_total_count", 0))
        first_row = last_row + 1

        if not page_rows or first_row > total:
            break

    frame = pd.DataFrame(rows, columns=["observation_date", "value"])
    # 일간 시장 지표는 그날 공개된다. 월간·분기는 공표일정을 안 받으니 모른다(NaT).
    frame["published_at"] = frame["observation_date"] if cycle == "D" else pd.NaT
    return frame


# ── 관세청 10일 단위 잠정치 ──────────────────────────────────────────────────

def tradedata_span(period_text: str) -> str:
    compact = period_text.replace(" ", "")

    if compact.endswith("~10"):
        return "10d"

    if compact.endswith("~20"):
        return "20d"

    return "month"


def tradedata_dates(month_text: str, span: str) -> tuple[pd.Timestamp, pd.Timestamp]:
    """(관측 구간 끝날, 발표일). 관세청은 1~10일치를 11일, 1~20일치를 21일, 한 달치를 다음 달 1일에 낸다."""
    month_start = pd.Timestamp(dt.datetime.strptime(month_text, "%Y%m"))

    if span == "10d":
        return month_start + pd.Timedelta(days=9), month_start + pd.Timedelta(days=10)

    if span == "20d":
        return month_start + pd.Timedelta(days=19), month_start + pd.Timedelta(days=20)

    next_month = month_start + pd.offsets.MonthBegin(1)
    return next_month - pd.Timedelta(days=1), next_month


def fetch_tradedata(direction: str, api_key: str, since: str) -> dict[str, pd.DataFrame]:
    """수출 또는 수입 10일 잠정치. 구간(10d·20d·month)별 프레임을 돌려준다. 키는 Encoding 키라 그대로 붙인다.

    관세청은 한 호출에 10년까지만 받으므로 9년 창으로 나눠 부른다(저장은 append-only라 겹쳐도 한 행).
    """
    window_start = dt.date.fromisoformat(since).replace(day=1)
    today = dt.date.today()
    items = []

    while window_start <= today:
        window_end = min(window_start.replace(year=window_start.year + 9), today)
        url = (f"{TRADEDATA_ENDPOINTS[direction]}?serviceKey={api_key}"
               f"&strtYymm={window_start.strftime('%Y%m')}&endYymm={window_end.strftime('%Y%m')}")
        root = ElementTree.fromstring(http_get(url))
        result_code = root.findtext(".//resultCode")

        if result_code != "00":
            raise RuntimeError(f"관세청 resultCode={result_code} {root.findtext('.//resultMsg')}")

        items.extend(root.iter("item"))
        window_start = window_end.replace(day=1) + dt.timedelta(days=32)
        window_start = window_start.replace(day=1)

    rows_by_span: dict[str, list] = {"10d": [], "20d": [], "month": []}

    for item in items:
        month_text = (item.findtext("priodMon") or "").strip()
        total_text = (item.findtext("itemUsdAmt00") or "").replace(",", "").strip()

        if not (month_text and total_text):
            continue

        span = tradedata_span(item.findtext("priodDt") or "")
        observation_date, published_at = tradedata_dates(month_text, span)
        rows_by_span[span].append((observation_date, published_at, float(total_text) * 1000.0))

    return {
        f"{direction}_{span}": pd.DataFrame(rows, columns=["observation_date", "published_at", "value"])
        for span, rows in rows_by_span.items() if rows
    }


# ── 저장 ─────────────────────────────────────────────────────────────────────

def finalize_frame(frame: pd.DataFrame, source: str, series_id: str, grade: str, fetched_at: pd.Timestamp) -> pd.DataFrame:
    frame = frame.copy()
    frame["observation_date"] = pd.to_datetime(frame["observation_date"])
    frame["published_at"] = pd.to_datetime(frame["published_at"])
    frame["value"] = frame["value"].astype("float64")
    frame["source"] = source
    frame["series_id"] = series_id
    frame["fetched_at"] = fetched_at
    if "grade" not in frame.columns:
        frame["grade"] = grade

    return frame[OUTPUT_COLUMNS]


def append_parquet(path: Path, new_frame: pd.DataFrame) -> tuple[int, int]:
    """기존 파일에 새 행만 붙인다. (추가된 행 수, 합친 뒤 총 행 수). 임시 파일에 쓰고 바꿔치기한다."""
    if path.exists():
        existing = pd.read_parquet(path)
        combined = pd.concat([existing, new_frame], ignore_index=True)
    else:
        existing = None
        combined = new_frame

    combined = combined.drop_duplicates(subset=DEDUP_KEYS, keep="first")
    combined = combined.sort_values(["observation_date", "published_at"], na_position="first").reset_index(drop=True)
    added = len(combined) - (0 if existing is None else len(existing))
    temporary_path = path.with_suffix(".parquet.tmp")
    combined.to_parquet(temporary_path, index=False)
    os.replace(temporary_path, path)
    return added, len(combined)


def store_series(output_directory: Path, source: str, series_id: str, frame: pd.DataFrame, grade: str,
                 fetched_at: pd.Timestamp) -> None:
    finalized = finalize_frame(frame, source, series_id, grade, fetched_at)
    path = output_directory / f"{source}_{series_id}.parquet"
    added, total = append_parquet(path, finalized)
    last_observation = finalized["observation_date"].max()
    last_text = "-" if pd.isna(last_observation) else last_observation.strftime("%Y-%m-%d")
    grade_text = "/".join(sorted(finalized["grade"].unique()))
    log(f"{source} {series_id} rows={total} added={added} last={last_text} grade={grade_text}")


# ── 소스별 실행 ──────────────────────────────────────────────────────────────

def run_fred(keys: dict, since: str, output_directory: Path, fetched_at: pd.Timestamp) -> int:
    failures = 0

    for series_id in FRED_SERIES:
        try:
            frame = fetch_fred_series(series_id, keys["fred"], since)
            store_series(output_directory, "fred", series_id, frame, "A", fetched_at)

        except Exception as error:
            failures += 1
            log(f"fred {series_id} 실패 — {error} (기존 파일 유지)")

    return failures


def run_ecos(keys: dict, since: str, output_directory: Path, fetched_at: pd.Timestamp) -> int:
    failures = 0

    for series_id, (stat_code, cycle, item_code) in ECOS_SERIES.items():
        try:
            frame = fetch_ecos_series(stat_code, cycle, item_code, keys["ecos"], since)
            store_series(output_directory, "ecos", series_id, frame, "B", fetched_at)

        except Exception as error:
            failures += 1
            log(f"ecos {series_id} 실패 — {error} (기존 파일 유지)")

    return failures


def run_tradedata(keys: dict, since: str, output_directory: Path, fetched_at: pd.Timestamp) -> int:
    failures = 0

    for direction in TRADEDATA_ENDPOINTS:
        try:
            frames = fetch_tradedata(direction, keys["datagokr"], since)

            for series_id, frame in frames.items():
                store_series(output_directory, "tradedata", series_id, frame, "B", fetched_at)

        except Exception as error:
            failures += 1
            hint = ""

            if "NOT_REGISTERED" in str(error):
                hint = f" — 활용신청 필요 {TRADEDATA_APPLY_URLS[direction]}"

            log(f"tradedata {direction} 실패 — {error}{hint} (기존 파일 유지)")

    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description="거시 시계열 시점 고정 적재")
    parser.add_argument("--source", choices=["fred", "ecos", "tradedata", "all"], default="all")
    parser.add_argument("--since", default="2005-01-01", help="관측 시작일 YYYY-MM-DD")
    parser.add_argument("--output-directory", default=str(DEFAULT_OUTPUT_DIRECTORY))
    arguments = parser.parse_args()

    dt.date.fromisoformat(arguments.since)
    output_directory = Path(arguments.output_directory)
    output_directory.mkdir(parents=True, exist_ok=True)
    keys = load_keys()
    fetched_at = pd.Timestamp.now(tz="UTC").tz_localize(None)
    runners = {"fred": run_fred, "ecos": run_ecos, "tradedata": run_tradedata}
    selected = list(runners) if arguments.source == "all" else [arguments.source]
    failures = 0

    for source in selected:
        if source not in ("fred", "ecos", "tradedata"):
            continue

        missing_key = {"fred": "fred", "ecos": "ecos", "tradedata": "datagokr"}[source]

        if not keys.get(missing_key):
            failures += 1
            log(f"{source} 건너뜀 — _private/keys.json 에 '{missing_key}' 키가 없다")
            continue

        failures += runners[source](keys, arguments.since, output_directory, fetched_at)

    log(f"완료 — 실패 {failures}건, 저장 위치 {output_directory}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
