#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BT-09 신호 계층 — 정렬·지표 헬퍼 + 신호 컨텍스트(build_ctx).

역할: 벤치마크 날짜축에 크로스에셋 신호를 **룩어헤드 없이** 정렬(align_ff)하고,
NaN 내성 지표(SMA/변화율/z/롤링)를 파생해 build_ctx 하나로 묶는다.
전략(bt09_strategies)·리포트(bt09_report)·엔트리가 이 모듈을 공유한다.

경계: 매크로(TNX/oil/DXY/KRW)는 **신호전용** — 여기서 수익곡선을 만들지 않는다.
US계열 신호는 KR 벤치마크에서 strict=True(직전 세션값)로 정렬해 US→KR 시차를 차단.
"""
import sys
import math
from pathlib import Path
from datetime import date as _date

import numpy as np

# ── 부트스트랩(멱등): PYQuant + BT-08 엔진을 sys.path 에 배선 ──────────────────
_HERE = Path(__file__).resolve()
_REPO = _HERE.parents[3]                        # .../Quant
_PYQ = _REPO / "PYQuant"
_BT08 = _REPO / "research" / "backtests" / "08_crisis_response"
for _p in (str(_PYQ), str(_BT08)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import backtest_crisis_response as bt08                       # noqa: E402  (엔진 헬퍼 재사용)

load_series = bt08.load_series                                # build_ctx 가 사용

# ── 신호 상수 ────────────────────────────────────────────────────────────────
TODAY = _date.today().isoformat()
STALE_CAP_DAYS = 7          # 신호 정체 상한(달력일). 초과 → NA(중립)

# 신호 데이터 소스(달력 시작·가용범위 — probe로 실측):
#   ^VIX 1990 · ^TNX 1985(raw%) · CL=F 2000-08 · DX-Y.NYB 1985 · KRW=X 2003-12 · ^SOX 1994-05
SIGNAL_TICKERS = {
    "gspc": "^GSPC", "ixic": "^IXIC", "vix": "^VIX", "tnx": "^TNX",
    "oil": "CL=F", "dxy": "DX-Y.NYB", "krw": "KRW=X", "sox": "^SOX",
}
# 각 신호가 US(미국장) 마감 계열인지 → KR 벤치마크에서 strict=True(직전세션)로 정렬
US_SESSION = {"gspc", "ixic", "vix", "tnx", "oil", "dxy", "sox"}   # krw=국내FX(동일세션)


# ════════════════════════════════════════════════════════════════════════════
# 정렬·지표 헬퍼 (NaN 내성 — 결측신호는 조건 미성립=중립)
# ════════════════════════════════════════════════════════════════════════════
def _iso_days(a, b):
    ya, ma, da = int(a[:4]), int(a[5:7]), int(a[8:10])
    yb, mb, db = int(b[:4]), int(b[5:7]), int(b[8:10])
    return (_date(yb, mb, db) - _date(ya, ma, da)).days


def align_ff(dates_ref, dates_src, vals_src, strict=False, cap=STALE_CAP_DAYS):
    """dates_ref 각 날짜에 dates_src(<=d 또는 <d) 최신값 forward-fill.
    strict=True → 직전세션(<d)만(US→KR 시차). 정체>cap → NaN(중립)."""
    out = np.full(len(dates_ref), np.nan)
    n = len(dates_src)
    si, last_v, last_d = 0, np.nan, None
    for i, d in enumerate(dates_ref):
        while si < n and (dates_src[si] < d if strict else dates_src[si] <= d):
            last_v, last_d = vals_src[si], dates_src[si]
            si += 1
        if last_d is not None and _iso_days(last_d, d) <= cap:
            out[i] = last_v
    return out


def sma_nan(x, win):
    """완전창(NaN 없음)일 때만 SMA. 결측 관용."""
    n = len(x)
    out = np.full(n, np.nan)
    for t in range(win - 1, n):
        w = x[t - win + 1:t + 1]
        if not np.any(np.isnan(w)):
            out[t] = w.mean()
    return out


def diff_n(x, n):
    """x[t] - x[t-n] (양끝 non-NaN일 때만). 금리 %p 변화 등."""
    out = np.full(len(x), np.nan)
    for t in range(n, len(x)):
        if not (np.isnan(x[t]) or np.isnan(x[t - n])):
            out[t] = x[t] - x[t - n]
    return out


def pct_n(x, n):
    """x[t]/x[t-n]-1 (양끝 non-NaN·양수일 때만). 유가·지수 n일 변화율."""
    out = np.full(len(x), np.nan)
    for t in range(n, len(x)):
        a, b = x[t], x[t - n]
        if not (np.isnan(a) or np.isnan(b)) and b > 0:
            out[t] = a / b - 1.0
    return out


def roll_z(x, win):
    return bt08.rolling_z(x, win)


def roll_max(x, win):
    out = np.full(len(x), np.nan)
    for t in range(win - 1, len(x)):
        w = x[t - win + 1:t + 1]
        if not np.any(np.isnan(w)):
            out[t] = w.max()
    return out


def roll_min(x, win):
    out = np.full(len(x), np.nan)
    for t in range(win - 1, len(x)):
        w = x[t - win + 1:t + 1]
        if not np.any(np.isnan(w)):
            out[t] = w.min()
    return out


def roll_pctrank(x, win):
    """창 내 x[t]의 백분위(0~1). 저변동성 압축 판정용."""
    out = np.full(len(x), np.nan)
    for t in range(win - 1, len(x)):
        w = x[t - win + 1:t + 1]
        if np.any(np.isnan(w)):
            continue
        out[t] = float(np.mean(w <= x[t]))
    return out


def _ok(v):
    return not (v is None or (isinstance(v, float) and math.isnan(v)))


def _series_ret(x):
    out = np.full(len(x), np.nan)
    for t in range(1, len(x)):
        if not (np.isnan(x[t]) or np.isnan(x[t - 1])) and x[t - 1] > 0:
            out[t] = x[t] / x[t - 1] - 1.0
    return out


# ════════════════════════════════════════════════════════════════════════════
# 컨텍스트: 벤치마크 날짜에 정렬된 신호배열 + 파생 (전부 close t 까지)
# ════════════════════════════════════════════════════════════════════════════
def build_ctx(src, dates, is_kr):
    """벤치마크 dates 에 모든 신호를 정렬(US계열은 KR일 때 strict=True). 파생지표 포함."""
    raw = {}
    cov = {}
    for key, tk in SIGNAL_TICKERS.items():
        sd, sc = load_series(src, tk, "1985-01-01", TODAY)
        strict = is_kr and (key in US_SESSION)
        a = align_ff(dates, sd, sc, strict=strict) if len(sc) else np.full(len(dates), np.nan)
        # TNX 스케일 방어(현재 raw% median≈4.4; 혹시 x10로 바뀌면 정규화)
        if key == "tnx" and len(sc):
            fin = a[~np.isnan(a)]
            if len(fin) and np.median(fin) > 20.0:
                a = a / 10.0
        raw[key] = a
        cov[key] = float(np.mean(~np.isnan(a))) * 100.0 if len(a) else 0.0
    ctx = dict(raw)
    ctx["_cov"] = cov
    # 파생
    ctx["oil_20d"] = pct_n(raw["oil"], 20)
    ctx["tnx_20d"] = diff_n(raw["tnx"], 20)     # %p
    ctx["tnx_5d"]  = diff_n(raw["tnx"], 5)
    ctx["tnx_sma100"] = sma_nan(raw["tnx"], 100)
    ctx["vix_1d"]  = pct_n(raw["vix"], 1)
    ctx["vix_d5"]  = diff_n(raw["vix"], 5)
    ctx["vix_d3"]  = diff_n(raw["vix"], 3)
    ctx["z_vix_d5"] = roll_z(ctx["vix_d5"], 120)
    ctx["z_vix_d3"] = roll_z(ctx["vix_d3"], 120)
    ctx["vix_sma20"] = sma_nan(raw["vix"], 20)
    ctx["krw_d5"]  = pct_n(raw["krw"], 5)
    ctx["z_krw_d5"] = roll_z(ctx["krw_d5"], 60)
    ctx["dxy_20d"] = pct_n(raw["dxy"], 20)
    ctx["z_dxy_20d"] = roll_z(ctx["dxy_20d"], 120)
    ctx["dxy_lag10"] = np.concatenate([np.full(10, np.nan), raw["dxy"][:-10]])
    ctx["vix_lag5"]  = np.concatenate([np.full(5, np.nan), raw["vix"][:-5]])
    ctx["sox_sma50"] = sma_nan(raw["sox"], 50)
    ctx["gspc_ret"]  = _series_ret(raw["gspc"])
    return ctx
