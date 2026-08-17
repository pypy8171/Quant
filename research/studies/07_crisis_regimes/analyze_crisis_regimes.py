#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
위기 레짐 지수레벨 특성화 (엔진 무관, 오프라인).

목적: 역사적 위기 이벤트를 지수·매크로 시계열만으로 기술통계 특성화한다.
      매매를 하지 않으며(순수 기술통계), peak/trough 앵커는 사후참조(hindsight) 라벨이다.

절대 경계(스펙):
 1. 엔진(PYQuant/backtest/engine.py)·yfinance_source.py 절대 미수정. 데이터는
    PYQuant/data/index_source.py 의 IndexSource().get_historical_ohlcv 만 사용.
 2. 종목레벨 breadth/dispersion 금지(생존편향). 지수·매크로만.
 3. FIFO 원장 재사용 금지 — 이 파일은 독립 분석 레이어.
 4. peak/trough 앵커 = hindsight 라벨. 신호 타이밍 스킬 주장 불가.
 5. n<3 셀은 verdict = "표본부족 — 일반화 금지" 고정. README 상단 배너 하드코딩.

단독 실행:
    python research/studies/07_crisis_regimes/analyze_crisis_regimes.py
콘솔 요약 + summary.tsv + README.md 생성. 네트워크 실패 티커는 NA로 스킵.
"""
import os
import sys
import math
import calendar
from pathlib import Path
from datetime import date as _date, timedelta

# ── PYQuant 를 sys.path 에 넣어 IndexSource import (IndexSource 가 kis.client.Bar 를 import) ──
_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]                 # .../Quant
_PYQ = _REPO / "PYQuant"
if str(_PYQ) not in sys.path:
    sys.path.insert(0, str(_PYQ))

from data.index_source import IndexSource  # noqa: E402

OUT_DIR = _HERE.parent
TSV_PATH = OUT_DIR / "summary.tsv"
README_PATH = OUT_DIR / "README.md"

# ── 라벨링 상수 (임계는 여기서만) ─────────────────────────────────────────────
V_SHAPE_MULT = 1.5      # recovery_days <= fall_days*1.5  → V
U_SHAPE_CAP = 252       # recovery_days <= 252            → U (초과/미회복 → L)
FAST_DAYS = 30          # fall_days <= 30                 → fast
RECOVERY_CAP = 252      # 미회복 시 recovery_days 캡
RVOL_PRE_LOOKBACK = 60  # peak 직전 거래일수
RVOL_WIN = 20           # 실현변동성 롤링 창
VIX_SYSTEMIC = 40.0     # vix_peak >= 40 → systemic
KRW_SYSTEMIC = 10.0     # krw_chg(%) >= 10 (원화 급락) → systemic

ANNUAL = math.sqrt(252.0)
RECOVERY_EXT_DAYS = 500  # 앵커창 이후 recovery 탐색용 달력일 여유(peak/trough 앵커는 창 내 고정)

# ── 이벤트 카탈로그 (id, 원인, 대표지수, search_start_ym, search_end_ym, 보조지표) ──
# 대표지수 = 앵커계산 기준. search창 = 앵커 탐색범위(실제 peak/trough 는 데이터로 결정).
EVENTS = [
    ("1929_great_crash",       "financial",      "^GSPC", "1929-06", "1933-06", []),
    ("1987_black_monday",      "structural",     "^GSPC", "1987-07", "1988-06", ["^IXIC", "^TNX"]),
    ("1990_gulf_war",          "war",            "^GSPC", "1990-06", "1991-06", ["^IXIC", "^TNX"]),
    ("1997_imf_asian",         "financial",      "^KS11", "1997-06", "1999-06", ["^GSPC"]),
    ("1998_ltcm_russia",       "financial",      "^GSPC", "1998-06", "1999-01", ["^KS11"]),
    ("2000_dotcom",            "financial",      "^IXIC", "2000-01", "2003-06", ["^GSPC", "^KQ11", "^VIX"]),
    ("2001_911",               "terror",         "^GSPC", "2001-07", "2002-01", ["^VIX", "^KS11"]),
    ("2003_sars_iraq",         "pandemic+war",   "^GSPC", "2002-12", "2003-09", ["^KS11"]),
    ("2008_gfc",               "financial",      "^GSPC", "2007-08", "2010-06", ["^KS11", "^VIX", "^TNX", "KRW=X"]),
    ("2010_flash_euro",        "financial",      "^GSPC", "2010-03", "2010-12", ["^VIX", "^KS11"]),
    ("2011_us_downgrade_euro", "financial",      "^GSPC", "2011-05", "2012-06", ["^KS11", "^VIX", "KRW=X"]),
    ("2015_china_yuan",        "financial",      "^GSPC", "2015-06", "2016-04", ["^KS11", "^VIX"]),
    ("2018_fed_q4",            "inflation",      "^GSPC", "2018-08", "2019-06", ["^KS11", "^VIX", "KRW=X"]),
    ("2020_covid",             "pandemic",       "^GSPC", "2020-01", "2021-01", ["^KS11", "^VIX", "^TNX", "KRW=X"]),
    ("2022_inflation_bear",    "inflation",      "^GSPC", "2021-11", "2023-06", ["^KS11", "^IXIC", "^VIX", "^TNX", "KRW=X"]),
    ("2023_svb",               "financial",      "^GSPC", "2023-02", "2023-08", ["^VIX", "^KS11"]),
    ("2024_yen_carry",         "structural",     "^GSPC", "2024-07", "2024-12", ["^KS11", "^VIX"]),
]

# 데이터 하한(스펙) — 커버리지 판정 참고용
DATA_FLOOR = {
    "^GSPC": "1927", "^IXIC": "1971", "^DJI": "1992", "^KS11": "1996-12",
    "^KQ11": "2000-10", "^VIX": "1990", "^TNX": "1962", "GC=F": "2000",
    "CL=F": "2000", "DX-Y.NYB": "1971", "KRW=X": "2003-12", "^SP500TR": "1988",
}

BANNER = (
    "> ⚠️ 이 표는 **엣지 발견이 아니라 시나리오 스트레스테스트**다. 위기 표본 n≈17, "
    "거동셀당 1~3개 → 셀단위 전략우열 비교는 통계적으로 무의미. 지수레벨 기술통계만이며 "
    "개별종목·수급·survivorship 미반영. peak/trough 앵커는 hindsight 라벨(신호타이밍 스킬 "
    "주장 불가). 데이터: yfinance 무키, 대표지수 커버리지는 커버리지표 참조."
)

NA = "NA"
INSUFFICIENT = "표본부족 — 일반화 금지"


# ── 유틸 ─────────────────────────────────────────────────────────────────────
def ym_to_start(ym: str) -> str:
    return f"{ym}-01"


def ym_to_end(ym: str) -> str:
    y, m = (int(x) for x in ym.split("-"))
    last = calendar.monthrange(y, m)[1]
    return f"{y:04d}-{m:02d}-{last:02d}"


def add_days(iso: str, days: int) -> str:
    return (_date.fromisoformat(iso) + timedelta(days=days)).isoformat()


def fmt(x, nd=2):
    if x is None:
        return NA
    if isinstance(x, float):
        if math.isnan(x):
            return NA
        return f"{x:.{nd}f}"
    return str(x)


def realized_vol_series(closes):
    """일간로그수익률 롤링 RVOL_WIN 표준편차*sqrt(252)*100. 인덱스 정렬은 closes 와 동일(창 미만이면 None)."""
    n = len(closes)
    rets = [None] * n
    for i in range(1, n):
        if closes[i - 1] > 0 and closes[i] > 0:
            rets[i] = math.log(closes[i] / closes[i - 1])
    out = [None] * n
    for i in range(n):
        seg = [r for r in rets[max(0, i - RVOL_WIN + 1): i + 1] if r is not None]
        if len(seg) >= max(5, RVOL_WIN // 2):
            mean = sum(seg) / len(seg)
            var = sum((r - mean) ** 2 for r in seg) / (len(seg) - 1)
            out[i] = math.sqrt(var) * ANNUAL * 100.0
    return out


def asof(bars, target_date):
    """target_date(포함) 이하 마지막 종가. 없으면 None."""
    val = None
    for b in bars:
        if b.date <= target_date:
            val = b.close
        else:
            break
    return val


def max_close_in_range(bars, d0, d1):
    vals = [b.close for b in bars if d0 <= b.date <= d1]
    return max(vals) if vals else None


# ── 월별 움직임(이벤트 전후 3개월) ────────────────────────────────────────────
def _shift_ym(y, m, k):
    """(y,m) 에서 k개월 이동(k 음수 허용) → (y,m)."""
    idx = y * 12 + (m - 1) + k
    return idx // 12, idx % 12 + 1


def month_end_closes(bars):
    """{(y,m): 그 달의 마지막 종가}. bars 오름차순 전제(마지막 값이 월말)."""
    out = {}
    for b in bars:
        out[(int(b.date[:4]), int(b.date[5:7]))] = b.close
    return out


def monthly_return(mec, y, m):
    """(y,m) 월간수익률% = 월말종가/전월말종가 − 1. 없으면 None."""
    cur = mec.get((y, m))
    prev = mec.get(_shift_ym(y, m, -1))
    if cur is None or prev is None or prev <= 0:
        return None
    return (cur / prev - 1.0) * 100.0


def monthly_context(src, rep, peak_date, trough_date, coverage):
    """대표지수 월간등락: 고점 직전 3개월(전−3,−2,−1) + 저점 직후 3개월(후+1,+2,+3).
    월말종가 기준. 데이터 하한/결측은 None. peak_ym/trough_ym 문자열도 반환."""
    py, pm = int(peak_date[:4]), int(peak_date[5:7])
    ty, tm = int(trough_date[:4]), int(trough_date[5:7])
    dl_s = add_days(peak_date, -170)     # 전−3월의 전월 종가까지 확보하려 여유
    dl_e = add_days(trough_date, 170)
    today = _date.today().isoformat()
    if dl_e > today:
        dl_e = today
    bars = src.get_historical_ohlcv(rep, dl_s, dl_e)
    _record_cov(coverage, rep, bars)
    mec = month_end_closes(bars)
    pre = [monthly_return(mec, *_shift_ym(py, pm, k)) for k in (-3, -2, -1)]
    post = [monthly_return(mec, *_shift_ym(ty, tm, k)) for k in (1, 2, 3)]
    return pre, post, f"{py:04d}-{pm:02d}", f"{ty:04d}-{tm:02d}"


# ── 이벤트 분석 ───────────────────────────────────────────────────────────────
def analyze_event(src, ev, coverage):
    eid, cause, rep, s_ym, e_ym, aux = ev
    s_start = ym_to_start(s_ym)
    s_end = ym_to_end(e_ym)
    # recovery 는 앵커창 이후까지 볼 수 있게 다운로드 상한을 여유있게(앵커는 창 내 고정).
    dl_end = add_days(s_end, RECOVERY_EXT_DAYS)
    today = _date.today().isoformat()
    if dl_end > today:
        dl_end = today

    bars = src.get_historical_ohlcv(rep, s_start, dl_end)
    _record_cov(coverage, rep, bars)

    row = {
        "id": eid, "cause": cause, "rep_index": rep,
        "peak_date": NA, "peak_close": None, "trough_date": NA, "trough_close": None,
        "max_dd": None, "fall_days": None, "recovery_days": None, "recovered": NA,
        "shape": NA, "speed": NA, "rvol_pre": None, "rvol_peak": None,
        "vix_peak": None, "tnx_chg": None, "krw_chg": None, "systemic_tag": NA,
        "pre_months": [None, None, None], "post_months": [None, None, None],
        "peak_ym": NA, "trough_ym": NA,
    }
    if not bars:
        row["note"] = "대표지수 결측 — 분석불가"
        return row

    # 앵커 탐색은 [s_start, s_end] 내에서만.
    anchor = [i for i, b in enumerate(bars) if b.date <= s_end]
    if not anchor:
        row["note"] = "앵커창 내 데이터 없음"
        return row
    a0, a1 = anchor[0], anchor[-1]

    # ── 앵커 = 표준 최대낙폭(peak→trough 하락폭이 최대인 쌍). ──
    # 스펙의 "창 내 종가 최댓값 첫도달=peak" 단순화는 회복랠리가 직전고점을 넘는
    # 장기창(예: 1997 IMF, 2020 코로나)에서 peak 가 창 후반 회복고점에 꽂혀 폭락을
    # 통째로 놓친다(dd≈0 아티팩트). 위기 특성화의 취지는 '가장 깊은 하락'이므로
    # 표준 max-drawdown 알고리즘으로 대체(결정론적, peak<=trough 보장). 편차는 README 명기.
    run_max_i = a0
    trough_i = a0
    peak_i = a0
    worst_dd = 0.0
    for i in range(a0, a1 + 1):
        if bars[i].close > bars[run_max_i].close:
            run_max_i = i
        dd = bars[i].close / bars[run_max_i].close - 1.0
        if dd < worst_dd:
            worst_dd = dd
            trough_i = i
            peak_i = run_max_i

    peak_c, trough_c = bars[peak_i].close, bars[trough_i].close
    row["peak_date"] = bars[peak_i].date
    row["peak_close"] = peak_c
    row["trough_date"] = bars[trough_i].date
    row["trough_close"] = trough_c
    row["max_dd"] = (trough_c / peak_c - 1.0) * 100.0
    fall_days = trough_i - peak_i
    row["fall_days"] = fall_days

    # recovery: trough 이후(다운로드 전체 구간) 종가가 peak_close 회복한 첫날. 캡 252.
    rec_days = None
    recovered = False
    for j in range(trough_i + 1, len(bars)):
        if bars[j].close >= peak_c:
            rec_days = j - trough_i
            recovered = True
            break
    if rec_days is None or rec_days > RECOVERY_CAP:
        recovered = recovered and rec_days is not None and rec_days <= RECOVERY_CAP
        rec_days_capped = RECOVERY_CAP
    else:
        rec_days_capped = rec_days
    row["recovered"] = "True" if recovered else "False"
    row["recovery_days"] = rec_days_capped

    # shape
    if recovered and rec_days is not None and rec_days <= fall_days * V_SHAPE_MULT:
        shape = "V"
    elif recovered and rec_days is not None and rec_days <= U_SHAPE_CAP:
        shape = "U"
    else:
        shape = "L"
    row["shape"] = shape
    row["speed"] = "fast" if fall_days <= FAST_DAYS else "slow"

    # rvol_pre: peak 직전 60거래일 일간수익률 std*sqrt(252)*100
    pre0 = max(0, peak_i - RVOL_PRE_LOOKBACK)
    pre_closes = [bars[k].close for k in range(pre0, peak_i + 1)]
    if len(pre_closes) >= 20:
        rets = [math.log(pre_closes[k] / pre_closes[k - 1])
                for k in range(1, len(pre_closes)) if pre_closes[k - 1] > 0]
        if len(rets) >= 2:
            mean = sum(rets) / len(rets)
            var = sum((r - mean) ** 2 for r in rets) / (len(rets) - 1)
            row["rvol_pre"] = math.sqrt(var) * ANNUAL * 100.0

    # rvol_peak: shock구간[peak..trough] 20일 실현변동성 최댓값
    rv = realized_vol_series([b.close for b in bars])
    seg = [rv[k] for k in range(peak_i, trough_i + 1) if rv[k] is not None]
    if seg:
        row["rvol_peak"] = max(seg)

    # ── 보조지표 ──
    peak_d, trough_d = bars[peak_i].date, bars[trough_i].date

    # 이벤트 전후 월별 움직임(대표지수)
    pre_m, post_m, p_ym, t_ym = monthly_context(src, rep, peak_d, trough_d, coverage)
    row["pre_months"], row["post_months"] = pre_m, post_m
    row["peak_ym"], row["trough_ym"] = p_ym, t_ym

    if "^VIX" in aux:
        vbars = src.get_historical_ohlcv("^VIX", s_start, s_end)
        _record_cov(coverage, "^VIX", vbars)
        vmax = max_close_in_range(vbars, peak_d, trough_d)
        if vmax is not None:
            row["vix_peak"] = vmax
    if "^TNX" in aux:
        tbars = src.get_historical_ohlcv("^TNX", s_start, s_end)
        _record_cov(coverage, "^TNX", tbars)
        tp, tt = asof(tbars, peak_d), asof(tbars, trough_d)
        if tp is not None and tt is not None:
            row["tnx_chg"] = tt - tp   # 금리 %p 변화
    if "KRW=X" in aux:
        kbars = src.get_historical_ohlcv("KRW=X", s_start, s_end)
        _record_cov(coverage, "KRW=X", kbars)
        kp, kt = asof(kbars, peak_d), asof(kbars, trough_d)
        if kp is not None and kt is not None and kp > 0:
            row["krw_chg"] = (kt / kp - 1.0) * 100.0   # +면 원화 약세

    # systemic 태그(휴리스틱, 라벨만)
    sysm = False
    if row["vix_peak"] is not None and row["vix_peak"] >= VIX_SYSTEMIC:
        sysm = True
    if row["krw_chg"] is not None and row["krw_chg"] >= KRW_SYSTEMIC:
        sysm = True
    row["systemic_tag"] = "systemic" if sysm else "narrow/external"
    return row


def _record_cov(coverage, ticker, bars):
    c = coverage.setdefault(ticker, {"first": None, "last": None, "nbars": 0, "hits": 0, "empties": 0})
    if bars:
        c["hits"] += 1
        c["nbars"] += len(bars)
        f, l = bars[0].date, bars[-1].date
        if c["first"] is None or f < c["first"]:
            c["first"] = f
        if c["last"] is None or l > c["last"]:
            c["last"] = l
    else:
        c["empties"] += 1


# ── 출력 ─────────────────────────────────────────────────────────────────────
TSV_COLS = ["id", "cause", "rep_index", "peak_date", "peak_close", "trough_date",
            "trough_close", "max_dd", "fall_days", "recovery_days", "recovered",
            "shape", "speed", "rvol_pre", "rvol_peak", "vix_peak", "tnx_chg",
            "krw_chg", "systemic_tag"]


def write_tsv(rows):
    lines = ["\t".join(TSV_COLS)]
    for r in rows:
        cells = []
        for c in TSV_COLS:
            v = r.get(c)
            if c in ("peak_close", "trough_close"):
                cells.append(fmt(v, 2))
            elif c in ("max_dd", "rvol_pre", "rvol_peak", "vix_peak", "tnx_chg", "krw_chg"):
                cells.append(fmt(v, 2))
            elif v is None:
                cells.append(NA)
            else:
                cells.append(str(v))
        lines.append("\t".join(cells))
    TSV_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_grid(rows):
    grid = {}   # (speed, shape) -> [ids]
    for r in rows:
        sp, sh = r.get("speed"), r.get("shape")
        if sp in ("fast", "slow") and sh in ("V", "U", "L"):
            grid.setdefault((sp, sh), []).append(r["id"])
    return grid


def write_readme(rows, grid, coverage):
    L = []
    L.append("# 07 · 위기 레짐 지수레벨 특성화\n")
    L.append(BANNER + "\n")
    L.append("_생성: `analyze_crisis_regimes.py` (엔진 무관 단독실행). 재실행 시 parquet 캐시로 결정론적 재현._\n")

    # 거동 그리드
    L.append("## 거동 그리드 — speed(fast/slow) × shape(V/U/L)\n")
    L.append("각 셀 = 해당 이벤트 id + n. **n<3 → 일반화 금지.** 원인태그는 부(副) 정보(개별 이벤트 표 참조).\n")
    L.append("| speed \\ shape | V | U | L |")
    L.append("|---|---|---|---|")
    for sp in ("fast", "slow"):
        cells = [f"**{sp}**"]
        for sh in ("V", "U", "L"):
            ids = grid.get((sp, sh), [])
            if not ids:
                cells.append("_(없음)_")
            else:
                body = "<br>".join(ids) + f"<br>**n={len(ids)}**"
                if len(ids) < 3:
                    body += f"<br>_{INSUFFICIENT}_"
                cells.append(body)
        L.append("| " + " | ".join(cells) + " |")
    L.append("")

    # verdict 요약
    L.append("### 셀 verdict\n")
    L.append("| 셀 | n | verdict |")
    L.append("|---|---|---|")
    for sp in ("fast", "slow"):
        for sh in ("V", "U", "L"):
            n = len(grid.get((sp, sh), []))
            verdict = INSUFFICIENT if n < 3 else "표본 3+ (그래도 지수레벨 기술통계·hindsight 라벨)"
            L.append(f"| {sp}·{sh} | {n} | {verdict} |")
    L.append("")

    # 개별 이벤트 표 — 17열은 너무 넓어 가독성이 무너짐. id로 잇는 2개 표로 분할.
    L.append("## 개별 이벤트 (지수레벨 기술통계)\n")
    L.append("표가 넓어 **① 낙폭·회복 타이밍**과 **② 변동성·매크로**로 나눴다(맨 왼쪽 id로 연결). "
             "각 열의 뜻은 두 표 아래 **지표 설명** 참조.\n")

    L.append("### ① 낙폭·회복 타이밍\n")
    hdr_a = ["id(이벤트)", "원인", "대표(지수)", "peak(고점일)", "trough(저점일)",
             "max_dd%(최대낙폭)", "fall_d(하락일)", "rec_d(회복일)", "shape(형태)", "speed(속도)"]
    L.append("| " + " | ".join(hdr_a) + " |")
    L.append("|" + "---|" * len(hdr_a))
    for r in rows:
        L.append("| " + " | ".join([
            r["id"], r["cause"], r["rep_index"], r["peak_date"], r["trough_date"],
            fmt(r["max_dd"], 1), fmt(r["fall_days"], 0), fmt(r["recovery_days"], 0),
            r["shape"], r["speed"],
        ]) + " |")
    L.append("")

    L.append("### ② 변동성·매크로·시스템성\n")
    hdr_b = ["id(이벤트)", "recov(회복)", "rvol_pre(사전변동성)", "rvol_peak(충격변동성)",
             "vix_pk(VIX정점)", "tnx_Δ(금리Δ%p)", "krw_Δ%(원화Δ)", "tag(시스템성)"]
    L.append("| " + " | ".join(hdr_b) + " |")
    L.append("|" + "---|" * len(hdr_b))
    for r in rows:
        L.append("| " + " | ".join([
            r["id"], r["recovered"], fmt(r["rvol_pre"], 1), fmt(r["rvol_peak"], 1),
            fmt(r["vix_peak"], 1), fmt(r["tnx_chg"], 2), fmt(r["krw_chg"], 1),
            r["systemic_tag"],
        ]) + " |")
    L.append("")
    L.append("### 지표 설명 (한글 — 어떤 지표이고 값이 무슨 뜻인지)\n")
    L.append(
        "- **peak(고점일) / trough(저점일)**: 최대낙폭 구간의 시작(직전 고점)·끝(저점) 날짜.\n"
        "- **max_dd%(최대낙폭)**: 고점 대비 저점까지 최대 하락률(%). 음수가 클수록 깊은 폭락(예: −86.2 = 86% 폭락).\n"
        "- **fall_d(하락일수)**: 고점→저점 거래일수. 작을수록 급속 붕괴(예: 2020 코로나 23일).\n"
        "- **rec_d(회복일수)**: 저점→전고점종가 회복 거래일수. 미회복/>252 는 252 캡 + `recov=False`.\n"
        "- **recov(회복여부)**: 창+여유(500일) 내 전고점 회복 여부(True/False).\n"
        "- **shape(회복형태)**: V(rec≤fall×1.5, 급반등) / U(rec≤252, 완만회복) / L(미회복·장기, 영구타격).\n"
        "- **speed(하락속도)**: fast(fall≤30거래일) / slow(그외).\n"
        "- **rvol_pre(사전 실현변동성)**: 고점 직전 60거래일 연율화 변동성(%). 위기 전 시장이 얼마나 조용했나.\n"
        "- **rvol_peak(충격 실현변동성)**: 하락구간 20일 실현변동성 최댓값(%). 붕괴의 격렬함.\n"
        "- **vix_pk(VIX 정점)**: 하락구간 공포지수 최댓값(있을 때). ≥40이면 공황 수준(예: 2020 82.7).\n"
        "- **tnx_Δ(금리 변화)**: 미국채10년 금리 %p 변화(고점→저점). 음수=안전자산 쏠림(금리↓).\n"
        "- **krw_Δ%(원화 변화)**: USD/KRW % 변화(고점→저점). +면 원화 약세=자본이탈(예: 2008 +68.8%).\n"
        "- **tag(시스템성)**: systemic(vix≥40 또는 원화≥+10%) / narrow·external(국지·외생). 휴리스틱 라벨.\n"
    )

    # 이벤트 전후 월별 움직임
    L.append("## 이벤트 전후 월별 움직임 (대표지수 월간 등락률 %)\n")
    L.append("각 값 = 대표지수 **월말종가 기준 그 달의 등락률(%)**. `전−k월`=고점(peak) 직전 k번째 달, "
             "`후+k월`=저점(trough) 직후 k번째 달. 가운데 `고점→저점(최대낙폭)`이 충격 본체. "
             "정의: 월간등락률 = 월말종가/전월말종가 − 1. 데이터 하한 이전·결측은 NA.\n")
    mh = ["id(이벤트)", "대표", "고점월", "전−3월", "전−2월", "전−1월",
          "고점→저점(최대낙폭)", "후+1월", "후+2월", "후+3월", "저점월"]
    L.append("| " + " | ".join(mh) + " |")
    L.append("|" + "---|" * len(mh))

    def _spct(x):
        if x is None or (isinstance(x, float) and math.isnan(x)):
            return NA
        return f"{x:+.1f}"

    for r in rows:
        pre = r.get("pre_months") or [None, None, None]
        post = r.get("post_months") or [None, None, None]
        L.append("| " + " | ".join([
            r["id"], r["rep_index"], r.get("peak_ym", NA),
            _spct(pre[0]), _spct(pre[1]), _spct(pre[2]),
            fmt(r["max_dd"], 1), _spct(post[0]), _spct(post[1]), _spct(post[2]),
            r.get("trough_ym", NA),
        ]) + " |")
    L.append("")

    # 커버리지
    L.append("## 데이터 커버리지\n")
    L.append("| 티커 | 스펙하한 | 실측first | 실측last | 총bars | 비어있던요청수 |")
    L.append("|---|---|---|---|---|---|")
    for t in sorted(coverage.keys()):
        c = coverage[t]
        L.append(f"| {t} | {DATA_FLOOR.get(t, '?')} | {c['first'] or NA} | "
                 f"{c['last'] or NA} | {c['nbars']} | {c['empties']} |")
    L.append("")
    # 결측 이벤트/티커
    missing = [r["id"] for r in rows if r["peak_close"] is None]
    if missing:
        L.append(f"**대표지수 결측 이벤트**: {', '.join(missing)}\n")
    L.append("_결측(NA) 셀은 해당 티커가 그 이벤트 구간에서 데이터가 없거나(하한 이전) "
             "네트워크로 못 받은 경우. 죽지 않고 NA 처리._\n")

    # 방법·경계
    L.append("## 방법·경계 (정직성)\n")
    L.append("- **앵커 = 표준 최대낙폭(max-drawdown)**: search창 내에서 peak→trough 하락폭이 최대인 쌍. "
             "스펙 원문의 '창 내 종가 최댓값 첫도달=peak' 단순화는 회복랠리가 직전고점을 넘는 장기창"
             "(1997 IMF·2020 코로나 등)에서 peak 가 창 후반 회복고점에 꽂혀 폭락을 통째로 놓치는 "
             "dd≈0 아티팩트를 만들어 **의도적으로 대체**함(정직한 라벨 우선). 결정론적·peak≤trough 보장.\n")
    L.append("- peak/trough 는 **search창 내에서만** 앵커(사후참조 hindsight). recovery 만 창 이후 "
             f"최대 {RECOVERY_EXT_DAYS}일 여유 다운로드로 탐색(앵커는 이동 안 함).\n")
    L.append("- 종목레벨 breadth/dispersion·수급·survivorship **미반영**(생존편향 방지 위해 의도적 배제).\n")
    L.append("- FIFO 원장 재사용 안 함 — 매매 왕복 없는 지수곡선 전용 독립 레이어.\n")
    L.append("- 짧은 search창(예: 2008 GFC end=2010-06)에서 지수 완전회복이 창+여유 밖이면 L 로 라벨될 수 있음 — "
             "이는 **회복 미탐지**이지 영구 L 이 아님(캡 아티팩트). 개별 판단 시 주의.\n")

    README_PATH.write_text("\n".join(L), encoding="utf-8")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    src = IndexSource()
    coverage = {}
    rows = []
    print(f"[crisis] 이벤트 {len(EVENTS)}건 분석 시작 (캐시: {src._cache})")
    for ev in EVENTS:
        try:
            r = analyze_event(src, ev, coverage)
        except Exception as e:
            print(f"[crisis] {ev[0]} 실패: {type(e).__name__}: {e}")
            r = {"id": ev[0], "cause": ev[1], "rep_index": ev[2],
                 "peak_date": NA, "peak_close": None, "trough_date": NA,
                 "trough_close": None, "max_dd": None, "fall_days": None,
                 "recovery_days": None, "recovered": NA, "shape": NA, "speed": NA,
                 "rvol_pre": None, "rvol_peak": None, "vix_peak": None,
                 "tnx_chg": None, "krw_chg": None, "systemic_tag": NA}
        rows.append(r)
        print(f"  {r['id']:24s} dd={fmt(r['max_dd'],1):>7} fall={fmt(r['fall_days'],0):>4} "
              f"rec={fmt(r['recovery_days'],0):>4} {r['shape']}/{r['speed']} "
              f"vix={fmt(r['vix_peak'],1):>6} tag={r['systemic_tag']}")

    grid = build_grid(rows)
    write_tsv(rows)
    write_readme(rows, grid, coverage)

    print(f"\n[crisis] 거동 그리드:")
    for sp in ("fast", "slow"):
        for sh in ("V", "U", "L"):
            ids = grid.get((sp, sh), [])
            print(f"  {sp:4s}·{sh}: n={len(ids)}  {ids}")
    print(f"\n[crisis] 완료 → {TSV_PATH}\n              → {README_PATH}")


if __name__ == "__main__":
    main()
