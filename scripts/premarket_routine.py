#!/usr/bin/env python3
"""장전 시황 브리핑 루틴 프롬프트 — 저장소 정본과 클라우드에 올린 것이 같은지 본다.

정본은 docs/premarket/ROUTINE_PROMPT.md 의 <!-- prompt-start --> ~ <!-- prompt-end --> 사이다.
루틴 갱신(RemoteTrigger)은 대화 세션만 할 수 있어 여기서는 올리지 않는다 — 올린 뒤 해시를 적어 두고,
그 뒤 정본이 바뀌면 낡음을 잡는 것까지가 이 스크립트의 일이다.

  py scripts/premarket_routine.py --render   본문을 그대로 출력(루틴에 붙여 넣을 텍스트)
  py scripts/premarket_routine.py --mark     지금 본문의 sha256 앞 16자리를 _private/dashboards.json 루틴 행 prompt_sha 에 적는다
  py scripts/premarket_routine.py --check    본문 해시와 prompt_sha 비교. 다르면 exit 1 (check_docs.py 가 부른다)

dashboards.json 이 없거나(다른 PC) 루틴 행이 없으면 --check 는 조용히 통과한다.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROMPT_DOC = ROOT / "docs" / "premarket" / "ROUTINE_PROMPT.md"
DASHBOARDS_JSON = ROOT / "_private" / "dashboards.json"
ROUTINE_NAME = "장전 시황 브리핑 routine"
START, END = "<!-- prompt-start -->", "<!-- prompt-end -->"

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def prompt_body() -> str:
    # 표식은 줄 하나를 통째로 차지하는 것만 센다 — 머리글이 표식을 인용하기 때문이다
    lines = PROMPT_DOC.read_text(encoding="utf-8-sig").splitlines()
    marks = [index for index, line in enumerate(lines) if line.strip() in (START, END)]
    if len(marks) != 2 or lines[marks[0]].strip() != START:
        raise SystemExit(f"{PROMPT_DOC.relative_to(ROOT)}: {START} / {END} 줄이 하나씩 있어야 한다")
    return "\n".join(lines[marks[0] + 1:marks[1]]).strip("\n") + "\n"


def prompt_sha(body: str) -> str:
    return hashlib.sha256(body.replace("\r\n", "\n").encode("utf-8")).hexdigest()[:16]


def routine_row() -> tuple[list | None, dict | None]:
    if not DASHBOARDS_JSON.exists():
        return None, None
    rows = json.loads(DASHBOARDS_JSON.read_text(encoding="utf-8"))
    for row in rows:
        if row.get("kind") == "routine" and row.get("name") == ROUTINE_NAME:
            return rows, row
    return rows, None


def main(argv: list[str]) -> int:
    body = prompt_body()
    sha = prompt_sha(body)
    if "--render" in argv:
        sys.stdout.write(body)
        return 0
    rows, row = routine_row()
    if "--mark" in argv:
        if row is None:
            print(f"{DASHBOARDS_JSON.relative_to(ROOT)}: '{ROUTINE_NAME}' 행이 없다")
            return 1
        row["prompt_sha"] = sha
        # 한 줄에 한 행 — 손으로 고치는 파일이라 원래 모양을 지킨다
        lines = ",\n".join("  " + json.dumps(r, ensure_ascii=False) for r in rows)
        DASHBOARDS_JSON.write_text("[\n" + lines + "\n]\n", encoding="utf-8", newline="\n")
        print(f"[ok] prompt_sha={sha} 기록")
        return 0
    # --check (기본)
    if row is None:
        return 0
    marked = row.get("prompt_sha")
    if marked == sha:
        return 0
    print(f"docs/premarket/ROUTINE_PROMPT.md: 루틴 프롬프트가 올린 것과 다르다(정본 {sha}, 올린 것 {marked or '없음'}) "
          f"→ /schedule 로 루틴을 갱신한 뒤 py scripts/premarket_routine.py --mark")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
