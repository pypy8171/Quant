# -*- coding: utf-8 -*-
"""코드 의존 그래프 생성기 (설치 불필요, Mermaid + Graphviz DOT + JSON/질의).

Quant/include·Quant/src 를 스캔해 로컬 `#include "..."` 관계를 뽑아
모듈(api/core/ipc/modes/risk/strategy/universe/utils/main) 간 의존 그래프와
파일 단위 상세 그래프를 docs/CODE_GRAPH.md 로 생성한다.

재현 규약: 코드가 바뀌면 이 스크립트를 다시 돌려 문서를 갱신한다(손편집 금지).

사용법:
    py scripts/gen_code_graph.py                 # 문서(md) + dot 생성
    py scripts/gen_code_graph.py --json          # docs/code_graph.json 도 생성(에이전트 소비용)
    py scripts/gen_code_graph.py --impact core/Types.h
        # 이 헤더를 (직접/전이) include 하는 파일·모듈 = 재검증·재빌드 영향범위
    py scripts/gen_code_graph.py --check         # 산출물(md·dot·json)이 낡았으면 exit 1

C++ include 그래프 외에 Python import 그래프(PYQuant/**·scripts/*.py 내부 import만)와
프로세스 경계 파일(regime.json·prices_live.json·trades_*.csv·universe*.json 등을 코드에서
문자열로 찾아 읽는 쪽/쓰는 쪽)을 같은 문서의 별도 절과 json에 싣는다.

Graphviz(dot)가 설치돼 있으면 docs/code_graph.dot 을 렌더링할 수 있다:
    dot -Tsvg docs/code_graph.dot -o docs/code_graph.svg
"""
import ast
import json
import os
import re
import sys
from collections import defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
SCAN_DIRS = [os.path.join(ROOT, "Quant", "include"), os.path.join(ROOT, "Quant", "src")]
OUT_MD = os.path.join(ROOT, "docs", "CODE_GRAPH.md")
OUT_DOT = os.path.join(ROOT, "docs", "code_graph.dot")
OUT_JSON = os.path.join(ROOT, "docs", "code_graph.json")

INCLUDE_RE = re.compile(r'#\s*include\s+"([^"]+)"')
MODULES = ["api", "core", "ipc", "modes", "risk", "strategy", "universe", "utils"]

PY_ROOTS = [os.path.join(ROOT, "PYQuant"), os.path.join(ROOT, "scripts")]
PY_SKIP_DIRS = {"__pycache__", "node_modules", ".git", "logs", "out", "_private"}
# 프로세스 경계 파일. (표시 이름, 문자열 리터럴 정규식, 리터럴이 없을 때 잡는 식별자 정규식)
# C++ 쪽은 경로가 config·Logger를 거쳐 오므로 식별자(regime_file_ 등)로 잡는다.
BOUNDARY_FILES = [
    ("regime.json", r"regime\.json", r"\bregime_file"),
    ("prices_live.json", r"prices_live\.json", r"\bprices_live"),
    ("trades_*.csv", r"trades_[^\"'\s]*\.csv", r"\"trades_\""),
    ("universe*.json", r"universe[^\"'\s]*\.json", r"\buniverse_(?:file|path|scan_file)"),
    ("open_orders.txt", r"open_orders\.(?:txt|tmp|json)", r"\bopen_orders_file"),
    ("quant_trader.log", r"quant_trader\.log", None),
    ("kis_token_*.json", r"kis_token[^\"'\s]*\.json", r"\bkis_token_"),
]
SELF = os.path.abspath(__file__)
WRITE_HINTS = re.compile(r"ofstream|\bwrite|dump\(|to_csv|to_json|\bsave\b|rename|replace\(|\"[wa]b?\"|'[wa]b?'|Set-Content|Out-File|append")
READ_HINTS = re.compile(r"ifstream|\bread|load\(|read_csv|read_json|read_text|\bopen\(|exists|stat\(|glob|\"r\"|'r'|getline|parse")


def module_of(rel_path):
    """Quant 하위 상대경로 -> 모듈명. include/src 접두를 벗기고 첫 세그먼트."""
    parts = rel_path.replace("\\", "/").split("/")
    if "include" in parts:
        parts = parts[parts.index("include") + 1:]
    elif "src" in parts:
        parts = parts[parts.index("src") + 1:]
    if len(parts) == 1:  # main.cpp
        return "main"
    return parts[0]


def include_module(inc):
    """#include "..." 문자열 -> 모듈명(로컬만), 표준/외부는 None."""
    seg = inc.replace("\\", "/").split("/")[0]
    return seg if seg in MODULES else None


def label_of(f):
    """abs path -> 'module/File.ext' 표시용 라벨(include/ 또는 src/ 이후)."""
    rel = os.path.relpath(f, ROOT).replace("\\", "/")
    return rel.split("include/")[-1].split("src/")[-1]


class Graph:
    def __init__(self):
        self.mods = []                          # 정렬된 모듈 목록
        self.mod_edges = defaultdict(int)       # (src_mod, dst_mod) -> weight
        self.file_edges = defaultdict(set)      # src_mod -> {(src_label, include_str)}
        self.file_mod = {}                       # label -> module
        self.file_includes = defaultdict(set)   # label -> {include_str} (로컬만)
        self.reverse = defaultdict(set)         # include_str -> {label that includes it}


def scan():
    g = Graph()
    files = []
    for base in SCAN_DIRS:
        for dirpath, _, names in os.walk(base):
            for n in names:
                if n.endswith((".h", ".hpp", ".cpp", ".cc")):
                    files.append(os.path.join(dirpath, n))

    for f in files:
        rel = os.path.relpath(f, ROOT)
        lab = label_of(f)
        g.file_mod[lab] = module_of(rel)

    for f in files:
        lab = label_of(f)
        m = g.file_mod[lab]
        try:
            with open(f, "r", encoding="utf-8", errors="ignore") as fh:
                text = fh.read()
        except OSError:
            continue
        for inc in INCLUDE_RE.findall(text):
            dm = include_module(inc)
            if dm is None:
                continue
            g.file_includes[lab].add(inc)
            g.reverse[inc].add(lab)
            g.file_edges[m].add((lab, inc))
            if dm != m:
                g.mod_edges[(m, dm)] += 1

    g.mods = sorted(set(g.file_mod.values()))
    return g


# ----------------------------- 질의: --impact -----------------------------

def impact(g, target):
    """target(예: 'core/Types.h')을 직접/전이로 include 하는 라벨 집합."""
    direct = set(g.reverse.get(target, set()))
    # 전이: 헤더 A가 target을 include하고, B가 A를 include하면 B도 영향
    seen = set(direct)
    q = deque(direct)
    while q:
        cur = q.popleft()
        for parent in g.reverse.get(cur, set()):  # cur를 include하는 것들
            if parent not in seen:
                seen.add(parent)
                q.append(parent)
    transitive = seen - direct
    return direct, transitive


def print_impact(g, target):
    if target not in g.reverse and target not in g.file_includes:
        print(f"[warn] '{target}' 을(를) include 하는 로컬 파일이 없거나 인식 불가.")
        print("       예: core/Types.h, utils/Logger.h, strategy/StrategyBase.h")
        return 1
    direct, transitive = impact(g, target)
    all_labels = sorted(direct | transitive)
    mods = sorted({g.file_mod.get(l, "?") for l in all_labels})
    print(f"# impact of touching  {target}")
    print(f"직접 include: {len(direct)}  |  전이 포함 총: {len(all_labels)}  |  영향 모듈: {len(mods)}")
    print(f"재검증/재빌드 대상 모듈: {', '.join(mods)}")
    print("")
    print("[직접]")
    for l in sorted(direct):
        print(f"  {l}")
    if transitive:
        print("[전이]")
        for l in sorted(transitive):
            print(f"  {l}")
    return 0


# ----------------------------- Python import 그래프 -----------------------------

def py_files():
    """PYQuant/**·scripts/*.py. 가상환경·캐시(.으로 시작)는 건너뛴다."""
    out = []
    for base in PY_ROOTS:
        if not os.path.isdir(base):
            continue
        for dirpath, dirs, names in os.walk(base):
            dirs[:] = sorted(d for d in dirs if d not in PY_SKIP_DIRS and not d.startswith((".", "build_")))
            if os.path.basename(base) == "scripts" and dirpath != base:
                continue
            for n in sorted(names):
                if n.endswith(".py"):
                    out.append(os.path.join(dirpath, n))
    return out


def py_label(f):
    return os.path.relpath(f, ROOT).replace("\\", "/")


def py_package(label):
    """PYQuant/strategy/base.py -> PYQuant/strategy, scripts/x.py -> scripts."""
    parts = label.split("/")
    return "/".join(parts[:2]) if parts[0] == "PYQuant" and len(parts) > 2 else parts[0]


def _is_script_module(top):
    return os.path.exists(os.path.join(ROOT, "scripts", top + ".py"))


def scan_python():
    files = py_files()
    labels = {f: py_label(f) for f in files}
    # 내부 모듈 이름: PYQuant 바로 아래 패키지·모듈, scripts/*.py 의 stem
    internal = set()
    for lab in labels.values():
        parts = lab.split("/")
        if parts[0] == "PYQuant" and len(parts) >= 2:
            internal.add(parts[1][:-3] if parts[1].endswith(".py") else parts[1])
        elif parts[0] == "scripts":
            internal.add(parts[1][:-3])
    internal.discard("__init__")
    edges = defaultdict(set)      # label -> {module string}
    reverse = defaultdict(set)    # module string -> {label}
    for f, lab in labels.items():
        try:
            with open(f, "r", encoding="utf-8-sig", errors="ignore") as fh:
                tree = ast.parse(fh.read())
        except (SyntaxError, ValueError, OSError):
            continue
        for node in ast.walk(tree):
            names = []
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom):
                if node.level and node.level > 0:
                    # 상대 import: 같은 패키지 기준으로 절대 이름을 만든다
                    pkg = lab.split("/")[1:-1] if lab.startswith("PYQuant/") else []
                    base = ".".join(pkg[:max(0, len(pkg) - node.level + 1)])
                    names = [".".join(x for x in (base, node.module or "") if x)]
                elif node.module:
                    names = [node.module]
            for n in names:
                if n.startswith("PYQuant."):
                    n = n[len("PYQuant."):]
                top = n.split(".")[0]
                if n and top in internal:
                    edges[lab].add(n)
                    reverse[n].add(lab)
    pkg_edges = defaultdict(int)
    for lab, mods in edges.items():
        sp = py_package(lab)
        for m in mods:
            top = m.split(".")[0]
            if lab.startswith("scripts/") and _is_script_module(top):
                dp = "scripts"
            elif os.path.exists(os.path.join(ROOT, "PYQuant", top + ".py")):
                dp = "PYQuant"
            else:
                dp = "PYQuant/" + top
            if dp != sp:
                pkg_edges[(sp, dp)] += 1
    return {
        "files": sorted(labels.values()),
        "edges": {k: sorted(v) for k, v in sorted(edges.items())},
        "imported_by": {k: sorted(v) for k, v in sorted(reverse.items())},
        "package_edges": [{"from": a, "to": b, "weight": w} for (a, b), w in sorted(pkg_edges.items())],
    }


# ----------------------------- 프로세스 경계 파일 -----------------------------

def code_files_for_boundary():
    out = []
    for base in SCAN_DIRS:
        for dirpath, _, names in os.walk(base):
            for n in names:
                if n.endswith((".h", ".hpp", ".cpp", ".cc")):
                    out.append(os.path.join(dirpath, n))
    out.extend(py_files())
    return out


def _classify(lines, idx, name_hint=None):
    """리터럴이 있는 줄 ±3줄(상수에 담겼으면 그 상수를 쓰는 줄도)에서 읽기/쓰기 힌트를 본다."""
    windows = [idx]
    if name_hint:
        pat = re.compile(r"\b" + re.escape(name_hint) + r"\b")
        windows += [i for i, ln in enumerate(lines) if pat.search(ln) and i != idx]
    modes = set()
    for w in windows:
        chunk = "\n".join(lines[max(0, w - 3): w + 4])
        if WRITE_HINTS.search(chunk):
            modes.add("write")
        if READ_HINTS.search(chunk):
            modes.add("read")
    return modes


def scan_boundary():
    lit_re = re.compile(r"[\"']([^\"'\n]*?(" + "|".join(p for _, p, _ in BOUNDARY_FILES) + r"))[\"']")
    idents = [(n, re.compile(ir)) for n, _, ir in BOUNDARY_FILES if ir]
    # 줄 앞쪽의 대입 대상(첫 식별자) — 그 이름을 쓰는 줄도 힌트 창에 넣는다
    assign_re = re.compile(r"^\s*(?:[\w:<>\[\]&*]+\s+)*([A-Za-z_]\w*)\s*=[^=]")
    comment_re = re.compile(r"^\s*(//|#|\*|/\*)")
    result = {name: {"readers": set(), "writers": set(), "mentions": set()} for name, _, _ in BOUNDARY_FILES}
    for f in code_files_for_boundary():
        if os.path.abspath(f) == SELF:
            continue
        try:
            with open(f, "r", encoding="utf-8-sig", errors="ignore") as fh:
                lines = fh.read().split("\n")
        except OSError:
            continue
        lab = os.path.relpath(f, ROOT).replace("\\", "/")
        for i, ln in enumerate(lines):
            if comment_re.match(ln):
                continue
            hits = []
            for m in lit_re.finditer(ln):
                hits.append(next(n for n, p, _ in BOUNDARY_FILES if re.fullmatch(p, m.group(2))))
            if not hits:
                hits = [n for n, ir in idents if ir.search(ln)]
            if not hits:
                continue
            am = assign_re.match(ln)
            modes = _classify(lines, i, am.group(1) if am else None)
            for name in set(hits):
                result[name]["mentions"].add(lab)
                if "write" in modes:
                    result[name]["writers"].add(lab)
                if "read" in modes:
                    result[name]["readers"].add(lab)
    return {n: {"writers": sorted(v["writers"]), "readers": sorted(v["readers"]),
                "mentions": sorted(v["mentions"])} for n, v in result.items() if v["mentions"]}


# ----------------------------- 렌더러 -----------------------------

def render_mermaid_modules(g):
    lines = ["```mermaid", "graph LR"]
    order = [m for m in ["main", "modes", "core", "strategy", "api",
                         "universe", "risk", "ipc", "utils"] if m in g.mods]
    for m in g.mods:
        if m not in order:
            order.append(m)
    for m in order:
        lines.append(f"  {m}[{m}]")
    for (s, d), w in sorted(g.mod_edges.items()):
        lbl = f"|{w}|" if w > 1 else ""
        lines.append(f"  {s} -->{lbl} {d}")
    lines.append("```")
    return "\n".join(lines)


def render_mermaid_files(g):
    lines = ["```mermaid", "graph LR"]
    seen_nodes = set()

    def nid(label):
        return "n_" + re.sub(r"[^0-9A-Za-z]", "_", label)

    for m in g.mods:
        edges = sorted(g.file_edges.get(m, []))
        if not edges:
            continue
        lines.append(f"  subgraph {m}")
        for s in sorted({s for s, _ in edges}):
            if nid(s) not in seen_nodes:
                lines.append(f'    {nid(s)}["{s}"]')
                seen_nodes.add(nid(s))
        lines.append("  end")
    for m in g.mods:
        for s, inc in sorted(g.file_edges.get(m, [])):
            lines.append(f"  {nid(s)} --> {nid(inc)}")
    lines.append("```")
    return "\n".join(lines)


def render_dot(g):
    lines = ["digraph code_graph {", '  rankdir=LR;',
             '  node [shape=box, style=rounded, fontname="Consolas"];']
    for m in g.mods:
        lines.append(f'  "{m}";')
    for (s, d), w in sorted(g.mod_edges.items()):
        lines.append(f'  "{s}" -> "{d}" [label="{w}"];')
    lines.append("}")
    return "\n".join(lines)


def hubs(g, top=8):
    """유입(in-degree) 상위 헤더 = 공용 허브 = 재빌드 팬아웃 큰 지점."""
    counts = [(inc, len(labs)) for inc, labs in g.reverse.items()]
    counts.sort(key=lambda x: (-x[1], x[0]))
    return counts[:top]


def render_mermaid_python(py):
    lines = ["```mermaid", "graph LR"]
    nodes = sorted({e["from"] for e in py["package_edges"]} | {e["to"] for e in py["package_edges"]})

    def nid(x):
        return "p_" + re.sub(r"[^0-9A-Za-z]", "_", x)

    for n in nodes:
        lines.append(f'  {nid(n)}["{n}"]')
    for e in py["package_edges"]:
        lbl = f"|{e['weight']}|" if e["weight"] > 1 else ""
        lines.append(f"  {nid(e['from'])} -->{lbl} {nid(e['to'])}")
    lines.append("```")
    return "\n".join(lines)


def render_python_table(py):
    rows = ["| 파일 | 내부 import |", "|---|---|"]
    for lab, mods in py["edges"].items():
        rows.append(f"| `{lab}` | " + ", ".join(f"`{m}`" for m in mods) + " |")
    return "\n".join(rows)


def render_boundary_table(b):
    rows = ["| 파일 | 쓰는 쪽 | 읽는 쪽 | 언급만 |", "|---|---|---|---|"]
    for name, v in b.items():
        only = sorted(set(v["mentions"]) - set(v["writers"]) - set(v["readers"]))
        rows.append(f"| `{name}` | " + ", ".join(f"`{x}`" for x in v["writers"]) + " | "
                    + ", ".join(f"`{x}`" for x in v["readers"]) + " | "
                    + ", ".join(f"`{x}`" for x in only) + " |")
    return "\n".join(rows)


def to_json(g, py=None, boundary=None):
    out = {
        "modules": g.mods,
        "module_edges": [{"from": s, "to": d, "weight": w}
                         for (s, d), w in sorted(g.mod_edges.items())],
        "file_module": g.file_mod,
        "file_includes": {k: sorted(v) for k, v in sorted(g.file_includes.items())},
        "included_by": {k: sorted(v) for k, v in sorted(g.reverse.items())},
        "hubs": [{"header": h, "in_degree": c} for h, c in hubs(g, top=20)],
    }
    if py is not None:
        out["python"] = py
    if boundary is not None:
        out["boundary_files"] = boundary
    return out


def build_outputs(g):
    """md·dot·json 본문을 만든다. --check 는 이걸 파일과 비교만 한다."""
    py = scan_python()
    boundary = scan_boundary()
    md = []
    md.append("# 코드 의존 그래프 (Code Graph)")
    md.append("")
    md.append("> 자동 생성물. 손편집 금지 — 코드가 바뀌면 `py scripts/gen_code_graph.py` 로 재생성한다.")
    md.append("> `Quant/include`·`Quant/src` 의 로컬 `#include \"...\"` 관계에서 뽑았다. 표준/외부 헤더는 제외.")
    md.append("")
    md.append("## 모듈 의존 그래프")
    md.append("")
    md.append("화살표 A→B 는 \"모듈 A가 모듈 B의 헤더를 include 한다\". 숫자는 그런 include 파일 쌍의 수(의존 강도).")
    md.append("")
    md.append(render_mermaid_modules(g))
    md.append("")
    md.append("## 공용 허브 헤더 (재빌드 팬아웃)")
    md.append("")
    md.append("유입(누가 나를 include)이 많은 헤더. 한 줄만 바꿔도 아래 개수만큼 번역단위가 재컴파일된다.")
    md.append("증분빌드를 줄이려면 이 헤더를 얇게 유지한다(무거운 include를 전방선언/pimpl로 분리).")
    md.append("")
    md.append("| 헤더 | 유입 수 |")
    md.append("|---|---|")
    for h, c in hubs(g, top=8):
        md.append(f"| `{h}` | {c} |")
    md.append("")
    md.append("## 파일 단위 상세")
    md.append("")
    md.append("각 소스/헤더가 어떤 로컬 헤더를 include 하는지. 유입이 많은 노드(`utils/Logger.h`·`core/Types.h`)가 공용 허브다.")
    md.append("")
    md.append(render_mermaid_files(g))
    md.append("")
    md.append("## Python import 그래프")
    md.append("")
    md.append("`PYQuant/**`·`scripts/*.py` 의 내부 import만(표준·서드파티 제외). 화살표는 패키지 단위, 숫자는 파일 쌍 수.")
    md.append("")
    md.append(render_mermaid_python(py))
    md.append("")
    md.append(render_python_table(py))
    md.append("")
    md.append("## 프로세스 경계 파일")
    md.append("")
    md.append("C++ 엔진·Python 사이드카·스크립트가 파일로 주고받는 지점. 코드의 문자열 리터럴에서 찾았고,")
    md.append("읽기/쓰기는 리터럴 주변 줄의 힌트(ofstream·dump·read_text 등)로 분류했다. 힌트가 없으면 '언급만'.")
    md.append("")
    md.append(render_boundary_table(boundary))
    md.append("")
    md.append("## 영향범위 질의 · 기계 소비")
    md.append("")
    md.append("편집·커밋 전 영향범위(재검증/재빌드 대상)를 파일 열지 않고 뽑는다:")
    md.append("")
    md.append("```bash")
    md.append("py scripts/gen_code_graph.py --impact core/Types.h")
    md.append("py scripts/gen_code_graph.py --json   # docs/code_graph.json")
    md.append("```")
    md.append("")
    md.append("`docs/code_graph.dot` 도 생성했다. Graphviz가 있으면 SVG로 렌더할 수 있다.")
    md.append("")
    md.append("```bash")
    md.append("dot -Tsvg docs/code_graph.dot -o docs/code_graph.svg")
    md.append("```")
    md.append("")
    md_text = "\n".join(md) + "\n"
    dot_text = render_dot(g) + "\n"
    json_text = json.dumps(to_json(g, py, boundary), ensure_ascii=False, indent=2) + "\n"
    return md_text, dot_text, json_text


def _read_or_empty(path):
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            return f.read().replace("\r\n", "\n")
    except OSError:
        return ""


def check_outputs(g):
    """산출물이 지금 코드와 다르면 낡은 파일을 찍고 1."""
    md_text, dot_text, json_text = build_outputs(g)
    stale = []
    for path, want in ((OUT_MD, md_text), (OUT_DOT, dot_text), (OUT_JSON, json_text)):
        if _read_or_empty(path).rstrip("\n") != want.rstrip("\n"):
            stale.append(os.path.relpath(path, ROOT))
    if stale:
        for p in stale:
            print(f"[stale] {p}")
        print("py scripts/gen_code_graph.py --json 으로 재생성")
        return 1
    print("[ok] code graph 최신")
    return 0


def write_docs(g, want_json):
    md_text, dot_text, json_text = build_outputs(g)
    os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(md_text)
    with open(OUT_DOT, "w", encoding="utf-8") as f:
        f.write(dot_text)
    print(f"[ok] modules={len(g.mods)} module_edges={len(g.mod_edges)}")
    print(f"[ok] wrote {os.path.relpath(OUT_MD, ROOT)}")
    print(f"[ok] wrote {os.path.relpath(OUT_DOT, ROOT)}")
    if want_json:
        with open(OUT_JSON, "w", encoding="utf-8") as f:
            f.write(json_text)
        print(f"[ok] wrote {os.path.relpath(OUT_JSON, ROOT)}")


def main(argv):
    g = scan()
    if "--impact" in argv:
        i = argv.index("--impact")
        if i + 1 >= len(argv):
            print("[err] --impact 뒤에 헤더 경로가 필요합니다. 예: --impact core/Types.h")
            return 2
        return print_impact(g, argv[i + 1].replace("\\", "/"))
    if "--check" in argv:
        return check_outputs(g)
    write_docs(g, want_json=("--json" in argv))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
