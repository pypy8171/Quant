#!/usr/bin/env python3
"""세션 교통정리 — 여러 코드 세션이 하루 동안 남긴 상태를 한 표로 모은다.

한 세션이 하루 끝(또는 머지 큐가 길어졌을 때) 이 스크립트를 돌려 아래를 한 번에 본다.
  1. main — origin 대비 미푸시, 메인 트리 미커밋(생성기 산출물은 따로 표시)
  2. worktree — 브랜치·HEAD 나이·main 대비 앞뒤·미커밋. 주인 없음(detached·오래됨) 표시
  3. 브랜치 — main에 들어간 것(삭제 가능)·worktree 없는 미머지 브랜치(주인 없는)
  4. 현황판 `_private/SESSION_CLAIMS.md` — 완료 행/진행 행, 살아 있는 세션(--live) 대비 죽은 세션·미등록 세션,
     D-NNN 예약 대비 docs/DECISIONS.md 착지 여부
  5. 인계 파일 `_private/HANDOFF_*.md` — 나이, 보관 후보
  6. 배포 exe — `Quant/build_win/quant_trader.exe` 시각 뒤에 쌓인 C++ 커밋 수

판정은 이 스크립트가 하고, 남의 줄을 고치거나 트리를 지우는 일은 하지 않는다.
행동 옵션은 셋뿐이고 전부 되돌릴 수 있다:
  --prune-branches      main에 들어갔고 worktree가 없는 브랜치를 `git branch -d`
  --archive-handoffs    --handoff-days보다 오래된 인계 파일을 _private/archive/handoff/로 옮긴다
  --orphan-patch PATH   주인 없는 worktree의 미커밋 변경을 _private/archive/orphans/에 패치로 남긴다(제거는 손으로)

사용:
    py scripts/session_triage.py --live quant-5a,quant-87          # 보고서 stdout
    py scripts/session_triage.py --live ... --out _private/TRIAGE_2026-09-13.md
    py scripts/session_triage.py --prune-branches --archive-handoffs
절차 정본은 CLAUDE.md "다중 세션" 절과 .claude/commands/triage.md.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PRIVATE = REPO / "_private"
BOARD = PRIVATE / "SESSION_CLAIMS.md"
DECISIONS = REPO / "docs" / "DECISIONS.md"
EXE = REPO / "Quant" / "build_win" / "quant_trader.exe"
GENERATED = ("docs/facts.json", "docs/CODE_GRAPH.md")
CPP_PATHS = ("Quant/include", "Quant/src", "Quant/tests", "Quant/CMakeLists.txt")

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def git(*args: str, cwd: Path = REPO, check: bool = False) -> str:
    r = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def git_ok(*args: str, cwd: Path = REPO) -> bool:
    return subprocess.run(["git", *args], cwd=str(cwd), capture_output=True).returncode == 0


def count(*args: str, cwd: Path = REPO) -> int:
    out = git("rev-list", "--count", *args, cwd=cwd)
    return int(out) if out.isdigit() else -1


def commit_time(ref: str, cwd: Path = REPO) -> dt.datetime | None:
    out = git("log", "-1", "--format=%cI", ref, cwd=cwd)
    try:
        return dt.datetime.fromisoformat(out).astimezone()
    except ValueError:
        return None


def age_days(t: dt.datetime | None, now: dt.datetime) -> float:
    return (now - t).total_seconds() / 86400 if t else 0.0


# ── 1. main ──────────────────────────────────────────────────────────────────

def section_main(now: dt.datetime) -> tuple[list[str], list[str]]:
    lines, actions = [], []
    head = git("rev-parse", "--short", "main")
    has_origin = git_ok("rev-parse", "--verify", "origin/main")
    lines.append(f"- main `{head}` ({commit_time('main').strftime('%m-%d %H:%M') if commit_time('main') else '?'})")
    if has_origin:
        ahead, behind = count("origin/main..main"), count("main..origin/main")
        lines.append(f"- origin/main 대비 앞 {ahead} · 뒤 {behind}")
        if ahead:
            for l in git("log", "--oneline", "origin/main..main").splitlines():
                lines.append(f"  - {l}")
            actions.append(f"미푸시 {ahead}건 — 푸시는 사용자가 말할 때만")
        if behind:
            actions.append(f"origin/main이 {behind}건 앞선다 — 머지 큐 진행 전 `git pull --ff-only`")
    status = git("status", "--porcelain").splitlines()
    if status:
        lines.append(f"- 메인 트리 미커밋 {len(status)}건:")
        for s in status:
            path = s[3:].strip()
            tag = " (생성기 산출물 — `py scripts/gen_facts.py --apply` 뒤 docs(facts) 커밋)" if path in GENERATED else ""
            lines.append(f"  - `{s[:2]}` {path}{tag}")
        gen = [s[3:].strip() for s in status if s[3:].strip() in GENERATED]
        other = [s for s in status if s[3:].strip() not in GENERATED]
        if gen:
            actions.append(f"생성기 산출물 미커밋: {', '.join(gen)} — 마지막 머지 뒤 재생성해 한 커밋으로")
        if other:
            actions.append(f"메인 트리에 코드/문서 미커밋 {len(other)}건 — 소유 세션을 현황판에서 찾는다(없으면 주인 없는)")
    else:
        lines.append("- 메인 트리 미커밋 없음")
    return lines, actions


# ── 2. worktree ──────────────────────────────────────────────────────────────

def worktrees() -> list[dict]:
    out = git("worktree", "list", "--porcelain")
    items, cur = [], {}
    for line in out.splitlines() + [""]:
        if not line:
            if cur:
                items.append(cur)
            cur = {}
            continue
        k, _, v = line.partition(" ")
        cur[k] = v if v else True
    return items


def section_worktrees(now: dt.datetime, stale_days: float) -> tuple[list[str], list[str], list[dict]]:
    lines, actions, info = [], [], []
    lines.append("| 경로 | 브랜치 | HEAD | main 앞/뒤 | 미커밋 | 판정 |")
    lines.append("|---|---|---|---|---|---|")
    for wt in worktrees():
        path = Path(wt["worktree"])
        branch = wt.get("branch", "").replace("refs/heads/", "") or "(detached)"
        if branch == "main":
            continue
        exists = path.exists()
        if not exists:
            lines.append(f"| {path} | {branch} | — | — | — | 폴더 없음 → `git worktree prune` |")
            actions.append(f"worktree 폴더가 없다: {path} — `git worktree prune`")
            continue
        head_t = commit_time("HEAD", cwd=path)
        age = age_days(head_t, now)
        ahead, behind = count("main..HEAD", cwd=path), count("HEAD..main", cwd=path)
        dirty = git("status", "--porcelain", cwd=path).splitlines()
        verdict = "진행"
        if branch == "(detached)":
            verdict = "주인 없음(detached)"
        elif age > stale_days and ahead == 0:
            verdict = f"주인 없음(HEAD {age:.0f}일 전, main과 차이 없음)"
        elif age > stale_days:
            verdict = f"오래됨(HEAD {age:.0f}일 전)"
        if dirty and verdict.startswith("주인 없음"):
            actions.append(f"주인 없는 worktree {path}에 미커밋 {len(dirty)}건 — `--orphan-patch {path}`로 패치 보관 뒤 `git worktree remove --force`")
        elif verdict.startswith("주인 없음"):
            actions.append(f"주인 없는 worktree {path} — `git worktree remove {path}`")
        if behind > 0 and verdict == "진행":
            actions.append(f"{branch}가 main보다 {behind}건 뒤 — 머지 전 `git rebase main`")
        lines.append(f"| {path} | {branch} | {head_t.strftime('%m-%d %H:%M') if head_t else '?'} | +{ahead}/−{behind} | {len(dirty)} | {verdict} |")
        info.append({"path": path, "branch": branch, "dirty": dirty, "verdict": verdict})
    return lines, actions, info


# ── 3. 브랜치 ────────────────────────────────────────────────────────────────

def section_branches(wt_info: list[dict]) -> tuple[list[str], list[str], list[str]]:
    lines, actions, prunable = [], [], []
    held = {w["branch"] for w in wt_info}
    refs = git("for-each-ref", "--sort=-committerdate", "--format=%(refname:short)|%(objectname:short)|%(committerdate:short)", "refs/heads")
    for row in refs.splitlines():
        name, sha, date = row.split("|")
        if name == "main":
            continue
        merged = git_ok("merge-base", "--is-ancestor", name, "main")
        ahead = count(f"main..{name}")
        in_wt = name in held
        if merged and not in_wt:
            verdict = "main에 들어감 → 삭제 가능"
            prunable.append(name)
        elif merged and in_wt:
            verdict = "main에 들어감, worktree 아직 있음"
        elif in_wt:
            verdict = f"진행 (main 앞 {ahead})"
        else:
            verdict = f"주인 없는 브랜치 (main 앞 {ahead}, worktree 없음)"
            actions.append(f"주인 없는 브랜치 {name} — 소유 세션이 없으면 `git worktree add`로 다시 잡거나 머지 큐에 올린다")
        lines.append(f"- `{name}` {sha} {date} — {verdict}")
    if prunable:
        actions.append(f"삭제 가능 브랜치 {len(prunable)}개: {', '.join(prunable)} — `--prune-branches`")
    return lines, actions, prunable


# ── 4. 현황판 ────────────────────────────────────────────────────────────────

DONE_RE = re.compile(r"(머지|커밋|푸시)\s*완료")
RELEASED_RE = re.compile(r"(해제|완료)")
D_RE = re.compile(r"D-(\d{3})")


def parse_tables(text: str) -> dict[str, list[list[str]]]:
    tables: dict[str, list[list[str]]] = {}
    section, cur = "", []
    for line in text.splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
            cur = tables.setdefault(section, [])
            continue
        if line.startswith("|") and not re.match(r"^\|\s*-", line):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            cur.append(cells)
    return tables


def section_board(live: set[str]) -> tuple[list[str], list[str]]:
    lines, actions = [], []
    if not BOARD.exists():
        return ["- 현황판 없음"], []
    text = BOARD.read_text(encoding="utf-8")
    tables = parse_tables(text)
    queue = [r for r in tables.get("머지 큐", [])[1:] if len(r) >= 5]
    done = [r for r in queue if DONE_RE.search(r[4])]
    open_rows = [r for r in queue if not DONE_RE.search(r[4])]
    lines.append(f"- 머지 큐 {len(queue)}행 — 완료 {len(done)} · 진행 {len(open_rows)}")
    for r in open_rows:
        lines.append(f"  - 큐 {r[0]} {r[1]} `{r[2]}` {r[3][:60]} — {r[4][:80]}")
    nums = [r[0] for r in queue]
    dup = sorted({n for n in nums if nums.count(n) > 1})
    if dup:
        actions.append(f"머지 큐 순서 번호 중복: {', '.join(dup)} — 압축 때 다시 매긴다")
    if len(done) >= 8:
        actions.append(f"완료 행 {len(done)}개 — _private/archive/SESSION_CLAIMS_<날짜>.md로 옮기고 진행 행만 남긴다")

    owners = [r for r in tables.get("파일 소유 (진행 중인 것만)", [])[1:] if len(r) >= 3]
    released = [r for r in owners if RELEASED_RE.search(r[2])]
    if released:
        actions.append(f"파일 소유 표에 해제/완료 행 {len(released)}개가 남아 있다 — 지운다")

    board_sessions = {r[1] for r in queue} | {r[0].split()[0] for r in owners}
    board_sessions = {s for s in board_sessions if s.startswith("quant-")}
    active_sessions = {r[1] for r in open_rows} | {r[0].split()[0] for r in owners if not RELEASED_RE.search(r[2])}
    if live:
        dead = sorted(active_sessions - live)
        unreg = sorted(s for s in live if s not in board_sessions)
        lines.append(f"- 살아 있는 세션 {len(live)}: {', '.join(sorted(live))}")
        if dead:
            lines.append(f"- 진행 행이 있는데 죽은 세션: {', '.join(dead)}")
            actions.append(f"죽은 세션의 진행 행: {', '.join(dead)} — 브랜치가 남았으면 주인 없는 브랜치로 처리, 행은 보관")
        if unreg:
            lines.append(f"- 살아 있는데 현황판에 없는 세션: {', '.join(unreg)}")
            actions.append(f"미등록 세션 {', '.join(unreg)} — 코드 브랜치를 잡고 있는지 지목해서 묻는다(문서·읽기 세션이면 무시)")

    m = re.search(r"## D-NNN 예약\s*\n+(.+)", text)
    reserved = sorted(set(D_RE.findall(re.sub(r"다음 번호는 D-\d{3}", "", m.group(1))))) if m else []
    landed = set(D_RE.findall("\n".join(l for l in DECISIONS.read_text(encoding="utf-8").splitlines() if re.match(r"^#{2,3} D-\d{3}", l)))) if DECISIONS.exists() else set()
    pending = [f"D-{n}" for n in reserved if n not in landed]
    if reserved:
        lines.append(f"- D-NNN 예약 {len(reserved)}개, 그중 docs/DECISIONS.md(main)에 아직 없는 것: {', '.join(pending) or '없음'}")
        nxt = int(max(reserved)) + 1 if reserved else 0
        lines.append(f"- 다음 번호: D-{nxt:03d}")
    return lines, actions


# ── 5. 인계 파일 ─────────────────────────────────────────────────────────────

def section_handoffs(now: dt.datetime, handoff_days: float) -> tuple[list[str], list[str], list[Path]]:
    lines, actions, old = [], [], []
    files = sorted(PRIVATE.glob("HANDOFF*.md"), key=lambda p: p.stat().st_mtime)
    if not files:
        return ["- 인계 파일 없음"], [], []
    for f in files:
        mt = dt.datetime.fromtimestamp(f.stat().st_mtime).astimezone()
        age = age_days(mt, now)
        title = f.read_text(encoding="utf-8", errors="replace").splitlines()[0].lstrip("# ").strip()[:90]
        flag = " ← 보관 후보" if age > handoff_days else ""
        if age > handoff_days:
            old.append(f)
        lines.append(f"- `{f.relative_to(REPO)}` ({mt.strftime('%m-%d %H:%M')}, {age:.1f}일) — {title}{flag}")
    if old:
        actions.append(f"인계 파일 {len(old)}개가 {handoff_days:g}일 넘음 — 열린 항목을 보고서로 옮기고 `--archive-handoffs`")
    return lines, actions, old


# ── 6. 배포 exe ──────────────────────────────────────────────────────────────

def section_exe() -> tuple[list[str], list[str]]:
    if not EXE.exists():
        return ["- exe 없음"], []
    mt = dt.datetime.fromtimestamp(EXE.stat().st_mtime).astimezone()
    since = mt.strftime("%Y-%m-%dT%H:%M:%S")
    n = len(git("log", "--oneline", f"--since={since}", "main", "--", *CPP_PATHS).splitlines())
    lines = [f"- `Quant/build_win/quant_trader.exe` {mt.strftime('%m-%d %H:%M')} 빌드 — 그 뒤 C++ 커밋 {n}건"]
    actions = [f"exe가 C++ 커밋 {n}건 뒤처짐 — 다음 거래일 08:45 전 배포 세션이 재빌드·교체(메모리 project_trader_watchdog_owner)"] if n else []
    return lines, actions


# ── 행동 ─────────────────────────────────────────────────────────────────────

def prune_branches(names: list[str]) -> list[str]:
    out = []
    for n in names:
        out.append(f"- `git branch -d {n}`: {git('branch', '-d', n) or 'ok'}")
    return out


def archive_handoffs(files: list[Path], now: dt.datetime) -> list[str]:
    dest = PRIVATE / "archive" / "handoff"
    dest.mkdir(parents=True, exist_ok=True)
    out = []
    for f in files:
        target = dest / f.name
        if target.exists():
            target = dest / f"{f.stem}_{now.strftime('%H%M%S')}{f.suffix}"
        shutil.move(str(f), str(target))
        out.append(f"- {f.relative_to(REPO)} → {target.relative_to(REPO)}")
    return out


def orphan_patch(path: Path, now: dt.datetime) -> list[str]:
    dest = PRIVATE / "archive" / "orphans"
    dest.mkdir(parents=True, exist_ok=True)
    name = f"{path.name}_{now.strftime('%Y-%m-%d_%H%M')}"
    head = git("rev-parse", "--short", "HEAD", cwd=path)
    diff = git("diff", "HEAD", cwd=path)
    untracked = git("ls-files", "--others", "--exclude-standard", cwd=path).splitlines()
    patch = dest / f"{name}.patch"
    patch.write_text(f"# worktree {path} HEAD {head}\n# untracked: {', '.join(untracked) or '없음'}\n{diff}\n", encoding="utf-8")
    return [f"- 패치 저장: {patch.relative_to(REPO)} (HEAD {head}, 미추적 {len(untracked)}개는 목록만)",
            f"- 제거는 손으로: `git worktree remove --force \"{path}\"`"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--live", default="", help="ListAgents로 확인한 살아 있는 세션 이름, 쉼표 구분")
    ap.add_argument("--out", help="보고서를 이 파일에도 쓴다")
    ap.add_argument("--stale-days", type=float, default=2.0, help="worktree HEAD가 이 일수보다 오래되면 주인 없음/오래됨")
    ap.add_argument("--handoff-days", type=float, default=1.0, help="인계 파일이 이 일수보다 오래되면 보관 후보")
    ap.add_argument("--prune-branches", action="store_true")
    ap.add_argument("--archive-handoffs", action="store_true")
    ap.add_argument("--orphan-patch", metavar="PATH")
    a = ap.parse_args()

    now = dt.datetime.now().astimezone()
    live = {s.strip() for s in a.live.split(",") if s.strip()}
    report: list[str] = [f"# 세션 교통정리 {now.strftime('%Y-%m-%d %H:%M')}", ""]
    actions: list[str] = []

    def add(title: str, lines: list[str], acts: list[str]) -> None:
        report.extend([f"## {title}", *lines, ""])
        actions.extend(acts)

    l, ac = section_main(now)
    add("1. main", l, ac)
    l, ac, wt_info = section_worktrees(now, a.stale_days)
    add("2. worktree", l, ac)
    l, ac, prunable = section_branches(wt_info)
    add("3. 브랜치", l, ac)
    l, ac = section_board(live)
    add("4. 현황판", l, ac)
    l, ac, old_handoffs = section_handoffs(now, a.handoff_days)
    add("5. 인계 파일", l, ac)
    l, ac = section_exe()
    add("6. 배포 exe", l, ac)

    report.append("## 할 일 (자동 판정)")
    report.extend([f"- [ ] {x}" for x in actions] or ["- 없음"])
    report.append("")

    done_lines: list[str] = []
    if a.prune_branches and prunable:
        done_lines += prune_branches(prunable)
    if a.archive_handoffs and old_handoffs:
        done_lines += archive_handoffs(old_handoffs, now)
    if a.orphan_patch:
        done_lines += orphan_patch(Path(a.orphan_patch), now)
    if done_lines:
        report.extend(["## 실행한 행동", *done_lines, ""])

    text = "\n".join(report)
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n", encoding="utf-8")
        print(f"[저장] {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
