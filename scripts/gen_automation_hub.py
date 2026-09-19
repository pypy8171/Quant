#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""자동화·대시보드를 한 파일에 모은다 → _private/AUTOMATION_HUB.md (gitignore, 통째로 생성물).

손으로 고치는 원본은 둘뿐이다:
  - 시간표: scripts/market_close_timetable.ps1 (모의·실계좌 두 갈래)
  - 대시보드·링크: _private/dashboards.json
나머지(예약작업 실제 상태·훅 배선)는 OS와 .claude/settings.json 에서 그때그때 읽는다.

부르는 곳: scripts/maintain.py --daily, scripts/gen_facts.py --apply(Stop 훅 sync-gate.ps1 이 gen 블록이 낡을 때 돈다).
손으로: py scripts/gen_automation_hub.py
"""
from __future__ import annotations

import csv
import io
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "_private" / "AUTOMATION_HUB.md"
sys.path.insert(0, str(ROOT / "scripts"))
import gen_facts  # noqa: E402  (시간표·훅·대시보드 읽기는 gen_facts 와 같은 함수를 쓴다)

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def scheduled_tasks() -> list[dict]:
    """Quant·claude_ 로 시작하는 예약작업의 실제 상태. Windows 가 아니거나 실패하면 빈 목록."""
    try:
        run = subprocess.run(["schtasks", "/query", "/fo", "csv", "/v"], capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return []
    # 콘솔 코드페이지가 셸마다 다르다(PowerShell cp949·Git Bash utf-8). utf-8 로 먼저 풀어 보고 안 되면 cp949.
    try:
        text = run.stdout.decode("utf-8")
    except UnicodeDecodeError:
        text = run.stdout.decode("cp949", errors="replace")
    rows = []
    header: list[str] | None = None
    for record in csv.reader(io.StringIO(text)):
        if not record:
            continue
        if record[0] in ("HostName", "호스트 이름"):
            header = record
            continue
        if header is None or len(record) != len(header):
            continue
        entry = dict(zip(header, record))
        name = entry.get("TaskName", entry.get("작업 이름", "")).lstrip("\\")
        if not (name.startswith("Quant") or name.startswith("claude_")):
            continue
        rows.append({
            "name": name,
            "state": entry.get("Scheduled Task State", entry.get("예약된 작업 상태", "")),
            "next": entry.get("Next Run Time", entry.get("다음 실행 시간", "")),
            "last": entry.get("Last Run Time", entry.get("마지막 실행 시간", "")),
            "result": entry.get("Last Result", entry.get("마지막 결과", "")),
            "start": entry.get("Start Time", entry.get("시작 시간", "")),
            "action": entry.get("Task To Run", entry.get("실행할 작업", "")),
        })
    rows.sort(key=lambda r: r["name"])
    return rows


def current_mode(tasks: list[dict]) -> str:
    """감시견 예약작업이 넘기는 config 의 kis.is_paper 로 지금 계좌 모드를 읽는다."""
    config_rel = "Quant/config/config_dev_paper.json"
    for task in tasks:
        if task["name"] == "QuantAutoTradeGuard":
            action = task["action"]
            marker = "-Config "
            if marker in action:
                config_rel = action.split(marker, 1)[1].split()[0].strip('"')
    config_path = Path(config_rel) if Path(config_rel).is_absolute() else ROOT / config_rel
    try:
        paper = bool(json.loads(config_path.read_text(encoding="utf-8"))["kis"]["is_paper"])
    except (OSError, ValueError, KeyError, TypeError):
        return f"모름 (config 못 읽음: {config_rel})"
    return f"{'모의' if paper else '실계좌'} ({config_rel})"


def to_hhmm(text: str) -> str:
    """schtasks 의 '오후 4:05:00' / '4:05:00 PM' / '16:05:00' 을 HH:MM 으로."""
    text = text.strip()
    if not text or text == "N/A":
        return ""
    afternoon = ("오후" in text) or text.upper().endswith("PM")
    morning = ("오전" in text) or text.upper().endswith("AM")
    digits = "".join(ch for ch in text if ch.isdigit() or ch == ":")
    parts = digits.split(":")
    try:
        hour, minute = int(parts[0]), int(parts[1])
    except (IndexError, ValueError):
        return text
    if afternoon and hour < 12:
        hour += 12
    if morning and hour == 12:
        hour = 0
    return f"{hour:02d}:{minute:02d}"


def current_plan(facts: dict, mode_text: str) -> list[dict]:
    """지금 계좌 모드의 시간표 행. 모드를 못 읽으면 모의로 본다."""
    key = "live" if mode_text.startswith("실계좌") else "paper"
    return (facts.get("market_close_timetable") or {}).get(key, [])


def render() -> str:
    facts = {"market_close_timetable": gen_facts.market_close_timetable(), "harness": gen_facts.harness()}
    tasks = scheduled_tasks()
    lines = [
        "# 자동화·대시보드 허브 (생성물 — 손으로 고치지 않는다)",
        "",
        f"만든 시각 {datetime.now():%Y-%m-%d %H:%M}. 다시 만들기 `py scripts/gen_automation_hub.py`. "
        "원본은 시간표 `scripts/market_close_timetable.ps1`, 링크 `_private/dashboards.json`. 각 자동화가 무엇을 만드는지·실패하면 어디를 보는지는 `docs/AUTOMATION.md`.",
        "",
        f"**지금 계좌 모드: {current_mode(tasks)}** — 모드를 바꾸면 `powershell -File scripts\\market_close_timetable.ps1 -Apply -Config <config>` 로 아래 시간표를 예약작업에 옮긴다.",
        "",
        "## 1. 마감 자동화 시간표 (평일, KST)",
        "",
        gen_facts.r_market_close_timetable(facts),
        "",
        "## 2. 예약작업 실제 등록 상태 (Windows 작업 스케줄러, 이 PC)",
        "",
    ]
    if tasks:
        planned = {row["name"]: row["at"] for row in current_plan(facts, current_mode(tasks))}
        lines += ["| 작업 이름 | 상태 | 등록 시각 | 시간표 대비 | 다음 실행 | 마지막 실행 | 마지막 결과 |", "|---|---|---|---|---|---|---|"]
        for task in tasks:
            flag = "" if task["result"] in ("0", "267009", "267011", "267014") else " ⚠"
            registered = to_hhmm(task["start"])
            expected = planned.get(task["name"])
            if task["name"] == "QuantAutoTradeGuard":
                verdict = "감시견(-Until·-Hours 는 `scripts/market_close_timetable.ps1` 검사가 본다)"
            elif expected is None:
                verdict = "⚠ 시간표에 없음 — `scripts/market_close_timetable.ps1` 에 넣거나 작업을 지운다"
            elif expected == registered:
                verdict = "일치"
            else:
                verdict = f"⚠ 예정 {expected} — `scripts/market_close_timetable.ps1 -Apply`"
            state = task["state"] if task["state"] == "Enabled" else f"⚠ {task['state']}"
            lines.append(f"| `{task['name']}` | {state} | {registered} | {verdict} | {task['next']} | {task['last']} | {task['result']}{flag} |")
        lines += ["", "마지막 결과 0=성공, 267009=실행 중, 267011=아직 안 돎, 267014=중단. 그 밖의 값은 `docs/AUTOMATION.md` 5절(실패하면 어디를 보나). "
                      "⚠ 가 하나라도 있으면 시간표·등록이 어긋난 것이다."]
    else:
        lines.append("(schtasks 를 못 읽었다 — Windows 에서만 채워진다)")
    lines += ["", "## 3. 세션 훅 (`.claude/settings.json` 배선)", "", gen_facts.r_hooks(facts),
              "", "훅이 하는 일은 `docs/AUTOMATION.md` 3절.",
              "", "## 4. 대시보드·발행본·루틴 링크", "", gen_facts.r_dashboards(facts),
              "", "발행본 재발행은 대화 세션에서 `/dashboard-sync`(헤드리스 예약작업은 HTML 만 만든다). "
                  "새로 발행하거나 URL 이 바뀌면 `_private/dashboards.json` 에 적는다 — 이 표와 `_private/LINKS.md` 의 표가 거기서 생성된다.", ""]
    return "\n".join(lines)


def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(render(), encoding="utf-8", newline="\n")
    print(f"[ok] wrote {OUT.relative_to(ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
