#!/usr/bin/env python3
"""장 종료 후 그날 매매를 자동 정리한다 — 일지 생성 + 대시보드 갱신.

    py scripts/eod_autodoc.py                # 오늘
    py scripts/eod_autodoc.py --date 2026-09-07
    py scripts/eod_autodoc.py --dry-run      # 파일을 쓰지 않고 결과만 출력

이 스크립트는 LLM 없이 도는 결정론 경로다. 로그·원장에서 뽑을 수 있는 사실만 채우고,
해석이 필요한 자리는 빈 칸으로 남긴다. 해석은 `/eod-review`가 뒤에 붙어서 채운다.

기존 일지를 덮지 않는다. 사람이 쓴 일지에는 AUTO 마커가 없으므로 그런 파일은 건드리지 않고,
자동 생성분(마커 있음)만 최신 로그 기준으로 다시 만든다.
"""
from __future__ import annotations

import argparse
import csv as _csv
import re
import subprocess
import sys
from collections import Counter, defaultdict
from datetime import date as _date
from datetime import datetime
from pathlib import Path

# 콘솔이 cp949(한글 Windows 기본)면 ✅ 같은 이모지 출력에서 UnicodeEncodeError로
# 스크립트가 죽는다(예약작업 rc=267009의 원인). stdout/stderr를 UTF-8로 고정한다.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import _logdir  # noqa: E402
from log_patterns import PNL_RE, PREV_PNL_RE  # noqa: E402

JOURNAL_DIR = REPO / "strategies" / "DeviationScale" / "live"
RUN_LOG = REPO / "logs" / "eod_autodoc.log"

AUTO_BEGIN = "<!-- AUTO:BEGIN -->"
AUTO_END = "<!-- AUTO:END -->"

LINE_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\.\d+ \[(\w+)\s*\] (.*)$")
SESSION_RE = re.compile(r"=== Quant Trader")
SIZING_RE = re.compile(r"사이징 백스톱: 종목당 명목 (\d+)원, 동시보유 (\d+)종목")
FUNNEL_RE = re.compile(r"정배열 프리필터: (.*)$")
REGIME_RE = re.compile(r"국면=(\w+)")
NAME_RE = re.compile(r"(\d{6})\(([^)]{1,24})\)")
NUM_RE = re.compile(r"\d")

WEEKDAY_KR = "월화수목금토일"


# ─────────────────────────── 입력 찾기 ───────────────────────────

def find_files(ymd_compact: str):
    """그 날짜 원장(행 수 최대, 동률이면 mtime 최신)과 그 옆의 로그. 규칙은 _logdir 하나다.

    mtime으로 고르면 장 마감 뒤에 돌린 테스트 바이너리가 cwd 하위 logs/에 남긴 몇 줄짜리
    원장이 실제 원장을 이긴다(2026-09-08, 561체결이 7체결로 덮일 뻔했다).
    """
    csvp = _logdir.find_ledger(ymd_compact)
    if csvp is None:
        return None, None
    log = csvp.parent / "quant_trader.log"
    return (log if log.exists() else None), csvp


# ─────────────────────────── 로그 파싱 ───────────────────────────

def scan_log(log: Path, ymd: str) -> dict:
    """세션 경계와 그 세션의 설정·국면·단계별 통과율을 묶어서 낸다.

    세션마다 갈라 두는 이유: 장중에 코드·설정을 고치고 재기동하면 같은 날 안에서도
    동작이 갈린다. 하나로 뭉치면 그 차이가 시장 탓으로 오독된다.
    """
    sessions: list[dict] = []
    pnl: list[tuple[str, int, int]] = []
    prev_pnl: list[tuple[str, int, int]] = []
    names: dict[str, str] = {}
    warns: Counter = Counter()
    cur: dict | None = None

    with log.open(encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip("\n"))
            if not m:
                continue
            day, hms, lvl, rest = m.groups()
            if day != ymd:
                continue

            if SESSION_RE.search(rest):
                cur = {"at": hms, "slots": None, "cap": None, "funnel": None,
                       "regime": None, "registered": None, "note": ""}
                sessions.append(cur)

            for t, n in NAME_RE.findall(rest):
                names.setdefault(t, n)

            if cur is not None:
                s = SIZING_RE.search(rest)
                if s and cur["slots"] is None:
                    cur["cap"], cur["slots"] = int(s[1]), int(s[2])
                s = FUNNEL_RE.search(rest)
                if s and cur["funnel"] is None:
                    cur["funnel"] = s[1].strip()
                    r = re.search(r"등록=(\d+)", s[1])
                    if r:
                        cur["registered"] = int(r[1])
                s = REGIME_RE.search(rest)
                if s and cur["regime"] is None:
                    cur["regime"] = s[1]

            s = PNL_RE.search(rest)
            if s:
                pnl.append((hms, int(s[1]), int(s[2])))

            s = PREV_PNL_RE.search(rest)
            if s:
                prev_pnl.append((hms, int(s[1]), int(s[2])))

            if lvl in ("WARN", "ERROR"):
                warns[NUM_RE.sub("N", rest)[:80]] += 1

    return {"sessions": sessions, "pnl": pnl, "prev_pnl": prev_pnl,
            "names": names, "warns": warns.most_common(10)}


# ─────────────────────────── 원장 파싱 ───────────────────────────

# 매매가 아닌 이벤트. 집계에서 갈라 내지 않으면 체결 수가 부풀어 보인다.
NON_STRATEGY = {"TEST", "STARTUP_PROBE"}


def scan_ledger(path: Path) -> dict:
    rows = list(_csv.DictReader(path.open(encoding="utf-8-sig", errors="replace")))
    if not rows:
        return {}

    events = Counter(r["event"] for r in rows)
    sides = Counter(r["side"] for r in rows)

    rejects = Counter()
    for r in rows:
        if r["event"] == "REJECTED":
            rejects[re.sub(r"\d", "N", r.get("reason", ""))[:44]] += 1

    per = defaultdict(lambda: {"B": 0, "Bq": 0, "Bn": 0.0, "S": 0, "Sq": 0, "Sn": 0.0,
                               "breason": Counter(), "sreason": Counter(),
                               "strat": Counter(), "t0": "", "t1": "", "rp": 0.0})
    probe = 0
    for r in rows:
        if r["event"] != "FILL":
            continue
        strat = r.get("strategy", "")
        if strat in NON_STRATEGY:
            probe += 1
            continue
        t = r["ticker"]
        q = int(float(r.get("fill_qty") or 0))
        px = float(r.get("fill_price") or 0)
        a = per[t]
        a["strat"][strat] += 1
        a["rp"] += float(r.get("realized_pnl") or 0)
        if not a["t0"]:
            a["t0"] = r["ts_kst"][11:19]
        a["t1"] = r["ts_kst"][11:19]
        why = (r.get("entry_reason") or r.get("reason") or "").strip()[:72]
        if r["side"] == "BUY":
            a["B"] += 1; a["Bq"] += q; a["Bn"] += q * px; a["breason"][why] += 1
        else:
            a["S"] += 1; a["Sq"] += q; a["Sn"] += q * px; a["sreason"][why] += 1

    return {"events": events, "sides": sides, "rejects": rejects.most_common(10),
            "per": dict(per), "probe": probe, "n_rows": len(rows),
            "span": (rows[0]["ts_kst"], rows[-1]["ts_kst"])}


# ─────────────────────────── 렌더 ───────────────────────────

def won(x) -> str:
    return f"{int(x):,}"


def signed(x) -> str:
    return f"+{int(x):,}" if x > 0 else f"{int(x):,}"


def rel(p) -> str:
    """일지에 박히는 경로는 반드시 repo 상대경로로. 절대경로는 사용자명이 그대로 노출된다."""
    if p is None:
        return "—"
    try:
        return Path(p).resolve().relative_to(REPO).as_posix()
    except ValueError:
        return Path(p).name


def render(ymd: str, log_facts: dict, led: dict, log_path, csv_path) -> str:
    d = datetime.strptime(ymd, "%Y-%m-%d").date()
    dow = WEEKDAY_KR[d.weekday()]
    names = log_facts["names"]
    sess = log_facts["sessions"]
    pnl = log_facts["pnl"]
    prev_pnl = log_facts.get("prev_pnl") or []

    L: list[str] = []
    add = L.append

    add(f"# 라이브 모의 매매 일지 — {ymd} ({dow})")
    add("")
    add("> 환경: **KIS 모의계좌**(openapivts, is_paper=true). 시세·랭킹·지수는 실전 도메인 REST 폴링, 주문은 모의 발주.")
    add("> 전략: **DeviationScale (DEVSCALE)** 정배열눌림 지정가 분할 매수 + **ITB** 청산 관리(전일 보유분).")
    add(f"> 손익은 모의(가상) 기준. 로그 `{rel(log_path)}`, 원장 `{csv_path.name}`"
        f"({led.get('n_rows', 0)} 이벤트, {led.get('span', ('', ''))[0][11:19]}~{led.get('span', ('', ''))[1][11:19]}).")
    add("")
    add(f"> 이 일지는 `scripts/eod_autodoc.py`가 로그·원장에서 자동 생성했다"
        f"(생성 {datetime.now():%Y-%m-%d %H:%M}). 아래 AUTO 구간은 재실행하면 다시 쓰인다.")
    add("> 해석·판단이 필요한 자리는 비워 두었다. `/eod-review`로 채운다.")
    add("")
    add(AUTO_BEGIN)
    add("")
    add("---")
    add("")

    # 1. 손익
    add("## 1. 손익 추이 (잔고 대조 폴링)")
    add("")
    # 하루의 성과는 전일대비 줄로 적는다. 아래 표의 잔고 대조 값은 세션이 뜬 시점을 0으로
    #  잡아 재기동마다 기준이 옮겨가고 간밤 갭이 빠진다 — 2026-09-10에 두 값의 부호가 갈렸다.
    if prev_pnl:
        last = prev_pnl[-1]
        add(f"**전일대비 종료 손익 {signed(last[1])}원** (전일총자산 {won(last[2])}, "
            f"{last[0][:5]} 기준). 아래 표는 세션 시작을 0으로 잡은 잔고 대조 카운터라 "
            f"기준이 다르다.")
    else:
        add("전일대비 손익 줄이 로그에 없다 — 아래 표는 세션 기준 카운터다.")
    add("")
    if pnl:
        lo = min(pnl, key=lambda x: x[1])
        hi = max(pnl, key=lambda x: x[1])
        hourly: dict[str, tuple] = {}
        for hms, v, eq in pnl:
            hourly.setdefault(hms[:2], (hms, v, eq))
        # 정시 표본에 고점·저점·종료를 섞어 시간순으로 낸다. 라벨 없이 나열하면
        #  어느 줄이 극점인지 안 보이고, 뒤에 몰아 붙이면 시간 흐름이 깨진다.
        marks: dict[str, list[str]] = defaultdict(list)
        marks[hi[0]].append("고점")
        marks[lo[0]].append("저점")
        marks[pnl[-1][0]].append("종료")
        rows = {hms: (hms, v, eq) for hms, v, eq in hourly.values()}
        for hms in marks:
            rows[hms] = next(r for r in pnl if r[0] == hms)
        add("| 시각 | 당일손익 | 총평가 |")
        add("|---|---|---|")
        for hms, v, eq in sorted(rows.values()):
            tag = f" ({'·'.join(marks[hms])})" if hms in marks else ""
            cell = f"**{signed(v)}원**" if tag else f"{signed(v)}원"
            add(f"| {hms[:5]}{tag} | {cell} | {won(eq)} |")
    else:
        add("잔고 대조 표본 없음 — 로그로 확인 불가.")
    add("")

    # 2. 세션
    add(f"## 2. 세션 {len(sess)}회 — 설정·국면 분기")
    add("")
    regimes = {s["regime"] for s in sess if s["regime"]}
    if len(regimes) == 1:
        add(f"국면은 전 세션 **{regimes.pop()}**이었다. 세션 간 동작 차이는 국면 전환이 아니라 "
            "코드·설정 변경 때문이다.")
    elif regimes:
        add(f"국면이 세션에 따라 갈렸다({', '.join(sorted(regimes))}). 동작 차이의 원인을 "
            "국면과 코드 변경으로 나눠 봐야 한다.")
    add("")
    add("| # | 시각 | 슬롯 | 종목당 명목 | 등록 | 국면 | 프리필터 단계별 통과율 |")
    add("|---|------|------|------|------|------|------|")
    for i, s in enumerate(sess, 1):
        cap = f"{won(s['cap'])}원" if s["cap"] else "—"
        add(f"| {i} | {s['at'][:5]} | {s['slots'] or '—'} | {cap} | "
            f"{s['registered'] if s['registered'] is not None else '—'} | "
            f"{s['regime'] or '—'} | {s['funnel'] or '—'} |")
    add("")
    add("> **이 세션에서 무엇을 왜 바꿨나** — 로그로는 알 수 없다. 재기동마다 채워 넣는다.")
    add("")

    # 3. 종목별
    add("## 3. 종목별 매수 사유 / 매도 사유")
    add("")
    per = led.get("per", {})
    if per:
        add(f"실전략 체결 {sum(a['B'] + a['S'] for a in per.values())}건"
            f"(기동 점검·부하시험 {led.get('probe', 0)}건 제외), 종목 {len(per)}개.")
        add("")
        add("| 종목 | 시각 | 매수 | 매도 | 매수 명목 | 매도 명목 | 실현 |")
        add("|---|---|---|---|---|---|---|")
        for t, a in sorted(per.items(), key=lambda kv: -(kv[1]["Bn"] + kv[1]["Sn"])):
            label = f"{t} {names.get(t, '')}".strip()
            add(f"| {label} | {a['t0'][:5]}~{a['t1'][:5]} | {a['B']}건 {a['Bq']}주 | "
                f"{a['S']}건 {a['Sq']}주 | {won(a['Bn'])} | {won(a['Sn'])} | {won(a['rp'])}원 |")
        add("")
        add("### 사유 (원장 `entry_reason` 원문)")
        add("")
        for t, a in sorted(per.items(), key=lambda kv: -(kv[1]["Bn"] + kv[1]["Sn"])):
            label = f"{t} {names.get(t, '')}".strip()
            add(f"**{label}**")
            add("")
            for why, n in a["breason"].most_common(3):
                add(f"- 매수 {n}건 — {why or '_사유 없음_'}")
            for why, n in a["sreason"].most_common(3):
                add(f"- 매도 {n}건 — {why or '_사유 없음_'}")
            add("")
    else:
        add("실전략 체결 없음.")
    add("")

    # 4. 집계
    add("## 4. 이벤트·거부 집계")
    add("")
    ev = led.get("events", Counter())
    add("| 이벤트 | 건수 |")
    add("|---|---|")
    for k in ("ACCEPTED", "FILL", "REJECTED", "CANCELLED"):
        add(f"| {k} | {ev.get(k, 0)} |")
    add("")
    sd = led.get("sides", Counter())
    add(f"방향은 BUY {sd.get('BUY', 0)} / SELL {sd.get('SELL', 0)} / 기타 "
        f"{sum(v for k, v in sd.items() if k not in ('BUY', 'SELL'))}.")
    add("")
    if led.get("rejects"):
        add("| 거부 사유 (숫자 정규화) | 건수 |")
        add("|---|---|")
        for why, n in led["rejects"]:
            add(f"| {why} | {n} |")
        add("")
    if log_facts["warns"]:
        add("| 경고·오류 상위 | 건수 |")
        add("|---|---|")
        for why, n in log_facts["warns"]:
            add(f"| {why} | {n} |")
        add("")

    add(AUTO_END)
    add("")
    add("---")
    add("")
    add("## 5. 이슈 & 조치")
    add("")
    add("_(미작성 — 그날 발견하고 고친 것을 적는다. 코드 변경은 docs/DECISIONS.md의 D-NNN과 연결한다.)_")
    add("")
    add("## 6. 내일 확인할 것")
    add("")
    add("_(미작성)_")
    add("")
    return "\n".join(L)


# ─────────────────────────── 쓰기 ───────────────────────────

def write_journal(path: Path, body: str, dry: bool) -> str:
    """기존 일지를 존중한다 — 마커가 없는 파일(사람이 쓴 것)은 건드리지 않는다."""
    if path.exists():
        old = path.read_text(encoding="utf-8")
        if AUTO_BEGIN not in old:
            return f"보존(수기 일지) {path.name}"
        new_auto = body.split(AUTO_BEGIN, 1)[1].rsplit(AUTO_END, 1)[0]
        head, rest = old.split(AUTO_BEGIN, 1)
        _, tail = rest.split(AUTO_END, 1)
        merged = head + AUTO_BEGIN + new_auto + AUTO_END + tail
        if not dry:
            path.write_text(merged, encoding="utf-8")
        return f"AUTO 구간 갱신 {path.name}"
    if not dry:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
    return f"신규 생성 {path.name}"


def run_dashboard(ymd: str, dry: bool) -> list[str]:
    """재생성 절차는 scripts/refresh_dashboard.py가 소유한다.

    순서(라이브 백필 · 리뷰 항목 · 생성기)를 여기에도 적어 두면 한쪽만 고쳐져 갈라진다.
    16:05 예약 실행과 장중 자동 갱신이 같은 절차를 쓰게 하려고 그쪽으로 넘긴다.
    """
    args = ["--live", ymd] + (["--dry-run"] if dry else [])
    r = subprocess.run([sys.executable, "scripts/refresh_dashboard.py", *args], cwd=REPO,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    body = [l.strip() for l in (r.stdout or r.stderr or "").strip().splitlines()[1:] if l.strip()]
    return body or [f"refresh_dashboard rc={r.returncode}"]


def run_ledger_sync(dry: bool) -> list[str]:
    """결정 원장 파생 문서 갱신. 매매 없는 날에도 결정은 쌓이므로 원장 유무와 무관하게 돈다."""
    script = REPO / "scripts" / "sync_ledgers.py"
    if not script.exists():
        return []
    if dry:
        return ["원장 동기화 건너뜀(dry-run)"]
    r = subprocess.run([sys.executable, str(script)], cwd=REPO,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout or r.stderr).strip().splitlines()
    return out[-1:] if out else ["원장 동기화: 출력 없음"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="YYYY-MM-DD (기본: 오늘)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-dashboard", action="store_true")
    a = ap.parse_args()

    ymd = a.date or _date.today().isoformat()
    compact = ymd.replace("-", "")
    log_path, csv_path = find_files(compact)

    lines = [f"[{datetime.now():%Y-%m-%d %H:%M:%S}] eod_autodoc {ymd}"]
    for l in run_ledger_sync(a.dry_run):
        lines.append("  " + l)
    if csv_path is None:
        lines.append("  원장 없음 — 매매하지 않은 날로 보고 건너뜀")
        print("\n".join(lines))
        _append_run_log(lines, a.dry_run)
        return 0

    led = scan_ledger(csv_path)
    log_facts = ({"sessions": [], "pnl": [], "prev_pnl": [], "names": {}, "warns": []}
                 if log_path is None else scan_log(log_path, ymd))

    body = render(ymd, log_facts, led, log_path, csv_path)
    lines.append("  " + write_journal(JOURNAL_DIR / f"{ymd}.md", body, a.dry_run))
    lines.append(f"  세션 {len(log_facts['sessions'])} · 체결 {led.get('events', Counter()).get('FILL', 0)}"
                 f" · 거부 {led.get('events', Counter()).get('REJECTED', 0)} · 종목 {len(led.get('per', {}))}")

    if not a.no_dashboard:
        for l in run_dashboard(ymd, a.dry_run):
            lines.append("  " + l)

    print("\n".join(lines))
    _append_run_log(lines, a.dry_run)
    return 0


def _append_run_log(lines: list[str], dry: bool) -> None:
    if dry:
        return
    RUN_LOG.parent.mkdir(parents=True, exist_ok=True)
    with RUN_LOG.open("a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
