"""파일 색인 갱신·검사 — docs/FILE_INDEX.md 와 _private/FILE_INDEX.md 를 실제 트리와 맞춘다.

색인 자체가 설명의 정본이다. 기존 줄의 설명을 경로 키로 읽어 두고 트리를 다시 훑어 전체를 다시 쓴다.
새 파일은 `(설명 필요)` 자리표시자로 들어가고 없어진 파일은 빠진다. 날짜 파일·로그처럼 자동으로 생기는
파일은 아래 DEFAULTS 규칙이 설명을 채워 자리표시자를 만들지 않는다.

  py scripts/file_index.py             # 두 색인을 다시 쓴다. 자리표시자가 남으면 [new]/[todo] 줄과 exit 1
  py scripts/file_index.py --check     # 쓰지 않고 판정만. 트리와 색인이 어긋나면 exit 1
  py scripts/file_index.py --check --staged   # 커밋 게이트용 — 스테이징된 추가·삭제만 본다

Stop 훅 `.claude/hooks/file-index-gate.ps1` 이 턴 끝에 갱신을 돌리고, 커밋 게이트 `docs-gate.ps1` 이 --check --staged 로 막는다.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import OrderedDict
from pathlib import Path
from urllib.parse import quote, unquote

REPO = Path(__file__).resolve().parent.parent
PLACEHOLDER = "(설명 필요)"
PUB_INDEX = "docs/FILE_INDEX.md"
PRIV_INDEX = "_private/FILE_INDEX.md"
PRIV_ROOT = "_private"
SKIP_DIRS = {"__pycache__", ".git"}

# 자동으로 생기는 파일의 기본 설명. 색인에 이미 설명이 있으면 그쪽이 우선한다. {md}는 MM-DD.
DEFAULTS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^docs/eod/\d{4}-(?P<md>\d{2}-\d{2})\.md$"), "{md} 매매 사후검토"),
    (re.compile(r"^docs/premarket/\d{4}-(?P<md>\d{2}-\d{2})\.md$"), "{md} 장전 시황 브리핑"),
    (re.compile(r"^strategies/[^/]+/live/\d{4}-(?P<md>\d{2}-\d{2})\.md$"), "{md} 라이브 매매일지"),
    (re.compile(r"^_private/_intraday_issues/\d{4}-(?P<md>\d{2}-\d{2})\.md$"), "{md} 장중 이슈 대장"),
    (re.compile(r"^_private/_cron/\d{4}-(?P<md>\d{2}-\d{2})_\d{4}_task\.md$"), "{md} 예약작업 지시서"),
    (re.compile(r"^_private/_cron/\d{4}-(?P<md>\d{2}-\d{2})_\d{4}_report\.md$"), "{md} 예약작업 실행 리포트"),
    (re.compile(r"^_private/_cron/.*\.(log|err|pid|out)$"), "예약작업 실행 로그(자동 생성)"),
    (re.compile(r"^_private/TRIAGE_\d{4}-(?P<md>\d{2}-\d{2})\.md$"), "{md} 교통정리 판정 보고서"),
    (re.compile(r"^_private/archive/SESSION_CLAIMS_\d{4}-(?P<md>\d{2}-\d{2}).*\.md$"), "{md} 세션 현황판 보관본"),
    (re.compile(r"^_private/archive/handoff/.*\.md$"), "세션 인계 문서 보관본"),
    (re.compile(r"^_private/archive/orphans/.*\.patch$"), "주인 없는 worktree 편집분 패치 보관"),
    (re.compile(r"^_private/주식_study/\d{4}-(?P<md>\d{2}-\d{2})\.md$"), "{md} 주식 스터디 저널"),
    (re.compile(r"^_private/주식_study/\d{4}-(?P<md>\d{2}-\d{2})_재무/README\.md$"), "{md} 재무·사업 분석 종목 색인"),
    (re.compile(r"^_private/주식_study/\d{4}-\d{2}-\d{2}_재무/(?P<name>[^/_]+)_\d{6}\.md$"), "{name} 재무·사업 분석 리포트"),
    (re.compile(r"\.(log|out|err)$"), "실행 로그(자동 생성)"),
    (re.compile(r"\.pid$"), "프로세스 id 파일(자동 생성)"),
]

ENTRY_RE = re.compile(r"^- \[[^\]]+\]\(([^)#][^)]*)\) — (.*)$")  # 목차의 #앵커 줄은 제외


# ── 트리 ─────────────────────────────────────────────────────────────────

def git_lines(*args: str) -> list[str]:
    out = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", check=False).stdout
    return [l.strip() for l in out.splitlines() if l.strip()]


def public_files() -> list[str]:
    # 추적 파일 + 아직 add 하지 않은 새 파일(ignore 제외). 작업 트리에서 지운 추적 파일은 뺀다.
    files = set(git_lines("ls-files", "--cached", "--others", "--exclude-standard"))
    files -= set(git_lines("ls-files", "--deleted"))
    files.add(PUB_INDEX)
    return sorted(files)


def private_files() -> list[str]:
    root = REPO / PRIV_ROOT
    if not root.is_dir():
        return []
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for f in filenames:
            if f.endswith(".pyc"):
                continue
            files.append(Path(dirpath, f).relative_to(REPO).as_posix())
    files.append(PRIV_INDEX)
    return sorted(set(files))


# ── 색인 읽기·쓰기 ─────────────────────────────────────────────────────────

def read_descriptions(index_rel: str) -> dict[str, str]:
    p = REPO / index_rel
    if not p.exists():
        return {}
    base = Path(index_rel).parent
    found: dict[str, str] = {}
    for line in p.read_text(encoding="utf-8").splitlines():
        m = ENTRY_RE.match(line)
        if not m:
            continue
        path = (base / unquote(m.group(1))).as_posix()
        path = os.path.normpath(path).replace("\\", "/")
        found[path] = m.group(2).strip()
    return found


def default_description(path: str) -> str | None:
    for pat, tmpl in DEFAULTS:
        m = pat.search(path)
        if m:
            return tmpl.format(**{k: v for k, v in m.groupdict().items() if v})
    return None


def resolve(files: list[str], known: dict[str, str], self_desc: dict[str, str]) -> OrderedDict[str, str]:
    out: OrderedDict[str, str] = OrderedDict()
    for f in files:
        if f in self_desc:
            out[f] = self_desc[f]
        elif known.get(f) and known[f] != PLACEHOLDER:
            out[f] = known[f]
        else:
            out[f] = default_description(f) or PLACEHOLDER
    return out


def link(path: str, index_rel: str) -> str:
    rel = os.path.relpath(path, Path(index_rel).parent).replace("\\", "/")
    return quote(rel, safe="/-_.~")


def render(rows: OrderedDict[str, str], index_rel: str, title: str, intro: str) -> str:
    groups: OrderedDict[str, list[str]] = OrderedDict()
    for p in sorted(rows, key=lambda s: (str(Path(s).parent), s)):
        groups.setdefault(Path(p).parent.as_posix(), []).append(p)
    tops: OrderedDict[str, list[str]] = OrderedDict()
    for g in groups:
        top = g.split("/")[0] if g != "." else "(루트)"
        tops.setdefault(top, []).append(g)
    out = [f"# {title}", "", intro, "", "## 목차", ""]
    for top, gs in tops.items():
        n = sum(len(groups[g]) for g in gs)
        anchor = top.strip("()").lower().replace("/", "").replace(".", "")
        out.append(f"- [{top}](#{anchor}) — {n}개")
    out.append("")
    for top, gs in tops.items():
        out += [f"## {top}", ""]
        for g in gs:
            if len(gs) > 1 or g != ".":
                out += [f"### {g}/" if g != "." else "### 루트", ""]
            for p in groups[g]:
                out.append(f"- [{Path(p).name}]({link(p, index_rel)}) — {rows[p]}")
            out.append("")
    return "\n".join(out) + "\n"


INTRO_PUB = (
    "저장소의 추적 파일 전부를 폴더별로 한 줄씩 적은 색인이다. 찾을 때는 Ctrl+F로 파일명이나 낱말을 검색한다. "
    "링크는 이 문서 기준 상대 경로다. 개인 파일(`_private/`)은 `_private/FILE_INDEX.md`에 따로 있다.\n\n"
    "이 문서는 `py scripts/file_index.py`가 다시 쓴다 — 설명은 이 문서의 줄이 정본이고, 새 파일은 `(설명 필요)`로 들어오니 "
    "그 자리에서 채운다. 턴 끝 Stop 훅과 커밋 게이트가 빠진 파일·남은 자리표시자를 잡는다."
)
INTRO_PRIV = (
    "`_private/` 아래 파일 전부를 폴더별로 한 줄씩 적은 색인이다(gitignore, 커밋 대상 아님). "
    "찾을 때는 Ctrl+F로 파일명이나 낱말을 검색한다. 링크는 이 문서 기준 상대 경로다. "
    "저장소 추적 파일은 `docs/FILE_INDEX.md`에 있다.\n\n"
    "이 문서는 `py scripts/file_index.py`가 다시 쓴다 — 설명은 이 문서의 줄이 정본이고, 새 파일은 `(설명 필요)`로 들어온다."
)
SELF = {
    PUB_INDEX: "이 파일 — 저장소 전체 파일 한 줄 색인(`scripts/file_index.py`가 생성)",
    PRIV_INDEX: "이 파일 — _private 파일 한 줄 색인(`scripts/file_index.py`가 생성)",
}


# ── 판정 ─────────────────────────────────────────────────────────────────

def diff_report(files: list[str], known: dict[str, str], rows: OrderedDict[str, str]) -> list[str]:
    report = []
    for f in files:
        if f not in known:
            report.append(f"[new] {f}" if rows[f] == PLACEHOLDER else f"[auto] {f} — {rows[f]}")
        elif rows[f] == PLACEHOLDER:
            report.append(f"[todo] {f}")
    for f in known:
        if f not in rows:
            report.append(f"[gone] {f}")
    return report


def staged_report(known_pub: dict[str, str]) -> list[str]:
    report = []
    for f in git_lines("diff", "--cached", "--name-only", "--diff-filter=A"):
        if f == PUB_INDEX:
            continue
        if f not in known_pub:
            report.append(f"[new] {f}")
        elif known_pub[f] == PLACEHOLDER:
            report.append(f"[todo] {f}")
    for f in git_lines("diff", "--cached", "--name-only", "--diff-filter=D"):
        if f in known_pub:
            report.append(f"[gone] {f}")
    if git_lines("diff", "--name-only", "--", PUB_INDEX):
        report.append(f"[unstaged] {PUB_INDEX} — 작업 트리 변경이 스테이징되지 않았다")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="쓰지 않고 판정만")
    ap.add_argument("--staged", action="store_true", help="--check 와 함께: 스테이징된 추가·삭제만 본다(공개 색인)")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    known_pub = read_descriptions(PUB_INDEX)
    if args.check and args.staged:
        rep = staged_report(known_pub)
        print("\n".join(rep) if rep else "파일 색인 정합 — 스테이징된 추가·삭제가 색인과 맞는다.")
        return 1 if rep else 0

    known_priv = read_descriptions(PRIV_INDEX)
    pub_files, priv_files = public_files(), private_files()
    rows_pub = resolve(pub_files, known_pub, SELF)
    rows_priv = resolve(priv_files, known_priv, SELF)
    rep = diff_report(pub_files, known_pub, rows_pub) + diff_report(priv_files, known_priv, rows_priv)

    if not args.check:
        # LF 고정 — 기본값(CRLF)으로 쓰면 커밋된 LF 블롭과 어긋나 재생성마다 git status 에 M 이 뜬다.
        (REPO / PUB_INDEX).write_text(render(rows_pub, PUB_INDEX, "파일 색인", INTRO_PUB), encoding="utf-8", newline="\n")
        if (REPO / PRIV_ROOT).is_dir():
            (REPO / PRIV_INDEX).write_text(render(rows_priv, PRIV_INDEX, "_private 파일 색인", INTRO_PRIV), encoding="utf-8", newline="\n")

    blocking = rep if args.check else [r for r in rep if r.startswith(("[new]", "[todo]"))]
    if rep:
        print("\n".join(rep))
    else:
        print(f"파일 색인 정합 — 공개 {len(rows_pub)}개, 개인 {len(rows_priv)}개.")
    return 1 if blocking else 0


if __name__ == "__main__":
    sys.exit(main())
