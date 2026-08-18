#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정적 매매·백테스트 대시보드 생성기 — 데이터 계약 → 단일 자체완결 HTML.

대원칙(DASHBOARD_SPEC.md): 대시보드는 **정규화 스키마만 읽는다**. 엔진·연구코드를 전혀 모르고
아래 파일만 발견해 렌더한다.
  · 백테스트: `research/studies/**/metrics.json`(계열 B 배열)·`*_metrics.json`(단일객체) — quant.metrics/v1
  · 라이브   : `research/dashboard/live.json`(quant.live/v1) — trades CSV 롤업 + 매매일지 카드
지표·라이브 요약 산출은 producer가 담당:
  PYQuant/dashboard/backfill_series_a.py  (계열 A: BACKTEST_LOG·06 TSV → metrics.json)
  PYQuant/dashboard/backfill_live.py       (logs/trades·strategies live md → live.json)

산출은 CDN·외부폰트·스크립트 0의 자체완결 HTML → 로컬 무서버 열람 + Artifact 공유(CSP 안전).
JS 없이도 표·차트가 보이도록 서버측(파이썬) 렌더, JS는 탭 토글·정렬 편의만.

정직성 규율(bias-auditor·GUARDRAILS): honesty_label·caveat·holdout(train→hold)·family를 표에
못박아 카드가 맥락 없이 숫자만 키우지 못하게 한다. 예: regime-ON 비참여(현금)는 초록 '견고'가 아니라
'맥락필수'(앰버) — '현금이 시장을 이김=실력'이라는 오독을 차단.

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

_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[2]                          # .../Quant
STUDIES = _REPO / "research" / "studies"
OUT_DIR = _REPO / "research" / "dashboard"
LIVE_JSON = OUT_DIR / "live.json"
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


# ── 렌더 ──────────────────────────────────────────────────────────────────────
def render(rows, live):
    n_studies = len({r.get("study_id", "") for r in rows if r.get("study_id")})
    n_bench = len({r.get("benchmark", "") for r in rows if r.get("benchmark")})
    n_fam = len({r.get("family", "A_portfolio") for r in rows})
    n_live = len(live.get("journals", [])) + len(live.get("order_log", []))
    over = (f'<div class="stat"><b>{len(rows)}</b>백테스트행</div>'
            f'<div class="stat"><b>{n_studies}</b>스터디</div>'
            f'<div class="stat"><b>{n_fam}</b>계열</div>'
            f'<div class="stat"><b>{len(live.get("journals",[]))}</b>매매일지</div>'
            f'<div class="stat"><b>{len(live.get("order_log",[]))}</b>주문로그일</div>')
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
        "@@DATA@@": html.escape(json.dumps(rows, ensure_ascii=False), quote=True),
    }
    for k, v in repl.items():
        tpl = tpl.replace(k, v)
    return tpl


HTML_TMPL = """<title>퀀트 매매 대시보드</title>
<style>
:root{
  --bg:#f6f8fb; --card:#fff; --ink:#1a1d24; --sub:#5b6472; --line:#e3e7ee;
  --accent:#2d6cdf; --pos:#12805c; --neg:#c23b3b; --zero:#8a93a3;
  --bh:#fff7e6; --bhline:#f0d9a8; --flip:#b4401f; --track:#e8ecf3;
  --zebra:#fafbfd;
  --b-robust-bg:#e6f4ee; --b-robust-fg:#12805c;
  --b-hf-bg:#eef1f6; --b-hf-fg:#5b6472;
  --b-of-bg:#fdeede; --b-of-fg:#a15b13;
  --b-cr-bg:#fdf3e0; --b-cr-fg:#9a6a12;
  --b-un-bg:#eef1f6; --b-un-fg:#8a93a3;
  --pill-bg:#eef1f6; --pill-fg:#5b6472;
}
DARKVARS
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
  font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Malgun Gothic",sans-serif}
h1,h2,h3,h4{text-wrap:balance}
:root{--mono:"SF Mono",SFMono-Regular,ui-monospace,Consolas,"D2Coding",monospace}
.wrap{max-width:1200px;margin:0 auto;padding:26px 20px 60px}
header h1{margin:0 0 4px;font-size:22px}
.gen{color:var(--sub);font-size:12px}
.overview{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0}
.stat{background:var(--card);border:1px solid var(--line);border-radius:10px;
  padding:9px 15px;font-size:12.5px;color:var(--sub)}
.stat b{font-size:19px;color:var(--ink);margin-right:5px}
.legend{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0 4px;font-size:12px}
.legend .lbl{color:var(--sub)}
.badge{display:inline-block;padding:2px 9px;border-radius:999px;font-size:11.5px;font-weight:600;
  white-space:nowrap}
.b-robust{background:var(--b-robust-bg);color:var(--b-robust-fg)}
.b-honest_failure{background:var(--b-hf-bg);color:var(--b-hf-fg)}
.b-overfit_suspect{background:var(--b-of-bg);color:var(--b-of-fg)}
.b-context_required{background:var(--b-cr-bg);color:var(--b-cr-fg)}
.b-unlabeled{background:var(--b-un-bg);color:var(--b-un-fg)}
.cav{color:var(--flip);font-weight:700;cursor:help}
/* 탭 */
.tabs{display:flex;gap:6px;margin:14px 0 4px;border-bottom:2px solid var(--line)}
.tab-btn{appearance:none;border:0;background:none;font:inherit;font-weight:600;font-size:14px;
  color:var(--sub);padding:9px 16px;cursor:pointer;border-bottom:2px solid transparent;
  margin-bottom:-2px}
.tab-btn[aria-selected="true"]{color:var(--accent);border-bottom-color:var(--accent)}
.tab-btn:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:6px}
.panel[hidden]{display:none}
section.fam{background:var(--card);border:1px solid var(--line);border-radius:14px;
  padding:18px 20px;margin:16px 0}
section.fam h2{margin:0 0 4px;font-size:17px}
.fdesc{color:var(--sub);font-size:12.5px;margin:0 0 10px}
.empty{color:var(--sub);font-size:13px;background:var(--bg);border:1px dashed var(--line);
  border-radius:10px;padding:14px}
.grp{margin:16px 0 6px}
.grp h3{margin:0 0 8px;font-size:14.5px;display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
.grp h3 .win{color:var(--sub);font-weight:400;font-size:12px}
.gcap{font-size:11.5px;color:var(--sub);font-weight:400;margin-left:auto}
.banner{font-size:12px;border-radius:9px;padding:8px 12px;margin:0 0 8px;line-height:1.45}
.banner.flip{background:var(--b-cr-bg);color:var(--b-cr-fg);border:1px solid var(--bhline)}
.tw{overflow-x:auto;border:1px solid var(--line);border-radius:10px}
table{border-collapse:collapse;width:100%;font-size:12.5px}
th,td{padding:7px 10px;text-align:right;white-space:nowrap;border-bottom:1px solid var(--line)}
tbody tr:nth-child(even){background:var(--zebra)}
td.k-num,td.k-num1,td.k-int,td.k-signed,td.k-abar{font-family:var(--mono);font-size:12px;
  font-variant-numeric:tabular-nums}
th{position:sticky;top:0;z-index:2;background:var(--card);color:var(--sub);font-weight:600;
  cursor:pointer;user-select:none}
th:focus-visible{outline:2px solid var(--accent);outline-offset:-2px}
th.c-strategy,th.c-side,th.c-label,th.c-event,th.c-_holdout,th.c-_honesty,
td.k-left,td.k-raw,td.k-strat,td.k-bar{text-align:left}
/* sticky 첫 열(전략) — 가로 스크롤 시 맥락 유지 */
th.c-strategy{left:0;z-index:3}
td.c-strategy{position:sticky;left:0;background:var(--card);font-weight:700;z-index:1}
tbody tr:nth-child(even) td.c-strategy{background:var(--zebra)}
tr.bh td.c-strategy{background:var(--bh)}
td.c-label{color:var(--sub);max-width:230px;white-space:normal}
tbody tr:hover td{background:color-mix(in srgb,var(--accent) 6%,transparent)}
tr.bh{background:var(--bh)}
tr.bh td{border-bottom:1px solid var(--bhline)}
tr.bh td.c-strategy::after{content:" ·기준선";color:var(--sub);font-weight:400;font-size:11px}
.pos{color:var(--pos)} .neg{color:var(--neg)} .zero{color:var(--zero)} .na{color:var(--zero)}
.arw{color:var(--sub)}
.pill{display:inline-block;padding:1px 7px;border-radius:999px;font-size:10.5px;font-weight:600;
  background:var(--pill-bg);color:var(--pill-fg);vertical-align:middle}
.pill.p-def{background:var(--b-robust-bg);color:var(--b-robust-fg)}
.pill.p-off{background:var(--b-of-bg);color:var(--b-of-fg)}
.pill.p-strat{background:color-mix(in srgb,var(--accent) 16%,transparent);color:var(--accent)}
/* 인라인 초과CAGR 바 */
.abar{display:inline-flex;align-items:center;gap:7px;justify-content:flex-end}
.abar .track{position:relative;width:60px;height:8px;background:var(--track);border-radius:4px;
  flex:none}
.abar .track::before{content:"";position:absolute;left:50%;top:-1px;bottom:-1px;width:1px;
  background:var(--sub);opacity:.5}
.abar .fill{position:absolute;top:0;height:100%;border-radius:4px}
.abar .fill.pos{background:var(--pos)} .abar .fill.neg{background:var(--neg)}
.abar .fill.zero{background:var(--zero)}
.abar .aval{min-width:42px;text-align:right;font-variant-numeric:tabular-nums}
/* caveat 각주 */
.caveats{margin:8px 2px 2px;font-size:12px}
.caveats summary{cursor:pointer;color:var(--flip);font-weight:600}
.caveats ul{margin:8px 0 0;padding-left:20px;color:var(--sub);line-height:1.55}
.caveats b{color:var(--ink)}
/* 라이브 카드 */
.cards{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(250px,1fr))}
.card{display:block;text-decoration:none;color:inherit;background:var(--bg);
  border:1px solid var(--line);border-radius:12px;padding:13px 15px;transition:border-color .12s}
.card:hover{border-color:var(--accent)}
.chead{display:flex;gap:8px;align-items:center;margin-bottom:6px}
.cdate{font-family:var(--mono);font-size:12px;color:var(--sub);font-variant-numeric:tabular-nums}
.ctitle{font-weight:700;font-size:13.5px;margin-bottom:5px;line-height:1.35}
.csumm{font-size:12px;color:var(--sub);line-height:1.5;
  display:-webkit-box;-webkit-line-clamp:4;-webkit-box-orient:vertical;overflow:hidden}
/* 주문 상태 스택바 */
.stack{display:inline-flex;width:120px;height:9px;border-radius:5px;overflow:hidden;
  background:var(--track);vertical-align:middle}
.stack .seg{height:100%}
.stack .seg.pos{background:var(--pos)} .stack .seg.neg{background:var(--neg)}
.stack .seg.zero{background:var(--zero)}
td.cstrat{color:var(--sub);font-size:11.5px}
.notes{margin-top:22px;font-size:12px;color:var(--sub)}
.notes h3{font-size:13px;color:var(--ink);margin:0 0 6px}
.notes li{margin:3px 0} code{background:var(--card);border:1px solid var(--line);padding:1px 5px;
  border-radius:5px;font-size:11.5px}
</style>
<div class="wrap">
<header>
  <h1>퀀트 매매 대시보드 <span style="font-size:13px;color:var(--sub)">quant.metrics/v1 · quant.live/v1</span></h1>
  <div class="gen">생성 @@GEN@@ · 데이터 계약(정규화 스키마)만 렌더 · 계열·비교군 분리</div>
</header>
<div class="overview">@@OVERVIEW@@</div>
<div class="legend"><span class="lbl">정직성:</span>@@LEGEND@@</div>

<div class="tabs" role="tablist">
  <button class="tab-btn" role="tab" id="tab-bt" aria-controls="panel-bt" aria-selected="true">백테스트</button>
  <button class="tab-btn" role="tab" id="tab-live" aria-controls="panel-live" aria-selected="false">라이브 매매</button>
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
    <li>계열 A 출처=<code>BACKTEST_LOG.md</code>(실행 원문 단일소유자)·06 <code>summary_*.tsv</code>. 계열 B 재현=<code>BACKTEST_AS_OF=2026-08-15 INDEX_OFFLINE=1</code>.</li>
  </ul>
</div>
</div>
<div class="panel" id="panel-live" role="tabpanel" aria-labelledby="tab-live" hidden>
@@LIVE@@
</div>
</div>
<script id="mdata" type="application/json">@@DATA@@</script>
<script>
// 탭 토글
(function(){
  var tabs=[['tab-bt','panel-bt'],['tab-live','panel-live']];
  function sel(id){
    tabs.forEach(function(t){
      var b=document.getElementById(t[0]),p=document.getElementById(t[1]);
      var on=t[0]===id; b.setAttribute('aria-selected',on?'true':'false'); p.hidden=!on;
    });
  }
  tabs.forEach(function(t){
    var b=document.getElementById(t[0]);
    b.addEventListener('click',function(){sel(t[0]);});
    b.addEventListener('keydown',function(e){
      if(e.key==='Enter'||e.key===' '){e.preventDefault();sel(t[0]);}
    });
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
    th.addEventListener('keydown',function(e){
      if(e.key==='Enter'||e.key===' '){e.preventDefault();sortBy();}
    });
  });
});
</script>
"""

# 다크 팔레트 — 라이트 :root를 토큰 단위로 오버라이드(3-state: system/dark/light 명시).
_DARK = """  --bg:#0f1218; --card:#171b23; --ink:#e6e9ef; --sub:#9aa3b2; --line:#262c38;
  --accent:#5a8cf0; --pos:#3ec98e; --neg:#e06b6b; --zero:#6b7482;
  --bh:#2a2416; --bhline:#4a3d1e; --flip:#e07a4f; --track:#2a313d; --zebra:#141821;
  --b-robust-bg:#13322a; --b-robust-fg:#3ec98e;
  --b-hf-bg:#20262f; --b-hf-fg:#9aa3b2;
  --b-of-bg:#33260f; --b-of-fg:#e0a95b;
  --b-cr-bg:#332816; --b-cr-fg:#e0b978;
  --b-un-bg:#20262f; --b-un-fg:#6b7482;
  --pill-bg:#20262f; --pill-fg:#9aa3b2;"""
_DARKVARS = ("@media(prefers-color-scheme:dark){:root:not([data-theme=\"light\"]){" + _DARK + "}}\n"
             "  :root[data-theme=\"dark\"]{" + _DARK + "}")
HTML_TMPL = HTML_TMPL.replace("DARKVARS", _DARKVARS)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rows = discover(STUDIES)
    live = load_live()
    if not rows:
        print("! metrics.json을 찾지 못했습니다.", file=sys.stderr)
    OUT_HTML.write_text(render(rows, live), encoding="utf-8")
    fams = {}
    for r in rows:
        f = r.get("family", "A_portfolio")
        fams[f] = fams.get(f, 0) + 1
    print(f"✅ 대시보드 생성: {OUT_HTML}")
    print(f"   백테스트 {len(rows)}행 · 계열 {dict(fams)} · "
          f"라이브 일지 {len(live.get('journals',[]))}·주문 {len(live.get('order_log',[]))}")


if __name__ == "__main__":
    main()
