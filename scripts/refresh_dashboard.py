#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""매매일지나 백테스트가 바뀌면 리뷰 항목과 대시보드를 다시 만든다.

대시보드는 생성 시점에 데이터가 HTML 본문에 박히는 구조라, 원천이 바뀌어도
생성기를 다시 돌리기 전까지 화면은 예전 것을 보여준다. 이 절차를 아는 곳이
평일 16:05 예약작업 하나뿐이어서, 장중에 일지를 고치거나 스터디를 새로 넣으면
저녁까지 리뷰 탭이 뒤처졌다. 이 스크립트가 그 절차 한 벌을 소유하고,
`eod_autodoc.py`와 Claude 훅이 여기로 들어온다.

    py scripts/refresh_dashboard.py                    # 라이브(오늘) + 백테스트
    py scripts/refresh_dashboard.py --live 2026-09-07  # 그 날짜 리뷰만
    py scripts/refresh_dashboard.py --backtest         # 스터디 지표만
    py scripts/refresh_dashboard.py --render           # 생성기만
    py scripts/refresh_dashboard.py --dirty            # 대기표에 적힌 것만
    py scripts/refresh_dashboard.py --dry-run

`--if-stale`은 원천 파일의 수정시각을 `dashboard.html`과 비교해 뒤처진 것만
다시 만든다. 누가 어떤 경로로 고쳤는지(편집 도구·스크립트·다른 세션)를 따지지
않으므로 놓치는 경로가 없다. 재생성이 실패하면 낡은 상태가 남아 다음 호출에서
다시 시도한다.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import date as _date, datetime
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

REPO = Path(__file__).resolve().parents[1]
DASHBOARD = REPO / "research" / "dashboard" / "dashboard.html"
RUN_LOG = REPO / "logs" / "refresh_dashboard.log"

BACKFILL_LIVE = "PYQuant/dashboard/backfill_live.py"
REVIEW_ENTRY = "scripts/build_review_entry.py"
BACKFILL_STUDIES = "scripts/backfill_studies.py"
BUILD_DASHBOARD = "PYQuant/dashboard/build_dashboard.py"

_DATE = re.compile(r"(\d{4})-(\d{2})-(\d{2})")


def classify(path: str) -> dict:
    """저장소 상대경로 하나가 무엇을 낡게 만드는지 판정한다.

    훅(PowerShell)과 사람이 같은 규칙을 보게 하려고 여기에도 둔다. 판정 결과는
    {"live": [날짜…], "backtest": bool, "render": bool} 조각이다.
    """
    p = path.replace("\\", "/").lstrip("./")
    out: dict = {}
    m = _DATE.search(Path(p).stem)
    ymd = m.group(0) if m else None

    if re.fullmatch(r"strategies/[^/]+/live/\d{4}-\d{2}-\d{2}\.md", p) and ymd:
        out["live"] = [ymd]
    elif re.fullmatch(r"docs/eod/\d{4}-\d{2}-\d{2}\.md", p) and ymd:
        out["live"] = [ymd]
    elif p.startswith("research/studies/"):
        out["backtest"] = True
    elif p in ("research/dashboard/live.json", "research/dashboard/reviews.json"):
        out["render"] = True
    return out


def watched() -> list[Path]:
    """대시보드에 실리는 원천 파일들. 여기 없는 것이 바뀌어도 화면은 그대로다."""
    out: list[Path] = []
    out += sorted((REPO / "strategies").glob("*/live/*.md"))
    out += sorted((REPO / "docs" / "eod").glob("*.md"))
    out += sorted((REPO / "research" / "studies").rglob("metrics.json"))
    for n in ("live.json", "reviews.json"):
        f = REPO / "research" / "dashboard" / n
        if f.exists():
            out.append(f)
    return out


def stale() -> dict:
    """dashboard.html보다 새 원천이 있으면 무엇을 다시 만들지 돌려준다.

    수정시각으로 보는 이유는 편집 경로를 가리지 않기 위해서다. 편집 도구·스크립트·
    다른 세션 중 무엇이 고쳤든 같은 방식으로 걸린다.
    """
    try:
        cut = DASHBOARD.stat().st_mtime
    except OSError:
        cut = 0.0          # 산출물이 없으면 전부 낡은 것으로 본다
    live, backtest, render = set(), False, False
    for f in watched():
        try:
            if f.stat().st_mtime <= cut:
                continue
        except OSError:
            continue
        got = classify(f.relative_to(REPO).as_posix())
        live.update(got.get("live") or ())
        backtest = backtest or got.get("backtest", False)
        render = render or got.get("render", False)
    if not (live or backtest or render):
        return {}
    return {"live": sorted(live), "backtest": backtest, "render": render}


def _run(script: str, args: list[str], dry: bool, out: list[str]) -> int:
    if dry:
        out.append(("(dry) " + script + " " + " ".join(args)).rstrip())
        return 0
    r = subprocess.run([sys.executable, script, *args], cwd=REPO,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    tail = (r.stdout or r.stderr or "").strip().splitlines()
    out.append(f"{script} rc={r.returncode} {tail[-1] if tail else ''}".rstrip())
    return r.returncode


def refresh(live_dates: list[str], backtest: bool, render: bool, dry: bool) -> tuple[list[str], int]:
    """순서가 있다. 라이브 백필과 리뷰 항목을 채운 뒤에 생성기를 돌려야 화면에 실린다.

    생성기는 마지막에 한 번만 돈다 — 날짜가 여럿이어도 산출물은 HTML 한 장이다.
    """
    lines: list[str] = []
    rc = 0
    if live_dates:
        rc |= _run(BACKFILL_LIVE, [], dry, lines)
        for d in sorted(set(live_dates)):
            rc |= _run(REVIEW_ENTRY, ["--date", d], dry, lines)
    if backtest:
        rc |= _run(BACKFILL_STUDIES, [], dry, lines)
    if live_dates or backtest or render:
        rc |= _run(BUILD_DASHBOARD, [], dry, lines)
    else:
        lines.append("갱신할 것 없음")
    return lines, rc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--live", nargs="*", metavar="YYYY-MM-DD",
                    help="이 날짜들의 리뷰 항목을 갱신한다(값 없으면 오늘)")
    ap.add_argument("--backtest", action="store_true", help="스터디 지표를 다시 백필한다")
    ap.add_argument("--render", action="store_true", help="생성기만 돌린다")
    ap.add_argument("--if-stale", dest="if_stale", action="store_true",
                    help="dashboard.html보다 새 원천이 있을 때만 그 부분을 다시 만든다")
    ap.add_argument("--quiet", action="store_true", help="갱신할 것이 없으면 아무것도 출력하지 않는다")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    if a.if_stale:
        d = stale()
        if not d:
            if not a.quiet:
                print("[refresh_dashboard] 대시보드가 최신이다")
            return 0
        live, backtest, render = list(d.get("live") or ()), bool(d.get("backtest")), bool(d.get("render"))
    elif a.live is not None or a.backtest or a.render:
        live = (a.live or [_date.today().isoformat()]) if a.live is not None else []
        backtest, render = a.backtest, a.render
    else:
        live, backtest, render = [_date.today().isoformat()], True, True

    lines, rc = refresh(live, backtest, render, a.dry_run)
    head = (f"[{datetime.now():%Y-%m-%d %H:%M:%S}] refresh_dashboard "
            f"live={','.join(live) or '-'} backtest={backtest} render={render}")
    print("\n".join([head] + ["  " + l for l in lines]))

    if not a.dry_run:
        RUN_LOG.parent.mkdir(parents=True, exist_ok=True)
        with RUN_LOG.open("a", encoding="utf-8") as f:
            f.write("\n".join([head] + ["  " + l for l in lines]) + "\n")
    return 1 if rc else 0


if __name__ == "__main__":
    raise SystemExit(main())
