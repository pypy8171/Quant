#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정적 매매·백테스트·리뷰 대시보드 생성기 — 데이터 계약 → 단일 자체완결 HTML.

대원칙(DASHBOARD_SPEC.md): 대시보드는 **정규화 스키마만 읽는다**. 엔진·연구코드를 전혀 모르고
아래 파일만 발견해 렌더한다.
  · 백테스트: `research/studies/**/metrics.json`(계열 B 배열)·`*_metrics.json`(단일객체) — quant.metrics/v1
  · 라이브   : `research/dashboard/live.json`(quant.live/v1) — trades CSV 롤업 + 매매일지 카드
  · 리뷰     : `research/dashboard/reviews.json`(quant.review/v1) — 실증 사후검토(post-mortem)
지표·라이브 요약 산출은 producer가 담당:
  PYQuant/dashboard/backfill_series_a.py  (계열 A: BACKTEST_LOG·06 TSV → metrics.json)
  PYQuant/dashboard/backfill_live.py       (logs/trades·strategies live md → live.json)
  리뷰는 strategies/<전략>/reviews/*.md 회의 산출을 손으로 reviews.json에 정규화(회의 1회당 1객체).

산출은 CDN·외부폰트·스크립트 0의 자체완결 HTML → 로컬 무서버 열람 + Artifact 공유(CSP 안전).
JS 없이도 표·차트·리뷰가 보이도록 서버측(파이썬) 렌더(스파크라인 SVG 포함), JS는 탭 토글·테마·정렬 편의만.

시각 언어: 앰버 액센트(#B7841F/#D4A02C) + 쿨잉크 뉴트럴, 넘버드 섹션·심각도 칩·토픽바 테마토글.
폰트는 자체완결 원칙대로 시스템 스택(IBM Plex 미사용).

정직성 규율(bias-auditor·GUARDRAILS): honesty_label·caveat·holdout(train→hold)·family를 표에
못박아 카드가 맥락 없이 숫자만 키우지 못하게 한다.

실행:
    python PYQuant/dashboard/backfill_series_a.py   # 계열 A metrics.json 생성/갱신
    python PYQuant/dashboard/backfill_live.py        # live.json 생성/갱신
    python PYQuant/dashboard/build_dashboard.py      # → research/dashboard/dashboard.html
"""
import html
import json
import sys
from datetime import date
from pathlib import Path

# Windows 콘솔(cp949)에서 성공 print(✅ 등) 깨짐/크래시 방지
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass

# Windows 콘솔(cp949)에서 성공 print(✅ 등) 깨짐/크래시 방지
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[2]                          # .../Quant
STUDIES = _REPO / "research" / "studies"
OUT_DIR = _REPO / "research" / "dashboard"
LIVE_JSON = OUT_DIR / "live.json"
REVIEWS_JSON = OUT_DIR / "reviews.json"
OUT_HTML = OUT_DIR / "dashboard.html"

HONESTY = {
    "robust":          ("견고", "look-ahead 차단·비용·홀드아웃 등 방법론이 견고. 승패는 지표값이 말함."),
    "honest_failure":  ("정직한 실패", "엣지 없음(또는 실패)을 정직하게 보고한 결과."),
    "overfit_suspect": ("과최적화 의심", "표본·자유도 대비 성과가 과함 — 신뢰 보류(소표본 포함)."),
    "context_required":("맥락필수", "헤드라인 숫자가 오독을 부름(비참여·생존편향 등) — 캡션·짝 해석 필수."),
    "unlabeled":       ("미분류", "정직성 라벨 미지정."),
}
FAMILY = {
    "A_portfolio": ("계열 A · 종목 포트폴리오",
                    "PYQuant 엔진, 종목 선택·비중 전략. 승률·체결수 유의. 출처: BACKTEST_LOG·06 TSV."),
    "B_overlay":   ("계열 B · 지수 익스포저 오버레이",
                    "위기 연구. 지수 0~1.2x 익스포저 토글 — 종목 포트폴리오와 직접 비교 불가(별도 비교군)."),
}


# ── 발견 ─────────────────────────────────────────────────────────────────────
def discover(root: Path):
    """metrics.json(배열) + *_metrics.json(단일객체) 전부 → 정규화 행 리스트."""
    rows, seen = [], set()
    for pat, is_arr in (("metrics.json", True), ("*_metrics.json", False)):
        for p in sorted(root.rglob(pat)):
            if p in seen:
                continue
            seen.add(p)
            try:
                data = json.loads(p.read_text(encoding="utf-8"))
            except Exception as e:
                print(f"  ! 스킵(파싱실패) {p}: {e}", file=sys.stderr)
                continue
            for r in (data if isinstance(data, list) else [data]):
                r["_src"] = str(p.relative_to(_REPO)).replace("\\", "/")
                rows.append(r)
    return rows


def load_live():
    if not LIVE_JSON.exists():
        return {"journals": [], "order_log": []}
    try:
        return json.loads(LIVE_JSON.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"  ! live.json 파싱실패: {e}", file=sys.stderr)
        return {"journals": [], "order_log": []}


def load_reviews():
    if not REVIEWS_JSON.exists():
        return []
    try:
        data = json.loads(REVIEWS_JSON.read_text(encoding="utf-8"))
        return data.get("reviews", []) if isinstance(data, dict) else []
    except Exception as e:
        print(f"  ! reviews.json 파싱실패: {e}", file=sys.stderr)
        return []


# ── 포매팅 헬퍼 ───────────────────────────────────────────────────────────────
def esc(v):
    return html.escape(str(v), quote=True)


def _isna(v):
    return v is None or (isinstance(v, float) and v != v)


def num(v, n=2, suffix=""):
    if _isna(v):
        return '<span class="na">—</span>'
    try:
        return f"{float(v):,.{n}f}{suffix}"
    except (TypeError, ValueError):
        return esc(v)


def signed(v, n=2):
    if _isna(v):
        return '<span class="na">—</span>'
    cls = "pos" if v > 0 else ("neg" if v < 0 else "zero")
    sign = "+" if v > 0 else ""
    return f'<span class="{cls}">{sign}{float(v):,.{n}f}</span>'


def honesty_cell(r):
    label = r.get("honesty_label", "unlabeled")
    ko, tip = HONESTY.get(label, HONESTY["unlabeled"])
    badge = f'<span class="badge b-{esc(label)}" title="{esc(tip)}">{esc(ko)}</span>'
    cav = r.get("caveat")
    if cav:
        badge += f' <span class="cav" title="{esc(cav)}">⚠</span>'
    return badge


def holdout_cell(r):
    """train→hold Calmar 이행. 음수 전환은 빨강으로. (그룹 배너가 전패를 설명하므로 행은 수치만)."""
    tr, ho = r.get("train_calmar"), r.get("holdout_calmar")
    if tr is None and ho is None:
        return '<span class="na">—</span>'
    return f'{num(tr,2)} <span class="arw">→</span> {signed(ho,2)}'


def alpha_cell(r):
    """초과CAGR(%p) 인라인 바 — 0 중심, +초록/−빨강. 그룹 최대치(_amax)로 스케일."""
    a = r.get("alpha")
    if _isna(a):
        return '<span class="na">—</span>'
    amax = r.get("_amax") or 1.0
    frac = min(abs(a) / amax, 1.0) if amax else 0.0
    w = frac * 50.0                      # 트랙 반너비 50%
    cls = "pos" if a > 0 else ("neg" if a < 0 else "zero")
    if a >= 0:
        fill = f'left:50%;width:{w:.1f}%'
    else:
        fill = f'right:50%;width:{w:.1f}%'
    val = f'{"+" if a > 0 else ""}{a:,.1f}'
    return (f'<span class="abar"><span class="track">'
            f'<span class="fill {cls}" style="{fill}"></span></span>'
            f'<span class="aval {cls}">{val}</span></span>')


# ── 테이블 ────────────────────────────────────────────────────────────────────
# 계열 B(오버레이): 연환산 민감 → total_return 1차. side는 전략 셀 pill로 흡수.
COLS_B = [
    ("전략", "strategy", "strat"), ("설명", "label", "left"),
    ("총수익%", "total_return", "num"), ("MDD%", "mdd", "num"),
    ("Calmar", "calmar", "num"), ("Sharpe", "sharpe", "num"),
    ("초과CAGR%p", "alpha", "abar"), ("낙폭축소%p", "mdd_red", "signed"),
    ("활성%", "active_pct", "num1"), ("홀드아웃(train→2022)", "_holdout", "raw"),
    ("정직성·비고", "_honesty", "raw"),
]
# 계열 A(포트폴리오): 백필행은 cagr/calmar/sortino/turnover 없음 → 빈 컬럼 제외.
COLS_A = [
    ("전략", "strategy", "strat"), ("이벤트", "event", "left"),
    ("총수익%", "total_return", "num"), ("MDD%", "mdd", "num"),
    ("Sharpe", "sharpe", "num"), ("승률%", "win_rate", "num1"),
    ("체결수", "n_trades", "int"), ("초과%p", "alpha", "abar"),
    ("정직성·비고", "_honesty", "raw"),
]

SIDE_PILL = {"방어": "def", "공세": "off"}


def cell(r, key, kind):
    if key == "_holdout":
        return holdout_cell(r)
    if key == "_honesty":
        return honesty_cell(r)
    if kind == "abar":
        return alpha_cell(r)
    v = r.get(key)
    if kind == "strat":
        s = esc(v) if v not in (None, "") else '<span class="na">—</span>'
        side = r.get("side")
        if side:
            s += f' <span class="pill p-{SIDE_PILL.get(side,"def")}">{esc(side)}</span>'
        return s
    if kind == "left":
        return esc(v) if v not in (None, "") else '<span class="na">—</span>'
    if kind == "num":
        return num(v, 2)
    if kind == "num1":
        return num(v, 1)
    if kind == "int":
        return num(v, 0)
    if kind == "signed":
        return signed(v, 2)
    return esc(v)


def table(rows, cols):
    head = "".join(f'<th class="c-{esc(k)}" tabindex="0" role="button" '
                   f'title="클릭·Enter로 정렬">{esc(t)}</th>' for t, k, _ in cols)
    body = []
    for r in rows:
        is_bh = r.get("strategy") == "BH"
        tds = "".join(f'<td class="c-{esc(k)} k-{esc(kind)}">{cell(r,k,kind)}</td>'
                      for _, k, kind in cols)
        body.append(f'<tr class="{"bh" if is_bh else ""}">{tds}</tr>')
    return (f'<div class="tw"><table><thead><tr>{head}</tr></thead>'
            f'<tbody>{"".join(body)}</tbody></table></div>')


def caveats_block(rows):
    """행별 caveat를 접이식 각주로 — 표를 넓히지 않고 전문을 보존."""
    items = [(r.get("strategy", ""), r.get("caveat")) for r in rows if r.get("caveat")]
    if not items:
        return ""
    lis = "".join(f'<li><b>{esc(s)}</b> — {esc(c)}</li>' for s, c in items)
    return (f'<details class="caveats"><summary>⚠ 비고 {len(items)}건 '
            f'(편향·해석 주의)</summary><ul>{lis}</ul></details>')


def holdout_banner(rows):
    flips = [r for r in rows
             if r.get("train_calmar") is not None and r.get("holdout_calmar") is not None
             and r["train_calmar"] > 0 >= r["holdout_calmar"]]
    if not flips:
        return ""
    return ('<div class="banner flip"><b>⚠ 홀드아웃(2022) 표본외 붕괴</b> — 이 그룹 '
            f'{len(flips)}개 전략 모두 학습구간 Calmar +에서 홀드아웃 Calmar −로 전환. '
            '학습구간에서 예뻐 보인 지표가 표본 밖에서 지워졌다(과최적화 신호).</div>')


# ── 백테스트 탭 ───────────────────────────────────────────────────────────────
def render_backtest(rows):
    fams = {}
    for r in rows:
        fams.setdefault(r.get("family", "A_portfolio"), []).append(r)

    sections = []
    for fam in ("B_overlay", "A_portfolio"):
        frows = fams.get(fam, [])
        title, desc = FAMILY[fam]
        if not frows:
            sections.append(f'<section class="fam"><h2>{esc(title)}</h2>'
                            f'<p class="fdesc">{esc(desc)}</p>'
                            f'<p class="empty">아직 이 계열의 <code>metrics.json</code>이 없습니다. '
                            f'백필 producer를 실행하면 자동으로 채워집니다.</p></section>')
            continue
        blocks = [f'<section class="fam"><h2>{esc(title)}</h2><p class="fdesc">{esc(desc)}</p>']
        cols = COLS_B if fam == "B_overlay" else COLS_A
        groups = {}
        for r in frows:
            groups.setdefault((r.get("study_id", ""), r.get("benchmark", "")), []).append(r)
        for (study, bench), grp in sorted(groups.items()):
            amax = max((abs(r["alpha"]) for r in grp
                        if r.get("strategy") != "BH" and not _isna(r.get("alpha"))),
                       default=1.0) or 1.0
            for r in grp:
                r["_amax"] = amax
            win = grp[0].get("window", "")
            gtitle = " · ".join(x for x in (study, bench) if x)
            n_beat = sum(1 for r in grp
                         if r.get("strategy") != "BH" and not _isna(r.get("alpha"))
                         and r["alpha"] > 0)
            n_strat = sum(1 for r in grp if r.get("strategy") != "BH")
            cap = (f'<span class="gcap">{n_strat}전략 · BH 초과 '
                   f'<b class="pos">{n_beat}</b>/{n_strat}</span>') if n_strat else ""
            blocks.append(
                f'<div class="grp"><h3>{esc(gtitle)} '
                f'<span class="win">{esc(win)}</span> {cap}</h3>'
                f'{holdout_banner(grp)}{table(grp, cols)}{caveats_block(grp)}</div>')
        blocks.append('</section>')
        sections.append("".join(blocks))
    return "".join(sections)


# ── 라이브 탭 ─────────────────────────────────────────────────────────────────
def render_live(live):
    journals = live.get("journals", [])
    order_log = live.get("order_log", [])
    out = ['<section class="fam"><h2>라이브(모의) 매매 기록</h2>'
           '<p class="fdesc">KIS 모의계좌(paper) 실증. 아래 <b>매매 일지</b>는 서술 원문 링크, '
           '<b>주문 로그</b>는 <code>logs/trades_*.csv</code> 일자별 롤업. '
           '체결가·실현손익은 체결통보 원장(별도)이라 여기선 주문흐름만 집계(손익 미생성).</p>']

    # 매매 일지 카드
    if journals:
        cards = []
        for c in journals:
            summ = c.get("summary") or c.get("strat_line") or ""
            href = "../../" + esc(c.get("path", ""))
            cards.append(
                f'<a class="card" href="{href}">'
                f'<div class="chead"><span class="cdate">{esc(c.get("date",""))}</span>'
                f'<span class="pill p-strat">{esc(c.get("strategy",""))}</span></div>'
                f'<div class="ctitle">{esc(c.get("title",""))}</div>'
                f'<div class="csumm">{esc(summ)}</div></a>')
        out.append('<div class="grp"><h3>매매 일지 <span class="win">'
                   '(카드 클릭 = 원문 md, 로컬 열람 시)</span></h3>'
                   f'<div class="cards">{"".join(cards)}</div></div>')

    # 주문 로그 롤업
    if order_log:
        rows = []
        for o in sorted(order_log, key=lambda x: x["date"], reverse=True):
            tot = o.get("total", 0) or 1
            seg = []
            for k, cls in (("accepted", "pos"), ("cancelled", "zero"), ("rejected", "neg")):
                v = o.get(k, 0)
                if v:
                    seg.append(f'<span class="seg {cls}" style="width:{v/tot*100:.1f}%" '
                               f'title="{k} {v}"></span>')
            strat = " · ".join(f'{esc(k)} {v}' for k, v in
                               list(o.get("by_strategy", {}).items())[:3])
            rows.append(
                f'<tr><td class="k-left">{esc(o.get("date",""))}</td>'
                f'<td class="k-int">{num(o.get("total"),0)}</td>'
                f'<td class="k-int"><span class="pos">{num(o.get("accepted"),0)}</span></td>'
                f'<td class="k-int"><span class="zero">{num(o.get("cancelled"),0)}</span></td>'
                f'<td class="k-int"><span class="neg">{num(o.get("rejected"),0)}</span></td>'
                f'<td class="k-int">{num(o.get("n_tickers"),0)}</td>'
                f'<td class="k-bar"><span class="stack">{"".join(seg)}</span></td>'
                f'<td class="k-left cstrat">{strat}</td></tr>')
        out.append(
            '<div class="grp"><h3>주문 로그 요약 <span class="win">'
            '(접수=초록·취소=회색·거부=빨강)</span></h3>'
            '<div class="tw"><table><thead><tr>'
            '<th>일자</th><th>총주문</th><th>접수</th><th>취소</th><th>거부</th>'
            '<th>종목수</th><th>상태 비율</th><th>전략</th></tr></thead>'
            f'<tbody>{"".join(rows)}</tbody></table></div></div>')

    if not journals and not order_log:
        out.append('<p class="empty">라이브 기록이 없습니다. '
                   '<code>python PYQuant/dashboard/backfill_live.py</code>로 생성.</p>')
    out.append('</section>')
    return "".join(out)


# ── 리뷰 탭 (사후검토) ─────────────────────────────────────────────────────────
def _sec_h(n, title):
    return (f'<div class="sec-h"><span class="n">{esc(n)}</span>'
            f'<h2>{esc(title)}</h2><span class="rule"></span></div>')


def spark_svg(pnl):
    """08-21 세션 손익 스파크라인을 서버측 SVG로 렌더(JS 불필요). 포스트모템 JS 좌표계 포팅."""
    data = pnl.get("series", [])
    if not data:
        return ""
    W, H, PADL, PADR, PADT, PADB = 640, 200, 54, 14, 16, 26
    vals = [d["v"] for d in data]
    mn = min(min(vals), pnl.get("floor", min(vals)))
    mx = max(max(vals), pnl.get("ceil", max(vals)))
    pad = (mx - mn) * 0.08
    mn -= pad
    mx += pad
    n = len(data)

    def X(i):
        return PADL + (W - PADL - PADR) * i / (n - 1 if n > 1 else 1)

    def Y(v):
        return PADT + (H - PADT - PADB) * (1 - (v - mn) / (mx - mn if mx != mn else 1))

    parts = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="08-21 세션 손익">']
    parts.append('<defs><linearGradient id="g1" x1="0" y1="0" x2="0" y2="1">'
                 '<stop offset="0%" stop-color="var(--accent)" stop-opacity="0.22"/>'
                 '<stop offset="100%" stop-color="var(--accent)" stop-opacity="0"/></linearGradient></defs>')
    # gridlines
    for g in pnl.get("gridlines", [0]):
        if g < mn or g > mx:
            continue
        y = Y(g)
        zero = (g == 0)
        parts.append(f'<line x1="{PADL}" y1="{y:.1f}" x2="{W-PADR}" y2="{y:.1f}" '
                     f'stroke="{"var(--ok)" if zero else "var(--grid)"}" '
                     f'stroke-width="{1.4 if zero else 1}" '
                     f'{"stroke-dasharray=\"5 4\"" if zero else ""}/>')
        parts.append(f'<text x="{PADL-8}" y="{y+3:.1f}" text-anchor="end" '
                     f'fill="{"var(--ok)" if zero else "var(--faint)"}" font-size="10" '
                     f'class="mono">{int(g/1000)}k</text>')
    # area + line
    dpath = f'M {X(0):.1f} {Y(vals[0]):.1f}'
    for i in range(1, n):
        dpath += f' L {X(i):.1f} {Y(vals[i]):.1f}'
    area = dpath + f' L {X(n-1):.1f} {Y(mn):.1f} L {X(0):.1f} {Y(mn):.1f} Z'
    parts.append(f'<path d="{area}" fill="url(#g1)"/>')
    parts.append(f'<path d="{dpath}" fill="none" stroke="var(--accent)" stroke-width="2" '
                 f'stroke-linejoin="round" stroke-linecap="round"/>')
    # points + x labels
    for i, d in enumerate(data):
        last = (i == n - 1)
        fill = ("var(--ok)" if d["v"] >= 0 else "var(--crit)") if last else "var(--accent)"
        parts.append(f'<circle cx="{X(i):.1f}" cy="{Y(d["v"]):.1f}" r="{4 if last else 2.5}" '
                     f'fill="{fill}" stroke="var(--surface)" stroke-width="{2 if last else 0}"/>')
        parts.append(f'<text x="{X(i):.1f}" y="{H-9}" text-anchor="middle" '
                     f'fill="var(--faint)" font-size="9.5" class="mono">{esc(d["t"])}</text>')
    # last value label
    lv = vals[-1]
    lab = f'{"+" if lv >= 0 else "−"}{abs(lv):,}'
    parts.append(f'<text x="{X(n-1)-4:.1f}" y="{Y(lv)-10:.1f}" text-anchor="end" '
                 f'fill="{"var(--ok)" if lv>=0 else "var(--crit)"}" font-size="12" '
                 f'font-weight="600" class="mono">{lab}</text>')
    parts.append('</svg>')
    return "".join(parts)


def breach_bar(br):
    """08-20 손실한도 돌파 바 — 0~scale_floor 스케일에 최저·한도·마감선 배치."""
    floor = br.get("scale_floor", br["min_value"] * 1.12)
    def pct(v):
        return max(0.0, min(100.0, abs(v) / abs(floor) * 100.0))
    fillp = pct(br["min_value"])
    limp = pct(br["limit"])
    closep = pct(br["close_value"])
    mn = f'{"−" if br["min_value"]<0 else ""}{abs(br["min_value"]):,}'
    return (
        '<div class="breach">'
        f'<div class="b0"><span class="num neg">{esc(mn)}</span>'
        f'<span class="lim">세션 최저 · 한도 −300k 초과</span></div>'
        '<div class="bar">'
        f'<div class="fill" style="width:{fillp:.1f}%"></div>'
        f'<div class="limline" style="left:{limp:.1f}%"><span class="ll">한도 −300k</span></div>'
        f'<div class="close" style="left:{closep:.1f}%"></div></div>'
        f'<small>{br.get("caption_html","")}</small></div>')


def render_reviews(reviews):
    if not reviews:
        return ('<section class="fam"><h2>실증 사후검토</h2>'
                '<p class="empty">리뷰가 없습니다. '
                '<code>research/dashboard/reviews.json</code>에 회의 산출을 정규화하세요.</p></section>')
    out = []
    for rv in reviews:
        out.append('<article class="review">')
        # hero
        out.append(
            '<div class="rv-hero">'
            f'<div class="eyebrow">{esc(rv.get("eyebrow",""))}</div>'
            f'<h1 class="rv-h1">{rv.get("headline_html","")}</h1>'
            f'<p class="verdict">{rv.get("verdict_html","")}</p>'
            '<div class="metaline">'
            + "".join(f'<span>{m}</span>' for m in rv.get("meta_html", []))
            + '</div></div>')

        # 01 KPI compare
        kpi = rv.get("kpi")
        if kpi:
            da, db = kpi["day_a"], kpi["day_b"]
            cmp_rows = []
            rws = kpi["rows"]
            for i, row in enumerate(rws):
                last = "row-last" if i == len(rws) - 1 else ""
                def cellhtml(c):
                    cls = c.get("cls", "")
                    note = f'<span class="note">{esc(c["note"])}</span>' if c.get("note") else ""
                    return (f'<div class="cell {last}"><span class="big {cls}">{esc(c["big"])}</span>{note}</div>')
                cmp_rows.append(
                    f'<div class="rl {last}">{esc(row["label"])}</div>'
                    + cellhtml(row["a"]) + cellhtml(row["b"]))
            out.append(
                _sec_h("01", "이틀 대비")
                + '<div class="cmp"><div class="ch rowlbl">지표</div>'
                + f'<div class="ch"><span class="day">{esc(da["day"])}</span><span class="wk">{esc(da["wk"])}</span></div>'
                + f'<div class="ch"><span class="day">{esc(db["day"])}</span><span class="wk">{esc(db["wk"])}</span></div>'
                + "".join(cmp_rows) + '</div>')

        # 02 incidents
        incs = rv.get("incidents", [])
        if incs:
            cards = []
            for inc in incs:
                cards.append(
                    '<div class="inc">'
                    f'<div class="tag"><span class="pill crit">{esc(inc["sev"])}</span>'
                    f'<span class="when">{esc(inc["when"])}</span></div>'
                    f'<h3>{esc(inc["title"])}</h3>'
                    f'<p>{inc.get("desc_html","")}</p>'
                    f'<div class="ev">{esc(inc.get("evidence",""))}</div>'
                    f'<div class="impact">{inc.get("impact_html","")}</div></div>')
            out.append(_sec_h("02", "크리티컬 인시던트")
                       + f'<div class="inc-grid">{"".join(cards)}</div>')

        # 03 PnL / regime
        pnl = rv.get("pnl")
        breach = rv.get("breach")
        halt = rv.get("halt")
        if pnl or breach or halt:
            left = ""
            if pnl:
                left = ('<div class="card">'
                        f'<h3>{pnl.get("title_html","")}</h3>'
                        f'<div class="chartwrap">{spark_svg(pnl)}</div>'
                        f'<p class="cardnote">{pnl.get("note_html","")}</p></div>')
            right_cards = []
            if breach:
                right_cards.append('<div class="card">'
                                   f'<h3>{breach.get("title_html","")}</h3>'
                                   f'{breach_bar(breach)}</div>')
            if halt:
                evs = "".join(
                    f'<div class="ev2"><span class="t">{esc(e["t"])}</span>'
                    f'<span class="d"><span class="s {esc(e["state"])}"></span>{e["label_html"]}</span></div>'
                    for e in halt.get("events", []))
                right_cards.append('<div class="card">'
                                   f'<h3>{halt.get("title_html","")}</h3>'
                                   f'<div class="tl">{evs}</div></div>')
            out.append(_sec_h("03", "손익 · 레짐")
                       + f'<div class="panel">{left}<div class="pcol">{"".join(right_cards)}</div></div>')

        # 04 axes
        axes = rv.get("axes", [])
        if axes:
            fcards = []
            for ax in axes:
                items = "".join(
                    f'<li><span class="k {esc(it["kind"])}">{esc(it["kind"].upper())}</span>'
                    f'<span>{it["text_html"]}</span></li>'
                    for it in ax.get("items", []))
                fcards.append(
                    '<div class="fcard"><div class="top">'
                    f'<span class="ax">{esc(ax["n"])}</span>'
                    f'<span class="pill {esc(ax.get("tag_cls","info"))}">{esc(ax.get("tag",""))}</span></div>'
                    f'<h3>{esc(ax["title"])}</h3><ul>{items}</ul></div>')
            out.append(_sec_h("04", "6축 근거분석")
                       + f'<div class="fgrid">{"".join(fcards)}</div>')

        # 05 dissent
        dis = rv.get("dissent")
        if dis:
            drows = "".join(
                f'<div class="row2"><span class="q">{esc(d["q"])}</span>'
                f'<span class="j">{d["j_html"]}</span></div>'
                for d in dis.get("items", []))
            lb = dis.get("limit_block", {})
            lb_body = "".join(f'<p>{b}</p>' for b in lb.get("body_html", []))
            out.append(_sec_h("05", "패널 이견 · 검증 한계")
                       + '<div class="two">'
                       + f'<div class="block"><h3>{esc(dis.get("title","주요 이견"))}</h3>'
                       + f'<div class="dis">{drows}</div></div>'
                       + f'<div class="block lim"><h3>{esc(lb.get("title",""))}</h3>'
                       + f'<div class="limbody">{lb_body}</div></div></div>')

        # 06 improvements
        imp = rv.get("improvements")
        if imp:
            lanes = []
            for lane in imp.get("lanes", []):
                prio = lane["prio"].lower()
                if lane["prio"] == "P0":
                    imps = "".join(
                        '<div class="imp">'
                        f'<div class="h"><span class="id">{esc(it["id"])}</span>'
                        f'<span class="chip">{esc(it["cat"])}</span>'
                        f'<span class="chip eff">{esc(it["eff"])}</span>'
                        f'<h3>{esc(it["title"])}</h3></div>'
                        f'<p class="prob">{it.get("prob_html","")}</p>'
                        + (f'<p class="fix">{it["fix_html"]}</p>' if it.get("fix_html") else "")
                        + '</div>'
                        for it in lane.get("items", []))
                else:
                    imps = "".join(
                        '<div class="imp">'
                        f'<div class="h"><span class="id">{esc(it["id"])}</span>'
                        f'<span class="chip">{esc(it["cat"])}</span>'
                        f'<span class="chip eff">{esc(it["eff"])}</span>'
                        f'<h3>{esc(it["title"])}</h3></div>'
                        f'<p class="prob">{it.get("prob_html","")}</p></div>'
                        for it in lane.get("items", []))
                lanes.append(
                    f'<div class="{prio}lane">'
                    f'<div class="lane-h {prio}"><span class="badge2">{esc(lane["prio"])}</span>'
                    f'<span class="lh">{esc(lane["title"])}</span>'
                    f'<span class="ct">{esc(lane.get("count_note",""))}</span></div>'
                    f'<div class="imps">{imps}</div></div>')
            out.append(_sec_h("06", "개선안")
                       + f'<p class="fdesc">{esc(imp.get("section_sub",""))}</p>'
                       + f'<div class="lane">{"".join(lanes)}</div>')

        # 07 gate
        gate = rv.get("gate", [])
        if gate:
            lis = "".join(f'<li><span>{g["html"]}</span></li>' for g in gate)
            out.append(_sec_h("07", "후속 게이트")
                       + f'<div class="block"><ol class="gate">{lis}</ol></div>')

        # footer of the review
        out.append(
            '<div class="rv-foot">'
            f'<div>정리 문서 <a href="../../{esc(rv.get("doc_path",""))}">'
            f'<span class="mono">{esc(rv.get("doc_path",""))}</span></a></div>'
            f'<div>근거 <span class="mono">{esc(rv.get("log_path",""))}</span></div></div>')
        out.append('</article>')
    return "".join(out)


# ── 렌더 ──────────────────────────────────────────────────────────────────────
def render(rows, live, reviews):
    n_studies = len({r.get("study_id", "") for r in rows if r.get("study_id")})
    n_fam = len({r.get("family", "A_portfolio") for r in rows})
    over = (f'<div class="stat"><b>{len(rows)}</b>백테스트행</div>'
            f'<div class="stat"><b>{n_studies}</b>스터디</div>'
            f'<div class="stat"><b>{n_fam}</b>계열</div>'
            f'<div class="stat"><b>{len(live.get("journals",[]))}</b>매매일지</div>'
            f'<div class="stat"><b>{len(live.get("order_log",[]))}</b>주문로그일</div>'
            f'<div class="stat"><b>{len(reviews)}</b>리뷰</div>')
    legend = "".join(
        f'<span class="badge b-{esc(k)}" title="{esc(v[1])}">{esc(v[0])}</span>'
        for k, v in HONESTY.items())

    tpl = HTML_TMPL
    repl = {
        "@@GEN@@": date.today().isoformat(),
        "@@OVERVIEW@@": over,
        "@@LEGEND@@": legend,
        "@@BACKTEST@@": render_backtest(rows),
        "@@LIVE@@": render_live(live),
        "@@REVIEWS@@": render_reviews(reviews),
        "@@DATA@@": html.escape(json.dumps(rows, ensure_ascii=False), quote=True),
    }
    for k, v in repl.items():
        tpl = tpl.replace(k, v)
    return tpl


HTML_TMPL = """<title>퀀트 매매 대시보드</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{
  /* 신 토큰 — 시각 언어 */
  --bg:#F4F5F7; --surface:#FFFFFF; --surface-2:#EDEFF3; --line:#DCE0E7;
  --ink:#171B21; --muted:#5B6472; --faint:#8A94A6;
  --accent:#B7841F; --accent-soft:#F3E7C8;
  --crit:#D23A3F; --warn:#C67F1E; --ok:#1F9366; --info:#4A6FA6;
  --crit-bg:#FBE9E9; --warn-bg:#FAF0DF; --ok-bg:#E4F3EC; --info-bg:#E8EFF7;
  --p0:#D23A3F; --p1:#4A6FA6; --p2:#6C7583;
  --shadow:0 1px 2px rgba(18,22,28,.05),0 4px 16px rgba(18,22,28,.05);
  --grid:rgba(23,27,33,.06);
  /* 구 토큰 — 백테스트/라이브 렌더러 호환(신 팔레트로 리토큰) */
  --card:#FFFFFF; --sub:#5B6472; --pos:#1F9366; --neg:#D23A3F; --zero:#8A94A6;
  --bh:#FAF0DF; --bhline:#EAD9B0; --flip:#C67F1E; --track:#EDEFF3; --zebra:#F7F8FA;
  --b-robust-bg:#E4F3EC; --b-robust-fg:#1F9366;
  --b-hf-bg:#EDEFF3; --b-hf-fg:#5B6472;
  --b-of-bg:#FAF0DF; --b-of-fg:#B7841F;
  --b-cr-bg:#FAF0DF; --b-cr-fg:#C67F1E;
  --b-un-bg:#EDEFF3; --b-un-fg:#8A94A6;
  --pill-bg:#EDEFF3; --pill-fg:#5B6472;
  --mono:"Cascadia Mono","SF Mono",SFMono-Regular,Consolas,"D2Coding",ui-monospace,monospace;
}
DARKVARS
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--ink);
  font:15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Malgun Gothic",sans-serif;
  -webkit-font-smoothing:antialiased}
h1,h2,h3,h4{margin:0;text-wrap:balance}
.mono{font-family:var(--mono);font-variant-numeric:tabular-nums}
a{color:var(--accent)}
.wrap{max-width:1120px;margin:0 auto;padding:0 24px}

/* topbar */
.topbar{position:sticky;top:0;z-index:20;background:color-mix(in srgb,var(--bg) 88%,transparent);
  backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
.topbar .row{display:flex;align-items:center;gap:14px;height:56px}
.brand{display:flex;align-items:center;gap:10px;font-weight:600;letter-spacing:.02em}
.dot{width:9px;height:9px;border-radius:50%;background:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
.brand .sub{color:var(--faint);font-weight:400;font-size:13px}
.spacer{flex:1}
.tzbtn{border:1px solid var(--line);background:var(--surface);color:var(--muted);
  border-radius:8px;height:34px;padding:0 12px;cursor:pointer;font:inherit;font-size:13px;
  display:flex;align-items:center;gap:7px}
.tzbtn:hover{color:var(--ink);border-color:var(--faint)}
.tzbtn:focus-visible{outline:2px solid var(--accent);outline-offset:2px}

/* header */
header.head{padding:30px 0 8px}
header.head h1{font-size:24px;font-weight:700;letter-spacing:-.01em}
header.head h1 .v{font-size:13px;color:var(--faint);font-weight:400;font-family:var(--mono)}
.gen{color:var(--faint);font-size:12px;margin-top:5px}
.overview{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0 0}
.stat{background:var(--surface);border:1px solid var(--line);border-radius:10px;
  padding:9px 15px;font-size:12.5px;color:var(--muted);box-shadow:var(--shadow)}
.stat b{font-size:19px;color:var(--ink);margin-right:5px;font-variant-numeric:tabular-nums}
.legend{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:12px 0 4px;font-size:12px}
.legend .lbl{color:var(--faint)}
.badge{display:inline-block;padding:2px 9px;border-radius:999px;font-size:11.5px;font-weight:600;white-space:nowrap}
.b-robust{background:var(--b-robust-bg);color:var(--b-robust-fg)}
.b-honest_failure{background:var(--b-hf-bg);color:var(--b-hf-fg)}
.b-overfit_suspect{background:var(--b-of-bg);color:var(--b-of-fg)}
.b-context_required{background:var(--b-cr-bg);color:var(--b-cr-fg)}
.b-unlabeled{background:var(--b-un-bg);color:var(--b-un-fg)}
.cav{color:var(--flip);font-weight:700;cursor:help}

/* tabs */
.tabs{display:flex;gap:4px;margin:16px 0 4px;border-bottom:1px solid var(--line)}
.tab-btn{appearance:none;border:0;background:none;font:inherit;font-weight:600;font-size:14px;
  color:var(--muted);padding:10px 16px;cursor:pointer;border-bottom:2px solid transparent;margin-bottom:-1px}
.tab-btn[aria-selected="true"]{color:var(--accent);border-bottom-color:var(--accent)}
.tab-btn:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:6px}
.panel[hidden]{display:none}
.panel{padding:8px 0 40px}

/* fam sections (backtest/live) */
section.fam{background:var(--surface);border:1px solid var(--line);border-radius:14px;
  padding:18px 20px;margin:16px 0;box-shadow:var(--shadow)}
section.fam h2{margin:0 0 4px;font-size:17px;font-weight:600}
.fdesc{color:var(--muted);font-size:12.5px;margin:0 0 10px}
.empty{color:var(--muted);font-size:13px;background:var(--surface-2);border:1px dashed var(--line);
  border-radius:10px;padding:14px}
.grp{margin:16px 0 6px}
.grp h3{margin:0 0 8px;font-size:14.5px;display:flex;gap:10px;align-items:baseline;flex-wrap:wrap;font-weight:600}
.grp h3 .win{color:var(--faint);font-weight:400;font-size:12px}
.gcap{font-size:11.5px;color:var(--faint);font-weight:400;margin-left:auto}
.banner{font-size:12px;border-radius:9px;padding:8px 12px;margin:0 0 8px;line-height:1.45}
.banner.flip{background:var(--b-cr-bg);color:var(--b-cr-fg);border:1px solid var(--bhline)}
.tw{overflow-x:auto;border:1px solid var(--line);border-radius:10px}
table{border-collapse:collapse;width:100%;font-size:12.5px}
th,td{padding:7px 10px;text-align:right;white-space:nowrap;border-bottom:1px solid var(--line)}
tbody tr:nth-child(even){background:var(--zebra)}
td.k-num,td.k-num1,td.k-int,td.k-signed,td.k-abar{font-family:var(--mono);font-size:12px;font-variant-numeric:tabular-nums}
th{position:sticky;top:0;z-index:2;background:var(--surface);color:var(--muted);font-weight:600;cursor:pointer;user-select:none}
th:focus-visible{outline:2px solid var(--accent);outline-offset:-2px}
th.c-strategy,th.c-side,th.c-label,th.c-event,th.c-_holdout,th.c-_honesty,
td.k-left,td.k-raw,td.k-strat,td.k-bar{text-align:left}
th.c-strategy{left:0;z-index:3}
td.c-strategy{position:sticky;left:0;background:var(--surface);font-weight:700;z-index:1}
tbody tr:nth-child(even) td.c-strategy{background:var(--zebra)}
tr.bh td.c-strategy{background:var(--bh)}
td.c-label{color:var(--muted);max-width:230px;white-space:normal}
tbody tr:hover td{background:color-mix(in srgb,var(--accent) 6%,transparent)}
tr.bh{background:var(--bh)}
tr.bh td{border-bottom:1px solid var(--bhline)}
tr.bh td.c-strategy::after{content:" ·기준선";color:var(--faint);font-weight:400;font-size:11px}
.pos{color:var(--pos)} .neg{color:var(--neg)} .zero{color:var(--zero)} .na{color:var(--zero)}
.arw{color:var(--faint)}
.pill{display:inline-flex;align-items:center;gap:6px;padding:2px 9px;border-radius:999px;font-size:12px;font-weight:600;white-space:nowrap}
.pill p-def,.pill.p-def{background:var(--b-robust-bg);color:var(--b-robust-fg)}
.pill.p-off{background:var(--b-of-bg);color:var(--b-of-fg)}
.pill.p-strat{background:color-mix(in srgb,var(--accent) 16%,transparent);color:var(--accent)}
.pill.crit{background:var(--crit-bg);color:var(--crit)}
.pill.warn{background:var(--warn-bg);color:var(--warn)}
.pill.ok{background:var(--ok-bg);color:var(--ok)}
.pill.info{background:var(--info-bg);color:var(--info)}
.pill.crit::before,.pill.warn::before,.pill.ok::before,.pill.info::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}
.abar{display:inline-flex;align-items:center;gap:7px;justify-content:flex-end}
.abar .track{position:relative;width:60px;height:8px;background:var(--track);border-radius:4px;flex:none}
.abar .track::before{content:"";position:absolute;left:50%;top:-1px;bottom:-1px;width:1px;background:var(--faint);opacity:.5}
.abar .fill{position:absolute;top:0;height:100%;border-radius:4px}
.abar .fill.pos{background:var(--pos)} .abar .fill.neg{background:var(--neg)} .abar .fill.zero{background:var(--zero)}
.abar .aval{min-width:42px;text-align:right;font-variant-numeric:tabular-nums}
.caveats{margin:8px 2px 2px;font-size:12px}
.caveats summary{cursor:pointer;color:var(--flip);font-weight:600}
.caveats ul{margin:8px 0 0;padding-left:20px;color:var(--muted);line-height:1.55}
.caveats b{color:var(--ink)}
.cards{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(250px,1fr))}
.card.j,.cards .card{display:block;text-decoration:none;color:inherit;background:var(--surface-2);
  border:1px solid var(--line);border-radius:12px;padding:13px 15px;transition:border-color .12s}
.cards .card:hover{border-color:var(--accent)}
.chead{display:flex;gap:8px;align-items:center;margin-bottom:6px}
.cdate{font-family:var(--mono);font-size:12px;color:var(--faint);font-variant-numeric:tabular-nums}
.ctitle{font-weight:700;font-size:13.5px;margin-bottom:5px;line-height:1.35}
.csumm{font-size:12px;color:var(--muted);line-height:1.5;
  display:-webkit-box;-webkit-line-clamp:4;-webkit-box-orient:vertical;overflow:hidden}
.stack{display:inline-flex;width:120px;height:9px;border-radius:5px;overflow:hidden;background:var(--track);vertical-align:middle}
.stack .seg{height:100%}
.stack .seg.pos{background:var(--pos)} .stack .seg.neg{background:var(--neg)} .stack .seg.zero{background:var(--zero)}
td.cstrat{color:var(--muted);font-size:11.5px}
.notes{margin-top:22px;font-size:12px;color:var(--muted)}
.notes h3{font-size:13px;color:var(--ink);margin:0 0 6px}
.notes li{margin:3px 0}
code{background:var(--surface-2);border:1px solid var(--line);padding:1px 5px;border-radius:5px;font-size:11.5px;font-family:var(--mono)}

/* ============ 리뷰 (사후검토) ============ */
.review{padding:6px 0 10px}
.review + .review{margin-top:20px;border-top:1px solid var(--line);padding-top:24px}
.rv-hero{padding:22px 0 8px}
.eyebrow{font-family:var(--mono);font-size:12px;letter-spacing:.16em;text-transform:uppercase;color:var(--accent);font-weight:600}
.rv-h1{font-size:clamp(26px,4vw,40px);line-height:1.1;font-weight:700;margin:14px 0 0;letter-spacing:-.01em}
.verdict{margin:20px 0 0;font-size:clamp(15px,2vw,18px);line-height:1.5;color:var(--ink);max-width:74ch;
  border-left:3px solid var(--accent);padding:4px 0 4px 18px}
.verdict b{color:var(--accent)}
.metaline{display:flex;flex-wrap:wrap;gap:8px 18px;margin-top:20px;color:var(--muted);font-size:13px}
.metaline .mono{color:var(--ink)}
.review section,.rv-sec{padding:26px 0}
.sec-h{display:flex;align-items:baseline;gap:12px;margin-bottom:16px}
.sec-h h2{font-size:20px;font-weight:600;letter-spacing:-.01em}
.sec-h .n{font-family:var(--mono);font-size:12px;color:var(--faint)}
.sec-h .rule{flex:1;height:1px;background:var(--line);align-self:center}
.cmp{display:grid;grid-template-columns:150px 1fr 1fr;border:1px solid var(--line);border-radius:14px;overflow:hidden;background:var(--surface);box-shadow:var(--shadow)}
.cmp .ch{padding:14px 16px;font-weight:600;font-size:14px;border-bottom:1px solid var(--line);background:var(--surface-2)}
.cmp .ch.rowlbl{color:var(--faint);font-weight:500;font-size:12px;text-transform:uppercase;letter-spacing:.08em;display:flex;align-items:center}
.cmp .ch .day{font-family:var(--mono)}
.cmp .ch .wk{color:var(--muted);font-weight:400;font-size:12px;margin-left:8px}
.cmp .cell{padding:13px 16px;border-bottom:1px solid var(--line);display:flex;flex-direction:column;gap:3px;justify-content:center}
.cmp .rl{padding:13px 16px;border-bottom:1px solid var(--line);color:var(--muted);font-size:13px;background:color-mix(in srgb,var(--surface-2) 45%,transparent);display:flex;align-items:center}
.cmp .row-last{border-bottom:none}
.cmp .big{font-size:20px;font-weight:600;letter-spacing:-.01em}
.cmp .big.small{font-size:14.5px}
.cmp .note{font-size:12px;color:var(--faint)}
.cmp .neg{color:var(--crit)} .cmp .pos{color:var(--ok)}
.inc-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.inc{position:relative;background:var(--surface);border:1px solid var(--line);border-radius:14px;padding:20px 20px 20px 24px;box-shadow:var(--shadow);overflow:hidden}
.inc::before{content:"";position:absolute;left:0;top:0;bottom:0;width:4px;background:var(--crit)}
.inc .tag{display:flex;align-items:center;gap:10px;margin-bottom:10px}
.inc h3{font-size:16.5px;font-weight:600}
.inc .when{font-family:var(--mono);font-size:12px;color:var(--faint);margin-left:auto}
.inc p{margin:10px 0 0;color:var(--muted);font-size:14px;line-height:1.5}
.inc .ev{margin-top:12px;background:var(--surface-2);border:1px solid var(--line);border-radius:8px;padding:9px 11px;font-family:var(--mono);font-size:11.5px;color:var(--ink);line-height:1.5;overflow-x:auto;white-space:pre-wrap;word-break:break-word}
.inc .ev b{color:var(--crit)}
.inc .impact{margin-top:11px;font-size:13px;display:flex;gap:7px;align-items:flex-start;color:var(--ink)}
.inc .impact::before{content:"→";color:var(--accent);font-weight:700}
.panel .pcol{display:flex;flex-direction:column;gap:16px}
.review .panel,.rvpanel{display:grid;grid-template-columns:1.35fr 1fr;gap:16px;padding:0}
.review .card{background:var(--surface);border:1px solid var(--line);border-radius:14px;padding:18px 20px;box-shadow:var(--shadow)}
.review .card h3{font-size:14px;font-weight:600;display:flex;align-items:center;gap:9px}
.review .card h3 .u{color:var(--faint);font-weight:400;font-size:12px;font-family:var(--mono)}
.chartwrap{margin-top:14px}
.review svg{display:block;width:100%;height:auto;overflow:visible}
.cardnote{margin-top:10px;font-size:12.5px;color:var(--muted);line-height:1.5}
.cardnote b{color:var(--ink)} .cardnote .neg{color:var(--crit)}
.breach{display:flex;flex-direction:column;gap:12px;margin-top:8px}
.breach .b0{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap}
.breach .num{font-size:32px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums}
.breach .lim{font-family:var(--mono);font-size:12px;color:var(--muted)}
.bar{position:relative;height:34px;background:var(--surface-2);border-radius:7px;border:1px solid var(--line);overflow:visible;margin-top:16px}
.bar .fill{position:absolute;top:0;bottom:0;left:0;border-radius:6px 0 0 6px;background:linear-gradient(90deg,var(--crit),color-mix(in srgb,var(--crit) 55%,var(--warn)));opacity:.85}
.bar .limline{position:absolute;top:-4px;bottom:-4px;width:2px;background:var(--warn);z-index:3}
.bar .limline .ll{position:absolute;top:-16px;left:50%;transform:translateX(-50%);font-family:var(--mono);font-size:10px;color:var(--warn);white-space:nowrap}
.bar .close{position:absolute;top:-4px;bottom:-4px;width:2px;background:var(--ink);z-index:4}
.breach small{color:var(--faint);font-size:12px;line-height:1.5}
.breach small .neg{color:var(--crit)} .breach small b{color:var(--ink)}
.tl{margin-top:6px;display:flex;flex-direction:column}
.tl .ev2{display:grid;grid-template-columns:56px 1fr;gap:12px;padding:7px 0;border-bottom:1px dashed var(--line);align-items:center}
.tl .ev2:last-child{border-bottom:none}
.tl .t{font-family:var(--mono);font-size:12px;color:var(--muted);text-align:right}
.tl .d{display:flex;align-items:center;gap:9px;font-size:13px}
.tl .d .s{width:8px;height:8px;border-radius:2px;flex:none}
.tl .faintnote{color:var(--faint);font-size:11.5px}
.s.on{background:var(--crit)} .s.off{background:var(--ok)} .s.crash{background:var(--ink)}
.fgrid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}
.fcard{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:16px 17px;box-shadow:var(--shadow)}
.fcard .top{display:flex;align-items:center;gap:9px;margin-bottom:9px}
.fcard .ax{font-family:var(--mono);font-size:11px;color:var(--faint);letter-spacing:.04em}
.fcard h3{font-size:15px;font-weight:600;margin-bottom:8px;line-height:1.3}
.fcard ul{margin:0;padding:0;list-style:none;display:flex;flex-direction:column;gap:7px}
.fcard li{display:grid;grid-template-columns:auto 1fr;gap:9px;font-size:13px;color:var(--muted);line-height:1.45}
.fcard li .k{font-family:var(--mono);font-size:10px;font-weight:600;padding:1px 6px;border-radius:5px;height:fit-content;margin-top:2px;white-space:nowrap}
.k.bug{background:var(--crit-bg);color:var(--crit)}
.k.des{background:var(--info-bg);color:var(--info)}
.k.unc{background:var(--warn-bg);color:var(--warn)}
.fcard li b{color:var(--ink);font-weight:600}
.two{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.block{background:var(--surface);border:1px solid var(--line);border-radius:14px;padding:20px 22px;box-shadow:var(--shadow)}
.block h3{font-size:15px;font-weight:600;margin-bottom:12px;display:flex;align-items:center;gap:9px}
.dis{display:flex;flex-direction:column;gap:13px}
.dis .row2{font-size:13px;line-height:1.5}
.dis .q{font-weight:600;color:var(--ink);display:block;margin-bottom:2px}
.dis .j{color:var(--muted)} .dis .j b{color:var(--accent);font-weight:600}
.block.lim{background:color-mix(in srgb,var(--warn) 7%,var(--surface))}
.limbody p{margin:0 0 10px;font-size:13px;color:var(--muted);line-height:1.55}
.limbody p:last-child{margin-bottom:0} .limbody b{color:var(--ink)}
.lane{display:flex;flex-direction:column;gap:14px}
.lane-h{display:flex;align-items:center;gap:11px;margin:6px 0 2px}
.lane-h .badge2{font-family:var(--mono);font-weight:700;font-size:13px;color:#fff;background:var(--p0);border-radius:7px;padding:3px 10px;letter-spacing:.03em}
.lane-h.p1 .badge2{background:var(--p1)} .lane-h.p2 .badge2{background:var(--p2)}
.lane-h .lh{font-weight:600;font-size:15px}
.lane-h .ct{color:var(--faint);font-size:12px;font-family:var(--mono);margin-left:auto}
.imps{display:grid;gap:12px}
.p0lane .imps{grid-template-columns:1fr}
.p1lane .imps,.p2lane .imps{grid-template-columns:repeat(2,1fr)}
.imp{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:15px 17px;box-shadow:var(--shadow)}
.p0lane .imp{border-left:3px solid var(--p0)}
.imp .h{display:flex;align-items:center;gap:10px;margin-bottom:7px;flex-wrap:wrap}
.imp .id{font-family:var(--mono);font-weight:600;font-size:12px;color:var(--accent)}
.imp h3{font-size:14.5px;font-weight:600;line-height:1.3;flex-basis:100%;order:5;margin-top:2px}
.imp .chip{font-size:11px;font-family:var(--mono);padding:1px 8px;border-radius:999px;border:1px solid var(--line);color:var(--muted);background:var(--surface-2)}
.imp .chip.eff::before{content:"노력 "}
.imp .prob{font-size:13px;color:var(--muted);line-height:1.5;margin:2px 0 0}
.imp .fix{font-size:13px;color:var(--ink);line-height:1.5;margin:9px 0 0;padding-top:9px;border-top:1px dashed var(--line)}
.imp .fix::before{content:"수정 ";font-family:var(--mono);font-size:10px;color:var(--accent);font-weight:600;letter-spacing:.05em}
.gate{list-style:none;margin:0;padding:0;counter-reset:g;display:flex;flex-direction:column;gap:11px}
.gate li{display:grid;grid-template-columns:26px 1fr;gap:11px;font-size:13.5px;line-height:1.45;color:var(--muted)}
.gate li::before{counter-increment:g;content:counter(g);font-family:var(--mono);font-weight:700;color:var(--accent);background:var(--accent-soft);border-radius:7px;height:24px;display:flex;align-items:center;justify-content:center;font-size:12px}
.gate li b{color:var(--ink);font-weight:600}
.gate .stop{color:var(--crit);font-weight:600}
.rv-foot{margin-top:20px;padding-top:16px;border-top:1px solid var(--line);color:var(--faint);font-size:12.5px;display:flex;flex-direction:column;gap:6px}
.rv-foot .mono{color:var(--muted)}

@media(max-width:820px){
  .inc-grid,.review .panel,.rvpanel,.fgrid,.two,.p1lane .imps,.p2lane .imps{grid-template-columns:1fr}
  .cmp{grid-template-columns:110px 1fr 1fr}
}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style>

<div class="topbar">
  <div class="wrap row">
    <span class="brand"><span class="dot"></span>Quant <span class="sub">매매 대시보드</span></span>
    <span class="spacer"></span>
    <button class="tzbtn" id="tz" aria-label="테마 전환"><span id="tzi">◐</span><span id="tzt">테마</span></button>
  </div>
</div>

<header class="head wrap">
  <h1>퀀트 매매 대시보드 <span class="v">quant.metrics/v1 · quant.live/v1 · quant.review/v1</span></h1>
  <div class="gen">생성 @@GEN@@ · 데이터 계약(정규화 스키마)만 렌더 · 계열·비교군 분리</div>
  <div class="overview">@@OVERVIEW@@</div>
  <div class="legend"><span class="lbl">정직성:</span>@@LEGEND@@</div>
</header>

<div class="wrap">
<div class="tabs" role="tablist">
  <button class="tab-btn" role="tab" id="tab-bt" aria-controls="panel-bt" aria-selected="true">백테스트</button>
  <button class="tab-btn" role="tab" id="tab-live" aria-controls="panel-live" aria-selected="false">라이브 매매</button>
  <button class="tab-btn" role="tab" id="tab-rv" aria-controls="panel-rv" aria-selected="false">리뷰</button>
</div>

<div class="panel" id="panel-bt" role="tabpanel" aria-labelledby="tab-bt">
@@BACKTEST@@
<div class="notes">
  <h3>읽는 법 · 규율</h3>
  <ul>
    <li><b>계열 분리</b> — 계열 B(지수 오버레이)는 종목 포트폴리오(계열 A)와 <b>직접 비교 불가</b>. 표를 계열·벤치마크로 나눈 이유.</li>
    <li><b>총수익률이 1차</b> — <code>CAGR</code>/<code>Calmar</code>는 연환산이라 창 길이에 민감. 총수익%를 먼저 본다.</li>
    <li><b>초과CAGR(막대)</b> = 전략 CAGR − Buy&amp;Hold CAGR(%p). 0 중심 바, +초록/−빨강. 그룹 최대치로 스케일.</li>
    <li><b>정직성·비고</b> — <span class="cav">⚠</span>에 마우스=편향/해석 주의(생존편향·비참여·소표본·수정주가). 표 아래 <b>비고</b>에 전문.</li>
    <li><b>맥락필수</b> 라벨 — regime-ON 비참여(현금)처럼 헤드라인 숫자가 오독을 부르는 행. 초록 '견고'와 구분.</li>
    <li><b>홀드아웃 배너</b> — 학습구간 Calmar + → 2022 − 전환(표본외 붕괴). 예뻐 보인 지표가 지우면 안 되는 사실.</li>
  </ul>
</div>
</div>

<div class="panel" id="panel-live" role="tabpanel" aria-labelledby="tab-live" hidden>
@@LIVE@@
</div>

<div class="panel" id="panel-rv" role="tabpanel" aria-labelledby="tab-rv" hidden>
@@REVIEWS@@
</div>
</div>

<script id="mdata" type="application/json">@@DATA@@</script>
<script>
// 테마 토글(3-state: system→명시 dark/light)
(function(){
  var btn=document.getElementById('tz'),ic=document.getElementById('tzi'),tt=document.getElementById('tzt');
  function cur(){var e=document.documentElement.getAttribute('data-theme');if(e)return e;
    return window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';}
  function apply(t){document.documentElement.setAttribute('data-theme',t);
    ic.textContent=t==='dark'?'\\u263e':'\\u2600';tt.textContent=t==='dark'?'다크':'라이트';}
  apply(cur());
  btn.addEventListener('click',function(){apply(cur()==='dark'?'light':'dark');});
})();
// 탭 토글(제네릭: 모든 .tab-btn)
(function(){
  var btns=[].slice.call(document.querySelectorAll('.tab-btn'));
  function sel(id){btns.forEach(function(b){
    var on=b.id===id,p=document.getElementById(b.getAttribute('aria-controls'));
    b.setAttribute('aria-selected',on?'true':'false');if(p)p.hidden=!on;});}
  btns.forEach(function(b){
    b.addEventListener('click',function(){sel(b.id);});
    b.addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();sel(b.id);}});
  });
})();
// 헤더 클릭 정렬(숫자/텍스트 자동). JS 없이도 표는 이미 완성됨.
document.querySelectorAll('table').forEach(function(tb){
  tb.querySelectorAll('th').forEach(function(th,ci){
    if(!th.hasAttribute('role')) th.setAttribute('role','button');
    function sortBy(){
      var body=tb.tBodies[0], rows=[].slice.call(body.rows);
      var asc=th.dataset.asc!=='1'; th.dataset.asc=asc?'1':'0';
      rows.sort(function(a,b){
        var x=(a.cells[ci].innerText||'').replace(/[,%p+]/g,'').trim();
        var y=(b.cells[ci].innerText||'').replace(/[,%p+]/g,'').trim();
        var nx=parseFloat(x), ny=parseFloat(y);
        var both=!isNaN(nx)&&!isNaN(ny);
        return (both?(nx-ny):x.localeCompare(y,'ko'))*(asc?1:-1);
      });
      rows.forEach(function(r){body.appendChild(r);});
    }
    th.addEventListener('click',sortBy);
    th.addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();sortBy();}});
  });
});
</script>
"""

# 다크 팔레트 — 라이트 :root를 토큰 단위로 오버라이드(3-state: system/dark/light 명시).
_DARK = """  --bg:#0E1216; --surface:#151B22; --surface-2:#1B222B; --line:#2A333E;
  --ink:#E7ECF2; --muted:#93A0B2; --faint:#66717F;
  --accent:#D4A02C; --accent-soft:#3A2F16;
  --crit:#F0656A; --warn:#E8A33D; --ok:#3FB98A; --info:#7BA0D4;
  --crit-bg:#2A1618; --warn-bg:#2A2113; --ok-bg:#13251D; --info-bg:#16202E;
  --p0:#F0656A; --p1:#7BA0D4; --p2:#8A94A6;
  --shadow:0 1px 2px rgba(0,0,0,.3),0 6px 22px rgba(0,0,0,.28);
  --grid:rgba(231,236,242,.07);
  --card:#151B22; --sub:#93A0B2; --pos:#3FB98A; --neg:#F0656A; --zero:#66717F;
  --bh:#2A2113; --bhline:#4A3D1E; --flip:#E8A33D; --track:#1B222B; --zebra:#131920;
  --b-robust-bg:#13251D; --b-robust-fg:#3FB98A;
  --b-hf-bg:#1B222B; --b-hf-fg:#93A0B2;
  --b-of-bg:#2A2113; --b-of-fg:#E8A33D;
  --b-cr-bg:#2A2113; --b-cr-fg:#E8A33D;
  --b-un-bg:#1B222B; --b-un-fg:#66717F;
  --pill-bg:#1B222B; --pill-fg:#93A0B2;"""
_DARKVARS = ("@media(prefers-color-scheme:dark){:root:not([data-theme=\"light\"]){" + _DARK + "}}\n"
             "  :root[data-theme=\"dark\"]{" + _DARK + "}")
HTML_TMPL = HTML_TMPL.replace("DARKVARS", _DARKVARS)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rows = discover(STUDIES)
    live = load_live()
    reviews = load_reviews()
    if not rows:
        print("! metrics.json을 찾지 못했습니다.", file=sys.stderr)
    OUT_HTML.write_text(render(rows, live, reviews), encoding="utf-8")
    fams = {}
    for r in rows:
        f = r.get("family", "A_portfolio")
        fams[f] = fams.get(f, 0) + 1
    print(f"✅ 대시보드 생성: {OUT_HTML}")
    print(f"   백테스트 {len(rows)}행 · 계열 {dict(fams)} · "
          f"라이브 일지 {len(live.get('journals',[]))}·주문 {len(live.get('order_log',[]))} · "
          f"리뷰 {len(reviews)}")


if __name__ == "__main__":
    main()
