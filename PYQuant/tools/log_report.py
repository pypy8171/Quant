#!/usr/bin/env python3
"""
quant_trader.log 운용 리포트 생성기 (log analysis / ops report)

거래 시스템 운용의 핵심은 "돌려놓고 로그로 상태를 읽어내는 것"이다. 이 도구는
quant_trader 로그를 파싱해 세션·주문·체결·신뢰성(장애) 지표를 뽑아 운용 리포트로
만든다. 외부 의존성 없음(표준 라이브러리만) → 서버 어디서나 바로 실행.

파싱 대상 로그 포맷:
    2026-05-15 12:56:00.123 [INFO ] 메시지
    (레벨은 5칸 패딩: "INFO ","WARN ","ERROR","DEBUG")

추출 이벤트(소스의 실제 로그 문자열과 1:1로 맞춤):
    세션 시작   === Quant Trader v2.0 ===
    설정/토큰   [Main] 설정 로드: ... / [KIS] 토큰 발급|캐시 토큰 재사용
    장 시작     [DataThread] 장 시작 — OrderGate 일별 카운터 리셋
    전략 신호   [Strategy] 신호: [<id>] <ticker> ...
    주문 접수   [OrderRouter] 접수 [ORD-x] ODNO=x <ticker> BUY|SELL N주 [RTT=Nms]
    주문 거부   [OrderRouter] 거부 [ORD-x] <ticker> → <사유>       (게이트)
                [OrderRouter] KIS 예외|거부 [ORD-x] <ticker> ...    (KIS단)
    체결 확인   [OrderRouter] 체결 확인 [ORD-x] ODNO=x <ticker> BUY|SELL N주 @P (누적 c/t주)
    취소/정정   [OrderRouter] 취소 접수|정정 접수|취소 거부|...
    체결통보    [OrderRouter] 중복 체결통보 무시 | 체결통보 매핑 실패
    신뢰성      [Control] WebSocket ...초 이상 시세 미수신 — 재연결 시도
                [Control] WebSocket 재연결 성공|실패
                [ZMQ] KILL 명령 수신 ...

사용법:
    python tools/log_report.py <로그경로> [--md 리포트.md] [--html 리포트.html]
    python tools/log_report.py ../Quant/quant_trader.log
    python tools/log_report.py quant_trader.log --md ops_report.md --html ops_report.html
"""
from __future__ import annotations

import argparse
import html
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime
from statistics import mean

# ── 레코드 앵커 ──────────────────────────────────────────────────────────────
# 줄 단위가 아니라 "타임스탬프+레벨" 앵커로 레코드를 자른다. 실제 로그에는
# WS 덤프 라인이 개행 없이 다음 로그와 붙는 경우가 있어(로깅 레이스/중단),
# 단순 줄 파싱은 뒤에 붙은 레코드를 통째로 놓친다. finditer 분할로 강건화.
ANCHOR_RE = re.compile(
    r"(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}) \[(?P<lvl>[A-Z ]{5})\] "
)
TS_FMT = "%Y-%m-%d %H:%M:%S.%f"

# ── 메시지 패턴 (소스 로그 문자열과 1:1) ─────────────────────────────────────
RE_SESSION = re.compile(r"^=== Quant Trader")
RE_CONFIG = re.compile(r"^\[Main\] 설정 로드: (?P<cfg>.+)$")
RE_MODE_END = re.compile(r"^\[(?P<mode>[A-Z_]+)\] 종료$")
RE_TOKEN = re.compile(r"^\[KIS\] (?P<kind>토큰 발급 성공|캐시 토큰 재사용)")
RE_MKT_OPEN = re.compile(r"^\[DataThread\] 장 시작")
RE_SIGNAL = re.compile(r"^\[Strategy\] 신호: \[(?P<sid>[^\]]+)\] (?P<ticker>\S+)")
RE_ACCEPT = re.compile(
    r"^\[OrderRouter\] 접수 \[(?P<oid>[^\]]+)\] ODNO=(?P<odno>\S+) "
    r"(?P<ticker>\S+) (?P<side>BUY|SELL) (?P<qty>\d+)주"
)
RE_REJECT_GATE = re.compile(
    r"^\[OrderRouter\] 거부 \[(?P<oid>[^\]]+)\] (?P<ticker>\S+) → (?P<reason>.+)$"
)
RE_REJECT_KIS = re.compile(
    r"^\[OrderRouter\] KIS (?P<kind>예외|거부) \[(?P<oid>[^\]]+)\] (?P<ticker>\S+)"
    r"(?: — (?P<what>.+))?$"
)
RE_FILL = re.compile(
    r"^\[OrderRouter\] 체결 확인 \[(?P<oid>[^\]]+)\] ODNO=(?P<odno>\S+) "
    r"(?P<ticker>\S+) (?P<side>BUY|SELL) (?P<qty>\d+)주 @(?P<price>\d+) "
    r"\(누적 (?P<cum>\d+)/(?P<tot>\d+)주\)"
)
RE_CANCEL_OK = re.compile(r"^\[OrderRouter\] 취소 접수")
RE_REVISE_OK = re.compile(r"^\[OrderRouter\] 정정 접수")
RE_CXL_RVS_NG = re.compile(r"^\[OrderRouter\] (취소|정정) (거부|무시|예외)")
RE_DEDUP = re.compile(r"^\[OrderRouter\] 중복 체결통보 무시")
RE_FILL_MISS = re.compile(r"^\[OrderRouter\] 체결통보 매핑 실패")
RE_STALE = re.compile(r"^\[Control\] WebSocket .*재연결 시도")
RE_RECONNECT_OK = re.compile(r"^\[Control\] WebSocket 재연결 성공")
RE_RECONNECT_NG = re.compile(r"^\[Control\] WebSocket 재연결 실패")
RE_KILL = re.compile(r"KILL 명령 수신")
# 접수 라인에 붙을 수 있는 선택적 주문 왕복지연 계측 토큰 (계측 추가 시)
RE_RTT = re.compile(r"RTT=(?P<ms>\d+)ms")


def _pct(n: int, d: int) -> str:
    return f"{100.0 * n / d:.1f}%" if d else "—"


def _percentile(vals: list[float], p: float) -> float:
    """선형보간 백분위수 (numpy 없이)."""
    if not vals:
        return 0.0
    s = sorted(vals)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


@dataclass
class Session:
    """하나의 실행 세션(=== 시작 ~ 다음 === 직전)."""
    start: datetime
    end: datetime | None = None
    config: str = ""
    mode: str = ""
    token_new: int = 0
    token_cached: int = 0
    market_opens: int = 0

    signals: int = 0
    accepted: int = 0
    rejected_gate: int = 0
    rejected_kis: int = 0
    filled_orders: int = 0          # 완전체결(누적>=총량) 건수
    partial_fills: int = 0          # 부분체결 이벤트 수
    cancels: int = 0
    revises: int = 0
    cxl_rvs_fail: int = 0
    dedup: int = 0
    fill_miss: int = 0

    traded_notional: float = 0.0    # 체결가*수량 합
    reject_reasons: Counter = field(default_factory=Counter)
    accepted_by_ticker: Counter = field(default_factory=Counter)
    rtts: list[float] = field(default_factory=list)

    stale: int = 0
    reconnect_ok: int = 0
    reconnect_ng: int = 0
    kill_events: list[tuple[str, str]] = field(default_factory=list)  # (ts, msg)

    warns: int = 0
    errors: int = 0
    error_msgs: Counter = field(default_factory=Counter)

    @property
    def duration_sec(self) -> float:
        if self.end is None:
            return 0.0
        return (self.end - self.start).total_seconds()

    @property
    def total_orders(self) -> int:
        return self.accepted + self.rejected_gate + self.rejected_kis

    @property
    def rejected(self) -> int:
        return self.rejected_gate + self.rejected_kis


def parse(path: str) -> list[Session]:
    sessions: list[Session] = []
    cur: Session | None = None
    last_ts: datetime | None = None

    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    anchors = list(ANCHOR_RE.finditer(text))
    for idx, m in enumerate(anchors):
        seg_start = m.end()
        seg_end = anchors[idx + 1].start() if idx + 1 < len(anchors) else len(text)
        seg = text[seg_start:seg_end]
        nl = seg.find("\n")
        msg = seg if nl == -1 else seg[:nl]  # 메시지는 첫 줄만 (개행 누락 시 통째 수용)

        ts = datetime.strptime(m["ts"], TS_FMT)
        lvl = m["lvl"].strip()
        last_ts = ts

        if RE_SESSION.match(msg):
            if cur is not None:
                cur.end = cur.end or last_ts
            cur = Session(start=ts)
            sessions.append(cur)
            continue
        if cur is None:
            # 세션 마커 이전 라인 → 임시 세션으로 수용
            cur = Session(start=ts)
            sessions.append(cur)

        cur.end = ts

        if lvl == "WARN":
            cur.warns += 1
        elif lvl == "ERROR":
            cur.errors += 1
            cur.error_msgs[msg[:80]] += 1

        if (mm := RE_CONFIG.match(msg)):
            cur.config = mm["cfg"]
        elif (mm := RE_MODE_END.match(msg)):
            cur.mode = mm["mode"]
        elif (mm := RE_TOKEN.match(msg)):
            if "발급" in mm["kind"]:
                cur.token_new += 1
            else:
                cur.token_cached += 1
        elif RE_MKT_OPEN.match(msg):
            cur.market_opens += 1
        elif RE_SIGNAL.match(msg):
            cur.signals += 1
        elif (mm := RE_ACCEPT.match(msg)):
            cur.accepted += 1
            cur.accepted_by_ticker[mm["ticker"]] += 1
            if (r := RE_RTT.search(msg)):
                cur.rtts.append(float(r["ms"]))
        elif (mm := RE_REJECT_GATE.match(msg)):
            cur.rejected_gate += 1
            cur.reject_reasons[mm["reason"].strip()] += 1
        elif (mm := RE_REJECT_KIS.match(msg)):
            cur.rejected_kis += 1
            cur.reject_reasons[f"KIS {mm['kind']}"] += 1
        elif (mm := RE_FILL.match(msg)):
            cur.partial_fills += 1
            cur.traded_notional += int(mm["price"]) * int(mm["qty"])
            if int(mm["cum"]) >= int(mm["tot"]):
                cur.filled_orders += 1
        elif RE_CANCEL_OK.match(msg):
            cur.cancels += 1
        elif RE_REVISE_OK.match(msg):
            cur.revises += 1
        elif RE_CXL_RVS_NG.match(msg):
            cur.cxl_rvs_fail += 1
        elif RE_DEDUP.match(msg):
            cur.dedup += 1
        elif RE_FILL_MISS.match(msg):
            cur.fill_miss += 1
        elif RE_STALE.match(msg):
            cur.stale += 1
        elif RE_RECONNECT_OK.match(msg):
            cur.reconnect_ok += 1
        elif RE_RECONNECT_NG.match(msg):
            cur.reconnect_ng += 1

        if RE_KILL.search(msg):
            cur.kill_events.append((m["ts"], msg))

    return sessions


def _fmt_dur(sec: float) -> str:
    h, rem = divmod(int(sec), 3600)
    mnt, s = divmod(rem, 60)
    if h:
        return f"{h}h {mnt}m {s}s"
    if mnt:
        return f"{mnt}m {s}s"
    return f"{s}s"


@dataclass
class Aggregate:
    sessions: list[Session]

    def agg(self, attr: str) -> int:
        return sum(getattr(s, attr) for s in self.sessions)

    @property
    def reject_reasons(self) -> Counter:
        c: Counter = Counter()
        for s in self.sessions:
            c.update(s.reject_reasons)
        return c

    @property
    def rtts(self) -> list[float]:
        out: list[float] = []
        for s in self.sessions:
            out.extend(s.rtts)
        return out

    @property
    def kill_events(self) -> list[tuple[str, str]]:
        out: list[tuple[str, str]] = []
        for s in self.sessions:
            out.extend(s.kill_events)
        return out

    @property
    def error_msgs(self) -> Counter:
        c: Counter = Counter()
        for s in self.sessions:
            c.update(s.error_msgs)
        return c

    @property
    def total_uptime(self) -> float:
        return sum(s.duration_sec for s in self.sessions)


# ── 텍스트 리포트 ────────────────────────────────────────────────────────────
def render_text(sessions: list[Session], path: str) -> str:
    a = Aggregate(sessions)
    accepted = a.agg("accepted")
    rejected = a.agg("rejected_gate") + a.agg("rejected_kis")
    total_orders = accepted + rejected
    filled = a.agg("filled_orders")
    L = []
    p = L.append

    p("=" * 62)
    p(" 운용 리포트 (Ops Report) — quant_trader.log")
    p("=" * 62)
    p(f"  로그 파일     : {path}")
    p(f"  세션 수       : {len(sessions)}")
    if sessions:
        p(f"  기간          : {sessions[0].start:%Y-%m-%d %H:%M} "
          f"~ {(sessions[-1].end or sessions[-1].start):%Y-%m-%d %H:%M}")
    p(f"  누적 가동시간 : {_fmt_dur(a.total_uptime)}")
    p("")
    p("── 주문 (Order) ──────────────────────────────────────────────")
    p(f"  전략 신호     : {a.agg('signals')}")
    p(f"  주문 시도     : {total_orders}   (접수 {accepted} / 거부 {rejected})")
    p(f"  접수율        : {_pct(accepted, total_orders)}")
    p(f"  완전체결      : {filled}   부분체결 이벤트 {a.agg('partial_fills')}")
    p(f"  체결률        : {_pct(filled, accepted)}  (완전체결/접수)")
    p(f"  체결 대금     : {a.agg('traded_notional'):,.0f} 원")
    p(f"  취소/정정     : 취소 {a.agg('cancels')} / 정정 {a.agg('revises')} "
      f"/ 실패 {a.agg('cxl_rvs_fail')}")
    p(f"  체결통보      : 중복무시 {a.agg('dedup')} / 매핑실패 {a.agg('fill_miss')}")
    if total_orders == 0:
        p("  (주문 이벤트 없음 — FEED/시세 검증 세션이거나 장외 시간)")
    p("")

    if rejected:
        p("── 거부 사유 분포 (Reject reasons) ───────────────────────────")
        for reason, cnt in a.reject_reasons.most_common():
            p(f"  {cnt:>4}  {reason}")
        p("")

    rtts = a.rtts
    p("── 주문 지연 (Order latency, RTT) ────────────────────────────")
    if rtts:
        p(f"  표본 {len(rtts)}  p50 {_percentile(rtts,0.5):.0f}ms  "
          f"p99 {_percentile(rtts,0.99):.0f}ms  "
          f"max {max(rtts):.0f}ms  mean {mean(rtts):.0f}ms")
    else:
        p("  계측 없음 — 접수 로그에 RTT=Nms 토큰이 없음")
        p("  (OrderRouter에 왕복지연 계측 1줄 추가 시 여기 자동 집계)")
    p("")

    p("── 신뢰성 (Reliability) ──────────────────────────────────────")
    p(f"  WS stale 감지 : {a.agg('stale')}   재연결 성공 {a.agg('reconnect_ok')} "
      f"/ 실패 {a.agg('reconnect_ng')}")
    p(f"  WARN / ERROR  : {a.agg('warns')} / {a.agg('errors')}")
    kes = a.kill_events
    p(f"  Kill switch   : {len(kes)}건")
    for ts, msg in kes:
        p(f"      {ts}  {msg}")
    top_err = a.error_msgs.most_common(5)
    if top_err:
        p("  상위 ERROR:")
        for msg, cnt in top_err:
            p(f"      {cnt:>3}  {msg}")
    p("")

    p("── 세션별 요약 ───────────────────────────────────────────────")
    p(f"  {'#':>2} {'시작':<16} {'가동':>9} {'모드':<10} "
      f"{'신호':>4} {'접수':>4} {'거부':>4} {'체결':>4} {'ERR':>4}")
    for i, s in enumerate(sessions, 1):
        p(f"  {i:>2} {s.start:%m-%d %H:%M:%S} {_fmt_dur(s.duration_sec):>9} "
          f"{(s.mode or s.config.split('/')[-1] or '-'):<10.10} "
          f"{s.signals:>4} {s.accepted:>4} {s.rejected:>4} "
          f"{s.filled_orders:>4} {s.errors:>4}")
    p("=" * 62)
    return "\n".join(L)


# ── 마크다운 리포트 ──────────────────────────────────────────────────────────
def render_md(sessions: list[Session], path: str) -> str:
    a = Aggregate(sessions)
    accepted = a.agg("accepted")
    rejected = a.agg("rejected_gate") + a.agg("rejected_kis")
    total_orders = accepted + rejected
    filled = a.agg("filled_orders")
    rtts = a.rtts
    L = []
    p = L.append

    p(f"# 운용 리포트 — `{path}`\n")
    if sessions:
        p(f"- 기간: {sessions[0].start:%Y-%m-%d %H:%M} ~ "
          f"{(sessions[-1].end or sessions[-1].start):%Y-%m-%d %H:%M}")
    p(f"- 세션 수: **{len(sessions)}**, 누적 가동시간: **{_fmt_dur(a.total_uptime)}**\n")

    p("## 주문 지표\n")
    p("| 지표 | 값 |")
    p("|---|---|")
    p(f"| 전략 신호 | {a.agg('signals')} |")
    p(f"| 주문 시도 | {total_orders} (접수 {accepted} / 거부 {rejected}) |")
    p(f"| 접수율 | {_pct(accepted, total_orders)} |")
    p(f"| 완전체결 / 접수 (체결률) | {filled} / {accepted} = **{_pct(filled, accepted)}** |")
    p(f"| 체결 대금 | {a.agg('traded_notional'):,.0f} 원 |")
    p(f"| 취소 / 정정 / 실패 | {a.agg('cancels')} / {a.agg('revises')} / {a.agg('cxl_rvs_fail')} |")
    p(f"| 체결통보 중복무시 / 매핑실패 | {a.agg('dedup')} / {a.agg('fill_miss')} |")
    p("")

    if rejected:
        p("## 거부 사유 분포\n")
        p("| 건수 | 사유 |")
        p("|---:|---|")
        for reason, cnt in a.reject_reasons.most_common():
            p(f"| {cnt} | {reason} |")
        p("")

    p("## 주문 지연 (RTT)\n")
    if rtts:
        p(f"- 표본 {len(rtts)} | p50 **{_percentile(rtts,0.5):.0f}ms** | "
          f"p99 **{_percentile(rtts,0.99):.0f}ms** | max {max(rtts):.0f}ms | "
          f"mean {mean(rtts):.0f}ms\n")
    else:
        p("- 계측 없음 (접수 로그에 `RTT=Nms` 토큰 부재). "
          "OrderRouter에 왕복지연 계측 추가 시 자동 집계.\n")

    p("## 신뢰성\n")
    p("| 지표 | 값 |")
    p("|---|---|")
    p(f"| WS stale 감지 | {a.agg('stale')} |")
    p(f"| 재연결 성공 / 실패 | {a.agg('reconnect_ok')} / {a.agg('reconnect_ng')} |")
    p(f"| WARN / ERROR | {a.agg('warns')} / {a.agg('errors')} |")
    p(f"| Kill switch 발동 | {len(a.kill_events)} |")
    p("")
    if a.kill_events:
        p("Kill switch 이력:\n")
        for ts, msg in a.kill_events:
            p(f"- `{ts}` {msg}")
        p("")

    p("## 세션별 요약\n")
    p("| # | 시작 | 가동 | 모드 | 신호 | 접수 | 거부 | 체결 | ERR |")
    p("|---:|---|---:|---|---:|---:|---:|---:|---:|")
    for i, s in enumerate(sessions, 1):
        mode = s.mode or (s.config.split("/")[-1] if s.config else "-")
        p(f"| {i} | {s.start:%m-%d %H:%M:%S} | {_fmt_dur(s.duration_sec)} | "
          f"{mode} | {s.signals} | {s.accepted} | {s.rejected} | "
          f"{s.filled_orders} | {s.errors} |")
    p("")
    return "\n".join(L)


# ── HTML 리포트 (자기완결형, 외부 의존 없음) ─────────────────────────────────
def render_html(sessions: list[Session], path: str) -> str:
    a = Aggregate(sessions)
    accepted = a.agg("accepted")
    rejected = a.agg("rejected_gate") + a.agg("rejected_kis")
    total_orders = accepted + rejected
    filled = a.agg("filled_orders")
    rtts = a.rtts
    esc = html.escape

    def card(label: str, value: str, sub: str = "") -> str:
        sub_html = f'<div class="sub">{esc(sub)}</div>' if sub else ""
        return (f'<div class="card"><div class="lbl">{esc(label)}</div>'
                f'<div class="val">{esc(value)}</div>{sub_html}</div>')

    cards = [
        card("누적 가동시간", _fmt_dur(a.total_uptime), f"세션 {len(sessions)}"),
        card("주문 시도", str(total_orders), f"접수 {accepted} / 거부 {rejected}"),
        card("접수율", _pct(accepted, total_orders)),
        card("체결률", _pct(filled, accepted), f"완전체결 {filled}"),
        card("체결 대금", f"{a.agg('traded_notional'):,.0f}원"),
        card("주문 지연 p99",
             f"{_percentile(rtts,0.99):.0f}ms" if rtts else "계측 없음",
             f"p50 {_percentile(rtts,0.5):.0f}ms" if rtts else ""),
        card("WS 재연결", f"{a.agg('reconnect_ok')}/{a.agg('reconnect_ng')}",
             f"stale {a.agg('stale')}"),
        card("Kill switch", str(len(a.kill_events)),
             f"ERROR {a.agg('errors')}"),
    ]

    reject_rows = "".join(
        f"<tr><td>{esc(r)}</td><td class='num'>{c}</td></tr>"
        for r, c in a.reject_reasons.most_common()
    ) or "<tr><td colspan=2 class='muted'>거부 없음</td></tr>"

    sess_rows = ""
    for i, s in enumerate(sessions, 1):
        mode = s.mode or (s.config.split("/")[-1] if s.config else "-")
        sess_rows += (
            f"<tr><td class='num'>{i}</td><td>{s.start:%m-%d %H:%M:%S}</td>"
            f"<td>{_fmt_dur(s.duration_sec)}</td><td>{esc(mode)}</td>"
            f"<td class='num'>{s.signals}</td><td class='num'>{s.accepted}</td>"
            f"<td class='num'>{s.rejected}</td><td class='num'>{s.filled_orders}</td>"
            f"<td class='num'>{s.errors}</td></tr>"
        )

    period = ""
    if sessions:
        period = (f"{sessions[0].start:%Y-%m-%d %H:%M} ~ "
                  f"{(sessions[-1].end or sessions[-1].start):%Y-%m-%d %H:%M}")

    return f"""<!doctype html>
<html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>운용 리포트 — quant_trader</title>
<style>
  :root {{ --bg:#0f1115; --panel:#171a21; --line:#262b36; --fg:#e6e9ef;
          --muted:#8b93a3; --accent:#4da3ff; --ok:#39d98a; --warn:#ffb454; }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; background:var(--bg); color:var(--fg);
    font:14px/1.5 -apple-system,"Segoe UI",Roboto,"Malgun Gothic",sans-serif; padding:28px; }}
  h1 {{ font-size:20px; margin:0 0 4px; }}
  .meta {{ color:var(--muted); margin-bottom:22px; font-size:13px; }}
  .grid {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
    gap:12px; margin-bottom:26px; }}
  .card {{ background:var(--panel); border:1px solid var(--line); border-radius:10px;
    padding:14px 16px; }}
  .card .lbl {{ color:var(--muted); font-size:12px; }}
  .card .val {{ font-size:24px; font-weight:600; margin-top:4px; }}
  .card .sub {{ color:var(--muted); font-size:12px; margin-top:2px; }}
  h2 {{ font-size:15px; margin:26px 0 10px; color:var(--accent); }}
  table {{ width:100%; border-collapse:collapse; background:var(--panel);
    border:1px solid var(--line); border-radius:10px; overflow:hidden; }}
  th,td {{ padding:8px 12px; border-bottom:1px solid var(--line); text-align:left; }}
  th {{ color:var(--muted); font-weight:500; font-size:12px; }}
  td.num {{ text-align:right; font-variant-numeric:tabular-nums; }}
  .muted {{ color:var(--muted); text-align:center; }}
  tr:last-child td {{ border-bottom:none; }}
  .scroll {{ overflow-x:auto; }}
</style></head>
<body>
  <h1>운용 리포트 — quant_trader</h1>
  <div class="meta">{esc(path)} · {esc(period)}</div>
  <div class="grid">{''.join(cards)}</div>

  <h2>거부 사유 분포</h2>
  <div class="scroll"><table><thead><tr><th>사유</th><th class="num">건수</th></tr></thead>
    <tbody>{reject_rows}</tbody></table></div>

  <h2>세션별 요약</h2>
  <div class="scroll"><table><thead><tr>
    <th class="num">#</th><th>시작</th><th>가동</th><th>모드</th>
    <th class="num">신호</th><th class="num">접수</th><th class="num">거부</th>
    <th class="num">체결</th><th class="num">ERR</th></tr></thead>
    <tbody>{sess_rows}</tbody></table></div>
</body></html>"""


def main() -> int:
    ap = argparse.ArgumentParser(description="quant_trader.log 운용 리포트 생성기")
    ap.add_argument("logfile", help="quant_trader.log 경로")
    ap.add_argument("--md", metavar="PATH", help="마크다운 리포트 출력 경로")
    ap.add_argument("--html", metavar="PATH", help="HTML 리포트 출력 경로")
    args = ap.parse_args()

    # Windows 콘솔(cp949)에서 한글/박스문자 깨짐 방지 — stdout/stderr UTF-8 강제
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")  # type: ignore[union-attr]
        except (AttributeError, ValueError):
            pass

    try:
        sessions = parse(args.logfile)
    except FileNotFoundError:
        print(f"[오류] 로그 파일 없음: {args.logfile}", file=sys.stderr)
        return 1

    if not sessions:
        print("[경고] 파싱된 세션 없음 (빈 로그이거나 포맷 불일치).", file=sys.stderr)
        return 1

    print(render_text(sessions, args.logfile))

    if args.md:
        with open(args.md, "w", encoding="utf-8") as fh:
            fh.write(render_md(sessions, args.logfile))
        print(f"\n[+] 마크다운 리포트 저장: {args.md}")
    if args.html:
        with open(args.html, "w", encoding="utf-8") as fh:
            fh.write(render_html(sessions, args.logfile))
        print(f"[+] HTML 리포트 저장: {args.html}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
