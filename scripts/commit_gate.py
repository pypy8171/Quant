#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""커밋 직전 게이트 — 스테이징된 변경과 커밋 메시지를 한 번에 검사한다.

`@committer` 서브에이전트가 도구 호출 수십 번으로 하던 검사(보안·문체·문서·코드 규약·재현성)를 스크립트 한 번으로
줄인 것이다(09-14 실측: 서브에이전트 커밋 1건 ≈ 1M 토큰). 승인 절차는 그대로다 — 메인 세션이 이 게이트를 돌리고,
커밋명·파일 목록을 사용자에게 보여 승인받은 뒤에야 `git commit`을 친다. PreToolUse 훅(`secret-gate.ps1`·
`docs-gate.ps1`)은 그 뒤에도 그대로 돌아 마지막 백스톱이 된다.

  py scripts/commit_gate.py                       # 스테이징 검사
  py scripts/commit_gate.py --msg-file .claude/commit_msg.txt   # 커밋 메시지까지
  py scripts/commit_gate.py --comment-only        # 주석 전용 커밋인지(코드 줄 0)까지

종료 코드: 0 통과 · 1 차단(커밋 금지) · 3 사람 판단이 남음(목록을 보고 계획에 판정을 적는다).

검사 규칙이 고쳐지다 무뎌지는 것을 막으려고, 돌 때마다 먼저 규칙마다 견본 한 줄을 넣어 잡히는지 확인한다(`selftest`).
견본이 안 잡히면 diff를 보기 전에 차단한다. 통과(0·3)하면 `.claude/commit-gate.state`에 스테이징 트리 해시를 적고,
훅 `secret-gate.ps1`이 `--hook`으로 이 스크립트를 불러 "같은 트리에서 게이트가 통과했는가"를 확인한다 — 게이트를
건너뛴 `git commit`은 훅이 막는다.

git에 올리면 안 되는 단어(가족 실명·회사명·구직 관련 낱말)는 이 파일에도 둘 수 없으니 `_private/gate_words.txt`
(gitignore, 한 줄 한 단어)에서 읽는다. 파일이 없으면 차단하고 먼저 만들게 한다 — 목록 없이 통과시키면 게이트가 아니다.
"""
from __future__ import annotations

import argparse
import io
import os
import pathlib
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")
ROOT = pathlib.Path(__file__).resolve().parent.parent
os.chdir(ROOT)

# ── 규칙 ────────────────────────────────────────────────────────────────────
BLOCKED_PATHS = re.compile(
    r"(^|/)(config\.json|config_[^/]*\.json|[^/]*token[^/]*\.json|\.token_cache[^/]*|[^/]*\.pem|[^/]*\.key|\.env|id_rsa[^/]*"
    r"|[^/]*\.parquet|[^/]*\.log)$|^\.claude/|^_private/")
BLOCKED_PATH_OK = re.compile(r"\.example$")
PLACEHOLDER = re.compile(r"YOUR_|CHANGEME|REDACTED|EXAMPLE|<[^>]*>|x{4,}|\.\.\.|0{6,}", re.I)
SECRET_ASSIGN = re.compile(
    r"(app_?key|app_?secret|approval_?key|appkey|appsecret|access[_-]?token|bearer|api[_-]?key|secret|password|passwd"
    r"|credential)\s*[\"':=]+\s*[\"']?([A-Za-z0-9+/=._-]{20,})", re.I)
PRIVATE_PATTERNS = [
    (re.compile("-" * 5 + "BEGIN"), "PEM/키 블록"),
    (re.compile(r"\b\d{8}-\d{2}\b"), "계좌번호(8-2) 모양"),
    (re.compile(r"\b\d{6}-[1-4]\d{6}\b"), "주민번호 모양"),
    (re.compile(r"01[016789]-?\d{3,4}-?\d{4}"), "휴대폰 번호 모양"),
    (re.compile(r"[A-Za-z0-9._%+-]+@(gmail|naver|daum|kakao|outlook|hotmail)\.com"), "개인 이메일"),
]
DISMISSIVE = re.compile(r"몰라도 되|몰라도 됨|몰라도 상관|안 ?봐도 되|알 필요 ?없|대충 봐도|안 외워도|몰라도 OK|skip해도")
STYLE_BUNDLES = [
    ("과장·단정·설교조·메타헤더", re.compile(
        r"허구다|증명한다|입증한다|구조적으로 (막|차단)|결정론적으로 (막|차단)|권위적으로|정직성 배너|정직 경계|표방한|지배당"
        r"|다름 아니다|왜 이(걸|것)|무엇을 (하나|안)|로 공개한다|상태: |핵심은|주목할|다시 말해")),
    ("대체어 표 위반", re.compile(
        r"신선도|스테일|캐비엇|양날의 검|사망|소멸|폭풍|스톰|폭주|좌초|출혈|위장|봉쇄|원천 ?차단|잠식|재앙")),
    ("영어치환", re.compile(r"바이오프|바이아웃|워크스루|바이브|프로브|딥다이브|온보딩|얼라인|디폴트로")),
]
COMMIT_TYPES = "feat|fix|refactor|docs|test|bench|build|chore"
TITLE = re.compile(rf"^({COMMIT_TYPES})\([^)]+\): \S")
BACKTEST_TRIGGER = re.compile(r"^research/studies/0[789]_|^PYQuant/data/(index_source|asof)\.py$")
GITIGNORE_GUARD = re.compile(r"config|token|_private|\.claude|\.env|\.pem|\.key")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout


def run(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.returncode, (p.stdout + p.stderr).strip()


def private_words() -> list[str]:
    p = ROOT / "_private" / "gate_words.txt"
    if not p.exists():
        raise SystemExit("[차단] _private/gate_words.txt가 없다 — 가족 실명·회사명·구직 낱말을 한 줄에 하나씩 적어 만든다")
    return [w.strip() for w in io.open(p, encoding="utf-8") if w.strip() and not w.startswith("#")]


class Report:
    def __init__(self):
        self.blocks: list[str] = []
        self.judge: list[str] = []
        self.ok: list[str] = []

    def block(self, s): self.blocks.append(s)
    def ask(self, s): self.judge.append(s)
    def passed(self, s): self.ok.append(s)


def added_lines(diff: str):
    """스테이징 diff의 추가 줄을 (파일, 줄 번호, 내용)으로 돌려준다."""
    path, ln = "", 0
    for raw in diff.splitlines():
        if raw.startswith("+++ "):
            path = raw[6:] if raw.startswith("+++ b/") else raw[4:]
        elif raw.startswith("@@"):
            m = re.search(r"\+(\d+)", raw)
            ln = int(m.group(1)) - 1 if m else 0
        elif raw.startswith("+"):
            ln += 1
            yield path, ln, raw[1:]
        elif not raw.startswith("-"):
            ln += 1


def scan_line(r: Report, path: str, where: str, line: str, words: list[str]) -> None:
    """추가된 줄 하나의 보안·문체 검사. selftest가 같은 함수를 견본으로 두드린다."""
    m = SECRET_ASSIGN.search(line)
    if m and not PLACEHOLDER.search(m.group(2)):
        r.block(f"실값으로 보이는 시크릿 할당 {where}: {line.strip()[:70]}")
    for pat, why in PRIVATE_PATTERNS:
        if pat.search(line):
            r.block(f"{why} {where}: {line.strip()[:70]}")
    for w in words:
        if w in line:
            r.block(f"비공개 단어 '{w}' {where}")
    if path.endswith(".md") and DISMISSIVE.search(line):
        r.ask(f"독자 무시 뉘앙스 {where}: {line.strip()[:70]} — 설계 서술(모듈이 내부 상태를 몰라도 됨)이면 통과")
    if path.endswith(".md"):
        for name, pat in STYLE_BUNDLES:
            m2 = pat.search(line)
            if m2:
                r.ask(f"문체({name}) '{m2.group(0)}' {where}: {line.strip()[:70]} — 기술 용어·식별자·인용이면 통과")


def selftest(words: list[str]) -> list[str]:
    """규칙마다 견본 한 줄을 넣어 잡히는지 본다. 못 잡는 규칙 이름을 돌려준다(비어 있으면 정상)."""
    fails: list[str] = []

    def caught(path: str, line: str, kind: str) -> bool:
        t = Report()
        scan_line(t, path, "selftest", line, words)
        return bool(t.blocks if kind == "block" else t.judge)

    # 견본은 조각을 이어 붙여 만든다 — 이 파일 자체가 게이트·훅에 걸리지 않도록.
    samples = [
        ("a.py", 'app_key = "PS' + 'a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6"', "block", "시크릿 실값"),
        ("a.py", "-" * 5 + "BEGIN RSA PRIVATE KEY" + "-" * 5, "block", "PEM 블록"),
        ("a.md", "계좌 12345678" + "-01 로", "block", "계좌번호"),
        ("a.md", "번호 900101" + "-1234567", "block", "주민번호"),
        ("a.md", "연락 010" + "-1234-5678", "block", "휴대폰"),
        ("a.md", "메일 someone" + "@gmail.com", "block", "개인 이메일"),
        ("a.md", "이건 몰라도 되는 부분", "judge", "독자 무시"),
        ("a.md", "이 결과는 허구다", "judge", "문체 과장"),
        ("a.md", "데이터 신선도가", "judge", "문체 대체어"),
        ("a.md", "온보딩 절차", "judge", "문체 영어치환"),
    ]
    for w in words:
        samples.append(("a.md", f"경로 x/{w}/y", "block", f"비공개 단어 '{w}'"))
    for path, line, kind, name in samples:
        if not caught(path, line, kind):
            fails.append(name)
    if not caught("a.py", 'app_key = "YOUR_APP_KEY_GOES_HERE_1234567"', "block") is False:
        fails.append("자리표시자 통과")
    for bad in ("Quant/config/config.json", "Quant/config/config_dev_paper.json", "x/kis_token_a.json", ".env",
                "a.pem", "_private/x.md", ".claude/settings.json", "logs/a.log", "d.parquet"):
        if not (BLOCKED_PATHS.search(bad) and not BLOCKED_PATH_OK.search(bad)):
            fails.append(f"금지 경로 {bad}")
    if BLOCKED_PATHS.search("Quant/config/config.json.example") and not BLOCKED_PATH_OK.search("Quant/config/config.json.example"):
        fails.append("템플릿 경로 통과")
    if not GITIGNORE_GUARD.search("Quant/config/config_*.json"):
        fails.append(".gitignore 보호 줄")
    if TITLE.match("업데이트: 뭔가 고침") or not TITLE.match("fix(core): 잘못된 동작을 고친다"):
        fails.append("커밋 제목 형식")
    return fails


def tree_hash() -> str:
    return git("write-tree").strip()


STATE = ROOT / ".claude" / "commit-gate.state"


def check_staged(r: Report, comment_only: bool) -> list[str]:
    staged = [s for s in git("diff", "--cached", "--name-only").splitlines() if s]
    if not staged:
        r.block("스테이징된 파일이 없다")
        return staged
    # (a) 파일명·경로
    for s in staged:
        if BLOCKED_PATHS.search(s) and not BLOCKED_PATH_OK.search(s):
            r.block(f"금지 경로가 스테이징됨: {s}")
    tracked_leak = [t for t in git("ls-files").splitlines()
                    if re.search(r"config\.json$|\.token|kis_token|\.env$|\.pem$|\.key$", t) and not t.endswith(".example")]
    for t in tracked_leak:
        r.block(f"추적 중인 시크릿 파일: {t} (git rm --cached)")
    # (a-2) 스테이징 뒤 또 바뀐 파일 — `git commit -- <경로>`(pathspec 커밋)는 인덱스가 아니라 작업 트리를 담는다.
    staged_paths = set(staged)
    restaged_needed = [path for path in git("diff", "--name-only").splitlines() if path in staged_paths]
    for path in restaged_needed:
        r.ask(f"스테이징 뒤 작업 트리가 또 바뀐 파일: {path} — 경로를 적어 커밋하면(`git commit -- <경로>`) 지금 검사한 인덱스가 아니라 그 작업 트리 내용이 들어간다. 경로 없이 커밋하거나 `git add`로 맞춘 뒤 게이트를 다시 돌린다")
    # (b) 내용
    diff = git("diff", "--cached", "-U0")
    words = private_words()
    for path, ln, line in added_lines(diff):
        scan_line(r, path, f"{path}:{ln}", line, words)
    # (c) .gitignore 약화
    if ".gitignore" in staged:
        removed = [l[1:] for l in git("diff", "--cached", "-U0", "--", ".gitignore").splitlines()
                   if l.startswith("-") and not l.startswith("---")]
        for l in removed:
            if GITIGNORE_GUARD.search(l):
                r.block(f".gitignore에서 보호 줄 삭제: {l.strip()}")
        for path, ln, line in added_lines(git("diff", "--cached", "-U0", "--", ".gitignore")):
            if any(w in line for w in words):
                r.block(f".gitignore에 비공개 단어: {line.strip()}")
    for s in staged:
        if s.endswith(".example"):
            r.ask(f"템플릿 {s}이 스테이징됨 — 내용이 자리표시자뿐인지 눈으로 확인")
    r.passed(f"보안 스캔(경로·시크릿·개인정보·비공개 단어 {len(words)}개)")
    # (d-2) 평이화 게이트
    # .toml 도 넣는다 — 시트 명세(docs/tuning_sheet.toml)의 뜻풀이 문장이 산문이라 그렇다(2026-09-19).
    md = [s for s in staged if s.endswith((".md", ".toml")) and os.path.exists(s)]
    data_islands = [s for s in staged if s in ("research/dashboard/reviews.json", "research/dashboard/live.json")]
    if md or data_islands:
        rc, out = run(["py", "scripts/check_plain_language.py", *md, *data_islands])
        if rc != 0:
            r.ask("check_plain_language.py 적발 — 목록을 사용자에게 보이고 승인 뒤 --fix(코드·데이터 줄은 제외):\n    "
                  + out.replace("\n", "\n    ")[:1500])
        else:
            r.passed("check_plain_language.py")
    # (d-3) 코드 규약
    if any(s.endswith((".h", ".cpp", ".py")) for s in staged):
        rc, out = run(["py", "scripts/check_code_conventions.py"])
        (r.block if rc else r.passed)("check_code_conventions.py" + ("" if rc == 0 else ":\n    " + out.replace("\n", "\n    ")[:1500]))
        if comment_only:
            rc, out = run(["py", "scripts/check_code_conventions.py", "--comment-only"])
            (r.block if rc else r.passed)("주석 전용(코드 줄 0)" + ("" if rc == 0 else ":\n    " + out[:800]))
    # (e) 문서 드리프트 + 동기화 + 색인 (docs-gate.ps1과 같은 검사)
    if md:
        rc, out = run(["py", "scripts/check_docs.py"])
        (r.block if rc else r.passed)("check_docs.py" + ("" if rc == 0 else ":\n    " + out.replace("\n", "\n    ")[:1500]))
    rc, out = run(["py", "scripts/sync_impact.py", "--diff", "--state", ".claude/sync-gate.state"])
    (r.block if rc else r.passed)("sync_impact.py --diff" + ("" if rc == 0 else ":\n    " + out.replace("\n", "\n    ")[:1500]))
    rc, out = run(["py", "scripts/file_index.py", "--check", "--staged"])
    (r.block if rc else r.passed)("file_index.py --check --staged" + ("" if rc == 0 else ":\n    " + out.replace("\n", "\n    ")[:1000]))
    # (f) 백테스트 재현성
    if any(BACKTEST_TRIGGER.search(s) for s in staged):
        rc, out = run(["py", "scripts/check_backtest.py"])
        (r.block if rc else r.passed)("check_backtest.py" + ("" if rc == 0 else ":\n    " + out.replace("\n", "\n    ")[:1500]))
    return staged


def check_message(r: Report, msg: str) -> None:
    lines = msg.strip().splitlines()
    if not lines:
        r.block("커밋 메시지가 비어 있다")
        return
    title = lines[0]
    if not TITLE.match(title):
        r.block(f"제목 형식이 `type(범위): 한국어 제목`이 아니다(type은 {COMMIT_TYPES}): {title}")
    if len(title) > 100:
        r.ask(f"제목이 {len(title)}자 — 줄일 수 있으면 줄인다")
    words = private_words()
    for i, line in enumerate(lines, 1):
        for pat, why in PRIVATE_PATTERNS:
            if pat.search(line):
                r.block(f"메시지 {i}행 {why}: {line[:70]}")
        if any(w in line for w in words):
            r.block(f"메시지 {i}행에 비공개 단어: {line[:70]}")
        for name, pat in STYLE_BUNDLES:
            m = pat.search(line)
            if m:
                r.ask(f"메시지 {i}행 문체({name}) '{m.group(0)}': {line[:70]}")
    p = subprocess.run(["py", "scripts/check_plain_language.py", "--stdin"], input=msg, capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        r.ask("커밋 메시지 평이화 적발:\n    " + (p.stdout + p.stderr).strip().replace("\n", "\n    ")[:800])
    else:
        r.passed("커밋 메시지 평이화")


def main() -> int:
    ap = argparse.ArgumentParser(description="커밋 직전 게이트(스테이징 + 커밋 메시지)")
    ap.add_argument("--msg-file", help="커밋 메시지 파일(제목·본문 형식과 단어를 검사)")
    ap.add_argument("--comment-only", action="store_true", help="주석 전용 커밋인지(코드 줄 0)까지 본다")
    ap.add_argument("--hook", action="store_true",
                    help="secret-gate.ps1이 부르는 모드 — 보안 스캔과 '같은 트리에서 게이트 통과' 확인만, 외부 검사기는 안 돈다")
    a = ap.parse_args()
    words = private_words()
    fails = selftest(words)
    if fails:
        print("[차단] 게이트 자기 시험 실패 — 견본을 못 잡는 규칙: " + ", ".join(fails))
        return 1
    r = Report()
    if a.hook:
        staged = [s for s in git("diff", "--cached", "--name-only").splitlines() if s]
        for s in staged:
            if BLOCKED_PATHS.search(s) and not BLOCKED_PATH_OK.search(s):
                r.block(f"금지 경로가 스테이징됨: {s}")
        for path, ln, line in added_lines(git("diff", "--cached", "-U0")):
            scan_line(r, path, f"{path}:{ln}", line, words)
        r.judge.clear()
        want = tree_hash()
        have = STATE.read_text(encoding="utf-8").strip() if STATE.exists() else ""
        if have != want:
            r.block("이 스테이징 트리에서 게이트가 통과한 기록이 없다 — `py scripts/commit_gate.py --msg-file <파일>`을 먼저 돌린다"
                    + (" (스테이징이 그 뒤 바뀜)" if have else ""))
        for s in r.blocks:
            print(f"[차단] {s}")
        return 1 if r.blocks else 0
    staged = check_staged(r, a.comment_only)
    if a.msg_file:
        check_message(r, io.open(a.msg_file, encoding="utf-8").read())
    print(f"스테이징 {len(staged)}개: " + ", ".join(staged[:12]) + (" …" if len(staged) > 12 else ""))
    for s in r.ok:
        print(f"[통과] {s}")
    for s in r.judge:
        print(f"[확인] {s}")
    for s in r.blocks:
        print(f"[차단] {s}")
    if r.blocks:
        print(f"\n차단 {len(r.blocks)}건 — 커밋하지 않는다")
        return 1
    STATE.parent.mkdir(exist_ok=True)
    STATE.write_text(tree_hash() + "\n", encoding="utf-8")
    if r.judge:
        print(f"\n판단 {len(r.judge)}건 — 계획에 판정(통과/고침)을 적고 승인받는다")
        return 3
    print("\n통과 — 커밋명·파일 목록을 사용자에게 보이고 승인 뒤 커밋한다")
    return 0


if __name__ == "__main__":
    sys.exit(main())
