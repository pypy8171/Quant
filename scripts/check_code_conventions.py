#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""코드 작업 규약을 스테이징된 변경에 대해 기계적으로 검사한다.

정본은 docs/guides/MAINTENANCE_AUTOMATION.md 4절(주석·표기 규약)과 .clang-format(중괄호)이다.
이 스크립트는 그중 사람이 놓치기 쉬운 일곱 가지만 본다.

  1. 중괄호   — 스테이징된 Quant/**.h/.cpp에 brace_style.py --check
  2. D-NNN    — 추가된 줄이 가리키는 결정 번호가 docs/DECISIONS.md에 실재하는지
  3. 태그 철자 — `// [xxx]` 중 규약에 없는 태그(경고만)
  4. 캐스트   — 추가된 C++ 코드 줄의 C스타일 캐스트. 값은 static_cast,
                포인터는 reinterpret_cast로 쓴다. `(void)x;`는 예외.
  5. 복사     — 안 해도 되는 복사 두 가지. json 노드를 통째로 베끼는
                `.value("k", json::array())`는 오류, 값 range-for는 경고.
  6. 줄 성격  — 파일별 추가·삭제를 주석과 코드로 나눠 보고.
                --comment-only를 주면 코드 줄 변경이 0이 아닐 때 실패한다.
  7. 약어 이름 — 추가된 코드 줄의 식별자가 약어(qty·cnt·idx…)거나 한 글자면 오류.
                판정 표는 리네임에 쓴 scripts/rename_maps/01_fields.json과
                scripts/rename_frags.py(FRAG·WHOLE·SKIP·WIRE)를 그대로 쓴다.
                C++은 std::·zmq:: 한정 이름, 문자열 리터럴, `[wire]` 줄, 대문자 한 글자(템플릿 인자)를 뺀다.
                .py는 정규식 대신 ast로 "그 파일이 이름을 붙이는 자리"(변수·인자·함수·클래스·import 별칭)만
                본다 — f-문자열 접두사·독스트링 본문·라이브러리 멤버(`frame.iloc`)가 식별자로 잘못 읽히지
                않게 하려는 것이다. 라이브러리 관례로 굳은 이름(PY_CONVENTION)은 예외.

주석 밀도는 검사하지 않는다. 4절이 밀도를 게이트로 걸지 말라고 정해 두었고,
집계는 maintain.py --weekly가 리포트로 남긴다.

출력은 `파일:줄: 종류: 내용`. exit 0 = 통과, 1 = 위반.

사용:
    py scripts/check_code_conventions.py                # 스테이징된 변경
    py scripts/check_code_conventions.py --comment-only # 주석 전용 커밋임을 검증
    py scripts/check_code_conventions.py --worktree     # 스테이징 대신 워킹트리
"""
from __future__ import annotations

import ast
import builtins
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import rename_frags  # noqa: E402  — 약어 판정 표의 정본(3단계). 1단계 표는 아래 FIELD_MAP.
import json as _json  # noqa: E402

# 1단계(필드·약어 94개: it→iterator, sym→symbol_id …) 표. 조각 단위가 아니라 이름 전체로 맞춘다.
FIELD_MAP: dict[str, str] = _json.loads((ROOT / "scripts" / "rename_maps" / "01_fields.json").read_text(encoding="utf-8"))

CPP_EXT = {".h", ".hpp", ".cpp", ".cc"}
HASH_EXT = {".py", ".ps1"}
PY_BUILTIN_NAMES = set(dir(builtins))
ALLOWED_TAGS = {"inv", "lock-order", "wire", "formula"}
WHY_TAG_RE = re.compile(r"^why D-\d{3}$")
TAG_RE = re.compile(r"//\s*\[([A-Za-z][A-Za-z0-9 _./-]*)\]")
DNNN_RE = re.compile(r"\bD-(\d{3})\b")

# C스타일 캐스트 — 캐스트로 읽히는 모양만 잡는다.
#  `)` 바로 뒤에 피연산자가 공백 없이 붙어야 캐스트다. 그래서 `void f(int)`,
#  `[](int) {`, `is_stale(int) const` 같은 선언부는 걸리지 않는다.
#  `(void)x;`는 미사용 인자 관용구라 타입 목록에서 뺐다.
CAST_TYPE = (r"(?:unsigned\s+(?:long\s+long|long|int|char|short)|long\s+long"
             r"|std::size_t|std::time_t|std::u?int(?:8|16|32|64)_t|u?int(?:8|16|32|64)_t"
             r"|size_t|ssize_t|time_t|int|long|short|double|float|char|bool)")
C_CAST_RE = re.compile(r"\((?:const\s+|volatile\s+)*" + CAST_TYPE + r"\s*\**\)(?=[A-Za-z_(&])")

# json 노드 깊은 복사 — `j.value("k", json::array())`는 기본값을 만들려고 노드 전체를 베낀다.
#  필드 한둘을 읽을 때도 배열·객체가 통째로 복사된다. find()로 노드 참조를 잡는 쪽이 맞다.
JSON_VALUE_COPY_RE = re.compile(
    r"\.value\s*\([^,()]*,\s*(?:[A-Za-z_][A-Za-z0-9_]*::)*json::(?:array|object)\s*\(\s*\)\s*\)")

# 값 range-for — 원소 타입을 알 수 없어 경고로만 남긴다(int·포인터면 복사가 공짜다).
#  `auto&`·`auto*`는 걸리지 않는다. 구조적 바인딩 `auto [k, v]`도 pair를 통째로 뜨므로 같이 본다.
VALUE_RANGE_FOR_RE = re.compile(
    r"\bfor\s*\(\s*(?:const\s+)?auto\s+(?:[A-Za-z_][A-Za-z0-9_]*\s*:|\[)")

# 약어 이름 — 판정은 rename_frags.new_name이 한다(이름이 바뀌면 약어). 리터럴·주석·한정 이름은 먼저 지운다.
STRING_LITERAL_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'')
LINE_COMMENT_RE = re.compile(r"//.*$")
IDENT_RE = re.compile(r"(?<![\w'])[A-Za-z_]\w*")
SINGLE_LETTER_RE = re.compile(r"^[a-z]$")


def abbreviation_hits(path: str, text: str) -> list[tuple[str, str]]:
    """추가된 코드 한 줄에서 (약어, 풀어 쓴 이름) 쌍을 돌려준다."""
    if "[wire]" in text or text.lstrip().startswith("#include"):
        return []

    code = LINE_COMMENT_RE.sub("", STRING_LITERAL_RE.sub('""', text))
    basename = Path(path).name
    hits = []

    for match in IDENT_RE.finditer(code):
        name = match.group(0)
        start = match.start()

        if rename_frags.qualified_root(code, start) in rename_frags.STD_ROOTS:
            continue

        is_member = start >= 1 and (code[start - 1] == "." or code[start - 2:start] == "->")

        if SINGLE_LETTER_RE.match(name):
            hits.append((name, "풀어 쓴 이름"))
            continue

        if is_member and name in rename_frags.MEMBER_SKIP:   # std::from_chars_result::ec 같은 표준 멤버
            continue

        renamed = FIELD_MAP.get(name) or rename_frags.new_name(name, basename, is_member)

        if renamed != name:
            hits.append((name, renamed))

    return hits


# 파이썬 관례로 굳어 풀어 쓰지 않는 이름 — TODO T-14의 예외 규정(라이브러리 관례·지표명·단위 접미사)을 이름으로 적는다.
PY_CONVENTION = {
    "_",                                        # 쓰지 않는 값
    "kwargs",                                   # **kwargs
    "np", "pd", "plt", "mdates", "dt", "ET",    # import 별칭 — 라이브러리 문서가 이 이름으로 설명한다
    "df",                                       # pandas 데이터프레임
    "ax", "fig",                                # matplotlib
    "pnl", "atr", "adx", "rsi", "vwap", "ohlc", # 지표명 — 풀어 쓰면 오히려 못 알아본다
}


def python_abbreviation_hits(path: str, source: str) -> dict[int, list[tuple[str, str]]]:
    """.py 원문에서 (줄번호 → [(약어, 풀어 쓴 이름)]). 그 파일이 새로 이름을 붙이는 자리만 본다."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return {}   # 문법 오류는 다른 게이트(파이썬 자체)가 잡는다

    basename = Path(path).name
    found: list[tuple[str, int]] = []

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            found.append((node.name, node.lineno))

            if not isinstance(node, ast.ClassDef):
                arguments = node.args
                every = (list(arguments.posonlyargs) + list(arguments.args) + list(arguments.kwonlyargs)
                         + [one for one in (arguments.vararg, arguments.kwarg) if one is not None])

                for argument in every:
                    found.append((argument.arg, argument.lineno))

        elif isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store, ast.Del)):
            found.append((node.id, node.lineno))
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                if alias.asname:
                    found.append((alias.asname, node.lineno))
        elif isinstance(node, ast.ExceptHandler) and node.name:
            found.append((node.name, node.lineno))

    hits: dict[int, list[tuple[str, str]]] = {}

    for name, lineno in found:
        if name in PY_CONVENTION or name in PY_BUILTIN_NAMES or name.startswith("__"):
            continue

        if SINGLE_LETTER_RE.match(name):
            hits.setdefault(lineno, []).append((name, "풀어 쓴 이름"))
            continue

        renamed = FIELD_MAP.get(name) or rename_frags.new_name(name, basename, False)

        if renamed != name:
            hits.setdefault(lineno, []).append((name, renamed))

    return hits


def file_source(path: str, staged: bool) -> str:
    """검사 대상 본문 — 스테이징 검사면 인덱스 내용, --worktree면 작업 트리 파일."""
    if staged:
        return git("show", f":{path}")

    target = ROOT / path
    return target.read_text(encoding="utf-8", errors="replace") if target.exists() else ""


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

    # 4. C스타일 캐스트 — 값은 static_cast, 포인터는 reinterpret_cast
    for path, items in adds.items():
        if Path(path).suffix.lower() not in CPP_EXT:
            continue

        for ln, text in items:
            if is_comment(path, text) is True:
                continue

            for hit in C_CAST_RE.finditer(text):
                print(f"{path}:{ln}: 오류: C스타일 캐스트 {hit.group(0)} — "
                      f"값은 static_cast<T>(x), 포인터는 reinterpret_cast<T>(x)로 쓴다")
                problems += 1

    # 5. 불필요한 복사 — json 노드 깊은 복사(오류)와 값 range-for(경고)
    for path, items in adds.items():
        if Path(path).suffix.lower() not in CPP_EXT:
            continue

        for ln, text in items:
            if is_comment(path, text) is True:
                continue

            for hit in JSON_VALUE_COPY_RE.finditer(text):
                print(f"{path}:{ln}: 오류: json 노드 깊은 복사 {hit.group(0).strip()} — "
                      f"find()로 노드 참조를 잡고 없을 때만 빈 노드를 가리킨다")
                problems += 1

            if VALUE_RANGE_FOR_RE.search(text):
                print(f"{path}:{ln}: 경고: 값 range-for — 원소가 문자열·컨테이너·구조체면 "
                      f"const auto& 로 받는다")

    # 7. 약어 이름 — 용어를 먼저 익히는 것이 우선이라 새 코드는 풀어 쓴다(4절 "이름 표기", D-092)
    for path, items in adds.items():
        if Path(path).suffix.lower() not in CPP_EXT:
            continue

        for ln, text in items:
            if is_comment(path, text) is True:
                continue

            for name, renamed in abbreviation_hits(path, text):
                print(f"{path}:{ln}: 오류: 약어 이름 `{name}` — `{renamed}`처럼 풀어 쓴다")
                problems += 1

    # 7-파이썬. 같은 규칙을 .py에 건다. 줄 단위 정규식이 아니라 파일 전체를 ast로 읽고
    #  추가된 줄에 걸린 이름만 고른다 — 이름을 붙이는 자리가 어디인지는 문법이 알려 준다.
    for path, items in adds.items():
        if Path(path).suffix.lower() != ".py":
            continue

        by_line = python_abbreviation_hits(path, file_source(path, staged))

        if not by_line:
            continue

        for lineno, _text in items:
            for name, renamed in by_line.get(lineno, []):
                print(f"{path}:{lineno}: 오류: 약어 이름 `{name}` — `{renamed}`처럼 풀어 쓴다")
                problems += 1

    # 6. 줄 성격 집계
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
