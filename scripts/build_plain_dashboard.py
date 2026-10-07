"""시험 결과 쉽게 읽기 — 틀에 일지 요약·원문·주문 로그를 끼워 넣어 발행본 HTML을 만든다.

입력
  research/dashboard/plain/plain_template.html   틀(/*JOURNAL_DATA*/ 자리에 데이터, /*ASOF*/ 자리에 기준일)
  research/dashboard/plain/journal_summary.json  날짜별 쉬운 말 요약(사람·세션이 쓴 것)
  strategies/*/live/*.md                         일지 원문
  research/dashboard/live.json                   날짜별 주문 합계(PYQuant/dashboard/backfill_live.py 산출)
출력
  _private/dashboard_plain.html                  발행본(약 5MB, 저장소에 넣지 않는다)

요약이 없는 날은 일지 1절 손익과 5절 이슈 줄로 기본 요약을 만들어 넣는다 — 손으로 쓴 요약이 밀려도
날짜 목록이 멈추지 않게 하려는 것이다(2026-10-07, 10-02에서 멈춘 발행본을 보고 바꿈). 기본 요약은
"auto": true 로 표시되고, 세션이 journal_summary.json 에 같은 날짜를 쓰면 그쪽이 이긴다.

    py scripts/build_plain_dashboard.py
"""
import collections
import csv
import datetime
import json
import pathlib
import re
import sys

repo = pathlib.Path(__file__).resolve().parent.parent
plain_directory = repo / "research" / "dashboard" / "plain"
output_path = repo / "_private" / "dashboard_plain.html"

sys.path.insert(0, str(repo / "scripts"))
from _logdir import is_live_row  # noqa: E402

REASON_PLAIN = [
    ("매도가능수량 0", "팔 수 있는 수량이 없어 내지 않음"),
    ("KIS 취소 거부", "이미 체결돼 취소할 수 없음"),
    ("KIS API 거부", "증권사가 주문을 받지 않음"),
    ("Rate limit", "1분 주문 한도를 넘음"),
    ("동시 보유 종목 한도", "동시에 들 수 있는 종목 수를 넘음"),
    ("중복 신호", "같은 신호가 1초 안에 또 와서 무시"),
    ("취소 대상 없음", "취소할 주문이 이미 없음"),
    ("직전 취소가 대상 없음", "취소할 주문이 이미 없음"),
    ("같은 시장가 매도 진행 중", "같은 종목 매도 주문이 이미 나가 있음"),
    ("세션 창 밖", "매매 시간 밖이라 내지 않음"),
    ("점수 우선순위 미달", "더 나은 종목에 자리를 양보"),
    ("총노출 한도", "전체 투자 금액 한도에 걸려 새로 안 삼"),
    ("ENTRY_HALT", "시장이 나빠 새로 사지 않음"),
    ("포지션 한도", "한 종목 보유 수량 한도를 넘음"),
    ("종목당 명목 한도", "한 종목 투자 금액 한도를 넘음"),
    ("일일 손실 한도", "하루 손실 한도에 걸려 멈춤"),
]
WEEKDAYS = "월화수목금토일"


def read_text(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def plain_reason(text):
    for key, label in REASON_PLAIN:
        if key in text:
            return label

    return re.sub(r"\[.*?\]|\(.*?\)|[0-9.,]+", "", text).strip(" —-:")[:40]


def short_number(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return text or ""

    return str(int(value)) if value == int(value) else f"{value:g}"


def fallback_day(strategy, date, journal_text, order_day):
    """손으로 쓴 요약이 없는 날의 기본 요약 — 일지 1절 손익, 5절 이슈 줄, 주문 로그 체결 수만 옮긴다."""
    pnl_match = re.search(r"전일대비 종료 손익 \*{0,2}([+-−]?[0-9,]+)원", journal_text)
    pnl = int(pnl_match.group(1).replace(",", "").replace("−", "-")) if pnl_match else None
    issues = []
    issue_section = re.search(r"## 5\. 이슈 & 조치\n(.*?)(?=\n## |\Z)", journal_text, re.S)
    if issue_section:
        for line in issue_section.group(1).splitlines():
            if line.startswith("- "):
                issues.append(re.sub(r"\s+", " ", line[2:]).strip())

    fills = order_day.get("fill_events") if order_day else None
    weekday = WEEKDAYS[datetime.date.fromisoformat(date).weekday()]
    if pnl is None:
        verdict, pnl_text = "기록 없음", "손익 기록 없음"
    else:
        verdict = "이익" if pnl > 0 else ("손실" if pnl < 0 else "보합")
        pnl_text = f"전일대비 {pnl:+,}원"

    one_line = f"모의계좌는 체결 {fills}건에 {pnl_text}였습니다." if fills is not None else f"모의계좌는 {pnl_text}였습니다."
    return {
        "date": date, "strategy": strategy, "weekday": weekday, "pnl": pnl, "pnl_text": pnl_text,
        "buys": None, "sells": None, "verdict": verdict, "one_line": one_line,
        "happened": issues[:3] or ["이날 일지의 이슈 절이 아직 비어 있습니다. 아래 일지 원문을 보세요."],
        "fixed": "", "next": "", "auto": True,
    }


def main():
    summary = json.loads(read_text(plain_directory / "journal_summary.json"))
    live = json.loads(read_text(repo / "research" / "dashboard" / "live.json"))
    order_days = {day["date"]: day for day in live["order_log"]}

    raw = {}
    for path in sorted(repo.glob("strategies/*/live/*.md")):
        raw[f"{path.parent.parent.name}/{path.stem}"] = read_text(path)

    summary["raw"] = raw

    written = {(day["strategy"], day["date"]) for day in summary["days"]}
    added = []
    for key, text in raw.items():
        strategy, date = key.split("/")
        if (strategy, date) not in written and re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
            summary["days"].append(fallback_day(strategy, date, text, order_days.get(date)))
            added.append(key)

    summary["days"].sort(key=lambda day: (day["date"], day["strategy"]))

    orders = {}
    reject_totals = collections.Counter()
    for day in live["order_log"]:
        path = repo / day["path"]
        if not path.exists():
            continue

        rows = []
        rejected_reasons = collections.Counter()
        with open(path, encoding="utf-8-sig", newline="") as handle:
            for record in csv.DictReader(handle):
                # 시험 바이너리·부하시험 행은 대시보드 집계와 같은 지문(scripts/_logdir.py)으로 뺀다.
                if not is_live_row(record):
                    continue

                event = record.get("event", "")
                reason = record.get("reason") or ""
                if event == "REJECTED" and reason:
                    rejected_reasons[plain_reason(reason)] += 1
                    reject_totals[plain_reason(reason)] += 1

                rows.append([
                    (record.get("ts_kst") or "")[11:19], event, record.get("status", ""),
                    record.get("ticker", ""), record.get("side", ""),
                    short_number(record.get("order_qty")), short_number(record.get("order_price")),
                    short_number(record.get("fill_qty")), short_number(record.get("fill_price")),
                    reason or record.get("entry_reason") or "", short_number(record.get("realized_pnl")),
                ])

        orders[day["date"]] = {
            "total": day.get("total"), "accepted": day.get("accepted"), "filled": day.get("fill_events"),
            "cancelled": day.get("cancelled"), "rejected": day.get("rejected"),
            "buy": day.get("by_side", {}).get("BUY"), "sell": day.get("by_side", {}).get("SELL"),
            "tickers": day.get("n_tickers"), "realized_pnl": day.get("realized_pnl"),
            "notional": (day.get("buy_notional") or 0) + (day.get("sell_notional") or 0),
            "top_rejects": rejected_reasons.most_common(3), "source": day["path"], "rows": rows,
        }

    # 백테스트 탭의 전체 목록 — 표지 칸(verdict_tag·headline·pages·questions)만 옮긴다. 정본은 research/studies/index.json
    # (규약 research/studies/README.md "결과 발행 규약", 2026-10-07 quant-27과 맞춤). 손으로 고른 8건은 틀에 그대로 둔다.
    study_index = json.loads(read_text(repo / "research" / "studies" / "index.json"))
    summary["study_index"] = [
        {key: item.get(key) for key in ("number", "title", "question", "verdict", "verdict_tag", "headline", "pages", "questions")}
        for item in sorted(study_index, key=lambda item: item.get("number") or "")
        if item.get("number")
    ]
    summary["orders"] = orders
    summary["reject_totals"] = reject_totals.most_common()
    summary.setdefault("bt_specs", {})
    summary.setdefault("load_stages", {})
    summary.setdefault("bt_tables", {})

    as_of = max(day["date"] for day in summary["days"])
    payload = json.dumps(summary, ensure_ascii=False).replace("</", "<\\/")
    template = read_text(plain_directory / "plain_template.html")
    out = template.replace("/*JOURNAL_DATA*/", payload).replace("/*ASOF*/", as_of)
    output_path.write_bytes(out.encode("utf-8"))
    print(f"[build_plain_dashboard] {output_path.relative_to(repo).as_posix()} 기준 {as_of} · 일지 {len(summary['days'])}편"
          f" · 기본 요약 {len(added)}편 {added} · {len(out.encode('utf-8')) // 1024}KB")


if __name__ == "__main__":
    main()
