#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""라이브(모의) 매매 기록 → 대시보드용 정규화 요약 `research/dashboard/live.json`.

두 소스를 합친다(둘 다 이미 구조화/반구조화된 기록):
  1. `logs/trades_YYYYMMDD.csv` — 구조화 주문로그(ts·전략·종목·side·수량·가격·상태·사유).
     일자별로 상태·전략·종목을 롤업.
  2. `strategies/<전략>/live/YYYY-MM-DD.md` — 서술형 매매 일지. 제목·전략줄·한 줄 결과만
     뽑아 카드로 링크(원문은 md 그대로 — 대시보드는 요약+링크만).

정직성: 모의계좌(paper)·주문흐름 요약이다. 체결가·실현손익은 체결통보 원장(별도)에 있으므로
여기선 '무엇을 몇 번 발주/거부/취소했나'만 집계하고 손익을 지어내지 않는다.

실행:  python PYQuant/dashboard/backfill_live.py
"""
import csv
import json
import re
import sys
from collections import Counter
from pathlib import Path

# Windows 콘솔(cp949)에서 성공 print(✅ 등) 깨짐/크래시 방지
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[2]
# 엔진은 실행 시점 cwd 하위 logs/ 에 원장을 쓴다. 어디서 띄웠느냐에 따라 위치가 갈리므로
# 후보를 모두 훑고, 같은 날짜가 겹치면 이벤트가 많은 쪽(온전한 쪽)을 쓴다.
LOG_DIRS = [_REPO / "Quant" / "build_win" / "logs",
            _REPO / "Quant" / "logs",
            _REPO / "logs"]
STRAT = _REPO / "strategies"
OUT = _REPO / "research" / "dashboard" / "live.json"


def rel(p: Path) -> str:
    return str(p.relative_to(_REPO)).replace("\\", "/")


# ── 주문로그 롤업 ─────────────────────────────────────────────────────────────
def rollup_trades():
    best = {}
    for d in LOG_DIRS:
        if not d.is_dir():
            continue
        for p in sorted(d.glob("trades_*.csv")):
            m = re.search(r"trades_(\d{4})(\d{2})(\d{2})", p.name)
            date = f"{m.group(1)}-{m.group(2)}-{m.group(3)}" if m else p.stem
            row = _rollup_one(p, date)
            if row is None:
                continue
            if date not in best or row["total"] > best[date]["total"]:
                best[date] = row
    return [best[k] for k in sorted(best)]


def _rollup_one(p: Path, date: str):
    status, event, strat, tickers, sides = Counter(), Counter(), Counter(), set(), Counter()
    total = 0
    try:
        with open(p, encoding="utf-8-sig") as f:
            for r in csv.DictReader(f):
                total += 1
                status[(r.get("status") or "").strip()] += 1
                event[(r.get("event") or "").strip()] += 1
                strat[(r.get("strategy") or "").strip()] += 1
                sides[(r.get("side") or "").strip()] += 1
                t = (r.get("ticker") or "").strip()
                if t:
                    tickers.add(t)
    except Exception as e:
        print(f"  ! 스킵 {p}: {e}", file=sys.stderr)
        return None
    return {
        "date": date, "path": rel(p), "total": total,
        "by_status": dict(status.most_common()),
        "by_strategy": dict(strat.most_common()),
        "by_side": dict(sides.most_common()),
        "n_tickers": len(tickers),
        "accepted": status.get("ACCEPTED", 0),
        "rejected": status.get("REJECTED", 0),
        "cancelled": status.get("CANCELLED", 0),
        # filled = 최종 상태가 체결인 주문 수. fill_events = 체결통보 낱건 수(부분체결 포함).
        "filled": status.get("FILLED", 0) + status.get("EXECUTED", 0),
        "fill_events": event.get("FILL", 0) + event.get("FILLED", 0),
    }


# ── 매매 일지(md) 카드 ────────────────────────────────────────────────────────
_TITLE = re.compile(r"^#\s+(.*)")
_STRATLINE = re.compile(r"^>\s*전략\s*[:：]\s*(.*)")
_ONELINE_H = re.compile(r"##\s*\d+\..*한 줄")


def _clean(s: str) -> str:
    s = re.sub(r"\*\*(.+?)\*\*", r"\1", s)   # 볼드 제거
    return s.strip("> *").strip()


def scan_journals():
    cards = []
    for p in sorted(STRAT.glob("*/live/*.md")):
        strategy = p.parts[-3]                # strategies/<전략>/live/date.md
        date = p.stem
        title = strat_line = oneline = ""
        try:
            lines = p.read_text(encoding="utf-8").splitlines()
        except Exception as e:
            print(f"  ! 스킵 {p}: {e}", file=sys.stderr)
            continue
        for i, ln in enumerate(lines):
            if not title and (m := _TITLE.match(ln)):
                title = _clean(m.group(1))
            if not strat_line and (m := _STRATLINE.match(ln)):
                strat_line = _clean(m.group(1))
            if not oneline and _ONELINE_H.search(ln):
                # 다음 비어있지 않은 줄
                for nxt in lines[i + 1:i + 6]:
                    if nxt.strip():
                        oneline = _clean(nxt)
                        break
        cards.append({
            "date": date, "strategy": strategy, "path": rel(p),
            "title": title, "strat_line": strat_line, "summary": oneline,
        })
    cards.sort(key=lambda c: c["date"], reverse=True)
    return cards


def main():
    import datetime
    payload = {
        "schema": "quant.live/v1",
        "generated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "journals": scan_journals(),
        "order_log": rollup_trades(),
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"✅ 라이브 요약 저장: {rel(OUT)} "
          f"(일지 {len(payload['journals'])} · 주문로그일 {len(payload['order_log'])})")
    # 원장은 있는데 서술 일지가 없는 날 — /dashboard-sync 가 이 목록을 보고 일지를 채운다.
    have = {c["date"] for c in payload["journals"]}
    missing = sorted((o["date"] for o in payload["order_log"] if o["date"] not in have),
                     reverse=True)
    if missing:
        print("   일지 없는 매매일: " + ", ".join(missing))


if __name__ == "__main__":
    main()
