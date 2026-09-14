"""세션 기록(~/.claude/projects/<repo>/*.jsonl)을 읽어 토큰이 어디에 쓰였는지 집계한다.

각 API 호출은 그때까지의 문맥 전체를 다시 보내므로, 비용은 대체로 (호출 수) × (문맥 크기)다.
그래서 호출을 "그 턴이 부른 도구"로 분류해 어느 절차(탐색·편집·git·빌드·위임·훅 되돌림…)가
호출을 얼마나 만들었고 그때 문맥이 얼마였는지를 표로 낸다. 서브에이전트 기록은 하위 폴더에 있어
Agent 위임 비용으로 따로 센다. 하네스가 끼워 넣는 것(CLAUDE.md 재주입·압축 요약·훅 출력·IDE 파일
되비침)은 attachment/system 레코드에서 센다.

사용:
    py scripts/token_audit.py                 # 최근 7일
    py scripts/token_audit.py --days 30
    py scripts/token_audit.py --session <id앞자리>   # 세션 하나
    py scripts/token_audit.py --md docs/reports/TOKEN_AUDIT.md
"""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import json
import pathlib
import re
import sys

PROJECT_DIR = pathlib.Path.home() / ".claude" / "projects" / "c--Users-----source-repos-Quant"

# 턴 분류 — 그 턴의 tool_use 첫 항목으로 정한다.
GIT_RE = re.compile(r"\bgit\b")
BUILD_RE = re.compile(r"\b(cmake|ctest|ninja|msbuild|vcvars)")
TEST_RE = re.compile(r"\btest_[a-z_]+")
PY_SCRIPT_RE = re.compile(r"\b(py|python)\b\s+scripts/")
HOOK_NAME_RE = re.compile(r"hooks[\\/]+([\w-]+)\.ps1")


def classify(tool: str | None, inp: dict | None) -> str:
    if tool is None:
        return "응답(텍스트만)"
    if tool in ("Read", "Grep", "Glob"):
        return "탐색(Read/Grep/Glob)"
    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
        return "편집(Edit/Write)"
    if tool == "Agent":
        return "위임(Agent)"
    if tool == "SendMessage":
        return "세션 통신(SendMessage)"
    if tool == "Skill":
        return "스킬(Skill)"
    if tool == "AskUserQuestion":
        return "질문(AskUserQuestion)"
    if tool in ("Bash", "PowerShell"):
        cmd = str((inp or {}).get("command", ""))
        if GIT_RE.search(cmd):
            return "git(Bash)"
        if BUILD_RE.search(cmd) or TEST_RE.search(cmd):
            return "빌드/테스트(Bash)"
        if PY_SCRIPT_RE.search(cmd):
            return "스크립트 실행(Bash)"
        return "기타 셸(Bash)"
    if tool.startswith("mcp__"):
        return "MCP"
    return f"기타({tool})"


def text_of(content) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        out = []
        for b in content:
            if isinstance(b, dict):
                if b.get("type") == "text":
                    out.append(b.get("text", ""))
                elif b.get("type") == "tool_result":
                    out.append(text_of(b.get("content", "")))
            elif isinstance(b, str):
                out.append(b)
        return "\n".join(out)
    return ""


class Session:
    def __init__(self, sid: str, path: pathlib.Path, is_sub: bool):
        self.sid = sid
        self.path = path
        self.is_sub = is_sub
        self.first_ts: dt.datetime | None = None
        self.last_ts: dt.datetime | None = None
        self.turns = 0
        self.usage = collections.Counter()
        self.turn_cat = collections.Counter()
        self.turn_cat_tokens = collections.Counter()
        self.tool_calls = collections.Counter()
        self.tool_result_chars = collections.Counter()
        self.tool_input_chars = collections.Counter()
        self.stop_hook_turns = 0
        self.hook_count = collections.Counter()
        self.hook_ms = collections.Counter()
        self.hook_runs = collections.Counter()
        self.att_chars = collections.Counter()
        self.att_count = collections.Counter()
        self.sysprompt_chars = 0
        self.compactions = 0
        self.compact_pre = 0
        self.compact_ms = 0
        self.user_prompts = 0
        self.user_prompt_chars = 0
        self.first_ctx = None
        self.max_ctx = 0
        self.git_cmds = collections.Counter()
        self.agent_types = collections.Counter()
        self.reread_files = collections.Counter()
        self.slug = ""

    @property
    def total_in(self):
        return self.usage["input"] + self.usage["cache_create"] + self.usage["cache_read"]

    def date(self):
        return self.first_ts.strftime("%m-%d") if self.first_ts else "?"


def parse_ts(o) -> dt.datetime | None:
    t = o.get("timestamp")
    if not t:
        return None
    try:
        return dt.datetime.fromisoformat(t.replace("Z", "+00:00")).astimezone()
    except Exception:
        return None


def rendered_text(o: dict, a: dict, k: str) -> str:
    r = "".join(str(x.get("content", "")) for x in (o.get("rendered") or []) if isinstance(x, dict))
    if r:
        return r
    if k == "instructions":
        return "".join(f_.get("content", "") for f_ in a.get("files") or [])
    if k == "file":
        return json.dumps(a.get("content", ""), ensure_ascii=False)
    if k == "edited_text_file":
        return a.get("snippet", "")
    if k == "queued_command":
        return json.dumps(a.get("prompt", ""), ensure_ascii=False)
    return str(a.get("content") or a.get("stderr") or a.get("text") or "")


def scan(path: pathlib.Path, is_sub: bool) -> Session:
    s = Session(path.stem, path, is_sub)
    seen_msg = set()
    id2tool: dict[str, tuple[str, dict]] = {}
    state = {"mid": None, "tools": [], "ctx": 0, "pending_stop": False}

    def finalize():
        if state["mid"] is None:
            return
        tools = state["tools"]
        cat = classify(tools[0]["name"], tools[0].get("input")) if tools else classify(None, None)
        if state["pending_stop"]:
            cat = "Stop 훅 되돌림 재응답"
            s.stop_hook_turns += 1
            state["pending_stop"] = False
        s.turn_cat[cat] += 1
        s.turn_cat_tokens[cat] += state["ctx"]
        state["mid"], state["tools"], state["ctx"] = None, [], 0

    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                o = json.loads(line)
            except Exception:
                continue
            ts = parse_ts(o)
            if ts:
                s.first_ts = s.first_ts or ts
                s.last_ts = ts
            typ = o.get("type")
            if typ == "system":
                sub = o.get("subtype")
                if sub == "compact_boundary":
                    s.compactions += 1
                    cm = o.get("compactMetadata") or {}
                    s.compact_pre += cm.get("preTokens", 0)
                    s.compact_ms += cm.get("durationMs", 0)
                elif sub == "stop_hook_summary":
                    for h in o.get("hookInfos") or []:
                        m_ = HOOK_NAME_RE.search(h.get("command", ""))
                        name = m_.group(1) if m_ else "?"
                        s.hook_ms[name] += h.get("durationMs", 0)
                        s.hook_runs[name] += 1
                    if o.get("preventedContinuation"):
                        state["pending_stop"] = True
                        s.hook_count["Stop 훅 되돌림(preventedContinuation)"] += 1
                continue
            if typ == "attachment":
                a = o.get("attachment") or {}
                k = a.get("type", "?")
                if k == "prompt_snapshot":
                    s.sysprompt_chars = max(s.sysprompt_chars, len(json.dumps(a.get("systemPrompt", ""), ensure_ascii=False)))
                    continue
                if k == "total_tokens_reminder":
                    continue
                s.att_chars[k] += len(rendered_text(o, a, k))
                s.att_count[k] += 1
                if k == "hook_non_blocking_error" and "Stop" in str(a.get("hookEvent", "")):
                    state["pending_stop"] = True
                    s.hook_count["Stop 훅 되돌림(non-blocking stderr)"] += 1
                continue
            if typ == "user":
                finalize()
                m = o.get("message", {})
                content = m.get("content")
                blocks = content if isinstance(content, list) else [{"type": "text", "text": content or ""}]
                if o.get("isCompactSummary"):
                    s.att_chars["compact_summary"] += len(text_of(blocks))
                    s.att_count["compact_summary"] += 1
                    continue
                for b in blocks:
                    if not isinstance(b, dict):
                        continue
                    if b.get("type") == "tool_result":
                        name, _ = id2tool.get(b.get("tool_use_id", ""), ("?", {}))
                        s.tool_result_chars[name] += len(text_of(b.get("content", "")))
                    elif b.get("type") == "text" and not o.get("isMeta"):
                        t = b.get("text", "")
                        body = re.sub(r"<system-reminder>.*?</system-reminder>", "", t, flags=re.S).strip()
                        if body:
                            s.user_prompts += 1
                            s.user_prompt_chars += len(body)
                            if not s.slug:
                                s.slug = body[:40].replace("\n", " ")
                continue
            if typ == "assistant":
                m = o.get("message", {})
                mid = m.get("id")
                u = m.get("usage") or {}
                content = m.get("content") or []
                tools = [b for b in content if isinstance(b, dict) and b.get("type") == "tool_use"]
                if mid != state["mid"]:
                    finalize()
                    state["mid"] = mid
                state["tools"].extend(tools)
                for b in tools:
                    name = b.get("name", "?")
                    inp = b.get("input") or {}
                    id2tool[b.get("id", "")] = (name, inp)
                    s.tool_calls[name] += 1
                    s.tool_input_chars[name] += len(json.dumps(inp, ensure_ascii=False))
                    if name in ("Bash", "PowerShell"):
                        for g in re.findall(r"\bgit\s+(?:-C\s+\S+\s+)?([a-z-]+)", str(inp.get("command", ""))):
                            s.git_cmds[g] += 1
                    elif name == "Agent":
                        s.agent_types[inp.get("subagent_type", "general")] += 1
                    elif name == "Read":
                        s.reread_files[str(inp.get("file_path", ""))] += 1
                if mid in seen_msg or not u:
                    continue
                seen_msg.add(mid)
                s.turns += 1
                ci = u.get("input_tokens", 0)
                cc = u.get("cache_creation_input_tokens", 0)
                cr = u.get("cache_read_input_tokens", 0)
                s.usage["input"] += ci
                s.usage["cache_create"] += cc
                s.usage["cache_read"] += cr
                s.usage["output"] += u.get("output_tokens", 0)
                ctx = ci + cc + cr
                state["ctx"] = ctx
                if s.first_ctx is None:
                    s.first_ctx = ctx
                s.max_ctx = max(s.max_ctx, ctx)
    finalize()
    return s


def fmt(n: float) -> str:
    if n >= 1e6:
        return f"{n/1e6:.1f}M"
    if n >= 1e3:
        return f"{n/1e3:.0f}K"
    return f"{n:.0f}"


def scrub_home(text: str) -> str:
    """홈 경로를 `~`로 바꾼다 — 보고서가 저장소에 들어가므로 사용자 폴더 이름을 남기지 않는다."""
    home = str(pathlib.Path.home())
    for h in (home, home.replace("\\", "/")):
        for variant in (h, h[0].lower() + h[1:], h[0].upper() + h[1:]):
            text = text.replace(variant, "~")
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=7)
    ap.add_argument("--session", default=None)
    ap.add_argument("--md", default=None)
    ap.add_argument("--dir", default=str(PROJECT_DIR))
    a = ap.parse_args()
    root = pathlib.Path(a.dir)
    cutoff = dt.datetime.now().astimezone() - dt.timedelta(days=a.days)

    mains: list[Session] = []
    subs: dict[str, list[Session]] = collections.defaultdict(list)
    for p in sorted(root.glob("*.jsonl")):
        if a.session and not p.stem.startswith(a.session):
            continue
        s = scan(p, False)
        if s.turns == 0 or (s.last_ts and s.last_ts < cutoff):
            continue
        mains.append(s)
        sub_dir = root / p.stem
        if sub_dir.is_dir():
            for q in sub_dir.rglob("*.jsonl"):
                ss = scan(q, True)
                if ss.turns:
                    subs[p.stem].append(ss)

    out = []
    w = out.append
    w(f"# 토큰 사용 감사 — 최근 {a.days}일, 세션 {len(mains)}개 (생성 {dt.datetime.now():%Y-%m-%d %H:%M})")
    w("")
    w("토큰은 API가 보고한 usage 합(입력 = 비캐시 + 캐시 생성 + 캐시 읽기). 문자 수는 기록에 실린 길이(토큰 ≈ 문자/3).")
    w("")

    w("## 1. 세션별")
    w("")
    w("| 날짜 | 세션 | 턴(호출) | 입력 합 | 캐시읽기 | 출력 | 첫 문맥 | 최대 문맥 | 압축 | Stop훅 재응답 | 서브 호출/입력 | 첫 요청 |")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|")
    tot = collections.Counter()
    for s in sorted(mains, key=lambda x: x.first_ts or dt.datetime.min.replace(tzinfo=dt.timezone.utc)):
        sub_turns = sum(x.turns for x in subs.get(s.sid, []))
        sub_in = sum(x.total_in for x in subs.get(s.sid, []))
        cr_ratio = s.usage["cache_read"] / s.total_in if s.total_in else 0
        w(f"| {s.date()} | {s.sid[:8]} | {s.turns} | {fmt(s.total_in)} | {cr_ratio:.0%} | {fmt(s.usage['output'])} | {fmt(s.first_ctx or 0)} | {fmt(s.max_ctx)} | {s.compactions} | {s.stop_hook_turns} | {sub_turns}/{fmt(sub_in)} | {s.slug} |")
        tot["turns"] += s.turns
        tot["in"] += s.total_in
        tot["out"] += s.usage["output"]
        tot["sub_turns"] += sub_turns
        tot["sub_in"] += sub_in
        tot["compact"] += s.compactions
        tot["stop"] += s.stop_hook_turns
    w(f"| **합** | | {tot['turns']} | {fmt(tot['in'])} | | {fmt(tot['out'])} | | | {tot['compact']} | {tot['stop']} | {tot['sub_turns']}/{fmt(tot['sub_in'])} | |")
    w("")
    if tot["turns"]:
        w(f"- 메인 세션 호출당 평균 문맥 {fmt(tot['in']/tot['turns'])} 토큰, 세션당 평균 {tot['turns']/max(1,len(mains)):.0f}턴.")
        w(f"- 서브에이전트가 쓴 입력은 메인의 {tot['sub_in']/max(1,tot['in']):.0%}.")
    w("")

    w("## 2. 호출을 부른 절차별 (메인 세션 합)")
    w("")
    w("호출 하나가 그때의 문맥 전체를 보내므로 '문맥 토큰 합'이 실제 비용에 가깝다.")
    w("")
    w("| 절차 | 호출 수 | 호출 비율 | 문맥 토큰 합 | 토큰 비율 | 호출당 문맥 |")
    w("|---|---|---|---|---|---|")
    cat_n = collections.Counter()
    cat_t = collections.Counter()
    for s in mains:
        cat_n.update(s.turn_cat)
        cat_t.update(s.turn_cat_tokens)
    T = sum(cat_t.values()) or 1
    N = sum(cat_n.values()) or 1
    for k, v in cat_t.most_common():
        w(f"| {k} | {cat_n[k]} | {cat_n[k]/N:.0%} | {fmt(v)} | {v/T:.0%} | {fmt(v/max(1,cat_n[k]))} |")
    w("")

    w("## 3. 도구 결과가 문맥에 넣은 양 (메인 세션 합, 문자)")
    w("")
    w("한 번 들어온 결과는 이후 모든 호출에 다시 보내진다 — 큰 결과가 초반에 들어올수록 비싸다.")
    w("")
    w("| 도구 | 호출 | 결과 문자 | 호출당 결과 | 입력(인자) 문자 |")
    w("|---|---|---|---|---|")
    tc = collections.Counter()
    tr = collections.Counter()
    ti = collections.Counter()
    for s in mains:
        tc.update(s.tool_calls)
        tr.update(s.tool_result_chars)
        ti.update(s.tool_input_chars)
    for k, v in tr.most_common(20):
        w(f"| {k} | {tc[k]} | {fmt(v)} | {fmt(v/max(1,tc[k]))} | {fmt(ti[k])} |")
    w("")

    w("## 4. 하네스가 문맥에 끼워 넣은 양 (메인 세션 합, 문자)")
    w("")
    w("instructions = CLAUDE.md·메모리 재주입, compact_summary = 압축 요약, file/edited_text_file = IDE·외부 편집 파일 되비침, queued_command = 대기 중 넣은 요청, hook_* = 훅 출력.")
    w("")
    w("| 종류 | 건수 | 문자 | 건당 |")
    w("|---|---|---|---|")
    ac = collections.Counter()
    an = collections.Counter()
    for s in mains:
        ac.update(s.att_chars)
        an.update(s.att_count)
    for k, v in ac.most_common(16):
        w(f"| {k} | {an[k]} | {fmt(v)} | {fmt(v/max(1,an[k]))} |")
    w("")
    sp = max((s.sysprompt_chars for s in mains), default=0)
    w(f"- 시스템 프롬프트(도구 스키마 포함) 최대 {fmt(sp)}자 ≈ {fmt(sp/3)} 토큰 — 모든 호출의 바닥.")
    w("")
    w("### 압축(compact)")
    w("")
    cp = sum(s.compact_pre for s in mains)
    cms = sum(s.compact_ms for s in mains)
    n_c = sum(s.compactions for s in mains)
    w(f"- 압축 {n_c}회, 압축 호출 입력 합 {fmt(cp)} 토큰(전체 입력의 {cp/max(1,tot['in']):.1%}), 소요 합 {cms/60000:.0f}분(회당 {cms/max(1,n_c)/1000:.0f}초).")
    w("")
    w("### Stop 훅")
    w("")
    hm = collections.Counter()
    hr = collections.Counter()
    hc = collections.Counter()
    for s in mains:
        hm.update(s.hook_ms)
        hr.update(s.hook_runs)
        hc.update(s.hook_count)
    w("| 훅 | 실행 | 소요 합(초) | 회당(초) |")
    w("|---|---|---|---|")
    for k, v in hm.most_common():
        w(f"| {k} | {hr[k]} | {v/1000:.0f} | {v/max(1,hr[k])/1000:.1f} |")
    w("")
    w("되돌림: " + (", ".join(f"{k} {v}" for k, v in hc.most_common()) or "없음") + f" — 재응답 호출 {sum(s.stop_hook_turns for s in mains)}회.")
    w("")
    w(f"- 사용자 요청: {sum(s.user_prompts for s in mains)}건, {fmt(sum(s.user_prompt_chars for s in mains))}자.")
    w("")

    w("## 5. git 명령 · 서브에이전트 · 같은 파일 다시 읽기")
    w("")
    gc = collections.Counter()
    ag = collections.Counter()
    rr = collections.Counter()
    for s in mains:
        gc.update(s.git_cmds)
        ag.update(s.agent_types)
        for f_, n in s.reread_files.items():
            if n >= 3:
                rr[f_] += n
    w("git 하위명령 상위: " + ", ".join(f"{k} {v}" for k, v in gc.most_common(12)))
    w("")
    w("서브에이전트 종류: " + (", ".join(f"{k} {v}" for k, v in ag.most_common()) or "없음"))
    w("")
    if subs:
        sub_all = [x for lst in subs.values() for x in lst]
        sub_all.sort(key=lambda x: -x.total_in)
        w("서브에이전트 상위(입력 순, 세션:턴/입력/첫 요청):")
        for x in sub_all[:10]:
            w(f"- {x.sid[:8]}: {x.turns}턴/{fmt(x.total_in)} — {x.slug[:60]}")
        w("")
    w("같은 세션에서 3번 이상 Read한 파일 상위:")
    for k, v in rr.most_common(12):
        w(f"- {v}회 {k}")
    w("")

    text = scrub_home("\n".join(out))
    if a.md:
        pathlib.Path(a.md).write_text(text + "\n", encoding="utf-8", newline="\n")
        print(f"wrote {a.md}")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)


if __name__ == "__main__":
    main()
