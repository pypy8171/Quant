#!/usr/bin/env python3
"""실행 로그에서 미체결 주문을 복원해 open_orders.txt 보조 프로세스를 채운다.

평시에는 필요 없다 — OrderRouter가 접수·체결마다 보조 프로세스를 직접 쓰고, 기동 때
cancel_stale_orders()가 그것을 읽어 전부 취소한다. 이 스크립트는 두 경우의 복구용이다.

  1. 보조 프로세스가 없던 시절(2026-09-08 이전) 실행분의 유령 주문 정리
  2. 보조 프로세스가 유실됐는데 로그는 남아 있는 경우

로그 세 줄로 재구성한다.
  접수  [KIS] 주문 접수: <종목> <side> <수량>주  ODNO=<odno> ORGNO=<orgno>
  체결  [WS] 체결통보 ODNO=<odno> <종목> <side> <수량>주 @<가격>
  취소  [KIS] 취소 접수: <종목> 원ODNO=<odno> 취소ODNO=<신규>

남은 잔량(접수 - 체결)이 0보다 크고 취소된 적 없는 주문만 남긴다.
사용:  py scripts/seed_open_orders.py [--date YYYY-MM-DD] [--log <경로>] [--out <경로>]
"""
from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_LOG = REPO / "Quant" / "build_win" / "logs" / "quant_trader.log"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

ACCEPT_RE = re.compile(
    r"주문 접수: (\d{6}) (BUY|SELL) (\d+)주\s+ODNO=(\d+) ORGNO=(\d+)")
FILL_RE = re.compile(r"체결통보 ODNO=(\d+) (\d{6}) (BUY|SELL) (\d+)주")
CANCEL_RE = re.compile(r"취소 접수: (\d{6}) 원ODNO=(\d+)")
# 미연결 체결 — 재기동으로 라우터 기억이 지워진 뒤 들어온 체결. 원장에는 반영되지만
#  ODNO 매핑이 없어 예전 FILL_RE에 안 걸렸다. 이걸 못 세면 이미 다 채워진 주문이
#  계속 "미체결"로 남아 다음 기동이 또 취소를 시도한다.
ORPHAN_FILL_RE = re.compile(
    r"미매핑 체결 원장 반영 \[[^\]]*\] ODNO=(\d+) (\d{6}) (BUY|SELL) (\d+)주")
# 브로커가 "그 주문 없다"고 답한 기록. 취소 성공(취소 접수)만 종료로 보면, 이미 죽은
#  주문이 재기동마다 부활한다(2026-09-08: 9회 재기동에 51건까지 누적, 건당 왕복 2~3초를
#  취소 API에 쓰면서 EGW00201 초당한도와 WS 업그레이드 실패를 같이 불렀다).
DEAD_RE = re.compile(r"유령주문 취소 불가.*ODNO=(\d+)")
NOQTY_RE = re.compile(r"취소 거부: \d{6} ODNO=(\d+) — .*없습니다")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=dt.date.today().isoformat())
    ap.add_argument("--log", default=str(DEFAULT_LOG))
    ap.add_argument("--out", default="")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    log = Path(a.log)
    if not log.exists():
        print(f"로그 없음: {log}")
        return 1
    out = Path(a.out) if a.out else log.parent / "open_orders.txt"

    orders: dict[str, dict] = {}   # odno → {ticker, orgno, side, qty, filled}
    cancelled: set[str] = set()

    with log.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith(a.date):
                continue
            m = ACCEPT_RE.search(line)
            if m:
                tk, side, qty, odno, orgno = m.groups()
                orders[odno] = {"ticker": tk, "orgno": orgno, "side": side,
                                "qty": int(qty), "filled": 0}
                continue
            m = FILL_RE.search(line)
            if m:
                odno, _tk, _side, qty = m.groups()
                if odno in orders:
                    orders[odno]["filled"] += int(qty)
                continue
            m = ORPHAN_FILL_RE.search(line)
            if m:
                odno, _tk, _side, qty = m.groups()
                if odno in orders:
                    orders[odno]["filled"] += int(qty)
                continue
            m = CANCEL_RE.search(line)
            if m:
                cancelled.add(m.group(2))
                continue
            m = DEAD_RE.search(line) or NOQTY_RE.search(line)
            if m:
                cancelled.add(m.group(1))

    rows = []
    for odno, o in orders.items():
        if odno in cancelled:
            continue
        remain = o["qty"] - o["filled"]
        if remain > 0:
            rows.append(f'{odno}|{o["orgno"]}|{o["ticker"]}|{o["side"]}|{remain}')

    rows.sort()
    body = "\n".join(rows) + ("\n" if rows else "")
    buy = sum(1 for r in rows if "|BUY|" in r)
    print(f"{a.date}: 접수 {len(orders)}건, 종료확인 {len(cancelled)}건 "
          f"→ 미체결 {len(rows)}건 (매수 {buy}, 매도 {len(rows) - buy})")
    for r in rows:
        print("  " + r)

    if a.dry_run:
        print("(dry-run — 파일 미기록)")
        return 0
    out.write_text(body, encoding="utf-8", newline="\n")
    print(f"기록: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
