#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""저장소를 세어 docs/facts.json을 만들고, 문서의 <!-- gen:이름 --> 블록을 그 값으로 채운다.

셀 수 있는 것(전략·로더·거부 지점·빌드 타깃·하네스·config 키·디렉터리 트리·긴 함수)은
손으로 적지 않고 여기서 생성한다. 생성기는 블록 범위만 치환하고 파일의 나머지는 건드리지 않는다.
블록이 없는 문서는 건너뛴다(블록을 넣는 일은 문서 쪽 작업이다).

사용:
    py scripts/gen_facts.py            # docs/facts.json 갱신
    py scripts/gen_facts.py --check    # 블록이 낡았으면 파일·블록명을 찍고 exit 1
    py scripts/gen_facts.py --apply    # facts.json 갱신 + 블록 치환
"""
from __future__ import annotations

import ast
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_JSON = ROOT / "docs" / "facts.json"

# 토큰 캐시 파일명에 박힌 app_key 앞 8자를 가린다.
TOKEN_NAME_RE = re.compile(r"kis_token_[A-Za-z0-9]+\.json")

# 표식 블록을 둘 수 있는 문서. 블록이 없으면 건너뛴다.
TARGET_DOCS = [
    ".claude/PROJECT_FACTS.md",
    "docs/HARNESS.md",
    "docs/guides/PROJECT_GUIDE.md",
    "docs/GLOSSARY.md",
    "README.md",
    "docs/guides/CODE_GRAPH_GUIDE.md",
]

# 트리·함수 스캔에서 빼는 폴더. 점으로 시작하는 폴더는 .claude만 남긴다(.venv·캐시가 크다).
EXCLUDE_DIR_NAMES = {"_private", "logs", "out", ".git", "__pycache__", "node_modules"}
LONG_FUNC_LINES = 80

BLOCK_RE = re.compile(
    r"(<!--\s*gen:([A-Za-z0-9_-]+)[^>]*-->)(.*?)(<!--\s*/gen(?::[A-Za-z0-9_-]+)?\s*-->)",
    re.S,
)

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass


def rel(p: Path) -> str:
    return p.relative_to(ROOT).as_posix()


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8-sig", errors="replace")


def skip_dir(name: str) -> bool:
    if name in EXCLUDE_DIR_NAMES or name.startswith("build_"):
        return True
    return name.startswith(".") and name != ".claude"


def walk_files(base: Path, exts: tuple[str, ...]):
    if not base.exists():
        return
    for dirpath, dirs, names in os.walk(base):
        dirs[:] = sorted(d for d in dirs if not skip_dir(d))
        for n in sorted(names):
            if n.endswith(exts):
                yield Path(dirpath) / n


# ----------------------------- 집계 -----------------------------

def cpp_strategies() -> list[dict]:
    out = []
    pat = re.compile(r"class\s+(\w+)\s*(?:final\s*)?:\s*public\s+StrategyBase\b")
    for f in sorted((ROOT / "Quant" / "include" / "strategy").glob("*.h")):
        for m in pat.finditer(read(f)):
            out.append({"name": m.group(1), "file": rel(f)})
    return sorted(out, key=lambda d: d["name"])


def strategy_loaders() -> list[dict]:
    """StrategyFactory.cpp의 LOADERS 표 {"ID", load_fn} 쌍."""
    f = ROOT / "Quant" / "src" / "strategy" / "StrategyFactory.cpp"
    if not f.exists():
        return []
    pat = re.compile(r'\{\s*"([A-Z0-9_]+)"\s*,\s*(load_\w+)\s*\}')
    return [{"id": i, "loader": fn} for i, fn in pat.findall(read(f))]


def py_strategies() -> list[dict]:
    out = []
    pat = re.compile(r"^class\s+(\w+)\s*\(\s*StrategyBase\s*\)", re.M)
    for f in sorted((ROOT / "PYQuant" / "strategy").glob("*.py")):
        for m in pat.finditer(read(f)):
            out.append({"name": m.group(1), "file": rel(f)})
    return sorted(out, key=lambda d: d["name"])


def ordergate_rejects() -> dict:
    f = ROOT / "Quant" / "src" / "risk" / "OrderGate.cpp"
    n = 0
    if f.exists():
        pat = re.compile(r"reject_reason\s*=")
        n = sum(1 for ln in read(f).splitlines() if pat.search(ln))
    return {"file": "Quant/src/risk/OrderGate.cpp", "pattern": "reject_reason =", "count": n}


def build_targets() -> dict:
    f = ROOT / "Quant" / "CMakeLists.txt"
    text = read(f) if f.exists() else ""
    names = re.findall(r"add_executable\(\s*(\w+)", text)
    ctest = bool(re.search(r"\benable_testing\s*\(", text) or re.search(r"\badd_test\s*\(", text))
    return {"file": "Quant/CMakeLists.txt", "targets": names, "ctest_wired": ctest}


def harness() -> dict:
    c = ROOT / ".claude"
    commands = sorted(p.stem for p in (c / "commands").glob("*.md")) if (c / "commands").exists() else []
    agents = sorted(p.stem for p in (c / "agents").glob("*.md")) if (c / "agents").exists() else []
    skills = []
    if (c / "skills").exists():
        for p in sorted((c / "skills").iterdir()):
            if p.is_dir() or p.suffix == ".md":
                skills.append(p.stem if p.is_file() else p.name)
    hooks = sorted(p.name for p in (c / "hooks").glob("*.ps1")) if (c / "hooks").exists() else []
    wiring = []
    sfile = c / "settings.json"
    if sfile.exists():
        try:
            data = json.loads(read(sfile))
        except json.JSONDecodeError:
            data = {}
        for event, groups in (data.get("hooks") or {}).items():
            for grp in groups:
                for h in grp.get("hooks", []):
                    cmd = h.get("command", "")
                    m = re.search(r"hooks[\\/]+([\w.-]+\.ps1)", cmd)
                    wiring.append({
                        "event": event,
                        "matcher": grp.get("matcher", ""),
                        "hook_file": m.group(1) if m else "",
                        "command": cmd,
                    })
    return {
        "commands": commands, "agents": agents, "skills": skills, "hooks": hooks,
        "counts": {"commands": len(commands), "agents": len(agents),
                   "skills": len(skills), "hooks": len(hooks), "hook_wirings": len(wiring)},
        "settings_hooks": wiring,
    }


def configs() -> list[str]:
    """config 폴더의 파일명 목록. 값은 싣지 않는다.

    kis_token_<앱키앞8자>.json은 파일명 자체가 실제 app_key 앞부분을 담는다.
    facts.json은 git에 올라가므로 그 부분을 지우고 싣는다.
    """
    d = ROOT / "Quant" / "config"
    if not d.exists():
        return []
    return sorted(TOKEN_NAME_RE.sub("kis_token_<REDACTED>.json", p.name) for p in d.glob("*.json"))


def config_keys() -> dict:
    """키 이름만 본다. 값은 실키가 있어 절대 싣지 않는다."""
    f = ROOT / "Quant" / "config" / "config.json"
    if not f.exists():
        return {"top": [], "strategies": []}
    try:
        data = json.loads(read(f))
    except json.JSONDecodeError:
        return {"top": [], "strategies": []}
    top = sorted(data.keys()) if isinstance(data, dict) else []
    strat = data.get("strategies") if isinstance(data, dict) else None
    keys: set[str] = set()
    if isinstance(strat, dict):
        keys.update(strat.keys())
    elif isinstance(strat, list):
        for item in strat:
            if isinstance(item, dict):
                keys.update(item.keys())
    return {"top": top, "strategies": sorted(keys)}


def dir_tree(depth: int = 2) -> dict:
    def sub(d: Path, lvl: int) -> dict:
        out = {}
        try:
            entries = sorted(p for p in d.iterdir() if p.is_dir() and not skip_dir(p.name))
        except OSError:
            return out
        for p in entries:
            out[p.name] = sub(p, lvl + 1) if lvl < depth else {}
        return out
    return sub(ROOT, 1)


# ----------------------------- 긴 함수 -----------------------------

_CPP_KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "else", "do", "sizeof", "static_assert"}
_CPP_SIG = re.compile(r"^\s*(?:[\w:<>,*&\s~\[\]]+?\s+)?([\w:~]+(?:<[^>]*>)?)\s*\([^;{}]*\)\s*(?:const)?\s*(?:noexcept)?\s*(?:override|final)?\s*(?::[^{;]*)?\{?\s*$")


def _strip_cpp(text: str) -> str:
    """주석·문자열을 같은 줄 수의 공백으로 바꿔 중괄호 계산만 남긴다."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def long_functions_cpp(path: Path) -> list[dict]:
    lines = _strip_cpp(read(path)).split("\n")
    out, i, n = [], 0, len(lines)
    while i < n:
        ln = lines[i]
        m = _CPP_SIG.match(ln)
        if not m or ln.strip().startswith(("#", "}", "return")):
            i += 1
            continue
        name = m.group(1).split("<")[0]
        if name.split("::")[-1] in _CPP_KEYWORDS:
            i += 1
            continue
        # 여는 중괄호가 같은 줄이거나 다음 비어있지 않은 줄에 있어야 정의다.
        j = i
        if "{" not in ln:
            j = i + 1
            while j < n and not lines[j].strip():
                j += 1
            if j >= n or not lines[j].lstrip().startswith("{"):
                i += 1
                continue
        depth, k, closed = 0, j, -1
        while k < n:
            depth += lines[k].count("{") - lines[k].count("}")
            if depth <= 0 and k >= j:
                closed = k
                break
            k += 1
        if closed < 0:
            i += 1
            continue
        length = closed - i + 1
        if length > LONG_FUNC_LINES:
            out.append({"file": rel(path), "symbol": name, "lines": length, "start": i + 1})
        i = closed + 1
    return out


def long_functions_py(path: Path) -> list[dict]:
    try:
        tree = ast.parse(read(path))
    except (SyntaxError, ValueError):
        return []
    out = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            end = getattr(node, "end_lineno", None)
            if end is None:
                continue
            length = end - node.lineno + 1
            if length > LONG_FUNC_LINES:
                out.append({"file": rel(path), "symbol": node.name, "lines": length, "start": node.lineno})
    return out


def long_functions() -> list[dict]:
    out = []
    for base in (ROOT / "Quant" / "include", ROOT / "Quant" / "src"):
        for f in walk_files(base, (".h", ".hpp", ".cpp", ".cc")):
            out.extend(long_functions_cpp(f))
    for base in (ROOT / "PYQuant", ROOT / "scripts"):
        for f in walk_files(base, (".py",)):
            out.extend(long_functions_py(f))
    return sorted(out, key=lambda d: (-d["lines"], d["file"], d["symbol"]))


def collect() -> dict:
    return {
        "cpp_strategies": cpp_strategies(),
        "strategy_loaders": strategy_loaders(),
        "py_strategies": py_strategies(),
        "ordergate_rejects": ordergate_rejects(),
        "build_targets": build_targets(),
        "harness": harness(),
        "configs": configs(),
        "config_keys": config_keys(),
        "dir_tree": dir_tree(),
        "long_functions": long_functions(),
    }


# ----------------------------- 렌더 -----------------------------

def bt(s) -> str:
    return f"`{s}`"


def r_harness_counts(f: dict) -> str:
    h = f["harness"]
    rows = ["| 항목 | 개수 | 이름 |", "|---|---|---|"]
    for key, label in (("commands", "커맨드"), ("agents", "에이전트"), ("skills", "스킬"), ("hooks", "훅 파일")):
        rows.append(f"| {label} | {bt(len(h[key]))} | {', '.join(bt(x) for x in h[key])} |")
    rows.append(f"| settings.json 훅 배선 | {bt(h['counts']['hook_wirings'])} | "
                + ", ".join(bt(f"{w['event']}:{w['hook_file']}") for w in h["settings_hooks"]) + " |")
    return "\n".join(rows)


def r_ordergate_rejects(f: dict) -> str:
    o = f["ordergate_rejects"]
    return f"{bt(o['file'])}의 {bt(o['pattern'])} 지점: {bt(o['count'])}개"


def r_cpp_strategies(f: dict) -> str:
    loaders = {d["loader"]: d["id"] for d in f["strategy_loaders"]}
    rows = ["| 클래스 | 헤더 |", "|---|---|"]
    for s in f["cpp_strategies"]:
        rows.append(f"| {bt(s['name'])} | {bt(s['file'])} |")
    rows.append("")
    rows.append(f"로더(`StrategyFactory.cpp` LOADERS) {bt(len(loaders))}개: "
                + ", ".join(bt(f"{i} → {fn}") for fn, i in loaders.items()))
    return "\n".join(rows)


def r_py_strategies(f: dict) -> str:
    rows = ["| 클래스 | 파일 |", "|---|---|"]
    for s in f["py_strategies"]:
        rows.append(f"| {bt(s['name'])} | {bt(s['file'])} |")
    return "\n".join(rows)


def r_build_targets(f: dict) -> str:
    b = f["build_targets"]
    ctest = "있음" if b["ctest_wired"] else "없음"
    return (f"{bt(b['file'])} 실행 타깃 {bt(len(b['targets']))}개: "
            + ", ".join(bt(t) for t in b["targets"]) + f"\n\nctest 배선: {bt(ctest)}")


def r_dir_tree(f: dict) -> str:
    rows = []

    def emit(tree: dict, indent: int):
        for name, sub in tree.items():
            rows.append("  " * indent + f"- {bt(name + '/')}")
            emit(sub, indent + 1)
    emit(f["dir_tree"], 0)
    return "\n".join(rows)


def r_config_keys(f: dict) -> str:
    c = f["config_keys"]
    return ("최상위: " + ", ".join(bt(k) for k in c["top"])
            + "\n\n`strategies` 항목 키: " + ", ".join(bt(k) for k in c["strategies"])
            + "\n\nconfig 파일: " + ", ".join(bt(k) for k in f["configs"]))


def r_long_functions(f: dict) -> str:
    rows = [f"| 파일::심볼 | 줄 수 |", "|---|---|"]
    for d in f["long_functions"]:
        rows.append(f"| {bt(d['file'] + '::' + d['symbol'])} | {bt(d['lines'])} |")
    if len(rows) == 2:
        rows.append(f"| (없음) | |")
    return "\n".join(rows)


def r_hooks(f: dict) -> str:
    rows = ["| 이벤트 | matcher | 훅 파일 |", "|---|---|---|"]
    for w in f["harness"]["settings_hooks"]:
        rows.append(f"| {bt(w['event'])} | {bt(w['matcher'] or '(전체)')} | {bt(w['hook_file'] or w['command'])} |")
    return "\n".join(rows)


RENDERERS = {
    "harness-counts": r_harness_counts,
    "ordergate-rejects": r_ordergate_rejects,
    "cpp-strategies": r_cpp_strategies,
    "py-strategies": r_py_strategies,
    "build-targets": r_build_targets,
    "dir-tree": r_dir_tree,
    "config-keys": r_config_keys,
    "long-functions": r_long_functions,
    "hooks": r_hooks,
}


# ----------------------------- 블록 치환 -----------------------------

def splice(text: str, facts: dict) -> tuple[str, list[str], list[str]]:
    """블록 본문만 바꾼다. 반환: (새 본문, 낡은 블록명, 모르는 블록명)."""
    stale, unknown = [], []

    def sub(m: re.Match) -> str:
        name = m.group(2)
        fn = RENDERERS.get(name)
        if fn is None:
            unknown.append(name)
            return m.group(0)
        body = "\n" + fn(facts) + "\n"
        if m.group(3) != body:
            stale.append(name)
        return m.group(1) + body + m.group(4)

    return BLOCK_RE.sub(sub, text), stale, unknown


def main(argv: list[str]) -> int:
    check, apply = "--check" in argv, "--apply" in argv
    facts = collect()
    if not check:
        OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
        OUT_JSON.write_text(json.dumps(facts, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"[ok] wrote {rel(OUT_JSON)}")
    rc = 0
    for d in TARGET_DOCS:
        p = ROOT / d
        if not p.exists():
            continue
        raw = p.read_bytes()
        bom = raw.startswith(b"\xef\xbb\xbf")
        text = raw.decode("utf-8-sig", errors="replace")
        if "<!-- gen:" not in text and "<!--gen:" not in text:
            continue
        new, stale, unknown = splice(text, facts)
        for u in unknown:
            print(f"{d}: [warn] 모르는 블록 gen:{u}")
        if not stale:
            print(f"{d}: 최신")
            continue
        if check:
            for s in stale:
                print(f"{d}: gen:{s} 낡음")
            rc = 1
        elif apply:
            nl = "\r\n" if "\r\n" in text else "\n"
            data = new.replace("\r\n", "\n").replace("\n", nl).encode("utf-8")
            p.write_bytes((b"\xef\xbb\xbf" if bom else b"") + data)
            print(f"{d}: 치환 {', '.join('gen:' + s for s in stale)}")
        else:
            print(f"{d}: 낡은 블록 {', '.join('gen:' + s for s in stale)} (--apply로 치환)")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
