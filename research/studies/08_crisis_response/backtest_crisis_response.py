#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
위기 인과적 대응 백테스트 (엔진 무관, 오프라인).

목적: "이벤트를 모르는 상태에서(hindsight 금지) t-1 종가까지의 정보만으로 대응했다면
      손실을 얼마나 덜 봤을까"를 지수레벨 익스포저 토글로 인과 검증한다.
      strategist↔reviewer 논의(2026-08-16) 후 확정한 5개 대응법 + 기준선(Buy&Hold) + 조합.

절대 경계(BT-07 계승):
 1. 엔진(PYQuant/backtest/engine.py)·yfinance_source.py 절대 미수정.
    데이터는 PYQuant/data/index_source.py 의 IndexSource().get_historical_ohlcv 만 사용.
 2. 지수·매크로 시계열만(종목레벨 breadth/dispersion 금지 — 생존편향).
 3. 룩어헤드 차단: 익스포저 e[t]는 오직 close t 까지의 정보로 산출, 수익은 e[t-1]*ret[t].
    peak/trough 앵커는 이벤트별 진단(회복참여율 등) *평가에만* 쓰고 신호엔 절대 안 넣음.
 4. 정직성: 1차 지표 = 단일 연결곡선 Calmar/Sharpe 1개(검정 1회). 이벤트별은 진단·중앙값만.
    임계값은 사전등록 + 전체 스윕 공개(best 셀 보고 금지). 비용 0.21/0.5/1.0% 감도.

단독 실행:
    python research/studies/08_crisis_response/backtest_crisis_response.py
콘솔 요약 + summary_*.tsv + events_*.tsv + sweep.tsv + README.md 생성.
"""
import os
import sys
import math
from pathlib import Path
from datetime import date as _date, timedelta

import numpy as np

# ── PYQuant 를 sys.path 에 (IndexSource → kis.client.Bar import) ──
_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]                 # .../Quant
_PYQ = _REPO / "PYQuant"
if str(_PYQ) not in sys.path:
    sys.path.insert(0, str(_PYQ))

from data.index_source import IndexSource  # noqa: E402
from data.asof import as_of  # noqa: E402  (재현성: 벤치마크 종료일 상한 고정)

OUT_DIR = _HERE.parent
README_PATH = OUT_DIR / "README.md"

# ── 사전등록 임계값(기본값). 스윕은 SWEEP 그리드로 전량 공개 ──────────────────
COST_BASE = 0.0021          # 왕복 거래비용(기본). 감도: 0.0021 / 0.005 / 0.010
COST_GRID = [0.0021, 0.005, 0.010]
WARMUP = 252                # 워밍업(200MA + rvol 롤링). 이 구간 e=1.0(BH)
MA_LONG = 200               # M1 추세 이동평균
MA_SLOPE = 20               # M1 기울기 창(SMA_t vs SMA_{t-20})
RVOL_WIN = 20               # 실현변동성 창
RVOL_ROLL = 252             # M2 z-score 롤링창
Z_ENTER = 1.5               # M2 디리스킹 진입(z>1.5)
Z_EXIT = 0.5               # M2 재진입(z<0.5)
M2_DERISK_EXP = 0.5         # M2 축소 익스포저
VIX_OFF = 30.0              # M3 전액 컷
VIX_HALF = 25.0             # M3 반 컷
VIX_REENTER = 20.0          # M3 재진입
CB_DROP = -0.04             # M4 서킷 임계(일간수익)
CB_COOL = 5                 # M4 냉각 거래일
M5_TARGET_VOL = 0.15        # M5 목표 연율변동성
ANNUAL = math.sqrt(252.0)

# ── 이벤트 카탈로그(진단용 창). 대표지수 = 창내 max-dd 앵커 기준 ──────────────
EVENTS = [
    ("1929_great_crash",       "financial",    "^GSPC", "1929-06-01", "1933-06-30"),
    ("1987_black_monday",      "structural",   "^GSPC", "1987-07-01", "1988-06-30"),
    ("1990_gulf_war",          "war",          "^GSPC", "1990-06-01", "1991-06-30"),
    ("1997_imf_asian",         "financial",    "^KS11", "1997-06-01", "1999-06-30"),
    ("1998_ltcm_russia",       "financial",    "^GSPC", "1998-06-01", "1999-01-31"),
    ("2000_dotcom",            "financial",    "^IXIC", "2000-01-01", "2003-06-30"),
    ("2001_911",               "terror",       "^GSPC", "2001-07-01", "2002-01-31"),
    ("2003_sars_iraq",         "pandemic+war", "^GSPC", "2002-12-01", "2003-09-30"),
    ("2008_gfc",               "financial",    "^GSPC", "2007-08-01", "2010-06-30"),
    ("2010_flash_euro",        "financial",    "^GSPC", "2010-03-01", "2010-12-31"),
    ("2011_us_downgrade_euro", "financial",    "^GSPC", "2011-05-01", "2012-06-30"),
    ("2015_china_yuan",        "financial",    "^GSPC", "2015-06-01", "2016-04-30"),
    ("2018_fed_q4",            "inflation",    "^GSPC", "2018-08-01", "2019-06-30"),
    ("2020_covid",             "pandemic",     "^GSPC", "2020-01-01", "2021-01-31"),
    ("2022_inflation_bear",    "inflation",    "^GSPC", "2021-11-01", "2023-06-30"),
    ("2023_svb",               "financial",    "^GSPC", "2023-02-01", "2023-08-31"),
    ("2024_yen_carry",         "structural",   "^GSPC", "2024-07-01", "2024-12-31"),
]

# 거동유형(BT-07 결과) — 이벤트별 판정 문맥용
BEHAVIOR = {
    "2020_covid": "fast·U", "2023_svb": "fast·U", "2024_yen_carry": "fast·U",
    "1987_black_monday": "fast/slow·V", "2018_fed_q4": "slow·V", "2011_us_downgrade_euro": "slow·V",
    "2010_flash_euro": "slow·V", "1998_ltcm_russia": "slow·V", "2003_sars_iraq": "slow·V",
    "1929_great_crash": "slow·L", "2000_dotcom": "slow·L", "2008_gfc": "slow·L",
    "2022_inflation_bear": "slow·L", "1997_imf_asian": "slow·L", "2015_china_yuan": "slow·U",
    "1990_gulf_war": "slow·U", "2001_911": "slow·V",
}


# ════════════════════════════════════════════════════════════════════════════
# 데이터
# ════════════════════════════════════════════════════════════════════════════
def load_series(src, ticker, start, end):
    """연속 일봉 → (dates:list[str], closes:np.ndarray). 중복일 제거·오름차순."""
    bars = src.get_historical_ohlcv(ticker, start, end)
    seen, dates, closes = set(), [], []
    for b in bars:
        if b.date in seen or b.close <= 0:
            continue
        seen.add(b.date)
        dates.append(b.date)
        closes.append(float(b.close))
    return dates, np.asarray(closes, dtype=float)


def align_to(dates_ref, dates_src, vals_src):
    """dates_ref 각 날짜에 대해 dates_src(<=해당일 최신) 값을 채움(forward-fill).
    없으면 np.nan. VIX 를 벤치마크 거래일에 정렬할 때 사용(룩어헤드 없음: <=오늘 종가)."""
    m = dict(zip(dates_src, vals_src))
    out = np.full(len(dates_ref), np.nan)
    last = np.nan
    # dates_src 를 정렬된 상태로 가정, ref 도 정렬. 간단히 dict + 직전값 유지.
    # forward-fill: ref 날짜가 src 에 있으면 갱신, 없으면 직전 src 값 유지.
    si = 0
    sd = dates_src
    for i, d in enumerate(dates_ref):
        while si < len(sd) and sd[si] <= d:
            last = vals_src[si]
            si += 1
        out[i] = last
    return out


# ════════════════════════════════════════════════════════════════════════════
# 지표(전부 close t 까지만 사용 — 룩어헤드 없음)
# ════════════════════════════════════════════════════════════════════════════
def sma(closes, win):
    n = len(closes)
    out = np.full(n, np.nan)
    if n >= win:
        c = np.cumsum(np.insert(closes, 0, 0.0))
        out[win - 1:] = (c[win:] - c[:-win]) / win
    return out


def daily_returns(closes):
    r = np.zeros(len(closes))
    r[1:] = closes[1:] / closes[:-1] - 1.0
    return r


def rolling_vol(ret, win):
    """close t 까지 win 일 실현변동성(연율). ret 은 일간수익."""
    n = len(ret)
    out = np.full(n, np.nan)
    for t in range(win, n):
        w = ret[t - win + 1:t + 1]
        out[t] = np.std(w, ddof=1) * ANNUAL
    return out


def rolling_z(x, win):
    n = len(x)
    out = np.full(n, np.nan)
    for t in range(n):
        lo = t - win + 1
        if lo < 0:
            continue
        w = x[lo:t + 1]
        w = w[~np.isnan(w)]
        if len(w) < win // 2:
            continue
        mu, sd = np.mean(w), np.std(w, ddof=1)
        if sd > 0:
            out[t] = (x[t] - mu) / sd
    return out


# ════════════════════════════════════════════════════════════════════════════
# 대응법 5종 → 익스포저 e[t] (t 까지 정보로 결정, ret[t+1] 에 적용)
# ════════════════════════════════════════════════════════════════════════════
def expo_bh(closes, ret, vix, params):
    return np.ones(len(closes))


def expo_m1_trend(closes, ret, vix, params):
    """M1 200일선 추세게이트: C>SMA_L AND SMA_L 기울기 양전 → 1 else 0."""
    L = params.get("ma", MA_LONG)
    s = sma(closes, L)
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if t < L + MA_SLOPE or np.isnan(s[t]) or np.isnan(s[t - MA_SLOPE]):
            e[t] = 1.0  # 워밍업: 판단 불가 → BH
            continue
        up = (closes[t] > s[t]) and (s[t] > s[t - MA_SLOPE])
        e[t] = 1.0 if up else 0.0
    return e


def expo_m2_volz(closes, ret, vix, params):
    """M2 실현변동성 z-score 사전축소: z>1.5 → 0.5, z<0.5 → 1.0 (히스테리시스)."""
    rv = rolling_vol(ret, RVOL_WIN)
    z = rolling_z(rv, RVOL_ROLL)
    e = np.ones(len(closes))
    derisk = False
    for t in range(len(closes)):
        if not np.isnan(z[t]):
            if not derisk and z[t] > Z_ENTER:
                derisk = True
            elif derisk and z[t] < Z_EXIT:
                derisk = False
        e[t] = M2_DERISK_EXP if derisk else 1.0
    return e


def expo_m3_vix(closes, ret, vix, params):
    """M3 VIX 게이트(VIX 있는 구간만): >=30→0, >=25→0.5, <20→1, 그사이 유지. NA→1(중립)."""
    e = np.ones(len(closes))
    cur = 1.0
    for t in range(len(closes)):
        v = vix[t] if vix is not None else np.nan
        if np.isnan(v):
            cur = 1.0  # VIX 결측(2000년 이전·2025+) → 중립, 타 룰에 위임
        elif v >= VIX_OFF:
            cur = 0.0
        elif v >= VIX_HALF:
            cur = 0.5
        elif v < VIX_REENTER:
            cur = 1.0
        # 20~25 밴드: cur 유지(히스테리시스)
        e[t] = cur
    return e


def expo_m4_circuit(closes, ret, vix, params):
    """M4 일간 서킷브레이커(종가-종가·정직판): ret[t]<임계 → t 이후 N일 e=0.
    재진입: 냉각 경과 AND (C>SMA5 AND 최근 3일 연속 상승). *당일 몸통은 못 막음*(꼬리만)."""
    drop = params.get("drop", CB_DROP)
    cool = params.get("cool", CB_COOL)
    s5 = sma(closes, 5)
    e = np.ones(len(closes))
    cooldown = 0
    out = False
    for t in range(len(closes)):
        # close t 시점 결정(ret[t] 는 close t 에 확정)
        if ret[t] <= drop:
            out = True
            cooldown = cool
        if out:
            if cooldown > 0:
                cooldown -= 1
                e[t] = 0.0
            else:
                up3 = t >= 3 and ret[t] > 0 and ret[t - 1] > 0 and ret[t - 2] > 0
                recov = (not np.isnan(s5[t])) and closes[t] > s5[t]
                if up3 and recov:
                    out = False
                    e[t] = 1.0
                else:
                    e[t] = 0.0
        else:
            e[t] = 1.0
    return e


def expo_m5_voltarget(closes, ret, vix, params):
    """M5 변동성 타깃 사이징(연속): e = min(1, target/rvol20). 워밍업 e=1."""
    tv = params.get("tv", M5_TARGET_VOL)
    rv = rolling_vol(ret, RVOL_WIN)
    e = np.ones(len(closes))
    for t in range(len(closes)):
        if np.isnan(rv[t]) or rv[t] <= 0:
            e[t] = 1.0
        else:
            e[t] = min(1.0, tv / rv[t])
    return e


def expo_combo(closes, ret, vix, params):
    """조합 M4+M5: 연속 사이징 × 당일 컷(곱). 상관 가장 낮은 쌍."""
    e4 = expo_m4_circuit(closes, ret, vix, params)
    e5 = expo_m5_voltarget(closes, ret, vix, params)
    return e4 * e5


METHODS = [
    ("BH",    "Buy&Hold(기준선)",              expo_bh,          "상시 100% 보유"),
    ("M1",    "200일선 추세게이트",             expo_m1_trend,    "C<200MA·기울기음전 → 현금"),
    ("M2",    "실현변동성 z-score 축소",        expo_m2_volz,     "vol z>1.5 → 50% 축소"),
    ("M3",    "VIX 게이트(2000+)",             expo_m3_vix,      "VIX≥25 반컷·≥30 전액컷"),
    ("M4",    "일간 서킷브레이커(종가-종가)",   expo_m4_circuit,  "일간 -4% → 5일 현금"),
    ("M5",    "변동성 타깃 사이징",             expo_m5_voltarget,"e=min(1, 15%/실현vol)"),
    ("M4+M5", "서킷+vol타깃 조합",             expo_combo,       "연속사이징 × 당일컷"),
]


# ════════════════════════════════════════════════════════════════════════════
# 백테스트 엔진(단일 연결곡선)
# ════════════════════════════════════════════════════════════════════════════
def run_curve(closes, ret, e, cost):
    """e[t]=close t 결정 → 익일 적용. net[t]=e[t-1]*ret[t] - turnover*(cost/2).
    반환: net(일간순수익 배열, t>=1), equity(누적)."""
    n = len(closes)
    net = np.zeros(n)
    prev_pos = 1.0   # 초기 포지션
    prev2 = 1.0
    for t in range(1, n):
        pos = e[t - 1]                 # 오늘(t) 보유 익스포저 = 어제 종가 결정
        turn = abs(pos - prev2)        # 어제(t-1) 종가에 실행한 포지션 변경
        net[t] = pos * ret[t] - turn * (cost / 2.0)
        prev2 = pos
    equity = np.cumprod(1.0 + net)
    return net, equity


def max_drawdown(equity):
    peak = np.maximum.accumulate(equity)
    dd = equity / peak - 1.0
    return float(dd.min()) * 100.0


def curve_stats(net, equity):
    n = len(net)
    total = float(equity[-1]) - 1.0
    cagr = (float(equity[-1]) ** (252.0 / n) - 1.0) if n > 0 and equity[-1] > 0 else float("nan")
    mdd = max_drawdown(equity)
    sd = float(np.std(net[1:], ddof=1))
    sharpe = (float(np.mean(net[1:])) / sd * ANNUAL) if sd > 0 else float("nan")
    calmar = (cagr / abs(mdd / 100.0)) if mdd < 0 else float("nan")
    return dict(total=total * 100.0, cagr=cagr * 100.0, mdd=mdd, sharpe=sharpe, calmar=calmar)


def turnover_toggles(e):
    """익스포저 온↔오프 토글 수(휘프소 대리). |Δe|>0.5 인 변화 카운트."""
    d = np.abs(np.diff(e))
    return int(np.sum(d > 0.5))


def slice_window(dates, d0, d1):
    lo = next((i for i, d in enumerate(dates) if d >= d0), None)
    hi = next((i for i, d in enumerate(dates) if d > d1), len(dates))
    return lo, hi


def window_maxdd_anchor(closes):
    """창 내 표준 max-drawdown 앵커(peak_idx, trough_idx, dd%). hindsight — 평가전용."""
    n = len(closes)
    if n < 2:
        return 0, 0, 0.0
    peak_i = 0
    best_peak, best_tr, best_dd = 0, 0, 0.0
    for j in range(n):
        if closes[j] > closes[peak_i]:
            peak_i = j
        dd = closes[j] / closes[peak_i] - 1.0
        if dd < best_dd:
            best_dd, best_peak, best_tr = dd, peak_i, j
    return best_peak, best_tr, best_dd * 100.0


# ════════════════════════════════════════════════════════════════════════════
# 실행
# ════════════════════════════════════════════════════════════════════════════
def fmt(x, nd=1):
    if x is None or (isinstance(x, float) and (math.isnan(x) or math.isinf(x))):
        return "NA"
    return f"{x:.{nd}f}"


def build_benchmark(src, name, ticker, start, use_vix):
    dates, closes = load_series(src, ticker, start, as_of().isoformat())
    if len(closes) < WARMUP + 20:
        return None
    ret = daily_returns(closes)
    vix = None
    if use_vix:
        vd, vc = load_series(src, "^VIX", "2000-01-01", as_of().isoformat())
        vix = align_to(dates, vd, vc) if len(vc) else None
    # 갭 점검(연속성): 평균 연 거래일수
    years = (int(dates[-1][:4]) - int(dates[0][:4])) + 1
    bars_per_year = len(dates) / max(years, 1)
    return dict(name=name, ticker=ticker, dates=dates, closes=closes,
                ret=ret, vix=vix, span=f"{dates[0]}~{dates[-1]}",
                nbars=len(dates), bpy=bars_per_year)


def eval_benchmark(bm):
    """벤치마크에 대해 전 대응법 full-curve + 이벤트별 진단."""
    closes, ret, vix, dates = bm["closes"], bm["ret"], bm["vix"], bm["dates"]
    bh_net, bh_eq = run_curve(closes, ret, expo_bh(closes, ret, vix, {}), 0.0)
    bh_stats = curve_stats(bh_net, bh_eq)

    rows = []          # full-curve (비용별)
    ev_rows = []       # 이벤트별(기본비용)
    per_method_e = {}
    for code, label, fn, desc in METHODS:
        e = fn(closes, ret, vix, {})
        per_method_e[code] = e
        by_cost = {}
        for c in COST_GRID:
            net, eq = run_curve(closes, ret, e, c)
            st = curve_stats(net, eq)
            by_cost[c] = st
        base = by_cost[COST_BASE]
        # 낙폭축소%p = |BH낙폭| − |룰낙폭| = base − bh (둘 다 음수). +면 덜 빠짐(방어).
        mdd_red = base["mdd"] - bh_stats["mdd"]
        # 수익반납은 연율(CAGR)로 — 98년 누적 절대%p는 복리로 폭발해 분모 비호환.
        give = bh_stats["cagr"] - base["cagr"]             # %/yr, +면 방어대가로 수익 포기
        # 방어효율 = 낙폭축소%p / CAGR반납%p. 부호 강등 처리.
        if code == "BH":
            eff = "—"
        elif mdd_red <= 0:
            eff = "방어실패"        # 낙폭조차 못 줄임
        elif give <= 0:
            eff = "무비용방어"      # 낙폭 줄이고 수익도 유지/개선(성배)
        else:
            eff = fmt(mdd_red / give, 1)
        rows.append(dict(code=code, label=label, desc=desc, by_cost=by_cost,
                         base=base, mdd_red=mdd_red, give=give, eff=eff,
                         toggles=turnover_toggles(e)))

    # 이벤트별 진단(기본비용) — 각 창을 로컬 재계산
    for eid, cause, rep, d0, d1 in EVENTS:
        lo, hi = slice_window(dates, d0, d1)
        if lo is None or hi - lo < 10:
            continue
        wc = closes[lo:hi]
        wr = ret[lo:hi].copy()
        wr[0] = 0.0
        p_i, tr_i, bh_dd = window_maxdd_anchor(wc)
        # 회복참여율: trough → +126 거래일
        rec_hi = min(tr_i + 126, len(wc) - 1)
        row = dict(eid=eid, cause=cause, behavior=BEHAVIOR.get(eid, "?"),
                   window=f"{d0[:7]}~{d1[:7]}", bh_dd=bh_dd, methods={})
        for code, label, fn, desc in METHODS:
            if code == "BH":
                continue
            e = per_method_e[code][lo:hi]
            # 로컬 곡선(창 시작에서 리셋). 첫날 포지션=e[0].
            net = np.zeros(len(wc))
            prev2 = e[0]
            for t in range(1, len(wc)):
                pos = e[t - 1]
                net[t] = pos * wr[t] - abs(pos - prev2) * (COST_BASE / 2.0)
                prev2 = pos
            eq = np.cumprod(1.0 + net)
            m_dd = max_drawdown(eq)
            # 회복참여
            if rec_hi > tr_i:
                bh_rec = float(wc[rec_hi] / wc[tr_i] - 1.0)
                m_rec = float(np.prod(1.0 + net[tr_i + 1:rec_hi + 1]) - 1.0)
                part = (m_rec / bh_rec * 100.0) if abs(bh_rec) > 1e-9 else float("nan")
            else:
                part = float("nan")
            row["methods"][code] = dict(mdd=m_dd, mdd_red=bh_dd_minus(bh_dd, m_dd), part=part)
        ev_rows.append(row)

    return dict(bh=bh_stats, rows=rows, ev_rows=ev_rows)


def bh_dd_minus(bh_dd, m_dd):
    """낙폭축소%p = |BH낙폭| − |룰낙폭|. 둘 다 음수(%)이므로 = m_dd − bh_dd.
    예: bh_dd=-40, m_dd=-25 → +15(룰이 덜 빠짐=방어). 음수면 룰이 더 빠짐."""
    return m_dd - bh_dd


def run_sweep(bm):
    """임계값 사전등록 그리드 전량 → full-curve Calmar(기본비용). best 셀 강조 금지."""
    closes, ret, vix = bm["closes"], bm["ret"], bm["vix"]
    out = []
    # M4: drop × cool
    for drop in (-0.03, -0.04, -0.05):
        for cool in (3, 5, 10):
            e = expo_m4_circuit(closes, ret, vix, {"drop": drop, "cool": cool})
            net, eq = run_curve(closes, ret, e, COST_BASE)
            st = curve_stats(net, eq)
            out.append(("M4", f"drop={int(drop*100)}%,N={cool}", st["calmar"], st["mdd"], st["total"]))
    # M5: target vol
    for tv in (0.10, 0.15, 0.20):
        e = expo_m5_voltarget(closes, ret, vix, {"tv": tv})
        net, eq = run_curve(closes, ret, e, COST_BASE)
        st = curve_stats(net, eq)
        out.append(("M5", f"target={int(tv*100)}%", st["calmar"], st["mdd"], st["total"]))
    # M1: MA
    for ma in (100, 150, 200):
        e = expo_m1_trend(closes, ret, vix, {"ma": ma})
        net, eq = run_curve(closes, ret, e, COST_BASE)
        st = curve_stats(net, eq)
        out.append(("M1", f"MA={ma}", st["calmar"], st["mdd"], st["total"]))
    return out


def median(xs):
    xs = [x for x in xs if x is not None and not (isinstance(x, float) and math.isnan(x))]
    if not xs:
        return float("nan")
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def main():
    src = IndexSource()
    print("[load] 벤치마크 구축 중...")
    us = build_benchmark(src, "US(^GSPC)", "^GSPC", "1928-01-01", use_vix=True)
    kr = build_benchmark(src, "KR(^KS11)", "^KS11", "1996-01-01", use_vix=True)
    bms = [b for b in (us, kr) if b is not None]
    results = {}
    for bm in bms:
        print(f"[eval] {bm['name']} {bm['span']} bars={bm['nbars']} (~{bm['bpy']:.0f}/yr)")
        results[bm["name"]] = eval_benchmark(bm)
    sweeps = {bm["name"]: run_sweep(bm) for bm in bms}
    write_readme(bms, results, sweeps)
    print(f"[done] README → {README_PATH}")


def write_readme(bms, results, sweeps):
    L = []
    today = as_of().isoformat()
    L.append("# 위기 인과적 대응 백테스트 (BT-08)\n")
    L.append("> ⚠️ **정직성 배너.** 이 표는 *엣지 발견이 아니라 인과적 스트레스테스트*다. "
             "익스포저는 오직 **t-1 종가까지의 정보**로 산출(룩어헤드 차단), 수익=e[t-1]×지수수익. "
             "peak/trough 앵커는 **평가에만** 쓰고 신호엔 절대 미투입. "
             "방어대상 실질표본 n≈6~9(위기 17건, 방어가 켜지는 slow·L·systemic 소수)라 "
             "**임계값 단일 우승셀 보고 금지** — 사전등록 그리드 전량 공개. "
             "1차 지표=단일 연결곡선 Calmar/Sharpe 1개(검정 1회), 이벤트별은 진단·중앙값만. "
             "지수레벨 익스포저 토글이며 개별종목·수급·survivorship 미반영. "
             f"데이터: yfinance 무키. 생성일 {today}.\n")

    # ── 논의 요약 ──
    L.append("## 논의(strategist ↔ reviewer) 핵심 결론\n")
    L.append("- **사전 방어의 한계**: BT-07 rvol_pre·직전월수익을 보면 대부분 위기는 고점 직전까지 "
             "조용·강세다. 사전 디리스킹이 실제로 켜질 위기는 **slow·L·systemic 소수(2000·2008·2022)** 뿐이고, "
             "**fast·U(2020·2023·2024)는 폭락 전날까지 정상**이라 사전 방어가 원천 불가하다.\n")
    L.append("- **당일 방어의 원리적 한계**: 일간 -4%는 *종가에만* 확정되므로 그 급락 몸통은 "
             "e[t-1]=1로 **이미 맞은 뒤**다. 종가-종가 인과 구조에서 서킷브레이커(M4)는 "
             "**당일 몸통을 못 막고 꼬리(익일 이후)만 자른다.** '당일 대응'의 정직한 답이다.\n")
    L.append("- **양날의 검(BT-06 계승)**: 방어룰은 낙폭만 보면 다 좋아 보인다. 그래서 "
             "**낙폭 감소가 아니라 전 구간(위기+정상+회복) 순효과**로 평가한다 — 방어효율·회복참여율·휘프소.\n")

    # ── 대응법 5종 ──
    L.append("\n## 대응법 5종 + 기준선 + 조합\n")
    L.append("| 코드 | 대응법 | 트리거(후행 데이터만) | 이론상 효과/독 |")
    L.append("|---|---|---|---|")
    L.append("| M1 | 200일선 추세게이트 | C<200MA AND 200MA 기울기 음전 → 현금 | slow·L 방어 / fast·U 재진입지연으로 독 |")
    L.append("| M2 | 실현변동성 z-score 축소 | 20일 vol의 252일 z>1.5 → 50%축소 | vol 서서히 오른 slow·L / fast엔 안켜짐 |")
    L.append("| M3 | VIX 게이트(2000+) | VIX≥25 반컷·≥30 전액컷 | systemic 특화 / 동행지표라 사전방어 아님 |")
    L.append("| M4 | 일간 서킷브레이커 | 일간 -4% → 5일 현금(종가-종가) | 낙폭연장 방어 / V반등·계단하락서 톱니 |")
    L.append("| M5 | 변동성 타깃 사이징 | e=min(1, 15%/실현vol) 연속 | 전구간 완만방어·휘프소최소 / 당일급락 둔감 |")
    L.append("| M4+M5 | 조합 | 연속사이징 × 당일컷 | 상관 가장 낮은 쌍(느린 사이징+빠른 이벤트컷) |")

    # ── full-curve 결과(벤치마크별) ──
    for bm in bms:
        r = results[bm["name"]]
        bh = r["bh"]
        L.append(f"\n## 1차 지표 — 단일 연결곡선 · {bm['name']} ({bm['span']}, {bm['nbars']}봉)\n")
        L.append(f"> Buy&Hold 기준: 총수익 {fmt(bh['total'])}% · CAGR {fmt(bh['cagr'],2)}% · "
                 f"MDD {fmt(bh['mdd'])}% · Sharpe {fmt(bh['sharpe'],2)} · Calmar {fmt(bh['calmar'],2)}\n")
        L.append("| 대응법 | CAGR% | MDD% | Sharpe | **Calmar** | 낙폭축소%p | CAGR반납%p/yr | 방어효율 | 토글수 |")
        L.append("|---|---|---|---|---|---|---|---|---|")
        for row in r["rows"]:
            b = row["base"]
            L.append(f"| {row['code']} {row['label']} | {fmt(b['cagr'],2)} | {fmt(b['mdd'])} | "
                     f"{fmt(b['sharpe'],2)} | **{fmt(b['calmar'],2)}** | {fmt(row['mdd_red'])} | "
                     f"{fmt(row['give'],2)} | {row['eff']} | {row['toggles']} |")
        L.append("\n*낙폭축소%p = |BH MDD| − |룰 MDD|(+면 덜 빠짐). CAGR반납%p/yr = BH연율수익 − 룰연율수익"
                 "(+면 방어대가로 매년 포기하는 수익률; 98년 누적 절대%p는 복리로 폭발해 분모로 부적합→연율 사용). "
                 "방어효율 = 낙폭축소%p / CAGR반납%p(클수록 값어치; 낙폭↓·수익유지면 '무비용방어', "
                 "낙폭조차 못줄이면 '방어실패'). 토글수 = 익스포저 온↔오프 횟수(휘프소 대리).*\n")

        # 비용 감도
        L.append(f"\n### 거래비용 감도 · {bm['name']} (Calmar)\n")
        L.append("| 대응법 | 0.21% | 0.5% | 1.0% |")
        L.append("|---|---|---|---|")
        for row in r["rows"]:
            cs = row["by_cost"]
            L.append(f"| {row['code']} | {fmt(cs[0.0021]['calmar'],2)} | "
                     f"{fmt(cs[0.005]['calmar'],2)} | {fmt(cs[0.010]['calmar'],2)} |")

    # ── 이벤트별 진단(US) ──
    for bm in bms:
        r = results[bm["name"]]
        if not r["ev_rows"]:
            continue
        L.append(f"\n## 이벤트별 진단 · {bm['name']} (기본비용 0.21%, 진단용 — best 셀 판정 금지)\n")
        L.append("행 = 위기창 로컬 재계산. **낙폭축소%p**(+면 방어) / **회복참여%**(trough→+126일, "
                 "룰수익/BH수익, 100%=완전참여·낮을수록 반등반납). trough 앵커는 hindsight·평가전용.\n")
        codes = [c for c, *_ in METHODS if c != "BH"]
        L.append("| 이벤트 | 거동 | BH낙폭% | " + " | ".join(f"{c} 축소/참여" for c in codes) + " |")
        L.append("|---|---|---|" + "|".join(["---"] * len(codes)) + "|")
        # 중앙값 집계
        agg = {c: {"red": [], "part": []} for c in codes}
        for row in r["ev_rows"]:
            cells = []
            for c in codes:
                m = row["methods"].get(c)
                if m:
                    cells.append(f"{fmt(m['mdd_red'])}/{fmt(m['part'],0)}")
                    agg[c]["red"].append(m["mdd_red"])
                    agg[c]["part"].append(m["part"])
                else:
                    cells.append("NA")
            L.append(f"| {row['eid']} | {row['behavior']} | {fmt(row['bh_dd'])} | " + " | ".join(cells) + " |")
        L.append("| **중앙값** | — | — | " +
                 " | ".join(f"{fmt(median(agg[c]['red']))}/{fmt(median(agg[c]['part']),0)}" for c in codes) + " |")

    # ── 스윕 ──
    for bm in bms:
        sw = sweeps[bm["name"]]
        L.append(f"\n## 임계값 사전등록 스윕 · {bm['name']} (full-curve Calmar, 전량공개)\n")
        L.append("| 대응법 | 파라미터 | Calmar | MDD% | 총수익% |")
        L.append("|---|---|---|---|---|")
        for code, param, cal, mdd, tot in sw:
            L.append(f"| {code} | {param} | {fmt(cal,2)} | {fmt(mdd)} | {fmt(tot)} |")
        L.append("\n*여러 파라미터의 Calmar 분산을 그대로 노출. 특정 셀이 높다고 '엣지'가 아니라 "
                 "n≈6~9 표본의 곡선맞춤일 수 있음. 사전등록값(M4 -4%/N5, M5 15%, M1 200) 대비 개선은 "
                 "in-sample 튜닝으로 간주.*\n")

    # ── 용어 설명 ──
    L.append("\n## 용어 설명\n")
    L.append("- **익스포저(exposure)**: 위험자산 보유 비중(0=전액현금, 1=100% 보유). 대응법은 결국 이 값을 조절한다.")
    L.append("- **MDD(최대낙폭)**: 고점 대비 최대 하락률. 방어의 핵심 대상.")
    L.append("- **CAGR(연평균 복리수익률)**: 전 구간 수익을 연 단위로 환산한 값.")
    L.append("- **Sharpe(샤프)**: 변동성 1단위당 초과수익 — 위험 대비 효율.")
    L.append("- **Calmar(칼마)**: CAGR ÷ |MDD|. 낙폭 대비 수익 — 이 백테스트의 **1차 지표**.")
    L.append("- **실현변동성(rvol)**: 최근 N일 일간수익 표준편차를 연율화. 시장이 얼마나 출렁이는지.")
    L.append("- **낙폭축소%p**: |BH 낙폭| − |룰 낙폭|. +면 룰이 그만큼 덜 빠졌다는 뜻.")
    L.append("- **CAGR반납%p/yr**: 방어의 대가로 매년 포기하는 수익률(연율).")
    L.append("- **방어효율**: 낙폭축소 ÷ CAGR반납. 클수록 '포기한 수익 대비 지킨 낙폭'이 값어치 있다.")
    L.append("- **회복참여율**: 저점 이후 반등 구간에서 룰이 참여한 비율. 낮을수록 반등을 놓침(방어의 대가).")
    L.append("- **휘프소(whipsaw)/토글**: 팔자마자 오르고 사자마자 빠지는 톱니 반복. 토글=온↔오프 전환 횟수(휘프소 대리지표).")
    L.append("- **hindsight(사후정보)**: 결과를 이미 알고 되짚는 것. 이 백테스트는 hindsight **금지** — 익스포저는 오직 t−1 종가까지의 정보로만 계산.")

    # ── 경계 ──
    L.append("\n## 데이터·방법 경계(정직성)\n")
    for bm in bms:
        L.append(f"- **{bm['name']}**: {bm['span']}, {bm['nbars']}봉(~{bm['bpy']:.0f}/년). "
                 f"워밍업 {WARMUP}봉은 e=1(BH) — 이 구간 방어 없음.")
    L.append("- **VIX(M3)**: ^VIX 실측은 2000-01부터·캐시 끝단(최근) 결측 가능 → 그 구간 M3=중립(1.0), "
             "타 룰에 위임. 2000년 이전 위기(1929·1987·1990·1997·1998)는 M3 미적용.")
    L.append("- **1929·1987**: ^GSPC 풀히스토리에 포함되나 VIX 없음 → 가격룰(M1/M2/M4/M5)만 평가.")
    L.append("- **KR(^KS11)**: 1996-12 시작 → IMF(1997-07)는 워밍업(200MA 미확보) 구간과 겹쳐 M1 초기 방어 제한. 이 한계 명시.")
    L.append("- **거래비용**: 왕복 0.21/0.5/1.0% 감도. 위기때 슬리피지 확대는 정액모델이 과소계상(낙관편향) — "
             "감도표의 1.0%를 보수적 하한으로 참조.")
    L.append("- **자유도**: 5룰 임계값 노브 ~17개 vs 방어대상 실질표본 n≈6~9. 사전등록 그리드 전량공개로 "
             "곡선맞춤을 노출하되, '발견'이 아닌 '스트레스테스트'로 프레이밍.")
    L.append(f"\n---\n생성 스크립트: `research/studies/08_crisis_response/backtest_crisis_response.py`"
             f"(단독실행, IndexSource 전용, 결정론적 재현). 관련: BT-06(regime 양날의검)·BT-07(위기 특성화).\n")

    README_PATH.write_text("\n".join(L), encoding="utf-8")


if __name__ == "__main__":
    main()
