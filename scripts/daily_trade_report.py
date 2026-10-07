"""하루 매매 리포트 — 모의·실계좌의 그날 매매를 HTML 한 장으로 묶는다.

    py scripts/daily_trade_report.py                    # 오늘
    py scripts/daily_trade_report.py --date 2026-10-07  # 지난 날

산출: _private/daily_reports/YYYY-MM-DD.html (+ 날짜 목록 index.html). 실계좌 잔고·주문이 들어 있어
저장소 밖(gitignore)에만 쓴다. 예약작업 'Quant Daily Trade Report'가 애프터마켓 끝(20:00) 뒤 20:10에 부른다.

한 장에 담는 것(위에서 아래로):
  요약 카드 → 국면 곡선 → 종목별 1분봉 차트(매수▲·매도▼·지정가 대기선·손절선) → 주문·체결 표 →
  적재 현황(원장 바이너리·DB·틱 캡처·매매일지) → 오류·점검 판정.

재료는 전부 엔진이 이미 남기는 파일이다 — 새로 기록하는 것은 없다.
  주문·체결·사유   Quant/build_win/<로그 폴더>/trades_YYYYMMDD.csv
  원장 바이너리    <ledger_journal_dir>/ledger_YYYYMMDD.bin (PYQuant/tools/ledger_dump.py로 읽는다)
  1분봉           <capture_directory>/ticks_*.bin 의 체결 레코드로 만든다 — 마감 백필은 아침 유니버스만 받고
                  애프터마켓을 덮지 않아서, 매매한 종목의 하루 전체를 가진 것은 캡처뿐이다.
  국면            logs/regime_history.jsonl
  오류·판정        scripts/parse_quant_log.py --full --json, scripts/check_runtime_health.py
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import html
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

REPO = Path(__file__).resolve().parent.parent
OUTPUT_DIR = REPO / "_private" / "daily_reports"
CHART_LIBRARY = OUTPUT_DIR / "lightweight-charts.js"
CHART_LIBRARY_URL = "https://unpkg.com/lightweight-charts@4.2.3/dist/lightweight-charts.standalone.production.js"
KST = dt.timezone(dt.timedelta(hours=9))

sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "PYQuant" / "tools"))

# 계좌마다 파일이 흩어진 자리. 경로는 config(capture_dir·ledger_journal_dir)와 감시견 로그 폴더를 따른다.
ACCOUNTS = [
    {"key": "live", "label": "실계좌", "config": "Quant/config/config_live.json",
     "log_directory": "Quant/build_win/logs_live"},
    {"key": "paper", "label": "모의계좌", "config": "Quant/config/config_dev_paper.json",
     "log_directory": "Quant/build_win/logs"},
]

# 체결 캡처 형식 — Quant/include/core/TickCapture.h와 한 벌(scripts/capture_stats.py와 같은 정의).
CAPTURE_MAGIC = b"QTCAP\x00"
CAPTURE_HEADER = struct.Struct("<6sBBq")
RECORD_HEAD = struct.Struct("<HBB")
TRADE_RECORD = struct.Struct("<qq12s8sIBB6xdqdq")
KIND_TRADE = 1

DB_TABLES = ["ticks", "signals", "orders", "fills", "health", "account_snapshots", "proc_stats"]


def read_config(relative: str) -> dict:
    path = REPO / relative

    if not path.exists():
        return {}

    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


# ─────────────── 주문·체결 ───────────────
def read_trades(log_directory: Path, date_yyyymmdd: str) -> list[dict]:
    path = log_directory / f"trades_{date_yyyymmdd}.csv"

    if not path.exists():
        return []

    with path.open(encoding="utf-8", errors="replace", newline="") as handle:
        return [row for row in csv.DictReader(handle) if row.get("event") and row["event"] != "event"]


def number(text: str, default: float = 0.0) -> float:
    try:
        return float(text) if text not in ("", None) else default
    except ValueError:
        return default


def epoch_of(text: str) -> int:
    """'YYYY-MM-DD HH:MM:SS'(KST) → 차트 시각. 차트는 UTC로 그리므로 KST 벽시계 그대로 보이게 9시간을 더한 값이다."""
    moment = dt.datetime.strptime(text, "%Y-%m-%d %H:%M:%S")
    return int(moment.replace(tzinfo=dt.timezone.utc).timestamp())


STOP_RE = re.compile(r"손절=-?([\d.]+)%")


def order_timeline(rows: list[dict], day_end: int) -> tuple[list[dict], list[dict]]:
    """지정가 주문이 걸려 있던 구간과 체결 표시점을 만든다.

    주문 번호(ORD-NNNNNN)는 엔진이 다시 뜰 때마다 1부터 다시 센다 — 같은 번호가 다시 접수되면 앞의 것은
    그 시각에 끝난 것으로 본다(재기동 때 엔진이 미체결을 정리한다).
    """
    open_orders: dict[str, dict] = {}
    spans: list[dict] = []
    marks: list[dict] = []

    def close(order_id: str, until: int, outcome: str) -> None:
        span = open_orders.pop(order_id, None)

        if span is not None:
            span["end"] = max(until, span["start"] + 60)
            span["outcome"] = outcome
            spans.append(span)

    for row in rows:
        event = row["event"]
        order_id = row.get("order_id", "")
        moment = epoch_of(row["ts_kst"])

        if event == "ACCEPTED" and row.get("type") == "LIMIT":
            close(order_id, moment, "재기동")
            open_orders[order_id] = {"ticker": row["ticker"], "side": row["side"],
                                     "price": number(row["order_price"]), "start": moment,
                                     "quantity": int(number(row["order_qty"])), "reason": row.get("entry_reason", "")}
        elif event == "FILL":
            marks.append({"ticker": row["ticker"], "side": row["side"], "time": moment,
                          "price": number(row["fill_price"]), "quantity": int(number(row["fill_qty"])),
                          "reason": row.get("entry_reason", ""), "strategy": row.get("strategy", "")})

            if row.get("status") == "FILLED":
                close(order_id, moment, "체결")
        elif event == "CANCELLED":
            close(order_id, moment, "취소")

    for order_id in list(open_orders):
        close(order_id, day_end, "미체결로 마감")

    return spans, marks


def stop_lines(marks: list[dict], day_end: int) -> list[dict]:
    """매수 체결의 진입 사유에 '손절=-X%'가 있으면 그 가격을 점선으로. 손절은 미리 거는 주문이 없고
    엔진이 조건으로 판정하므로 예약 주문 선이 아니라 기준선이다."""
    lines: dict[tuple[str, float], dict] = {}

    for mark in marks:
        if mark["side"] != "BUY":
            continue

        found = STOP_RE.search(mark["reason"])

        if found:
            # 같은 주문이 나눠 체결되면 같은 선이 여러 번 나온다 — 종목·가격이 같으면 처음 것만
            price = round(mark["price"] * (1 - float(found.group(1)) / 100), 2)
            lines.setdefault((mark["ticker"], price), {"ticker": mark["ticker"], "start": mark["time"],
                                                       "end": day_end, "price": price})

    return list(lines.values())


# ─────────────── 1분봉 (체결 캡처) ───────────────
def capture_files(capture_directory: Path, day: dt.date) -> list[Path]:
    """그날 시작한 캡처 파일. 헤더의 시작 시각(UTC ms)으로 고른다 — 이름의 숫자도 같은 값이다."""
    chosen = []

    for path in sorted(capture_directory.glob("ticks_*.bin")):
        try:
            with path.open("rb") as handle:
                header = handle.read(CAPTURE_HEADER.size)
        except OSError:
            continue

        if len(header) < CAPTURE_HEADER.size:
            continue

        magic, _version, _pad, start_ms = CAPTURE_HEADER.unpack(header)

        if magic == CAPTURE_MAGIC and dt.datetime.fromtimestamp(start_ms / 1000, KST).date() == day:
            chosen.append(path)

    return chosen


def minute_bars(paths: list[Path], tickers: set[str]) -> tuple[dict[str, list[dict]], dict]:
    """캡처의 체결 레코드를 종목별 1분봉으로. 그날 캡처의 종류별 레코드 수도 같이 센다(적재 현황 표)."""
    encoded = {ticker.encode(): ticker for ticker in tickers}
    bars: dict[str, dict[int, list]] = defaultdict(dict)
    kinds: Counter = Counter()
    total_bytes = 0

    for path in paths:
        data = path.read_bytes()
        total_bytes += len(data)
        offset = CAPTURE_HEADER.size
        end = len(data)

        while offset + RECORD_HEAD.size <= end:
            length, kind, _version = RECORD_HEAD.unpack_from(data, offset)
            body = offset + RECORD_HEAD.size

            if body + length > end:
                break

            kinds[kind] += 1

            if kind == KIND_TRADE and length >= TRADE_RECORD.size:
                ticker_bytes = data[body + 16:body + 28].split(b"\0", 1)[0]
                ticker = encoded.get(ticker_bytes)

                if ticker is not None:
                    _received, wall_us, _t, _time, _id, _market, _direction, price, quantity, _strength, _accumulated = \
                        TRADE_RECORD.unpack_from(data, body)

                    if price > 0:
                        # KST 벽시계 분 — 차트가 UTC로 그리므로 9시간을 더해 둔다
                        minute = (wall_us // 1_000_000 + 9 * 3600) // 60 * 60
                        bar = bars[ticker].get(minute)

                        if bar is None:
                            bars[ticker][minute] = [price, price, price, price, quantity]
                        else:
                            bar[1] = max(bar[1], price)
                            bar[2] = min(bar[2], price)
                            bar[3] = price
                            bar[4] += quantity

            offset = body + length

    result = {}

    for ticker, by_minute in bars.items():
        result[ticker] = [{"time": minute, "open": bar[0], "high": bar[1], "low": bar[2], "close": bar[3],
                           "volume": bar[4]} for minute, bar in sorted(by_minute.items())]

    names = {1: "체결", 2: "호가", 3: "봉", 4: "유니버스"}
    statistics = {"files": [path.name for path in paths], "bytes": total_bytes,
                  "kinds": {names.get(kind, str(kind)): count for kind, count in sorted(kinds.items())}}
    return result, statistics


def parquet_bars(ticker: str, date_yyyymmdd: str) -> list[dict]:
    """캡처에 없는 종목의 대체 — 마감 백필 1분봉(정규장만)."""
    path = REPO / "PYQuant" / "data" / "minute" / ticker / f"{date_yyyymmdd}.parquet"

    if not path.exists():
        return []

    try:
        import pandas as pd
        frame = pd.read_parquet(path)
    except Exception:
        return []

    # minute_backfill.py 형식: date(YYYYMMDD)·hms(HHMMSS) 문자열 + OHLCV
    if not all(name in frame.columns for name in ("date", "hms", "open", "high", "low", "close")):
        return []

    bars = []

    for row in frame.itertuples(index=False):
        moment = dt.datetime.strptime(f"{row.date}{row.hms}", "%Y%m%d%H%M%S").replace(tzinfo=dt.timezone.utc)
        bars.append({"time": int(moment.timestamp()), "open": float(row.open), "high": float(row.high),
                     "low": float(row.low), "close": float(row.close),
                     "volume": float(getattr(row, "volume", 0) or 0)})

    return bars


# ─────────────── 원장 바이너리 ───────────────
def ledger_summary(journal_directory: Path, date_yyyymmdd: str) -> dict:
    path = journal_directory / f"ledger_{date_yyyymmdd}.bin"

    if not path.exists():
        return {"path": path.relative_to(REPO).as_posix(), "exists": False}

    import ledger_dump

    try:
        result = ledger_dump.read_records(path)
    except Exception as error:
        return {"path": path.relative_to(REPO).as_posix(), "exists": True, "error": str(error)}

    records = result.records
    kinds = Counter(record.kind for record in records)
    fills = [record for record in records if record.kind == "FILL"]
    positions = ledger_dump.rebuild_positions(records)
    holdings = [{"ticker": ticker, "quantity": state["quantity"], "average": round(state["average"], 2)}
                for (_account, ticker), state in sorted(positions.items()) if state["quantity"] > 0]
    regimes = Counter(record.regime or "모름" for record in fills)

    return {"path": path.relative_to(REPO).as_posix(), "exists": True, "bytes": path.stat().st_size,
            "records": len(records), "truncated": result.truncated, "kinds": dict(kinds),
            "commission": round(sum(record.commission for record in fills)),
            "tax": round(sum(record.tax for record in fills)),
            "realized": round(sum(record.pnl for record in fills if record.side == "SELL")),
            "fill_regimes": dict(regimes), "holdings": holdings}


# ─────────────── 국면 ───────────────
def regime_series(iso: str) -> list[dict]:
    path = REPO / "logs" / "regime_history.jsonl"

    if not path.exists():
        return []

    series = []

    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.startswith('{"ts":"' + iso):
                continue

            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue

            moment = dt.datetime.fromisoformat(item["ts"]).replace(tzinfo=None)
            series.append({"time": int(moment.replace(tzinfo=dt.timezone.utc).timestamp()),
                           "risk_score": item.get("risk_score"), "entry_scale": item.get("entry_scale"),
                           "regime": item.get("regime"), "entry_halt": item.get("entry_halt"),
                           "pct": item.get("pct", {})})

    # 차트에는 1분에 한 점이면 충분하다(하루 3천 줄 넘게 쌓인다)
    thinned = {}

    for point in series:
        thinned[point["time"] // 60 * 60] = point | {"time": point["time"] // 60 * 60}

    return list(thinned.values())


# ─────────────── 오류·판정 ───────────────
def log_events(log_directory: Path, date_yyyymmdd: str) -> dict:
    environment = os.environ | {"QUANT_LOG_DIR": str(log_directory), "PYTHONIOENCODING": "utf-8"}

    try:
        completed = subprocess.run([sys.executable, str(REPO / "scripts" / "parse_quant_log.py"), "--full",
                                    "--date", date_yyyymmdd, "--json"], capture_output=True, text=True, encoding="utf-8",
                                   errors="replace", env=environment, cwd=REPO, timeout=300)
        return json.loads(completed.stdout)
    except Exception as error:
        return {"error": str(error)}


def health_rows(log_directory: Path, iso: str) -> list[dict]:
    try:
        import check_runtime_health
        rows, _sessions = check_runtime_health.collect(iso, log_directory, 0, include_global=False)
    except Exception as error:
        return [{"name": "점검 실행", "status": "FAIL", "detail": f"check_runtime_health 실패: {error}"}]

    return [{"name": name, "status": "PASS" if ok else level, "detail": detail} for name, ok, level, detail in rows]


def database_counts(day: dt.date) -> dict:
    try:
        from check_runtime_health import import_psycopg2, tsdb_password
        password = tsdb_password()
        psycopg2 = import_psycopg2() if password else None
    except Exception as error:
        return {"error": f"준비 실패: {error}"}

    if not password:
        return {"error": ".env에 TSDB_PASSWORD 없음"}

    if psycopg2 is None:
        return {"error": "psycopg2 없음"}

    start = dt.datetime.combine(day, dt.time(0, 0), KST)
    end = start + dt.timedelta(days=1)
    counts: dict = {}

    try:
        connection = psycopg2.connect(host="localhost", port=5432, dbname="quant", user="quant",
                                      password=password, connect_timeout=3)
    except Exception as error:
        return {"error": f"접속 실패: {error}"}

    try:
        connection.autocommit = True

        with connection.cursor() as cursor:
            cursor.execute("SET statement_timeout = 20000")

            for table in DB_TABLES:
                try:
                    cursor.execute(f"SELECT COUNT(*) FROM {table} WHERE ts >= %s AND ts < %s", (start, end))
                    counts[table] = cursor.fetchone()[0]
                except Exception as error:
                    counts[table] = f"조회 실패: {str(error).splitlines()[0]}"

            try:
                cursor.execute("SELECT account, COUNT(*) FROM ledger_events WHERE trade_date = %s GROUP BY account",
                               (day,))
                counts["ledger_events"] = {account or "(없음)": count for account, count in cursor.fetchall()}
            except Exception as error:
                counts["ledger_events"] = f"조회 실패: {str(error).splitlines()[0]}"

            try:
                cursor.execute("SELECT * FROM regime WHERE date = %s", (day,))
                row = cursor.fetchone()
                names = [column.name for column in cursor.description]
                counts["regime_row"] = {name: str(value) for name, value in zip(names, row)} if row else None
            except Exception as error:
                counts["regime_row"] = f"조회 실패: {str(error).splitlines()[0]}"
    finally:
        connection.close()

    return counts


def journal_docs(iso: str) -> list[dict]:
    docs = []

    for path in sorted(REPO.glob(f"strategies/*/live/{iso}.md")) + sorted(REPO.glob(f"docs/market_close/{iso}.md")):
        docs.append({"path": str(path.relative_to(REPO)).replace("\\", "/"), "bytes": path.stat().st_size,
                     "modified": dt.datetime.fromtimestamp(path.stat().st_mtime).strftime("%H:%M"),
                     "url": path.as_uri()})

    return docs


# ─────────────── 계좌 하나 ───────────────
def capture_directory_of(account: dict) -> Path:
    return REPO / read_config(account["config"]).get("capture_directory", "PYQuant/data/ticks_raw")


def traded_tickers(rows: list[dict]) -> list[str]:
    return sorted({row["ticker"] for row in rows if row.get("ticker") and row["event"] in ("FILL", "ACCEPTED")})


def collect_bars(day: dt.date, tickers: set[str]) -> tuple[dict[str, list[dict]], dict[str, dict]]:
    """두 계좌의 캡처를 다 읽어 종목마다 분이 더 많은 쪽을 쓴다. 실계좌 엔진은 구독 칸이 적어 매매한 종목을
    자기 캡처에 안 남기는 날이 있다(2026-10-07 실측: 한 종목만) — 모의 캡처가 같은 종목을 갖고 있으면 그것을 쓴다.
    둘 다 없으면 마감 백필 1분봉(정규장만)."""
    best: dict[str, list[dict]] = {}
    statistics: dict[str, dict] = {}

    for account in ACCOUNTS:
        capture_directory = capture_directory_of(account)
        bars, capture = minute_bars(capture_files(capture_directory, day), tickers)
        capture["dir"] = str(capture_directory.relative_to(REPO)).replace("\\", "/")
        statistics[account["key"]] = capture

        for ticker, series in bars.items():
            if len(series) > len(best.get(ticker, [])):
                best[ticker] = series

    date_yyyymmdd = day.strftime("%Y%m%d")
    missing = sorted(ticker for ticker in tickers
                     if not best.get(ticker) and not (REPO / "PYQuant" / "data" / "minute" / ticker / f"{date_yyyymmdd}.parquet").exists())

    if missing:
        # 캡처에도 백필에도 없는 종목은 KIS 분봉으로 그 자리에서 받는다(마감 백필과 같은 도구·같은 폴더)
        print(f"[daily_trade_report] 1분봉 없는 {len(missing)}종목을 KIS에서 받는다: {','.join(missing)}")
        subprocess.run([sys.executable, str(REPO / "PYQuant" / "tools" / "minute_backfill.py"), "--start", day.isoformat(),
                        "--end", day.isoformat(), "--tickers", ",".join(missing)], cwd=REPO, timeout=600,
                       capture_output=True)

    for ticker in tickers:
        if not best.get(ticker):
            best[ticker] = parquet_bars(ticker, date_yyyymmdd)

    return best, statistics


def build_account(account: dict, day: dt.date, bars: dict[str, list[dict]], capture: dict) -> dict:
    date_yyyymmdd = day.strftime("%Y%m%d")
    iso = day.isoformat()
    config = read_config(account["config"])
    log_directory = REPO / account["log_directory"]
    journal_directory = REPO / config.get("ledger_journal_dir", account["log_directory"])
    day_end = epoch_of(f"{iso} 20:00:00")

    rows = read_trades(log_directory, date_yyyymmdd)
    spans, marks = order_timeline(rows, day_end)
    stops = stop_lines(marks, day_end)
    tickers = traded_tickers(rows)

    fills = [row for row in rows if row["event"] == "FILL"]
    summary = {
        "accepted": sum(row["event"] == "ACCEPTED" for row in rows),
        "fills": len(fills),
        "buy_amount": round(sum(number(row["fill_qty"]) * number(row["fill_price"]) for row in fills
                                if row["side"] == "BUY")),
        "sell_amount": round(sum(number(row["fill_qty"]) * number(row["fill_price"]) for row in fills
                                 if row["side"] == "SELL")),
        "realized": round(sum(number(row.get("realized_pnl", "")) for row in fills)),
        "rejected": sum(row["event"] == "REJECTED" for row in rows),
        "cancelled": sum(row["event"] == "CANCELLED" for row in rows),
        "reject_reasons": Counter(re.sub(r"\(.*", "", row.get("reason", "")).strip() or "(사유 없음)"
                                  for row in rows if row["event"] == "REJECTED").most_common(),
    }

    table = [{"time": row["ts_kst"][11:], "event": row["event"], "strategy": row.get("strategy", ""),
              "ticker": row.get("ticker", ""), "side": row.get("side", ""), "type": row.get("type", ""),
              "quantity": row.get("fill_qty") if row["event"] == "FILL" else row.get("order_qty", ""),
              "price": row.get("fill_price") if row["event"] == "FILL" else row.get("order_price", ""),
              "status": row.get("status", ""), "reason": row.get("reason", ""),
              "entry_reason": row.get("entry_reason", ""), "realized": row.get("realized_pnl", "")}
             for row in rows if row["event"] != "RECONCILE"]

    charts = []

    for ticker in tickers:
        charts.append({"ticker": ticker, "bars": bars.get(ticker, []),
                       "marks": [mark for mark in marks if mark["ticker"] == ticker],
                       "spans": [span for span in spans if span["ticker"] == ticker],
                       "stops": [stop for stop in stops if stop["ticker"] == ticker]})

    # 체결이 있었던 종목을 앞에
    charts.sort(key=lambda chart: (-len(chart["marks"]), chart["ticker"]))

    return {"key": account["key"], "label": account["label"], "log_directory": account["log_directory"],
            "has_trades": bool(rows), "summary": summary, "table": table, "charts": charts,
            "ledger": ledger_summary(journal_directory, date_yyyymmdd), "capture": capture,
            "events": log_events(log_directory, date_yyyymmdd), "health": health_rows(log_directory, iso),
            "trades_file": f"{account['log_directory']}/trades_{date_yyyymmdd}.csv"}


# ─────────────── HTML ───────────────
def chart_script() -> str:
    if not CHART_LIBRARY.exists():
        try:
            import urllib.request
            CHART_LIBRARY.parent.mkdir(parents=True, exist_ok=True)
            urllib.request.urlretrieve(CHART_LIBRARY_URL, CHART_LIBRARY)
        except Exception:
            return f'<script src="{CHART_LIBRARY_URL}"></script>'

    return "<script>" + CHART_LIBRARY.read_text(encoding="utf-8") + "</script>"


def render(report: dict) -> str:
    template = (Path(__file__).resolve().parent / "daily_trade_report.html").read_text(encoding="utf-8")
    payload = json.dumps(report, ensure_ascii=False, default=str).replace("</", "<\\/")
    return (template.replace("{{TITLE}}", html.escape(f"매매 리포트 {report['date']}"))
                    .replace("{{CHART_LIBRARY}}", chart_script())
                    .replace("{{DATA}}", payload))


def write_index() -> None:
    reports = sorted(OUTPUT_DIR.glob("20??-??-??.html"), reverse=True)
    items = "\n".join(f'<li><a href="{path.name}">{path.stem}</a></li>' for path in reports)
    (OUTPUT_DIR / "index.html").write_text(
        "<!doctype html><meta charset='utf-8'><title>매매 리포트 목록</title>"
        "<style>body{font-family:system-ui,'Malgun Gothic',sans-serif;max-width:640px;margin:32px auto;padding:0 16px}"
        "li{margin:6px 0;font-size:16px}</style><h1>매매 리포트</h1><ul>" + items + "</ul>", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="하루 매매 리포트(모의·실계좌) HTML")
    parser.add_argument("--date", default=dt.date.today().isoformat(), help="YYYY-MM-DD 또는 YYYYMMDD")
    parser.add_argument("--open", action="store_true", help="만든 뒤 브라우저로 연다")
    arguments = parser.parse_args()

    text = arguments.date.replace("-", "")
    day = dt.date(int(text[:4]), int(text[4:6]), int(text[6:8]))

    report = {"date": day.isoformat(), "generated": dt.datetime.now().strftime("%Y-%m-%d %H:%M"),
              "regime": regime_series(day.isoformat()), "accounts": [], "database": database_counts(day),
              "docs": journal_docs(day.isoformat())}

    date_yyyymmdd = day.strftime("%Y%m%d")
    tickers = set()

    for account in ACCOUNTS:
        tickers.update(traded_tickers(read_trades(REPO / account["log_directory"], date_yyyymmdd)))

    print(f"[daily_trade_report] 체결 캡처에서 {len(tickers)}종목 1분봉을 만드는 중")
    bars, captures = collect_bars(day, tickers)

    for account in ACCOUNTS:
        print(f"[daily_trade_report] {account['label']} 모으는 중")
        report["accounts"].append(build_account(account, day, bars, captures[account["key"]]))

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    destination = OUTPUT_DIR / f"{day.isoformat()}.html"
    destination.write_text(render(report), encoding="utf-8")
    write_index()
    print(f"[daily_trade_report] 썼다: {destination}")

    if arguments.open:
        os.startfile(destination)  # noqa: 윈도우 전용 — 사람이 손으로 부를 때만

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
