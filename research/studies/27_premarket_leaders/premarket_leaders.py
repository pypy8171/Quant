"""NXT 프리마켓(08:00~08:50) 주도 업종·테마 포착 — 1단계 프로토타입(조회 전용, 주문 없음).

하위 명령
  snapshot  네이버 시세판 한 바퀴(전 종목, 요청 약 50건)를 받아 NXT 프리마켓 값으로 업종·테마를 묶고
            동반 강세 묶음과 후보 종목을 daily/YYYY-MM-DD.jsonl 에 한 줄 덧붙인다. KIS 앱키를 쓰지 않는다.
  evaluate  장 마감 뒤 같은 날 마지막 스냅숏의 포착 종목이 당일 주도주가 됐는지 판정해 daily/YYYY-MM-DD_eval.json.
  kis-check 모의 앱키(Quant/config/config_dev_paper.json의 kis 절)로 순위 TR을 시장 코드별로 한 번씩, 1초 간격.
            인증 정보·토큰은 출력하지 않는다. 실계좌 앱키와 같으면 호출하지 않고 멈춘다.

시세 출처는 엔진 시세판(Quant/src/universe/MarketBoard.cpp)과 같은 네이버 polling 응답이다. 엔진은 맨 위 칸(KRX)만
읽어 09:00 전에는 거래대금이 비고, NXT 프리마켓 값은 overMarketPriceInfo 안에 있다.
  [wire] 2026-10-01 08:56 실측(005490): marketStatus "PREOPEN", 맨 위 accumulatedTradingValueRaw "",
  overMarketPriceInfo.tradingSessionType "PRE_MARKET"·overPrice "310,500"·fluctuationsRatio "1.47"·
  accumulatedTradingValueRaw "8749000000". 공식 문서 없음.
업종은 네이버 업종(79개, /api/stocks/industry), 테마는 PYQuant/data/themes/latest.json(전날 밤 적재, 인포스탁 분류).
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import statistics
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
DAILY = HERE / "daily"
INDUSTRY_CACHE = HERE / "industry_map.json"
THEME_SNAPSHOT = ROOT / "PYQuant" / "data" / "themes" / "latest.json"

LISTING = "https://m.stock.naver.com/api/stocks/marketValue/{market}?page={page}&pageSize=100"
POLLING = "https://polling.finance.naver.com/api/realtime/domestic/stock/"
INDUSTRY = "https://m.stock.naver.com/api/stocks/industry"
HEADERS = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/128.0",
           "Referer": "https://finance.naver.com/", "Accept": "application/json"}
CODES_PER_CALL = 900          # 엔진 시세판 기본값과 같다(1,000까지 받음, 09-26 실측)

# 판정 문턱 — 1단계 초깃값. 기록이 쌓이면 evaluate 결과로 다시 잡는다.
STRONG_CHANGE = 2.0           # 강세 종목: NXT 등락률 +2% 이상
MIN_VALUE_WON = 3e8           # 강세 종목: 프리마켓 누적 거래대금 3억 원 이상
GROUP_MIN_TRADED = 3          # 묶음 판정에 필요한 프리마켓 거래 종목 수
GROUP_MIN_STRONG = 2          # 동반 강세: 강세 종목 2개 이상
GROUP_MIN_MEDIAN = 1.0        # 동반 강세: 거래 종목 등락률 중앙값 +1% 이상
GROUP_MIN_BREADTH = 0.6       # 동반 강세: 거래 종목 중 상승 비율 60% 이상
TOP_GROUPS = 5
MAX_CANDIDATES = 20


def get_json(url: str, timeout: float = 10.0) -> dict:
    request = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def to_number(text) -> float | None:
    if text is None:
        return None
    cleaned = str(text).replace(",", "").strip()
    if not cleaned or cleaned in ("-", "N/A"):
        return None
    try:
        return float(cleaned)
    except ValueError:
        return None


def signed_ratio(block: dict) -> float | None:
    """over 칸의 fluctuationsRatio가 부호 없이 올 때를 대비해 compareToPreviousPrice(4 하한·5 하락)로 부호를 붙인다."""
    ratio = to_number(block.get("fluctuationsRatio"))
    if ratio is None:
        return None
    direction = str((block.get("compareToPreviousPrice") or {}).get("code", ""))
    if direction in ("4", "5") and ratio > 0:
        return -ratio
    return ratio


def fetch_listing() -> list[dict]:
    listing: list[dict] = []
    for market in ("KOSPI", "KOSDAQ"):
        page, pages = 1, 1
        while page <= pages and page <= 60:
            body = get_json(LISTING.format(market=market, page=page))
            total = int(body.get("totalCount") or 0)
            pages = (total + 99) // 100
            for row in body.get("stocks") or []:
                if row.get("stockEndType") == "stock":
                    listing.append({"code": row["itemCode"], "name": row.get("stockName", ""), "market": market})
            page += 1
    return listing


def fetch_board(codes: list[str]) -> list[dict]:
    urls = [POLLING + ",".join(codes[begin:begin + CODES_PER_CALL]) for begin in range(0, len(codes), CODES_PER_CALL)]
    with ThreadPoolExecutor(max_workers=4) as pool:
        bodies = list(pool.map(get_json, urls))
    rows: list[dict] = []
    for body in bodies:
        rows.extend(body.get("datas") or [])
    return rows


def parse_row(row: dict) -> dict:
    over = row.get("overMarketPriceInfo") or {}
    return {
        "code": row.get("itemCode"),
        "name": row.get("stockName", ""),
        "krx_status": row.get("marketStatus"),
        "krx_close": to_number(row.get("closePriceRaw")),
        "krx_change": to_number(row.get("fluctuationsRatioRaw")),
        "krx_value": to_number(row.get("accumulatedTradingValueRaw")),
        "session": over.get("tradingSessionType"),
        "nxt_price": to_number(over.get("overPrice")),
        "nxt_change": signed_ratio(over),
        "nxt_value": to_number(over.get("accumulatedTradingValueRaw")),
        "nxt_volume": to_number(over.get("accumulatedTradingVolumeRaw")),
        "nxt_time": over.get("localTradedAt"),
        "market_value": to_number(row.get("marketValueFullRaw")),
    }


def load_industry_map(max_age_days: int = 7) -> dict[str, str]:
    """종목코드 → 네이버 업종명. 업종 편입은 잘 안 바뀌어 7일 캐시한다(갱신 시 약 80건 호출, 0.1초 간격)."""
    if INDUSTRY_CACHE.exists():
        cached = json.loads(INDUSTRY_CACHE.read_text(encoding="utf-8"))
        age = dt.date.today() - dt.date.fromisoformat(cached["asof"])
        if age.days <= max_age_days:
            return cached["by_ticker"]
    groups, page = [], 1
    while True:
        body = get_json(f"{INDUSTRY}?page={page}&pageSize=100")
        groups.extend(body.get("groups") or [])
        if len(groups) >= int(body.get("totalCount") or 0) or not body.get("groups"):
            break
        page += 1
    by_ticker: dict[str, str] = {}
    for group in groups:
        member_page = 1
        while True:
            body = get_json(f"{INDUSTRY}/{group['no']}?page={member_page}&pageSize=100")
            stocks = body.get("stocks") or []
            for stock in stocks:
                by_ticker.setdefault(stock["itemCode"], group["name"])
            time.sleep(0.1)
            if len(stocks) < 100:
                break
            member_page += 1
    INDUSTRY_CACHE.write_text(json.dumps({"asof": dt.date.today().isoformat(), "by_ticker": by_ticker},
                                         ensure_ascii=False), encoding="utf-8")
    return by_ticker


def load_theme_map() -> tuple[dict[str, list[str]], str]:
    """종목코드 → 테마명 목록. 인포스탁 테마는 한 종목이 여러 테마에 든다."""
    if not THEME_SNAPSHOT.exists():
        return {}, ""
    snapshot = json.loads(THEME_SNAPSHOT.read_text(encoding="utf-8"))
    by_ticker: dict[str, list[str]] = {}
    for theme in snapshot.get("themes") or []:
        for member in theme.get("members") or []:
            by_ticker.setdefault(member["ticker"], []).append(theme["name"])
    return by_ticker, str(snapshot.get("asof", ""))


def judge_groups(quotes: list[dict], membership: dict[str, list[str]]) -> list[dict]:
    """묶음마다 프리마켓 거래 종목만으로 폭(상승 비율)·중앙값·강세 종목 수를 세고 동반 강세를 판정한다."""
    members: dict[str, list[dict]] = {}
    for quote in quotes:
        for group in membership.get(quote["code"], []):
            members.setdefault(group, []).append(quote)
    judged = []
    for group, rows in members.items():
        if len(rows) < GROUP_MIN_TRADED:
            continue
        changes = [row["nxt_change"] for row in rows]
        strong = [row for row in rows if row["nxt_change"] >= STRONG_CHANGE and row["nxt_value"] >= MIN_VALUE_WON]
        median = statistics.median(changes)
        breadth = sum(1 for change in changes if change > 0) / len(changes)
        value = sum(row["nxt_value"] for row in rows)
        leading = len(strong) >= GROUP_MIN_STRONG and median >= GROUP_MIN_MEDIAN and breadth >= GROUP_MIN_BREADTH
        judged.append({
            "group": group, "traded": len(rows), "strong": len(strong), "median_change": round(median, 2),
            "breadth": round(breadth, 2), "value_eok": round(value / 1e8, 1), "leading": leading,
            # 점수: 중앙값 × 강세 종목 수의 제곱근. 한 종목 급등 테마가 위로 오지 않게 폭을 곱한다.
            "score": round(median * (len(strong) ** 0.5) * breadth, 3),
            "members": [row["code"] for row in sorted(rows, key=lambda row: -row["nxt_value"])][:10],
        })
    judged.sort(key=lambda group: (-group["leading"], -group["score"]))
    return judged


def snapshot(arguments) -> None:
    now = dt.datetime.now()
    listing = fetch_listing()
    rows = [parse_row(row) for row in fetch_board([stock["code"] for stock in listing])]
    traded = [row for row in rows
              if row["session"] == "PRE_MARKET" and row["nxt_change"] is not None and (row["nxt_value"] or 0) > 0]
    industry_of = {code: [name] for code, name in load_industry_map().items()}
    theme_of, theme_asof = load_theme_map()
    industries = judge_groups(traded, industry_of)
    themes = judge_groups(traded, theme_of)

    leading_groups = [group for group in industries + themes if group["leading"]]
    picked: dict[str, dict] = {}
    for group in sorted(leading_groups, key=lambda group: -group["score"]):
        for code in group["members"]:
            quote = next(row for row in traded if row["code"] == code)
            if quote["nxt_change"] >= STRONG_CHANGE and quote["nxt_value"] >= MIN_VALUE_WON and code not in picked:
                picked[code] = {"code": code, "name": quote["name"], "nxt_change": quote["nxt_change"],
                                "nxt_value_eok": round(quote["nxt_value"] / 1e8, 1), "via": group["group"]}
    candidates = list(picked.values())[:MAX_CANDIDATES]

    record = {
        "time": now.isoformat(timespec="seconds"), "listed": len(listing), "board_rows": len(rows),
        "premarket_traded": len(traded), "theme_asof": theme_asof,
        "industries": industries[:TOP_GROUPS], "themes": themes[:TOP_GROUPS], "candidates": candidates,
        "top_value": [{"code": row["code"], "name": row["name"], "nxt_change": row["nxt_change"],
                       "nxt_value_eok": round(row["nxt_value"] / 1e8, 1)}
                      for row in sorted(traded, key=lambda row: -row["nxt_value"])[:20]],
    }
    DAILY.mkdir(exist_ok=True)
    with open(DAILY / f"{now:%Y-%m-%d}.jsonl", "a", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(record, ensure_ascii=False) + "\n")

    print(f"{record['time']} 상장 {len(listing)} · 시세 {len(rows)} · 프리마켓 거래 {len(traded)}종목")
    for label, groups in (("업종", industries), ("테마", themes)):
        for group in groups[:TOP_GROUPS]:
            mark = "동반강세" if group["leading"] else "-"
            print(f"  [{label}] {group['group']}: {mark} 중앙값 {group['median_change']:+.2f}% "
                  f"폭 {group['breadth']:.0%} 강세 {group['strong']}/{group['traded']} 대금 {group['value_eok']}억")
    print("  후보: " + ", ".join(f"{item['name']}({item['nxt_change']:+.1f}%)" for item in candidates))


def evaluate(arguments) -> None:
    """마지막 스냅숏의 후보·동반 강세 업종이 당일 KRX 정규장에서 주도했는지 본다(15:30 뒤 실행)."""
    day = arguments.date or dt.date.today().isoformat()
    lines = (DAILY / f"{day}.jsonl").read_text(encoding="utf-8").splitlines()
    last = json.loads(lines[-1])
    listing = fetch_listing()
    rows = [parse_row(row) for row in fetch_board([stock["code"] for stock in listing])]
    closed = [row for row in rows if row["krx_change"] is not None and (row["krx_value"] or 0) > 0]
    value_rank = {row["code"]: rank for rank, row in enumerate(sorted(closed, key=lambda row: -row["krx_value"]), 1)}
    change_rank = {row["code"]: rank for rank, row in
                   enumerate(sorted(closed, key=lambda row: -row["krx_change"]), 1)}
    by_code = {row["code"]: row for row in closed}
    market_median = statistics.median(row["krx_change"] for row in closed)

    results = []
    for item in last["candidates"]:
        row = by_code.get(item["code"])
        if row is None:
            continue
        results.append({**item, "krx_change": row["krx_change"], "value_rank": value_rank[item["code"]],
                        "change_rank": change_rank[item["code"]],
                        # 주도주 판정: 거래대금 상위 100 안이고 등락률이 시장 중앙값보다 3%p 이상 높다
                        "leader": value_rank[item["code"]] <= 100 and row["krx_change"] - market_median >= 3.0})
    industry_of = {code: [name] for code, name in load_industry_map().items()}
    day_industries = judge_groups([{**row, "nxt_change": row["krx_change"], "nxt_value": row["krx_value"]}
                                   for row in closed], industry_of)
    top_day = [group["group"] for group in sorted(day_industries, key=lambda group: -group["median_change"])[:10]]
    report = {
        "date": day, "snapshot_time": last["time"], "market_median_change": round(market_median, 2),
        "candidates": results,
        "hit_rate": round(sum(result["leader"] for result in results) / len(results), 2) if results else None,
        "premarket_leading_industries": [group["group"] for group in last["industries"] if group["leading"]],
        "day_top10_industries_by_median": top_day,
    }
    (DAILY / f"{day}_eval.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=1))


def kis_check(arguments) -> None:
    """모의 앱키로 순위 TR이 되는지만 본다. 응답 코드·메시지·행 수만 찍는다."""
    sys.path.insert(0, str(ROOT / "PYQuant"))
    from kis.client import KisClient   # noqa: E402 — PYQuant 경로를 넣은 뒤에만 불러올 수 있다

    paper = json.loads((ROOT / "Quant/config/config_dev_paper.json").read_text(encoding="utf-8"))["kis"]
    live = json.loads((ROOT / "Quant/config/config_live.json").read_text(encoding="utf-8"))
    live_keys = {section.get("app_key") for section in live.values() if isinstance(section, dict)}
    if not paper.get("is_paper") or paper.get("app_key") in live_keys:
        print("모의 앱키가 아니거나 실계좌 앱키와 같다 — 호출하지 않는다")
        return
    client = KisClient(paper["app_key"], paper["app_secret"], paper.get("account_no", ""), is_paper=True)
    calls = [
        ("volume-rank NX", "/uapi/domestic-stock/v1/quotations/volume-rank", "FHPST01710000",
         {"FID_COND_MRKT_DIV_CODE": "NX", "FID_COND_SCR_DIV_CODE": "20171", "FID_INPUT_ISCD": "0000",
          "FID_DIV_CLS_CODE": "0", "FID_BLNG_CLS_CODE": "3", "FID_TRGT_CLS_CODE": "111111111",
          "FID_TRGT_EXLS_CLS_CODE": "0000000000", "FID_INPUT_PRICE_1": "", "FID_INPUT_PRICE_2": "",
          "FID_VOL_CNT": "", "FID_INPUT_DATE_1": ""}),
        ("volume-rank J", "/uapi/domestic-stock/v1/quotations/volume-rank", "FHPST01710000",
         {"FID_COND_MRKT_DIV_CODE": "J", "FID_COND_SCR_DIV_CODE": "20171", "FID_INPUT_ISCD": "0000",
          "FID_DIV_CLS_CODE": "0", "FID_BLNG_CLS_CODE": "3", "FID_TRGT_CLS_CODE": "111111111",
          "FID_TRGT_EXLS_CLS_CODE": "0000000000", "FID_INPUT_PRICE_1": "", "FID_INPUT_PRICE_2": "",
          "FID_VOL_CNT": "", "FID_INPUT_DATE_1": ""}),
        ("inquire-price NX 005490", "/uapi/domestic-stock/v1/quotations/inquire-price", "FHKST01010100",
         {"FID_COND_MRKT_DIV_CODE": "NX", "FID_INPUT_ISCD": "005490"}),
    ][: arguments.max_calls]
    for label, path, transaction_id, parameters in calls:
        body = client._get(path, parameters, transaction_id)
        output = body.get("output")
        rows = len(output) if isinstance(output, list) else (1 if isinstance(output, dict) and output else 0)
        first = output[0] if isinstance(output, list) and output else (output if isinstance(output, dict) else {})
        sample = {key: first.get(key) for key in ("mksc_shrn_iscd", "hts_kor_isnm", "stck_prpr", "prdy_ctrt", "acml_tr_pbmn")
                  if key in first}
        print(f"{label}: rt_cd={body.get('rt_cd')} msg_cd={body.get('msg_cd')} msg={body.get('msg1', '').strip()} "
              f"행={rows} 첫행={sample}")
        time.sleep(1.1)   # 1초에 1회 이하 — 같은 앱키를 모의 엔진이 쓰고 있다


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("snapshot")
    evaluate_parser = sub.add_parser("evaluate")
    evaluate_parser.add_argument("--date")
    check_parser = sub.add_parser("kis-check")
    check_parser.add_argument("--max-calls", type=int, default=3)
    arguments = parser.parse_args()
    {"snapshot": snapshot, "evaluate": evaluate, "kis-check": kis_check}[arguments.command](arguments)


if __name__ == "__main__":
    main()
