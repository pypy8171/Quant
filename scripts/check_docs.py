#!/usr/bin/env python3
"""문서 드리프트 검사 — 색인/링크 정합을 결정론적으로 강제한다.

두 종류의 드리프트를 잡는다(둘 다 과거 실제로 발생):
  1. 깨진 내부 링크 — tracked .md의 상대 마크다운 링크가 실재(=git 추적) 파일을 가리키는가.
     (파일 이동/리네임 후 링크가 안 고쳐지는 사고, GitHub 404 예방)
  2. 색인 커버리지 — 새 스터디/전략 폴더가 색인에 등재됐는가.
     (07/08/09 스터디가 상위 색인에서 누락됐던 사고 예방)

git 추적 파일만 기준으로 삼는다 → GitHub/CI/다른 클론 어디서든 동일 판정(이식성).
사용:  python scripts/check_docs.py
종료코드: 0=통과, 1=드리프트 있음(항목별 출력). @committer가 커밋 전 호출한다.
"""
from __future__ import annotations

import posixpath
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# 콘솔 코드페이지와 무관하게 한글 출력이 깨지지 않도록 utf-8 고정.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# 인라인 마크다운 링크: [text](target)  — target에서 공백/제목 이후는 버린다.
LINK_RE = re.compile(r"\[[^\]]*\]\(\s*(<[^>]+>|[^)\s]+)")
FENCE_RE = re.compile(r"^```")
EXTERNAL_PREFIXES = ("http://", "https://", "mailto:", "tel:", "//")


def tracked_files() -> set[str]:
    out = subprocess.run(
        ["git", "-C", str(REPO), "ls-files"],
        capture_output=True, text=True, encoding="utf-8", check=True,
    ).stdout
    return {line.strip() for line in out.splitlines() if line.strip()}


def strip_code_fences(text: str) -> str:
    """```펜스 코드블록 내부를 제거해 예시 경로를 링크로 오탐하지 않는다."""
    lines, out, in_fence = text.splitlines(), [], False
    for ln in lines:
        if FENCE_RE.match(ln.strip()):
            in_fence = not in_fence
            out.append("")  # 줄번호 보존
            continue
        out.append("" if in_fence else ln)
    return "\n".join(out)


def is_dir_tracked(prefix: str, tracked: set[str]) -> bool:
    p = prefix.rstrip("/") + "/"
    return any(f.startswith(p) for f in tracked)


def check_links(tracked: set[str]) -> list[str]:
    problems: list[str] = []
    for rel in sorted(f for f in tracked if f.endswith(".md")):
        text = (REPO / rel).read_text(encoding="utf-8", errors="replace")
        base = posixpath.dirname(rel)
        for lineno, raw in enumerate(strip_code_fences(text).splitlines(), 1):
            for m in LINK_RE.finditer(raw):
                target = m.group(1).strip().strip("<>")
                if (not target or target.startswith("#")
                        or target.lower().startswith(EXTERNAL_PREFIXES)):
                    continue
                target = target.split("#", 1)[0].split("?", 1)[0]
                if not target:
                    continue  # 순수 앵커/쿼리
                joined = posixpath.normpath(posixpath.join(base, target))
                if target.endswith("/"):
                    if not is_dir_tracked(joined, tracked):
                        problems.append(f"{rel}:{lineno}  깨진 디렉터리 링크 → {target}")
                elif joined not in tracked and not is_dir_tracked(joined, tracked):
                    problems.append(f"{rel}:{lineno}  깨진 링크 → {target}")
    return problems


def check_coverage(tracked: set[str]) -> list[str]:
    problems: list[str] = []

    # 스터디: research/studies/NN_* 폴더는 전부 studies/README.md에 등재
    studies_index = REPO / "research/studies/README.md"
    if studies_index.exists():
        idx = studies_index.read_text(encoding="utf-8", errors="replace")
        dirs = sorted({
            re.match(r"(research/studies/\d\d_[^/]+)/", f).group(1)
            for f in tracked
            if re.match(r"research/studies/\d\d_[^/]+/", f)
        })
        for d in dirs:
            name = posixpath.basename(d)
            if name not in idx:
                problems.append(
                    f"research/studies/README.md  스터디 미등재 → {name} "
                    f"(폴더는 존재하나 색인에 없음)")

    # 전략: strategies/<X>/ 폴더는 전부 strategies/README.md에 등재
    strat_index = REPO / "strategies/README.md"
    if strat_index.exists():
        idx = strat_index.read_text(encoding="utf-8", errors="replace")
        dirs = sorted({
            f.split("/")[1] for f in tracked
            if f.startswith("strategies/") and "/" in f[len("strategies/"):]
        })
        for name in dirs:
            if name not in idx:
                problems.append(
                    f"strategies/README.md  전략 미등재 → {name} "
                    f"(폴더는 존재하나 색인에 없음)")
    return problems


def main() -> int:
    tracked = tracked_files()
    problems = check_links(tracked) + check_coverage(tracked)
    if not problems:
        print("문서 드리프트 검사 통과 — 깨진 내부 링크 0, 색인 커버리지 정합.")
        return 0
    print(f"문서 드리프트 {len(problems)}건 발견:\n")
    for p in problems:
        print(f"  - {p}")
    print("\n색인/링크를 고친 뒤 다시 실행하세요. (semantic 최신성은 docs/SYNC_MAP.md 체크리스트 참조)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
