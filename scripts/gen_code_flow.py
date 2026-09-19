# -*- coding: utf-8 -*-
"""실시간 매매 코드 흐름 문서 생성기 — docs/code_flow.toml → docs/CODE_FLOW.md (D-078).

읽는 순서(단계·걸음·볼 것)는 사람이 docs/code_flow.toml에 적고, 줄 번호·시그니처는 이 스크립트가 소스에서
찾아 채운다. 그래서 코드가 옮겨가도 문서의 링크는 그 줄을 따라가고, 심볼이 사라지면 --check가 막아
명세를 고치게 한다. 손으로 쓴 흐름 문서가 낡는 문제(docs/guides/PIPELINE_A_to_Z.md)를 이렇게 피한다.

심볼을 찾는 규칙:
  - `A::B::name` 은 마지막 조각 `name(`으로 후보 줄을 모으고, `.cpp`면 `B::name(` 정의 줄을 우선한다.
  - 후보 중 호출처럼 보이는 줄(`.name(`·`->name(`·`return name(`·`= name(`)은 버리고, 남은 것 중 들여쓰기가
    가장 얕은 줄을 고른다 — 정의·선언은 호출보다 바깥에 있다.
  - 함수로 못 찾으면 `struct|class|enum name` 타입 선언을 찾는다.
  - 걸음에 `match` 정규식이 있으면 위 규칙 대신 그 첫 매치 줄을 쓴다(람다·블록처럼 이름 없는 자리).

사용:
    py scripts/gen_code_flow.py            # docs/CODE_FLOW.md 다시 쓴다
    py scripts/gen_code_flow.py --check    # 심볼 누락·문서 낡음이면 exit 1 (sync-gate·docs-gate가 부른다)
"""
import argparse
import os
import re
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(ROOT, "docs", "code_flow.toml")
OUT = os.path.join(ROOT, "docs", "CODE_FLOW.md")
TESTS_DIR = "Quant/tests"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# 호출 자리에서 이름 앞에 오는 것들 — 이런 줄은 정의·선언이 아니다
_CALL_PREFIX = re.compile(r"(\.|->|\breturn\s+|=\s*|!\s*|\(\s*|,\s*|&&\s*|\|\|\s*|\bif\s*\(\s*|\bwhile\s*\(\s*)$")


def read_lines(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8-sig", errors="ignore") as f:
        return f.read().splitlines()


def indent_of(line):
    return len(line) - len(line.lstrip(" \t"))


def find_symbol(lines, sym, rel):
    """(줄 번호 1-based, 시그니처 문자열) 또는 None."""
    parts = sym.split("::")
    name = parts[-1]
    qual = parts[-2] if len(parts) >= 2 else None
    # 이 저장소는 함수가 snake_case, 타입이 PascalCase다 — 대문자로 시작하면 생성자 `Name(`보다 `class Name`을 먼저 본다
    if name[:1].isupper():
        ty = find_type(lines, name)
        if ty:
            return ty
    fn_re = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(")
    cands = []
    for i, ln in enumerate(lines):
        stripped = ln.strip()
        if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
            continue
        m = fn_re.search(ln)
        if not m:
            continue
        before = ln[: m.start()].rstrip()
        if _CALL_PREFIX.search(before + " ") and not before.endswith("::"):
            continue
        cands.append(i)

    if cands and qual and rel.endswith(".cpp"):
        # 정의 줄 `Qual::name(` 우선 — 헤더의 선언이 아니라 본문으로 보낸다
        def_re = re.compile(re.escape(qual) + r"::" + re.escape(name) + r"\s*\(")
        defs = [i for i in cands if def_re.search(lines[i])]
        if defs:
            cands = defs

    if cands:
        best = min(cands, key=lambda i: (indent_of(lines[i]), i))
        return best + 1, signature(lines, best)

    return find_type(lines, name)


def find_type(lines, name):
    ty_re = re.compile(r"\b(struct|class|enum\s+class|enum|namespace)\s+(alignas\([^)]*\)\s+)?" + re.escape(name) + r"\b")
    for i, ln in enumerate(lines):
        if ty_re.search(ln) and not ln.strip().startswith("//"):
            return i + 1, signature(lines, i)
    return None


def signature(lines, i):
    """매치 줄을 한 줄로 — 반환형이 앞줄에 따로 있으면 붙이고, 인자가 다음 줄로 이어지면 `…`로 끊는다."""
    s = lines[i].strip()
    if i > 0:
        prev = lines[i - 1].strip()
        # `std::optional<X>` 처럼 반환형만 놓인 앞줄
        if prev and not prev.endswith((";", "{", "}", ")", ",", "//")) and not prev.startswith(("//", "#", "[[")) \
                and "(" not in prev and len(prev) < 60 and lines[i][:1] not in (" ", "\t"):
            s = prev + " " + s
    s = re.sub(r"\s*//.*$", "", s)
    s = re.sub(r"\s*\{\s*$", "", s)
    if s.count("(") > s.count(")"):
        s += " …"
    if len(s) > 120:
        s = s[:117] + "…"
    # 람다 캡처 `[this](`는 백틱 안이라도 check_docs가 링크로 읽는다 — 공백 하나로 떼어 둔다
    s = re.sub(r"\]\(", "] (", s)
    return s.replace("|", "\\|")


def find_match(lines, pattern):
    rx = re.compile(pattern)
    for i, ln in enumerate(lines):
        if rx.search(ln) and not ln.strip().startswith("//"):
            return i + 1, signature(lines, i)
    return None


def rel_from_docs(rel):
    return "../" + rel.replace("\\", "/")


def render(spec):
    errors = []
    out = []
    out.append(f"# {spec['title']}")
    out.append("")
    # check_code_refs는 머리 5줄 안의 이 표식을 보고 줄번호 검사를 건너뛴다 — 여기 줄번호는 생성기가 소스에서 찍는다
    out.append("<!-- drift-check: snapshot — 줄 번호는 생성 시점 소스 기준, scripts/gen_code_flow.py가 다시 만든다 -->")
    out.append("> 자동 생성물. 손편집 금지 — 읽는 순서는 `docs/code_flow.toml`에 적고 `py scripts/gen_code_flow.py`로 다시 만든다.")
    out.append("> 줄 번호·시그니처는 생성 시점의 소스에서 찍었다. 심볼이 사라지면 `--check`가 막는다(D-078).")
    out.append("")
    out.append(spec["intro"].strip())
    out.append("")
    out.append("## 한 장 그림")
    out.append("")
    out.append("```mermaid")
    out.append(spec["diagram"].strip())
    out.append("```")
    out.append("")
    out.append("## 읽는 순서")
    out.append("")
    stages = spec["stage"]
    for st in stages:
        out.append(f"- [{st['name']}](#{anchor(st['name'])}) — {len(st.get('step', []))}걸음")
    out.append("")

    by_file = {}
    n = 0
    for st in stages:
        out.append(f"## {st['name']}")
        out.append("")
        out.append(st["summary"].strip())
        out.append("")
        for step in st.get("step", []):
            n += 1
            rel = step["file"].replace("\\", "/")
            lines = read_lines(rel)
            label = step["sym"]
            if lines is None:
                errors.append(f"[missing-file] {rel} ({label})")
                loc = None
            elif step.get("match"):
                loc = find_match(lines, step["match"])
                if loc is None:
                    errors.append(f"[missing] {rel}: match=/{step['match']}/ ({label})")
            else:
                loc = find_symbol(lines, step["sym"], rel)
                if loc is None:
                    errors.append(f"[missing] {rel}: {label}")
            if loc:
                line_no, sig = loc
                link = f"[`{label}`]({rel_from_docs(rel)}#L{line_no})"
                where = f"`{rel}:{line_no}`"
            else:
                link = f"`{label}`"
                where = f"`{rel}` (찾지 못함)"
                sig = ""
            out.append(f"{n}. {link} — {step['what']}  ")
            tail = f"   {where}"
            if sig:
                tail += f" · `{sig}`"
            if step.get("test"):
                t = step["test"]
                tail += f" · 시험 [{t}]({rel_from_docs(TESTS_DIR + '/' + t + '.cpp')})"
            out.append(tail)
            by_file.setdefault(rel, []).append((n, label))
        if st.get("review"):
            out.append("")
            out.append("리뷰할 때 볼 것:")
            out.append("")
            for r in st["review"]:
                out.append(f"- {r}")
        out.append("")

    out.append("## 파일별 — 한 파일을 열었을 때 이 문서의 어느 걸음인지")
    out.append("")
    out.append("| 파일 | 걸음 |")
    out.append("|---|---|")
    for rel in sorted(by_file):
        steps = ", ".join(f"{k} `{lbl.split('::')[-1] if '::' in lbl else lbl}`" for k, lbl in by_file[rel])
        out.append(f"| [{rel}]({rel_from_docs(rel)}) | {steps} |")
    out.append("")
    out.append("## 이 문서를 고치는 법")
    out.append("")
    out.append("- 걸음을 더하거나 순서를 바꾼다: `docs/code_flow.toml`의 `[[stage.step]]`. 심볼과 파일만 적으면 줄은 생성기가 찾는다.")
    out.append("- 함수 이름이 바뀌어 `[missing]`이 나면: 명세의 `sym`을 새 이름으로. 자리가 없어졌으면 걸음을 지운다.")
    out.append("- 이름 없는 자리(람다·블록)는 `match` 정규식으로 가리킨다.")
    out.append("- 그림(`diagram`)과 요약(`summary`)은 사람이 쓴다 — 스레드·큐가 바뀌면 같은 커밋에서 고친다.")
    out.append("")
    return "\n".join(out) + "\n", errors


def anchor(title):
    # GitHub·VS Code 식 제목 링크 id — 공백은 -, 구두점은 뺀다
    a = title.strip().lower()
    a = re.sub(r"[^\w\s\-가-힣]", "", a)
    return re.sub(r"\s+", "-", a)


def main():
    ap = argparse.ArgumentParser(description="docs/code_flow.toml → docs/CODE_FLOW.md")
    ap.add_argument("--check", action="store_true", help="심볼 누락·문서 낡음이면 exit 1, 쓰지 않는다")
    args = ap.parse_args()

    with open(SPEC, "rb") as f:
        spec = tomllib.load(f)
    text, errors = render(spec)

    if errors:
        for e in errors:
            print(e)
        print(f"docs/code_flow.toml: 찾지 못한 심볼 {len(errors)}개 → sym·file·match를 고친다", file=sys.stderr)
        if args.check:
            return 1

    if args.check:
        cur = ""
        if os.path.isfile(OUT):
            with open(OUT, "r", encoding="utf-8") as f:
                cur = f.read()
        if cur != text:
            print("docs/CODE_FLOW.md 낡음 → `py scripts/gen_code_flow.py`")
            return 1
        print("docs/CODE_FLOW.md 최신")
        return 0

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    n = sum(len(s.get("step", [])) for s in spec["stage"])
    print(f"docs/CODE_FLOW.md 생성 — 단계 {len(spec['stage'])}, 걸음 {n}, 누락 {len(errors)}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
