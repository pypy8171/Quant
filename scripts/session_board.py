#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""살아 있는 Claude 세션의 현황판 — 이름·브랜치·문맥·턴·압축·마지막 요청·현황판 줄·인계 파일.

세션이 셋 넘게 돌면 어느 세션이 문맥을 얼마나 썼고 인계 시점이 언제인지 아무도 모른다(09-14: 압축 문맥 중앙값
166K, 첫 압축 88턴). 이 스크립트가 하네스가 남기는 파일만 읽어 한 표로 만든다. 코드를 바꾸는 것은 없다.

원천(전부 읽기 전용):
  ~/.claude/sessions/<pid>.json                      살아 있는 세션의 name·sessionId·cwd·status·startedAt
  ~/.claude/projects/<slug>/<sessionId>.jsonl        assistant 레코드 usage(문맥), requestId(턴), compact_boundary(압축)
  _private/SESSION_CLAIMS.md                         세션 이름으로 줄 매칭
  _private/HANDOFF_<이름>.md                         인계 파일 유무

  py scripts/session_board.py                 # 표를 찍고 _private/session_board.json·.html을 다시 쓴다
  py scripts/session_board.py --quiet         # 파일만(Stop 훅용)
  py scripts/session_board.py --session <id>  # 한 세션만 JSON으로(훅이 stdin session_id를 넘길 때)
  py scripts/session_board.py --facts <id>    # 인계 파일 "기계적 사실" 절 본문(PreCompact 안전망용)

jsonl은 수십 MB까지 자라므로 통째로 읽지 않는다. 세션별로 마지막으로 읽은 바이트 위치와 그때까지의 집계를
`_private/session_board.state.json`에 두고, 늘어난 꼬리만 이어 읽는다. 상태 파일이 없거나 파일이 줄어들었으면 처음부터.
"""
from __future__ import annotations

import argparse
import html
import io
import json
import os
import pathlib
import re
import subprocess
import sys
import time
from datetime import datetime

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
ROOT = pathlib.Path(__file__).resolve().parent.parent
HOME = pathlib.Path.home()
SESSIONS_DIR = HOME / ".claude" / "sessions"
PRIVATE = ROOT / "_private"
CLAIMS = PRIVATE / "SESSION_CLAIMS.md"
OUT_JSON = PRIVATE / "session_board.json"
OUT_HTML = PRIVATE / "session_board.html"
STATE = PRIVATE / "session_board.state.json"

CONTEXT_WINDOW = 200_000      # 모델 문맥 창. 비율 색은 이 값 기준이다
WARN_PCT, DANGER_PCT = 50, 80  # 노랑·빨강 경계(%)
HANDOFF_CTX = 100_000          # 이 문맥을 넘긴 세션은 다음 작업 경계에서 인계한다(.claude/commands/handoff.md)
REQUEST_KEEP = 5               # 사용자 요청은 마지막 다섯 개만 기억한다
REQUEST_CHARS = 80


def project_slug(cwd: str) -> str:
    """cwd → ~/.claude/projects/ 아래 폴더 이름. 영숫자 아닌 글자는 전부 '-'."""
    return re.sub(r"[^A-Za-z0-9]", "-", cwd)


def transcript_path(cwd: str, session_id: str) -> pathlib.Path:
    return HOME / ".claude" / "projects" / project_slug(cwd) / f"{session_id}.jsonl"


def same_repo(cwd: str) -> bool:
    try:
        return pathlib.Path(cwd).resolve() == ROOT
    except OSError:
        return False


def live_sessions() -> list[dict]:
    """sessions/<pid>.json 중 이 저장소를 cwd로 쓰는 것. 죽은 프로세스의 파일은 하네스가 지우므로 pid는 따로 보지 않는다."""
    out = []
    for f in sorted(SESSIONS_DIR.glob("*.json")):
        try:
            o = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if not same_repo(o.get("cwd", "")):
            continue
        out.append(o)
    return out


def session_by_id(session_id: str) -> dict | None:
    for o in live_sessions():
        if o.get("sessionId") == session_id:
            return o
    return None


# ── jsonl 증분 집계 ─────────────────────────────────────────────────────────
def _empty_acc() -> dict:
    return {"offset": 0, "turns": 0, "requests": [], "compacts": 0, "compact_turns": [],
            "ctx": 0, "last_request_id": "", "user_requests": [], "branch": "", "last_ts": ""}


def _user_text(o: dict) -> str | None:
    """진짜 사용자 요청만. 도구 결과·메타·슬래시 명령 캡션은 건너뛴다."""
    if o.get("isMeta") or o.get("isSidechain"):
        return None
    c = (o.get("message") or {}).get("content")
    if isinstance(c, list):
        parts = [b.get("text", "") for b in c if isinstance(b, dict) and b.get("type") == "text"]
        if not parts:
            return None
        c = " ".join(parts)
    if not isinstance(c, str):
        return None
    c = c.strip()
    if not c or c.startswith("<"):
        return None
    return " ".join(c.split())[:REQUEST_CHARS]


def scan(path: pathlib.Path, acc: dict) -> dict:
    """offset 뒤의 줄만 읽어 acc를 갱신한다. 줄이 잘려 있으면(쓰는 중) 그 줄은 다음에 읽는다."""
    try:
        size = path.stat().st_size
    except OSError:
        return acc
    if size < acc["offset"]:
        acc = _empty_acc()
    if size == acc["offset"]:
        return acc
    with open(path, "rb") as fh:
        fh.seek(acc["offset"])
        buf = fh.read()
    end = buf.rfind(b"\n")
    if end < 0:
        return acc
    for raw in buf[:end].split(b"\n"):
        try:
            o = json.loads(raw.decode("utf-8", errors="replace"))
        except ValueError:
            continue
        t = o.get("type")
        if t == "assistant":
            rid = o.get("requestId") or ""
            if rid != acc["last_request_id"]:
                acc["turns"] += 1
                acc["last_request_id"] = rid
            u = (o.get("message") or {}).get("usage") or {}
            ctx = (u.get("input_tokens", 0) + u.get("cache_read_input_tokens", 0)
                   + u.get("cache_creation_input_tokens", 0))
            if ctx:
                acc["ctx"] = ctx
            acc["branch"] = o.get("gitBranch") or acc["branch"]
            acc["last_ts"] = o.get("timestamp") or acc["last_ts"]
        elif t == "user":
            txt = _user_text(o)
            if txt:
                acc["user_requests"] = (acc["user_requests"] + [txt])[-REQUEST_KEEP:]
            acc["branch"] = o.get("gitBranch") or acc["branch"]
        elif t == "system" and o.get("subtype") == "compact_boundary":
            acc["compacts"] += 1
            acc["compact_turns"] = (acc["compact_turns"] + [acc["turns"]])[-8:]
    acc["offset"] += end + 1
    return acc


def load_state() -> dict:
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_state(state: dict, live_ids: set[str]) -> None:
    state = {k: v for k, v in state.items() if k in live_ids}   # 죽은 세션 항목은 버린다
    PRIVATE.mkdir(exist_ok=True)
    STATE.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")


# ── 현황판·인계 파일 ────────────────────────────────────────────────────────
def claims_line(name: str) -> tuple[int, str]:
    """SESSION_CLAIMS.md에서 `| <이름> |` 셀이 있는 줄 중 마지막 것. (줄 번호, 상태 셀). 없으면 (0, '')."""
    if not name or not CLAIMS.exists():
        return 0, ""
    hit = (0, "")
    pat = re.compile(r"\|\s*" + re.escape(name) + r"(?:\(← [^|]*\))?\s*\|")
    for n, line in enumerate(CLAIMS.read_text(encoding="utf-8").splitlines(), 1):
        if pat.search(line):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            hit = (n, cells[-1] if cells else "")
    return hit


def handoff_file(name: str) -> str:
    p = PRIVATE / f"HANDOFF_{name}.md"
    return p.name if name and p.exists() else ""


# ── 조립 ────────────────────────────────────────────────────────────────────
def build_rows(state: dict, only: str | None = None) -> list[dict]:
    rows = []
    for s in live_sessions():
        sid = s.get("sessionId", "")
        if only and sid != only:
            continue
        acc = scan(transcript_path(s.get("cwd", ""), sid), state.get(sid) or _empty_acc())
        state[sid] = acc
        name = s.get("name") or sid[:8]
        ln, status = claims_line(name)
        pct = round(100 * acc["ctx"] / CONTEXT_WINDOW)
        rows.append({
            "name": name, "session_id": sid, "pid": s.get("pid"),
            "status": s.get("status", ""), "branch": acc["branch"] or "",
            "ctx": acc["ctx"], "ctx_k": round(acc["ctx"] / 1000), "ctx_pct": pct,
            "level": "danger" if pct >= DANGER_PCT else "warn" if pct >= WARN_PCT else "ok",
            "handoff_due": acc["ctx"] >= HANDOFF_CTX,
            "turns": acc["turns"], "compacts": acc["compacts"], "compact_turns": acc["compact_turns"],
            "last_request": acc["user_requests"][-1] if acc["user_requests"] else "",
            "user_requests": acc["user_requests"],
            "claims_line": ln, "claims_status": status,
            "handoff": handoff_file(name),
            "started": datetime.fromtimestamp((s.get("startedAt") or 0) / 1000).strftime("%m-%d %H:%M"),
            "last_ts": acc["last_ts"],
        })
    rows.sort(key=lambda r: r["name"])
    return rows


def git_facts() -> dict:
    def run(*a):
        try:
            r = subprocess.run(["git", *a], cwd=ROOT, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=10)
            return r.stdout.rstrip("\n")
        except (OSError, subprocess.SubprocessError):
            return ""
    return {"branch": run("rev-parse", "--abbrev-ref", "HEAD").strip(), "head": run("rev-parse", "--short", "HEAD").strip(),
            "porcelain": run("status", "--porcelain")}


def facts_text(row: dict) -> str:
    """인계 파일 '## 기계적 사실' 절 본문. PreCompact 안전망이 뼈대를 남길 때 쓴다."""
    g = git_facts()
    dirty = [l[3:] for l in g["porcelain"].splitlines() if len(l) > 3]
    lines = [
        f"- 브랜치·HEAD: {g['branch']} {g['head']} / 미커밋(트리 전체, 내 것인지는 확인): "
        + (", ".join(dirty[:12]) + (" …" if len(dirty) > 12 else "") if dirty else "없음"),
        f"- 현황판 줄: {row['claims_line'] or '없음'}행 {row['claims_status']}".rstrip(),
        f"- 문맥·턴: 약 {row['ctx_k']}K({row['ctx_pct']}%), {row['turns']}턴, 압축 {row['compacts']}회"
        + (f"(턴 {', '.join(map(str, row['compact_turns']))})" if row["compact_turns"] else ""),
        "- 최근 사용자 요청: " + (" / ".join(row["user_requests"]) if row["user_requests"] else "없음"),
        f"- 세션: {row['name']} {row['session_id']} (기록 {datetime.now().strftime('%Y-%m-%d %H:%M')})",
    ]
    return "\n".join(lines)


# ── 출력 ────────────────────────────────────────────────────────────────────
def print_table(rows: list[dict]) -> None:
    if not rows:
        print("살아 있는 세션 없음")
        return
    print(f"{'세션':<10}{'브랜치':<10}{'상태':<6}{'문맥':>8}{'턴':>5}{'압축':>4}  현황판  인계  마지막 요청")
    for r in rows:
        flag = {"ok": " ", "warn": "!", "danger": "!!"}[r["level"]]
        print(f"{r['name']:<10}{r['branch'][:9]:<10}{r['status']:<6}{r['ctx_k']:>5}K{r['ctx_pct']:>2}%{r['turns']:>5}"
              f"{r['compacts']:>4}  {r['claims_line'] or '-':>5}행  {'있음' if r['handoff'] else '  - '}  "
              f"{flag}{r['last_request'][:40]}")


def render_html(rows: list[dict]) -> str:
    def td(v, cls=""):
        return f'<td class="{cls}">{html.escape(str(v))}</td>'
    body = ""
    for r in rows:
        due = ' <span class="due">인계 시점</span>' if r["handoff_due"] else ""
        body += ("<tr>" + td(r["name"]) + td(r["branch"]) + td(r["status"])
                 + td(f"{r['ctx_k']}K ({r['ctx_pct']}%)", r["level"]) + td(r["turns"]) + td(r["compacts"])
                 + td(f"{r['claims_line'] or '-'}행 {r['claims_status']}") + td(r["handoff"] or "-")
                 + td(r["started"]) + f"<td>{html.escape(r['last_request'])}{due}</td></tr>\n")
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    return f"""<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="30">
<title>세션 현황판</title>
<style>
:root{{--bg:#f4f6fa;--fg:#1a1d24;--mut:#5c6473;--bd:#dde1ea;--warn:#b7791f;--danger:#c53030;--ok:#2f855a}}
@media(prefers-color-scheme:dark){{:root{{--bg:#0f1115;--fg:#e6e9ef;--mut:#8b93a7;--bd:#2a2f3a;--warn:#ecc94b;--danger:#fc8181;--ok:#68d391}}}}
body{{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif}}
h1{{font-size:16px;margin:0 0 4px}} .mut{{color:var(--mut);font-size:12px;margin-bottom:12px}}
table{{border-collapse:collapse;width:100%}} th,td{{border-bottom:1px solid var(--bd);padding:6px 8px;text-align:left;vertical-align:top}}
th{{color:var(--mut);font-weight:600}} td.warn{{color:var(--warn);font-weight:600}} td.danger{{color:var(--danger);font-weight:700}} td.ok{{color:var(--ok)}}
.due{{background:var(--danger);color:#fff;border-radius:4px;padding:0 5px;font-size:11px;margin-left:6px}}
</style>
<h1>세션 현황판</h1>
<div class="mut">{stamp} 기준 · 30초마다 새로 읽음 · 문맥 {WARN_PCT}%↑ 노랑, {DANGER_PCT}%↑ 빨강, {HANDOFF_CTX // 1000}K↑면 다음 작업 경계에서 /handoff · 원천 scripts/session_board.py</div>
<table><thead><tr><th>세션</th><th>브랜치</th><th>상태</th><th>문맥</th><th>턴</th><th>압축</th><th>현황판</th><th>인계 파일</th><th>시작</th><th>마지막 요청</th></tr></thead>
<tbody>{body or '<tr><td colspan="10">살아 있는 세션 없음</td></tr>'}</tbody></table>
"""


HANDOFF_TMPL = """# 인계 {name} — {stamp} — (한 줄 요약: 넘기는 세션이 채운다)

## 기계적 사실
{facts}

## 한 것
- (sha 제목 — 한 줄)

## 남은 것 (순서대로)
1. …

## 결정과 이유
- …

## 먼저 읽을 파일 3개
- 경로 — 왜

## 주의
- 남의 미커밋 파일, 살아 있는 다른 세션과 소유, 로컬 전용 변경
"""


def write_skeleton(row: dict) -> str:
    """PreCompact 안전망. 인계 파일이 없으면 틀을 만들고 '기계적 사실'만 채운다. 있으면 그 절만 바꾼다 —
    나머지 절은 넘기는 세션이 손으로 쓴 것이라 건드리지 않는다."""
    p = PRIVATE / f"HANDOFF_{row['name']}.md"
    facts = facts_text(row)
    if not p.exists():
        p.write_text(HANDOFF_TMPL.format(name=row["name"], stamp=datetime.now().strftime("%Y-%m-%d %H:%M"),
                                         facts=facts), encoding="utf-8")
        return f"{p.name} 새로 만듦(기계적 사실만)"
    s = p.read_text(encoding="utf-8")
    new, n = re.subn(r"(## 기계적 사실\n)(?:.*\n)*?(?=\n## )", lambda m: m.group(1) + facts + "\n", s, count=1)
    if n == 0:
        new = s.rstrip("\n") + "\n\n## 기계적 사실\n" + facts + "\n"
    p.write_text(new, encoding="utf-8")
    return f"{p.name} 기계적 사실 절 갱신"


def handoff_due(row: dict, state: dict) -> str:
    """Stop 훅. 이 턴에 HEAD가 바뀌었고(커밋 직후 = 작업 경계) 문맥이 HANDOFF_CTX를 넘었으면 안내 문구를 돌려준다.
    직전 HEAD는 세션별로 state에 둔다. 처음 보는 세션은 기록만 하고 알리지 않는다."""
    head = git_facts()["head"]
    acc = state.setdefault(row["session_id"], _empty_acc())
    prev = acc.get("last_head")
    acc["last_head"] = head
    if prev is None or prev == head or not row["handoff_due"]:
        return ""
    return (f"[인계 시점] 커밋 {prev}→{head} 직후이고 문맥 약 {row['ctx_k']}K(>{HANDOFF_CTX // 1000}K). "
            "/handoff 로 인계 파일을 쓰고 사용자에게 /clear 를 권할 것 (.claude/commands/handoff.md)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true", help="표를 찍지 않고 파일만 쓴다")
    ap.add_argument("--session", metavar="ID", help="이 세션만 JSON 한 줄로 찍는다(파일은 안 쓴다)")
    ap.add_argument("--facts", metavar="ID", help="이 세션의 인계 '기계적 사실' 절 본문을 찍는다(파일은 안 쓴다)")
    ap.add_argument("--skeleton", metavar="ID", help="이 세션의 _private/HANDOFF_<이름>.md 뼈대를 만들거나 기계적 사실만 갱신")
    ap.add_argument("--due", metavar="ID", help="인계 시점이면 안내를 찍고 2로 끝난다(Stop 훅용)")
    a = ap.parse_args()

    state = load_state()
    only = a.session or a.facts or a.skeleton or a.due
    rows = build_rows(state, only)
    if only:
        if not rows:
            print(f"세션 {only} 없음(살아 있지 않거나 다른 저장소)", file=sys.stderr)
            return 1
        row = rows[0]
        rc = 0
        if a.facts:
            print(facts_text(row))
        elif a.skeleton:
            print(write_skeleton(row))
        elif a.due:
            msg = handoff_due(row, state)
            if msg:
                print(msg, file=sys.stderr)
                rc = 2
        else:
            print(json.dumps(row, ensure_ascii=False))
        # 한 세션만 봐도 늘어난 꼬리를 읽었으니 상태는 남긴다. 다른 세션 항목은 그대로 둔다.
        save_state(state, {s.get("sessionId", "") for s in live_sessions()})
        return rc

    save_state(state, {r["session_id"] for r in rows})
    PRIVATE.mkdir(exist_ok=True)
    OUT_JSON.write_text(json.dumps({"updated": time.time(), "context_window": CONTEXT_WINDOW, "rows": rows},
                                   ensure_ascii=False, indent=1), encoding="utf-8")
    OUT_HTML.write_text(render_html(rows), encoding="utf-8")
    if not a.quiet:
        print_table(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
