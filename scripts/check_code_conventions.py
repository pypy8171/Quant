#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""코드 작업 규약을 스테이징된 변경에 대해 기계적으로 검사한다.

정본은 docs/guides/MAINTENANCE_AUTOMATION.md 4절(주석 규약)과 .clang-format(중괄호)이다.
이 스크립트는 그중 사람이 놓치기 쉬운 네 가지만 본다.

  1. 중괄호   — 스테이징된 Quant/**.h/.cpp에 brace_style.py --check
  2. D-NNN    — 추가된 줄이 가리키는 결정 번호가 docs/DECISIONS.md에 실재하는지
  3. 태그 철자 — `// [xxx]` 중 규약에 없는 태그(경고만)
  4. 줄 성격  — 파일별 추가·삭제를 주석과 코드로 나눠 보고.
                --comment-only를 주면 코드 줄 변경이 0이 아닐 때 실패한다.

주석 밀도는 검사하지 않는다. 4절이 밀도를 게이트로 걸지 말라고 정해 두었고,
집계는 maintain.py --weekly가 리포트로 남긴다.

출력은 `파일:줄: 종류: 내용`. exit 0 = 통과, 1 = 위반.

사용:
    py scripts/check_code_conventions.py                # 스테이징된 변경
    py scripts/check_code_conventions.py --comment-only # 주석 전용 커밋임을 검증
    py scripts/check_code_conventions.py --worktree     # 스테이징 대신 워킹트리
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CPP_EXT = {".h", ".hpp", ".cpp", ".cc"}
HASH_EXT = {".py", ".ps1"}
ALLOWED_TAGS = {"inv", "lock-order", "wire", "formula"}
WHY_TAG_RE = re.compile(r"^why D-\d{3}$")
TAG_RE = re.compile(r"//\s*\[([A-Za-z][A-Za-z0-9 _./-]*)\]")
DNNN_RE = re.compile(r"\bD-(\d{3})\b")

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass


def git(*args: str) -> str:
    p = subprocess.run(["git", *args], cwd=ROOT, capture_output=True)
    return p.stdout.decode("utf-8", "replace")


def added_lines(staged: bool) -> dict[str, list[tuple[int, str]]]:
    """diff에서 추가된 줄을 파일별로 (새 파일 기준 줄번호, 내용)으로 모은다."""
    args = ["diff", "-U0"] + (["--cached"] if staged else [])
    out = git(*args)
    result: dict[str, list[tuple[int, str]]] = {}
    path = ""
    lineno = 0
    for raw in out.splitlines():
        if raw.startswith("+++ "):
            p = raw[4:].strip()
            path = "" if p == "/dev/null" else p[2:] if p.startswith("b/") else p
            continue
        if raw.startswith("@@"):
            m = re.search(r"\+(\d+)", raw)
            lineno = int(m.group(1)) if m else 0
            continue
        if not path:
            continue
        if raw.startswith("+"):
            result.setdefault(path, []).append((lineno, raw[1:]))
            lineno += 1
    return result


def removed_lines(staged: bool) -> dict[str, list[str]]:
    args = ["diff", "-U0"] + (["--cached"] if staged else [])
    out = git(*args)
    result: dict[str, list[str]] = {}
    path = ""
    for raw in out.splitlines():
        if raw.startswith("--- "):
            continue
        if raw.startswith("+++ "):
            p = raw[4:].strip()
            path = "" if p == "/dev/null" else p[2:] if p.startswith("b/") else p
            continue
        if path and raw.startswith("-") and not raw.startswith("---"):
            result.setdefault(path, []).append(raw[1:])
    return result


def is_comment(path: str, line: str) -> bool | None:
    """주석이면 True, 코드면 False, 빈 줄이면 None."""
    s = line.strip()
    if not s:
        return None
    ext = Path(path).suffix.lower()
    if ext in CPP_EXT:
        return s.startswith(("//", "/*", "*/", "*"))
    if ext in HASH_EXT:
        return s.startswith("#")
    return None


def known_decisions() -> set[str]:
    f = ROOT / "docs" / "DECISIONS.md"
    if not f.exists():
        return set()
    text = f.read_text(encoding="utf-8", errors="replace")
    return set(re.findall(r"^#+\s*(D-\d{3})", text, re.M))


def staged_files(staged: bool) -> list[str]:
    args = ["diff", "--name-only"] + (["--cached"] if staged else [])
    return [x for x in git(*args).splitlines() if x.strip()]


def main(argv: list[str]) -> int:
    staged = "--worktree" not in argv
    comment_only = "--comment-only" in argv

    adds = added_lines(staged)
    dels = removed_lines(staged)
    files = staged_files(staged)
    problems = 0

    # 1. 중괄호 — 대상 파일이 있을 때만 부른다
    targets = [f for f in files if Path(f).suffix.lower() in CPP_EXT and f.startswith("Quant/")]
    targets = [f for f in targets if (ROOT / f).exists()]
    if targets:
        p = subprocess.run([sys.executable, str(ROOT / "scripts" / "brace_style.py"), "--check", *targets],
                           cwd=ROOT, capture_output=True)
        if p.returncode != 0:
            for line in p.stdout.decode("utf-8", "replace").splitlines():
                if line.startswith("would change"):
                    print(f"{line.split(': ',1)[-1]}:0: 오류: 중괄호 규칙 위반 — py scripts/brace_style.py 로 정리")
                    problems += 1

    # 2. D-NNN 참조 실재 여부
    known = known_decisions()
    for path, items in adds.items():
        for ln, text in items:
            for num in DNNN_RE.findall(text):
                if f"D-{num}" not in known:
                    print(f"{path}:{ln}: 오류: D-{num}이 docs/DECISIONS.md에 없다")
                    problems += 1

    # 3. 태그 철자 — 오탐이 있을 수 있어 경고로만 남긴다
    for path, items in adds.items():
        if Path(path).suffix.lower() not in CPP_EXT:
            continue
        for ln, text in items:
            if is_comment(path, text) is not True:
                continue
            for tag in TAG_RE.findall(text):
                t = tag.strip()
                if t in ALLOWED_TAGS or WHY_TAG_RE.match(t):
                    continue
                print(f"{path}:{ln}: 경고: 규약에 없는 태그 [{t}] "
                      f"(허용: inv, lock-order, wire, formula, why D-NNN)")

    # 4. 줄 성격 집계
    rows = []
    code_touch = 0
    for path in sorted(set(list(adds) + list(dels))):
        if Path(path).suffix.lower() not in (CPP_EXT | HASH_EXT):
            continue
        ca = sum(1 for _, t in adds.get(path, []) if is_comment(path, t) is True)
        xa = sum(1 for _, t in adds.get(path, []) if is_comment(path, t) is False)
        cd = sum(1 for t in dels.get(path, []) if is_comment(path, t) is True)
        xd = sum(1 for t in dels.get(path, []) if is_comment(path, t) is False)
        rows.append((path, ca, cd, xa, xd))
        code_touch += xa + xd

    if rows:
        print("줄 성격 (주석 +/- · 코드 +/-)")
        for path, ca, cd, xa, xd in rows:
            print(f"  {path}: 주석 +{ca}/-{cd} · 코드 +{xa}/-{xd}")

    if comment_only and code_touch:
        print(f"오류: 주석 전용 커밋인데 코드 줄이 {code_touch}줄 바뀌었다 — 기능 수정은 별도 커밋으로 나눈다")
        problems += 1

    print(f"위반 {problems}건")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
