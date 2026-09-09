#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""문서가 가리키는 코드 참조가 실재하는지 검사한다.

  (a) 상대 경로 — 마크다운 링크와 백틱 안의 Quant/·PYQuant/·scripts/·docs/ 경로. 없으면 실패.
  (b) 파일.(cpp|h|py)::심볼 — 그 파일이나 짝 파일(X.h↔X.cpp)에 \\b심볼\\b이 있어야 한다. 없으면 경고.
  (c) 파일.(cpp|h|py):숫자 — 줄번호 앵커. git diff 기준 새로 추가된 줄에서만 실패(--all이면 전부).

코드 펜스 안, 머리에 <!-- drift-check: snapshot 표식이 있는 문서, 면제 경로는 건너뛴다.
줄 끝에 <!-- drift-check: ok --> 를 달면 그 줄의 (c) 앵커 검사만 뺀다(앵커를 예시로 인용하는 줄).
출력은 `파일:줄: 종류: 내용`. exit 0/1.

사용:
    py scripts/check_code_refs.py [파일.md ...]   # 없으면 docs/**, README.md, CLAUDE.md, strategies/**
    py scripts/check_code_refs.py --diff-only       # 스테이징+워킹트리에서 바뀐 .md만
    py scripts/check_code_refs.py --all             # 줄번호 앵커를 새 줄뿐 아니라 전부 보고
"""
from __future__ import annotations

import fnmatch
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DEFAULT_GLOBS = ["docs/**/*.md", "README.md", "CLAUDE.md", "strategies/**/*.md"]
EXEMPT_GLOBS = ["docs/reports/*", "strategies/*/reviews/*", "docs/eod/*", "DAILY_LOG.md"]
CODE_DIRS = ["Quant", "PYQuant", "scripts"]
PATH_PREFIXES = ("Quant/", "PYQuant/", "scripts/", "docs/")
EXTERNAL = ("http://", "https://", "mailto:", "tel:", "//", "#")

LINK_RE = re.compile(r"\[[^\]]*\]\(\s*(<[^>]+>|[^)\s]+)")
TICK_RE = re.compile(r"`([^`\n]+)`")
PATH_IN_TICK_RE = re.compile(r"(?<![\w/.-])((?:Quant|PYQuant|scripts|docs)/[\w./+*{}<>-]+)")
PLACEHOLDER_HINTS = ("YYYY", "MM-DD", "<", "NN/", "*", "{", "}")
SYMBOL_RE = re.compile(r"(?<![\w/.-])([\w./-]+\.(?:cpp|h|py))::([A-Za-z_]\w*)")
ANCHOR_RE = re.compile(r"(?<![\w/.-])([\w./-]+\.(?:cpp|h|py)):(\d+)(?![\w:])")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
SKIP_DIR_NAMES = {"__pycache__", "node_modules", ".git", "logs", "out", "_private"}

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def git(*args: str) -> str:
    r = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    return r.stdout if r.returncode == 0 else ""


def rel(p: Path) -> str:
    """저장소 상대 경로. 밖의 파일은 절대 경로 그대로(미추적으로 취급)."""
    try:
        return p.relative_to(ROOT).as_posix()
    except ValueError:
        return p.as_posix()


def is_exempt(relpath: str) -> bool:
    return any(fnmatch.fnmatch(relpath, g) for g in EXEMPT_GLOBS)


def target_docs(argv: list[str]) -> list[Path]:
    files = [Path(a) for a in argv if not a.startswith("--")]
    if "--diff-only" in argv:
        names = set(git("diff", "HEAD", "--name-only").splitlines())
        names |= set(git("ls-files", "--others", "--exclude-standard").splitlines())
        files = [ROOT / n for n in sorted(names) if n.endswith(".md")]
    elif not files:
        for g in DEFAULT_GLOBS:
            files.extend(ROOT.glob(g))
    out = []
    for f in files:
        f = f if f.is_absolute() else ROOT / f
        if not f.exists() or f.suffix.lower() != ".md":
            continue
        rp = rel(f.resolve())
        if is_exempt(rp) or any(part in SKIP_DIR_NAMES for part in Path(rp).parts):
            continue
        out.append(f.resolve())
    return sorted(set(out))


# ----------------------------- 코드 색인 -----------------------------

class CodeIndex:
    """파일명 → 저장소 상대경로 후보. 심볼 검사용 본문은 필요할 때 읽어 캐시한다."""

    def __init__(self):
        self.by_name: dict[str, list[Path]] = {}
        self.text: dict[Path, str] = {}
        for d in CODE_DIRS:
            base = ROOT / d
            if not base.exists():
                continue
            for p in base.rglob("*"):
                if p.suffix not in (".cpp", ".h", ".hpp", ".py", ".cc"):
                    continue
                if any(part in SKIP_DIR_NAMES or part.startswith((".", "build_")) for part in p.relative_to(ROOT).parts[:-1]):
                    continue
                self.by_name.setdefault(p.name, []).append(p)

    def resolve(self, ref: str) -> Path | None:
        ref = ref.strip("./")
        if "/" in ref:
            p = ROOT / ref
            return p if p.exists() else None
        cands = self.by_name.get(ref, [])
        return cands[0] if cands else None

    def read(self, p: Path) -> str:
        if p not in self.text:
            try:
                self.text[p] = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                self.text[p] = ""
        return self.text[p]

    def pair(self, p: Path) -> list[Path]:
        """X.h ↔ X.cpp 짝. include/src 어느 쪽이든 이름으로 찾는다."""
        out = [p]
        other = {".h": [".cpp", ".cc"], ".hpp": [".cpp"], ".cpp": [".h", ".hpp"], ".cc": [".h"]}.get(p.suffix, [])
        for ext in other:
            out.extend(self.by_name.get(p.stem + ext, []))
        return out

    def has_symbol(self, p: Path, sym: str) -> bool:
        pat = re.compile(r"\b" + re.escape(sym) + r"\b")
        return any(pat.search(self.read(q)) for q in self.pair(p))


# ----------------------------- diff 정보 -----------------------------

def added_lines(relpath: str) -> set[int] | None:
    """HEAD 대비 새로 추가된 줄 번호. 미추적 파일은 None(=전부 새 줄)."""
    if Path(relpath).is_absolute() or not git("ls-files", "--error-unmatch", relpath).strip():
        return None
    out = git("diff", "-U0", "HEAD", "--", relpath)
    added: set[int] = set()
    for ln in out.splitlines():
        m = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", ln)
        if m:
            start, cnt = int(m.group(1)), int(m.group(2) or "1")
            added.update(range(start, start + cnt))
    return added


# ----------------------------- 검사 -----------------------------

def check_doc(doc: Path, idx: CodeIndex, report_all: bool) -> tuple[int, int]:
    rp = rel(doc)
    text = doc.read_text(encoding="utf-8-sig", errors="replace")
    head = "\n".join(text.splitlines()[:5])
    if "<!-- drift-check: snapshot" in head:
        return 0, 0
    fails = warns = 0
    new_lines = None
    new_lines_loaded = False
    in_fence = False
    for no, line in enumerate(text.splitlines(), 1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        # (a) 마크다운 링크
        for m in LINK_RE.finditer(line):
            tgt = m.group(1).strip("<>").split("#")[0]
            if not tgt or tgt.startswith(EXTERNAL) or any(h in tgt for h in PLACEHOLDER_HINTS):
                continue
            p = (doc.parent / tgt).resolve()
            if not p.exists():
                print(f"{rp}:{no}: path-missing: {tgt}")
                fails += 1
        # (a) 백틱 안 경로, (b) 심볼
        for tm in TICK_RE.finditer(line):
            body = tm.group(1)
            for pm in PATH_IN_TICK_RE.finditer(body):
                raw = pm.group(1).rstrip(".,;:")
                if any(h in raw for h in PLACEHOLDER_HINTS):
                    continue
                p = ROOT / raw
                if not p.exists():
                    print(f"{rp}:{no}: path-missing: {raw}")
                    fails += 1
            for sm in SYMBOL_RE.finditer(body):
                fref, sym = sm.group(1), sm.group(2)
                p = idx.resolve(fref)
                if p is None:
                    print(f"{rp}:{no}: path-missing: {fref} (::{sym})")
                    fails += 1
                elif not idx.has_symbol(p, sym):
                    print(f"{rp}:{no}: symbol-missing: {fref}::{sym}")
                    warns += 1
        # (c) 줄번호 앵커. 앵커를 나쁜 예시로 인용해야 하는 줄만 `<!-- drift-check: ok -->`로 뺀다.
        #     펜스 전체를 빼는 것과 달리 (a)·(b) 경로·심볼 검사는 그대로 받는다.
        for am in ([] if "drift-check: ok" in line else ANCHOR_RE.finditer(line)):
            if not report_all:
                if not new_lines_loaded:
                    new_lines = added_lines(rp)
                    new_lines_loaded = True
                if new_lines is not None and no not in new_lines:
                    continue
            print(f"{rp}:{no}: line-anchor: {am.group(1)}:{am.group(2)}")
            fails += 1
    return fails, warns


def main(argv: list[str]) -> int:
    docs = target_docs(argv)
    idx = CodeIndex()
    total_f = total_w = 0
    per_file: list[tuple[str, int, int]] = []
    for d in docs:
        f, w = check_doc(d, idx, report_all="--all" in argv)
        if f or w:
            per_file.append((rel(d), f, w))
        total_f += f
        total_w += w
    print(f"[summary] docs={len(docs)} fail={total_f} warn={total_w}")
    for name, f, w in sorted(per_file, key=lambda t: (-t[1], -t[2], t[0])):
        print(f"  {name}: fail={f} warn={w}")
    return 1 if total_f else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
