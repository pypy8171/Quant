#!/usr/bin/env python3
"""과거 trades_YYYYMMDD.csv 원장을 TimescaleDB fills 테이블에 적재한다.

레코더(PYQuant/main.py record)는 오늘(2026-09-15) 14시 30분경부터 떠 있었다 — 그 전에는
ZMQ 구독자가 없어서 CSV 원장에는 남았지만 DB엔 안 들어간 모의계좌(config_dev_paper.json,
50204275) 체결이 쌓여 있다. 날짜별 원장 선택은 scripts/_logdir.find_ledger 하나로 통일한다
(eod_collect.py와 같은 규칙 — 행 수 최대, 동률이면 mtime 최신 — 같은 날을 두고 집계가
갈리면 안 된다). 테스트 바이너리가 남긴 유령 행 필터도 그대로 가져왔다.

사용:
  py scripts/backfill_fills_db.py                  # trades_*.csv에서 발견되는 모든 날짜
  py scripts/backfill_fills_db.py --date 20260910   # 특정 날짜만
  py scripts/backfill_fills_db.py --dry-run         # 적재 없이 건수만 확인
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    _stream.reconfigure(encoding="utf-8")

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "PYQuant"))
import _logdir  # noqa: E402

KST = timezone(timedelta(hours=9))
ACCOUNT = "50204275"  # Quant/config/config_dev_paper.json account_no


def discover_dates() -> list[str]:
    seen = set()

    for log_dir in _logdir.candidate_dirs():
        if not log_dir.is_dir():
            continue

        for path in log_dir.glob("trades_*.csv"):
            m = re.search(r"trades_(\d{8})\.csv", path.name)

            if m:
                seen.add(m.group(1))

    return sorted(seen)


def load_rows(ymd: str) -> list[dict]:
    path = _logdir.find_ledger(ymd)

    if path is None:
        return []

    with path.open(encoding="utf-8-sig", errors="replace") as f:
        rows = list(csv.DictReader(f))

    rows = [r for r in rows if (r.get("event") or "").strip() == "FILL"]
    rows = [r for r in rows if (r.get("strategy") or "").strip() != "TEST"]
    rows = [r for r in rows
            if not ((r.get("ticker") or "").strip() == "047050"
                    and (r.get("odno") or "").strip() in ("R000777", "PREV-SESSION"))]

    return rows


def to_ts(ts_kst: str) -> datetime:
    return datetime.strptime(ts_kst, "%Y-%m-%d %H:%M:%S").replace(tzinfo=KST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="YYYYMMDD, 생략하면 발견되는 모든 날짜")
    ap.add_argument("--dry-run", action="store_true", help="적재 없이 건수만 출력")
    args = ap.parse_args()

    dates = [args.date] if args.date else discover_dates()

    from db.client import DbClient
    db = DbClient()

    total_inserted = 0
    total_skipped = 0

    for ymd in dates:
        rows = load_rows(ymd)

        if not rows:
            print(f"{ymd}: 원장 없음/체결 없음")
            continue

        # 오늘 날짜는 레코더가 이미 실시간으로 담고 있다 — 그 시작 시각 전까지만 백필한다.
        with db._conn.cursor() as cur:
            cur.execute(
                "SELECT MIN(ts) FROM fills WHERE account=%s AND ts::date=%s",
                (ACCOUNT, f"{ymd[:4]}-{ymd[4:6]}-{ymd[6:]}"),
            )
            cutoff = cur.fetchone()[0]

        inserted = 0
        skipped = 0

        for r in rows:
            ts = to_ts(r["ts_kst"])

            if cutoff is not None and ts >= cutoff:
                skipped += 1
                continue

            data = {
                "ts": ts,
                "odno": r.get("odno") or "",
                "ticker": r["ticker"],
                "side": r["side"],
                "filled_qty": int(r["fill_qty"]),
                "filled_price": float(r["fill_price"]),
                "strategy": r.get("strategy") or None,
                "account": ACCOUNT,
            }

            if not args.dry_run:
                db.insert_fill(data)

            inserted += 1

        suffix = f", 건너뜀(이미 실시간 적재) {skipped}건" if skipped else ""
        print(f"{ymd}: 적재 {inserted}건{suffix}")
        total_inserted += inserted
        total_skipped += skipped

    db.close()

    tag = " (dry-run)" if args.dry_run else ""
    print(f"합계: 적재 {total_inserted}건, 건너뜀 {total_skipped}건{tag}")


if __name__ == "__main__":
    main()
