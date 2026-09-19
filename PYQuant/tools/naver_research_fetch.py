"""네이버 증권 리서치(증권사 리포트) 목록·상세·PDF 본문과 종목 컨센서스 스냅샷을 받아 둔다.

장중 대시보드가 읽을 파일을 만드는 것이 목적이다. 대시보드 파일은 여기서 건드리지 않고
PYQuant/data/research/latest.json 만 갱신한다.

## 목록 (2026-09-20 실호출로 확인)
`https://m.stock.naver.com/api/research/{category}?pageSize=20&page=N` — 열려 있는 category 6개:
  company(종목분석)·industry(산업분석)·market(시황정보)·economy(경제분석)·invest(투자전략)·debenture(채권분석).
  bond·strategy·portfolio·derivative 는 404.
응답은 JSON 배열. 키: researchCategory, category(산업분석은 업종명), researchId, title, brokerName,
  writeDate("YYYY-MM-DD"), readCount, endUrl. company 에만 itemCode·itemName 이 있다.
목록에는 목표가·투자의견이 없다 — 상세에서 받는다. researchId 는 category 마다 따로 매기므로
  파일·중복 제거 키는 "{category}:{researchId}" 다(pdf/<category>_<id>.pdf).

## 상세
`https://m.stock.naver.com/api/research/{category}/{researchId}` → {researchContent, researchSummaries}.
researchContent 키: researchId, title, brokerName, writeDate, readCount, attachUrl(stock.pstatic.net/*.pdf),
  content(HTML 요약). company 는 itemCode, itemName, opinion(매수 등), goalPrice, prevGoalPrice,
  priceAtWriteDate 가 더 있다. `.../researchContent` 하위 경로는 404.

## 컨센서스
`https://m.stock.naver.com/api/stock/{code}/integration`
  consensusInfo: {itemCode, createDate, recommMean("4.00"), priceTargetMean("98,667")}
  totalInfos[]: code 별 {key, value} — lastClosePrice(전일 종가)·marketValue(억원으로 바꿔 저장)·per·eps·cnsPer(추정PER)·cnsEps(추정EPS)·pbr·bps 등.
`https://m.stock.naver.com/api/stock/{code}/finance/annual` (--estimates)
  financeInfo.trTitleList 에서 isConsensus=="Y" 인 결산기 열이 추정치. rowList[].title 은 매출액·영업이익·
  당기순이익·ROE·EPS·PER·BPS·PBR·주당배당금. 단위는 억원(비율은 %).

## 저장
  PYQuant/data/research/list/YYYY-MM-DD.parquet   받은 날짜별 목록+상세 필드, 이미 본 키는 다시 넣지 않는다
  PYQuant/data/research/pdf/<category>_<id>.pdf    --pdf
  PYQuant/data/research/text/<category>_<id>.txt   pdfplumber 로 뽑은 본문(빈 파일이면 이미지 PDF)
  PYQuant/data/research/latest.json                최근 3 작성일 리포트 요약(대시보드 입력)
  PYQuant/data/consensus/YYYY-MM-DD.parquet        종목 컨센서스 스냅샷(그날 파일에 종목별로 덮어쓴다)
  실패는 각 폴더 _failed.txt 에 "키<TAB>사유" 로 적고 계속 간다.

컨센서스 종목은 --codes 가 없으면 PYQuant/data/bars_all_pit.parquet 의 code 중 마지막 거래일 기준 60일 안에
거래가 있는 것(2026-09-20 기준 2,674 종목, 0.2초 간격이면 약 9분).

실행(저장소 루트에서, py 는 PYQuant/.venv-win 을 가리킨다. pdfplumber 가 없으면 `py -m pip install pdfplumber`):
  py PYQuant/tools/naver_research_fetch.py --pages 5 --pdf                 # 목록+상세+PDF+latest.json
  py PYQuant/tools/naver_research_fetch.py --consensus --sleep 0.2         # 컨센서스 스냅샷
  py PYQuant/tools/naver_research_fetch.py --pages 3 --pdf --limit 10      # 검증용
  py PYQuant/tools/naver_research_fetch.py --consensus --codes 005930,000660 --estimates
등급: 리포트는 게시일이 있어 B, 컨센서스는 현재 스냅샷이라 C(적재 시작일 뒤만 유효). 비공식 경로라 형식이 바뀌면 끊긴다.
"""

import argparse
import datetime as dt
import html
import json
import math
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[2]
BARS_PATH = REPO_ROOT / "PYQuant" / "data" / "bars_all_pit.parquet"
RESEARCH_DIR = REPO_ROOT / "PYQuant" / "data" / "research"
LIST_DIR = RESEARCH_DIR / "list"
PDF_DIR = RESEARCH_DIR / "pdf"
TEXT_DIR = RESEARCH_DIR / "text"
LATEST_PATH = RESEARCH_DIR / "latest.json"
CONSENSUS_DIR = REPO_ROOT / "PYQuant" / "data" / "consensus"

BASE_URL = "https://m.stock.naver.com/api"
CATEGORIES = ["company", "industry", "market", "economy", "invest", "debenture"]
HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/128.0 Safari/537.36"),
    "Referer": "https://m.stock.naver.com/",
}
PAGE_SIZE = 20
BACKOFF_SECONDS = (1, 4, 16)
LATEST_WRITE_DATES = 3
TEXT_HEAD_CHARS = 300

LIST_COLUMNS = ["key", "category", "research_id", "research_category", "sub_category", "title", "broker",
                "item_code", "item_name", "write_date", "read_count", "opinion", "goal_price", "prev_goal_price",
                "price_at_write", "pdf_url", "summary", "end_url", "fetched_at"]
CONSENSUS_COLUMNS = ["snapshot_date", "code", "consensus_date", "recommend_mean", "target_price_mean",
                     "last_close", "market_value", "per", "eps", "estimate_per", "estimate_eps", "pbr", "bps",
                     "estimate_period", "estimate_revenue", "estimate_operating_profit", "estimate_net_profit",
                     "estimate_roe", "estimate_eps_annual", "estimate_per_annual", "estimate_bps",
                     "estimate_dividend_per_share", "fetched_at"]
ESTIMATE_ROW_MAP = {
    "매출액": "estimate_revenue",
    "영업이익": "estimate_operating_profit",
    "당기순이익": "estimate_net_profit",
    "ROE": "estimate_roe",
    "EPS": "estimate_eps_annual",
    "PER": "estimate_per_annual",
    "BPS": "estimate_bps",
    "주당배당금": "estimate_dividend_per_share",
}
TOTAL_INFO_MAP = {
    "lastClosePrice": "last_close",
    "marketValue": "market_value",
    "per": "per",
    "eps": "eps",
    "cnsPer": "estimate_per",
    "cnsEps": "estimate_eps",
    "pbr": "pbr",
    "bps": "bps",
}
HTML_TAG = re.compile(r"<[^>]+>")


def log(message: str) -> None:
    print(time.strftime("%H:%M:%S"), message, flush=True)


def parse_number(text) -> float:
    """"98,667" → 98667.0, "14.36배" → 14.36, "4,903원" → 4903.0, "-"·None → nan. 시총 "6조 8,208억" 은 억 단위 float."""
    if text is None:
        return math.nan

    cleaned = str(text).replace(",", "").replace("+", "").strip()

    if cleaned in ("", "-"):
        return math.nan

    if "조" in cleaned or cleaned.endswith("억"):
        trillion_match = re.search(r"(-?\d+)조", cleaned)
        hundred_million_match = re.search(r"(-?\d+)억", cleaned)
        trillion = float(trillion_match.group(1)) if trillion_match else 0.0
        hundred_million = float(hundred_million_match.group(1)) if hundred_million_match else 0.0
        return trillion * 10000 + hundred_million

    cleaned = re.sub(r"[^0-9.\-]", "", cleaned)

    try:
        return float(cleaned)

    except ValueError:
        return math.nan


def strip_html(text) -> str:
    if not text:
        return ""

    plain = HTML_TAG.sub(" ", str(text))
    plain = html.unescape(plain)
    return re.sub(r"\s+", " ", plain).strip()


def request_bytes(url: str, sleep_seconds: float) -> bytes:
    """GET 한 번. 429·5xx·네트워크 오류는 1·4·16초 backoff 3회 뒤 예외, 그 밖의 4xx 는 바로 예외."""
    last_error = None

    for attempt in range(len(BACKOFF_SECONDS) + 1):
        try:
            request = urllib.request.Request(url, headers=HEADERS)

            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read()

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

    raise RuntimeError(f"{url}: {last_error}")


def request_json(url: str, sleep_seconds: float):
    return json.loads(request_bytes(url, sleep_seconds).decode("utf-8"))


def append_failed(folder: Path, key: str, error) -> None:
    folder.mkdir(parents=True, exist_ok=True)

    with open(folder / "_failed.txt", "a", encoding="utf-8") as handle:
        handle.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}\t{key}\t{error}\n")


# ---------------------------------------------------------------- 목록·상세

def load_known_keys() -> set:
    keys = set()

    if not LIST_DIR.exists():
        return keys

    for path in sorted(LIST_DIR.glob("*.parquet")):
        keys.update(pd.read_parquet(path, columns=["key"])["key"].tolist())

    return keys


def fetch_list(category: str, pages: int, sleep_seconds: float) -> list:
    items = []

    for page in range(1, pages + 1):
        url = f"{BASE_URL}/research/{category}?pageSize={PAGE_SIZE}&page={page}"
        page_items = request_json(url, sleep_seconds)

        if not page_items:
            break

        for item in page_items:
            item["_category"] = category

        items.extend(page_items)

    return items


def fetch_detail(category: str, research_id, sleep_seconds: float) -> dict:
    url = f"{BASE_URL}/research/{category}/{research_id}"
    return request_json(url, sleep_seconds).get("researchContent") or {}


def build_row(item: dict, detail: dict, fetched_at: str) -> dict:
    category = item["_category"]
    research_id = int(item["researchId"])
    return {
        "key": f"{category}:{research_id}",
        "category": category,
        "research_id": research_id,
        "research_category": item.get("researchCategory") or "",
        "sub_category": item.get("category") or "",
        "title": item.get("title") or "",
        "broker": item.get("brokerName") or "",
        "item_code": str(item.get("itemCode") or ""),
        "item_name": item.get("itemName") or "",
        "write_date": item.get("writeDate") or "",
        "read_count": parse_number(item.get("readCount")),
        "opinion": detail.get("opinion") or "",
        "goal_price": parse_number(detail.get("goalPrice")),
        "prev_goal_price": parse_number(detail.get("prevGoalPrice")),
        "price_at_write": parse_number(detail.get("priceAtWriteDate")),
        "pdf_url": detail.get("attachUrl") or "",
        "summary": strip_html(detail.get("content")),
        "end_url": item.get("endUrl") or "",
        "fetched_at": fetched_at,
    }


def save_list_rows(rows: list, snapshot_date: str) -> Path:
    LIST_DIR.mkdir(parents=True, exist_ok=True)
    path = LIST_DIR / f"{snapshot_date}.parquet"
    frame = pd.DataFrame(rows, columns=LIST_COLUMNS)

    if path.exists():
        frame = pd.concat([pd.read_parquet(path), frame], ignore_index=True)

    frame = frame.drop_duplicates("key", keep="last").sort_values(["write_date", "category", "research_id"],
                                                                  ascending=[False, True, False])
    frame["research_id"] = frame["research_id"].astype("int64")
    frame.to_parquet(path, index=False)
    return path


def run_research(pages: int, sleep_seconds: float, limit: int, snapshot_date: str) -> pd.DataFrame:
    """목록 → 새 키만 상세 → 그날 parquet 에 append. 돌려주는 값은 이번에 새로 넣은 행."""
    known = load_known_keys()
    fetched_at = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    listed = []

    for category in CATEGORIES:
        try:
            items = fetch_list(category, pages, sleep_seconds)
            listed.extend(items)
            log(f"목록 {category}: {len(items)}건")

        except Exception as error:
            append_failed(RESEARCH_DIR, f"list:{category}", error)
            log(f"목록 {category} 실패: {error}")

    fresh = [item for item in listed if f"{item['_category']}:{item['researchId']}" not in known]
    seen = set()
    unique_fresh = []

    for item in fresh:
        key = f"{item['_category']}:{item['researchId']}"

        if key not in seen:
            seen.add(key)
            unique_fresh.append(item)

    if limit > 0:
        unique_fresh = unique_fresh[:limit]

    log(f"목록 합계 {len(listed)}건, 이미 본 것 제외 {len(unique_fresh)}건 상세 조회")
    rows = []
    failed = 0

    for item in unique_fresh:
        category = item["_category"]
        research_id = item["researchId"]

        try:
            detail = fetch_detail(category, research_id, sleep_seconds)

        except Exception as error:
            failed += 1
            append_failed(RESEARCH_DIR, f"{category}:{research_id}", error)
            detail = {}

        rows.append(build_row(item, detail, fetched_at))

    if rows:
        path = save_list_rows(rows, snapshot_date)
        log(f"목록 저장 {path.relative_to(REPO_ROOT)} (+{len(rows)}행, 상세 실패 {failed})")

    return pd.DataFrame(rows, columns=LIST_COLUMNS)


# ---------------------------------------------------------------- PDF·본문

def extract_pdf_text(pdf_path: Path, max_pages: int) -> str:
    """페이지 하나에 0.1~0.6초 걸린다. max_pages 를 넘는 뒷장은 버리고 마지막 줄에 그 사실을 남긴다."""
    import pdfplumber   # 무거운 모듈이라 --pdf 일 때만 읽는다

    pages_text = []

    with pdfplumber.open(pdf_path) as document:
        total_pages = len(document.pages)

        for page_index, page in enumerate(document.pages):
            if max_pages > 0 and page_index >= max_pages:
                pages_text.append(f"[{total_pages}쪽 중 {max_pages}쪽까지만 뽑음]")
                break

            pages_text.append(page.extract_text() or "")

    return "\n\n".join(pages_text).strip()


def run_pdf(frame: pd.DataFrame, sleep_seconds: float, limit: int, max_pages: int) -> tuple:
    """pdf_url 이 있는 행을 내려받고 본문을 뽑는다. 이미 text 가 있으면 건너뛴다. (내려받기 성공, 본문 추출 성공) 수."""
    PDF_DIR.mkdir(parents=True, exist_ok=True)
    TEXT_DIR.mkdir(parents=True, exist_ok=True)
    targets = frame[frame["pdf_url"].astype(str).str.endswith(".pdf")]
    targets = targets[[not (TEXT_DIR / f"{key.replace(':', '_')}.txt").exists() for key in targets["key"]]]

    if limit > 0:
        targets = targets.head(limit)

    downloaded = 0
    extracted = 0

    for row in targets.itertuples(index=False):
        file_stem = row.key.replace(":", "_")
        pdf_path = PDF_DIR / f"{file_stem}.pdf"
        text_path = TEXT_DIR / f"{file_stem}.txt"

        try:
            if not pdf_path.exists():
                pdf_path.write_bytes(request_bytes(row.pdf_url, sleep_seconds))

            downloaded += 1

        except Exception as error:
            append_failed(PDF_DIR, row.key, error)
            continue

        try:
            text = extract_pdf_text(pdf_path, max_pages)
            text_path.write_text(text, encoding="utf-8")

            if text:
                extracted += 1

        except Exception as error:
            append_failed(TEXT_DIR, row.key, error)

    log(f"PDF 대상 {len(targets)}건: 내려받기 {downloaded}, 본문 추출 {extracted}")
    return downloaded, extracted


# ---------------------------------------------------------------- 대시보드 요약

def load_all_lists() -> pd.DataFrame:
    if not LIST_DIR.exists():
        return pd.DataFrame(columns=LIST_COLUMNS)

    frames = [pd.read_parquet(path) for path in sorted(LIST_DIR.glob("*.parquet"))]

    if not frames:
        return pd.DataFrame(columns=LIST_COLUMNS)

    return pd.concat(frames, ignore_index=True).drop_duplicates("key", keep="last")


def value_or_none(value):
    if value is None:
        return None

    if isinstance(value, float) and math.isnan(value):
        return None

    return value


def write_latest_json() -> Path:
    """최근 3 작성일(주말·공휴일이 있어 달력 3일이 아니라 작성일 3개)의 리포트를 대시보드용으로 쓴다."""
    frame = load_all_lists()
    recent_dates = sorted(frame["write_date"].dropna().unique().tolist(), reverse=True)[:LATEST_WRITE_DATES]
    recent = frame[frame["write_date"].isin(recent_dates)].sort_values(
        ["write_date", "category", "research_id"], ascending=[False, True, False])
    reports = []

    for row in recent.itertuples(index=False):
        file_stem = row.key.replace(":", "_")
        pdf_path = PDF_DIR / f"{file_stem}.pdf"
        text_path = TEXT_DIR / f"{file_stem}.txt"
        text_head = ""

        if text_path.exists():
            text_head = re.sub(r"\s+", " ", text_path.read_text(encoding="utf-8", errors="replace"))[:TEXT_HEAD_CHARS]

        reports.append({
            "key": row.key,
            "category": row.category,
            "research_category": row.research_category,
            "sub_category": row.sub_category,
            "title": row.title,
            "broker": row.broker,
            "item_code": row.item_code or None,
            "item_name": row.item_name or None,
            "write_date": row.write_date,
            "opinion": row.opinion or None,
            "goal_price": value_or_none(row.goal_price),
            "prev_goal_price": value_or_none(row.prev_goal_price),
            "price_at_write": value_or_none(row.price_at_write),
            "pdf_url": row.pdf_url or None,
            "pdf_path": str(pdf_path.relative_to(REPO_ROOT)).replace("\\", "/") if pdf_path.exists() else None,
            "text_path": str(text_path.relative_to(REPO_ROOT)).replace("\\", "/") if text_path.exists() else None,
            "text_head": text_head or (row.summary or "")[:TEXT_HEAD_CHARS],
            "end_url": row.end_url,
        })

    payload = {
        "generated_at": dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "source": f"{BASE_URL}/research/{{category}} ({', '.join(CATEGORIES)})",
        "write_dates": recent_dates,
        "count": len(reports),
        "count_by_category": {category: int((recent["category"] == category).sum()) for category in CATEGORIES},
        "reports": reports,
    }
    RESEARCH_DIR.mkdir(parents=True, exist_ok=True)
    LATEST_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=1), encoding="utf-8")
    return LATEST_PATH


# ---------------------------------------------------------------- 컨센서스

def load_universe_codes() -> list:
    bars = pd.read_parquet(BARS_PATH, columns=["Date", "code"])
    cutoff = bars["Date"].max() - pd.Timedelta(days=60)
    return sorted(bars.loc[bars["Date"] >= cutoff, "code"].unique().tolist())


def fetch_consensus(code: str, sleep_seconds: float, with_estimates: bool, snapshot_date: str,
                    fetched_at: str) -> dict:
    integration = request_json(f"{BASE_URL}/stock/{code}/integration", sleep_seconds)
    consensus = integration.get("consensusInfo") or {}
    row = {column: math.nan for column in CONSENSUS_COLUMNS}
    row.update({
        "snapshot_date": snapshot_date,
        "code": code,
        "consensus_date": consensus.get("createDate") or "",
        "recommend_mean": parse_number(consensus.get("recommMean")),
        "target_price_mean": parse_number(consensus.get("priceTargetMean")),
        "estimate_period": "",
        "fetched_at": fetched_at,
    })

    for info in integration.get("totalInfos") or []:
        column = TOTAL_INFO_MAP.get(info.get("code"))

        if column:
            row[column] = parse_number(info.get("value"))

    if with_estimates:
        finance = request_json(f"{BASE_URL}/stock/{code}/finance/annual", sleep_seconds).get("financeInfo") or {}
        estimate_keys = [title["key"] for title in finance.get("trTitleList") or [] if title.get("isConsensus") == "Y"]

        if estimate_keys:
            period_key = estimate_keys[0]
            row["estimate_period"] = period_key

            for finance_row in finance.get("rowList") or []:
                column = ESTIMATE_ROW_MAP.get(finance_row.get("title"))

                if column:
                    row[column] = parse_number((finance_row.get("columns") or {}).get(period_key, {}).get("value"))

    return row


def run_consensus(codes: list, sleep_seconds: float, limit: int, with_estimates: bool, snapshot_date: str) -> Path:
    CONSENSUS_DIR.mkdir(parents=True, exist_ok=True)
    path = CONSENSUS_DIR / f"{snapshot_date}.parquet"
    fetched_at = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    if limit > 0:
        codes = codes[:limit]

    log(f"컨센서스 {len(codes)}종목, sleep={sleep_seconds}, estimates={with_estimates}")
    rows = []
    failed = 0

    for index, code in enumerate(codes, start=1):
        try:
            rows.append(fetch_consensus(code, sleep_seconds, with_estimates, snapshot_date, fetched_at))

        except Exception as error:
            failed += 1
            append_failed(CONSENSUS_DIR, code, error)

        if index % 200 == 0 or index == len(codes):
            log(f"진행 {index}/{len(codes)} 실패 {failed}")

    frame = pd.DataFrame(rows, columns=CONSENSUS_COLUMNS)

    if path.exists():
        previous = pd.read_parquet(path)
        frame = pd.concat([previous[~previous["code"].isin(frame["code"])], frame], ignore_index=True)

    for column in CONSENSUS_COLUMNS:
        if column not in ("snapshot_date", "code", "consensus_date", "estimate_period", "fetched_at"):
            frame[column] = frame[column].astype("float64")

    frame = frame.sort_values("code").reset_index(drop=True)
    frame.to_parquet(path, index=False)
    covered = int(frame["target_price_mean"].notna().sum())
    log(f"컨센서스 저장 {path.relative_to(REPO_ROOT)}: {len(frame)}행, 목표가 있는 종목 {covered}, 실패 {failed}")
    return path


# ---------------------------------------------------------------- CLI

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pages", type=int, default=0, help="category 마다 받을 목록 페이지 수(20건/페이지). 0이면 목록 생략")
    parser.add_argument("--pdf", action="store_true", help="새로 받은 리포트의 PDF 를 내려받고 본문을 뽑는다")
    parser.add_argument("--pdf-all", action="store_true", help="지금까지 받은 목록 전체에서 아직 본문이 없는 PDF 를 처리한다")
    parser.add_argument("--consensus", action="store_true", help="유니버스 종목 컨센서스 스냅샷")
    parser.add_argument("--estimates", action="store_true", help="--consensus 에 finance/annual 추정 결산기 행을 붙인다(종목당 1콜 추가)")
    parser.add_argument("--codes", default="", help="컨센서스 종목을 쉼표로 직접 준다")
    parser.add_argument("--max-pages", type=int, default=40, help="PDF 본문은 앞 N쪽까지만(0이면 전부). 주간·월간 자료가 100쪽을 넘긴다")
    parser.add_argument("--sleep", type=float, default=0.2)
    parser.add_argument("--limit", type=int, default=0, help="검증용: 상세·PDF·컨센서스 종목 수 상한")
    parser.add_argument("--date", default=dt.date.today().isoformat(), help="저장 파일 날짜(기본 오늘)")
    arguments = parser.parse_args()

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    started = time.time()
    snapshot_date = arguments.date
    new_rows = pd.DataFrame(columns=LIST_COLUMNS)

    if arguments.pages > 0:
        new_rows = run_research(arguments.pages, arguments.sleep, arguments.limit, snapshot_date)

    if arguments.pdf_all:
        run_pdf(load_all_lists(), arguments.sleep, arguments.limit, arguments.max_pages)

    elif arguments.pdf:
        run_pdf(new_rows, arguments.sleep, arguments.limit, arguments.max_pages)

    if arguments.pages > 0 or arguments.pdf or arguments.pdf_all:
        latest = write_latest_json()
        log(f"요약 저장 {latest.relative_to(REPO_ROOT)}")

    if arguments.consensus:
        codes = [code.strip() for code in arguments.codes.split(",") if code.strip()] or load_universe_codes()
        run_consensus(codes, arguments.sleep, arguments.limit, arguments.estimates, snapshot_date)

    log(f"완료 {time.time() - started:.0f}초")
    return 0


if __name__ == "__main__":
    sys.exit(main())
