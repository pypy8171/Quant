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

Graphviz(dot)가 설치돼 있으면 docs/code_graph.dot 을 렌더링할 수 있다:
    dot -Tsvg docs/code_graph.dot -o docs/code_graph.svg
"""
import json
import os
import re
import sys
from collections import defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = [os.path.join(ROOT, "Quant", "include"), os.path.join(ROOT, "Quant", "src")]
OUT_MD = os.path.join(ROOT, "docs", "CODE_GRAPH.md")
OUT_DOT = os.path.join(ROOT, "docs", "code_graph.dot")
OUT_JSON = os.path.join(ROOT, "docs", "code_graph.json")

INCLUDE_RE = re.compile(r'#\s*include\s+"([^"]+)"')
MODULES = ["api", "core", "ipc", "modes", "risk", "strategy", "universe", "utils"]


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


def to_json(g):
    return {
        "modules": g.mods,
        "module_edges": [{"from": s, "to": d, "weight": w}
                         for (s, d), w in sorted(g.mod_edges.items())],
        "file_module": g.file_mod,
        "file_includes": {k: sorted(v) for k, v in sorted(g.file_includes.items())},
        "included_by": {k: sorted(v) for k, v in sorted(g.reverse.items())},
        "hubs": [{"header": h, "in_degree": c} for h, c in hubs(g, top=20)],
    }


def write_docs(g, want_json):
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
    md.append("## 영향범위 질의 · 기계 소비")
    md.append("")
    md.append("편집·커밋 전 영향범위(재검증/재빌드 대상)를 파일 열지 않고 뽑는다:")
    md.append("")
    md.append("```bash")
    md.append("py scripts/gen_code_graph.py --impact core/Types.h")
    md.append("py scripts/gen_code_graph.py --json   # docs/code_graph.json")
    md.append("```")
    md.append("")
    md.append("`docs/code_graph.dot` 도 생성했다(Graphviz 설치 시 `dot -Tsvg docs/code_graph.dot -o docs/code_graph.svg`).")
    md.append("")

    os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")
    with open(OUT_DOT, "w", encoding="utf-8") as f:
        f.write(render_dot(g) + "\n")
    print(f"[ok] modules={len(g.mods)} module_edges={len(g.mod_edges)}")
    print(f"[ok] wrote {os.path.relpath(OUT_MD, ROOT)}")
    print(f"[ok] wrote {os.path.relpath(OUT_DOT, ROOT)}")
    if want_json:
        with open(OUT_JSON, "w", encoding="utf-8") as f:
            json.dump(to_json(g), f, ensure_ascii=False, indent=2)
        print(f"[ok] wrote {os.path.relpath(OUT_JSON, ROOT)}")


def main(argv):
    g = scan()
    if "--impact" in argv:
        i = argv.index("--impact")
        if i + 1 >= len(argv):
            print("[err] --impact 뒤에 헤더 경로가 필요합니다. 예: --impact core/Types.h")
            return 2
        return print_impact(g, argv[i + 1].replace("\\", "/"))
    write_docs(g, want_json=("--json" in argv))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
