"""KIND 상장폐지 목록 적재 — 유가증권·코스닥의 연도별 상장폐지 회사(이름·폐지일·사유)를 받아 종목코드를 붙이고,
일봉 패널 `PYQuant/data/bars_all_pit_v2.parquet`에 없는 상폐사의 일봉을 네이버에서 받아 패널에 더한다.

왜 필요한가 — 스터디 19 유니버스는 DART 현재 회사 목록 기반이라 옛 상폐사가 빠져 등급 B였다(R2 README §2-3).
상폐사를 포함한 회사 목록(등급 A)을 만들려면 먼저 "누가 언제 빠졌는가"의 정본이 있어야 한다. KIND가 그 정본이다.

종목코드는 상폐 목록 화면에는 없고(회사 요약 팝업도 상폐사는 빈 칸) 회사명 자동완성(`common/searchcorpname.do`)이
`repisusrtcd2`(6자리)와 `liststatcd`(D = 상장폐지)를 준다. 네이버 siseJson은 상폐 종목도 정리매매 마지막 날까지
전 기간을 주므로(2026-09-20 실측: 파티게임즈 194510 → 2020-09-08까지) terminal_price를 그대로 얻는다.

    py PYQuant/tools/kind_delisted_fill.py --start 2000 --end 2026              # KIND 목록 + 코드 붙이기 + 커버율
    py PYQuant/tools/kind_delisted_fill.py --report-only                        # 받은 parquet으로 커버율만
    py PYQuant/tools/kind_delisted_fill.py --report-only --fill-bars            # 패널에 없는 상폐사 일봉을 받아 패널에 더한다

산출물: PYQuant/data/universe/kind_delisted.parquet
        (name, market, delist_date, reason, note, kind_id, code, bars_last_date)
        PYQuant/data/universe/membership.parquet — 종목별 첫·마지막 거래일·폐지일·사유·마지막 종가(백테스트 회원 기간표)
        --fill-bars 이면 PYQuant/data/bars_all_pit_v2.parquet 에 행이 늘고 캐시는 PYQuant/data/cache/bars_v2/<code>.parquet
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

import pandas as pd
import requests

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "PYQuant" / "tools"))
from naver_bars_backfill import CACHE_DIR, OUTPUT_COLUMNS, fetch_text, parse_bars   # noqa: E402

OUT_PATH = ROOT / "PYQuant" / "data" / "universe" / "kind_delisted.parquet"
CODE_CACHE_PATH = ROOT / "PYQuant" / "data" / "universe" / "kind_name_to_code.json"
BARS_PATH = ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
MEMBERSHIP_PATH = ROOT / "PYQuant" / "data" / "universe" / "membership.parquet"

KIND_MAIN = "https://kind.krx.co.kr/investwarn/delcompany.do?method=searchDelCompanyMain"
KIND_LIST = "https://kind.krx.co.kr/investwarn/delcompany.do"
KIND_NAME_SEARCH = "https://kind.krx.co.kr/common/searchcorpname.do"
MARKETS = {"1": "KOSPI", "2": "KOSDAQ"}   # 코넥스(6)는 일봉 패널에 없어 뺀다
PAGE_SIZE = 100
HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/128 Safari/537.36",
    "Accept-Language": "ko-KR,ko;q=0.9",
    "Referer": KIND_MAIN,
    "X-Requested-With": "XMLHttpRequest",
}
# 화면의 fnSearch()가 보내는 폼 그대로. method/forward가 다르면 "페이지 오류"만 돌아온다.
FORM_BASE = {
    "method": "searchDelCompanySub", "forward": "delcompany_sub", "searchType": "", "tabType": "1",
    "menuUrl": "/investwarn/delcompany.do?method=searchDelCompanyMain", "isurCd": "", "searchCodeType": "",
    "orderMode": "2", "orderStat": "D", "currentPageSize": str(PAGE_SIZE), "searchMode": "",
    "searchCorpName": "", "searchCorpNameTmp": "", "repIsuSrtCd": "",
}
ROW_PATTERN = re.compile(
    r"companysummary_open\('(?P<kind_id>\d+)'\)[^>]*title='(?P<name>[^']*)'.*?"
    r"<td class=\"txc\">(?P<date>\d{4}-\d{2}-\d{2})</td>\s*<td>(?P<reason>.*?)</td>\s*<td>(?P<note>.*?)</td>",
    re.S)
TOTAL_PATTERN = re.compile(r"전체\s*(\d+)")


def strip_tags(text: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", text)).strip()


def open_session() -> requests.Session:
    session = requests.Session()
    session.get(KIND_MAIN, headers=HEADERS, timeout=30)   # 세션 쿠키. 이것 없이 POST하면 페이지 오류
    return session


def fetch_year(session: requests.Session, market_code: str, year: int) -> list[dict]:
    rows: list[dict] = []
    page_index = 1

    while True:
        form = dict(FORM_BASE, marketType=market_code, fromDate=f"{year}-01-01", toDate=f"{year}-12-31",
                    pageIndex=str(page_index))
        response = session.post(KIND_LIST, data=form, headers=HEADERS, timeout=30)
        response.raise_for_status()
        html = response.text

        if "페이지 오류" in html:
            raise RuntimeError(f"KIND 페이지 오류 — market={market_code} year={year} page={page_index}")

        total_match = TOTAL_PATTERN.search(strip_tags(html))
        total = int(total_match.group(1)) if total_match else 0

        for match in ROW_PATTERN.finditer(html):
            rows.append({
                "name": match["name"].strip(), "market": MARKETS[market_code], "delist_date": match["date"],
                "reason": strip_tags(match["reason"]), "note": strip_tags(match["note"]), "kind_id": match["kind_id"],
            })

        if page_index * PAGE_SIZE >= total:
            break

        page_index += 1
        time.sleep(0.3)

    return rows


def fetch_all(session: requests.Session, start_year: int, end_year: int) -> pd.DataFrame:
    rows: list[dict] = []

    for year in range(start_year, end_year + 1):
        for market_code in MARKETS:
            year_rows = fetch_year(session, market_code, year)
            print(f"  {year} {MARKETS[market_code]:6s} {len(year_rows):3d}건", flush=True)
            rows.extend(year_rows)
            time.sleep(0.5)

    frame = pd.DataFrame(rows)
    frame["delist_date"] = pd.to_datetime(frame["delist_date"])
    return (frame.drop_duplicates(["name", "market", "delist_date"])
            .sort_values(["delist_date", "market", "name"]).reset_index(drop=True))


def lookup_code(session: requests.Session, name: str, market: str) -> str | None:
    """회사명 자동완성으로 6자리 종목코드. 같은 이름이 여럿이면 상장폐지(liststatcd D)·같은 시장을 우선한다."""
    response = session.post(KIND_NAME_SEARCH, data={"method": "searchCorpNameJson", "searchCorpName": name},
                            headers=HEADERS, timeout=30)
    response.raise_for_status()

    try:
        candidates = json.loads(response.text)
    except json.JSONDecodeError:
        return None

    market_code = {"KOSPI": "1", "KOSDAQ": "2"}[market]
    exact = [item for item in candidates if item.get("repisusrtkornm") == name or item.get("comabbrv") == name]
    pool = exact or candidates

    def rank(item: dict) -> tuple:
        return (item.get("liststatcd") != "D", item.get("spotisutrdmkttpcd") != market_code)

    for item in sorted(pool, key=rank):
        code = item.get("repisusrtcd2") or ""

        if re.fullmatch(r"\d{6}", code):
            return code

    return None


def attach_codes(session: requests.Session, delisted: pd.DataFrame) -> pd.DataFrame:
    """KIND 행마다 종목코드를 붙인다. 이름→코드는 json 캐시에 두어 재실행 때 다시 묻지 않는다."""
    cache: dict[str, str | None] = json.loads(CODE_CACHE_PATH.read_text(encoding="utf-8")) if CODE_CACHE_PATH.exists() else {}
    codes: list[str | None] = []
    asked = 0

    for row in delisted.itertuples(index=False):
        key = f"{row.market}|{row.name}"

        if key not in cache:
            cache[key] = lookup_code(session, row.name, row.market)
            asked += 1
            time.sleep(0.2)

            if asked % 100 == 0:
                print(f"  코드 조회 {asked}건", flush=True)
                CODE_CACHE_PATH.write_text(json.dumps(cache, ensure_ascii=False, indent=0), encoding="utf-8")

        codes.append(cache[key])

    CODE_CACHE_PATH.write_text(json.dumps(cache, ensure_ascii=False, indent=0), encoding="utf-8")
    return delisted.assign(code=codes)


def bars_terminals() -> pd.DataFrame:
    """일봉 패널의 종목별 마지막 거래일."""
    import pyarrow.parquet as pq
    bars = pq.read_table(BARS_PATH, columns=["Date", "code"]).to_pandas()
    return bars.groupby("code")["Date"].max().rename("bars_last_date").reset_index()


def attach_bars_last_date(delisted: pd.DataFrame) -> pd.DataFrame:
    delisted = delisted.drop(columns=["bars_last_date"], errors="ignore")
    return delisted.merge(bars_terminals(), on="code", how="left")


def fill_bars(delisted: pd.DataFrame, sleep_seconds: float) -> pd.DataFrame:
    """코드는 있는데 패널에 일봉이 없는 상폐사를 네이버에서 받아 패널에 더한다. 빈 응답은 건너뛴다."""
    targets = delisted[delisted["code"].notna() & delisted["bars_last_date"].isna()].drop_duplicates("code")
    print(f"패널에 없는 상폐사 {len(targets)}종목 일봉 받기")
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    end_date = pd.Timestamp.today().strftime("%Y%m%d")
    frames = []
    empty = 0

    for index, row in enumerate(targets.itertuples(index=False), start=1):
        cache_path = CACHE_DIR / f"{row.code}.parquet"

        if cache_path.exists():
            frame = pd.read_parquet(cache_path)
        else:
            frame = parse_bars(fetch_text(row.code, end_date, sleep_seconds))
            frame.to_parquet(cache_path, index=False)

        if frame.empty:
            empty += 1
            continue

        frame = frame.assign(code=row.code, name=row.name, market=row.market, delisted=True)
        frames.append(frame[OUTPUT_COLUMNS])

        if index % 100 == 0:
            print(f"  {index}/{len(targets)} (빈 응답 {empty})", flush=True)

    if not frames:
        print("더할 일봉이 없다")
        return delisted

    added = pd.concat(frames, ignore_index=True)
    panel = pd.read_parquet(BARS_PATH)
    panel = pd.concat([panel, added[panel.columns]], ignore_index=True).sort_values(["code", "Date"]).reset_index(drop=True)
    panel.to_parquet(BARS_PATH, index=False)
    print(f"패널에 {added['code'].nunique()}종목 {len(added)}행 추가(빈 응답 {empty}종목) → {BARS_PATH.relative_to(ROOT)} {len(panel)}행")
    return attach_bars_last_date(delisted)


def write_membership(delisted: pd.DataFrame) -> Path:
    """종목별 회원 기간표 — 패널의 첫·마지막 거래일에 KIND 폐지일·사유·마지막 종가(terminal_price)를 붙인다.
    백테스트는 이 표로 "그날 상장돼 있던 종목"만 고른다(bias-auditor 3행)."""
    import pyarrow.parquet as pq
    bars = pq.read_table(BARS_PATH, columns=["Date", "code", "name", "market", "delisted", "Close"]).to_pandas()
    bars = bars.sort_values(["code", "Date"])
    membership = bars.groupby("code").agg(name=("name", "last"), market=("market", "last"),
                                         effective_from=("Date", "min"), effective_to=("Date", "max"),
                                         delisted=("delisted", "max"), terminal_price=("Close", "last")).reset_index()
    kind = (delisted[delisted["code"].notna()].sort_values("delist_date")
            .drop_duplicates("code", keep="last")[["code", "delist_date", "reason"]]
            .rename(columns={"reason": "delist_reason"}))
    membership = membership.merge(kind, on="code", how="left")
    # end_reason: 살아 있으면 listed, 합병·완전자회사화·이전상장이면 merged(주주는 다른 주식을 받는다), 나머지는 delisted
    merged_pattern = r"합병|완전자회사|유가증권시장 상장|코스닥시장 상장|이전상장"
    is_merged = membership["delist_reason"].fillna("").str.contains(merged_pattern)
    membership["end_reason"] = "listed"
    membership.loc[membership["delisted"], "end_reason"] = "delisted"
    membership.loc[membership["delisted"] & is_merged, "end_reason"] = "merged"
    membership.loc[~membership["delisted"], ["terminal_price", "effective_to"]] = [pd.NA, pd.NaT]   # 상장 중이면 끝이 없다
    membership["terminal_price"] = membership["terminal_price"].astype("Int64")
    membership = membership[["code", "name", "market", "effective_from", "effective_to", "end_reason",
                             "delist_date", "delist_reason", "terminal_price"]]
    membership.to_parquet(MEMBERSHIP_PATH, index=False)
    ended = membership[membership["end_reason"] != "listed"]
    print(f"저장 {MEMBERSHIP_PATH.relative_to(ROOT)} {len(membership)}종목 — "
          f"{ended['end_reason'].value_counts().to_dict()}, KIND 폐지일 있는 것 {ended['delist_date'].notna().sum()}종목, "
          f"terminal_price 결측 {int(ended['terminal_price'].isna().sum())}")
    return MEMBERSHIP_PATH


def report(delisted: pd.DataFrame) -> None:
    delisted = delisted.assign(year=delisted["delist_date"].dt.year, has_code=delisted["code"].notna(),
                               has_bars=delisted["bars_last_date"].notna())
    by_year = (delisted.groupby(["year", "market"])
               .agg(kind=("name", "size"), code=("has_code", "sum"), bars=("has_bars", "sum")).reset_index())
    by_year["bars_%"] = (100 * by_year["bars"] / by_year["kind"]).round(0).astype(int)
    print("\n연도·시장별 KIND 상장폐지 건수 / 종목코드 붙은 수 / 일봉 패널에 가격 이력이 있는 수와 비율:")
    print(by_year.pivot(index="year", columns="market", values=["kind", "code", "bars", "bars_%"])
          .fillna(0).astype(int).to_string())
    total = len(delisted)
    code_count = int(delisted["has_code"].sum())
    bars_count = int(delisted["has_bars"].sum())
    print(f"\n합계 {total}건 — 코드 {code_count}건({100 * code_count / max(total, 1):.0f}%), "
          f"일봉 {bars_count}건({100 * bars_count / max(total, 1):.0f}%)")
    with_bars = delisted[delisted["has_bars"]]
    gap = (with_bars["delist_date"] - with_bars["bars_last_date"]).dt.days
    print(f"일봉 마지막 날과 폐지일 차이(일): 중앙값 {gap.median():.0f}, 30일 넘는 건 {(gap > 30).sum()}건 — 정리매매 뒤 폐지까지 보통 며칠")
    reasons = delisted.loc[~delisted["has_bars"], "reason"].str[:14].value_counts().head(8)
    print("일봉 없는 행의 사유 상위:\n" + reasons.to_string())


def main() -> int:
    parser = argparse.ArgumentParser(description="KIND 상장폐지 목록 적재·종목코드·일봉 패널 보강")
    parser.add_argument("--start", type=int, default=2000, help="시작 연도")
    parser.add_argument("--end", type=int, default=pd.Timestamp.today().year, help="끝 연도")
    parser.add_argument("--report-only", action="store_true", help="KIND를 다시 받지 않고 기존 parquet으로")
    parser.add_argument("--fill-bars", action="store_true", help="패널에 없는 상폐사 일봉을 네이버에서 받아 패널에 더한다")
    parser.add_argument("--sleep", type=float, default=0.15, help="네이버 호출 간격(초)")
    arguments = parser.parse_args()

    if arguments.report_only:
        delisted = pd.read_parquet(OUT_PATH)
    else:
        print(f"KIND 상장폐지 목록 {arguments.start}~{arguments.end}")
        session = open_session()
        delisted = fetch_all(session, arguments.start, arguments.end)
        delisted = attach_codes(session, delisted)
        delisted = attach_bars_last_date(delisted)

    if delisted["code"].isna().all() or "bars_last_date" not in delisted.columns:
        delisted = attach_codes(open_session(), delisted.drop(columns=["code"], errors="ignore"))
        delisted = attach_bars_last_date(delisted)

    if arguments.fill_bars:
        delisted = fill_bars(delisted, arguments.sleep)

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    delisted.to_parquet(OUT_PATH, index=False)
    print(f"저장 {OUT_PATH.relative_to(ROOT)} {len(delisted)}행")
    write_membership(delisted)
    report(delisted)
    return 0


if __name__ == "__main__":
    sys.exit(main())
