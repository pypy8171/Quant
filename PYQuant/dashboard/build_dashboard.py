#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정적 매매·백테스트·리뷰 대시보드 생성기 — 데이터 계약 → 단일 자체완결 HTML.

대원칙(DASHBOARD_SPEC.md): 대시보드는 **정규화 스키마만 읽는다**. 엔진·연구코드를 전혀 모르고
아래 파일만 발견해 렌더한다.
  · 백테스트: `research/studies/**/metrics.json`(계열 B 배열)·`*_metrics.json`(단일객체) — quant.metrics/v1
  · 라이브   : `research/dashboard/live.json`(quant.live/v1) — trades CSV 롤업 + 매매일지 카드
  · 리뷰     : `research/dashboard/reviews.json`(quant.review/v1) — 실증 사후검토(post-mortem)
  · 장전     : `docs/premarket/YYYY-MM-DD.md` — 아침 시황 브리핑(머리 `| 항목 | 값 |` 표 + `## ` 절)
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
import re
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
PREMARKET_DIR = _REPO / "docs" / "premarket"
RESEARCH_DIR = _REPO / "research"          # 리셋 라운드 종합 문서(research/RESET_*.md · RESET_*/README.md)
OUT_HTML = OUT_DIR / "dashboard.html"            # 운영용: 스터디·라이브·리뷰·장전 브리핑
OUT_PUBLIC = OUT_DIR / "dashboard_public.html"   # 공개용: 스터디 + 라이브(일지는 가림, README 링크 대상, 발행 전 _PUBLIC_FORBIDDEN 검사)
OUT_ROUNDS = OUT_DIR / "dashboard_rounds.html"   # 리서치 라운드만 — 작업 과정 문서라 공개본과 섞지 않는다
STUDY_INDEX = STUDIES / "index.json"   # 스터디마다 질문·방법·데이터·결과·왜를 손으로 적은 정본(번호순 카드 탭의 원천)

HONESTY = {
    "robust":          ("견고", "look-ahead 차단·비용·홀드아웃 등 방법론이 견고. 승패는 지표값이 말함."),
    "honest_failure":  ("벤치 못 이김", "비용을 반영하면 벤치(그냥 보유)를 이기지 못한 결과를 그대로 보고."),
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
            source = str(p.relative_to(_REPO)).replace("\\", "/")
            if isinstance(data, dict):
                data = normalize_study_dict(data, source)
            for row in data:
                row["_src"] = source
                rows.append(row)
    return rows


# ── 스터디 고유 스키마 → 정규화 행 ───────────────────────────────────────────
# 스터디 19·20·21은 러너가 자기 스키마(dict)로 metrics.json을 쓴다. 표에 실을 수 있게 quant.metrics/v1 행으로
# 바꾼다 — 숫자는 파일 값 그대로, 비고(caveat)에 "왜 그 결과인지"를 파일 안 진단 수치로 적는다.
def normalize_study_dict(data, source):
    schema = data.get("schema", "")
    if schema == "quant.metrics/macro_overlay.v1":
        return normalize_macro_overlay(data)
    if "center_judgment" in data and str(data.get("study", "")).startswith("19_"):
        return normalize_fundamental_factor(data)
    return [data]


_REGIME_KO = {"contraction": "수축", "expansion": "확장", "overheat": "과열", "recovery": "회복"}
_CHECK_KO = {
    "1_exp_minus_con_t": "① 확장−수축 월수익 t", "2_windows_pos_of_27": "② 연도 창 Calmar 개선",
    "3_neighbors_same_sign": "③ 이웃 6셀 같은 부호", "4_mdd_rel_reduction": "④ MDD 20% 감소",
    "5_cagr_giveback_pp": "⑤ CAGR 반납 ≤1%p", "6_transitions_per_year": "⑥ 전환 ≤6회/년",
    "7_excess_month_t": "⑦ 월 초과수익 t·DSR",
}
_SCALE_UPDATE_KO = {"daily": "배수 매일 갱신", "on_state_change": "배수는 축 구간 변경 시만 갱신"}


def normalize_macro_overlay(data):
    """스터디 20·21(거시 국면 오버레이 × 코스피). 셀마다 한 행 + 매수 후 보유 한 행."""
    study_id = data.get("study_id", "")
    cells = data.get("cells", {})
    if not cells:
        return []
    first = next(iter(cells.values()))
    window = first.get("period", data.get("period", ""))
    bench = "코스피 매수 후 보유"
    common = {"schema": "quant.metrics/v1", "study_id": study_id, "family": "B_overlay",
              "benchmark": bench, "window": window, "side": "long"}
    rows = [dict(common, strategy="BUY_AND_HOLD", label="코스피 지수, 노출 1.0 고정",
                 cagr=first["cagr_bh"] * 100, calmar=first["calmar_bh"], mdd=-first["mdd_bh"] * 100,
                 sharpe=first["sharpe_bh"], alpha=0.0, mdd_red=0.0, total_return=None, active_pct=100.0,
                 honesty_label="robust")]
    for name, cell in cells.items():
        checks = cell.get("checks", {})
        passed = [_CHECK_KO[key] for key, ok in checks.items() if key in _CHECK_KO and ok]
        failed = [_CHECK_KO[key] for key, ok in checks.items() if key in _CHECK_KO and not ok]
        means = cell.get("regime_month_mean_pct", {})
        since = cell.get("diagnostic_since_axes_valid") or {}
        why = []
        why.append(f"사전등록 7항목 중 {len(passed)} 통과({', '.join(passed) or '없음'}), "
                   f"미달 {len(failed)}({', '.join(failed) or '없음'}) → macro_apply={str(cell.get('macro_apply')).lower()}.")
        why.append(f"노출 변경 비용 누적 {cell.get('total_cost_pct', 0):.1f}%, 평균 노출 {cell.get('exposure_mean', 1):.2f}, "
                   f"노출<1 일수 {cell.get('days_exposure_below_1', 0):,}/{cell.get('sample_n', 0):,}, "
                   f"월 초과수익 t {cell.get('excess_month_t', float('nan')):.2f}, DSR p {cell.get('dsr_p', float('nan')):.2f}, "
                   f"연도 창 개선 {cell.get('windows_pos_of_27', 0)}/{cell.get('windows_total', 27)}.")
        if means:
            order = sorted(means.items(), key=lambda pair: -pair[1])
            why.append("국면별 코스피 월수익 평균 " + " · ".join(
                f"{_REGIME_KO.get(regime, regime)} {mean:+.2f}%" for regime, mean in order) + ".")
            if order[0][0] == "contraction":
                why.append("왜: '수축' 라벨이 붙은 달의 코스피가 가장 많이 올랐다 — 월간 거시 발표 지연으로 국면 라벨이 "
                           "저점 뒤에야 찍혀, 수축 배수 0.5가 가장 좋은 달을 잘라 낸다. 비용이 아니라 라벨 지연이 원인.")
        if since:
            why.append(f"축이 켜진 {since.get('from', '?')} 이후만 보면 MDD {since['mdd_bh']*100:.1f}% → "
                       f"{since['mdd_ov']*100:.1f}%, CAGR {since['cagr_bh']*100:.2f}% → {since['cagr_ov']*100:.2f}%"
                       f"(낙폭 방어는 있으나 반납이 합격선을 넘는다).")
        label = (f"{_SCALE_UPDATE_KO.get(cell.get('scale_update', 'daily'), cell.get('scale_update'))} · "
                 f"{'네 축(성장·물가·유동성·위험선호)' if name == 'four_axis' else '시장 축만' if name == 'market_only' else name}"
                 f" · 격자 {len(cell.get('grid', []) or [])}셀 · 비용 {cell.get('cost_bp', 0):.0f}bp")
        rows.append(dict(common, strategy=name, label=label,
                         cagr=cell["cagr_ov"] * 100, calmar=cell["calmar_ov"], mdd=-cell["mdd_ov"] * 100,
                         sharpe=cell["sharpe_ov"], alpha=(cell["cagr_ov"] - cell["cagr_bh"]) * 100,
                         mdd_red=(cell["mdd_ov"] - cell["mdd_bh"]) * 100, total_return=None,   # 둘 다 음수, 덜 빠진 만큼 +
                         active_pct=cell.get("exposure_mean", 1.0) * 100,
                         honesty_label="honest_failure" if not cell.get("macro_apply") else "robust",
                         caveat=" ".join(why)))
    return rows


def normalize_fundamental_factor(data):
    """스터디 19(재무 팩터, 월 리밸런스 동일가중). 판정 구간 60개월 숫자 한 행 + 벤치 한 행."""
    center = data.get("center_judgment", {})
    verdict = data.get("verdict", {})
    months = center.get("months", 0)
    years = months / 12.0 if months else 0.0
    net_annual = center.get("net_annual_return", 0.0)
    bench_annual = center.get("benchmark_annual_return", 0.0)
    study_id = "BT-" + str(data.get("study", "")).split("_")[0]
    common = {"schema": "quant.metrics/v1", "study_id": study_id, "family": "A_portfolio",
              "benchmark": data.get("benchmark", ""), "window": data.get("period", ""), "side": "long"}
    rows = [dict(common, strategy="BUY_AND_HOLD", event=f"판정 구간 {data.get('holdout', '')} · 연 {bench_annual*100:+.2f}%",
                 total_return=((1 + bench_annual) ** years - 1) * 100 if years else None,
                 mdd=None, sharpe=None, win_rate=None, n_trades=None, alpha=0.0, honesty_label="robust")]
    layers = [("1 초과수익 t≥2", verdict.get("layer1")), ("2 walk-forward", verdict.get("layer2")),
              ("3 ±1 이웃", verdict.get("layer3"))]
    why = [verdict.get("one_line", ""),
           "3층 판정: " + " · ".join(f"{name} {'통과' if ok else '미달'}" for name, ok in layers)
           + f" → {verdict.get('overall', '?')}.",
           f"연 순수익 {net_annual*100:.2f}%(벤치 {bench_annual*100:+.2f}%), 비용 연 {center.get('cost_drag_annual', 0)*100:.2f}%p, "
           f"편도 회전 월 {center.get('turnover_oneway_monthly', 0)*100:.0f}%, 뉴이-웨스트 t {center.get('excess_t_newey_west', 0):.2f}, "
           f"격자 {data.get('trials_prior', '?')}셀 중 중심 샤프/최대 {data.get('robustness_center_sharpe_ratio', 0):.2f}. "
           f"유니버스 중앙값 {data.get('universe_size_median', '?')}종목, 등급 {data.get('grade', '?')}(DART 회사 목록이 현재 기준이라 "
           "옛 상폐사 일부 누락 — 생존 편향 상한 미측정).",
           "왜: 방향은 맞으나 60개월 표본에서 t 1.14는 우연과 구분이 안 된다. 다음 실행은 상폐사 포함 회사 목록 뒤 새 번호로."]
    rows.append(dict(common, strategy=str(data.get("study", "")).split("/")[-1],
                     event=f"비용 {data.get('cost_level', '?')} · walk-forward {data.get('walk_forward_windows_positive')}/"
                           f"{data.get('walk_forward_windows')}창 양수 · 이웃 {data.get('robustness_neighbors_positive')}/"
                           f"{data.get('robustness_neighbors')} 양수",
                     total_return=((1 + net_annual) ** years - 1) * 100 if years else None,
                     mdd=float(data.get("mdd", 0.0)), sharpe=data.get("sharpe"), win_rate=None, n_trades=None,
                     alpha=center.get("excess_annual", 0.0) * 100,
                     honesty_label="honest_failure" if verdict.get("overall") != "통과" else "robust",
                     caveat=" ".join(sentence for sentence in why if sentence)))
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

# ── 장전 브리핑(docs/premarket) ───────────────────────────────────────────────
_PM_META_KEYS = ("발행", "본문 기준일", "스탠스", "국면 결론")
_PM_STANCE = {"관망": "info", "보수": "warn", "선별": "ok"}
_PM_WEEKDAY = "월화수목금토일"


def load_premarket():
    """docs/premarket/YYYY-MM-DD.md 전부를 최신순으로 읽는다.

    문서 형식은 docs/premarket/README.md가 정한다 — 머리 표에서 메타 네 항목을 뽑고,
    `## ` 절을 순서대로 담는다. 표 앞의 인용문(`>`)은 발행 경위 메모로 함께 싣는다.
    """
    items = []
    for f in sorted(PREMARKET_DIR.glob("????-??-??.md"), reverse=True):
        try:
            txt = f.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            print(f"  ! {f.name} 읽기 실패: {e}", file=sys.stderr)
            continue
        meta, head, sections, cur = {}, [], [], None
        for line in txt.splitlines():
            if line.startswith("## "):
                cur = [line[3:].strip(), []]
                sections.append(cur)
            elif cur is None:
                head.append(line)
            else:
                cur[1].append(line)
        for line in head:
            m = re.fullmatch(r"\|\s*([^|]+?)\s*\|\s*(.+?)\s*\|", line.strip())
            if m and m.group(1) in _PM_META_KEYS and m.group(1) not in meta:
                meta[m.group(1)] = m.group(2)
        note = " ".join(l[1:].strip() for l in head if l.startswith(">"))
        d = f.stem
        try:
            wk = _PM_WEEKDAY[date.fromisoformat(d).weekday()]
        except ValueError:
            wk = ""
        items.append({
            "date": d, "wk": wk, "file": f.relative_to(_REPO).as_posix(),
            "published": meta.get("발행", ""), "basis": meta.get("본문 기준일", ""),
            "stance": meta.get("스탠스", ""), "conclusion": meta.get("국면 결론", ""),
            "note": note, "sections": sections,
        })
    return items


def _md_inline(s):
    """볼드·코드·링크만. 브리핑 본문은 그 이상을 쓰지 않는다."""
    s = esc(s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"`(.+?)`", r"<code>\1</code>", s)
    s = re.sub(r"&lt;(https?://[^&]+)&gt;",
               r'<a href="\1" target="_blank" rel="noopener">\1</a>', s)
    s = re.sub(r"\[([^\]]+)\]\((https?://[^)]+)\)",
               r'<a href="\2" target="_blank" rel="noopener">\1</a>', s)
    s = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", s)   # 상대 링크는 대시보드에서 못 여니 글자만
    return s


def _md_table(rows):
    """`| a | b |` 줄 묶음 → 표. 둘째 줄이 `|---|` 구분선이면 첫 줄을 머리로."""
    cells = [[text.strip() for text in row.strip().strip("|").split("|")] for row in rows]
    has_head = len(cells) > 1 and all(re.fullmatch(r":?-{2,}:?", text) for text in cells[1])
    body = cells[2:] if has_head else cells
    html_rows = []
    if has_head:
        html_rows.append("<tr>" + "".join(f"<th>{_md_inline(text)}</th>" for text in cells[0]) + "</tr>")
    for row in body:
        html_rows.append("<tr>" + "".join(f"<td>{_md_inline(text)}</td>" for text in row) + "</tr>")
    return '<div class="tw"><table class="md-tbl">' + "".join(html_rows) + "</table></div>"


_MD_SKIP = re.compile(r"^\s*(?:-{3,}|\*{3,}|_{3,}|<!--.*?-->)\s*$")


def _md_block(lines):
    """문단·불릿·인용·표·코드 울타리·소제목이 있는 마크다운 조각을 HTML로."""
    out, para, ul, tbl, code = [], [], [], [], None

    def flush_para():
        if para:
            out.append(f"<p>{_md_inline(' '.join(para))}</p>")
            para.clear()

    def flush_ul():
        if ul:
            out.append("<ul>" + "".join(f"<li>{_md_inline(x)}</li>" for x in ul) + "</ul>")
            ul.clear()

    def flush_tbl():
        if tbl:
            out.append(_md_table(tbl))
            tbl.clear()

    for raw in lines:
        line = raw.rstrip()
        if code is not None:
            if line.startswith("```"):
                out.append(f"<pre>{esc(chr(10).join(code))}</pre>")
                code = None
            else:
                code.append(line)
            continue
        if line.startswith("```"):
            flush_para()
            flush_ul()
            flush_tbl()
            code = []
            continue
        if line.lstrip().startswith("|"):
            flush_para()
            flush_ul()
            tbl.append(line)
            continue
        flush_tbl()
        if _MD_SKIP.match(line):   # 구분선 `---`·자동 생성 마커 `<!-- AUTO:BEGIN -->` — 파일 안 표시일 뿐 화면에 실을 내용이 아니다
            flush_para()
            flush_ul()
            continue
        heading = re.match(r"(#{2,4})\s+(.*)", line)
        if heading:
            flush_para()
            flush_ul()
            level = min(len(heading.group(1)) + 1, 5)
            out.append(f"<h{level}>{_md_inline(heading.group(2))}</h{level}>")
        elif line.startswith("- "):
            flush_para()
            ul.append(line[2:])
        elif not line.strip():
            flush_para()
            flush_ul()
        elif line.startswith(">"):
            flush_para()
            flush_ul()
            out.append(f'<blockquote>{_md_inline(line[1:].strip())}</blockquote>')
        else:
            flush_ul()
            para.append(line.strip())
    flush_para()
    flush_ul()
    flush_tbl()
    if code is not None:
        out.append(f"<pre>{esc(chr(10).join(code))}</pre>")
    return "".join(out)


def load_rounds():
    """research/RESET_*/README.md · research/RESET_*.md 를 최신순으로. 한 라운드에 한 일·결과·남은 일이 전부 든 문서다."""
    files = sorted(RESEARCH_DIR.glob("RESET_*/README.md"), reverse=True) + \
            sorted(RESEARCH_DIR.glob("RESET_*.md"), reverse=True)
    items = []
    for path in files:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as error:
            print(f"  ! {path} 읽기 실패: {error}", file=sys.stderr)
            continue
        title = next((line[2:].strip() for line in lines if line.startswith("# ")), path.stem)
        body = [line for line in lines if not line.startswith("# ")]
        summary = next((line[1:].strip() for line in body if line.startswith(">")), "")
        attachments = []
        if path.name == "README.md":   # 라운드 폴더의 나머지 장(회의 보고 등)도 같이 싣는다 — 색인만 남고 본문이 사라지지 않게
            for sibling in sorted(path.parent.glob("*.md")):
                if sibling == path:
                    continue
                sibling_lines = sibling.read_text(encoding="utf-8", errors="replace").splitlines()
                sibling_title = next((line[2:].strip() for line in sibling_lines if line.startswith("# ")), sibling.stem)
                attachments.append({"title": sibling_title, "file": str(sibling.relative_to(_REPO)).replace("\\", "/"),
                                    "lines": [line for line in sibling_lines if not line.startswith("# ")]})
        items.append({"title": title, "file": str(path.relative_to(_REPO)).replace("\\", "/"),
                      "summary": summary, "lines": body, "attachments": attachments})
    return items


def render_rounds(items):
    if not items:
        return ('<section class="fam"><h2>리서치 라운드</h2>'
                '<p class="empty">라운드 문서가 없습니다. <code>research/RESET_*/README.md</code>에 두면 실립니다.</p></section>')
    out = ['<section class="fam"><h2>리서치 라운드 '
           f'<span class="sub">{len(items)}건 · 회의 결론·데이터 적재·공용 코드·스터디 판정·오너 결정·남은 일을 한 장씩 그대로 싣는다'
           ' — 스터디 숫자는 백테스트 탭에도 있다</span></h2>']
    for index, item in enumerate(items):
        out.append(
            f'<details class="pm"{" open" if index == 0 else ""}>'
            '<summary>'
            f'<span class="pm-sum">{_md_inline(item["title"])}</span>'
            f'<span class="pm-pub">{esc(item["file"])}</span>'
            '</summary>'
            f'<div class="pm-body">{_md_block(item["lines"])}</div>'
            + "".join(
                '<details class="pm rd-att"><summary>'
                f'<span class="pm-sum">{_md_inline(attachment["title"])}</span>'
                f'<span class="pm-pub">{esc(attachment["file"])}</span></summary>'
                f'<div class="pm-body">{_md_block(attachment["lines"])}</div></details>'
                for attachment in item.get("attachments", []))
            + '</details>')
    out.append("</section>")
    return "".join(out)


# ─── 스터디 색인 (research/studies/index.json) ───────────────────────────────
#  백테스트 탭은 숫자 표라 "어떤 모멘텀인지·어떤 하락장인지"가 안 보인다. 스터디마다 사람이 적은
#  질문·방법·데이터·결과·판정·왜·후속을 번호순 카드로 싣는다. 표가 없는 스터디(README만)도 여기엔 있다.
_STUDY_FIELDS = [("question", "질문"), ("method", "방법"), ("data", "데이터·기간"), ("result", "결과"),
                 ("verdict", "판정"), ("why", "왜"), ("followup", "후속·반영")]
# 용어 풀이 — 스터디 카드 안에서 처음 나올 때 한 번만 괄호로 붙이고, 표 머리와 탭 위 '용어 풀이' 상자에도 같은 문장을 쓴다.
# (정규식, 보이는 이름, 쉬운 말). 겹치는 낱말은 긴 것을 앞에 둔다(국면필터가 국면보다 먼저 잡히게).
_GLOSSARY = [
    (r"샤프|Sharpe", "샤프", "위험 한 단위당 얻은 수익, 1을 넘으면 쓸 만함"),
    (r"MDD", "MDD", "최대 낙폭, 고점에서 가장 많이 빠진 비율"),
    (r"CAGR", "CAGR", "연평균 수익률"),
    (r"Calmar", "Calmar", "연평균 수익 ÷ 최대 낙폭"),
    (r"승률", "승률", "이익으로 끝난 거래의 비율"),
    (r"α|알파", "α", "기준선(매수 후 보유) 대비 초과 수익"),
    (r"국면\s?필터|regime|국면", "국면", "시장이 오르는 때인지 내리는 때인지의 구분"),
    (r"모멘텀", "모멘텀", "최근 많이 오른 것이 더 오른다는 성질"),
    (r"변동성 타게팅|실현변동성|변동성", "변동성", "가격이 흔들리는 정도"),
    (r"동일가중", "동일가중", "종목마다 같은 금액"),
    (r"이동평균|\bMA\d*\b", "MA", "이동평균, 최근 N일 평균 가격 선"),
    (r"홀드아웃", "홀드아웃", "만들 때 안 본 구간으로 하는 검증"),
    (r"표본외|OOS", "표본외", "만들 때 안 본 구간"),
    (r"벤치마크|벤치", "벤치", "비교 기준선, 여기서는 매수 후 보유"),
    (r"유니버스", "유니버스", "살 수 있는 종목 후보 전체"),
    (r"노출", "노출", "돈을 실제로 넣어 둔 비중"),
    (r"VIX", "VIX", "미국 시장 공포지수"),
    (r"리밸런스", "리밸런스", "정한 비중대로 다시 맞추기"),
    (r"생존편향", "생존편향", "망한 회사가 목록에서 빠져 결과가 좋아 보이는 왜곡"),
    (r"ATR", "ATR", "하루 평균 가격 변동폭"),
    (r"\bIC\b", "IC", "신호와 다음 수익의 상관, 예측력"),
    (r"이격", "이격", "평균선에서 얼마나 떨어졌나"),
    (r"PIT|시점 고정", "시점 고정", "그때 실제로 알 수 있던 값만 씀"),
    (r"breadth", "breadth", "상승 종목 비율"),
    (r"팩터", "팩터", "수익을 가르는 종목 특성"),
    (r"눌림", "눌림", "오르던 중 잠깐 내린 자리"),
    (r"분위", "분위", "값 순서로 나눈 묶음"),
    (r"슬리피지", "슬리피지", "주문 가격과 실제 체결 가격의 차이"),
    (r"오버레이", "오버레이", "종목은 안 고르고 켜고 끄기만 덧씌움"),
    (r"\bbp\b", "bp", "1bp=0.01%"),
    (r"드래그", "드래그", "비용이 수익을 갉아먹는 몫"),
    (r"턴오버|turnover", "턴오버", "얼마나 자주 갈아탔나"),
    (r"소르티노|Sortino", "소르티노", "내릴 때 흔들림만 따진 샤프"),
]
_GLOSSARY_COMPILED = [(re.compile(pattern), plain_text) for pattern, _, plain_text in _GLOSSARY]


def annotate_terms(text, seen):
    """카드 하나 안에서 용어가 처음 나올 때만 뒤에 (쉬운 말)을 붙인다. 백틱 코드 안은 건드리지 않는다."""
    parts = text.split("`")
    for part_index in range(0, len(parts), 2):
        part = parts[part_index]
        for pattern, plain_text in _GLOSSARY_COMPILED:
            if pattern.pattern in seen:
                continue
            match = pattern.search(part)
            if match:
                seen.add(pattern.pattern)
                part = part[:match.end()] + f"({plain_text})" + part[match.end():]
        parts[part_index] = part
    return "`".join(parts)


def render_glossary():
    items = "".join(f"<li><b>{esc(name)}</b> — {esc(plain_text)}</li>" for _, name, plain_text in _GLOSSARY)
    return ('<details class="pm glossary"><summary><span class="pm-sum"><b>용어 풀이</b> · 카드 본문에는 처음 나올 때 괄호로 같은 풀이가 붙는다</span></summary>'
            f'<div class="pm-body"><ul class="glossary-list">{items}</ul></div></details>')


# 표 머리 아래 작은 글씨로 붙는 풀이
_COLUMN_PLAIN = {
    "total_return": "기간 전체 수익", "mdd": "최대 낙폭", "sharpe": "위험 대비 수익", "win_rate": "이익 거래 비율",
    "n_trades": "거래 횟수", "alpha": "기준선 대비", "cagr": "연평균 수익", "calmar": "연수익÷최대낙폭",
    "mdd_red": "낙폭 얼마나 줄였나", "active_pct": "돈 넣어 둔 기간 비율", "_holdout": "안 본 구간 검증",
    "_honesty": "믿을 수 있나",
}
_STATUS_CLASS = {"통과": "pos", "기각": "neg", "음성": "neg", "보류": "zero", "예약": "zero", "참고": "zero"}


def load_study_index():
    if not STUDY_INDEX.exists():
        return []
    try:
        items = json.loads(STUDY_INDEX.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"  ! {STUDY_INDEX} 읽기 실패: {error}", file=sys.stderr)
        return []
    return sorted((item for item in items if item.get("number")), key=lambda item: item["number"])


_SERIES_KO = {"index-macro": "지수·거시"}   # 색인의 계열 표기 중 영문 하나만 우리말로

_STUDY_NOTES = """<div class="notes">
  <h3>읽는 법 · 규율</h3>
  <ul>
    <li><b>계열 분리</b> — 계열 B(지수 오버레이)는 종목 포트폴리오(계열 A)와 <b>직접 비교 불가</b>. 표를 계열·벤치마크로 나눈 이유.</li>
    <li><b>CAGR·Calmar가 1차</b> — 창 길이가 다르면(예 98년 vs 30년) <code>총수익%</code>는 복리로 부풀어 직접 비교 불가. 연환산한 <code>CAGR%</code>·<code>Calmar</code>를 먼저 보고, 총수익%(raw)는 참고로 둔다.</li>
    <li><b>초과CAGR(막대)</b> = 전략 CAGR − 매수 후 보유 CAGR(%p). 0 중심 바, +초록/−빨강. 그룹 최대치로 스케일.</li>
    <li><b>정직성·비고</b> — <span class="cav">⚠</span>에 마우스=편향/해석 주의(생존편향·비참여·소표본·수정주가). 표 아래 <b>비고</b>에 전문.</li>
    <li><b>맥락필수</b> 라벨 — regime-ON 비참여(현금)처럼 헤드라인 숫자가 오독을 부르는 행. 초록 '견고'와 구분.</li>
    <li><b>홀드아웃 배너</b> — 학습구간 Calmar + → 2022 − 전환(표본외 붕괴). 예뻐 보인 지표가 지우면 안 되는 사실.</li>
  </ul>
</div>"""


def _study_tables(study_rows):
    """한 스터디의 숫자 표 — 벤치마크별 한 그룹. 열은 계열(A 종목 / B 지수 오버레이)로 고른다."""
    groups = {}
    for row in study_rows:
        groups.setdefault(row.get("benchmark", ""), []).append(row)
    out = []
    for bench, group_rows in sorted(groups.items()):
        family = group_rows[0].get("family", "A_portfolio")
        cols = COLS_B if family == "B_overlay" else COLS_A
        amax = max((abs(row["alpha"]) for row in group_rows
                    if row.get("strategy") not in BENCHMARK_SENTINELS and not _isna(row.get("alpha"))),
                   default=1.0) or 1.0
        for row in group_rows:
            row["_amax"] = amax
        win = group_rows[0].get("window", "")
        gtitle = " · ".join(part for part in (FAMILY[family][0], plain(bench)) if part)
        n_beat = sum(1 for row in group_rows
                     if row.get("strategy") not in BENCHMARK_SENTINELS and not _isna(row.get("alpha"))
                     and row["alpha"] > 0)
        strategy_count = sum(1 for row in group_rows if row.get("strategy") not in BENCHMARK_SENTINELS)
        caption = (f'<span class="gcap">{strategy_count}전략 · 매수 후 보유 초과 '
                   f'<b class="pos">{n_beat}</b>/{strategy_count}</span>') if strategy_count else ""
        out.append(
            f'<div class="group_rows"><h3>{esc(gtitle)} <span class="win">{esc(win)}</span> {caption}</h3>'
            f'{holdout_banner(group_rows)}{table(group_rows, cols)}{caveats_block(group_rows)}</div>')
    return "".join(out)


def render_studies(items, rows):
    """스터디 한 건 = 카드 하나. 위쪽은 사람이 적은 설명(index.json), 아래쪽은 그 번호의 숫자 표(metrics.json)."""
    by_study = {}
    for row in rows:
        study_id = str(row.get("study_id", ""))
        if study_id.startswith("BT-"):
            by_study.setdefault(study_id[3:5], []).append(row)
    if not items and not by_study:
        return ('<section class="fam"><h2>스터디 · 백테스트 결과</h2>'
                '<p class="empty">스터디가 없습니다. <code>research/studies/index.json</code>·<code>metrics.json</code>을 두면 실립니다.</p></section>')
    known = {item["number"] for item in items}
    # 표는 있는데 색인에 없는 번호 — 제목만이라도 카드로 세운다
    items = list(items) + [{"number": number, "title": NAME_MAP.get(number, f"BT-{number}"), "status": ""}
                           for number in sorted(by_study) if number not in known]
    items.sort(key=lambda item: item["number"])
    out = ['<section class="fam"><h2>스터디 · 백테스트 결과 '
           f'<span class="sub">{len(items)}건 · 번호순. 무엇을 물었고 어떻게 했고 어떤 데이터로 무엇이 나왔는지 한 장씩,'
           ' 숫자 표가 있는 스터디는 그 표까지 같은 카드에</span></h2>',
           render_glossary(),
           '<div class="tw si-nav"><table><thead><tr><th>번호</th><th>제목</th><th>계열</th><th>기간</th><th>판정</th><th>숫자 표</th></tr></thead><tbody>']
    for item in items:
        number = item["number"]
        status = item.get("status", "")
        status_class = _STATUS_CLASS.get(status[:2], "zero")
        series = _SERIES_KO.get(item.get("series", ""), item.get("series", ""))
        out.append(
            f'<tr><td class="mono"><a href="#study-{number}" data-study="{number}">{esc(number)}</a></td>'
            f'<td><a href="#study-{number}" data-study="{number}">{_md_inline(item.get("title", ""))}</a></td>'
            f'<td>{esc(series)}</td><td class="mono">{esc(item.get("period", ""))}</td>'
            f'<td><b class="{status_class}">{esc(status)}</b></td>'
            f'<td>{f"{len(by_study[number])}행" if number in by_study else "README만"}</td></tr>')
    out.append('</tbody></table></div>')
    for item in items:
        number = item["number"]
        status = item.get("status", "")
        status_class = _STATUS_CLASS.get(status[:2], "zero")
        seen_terms = set()
        body = "".join(
            f'<div class="si-row"><div class="si-key">{label}</div><div class="si-val">{_md_inline(annotate_terms(item.get(key, ""), seen_terms))}</div></div>'
            for key, label in _STUDY_FIELDS if item.get(key))
        files = " · ".join(esc(path) for path in item.get("files", []))
        tables = _study_tables(by_study[number]) if number in by_study else ""
        out.append(
            f'<details class="pm si-card" id="study-{number}"><summary>'
            f'<span class="pm-sum"><b class="mono">스터디 {esc(number)}</b> · {_md_inline(item.get("title", ""))}'
            f' <b class="si-status {status_class}">{esc(status)}</b>'
            + (f' <span class="pill p-strat si-has">숫자 표</span>' if tables else "")
            + f'</span><span class="pm-pub">{esc(item.get("period", ""))}</span></summary>'
            f'<div class="pm-body si-body">{body}'
            + (f'<div class="si-row"><div class="si-key">파일</div><div class="si-val mono">{files}</div></div>' if files else "")
            + '</div>'
            + (f'<div class="si-tables">{tables}</div>' if tables else "")
            + '</details>')
    out.append(_STUDY_NOTES)
    out.append("</section>")
    return "".join(out)


def _stance_pill(stance):
    cls = _PM_STANCE.get(stance, "")
    return f'<span class="pill pm-{cls}">{esc(stance or "—")}</span>'


def render_premarket(items):
    if not items:
        return ('<section class="fam"><h2>장전 시황 브리핑</h2>'
                '<p class="empty">브리핑이 없습니다. '
                '<code>docs/premarket/YYYY-MM-DD.md</code>에 두면 실립니다.</p></section>')
    rows = []
    for it in items:
        basis = it["basis"][5:] if len(it["basis"]) == 10 else it["basis"]
        rows.append(
            "<tr>"
            f'<td class="mono"><a href="#pm-{esc(it["date"])}">{esc(it["date"])}</a> ({esc(it["wk"])})</td>'
            f'<td class="mono">{esc(basis)}</td>'
            f'<td>{_stance_pill(it["stance"])}</td>'
            f'<td class="pm-concl">{esc(it["conclusion"])}</td>'
            "</tr>")
    out = ['<section class="fam"><h2>장전 시황 브리핑 '
           f'<span class="sub">{len(items)}건 · 평일 08:30 KST 루틴 · 정성 판단, 실제 국면은 엔진이 따로 판정</span></h2>',
           '<div class="tw"><table class="pm-tbl"><thead><tr>'
           '<th>발행일</th><th>기준일</th><th>스탠스</th><th>결론 한 줄</th></tr></thead>'
           '<tbody>' + "".join(rows) + '</tbody></table></div></section>']
    for i, it in enumerate(items):
        secs = "".join(
            f"<h3>{_md_inline(title)}</h3>{_md_block(body)}" for title, body in it["sections"])
        note = f'<div class="pm-note">{_md_inline(it["note"])}</div>' if it["note"] else ""
        out.append(
            f'<details class="pm" id="pm-{esc(it["date"])}"{" open" if i == 0 else ""}>'
            '<summary>'
            f'<span class="cdate">{esc(it["date"])} ({esc(it["wk"])})</span>'
            f'{_stance_pill(it["stance"])}'
            f'<span class="pm-sum">{esc(it["conclusion"])}</span>'
            f'<span class="pm-pub">{esc(it["published"])}</span>'
            '</summary>'
            f'{note}<div class="pm-body">{secs}</div>'
            f'<div class="jsrc">{esc(it["file"])}</div>'
            '</details>')
    return "".join(out)


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
    """초과CAGR(연복리, %p) 인라인 바 — 0 중심, +초록/−빨강. 그룹 최대치(_amax)로 스케일."""
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
# 계열 B(오버레이): 창 길이 상이(예 98yr vs 30yr) → 총수익%는 복리로 창에 종속.
# 연환산 CAGR%·Calmar가 창에 덜 민감한 1차 비교축. 총수익%는 raw로 뒤에 배치. side는 전략 셀 pill로 흡수.
COLS_B = [
    ("전략", "strategy", "strat"), ("설명", "label", "left"),
    ("CAGR%", "cagr", "num1"), ("Calmar", "calmar", "num"),
    ("MDD%", "mdd", "num"), ("Sharpe", "sharpe", "num"),
    ("초과CAGR%p", "alpha", "abar"), ("낙폭축소%p", "mdd_red", "signed"),
    ("총수익%(raw)", "total_return", "num"),
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

BENCHMARK_SENTINELS = ("BH", "BUY_AND_HOLD")  # metrics.json 벤치 행 표식 — 옛 기록은 "BH", 새 기록은 "BUY_AND_HOLD"
SIDE_PILL = {"방어": "def", "공세": "off"}


# 표시 평이화 — metrics.json에 baked-in된 축약/영문 벤치 라벨을 렌더 단계에서만 평문화.
# 데이터가 있어야 재생성되는 스터디(07~09 등)는 소스를 못 고치므로 여기서 표시만 정규화한다.
# (긴 문자열 우선 치환: 긴 원문이 그 부분 문자열보다 먼저 잡히게 순서 고정)
_PLAIN_MAP = [
    ("등가중 Buy&Hold", "동일가중 매수 후 보유"),
    ("Buy&Hold(기준선)", "매수 후 보유(기준선)"),
    ("등가중 B&H", "동일가중 매수 후 보유"),
    ("Buy&Hold", "매수 후 보유"),
    ("B&H", "매수 후 보유"),
    ("벤치 못 이김", "벤치 못 이김"),
    ("절제실험(ablation)", "제거실험"),
    ("애블레이션", "제거실험"),
    ("ablation", "제거실험"),
    ("Donchian", "채널 돌파"),
    ("등가중", "동일가중"),
]

# study_id(BT-NN) → 사람이 읽는 서술형 이름. metrics.json엔 기계 키를 남기고 표시만 교체.
# 정본: docs/STYLE_GUIDE.md · ../quant-devtools/check_plain_language.py NAME_MAP과 동기 유지.
NAME_MAP = {
    "01": "모멘텀·국면필터 롤링검증(1~5년)", "02": "변동성 타게팅 사이징",
    "03": "2022 약세장 국면필터 제거실험",  "04": "월별 시작시점 스윕",
    "05": "지표 4종 전기간 검증",           "06": "하락장 유사구간 6구간 비교",
    "07": "위기 레짐 지수레벨 특성화",      "08": "위기 인과 대응 5법",
    "09": "위기 대응·수익추구 전략 10종",   "10": "구조 국면 스코어러 제거실험",
    "11": "신호 3축 나란히 비교",
    "19": "재무 팩터 저PBR×고ROE 상위 30",
    "20": "거시 네 축 국면 오버레이(배수 매일 갱신)",
    "21": "거시 네 축 국면 오버레이(축 구간 변경 시만 갱신)",
}


def plain(v):
    if not isinstance(v, str):
        return v
    for a, b in _PLAIN_MAP:
        if a in v:
            v = v.replace(a, b)
    return v


def cell(r, key, kind):
    if key == "_holdout":
        return holdout_cell(r)
    if key == "_honesty":
        return honesty_cell(r)
    if kind == "abar":
        return alpha_cell(r)
    v = r.get(key)
    if kind == "strat":
        s = esc(plain(v)) if v not in (None, "") else '<span class="na">—</span>'
        side = r.get("side")
        if side:
            s += f' <span class="pill p-{SIDE_PILL.get(side,"def")}">{esc(side)}</span>'
        return s
    if kind == "left":
        return esc(plain(v)) if v not in (None, "") else '<span class="na">—</span>'
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
    head = "".join(f'<th class="c-{esc(key)}" tabindex="0" role="button" '
                   f'title="클릭·Enter로 정렬">{esc(label)}'
                   + (f'<small class="th-plain">{esc(_COLUMN_PLAIN[key])}</small>' if key in _COLUMN_PLAIN else "")
                   + '</th>' for label, key, _ in cols)
    body = []
    for row in rows:
        is_buy_and_hold = row.get("strategy") in BENCHMARK_SENTINELS
        row_shown = {**row, "strategy": "매수 후 보유"} if is_buy_and_hold else row  # 벤치 감지는 "BUY_AND_HOLD"(옛 "BH") 센티넬 유지, 표시만 평이화
        tds = "".join(f'<td class="c-{esc(k)} k-{esc(kind)}">{cell(row_shown,k,kind)}</td>'
                      for _, k, kind in cols)
        body.append(f'<tr class="{"benchmark" if is_buy_and_hold else ""}">{tds}</tr>')
    return (f'<div class="tw"><table><thead><tr>{head}</tr></thead>'
            f'<tbody>{"".join(body)}</tbody></table></div>')


def caveats_block(rows):
    """행별 caveat를 접이식 각주로 — 표를 넓히지 않고 전문을 보존."""
    items = [(r.get("strategy", ""), r.get("caveat")) for r in rows if r.get("caveat")]
    if not items:
        return ""
    lis = "".join(f'<li><b>{esc(plain(s))}</b> — {esc(plain(c))}</li>' for s, c in items)
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
# ── 라이브 탭 ─────────────────────────────────────────────────────────────────
def render_live(live, public=False):
    """public=True면 일지 본문의 세션 이름·계좌번호·비공개 경로를 가려서 싣는다(_mask_public)."""
    journals = live.get("journals", [])
    order_log = live.get("order_log", [])
    stamp = live.get("generated") or ""
    tail = (f' <span class="win">갱신 {esc(stamp)} · 일지 {len(journals)}건 · '
            f'원장 {len(order_log)}일</span>' if stamp else "")
    out = ['<section class="fam"><h2>라이브(모의) 매매 기록</h2>'
           '<p class="fdesc">KIS 모의계좌(paper) 실증. <b>주문 로그</b>는 <code>logs/trades_*.csv</code> 일자별 롤업 — '
           '체결 열은 최종 상태가 체결인 주문 수, 실현손익은 원장 체결 행의 합(매도측 수수료·거래세를 뺀 값)이고 '
           '평가손익은 넣지 않는다. '
           + '<b>매매 일지</b>는 카드 클릭으로 원문이 펼쳐진다'
           + ('(세션 이름·계좌번호는 가림).' if public else '.')
           + tail + '</p>']

    # 날짜별 실현손익 한 줄 요약(원장 값이 있는 날만)
    pnl_days = [day for day in order_log if day.get("realized_pnl") is not None]
    if pnl_days:
        total_pnl = sum(day["realized_pnl"] for day in pnl_days)
        wins = sum(1 for day in pnl_days if day["realized_pnl"] > 0)
        out.append(f'<p class="fdesc">실현손익 누계 <b class="{"pos" if total_pnl >= 0 else "neg"}">'
                   f'{num(total_pnl, 0)}원</b> · {len(pnl_days)}일 중 이익일 {wins}일</p>')

    # 매매 일지 카드
    if journals:
        cards = []
        for c in journals:
            summ = c.get("summary") or c.get("strat_line") or ""
            rel = c.get("path", "")
            # 원문을 상대링크로 걸면 로컬에서만 열린다. Artifact로 올리면 그 경로가 없어서
            # not found가 뜬다(자체완결 원칙 위반). 그래서 md 본문을 카드 안에 심는다.
            # <details>라 JS 없이도 펼쳐진다.
            body = ""
            try:
                body = (_REPO / rel).read_text(encoding="utf-8") if rel else ""
            except OSError:
                body = ""

            if public:
                body = _mask_public(body)

            inner = (f'<summary>'
                     f'<div class="chead"><span class="cdate">{esc(c.get("date",""))}</span>'
                     f'<span class="pill p-strat">{esc(c.get("strategy",""))}</span></div>'
                     f'<div class="ctitle">{esc(c.get("title",""))}</div>'
                     f'<div class="csumm">{esc(summ)}</div></summary>')
            if body:
                body_lines = [line for line in body.splitlines() if not line.startswith("# ")]
                inner += (f'<div class="jsrc">{esc(rel)}</div>'
                          f'<div class="jbody">{_md_block(body_lines)}</div>')
            else:
                inner += f'<div class="jsrc">원문을 찾지 못했다 — {esc(rel)}</div>'
            cards.append(f'<details class="card jcard">{inner}</details>')
        out.append('<div class="grp"><h3>매매 일지 <span class="win">'
                   + '(카드 클릭 = 원문 펼치기)</span></h3>'
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
                f'<td class="k-int"><span class="{"pos" if o.get("filled") else "zero"}">'
                f'{num(o.get("filled"),0)}</span></td>'
                f'<td class="k-int">{num(o.get("n_tickers"),0)}</td>'
                + (f'<td class="k-int"><span class="{"pos" if o["realized_pnl"] >= 0 else "neg"}">'
                   f'{num(o["realized_pnl"],0)}</span></td>'
                   f'<td class="k-int">{num((o.get("buy_notional",0)+o.get("sell_notional",0))/1e6,1)}</td>'
                   if o.get("realized_pnl") is not None else '<td class="k-int">–</td><td class="k-int">–</td>')
                + f'<td class="k-bar"><span class="stack">{"".join(seg)}</span></td>'
                f'<td class="k-left cstrat">{strat}</td></tr>')
        out.append(
            '<div class="grp"><h3>주문 로그 요약 <span class="win">'
            '(접수=초록·취소=회색·거부=빨강)</span></h3>'
            '<div class="tw"><table><thead><tr>'
            '<th>일자</th><th>총주문</th><th>접수</th><th>취소</th><th>거부</th>'
            '<th>체결</th><th>종목수</th><th>실현손익(원)</th><th>매매대금(백만)</th><th>상태 비율</th><th>전략</th></tr></thead>'
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
            # 링크가 아니라 경로 표기다. 상대링크는 Artifact에서 not found가 된다
            # (바로 아래 근거 log_path와 같은 취급).
            f'<div>정리 문서 <span class="mono">{esc(rv.get("doc_path",""))}</span></div>'
            f'<div>근거 <span class="mono">{esc(rv.get("log_path",""))}</span></div></div>')
        out.append('</article>')
    return "".join(out)


# ── 렌더 ──────────────────────────────────────────────────────────────────────
# 발행본 셋. 운영용은 매매 정보까지, 공개용은 스터디만, 리서치 라운드는 작업 과정 문서라 따로.
VARIANTS = {
    "ops":    {"out": OUT_HTML,   "title": "퀀트 매매 대시보드",   "brand": "매매 대시보드",
               "schemas": "quant.metrics/v1 · quant.live/v1 · quant.review/v1",
               "tabs": ("studies", "live", "reviews", "premarket")},
    # 공개본의 라이브 탭은 일지 원문을 싣되 세션 이름·계좌번호·비공개 경로를 가린다(_mask_public).
    "public": {"out": OUT_PUBLIC, "title": "퀀트 백테스트·모의매매 결과", "brand": "백테스트·모의매매 결과",
               "schemas": "quant.metrics/v1 · quant.live/v1", "tabs": ("studies", "live"), "public": True},
    "rounds": {"out": OUT_ROUNDS, "title": "퀀트 리서치 라운드",   "brand": "리서치 라운드",
               "schemas": "research/RESET_*", "tabs": ("rounds",)},
}

# 공개본에 있으면 안 되는 것 — 세션 이름·비공개 폴더·키 이름·계좌번호 꼴. 걸리면 파일을 안 쓴다.
_PUBLIC_FORBIDDEN = [re.compile(pattern) for pattern in
                     (r"\bquant-[0-9a-f]{2}\b", r"_private", r"app_?key", r"app_?secret", r"\b\d{8}-\d{2}\b")]

# 공개본 일지 가림 — 세션 이름(quant-53, quant-53-e4b2c7), 계좌번호(8자리-2자리), 비공개 경로, 키 이름.
# 가린 결과도 위 _PUBLIC_FORBIDDEN 검사를 한 번 더 지난다.
_PUBLIC_MASKS = [(re.compile(pattern), replacement) for pattern, replacement in (
    (r"\bquant-[0-9a-f]{2}(?:-[0-9a-f]+)?\b", "(세션)"),
    (r"\b\d{8}-\d{2}\b",                     "(계좌번호)"),
    (r"(pnl_baseline_\d{8}_)\d{8}",          r"\1(계좌번호)"),
    (r"[\w./\\-]*_private[\w./\\-]*",        "(비공개 경로)"),
    (r"app_?(?:key|secret)",                 "(키)"),
)]


# 하이픈 없는 8자리 계좌번호("주문·체결 계좌 50204275 87건")는 금액과 모양이 같아 "계좌"가 든 줄에서만 가린다.
_BARE_ACCOUNT_NUMBER = re.compile(r"\b(?!20\d{6}\b)\d{8}\b")


def _mask_public(text):
    for pattern, replacement in _PUBLIC_MASKS:
        text = pattern.sub(replacement, text)

    return "\n".join(_BARE_ACCOUNT_NUMBER.sub("(계좌번호)", line) if "계좌" in line else line
                     for line in text.split("\n"))


def render(rows, live, reviews, premarket, rounds, study_index, variant="ops"):
    variant_specification = VARIANTS[variant]
    n_fam = len({r.get("family", "A_portfolio") for r in rows})
    tabs = {   # id, 단추 이름, 개요 칸들, 본문
        "studies":   ("tab-si", "스터디 · 백테스트 결과",
                      [(len(study_index), "스터디"), (len(rows), "백테스트행"), (n_fam, "계열")],
                      lambda: render_studies(study_index, rows)),
        "live":      ("tab-live", "라이브 매매",
                      [(len(live.get("journals", [])), "매매일지"), (len(live.get("order_log", [])), "주문로그일")],
                      lambda: render_live(live, public=variant_specification.get("public", False))),
        "reviews":   ("tab-rv", "리뷰", [(len(reviews), "리뷰")], lambda: render_reviews(reviews)),
        "premarket": ("tab-pm", "장전 브리핑", [(len(premarket), "장전 브리핑")], lambda: render_premarket(premarket)),
        "rounds":    ("tab-rd", "리서치 라운드", [(len(rounds), "리서치 라운드")], lambda: render_rounds(rounds)),
    }
    buttons, panels, over = [], [], []
    for index, key in enumerate(variant_specification["tabs"]):
        button_identifier, label, overview_counts, body = tabs[key]
        panel_id = button_identifier.replace("tab-", "panel-")
        selected = "true" if index == 0 else "false"
        buttons.append(f'  <button class="tab-btn" role="tab" id="{button_identifier}" aria-controls="{panel_id}" '
                       f'aria-selected="{selected}">{label}</button>')
        panels.append(f'<div class="panel" id="{panel_id}" role="tabpanel" aria-labelledby="{button_identifier}"'
                      f'{"" if index == 0 else " hidden"}>\n{body()}\n</div>')
        over.extend(f'<div class="stat"><b>{count}</b>{name}</div>' for count, name in overview_counts)
    legend = ('<div class="legend"><span class="lbl">정직성:</span>' + "".join(
        f'<span class="badge b-{esc(grade)}" title="{esc(grade_label[1])}">{esc(grade_label[0])}</span>' for grade, grade_label in HONESTY.items())
        + '</div>') if "studies" in variant_specification["tabs"] else ""

    tpl = HTML_TMPL
    repl = {
        "@@TITLE@@": variant_specification["title"],
        "@@BRAND@@": variant_specification["brand"],
        "@@SCHEMAS@@": variant_specification["schemas"],
        "@@GEN@@": date.today().isoformat(),
        "@@OVERVIEW@@": "".join(over),
        "@@LEGEND@@": legend,
        "@@TABS@@": "\n".join(buttons),
        "@@PANELS@@": "\n\n".join(panels),
        "@@DATA@@": html.escape(json.dumps(rows if "studies" in variant_specification["tabs"] else [], ensure_ascii=False), quote=True),
    }
    for placeholder, value in repl.items():
        tpl = tpl.replace(placeholder, value)
    return tpl


HTML_TMPL = """<title>@@TITLE@@</title>
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
  --benchmark:#FAF0DF; --benchmark-line:#EAD9B0; --flip:#C67F1E; --track:#EDEFF3; --zebra:#F7F8FA;
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
.banner.flip{background:var(--b-cr-bg);color:var(--b-cr-fg);border:1px solid var(--benchmark-line)}
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
tr.benchmark td.c-strategy{background:var(--benchmark)}
td.c-label{color:var(--muted);max-width:230px;white-space:normal}
tbody tr:hover td{background:color-mix(in srgb,var(--accent) 6%,transparent)}
tr.benchmark{background:var(--benchmark)}
tr.benchmark td{border-bottom:1px solid var(--benchmark-line)}
tr.benchmark td.c-strategy::after{content:" ·기준선";color:var(--faint);font-weight:400;font-size:11px}
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
.jcard{cursor:pointer}
.jcard>summary{list-style:none;cursor:pointer}
.jcard>summary::-webkit-details-marker{display:none}
.jcard[open]{grid-column:1/-1;cursor:default}
.jcard[open] .csumm{-webkit-line-clamp:unset;display:block}
.jsrc{margin-top:10px;font-family:var(--mono);font-size:11px;color:var(--faint)}
.jbody{margin-top:6px;max-height:60vh;overflow:auto;word-break:break-word;
  font-size:13px;line-height:1.6;color:var(--ink);
  background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:12px 14px}
.jbody h3,.jbody h4,.jbody h5{margin:14px 0 6px;font-size:13.5px}
.jbody p{margin:6px 0}
.jbody ul{margin:6px 0 6px 18px}
.jbody blockquote{margin:6px 0;padding:6px 10px;border-left:3px solid var(--accent);background:var(--surface-2)}
.jbody table{font-size:12px;margin:8px 0}
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

/* ============ 장전 브리핑 ============ */
section.fam h2 .sub{color:var(--faint);font-weight:400;font-size:12px;margin-left:8px}
.pm-tbl{margin-top:8px}
.pm-tbl td.pm-concl{font-size:12.5px;color:var(--muted);line-height:1.45;white-space:normal}
.pm-tbl td.mono a{color:var(--ink);text-decoration:none;border-bottom:1px dotted var(--faint)}
.pill.pm-info{background:var(--info-bg);color:var(--info)}
.pill.pm-warn{background:var(--warn-bg);color:var(--warn)}
.pill.pm-ok{background:var(--ok-bg);color:var(--ok)}
.pm{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:12px 16px;margin-top:12px;box-shadow:var(--shadow)}
.pm>summary{list-style:none;cursor:pointer;display:flex;flex-wrap:wrap;gap:8px 12px;align-items:center}
.pm>summary::-webkit-details-marker{display:none}
.pm .pm-sum{flex:1 1 40ch;font-size:13px;color:var(--ink);line-height:1.45}
.pm .pm-pub{font-family:var(--mono);font-size:11px;color:var(--faint);white-space:nowrap}
.pm-note{margin-top:10px;padding:8px 12px;border-left:3px solid var(--accent);background:var(--surface-2);
  font-size:12px;color:var(--muted);line-height:1.55}
.pm-body{margin-top:8px;max-width:78ch;font-size:13.5px;line-height:1.7;color:var(--ink)}
.pm-body h3{font-size:14px;margin:18px 0 6px;color:var(--ink)}
.pm-body p{margin:0 0 10px}
.pm-body ul{margin:0 0 10px;padding-left:20px}
.pm-body li{margin:4px 0}
.pm-body blockquote{margin:0 0 10px;padding-left:12px;border-left:3px solid var(--line);color:var(--muted)}
.pm-body h4{font-size:13.5px;margin:14px 0 4px;color:var(--ink)}
.pm-body h5{font-size:13px;margin:10px 0 4px;color:var(--muted)}
.pm-body pre{margin:0 0 10px;padding:8px 12px;background:var(--surface-2);border:1px solid var(--line);border-radius:8px;font-family:var(--mono);font-size:11.5px;overflow-x:auto;white-space:pre}
.pm-body .tw{margin:0 0 12px}
.pm-body table.md-tbl th,.pm-body table.md-tbl td{text-align:left;white-space:normal;vertical-align:top;font-size:12px}
.pm-body table.md-tbl th{position:static;cursor:default}
.pm-body{max-width:none}
.rd-att{margin:8px 0 0 16px}
.si-link{font-size:11.5px;font-weight:400;color:var(--accent);text-decoration:none;border-bottom:1px dotted var(--accent)}
.si-nav td a{color:var(--ink);text-decoration:none;border-bottom:1px dotted var(--faint)}
.si-nav td,.si-nav th{white-space:normal;text-align:left;vertical-align:top}
.si-status{font-size:11.5px;margin-left:6px}
.si-body{display:grid;grid-template-columns:max-content 1fr;gap:6px 14px;font-size:13px}
.si-row{display:contents}
.si-key{color:var(--muted);font-weight:600;white-space:nowrap}
.si-val{line-height:1.6}
.si-has{font-size:11px;margin-left:6px}
.si-tables{margin-top:14px;padding-top:10px;border-top:1px dashed var(--line)}
.th-plain{display:block;font-weight:400;font-size:10px;color:var(--faint);white-space:nowrap}
.glossary{margin:10px 0 14px}
.glossary-list{columns:3;column-gap:24px;margin:0;padding-left:18px;font-size:12.5px;line-height:1.6}
.glossary-list li{break-inside:avoid}
@media (max-width:900px){.glossary-list{columns:1}}
.si-tables .grp h3{font-size:13.5px}
@media (max-width:600px){.si-body{grid-template-columns:1fr}.si-key{margin-top:6px}}

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
    <span class="brand"><span class="dot"></span>Quant <span class="sub">@@BRAND@@</span></span>
    <span class="spacer"></span>
    <button class="tzbtn" id="tz" aria-label="테마 전환"><span id="tzi">◐</span><span id="tzt">테마</span></button>
  </div>
</div>

<header class="head wrap">
  <h1>@@TITLE@@ <span class="v">@@SCHEMAS@@</span></h1>
  <div class="gen">생성 @@GEN@@ · 데이터 계약(정규화 스키마)만 렌더 · 계열·비교군 분리</div>
  <div class="overview">@@OVERVIEW@@</div>
  @@LEGEND@@
</header>

<div class="wrap">
<div class="tabs" role="tablist">
@@TABS@@
</div>

@@PANELS@@
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
  // 스터디 번호 링크(색인 표) → 그 카드를 펼친다
  [].slice.call(document.querySelectorAll('a[data-study]')).forEach(function(a){
    a.addEventListener('click',function(e){
      e.preventDefault();sel('tab-si');
      var card=document.getElementById('study-'+a.getAttribute('data-study'));
      if(card){card.open=true;card.scrollIntoView({behavior:'smooth',block:'start'});}
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
  --benchmark:#2A2113; --benchmark-line:#4A3D1E; --flip:#E8A33D; --track:#1B222B; --zebra:#131920;
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
    premarket = load_premarket()
    rounds = load_rounds()
    study_index = load_study_index()
    if not rows:
        print("! metrics.json을 찾지 못했습니다.", file=sys.stderr)
    for variant, variant_specification in VARIANTS.items():
        page = render(rows, live, reviews, premarket, rounds, study_index, variant)
        if variant == "public":
            hits = [pattern.pattern for pattern in _PUBLIC_FORBIDDEN if pattern.search(page)]
            if hits:
                print(f"! 공개본에 금지 패턴 {hits} — {variant_specification['out'].name}을 쓰지 않는다", file=sys.stderr)
                continue
        variant_specification["out"].write_text(page, encoding="utf-8")
        print(f"✅ 대시보드 생성({variant}): {variant_specification['out']}")
    fams = {}
    for r in rows:
        f = r.get("family", "A_portfolio")
        fams[f] = fams.get(f, 0) + 1
    print(f"   스터디 {len(study_index)} · 백테스트 {len(rows)}행 · 계열 {dict(fams)} · "
          f"라이브 일지 {len(live.get('journals',[]))}·주문 {len(live.get('order_log',[]))} · "
          f"리뷰 {len(reviews)} · 장전 브리핑 {len(premarket)} · 리서치 라운드 {len(rounds)}")


if __name__ == "__main__":
    main()
