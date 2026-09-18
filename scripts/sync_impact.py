#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""바뀐 소스가 낡게 만든 문서를 찍고, 도장(stamp)으로 문단 단위 낡음을 잡는다 (D-075).

두 가지 장치를 합친다.
  1. 매핑 — docs/sync_map.toml의 "소스 glob → 대표 문서" 규칙. 바뀐 파일과 대조해 봐야 할 문서를 찍는다.
     check가 gen/script인 규칙은 기계가 낡음을 확정한다. stamp/hint는 아래 도장이 있어야 확정된다.
  2. 도장 — 문서 안의 `<!-- sync: 경로@해시 경로@해시 -->`. 요약 문단이 어느 소스의 어느 버전을 보고 쓴 것인지
     남긴다. 해시는 git blob 해시 앞 7자라 git 없이도 계산된다. 소스가 바뀌면 그 도장만 낡음으로 나온다 —
     glob 매핑처럼 "관련 있어 보이는 문서 전부"가 아니라 "확실히 낡은 문단"이다.

출력 줄머리:
  [gen]    gen 블록이 낡음 (--fix면 치환함)          — 막는다
  [stale]  도장이 낡음. 문단을 고치고 --restamp       — 막는다
  [script] 재생성·검사 명령 실패                       — 막는다
  [hint]   매핑만 있고 도장이 없어 판단이 필요한 문서   — 같은 소스 버전에 한 번만 (--state)

사용:
    py scripts/sync_impact.py --diff [--fix] [--state 파일]   # HEAD 대비 바뀐 파일(스테이징+워킹트리+미추적)
    py scripts/sync_impact.py --stamps                          # 도장만 전부 검사
    py scripts/sync_impact.py --restamp 문서.md [...]           # 그 문서의 도장을 현재 해시로
    py scripts/sync_impact.py --restamp-all
    py scripts/sync_impact.py --render [--check]                # docs/SYNC_MAP.md §2 표 생성(--check는 낡음만 exit 1)
exit 0 = 막는 항목 없음, 1 = 있음.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAP = ROOT / "docs" / "sync_map.toml"
SYNC_MAP_MD = ROOT / "docs" / "SYNC_MAP.md"
PY = sys.executable

# 도장을 찾을 문서. gitignore 개인문서는 뺀다.
STAMP_GLOBS = ["CLAUDE.md", "README.md", "docs/**/*.md", "strategies/**/*.md", "research/*.md"]
STAMP_RE = re.compile(r"<!--\s*sync:\s*([^>]*?)\s*-->")
STAMP_ITEM_RE = re.compile(r"([^\s@]+)(?:@([0-9a-f]{7,40}))?")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
TICK_RE = re.compile(r"`[^`\n]*`")
RENDER_RE = re.compile(r"(<!--\s*sync-map:rules\s*-->)(.*?)(<!--\s*/sync-map:rules\s*-->)", re.S)

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def rel(p: Path) -> str:
    return p.relative_to(ROOT).as_posix()


def blob_hash(p: Path) -> str | None:
    """git hash-object와 같은 방식(blob 헤더 + sha1 앞 7자). 워킹트리 내용 기준이라 스테이징 여부와 무관하다.

    줄바꿈은 LF로 맞춘 뒤 센다. 에디터가 파일을 CRLF로 저장하면 내용이 그대로여도 바이트가 달라져 도장이
    낡은 것으로 잡혔다(2026-09-18 `Quant/include/api/IMarketDataSource.h`, git status에는 안 보이는 변경).
    그래서 LF로 커밋된 파일은 git 해시와 같고, CRLF로 커밋된 파일은 다르다 — 도장은 이 함수끼리만 비교하므로 상관없다.
    """
    if not p.is_file():
        return None
    data = p.read_bytes().replace(b"\r\n", b"\n")
    h = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
    return h[:7]


def glob_to_re(g: str) -> re.Pattern:
    out, i = [], 0
    while i < len(g):
        c = g[i]
        if g.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif g.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(c))
            i += 1
    return re.compile("^" + "".join(out) + "$")


def load_rules() -> list[dict]:
    with MAP.open("rb") as f:
        data = tomllib.load(f)
    rules = data.get("rule", [])
    for r in rules:
        r["_res"] = [glob_to_re(g) for g in r["src"]]
    return rules


def changed_files() -> list[str]:
    def git(*args: str) -> list[str]:
        try:
            r = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            return []
        return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]

    files = set(git("diff", "--name-only", "HEAD"))
    files.update(git("ls-files", "-o", "--exclude-standard"))
    return sorted(files)


# ----------------------------- 도장 -----------------------------

def stamp_docs() -> list[Path]:
    out: list[Path] = []
    for g in STAMP_GLOBS:
        out.extend(p for p in ROOT.glob(g) if p.is_file())
    return sorted(set(out))


def read_text(p: Path) -> str:
    return p.read_text(encoding="utf-8-sig", errors="replace")


def scan_stamps(doc: Path) -> list[dict]:
    """문서의 도장 하나하나를 {line, path, hash} 로 편다. 코드 펜스·인라인 코드 안은 예시라 뺀다."""
    out = []
    fenced = False
    for ln_no, line in enumerate(read_text(doc).splitlines(), 1):
        if FENCE_RE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        for m in STAMP_RE.finditer(TICK_RE.sub("", line)):
            for item in STAMP_ITEM_RE.finditer(m.group(1)):
                out.append({"doc": doc, "line": ln_no, "path": item.group(1), "hash": item.group(2)})
    return out


def check_stamps(docs: list[Path] | None = None) -> list[str]:
    """낡은 도장 줄 목록. 해시가 없는 도장은 '미기입'으로 낡음이다."""
    lines = []
    for doc in (docs or stamp_docs()):
        for s in scan_stamps(doc):
            cur = blob_hash(ROOT / s["path"])
            if cur is None:
                lines.append(f"[stale] {rel(doc)}:{s['line']} 도장 {s['path']} — 소스 파일이 없다")
            elif s["hash"] is None:
                lines.append(f"[stale] {rel(doc)}:{s['line']} 도장 {s['path']} 해시 미기입 → --restamp")
            elif not cur.startswith(s["hash"][:7]):
                lines.append(f"[stale] {rel(doc)}:{s['line']} 도장 {s['path']} {s['hash'][:7]}→{cur} — 문단 확인 뒤 "
                             f"`py scripts/sync_impact.py --restamp {rel(doc)}`")
    return lines


def restamp(doc: Path) -> list[str]:
    raw = doc.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig", errors="replace")
    changed = []

    def sub_stamp(m: re.Match) -> str:
        items = []
        for it in STAMP_ITEM_RE.finditer(m.group(1)):
            path, old = it.group(1), it.group(2)
            cur = blob_hash(ROOT / path)
            if cur is None:
                items.append(f"{path}@{old}" if old else path)
                changed.append(f"  {path}: 소스 없음, 그대로 둠")
                continue
            if old is None or not cur.startswith(old[:7]):
                changed.append(f"  {path}: {old or '(없음)'}→{cur}")
            items.append(f"{path}@{cur}")
        return "<!-- sync: " + " ".join(items) + " -->"

    lines, fenced = [], False
    for line in text.split("\n"):
        if FENCE_RE.match(line):
            fenced = not fenced
        if fenced or FENCE_RE.match(line):
            lines.append(line)
            continue
        # 인라인 코드 조각은 잘라 두고 나머지만 치환한 뒤 되붙인다
        pieces, last = [], 0
        for tm in TICK_RE.finditer(line):
            pieces.append(STAMP_RE.sub(sub_stamp, line[last:tm.start()]))
            pieces.append(tm.group(0))
            last = tm.end()
        pieces.append(STAMP_RE.sub(sub_stamp, line[last:]))
        lines.append("".join(pieces))
    new = "\n".join(lines)
    if new != text:
        doc.write_bytes((b"\xef\xbb\xbf" if bom else b"") + new.encode("utf-8"))
    return changed


def stamped_paths(doc: Path) -> set[str]:
    return {s["path"] for s in scan_stamps(doc)}


# ----------------------------- 매핑 -----------------------------

def match_rules(rules: list[dict], files: list[str]) -> list[tuple[dict, list[str]]]:
    out = []
    for r in rules:
        hit = [f for f in files if any(rx.match(f) for rx in r["_res"])]
        if hit:
            out.append((r, hit))
    return out


def run_cmd(cmd: str) -> tuple[int, str]:
    argv = cmd.split()
    if argv and argv[0] == "py":
        argv[0] = PY
    try:
        r = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, str(e)
    return r.returncode, (r.stdout + r.stderr).strip()


def load_state(p: Path | None) -> dict:
    if p is None or not p.exists():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def diff_report(fix: bool, state_path: Path | None) -> int:
    rules = load_rules()
    files = changed_files()
    out: list[str] = []
    blocking = False
    if not files:
        print("바뀐 파일 없음")
        return 0

    hits = match_rules(rules, files)
    need_gen = True  # 블록을 손으로 고친 경우도 잡아야 하므로 매핑과 무관하게 본다. 싸다.
    cmds: dict[str, list[str]] = {}
    fixers: dict[str, str] = {}  # cmd → fix_cmd. --fix 때는 검사 대신 재생성을 돌린다(gen 블록과 같은 대우, D-078)
    for r, hit in hits:
        if r["check"] == "script":
            cmds.setdefault(r["cmd"], []).extend(hit)
            if r.get("fix_cmd"):
                fixers[r["cmd"]] = r["fix_cmd"]

    # gen 블록: 낡으면 --fix 때 치환, 아니면 막는다
    if need_gen:
        code, text = run_cmd(f"py scripts/gen_facts.py {'--apply' if fix else '--check'}")
        for ln in text.splitlines():
            if "낡음" in ln or "치환" in ln:
                out.append(f"[gen] {ln}")
                blocking = blocking or ("낡음" in ln)

    # 재생성·검사 명령
    for cmd, hit in cmds.items():
        if fix and cmd in fixers:
            code, text = run_cmd(fixers[cmd])
            if code == 0:
                out.append(f"[script] `{fixers[cmd]}` 재생성함 (트리거: {', '.join(sorted(set(hit))[:4])})")
                continue
        else:
            code, text = run_cmd(cmd)
        if code != 0:
            blocking = True
            tail = "\n".join("    " + t for t in text.splitlines()[-6:])
            out.append(f"[script] `{cmd}` 실패 (트리거: {', '.join(sorted(set(hit))[:4])})\n{tail}")

    # 도장: 전부 검사한다 (매핑과 무관하게 낡은 도장은 막는다)
    stale = check_stamps()
    if stale:
        blocking = True
        out.extend(stale)

    # 힌트: stamp/hint 규칙 중, 바뀐 소스에 대한 도장이 그 문서에 없는 것만. 같은 소스 버전에 한 번만.
    state = load_state(state_path)
    new_state = dict(state)
    for r, hit in hits:
        if r["check"] not in ("stamp", "hint"):
            continue
        for dep in r["deps"]:
            dep_file = dep.split("#", 1)[0]
            dep_path = ROOT / dep_file
            stamped = stamped_paths(dep_path) if dep_path.is_file() else set()
            pending = []
            for f in hit:
                if f in stamped:
                    continue  # 도장이 잡는다
                key = f"{f}@{blob_hash(ROOT / f) or '-'}|{dep}"
                new_state[key] = True
                if key not in state:
                    pending.append(f)
            if pending:
                out.append(f"[hint] {', '.join(pending[:4])}{' …' if len(pending) > 4 else ''} → {dep} : {r['note']}")
    if state_path is not None:
        state_path.parent.mkdir(parents=True, exist_ok=True)
        state_path.write_text(json.dumps(new_state, ensure_ascii=False, indent=0), encoding="utf-8")

    print("\n".join(out) if out else "동기화 대상 없음")
    return 1 if blocking else 0


# ----------------------------- SYNC_MAP.md §2 렌더 -----------------------------

def render_table(rules: list[dict]) -> str:
    label = {"gen": "자동(gen 블록)", "script": "자동(명령)", "stamp": "도장", "hint": "힌트"}
    rows = ["| 소스(바뀌면) | 대표 문서(봐라) | 검사 | 맞출 것 |", "|---|---|---|---|"]
    for r in rules:
        src = ", ".join(f"`{g}`" for g in r["src"])
        deps = ", ".join(f"`{d}`" for d in r["deps"])
        chk = label[r["check"]] + (f" `{r['cmd']}`" if r.get("cmd") else "")
        rows.append(f"| {src} | {deps} | {chk} | {r['note']} |")
    return "\n".join(rows)


def render(check: bool) -> int:
    rules = load_rules()
    raw = SYNC_MAP_MD.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig", errors="replace")
    m = RENDER_RE.search(text)
    if not m:
        print(f"{rel(SYNC_MAP_MD)}: <!-- sync-map:rules --> 마커 없음")
        return 1
    body = "\n" + render_table(rules) + "\n"
    if m.group(2).replace("\r\n", "\n") == body:
        print(f"{rel(SYNC_MAP_MD)}: 최신")
        return 0
    if check:
        print(f"{rel(SYNC_MAP_MD)}: 규칙 표 낡음 → `py scripts/sync_impact.py --render`")
        return 1
    new = text[:m.start(2)] + body + text[m.end(2):]
    nl = "\r\n" if "\r\n" in text else "\n"
    SYNC_MAP_MD.write_bytes((b"\xef\xbb\xbf" if bom else b"")
                            + new.replace("\r\n", "\n").replace("\n", nl).encode("utf-8"))
    print(f"{rel(SYNC_MAP_MD)}: 규칙 표 갱신")
    return 0


def main(argv: list[str]) -> int:
    if "--render" in argv:
        return render("--check" in argv)
    if "--stamps" in argv:
        stale = check_stamps()
        print("\n".join(stale) if stale else "도장 전부 최신")
        return 1 if stale else 0
    if "--restamp-all" in argv:
        for d in stamp_docs():
            ch = restamp(d)
            if ch:
                print(f"{rel(d)}:\n" + "\n".join(ch))
        return 0
    if "--restamp" in argv:
        docs = [a for a in argv if not a.startswith("--")]
        if not docs:
            print("--restamp 문서.md [...]")
            return 1
        for a in docs:
            d = ROOT / a
            if not d.is_file():
                print(f"{a}: 없음")
                continue
            ch = restamp(d)
            print(f"{a}:\n" + ("\n".join(ch) if ch else "  변경 없음"))
        return 0
    if "--diff" in argv:
        state = None
        if "--state" in argv:
            state = ROOT / argv[argv.index("--state") + 1]
        return diff_report("--fix" in argv, state)
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
