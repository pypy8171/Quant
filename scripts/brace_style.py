"""C++ 제어문 중괄호·빈 줄 규칙을 기계적으로 적용한다.

규칙(동작은 바꾸지 않는다):
  1. if/else/for/while 본문이 한 문장이어도 중괄호를 넣는다(Allman: 여는 괄호는 다음 줄).
  2. `for (;;)`·`while (1)`은 `while (true)`로 바꾼다.
  3. 닫는 `}` 뒤와 제어문 앞에 빈 줄을 하나 둔다(`else`·`#`·`case`·`catch`·do-while은 예외).

사용: py scripts/brace_style.py [--check] [--report] 파일...
      파일을 주지 않으면 Quant/include Quant/src Quant/tests의 .h/.cpp 전부.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CTRL_RE = re.compile(r"^(?P<pre>\}\s*)?(?P<kw>else\s+if|if|for|while|switch)\b(?:\s+constexpr)?\s*\(")
ELSE_RE = re.compile(r"^(?P<pre>\}\s*)?else\b(?!\s*if\b)(?P<rest>.*)$")
BLOCK_START_RE = re.compile(r"^(if|for|while|switch|do|try)\b")
LOOP_FOREVER_RE = re.compile(r"\b(for\s*\(\s*;\s*;\s*\)|while\s*\(\s*1\s*\))")


def _is_digit_sep(code: str, i: int) -> bool:
    """`1'000'000` 같은 자릿수 구분자인지(문자 리터럴이 아닌지)."""
    return (
        code[i] == "'"
        and i > 0
        and code[i - 1].isalnum()
        and i + 1 < len(code)
        and code[i + 1].isdigit()
    )


def scan(code: str, start: int = 0):
    """문자열·문자 리터럴 밖의 (열, 문자)만 낸다."""
    i, n = start, len(code)
    in_str = None
    while i < n:
        c = code[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c == '"' or (c == "'" and not _is_digit_sep(code, i)):
            in_str = c
            i += 1
            continue
        yield i, c
        i += 1


def split_code_comment(line: str) -> tuple[str, str]:
    """코드 부분과 행말 주석을 나눈다. 문자열 리터럴 안의 // 는 무시한다."""
    while True:
        for i, c in scan(line):
            if c == "/" and i + 1 < len(line) and line[i + 1] == "/":
                return line[:i], line[i:]
            if c == "/" and i + 1 < len(line) and line[i + 1] == "*":
                j = line.find("*/", i + 2)
                if j < 0:
                    return line[:i], line[i:]
                line = line[:i] + " " * (j + 2 - i) + line[j + 2:]
                break
        else:
            return line, ""


def depth_delta(code: str) -> tuple[int, int, bool]:
    """괄호·중괄호 깊이 변화와 `;`로 끝나는지를 문자열 리터럴을 건너뛰며 센다."""
    paren = brace = 0
    last_sig = ""
    for _, c in scan(code):
        if c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        elif c == "{":
            brace += 1
        elif c == "}":
            brace -= 1
        if not c.isspace():
            last_sig = c
    return paren, brace, last_sig == ";"


class Fixer:
    def __init__(self, lines: list[str]):
        self.lines = lines
        self.notes: list[str] = []
        self.changed = False

    # ── 유틸 ────────────────────────────────────────────────────────────
    def code(self, i: int) -> str:
        return split_code_comment(self.lines[i])[0].strip()

    def indent(self, i: int) -> str:
        line = self.lines[i]
        return line[: len(line) - len(line.lstrip())]

    def next_nonblank(self, i: int) -> int:
        j = i + 1
        while j < len(self.lines) and not self.lines[j].strip():
            j += 1
        return j

    def header_end(self, i: int, start_col: int) -> tuple[int, int] | None:
        """`(`에서 시작해 짝이 맞는 `)`의 (줄, 열)을 돌려준다."""
        depth = 0
        j = i
        col = start_col
        while j < len(self.lines):
            code, _ = split_code_comment(self.lines[j])
            for k, c in scan(code, col):
                if c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                    if depth == 0:
                        return j, k
            j += 1
            col = 0
        return None

    def statement_end(self, k: int) -> int | None:
        """k에서 시작하는 한 문장의 마지막 줄. 앞의 주석 줄은 건너뛴다."""
        while k < len(self.lines) and (
            not self.lines[k].strip() or self.lines[k].lstrip().startswith("//")
        ):
            k += 1
        if k >= len(self.lines):
            return None
        code = self.code(k)
        if code.startswith("#"):
            return None
        if BLOCK_START_RE.match(code) or code.startswith("else"):
            end = self.block_end(k)
            if end is None:
                return None
            # if 뒤에 else 체인이 이어지면 문장은 거기까지다.
            while True:
                nxt = self.next_nonblank(end)
                if nxt < len(self.lines) and re.match(r"^\}?\s*else\b", self.code(nxt)):
                    end2 = self.block_end(nxt)
                    if end2 is None:
                        return None
                    end = end2
                else:
                    return end
        paren = brace = 0
        j = k
        while j < len(self.lines):
            c = split_code_comment(self.lines[j])[0]
            p, b, semi = depth_delta(c)
            paren += p
            brace += b
            if paren <= 0 and brace <= 0 and semi:
                return j
            if paren < 0 or brace < 0:
                return None
            j += 1
        return None

    def block_end(self, k: int) -> int | None:
        """제어문(중괄호 본문)이 끝나는 `}` 줄. 본문이 중괄호가 아니면 None."""
        j = k
        brace = 0
        seen_open = False
        while j < len(self.lines):
            c = split_code_comment(self.lines[j])[0]
            _, b, _ = depth_delta(c)
            if "{" in c:
                seen_open = True
            brace += b
            if seen_open and brace == 0:
                return j
            if not seen_open and j > k + 1 and self.code(j).endswith(";") and "{" not in c:
                return None  # 중괄호 없는 본문 — 먼저 감싸야 한다
            j += 1
        return None

    # ── 1. 중괄호 삽입 (아래에서 위로: 안쪽 문장이 먼저 감싸진다) ──────────
    def wrap(self, header_line: int, body_start: int, ind: str) -> bool:
        end = self.statement_end(body_start)
        if end is None:
            self.notes.append(f"{header_line + 1}: 본문 범위를 못 정해 건너뜀")
            return False
        self.lines.insert(end + 1, ind + "}")
        self.lines.insert(header_line + 1, ind + "{")
        self.changed = True
        return True

    def split_same_line(self, i: int, cut: int, ind: str) -> None:
        """`if (c) stmt;` 한 줄을 헤더/본문으로 나눈 뒤 감싼다."""
        code, comment = split_code_comment(self.lines[i])
        head = code[: cut + 1].rstrip()
        body = code[cut + 1:].strip()
        body_line = ind + "    " + body + ("  " + comment.strip() if comment.strip() else "")
        self.lines[i] = head
        self.lines.insert(i + 1, body_line)
        if BLOCK_START_RE.match(body) or body.startswith("else"):
            self.fix_line(i + 1)
        self.wrap(i, i + 1, ind)

    def fix_braces(self) -> None:
        i = len(self.lines) - 1
        while i >= 0:
            self.fix_line(i)
            i -= 1

    def fix_line(self, i: int) -> None:
        raw = self.lines[i]
        code = self.code(i)
        if not code or code.startswith("#") or raw.rstrip().endswith("\\"):
            return
        ind = self.indent(i)
        m = CTRL_RE.match(code)
        if m:
            col = split_code_comment(raw)[0].find("(", raw.find(m.group("kw")))
            he = self.header_end(i, col)
            if he is None:
                self.notes.append(f"{i + 1}: 괄호 짝을 못 찾음")
                return
            j, k = he
            rest = split_code_comment(self.lines[j])[0][k + 1:].strip()
            if rest.startswith("{") or rest == ";":
                return
            # `} while (0)` 뒤에 `;`가 없으면 매크로의 do-while 꼬리다(호출부가 `;`를 붙인다).
            #  제어문으로 보면 다음 문장을 중괄호로 감싸 파일을 깨뜨린다(09-12 test_ledger_reconciler 실측).
            if m.group("pre") and m.group("kw") == "while" and not rest:
                return
            if rest:
                if m.group("kw") == "switch":
                    return
                if j != i:
                    self.notes.append(f"{i + 1}: 여러 줄 헤더 뒤 같은 줄 본문 — 수동 확인")
                    return
                self.split_same_line(i, k, ind)
                return
            nxt = self.next_nonblank(j)
            if nxt >= len(self.lines) or self.code(nxt).startswith("{"):
                return
            self.wrap(j, nxt, ind)
            return
        m = ELSE_RE.match(code)
        if m:
            rest = m.group("rest").strip()
            if rest.startswith("{"):
                return
            if rest:
                cut = split_code_comment(raw)[0].find("else") + 3
                self.split_same_line(i, cut, ind)
                return
            nxt = self.next_nonblank(i)
            if nxt < len(self.lines) and not self.code(nxt).startswith("{"):
                self.wrap(i, nxt, ind)

    # ── 2. for(;;) / while(1) ───────────────────────────────────────────
    def fix_forever(self) -> None:
        for i, line in enumerate(self.lines):
            code, comment = split_code_comment(line)
            if LOOP_FOREVER_RE.search(code):
                self.lines[i] = LOOP_FOREVER_RE.sub("while (true)", code) + comment
                self.changed = True

    # ── 3. 빈 줄 ────────────────────────────────────────────────────────
    def fix_blank_lines(self) -> None:
        out: list[str] = []
        n = len(self.lines)
        for i, line in enumerate(self.lines):
            stripped = line.strip()
            code = self.code(i)
            # 제어문 앞: 바로 위 주석 블록까지 거슬러 올라가 그 앞에 빈 줄을 둔다.
            if BLOCK_START_RE.match(code) and self.indent(i):
                k = len(out) - 1
                while k >= 0 and out[k].lstrip().startswith("//"):
                    k -= 1
                if k >= 0:
                    prev = split_code_comment(out[k])[0].strip()
                    if (
                        prev
                        and not prev.endswith("{")
                        and prev != "else"
                        and not prev.endswith(":")
                        and not prev.startswith("#")
                        and not out[k].rstrip().endswith("\\")
                        and not (k == len(out) - 1 and not out[k].strip())
                    ):
                        out.insert(k + 1, "")
                        self.changed = True
            out.append(line)
            # 닫는 중괄호 뒤
            if stripped == "}" or (code == "}" and stripped.startswith("}")):
                nxt = self.lines[i + 1] if i + 1 < n else None
                if nxt is not None and nxt.strip():
                    ns = nxt.lstrip()
                    if not (
                        ns.startswith("}")
                        or ns.startswith("else")
                        or ns.startswith("#")
                        or ns.startswith("case ")
                        or ns.startswith("default:")
                        or ns.startswith("catch")
                        or ns.startswith("while")
                        or ns.startswith(")")
                        or ns.startswith(",")
                        or ns.startswith(";")
                    ):
                        out.append("")
                        self.changed = True
        self.lines = out


def process(path: Path, check: bool) -> tuple[bool, list[str]]:
    raw = path.read_bytes()
    nl = "\r\n" if b"\r\n" in raw else "\n"
    text = raw.decode("utf-8-sig" if raw.startswith(b"\xef\xbb\xbf") else "utf-8")
    bom = raw.startswith(b"\xef\xbb\xbf")
    lines = text.split("\n")
    lines = [l.rstrip("\r") for l in lines]
    fx = Fixer(lines)
    fx.fix_braces()
    fx.fix_forever()
    fx.fix_blank_lines()
    if fx.changed and not check:
        out = nl.join(fx.lines)
        path.write_bytes((b"\xef\xbb\xbf" if bom else b"") + out.encode("utf-8"))
    return fx.changed, fx.notes


def main(argv: list[str]) -> int:
    check = "--check" in argv
    report = "--report" in argv
    files = [Path(a) for a in argv if not a.startswith("--")]
    if not files:
        for d in ("Quant/include", "Quant/src", "Quant/tests"):
            files += sorted((ROOT / d).rglob("*.h")) + sorted((ROOT / d).rglob("*.cpp"))
    changed = 0
    for f in files:
        ch, notes = process(f, check)
        rel = f.resolve().relative_to(ROOT) if f.resolve().is_relative_to(ROOT) else f
        if ch:
            changed += 1
            print(f"{'would change' if check else 'changed'}: {rel}")
        if report or notes:
            for n in notes:
                print(f"  {rel}:{n}")
    print(f"{changed}/{len(files)} files")
    return 1 if (check and changed) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
