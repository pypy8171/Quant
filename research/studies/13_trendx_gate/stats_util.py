"""scipy 없는 환경용 통계 보조 — t 분포 양측 p값, BH(FDR) 보정, Spearman.

이 머신의 `py`·`.venv-win` 둘 다 scipy가 없다(2026-09-11 확인). 월 시계열 1표본 t의 p값과
탐색 격자의 BH 보정만 필요하므로 정규화 불완전 베타를 연분수(Numerical Recipes betacf)로 직접 둔다.
"""
from __future__ import annotations

import math

import numpy as np
import pandas as pd


def _betacf(a: float, b: float, x: float) -> float:
    """정규화 불완전 베타의 연분수 부분. NR 6.4의 betacf를 그대로 옮겼다."""
    max_it, eps, fpmin = 300, 3e-14, 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    if abs(d) < fpmin:
        d = fpmin
    d = 1.0 / d
    h = d
    for m in range(1, max_it + 1):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < fpmin:
            d = fpmin
        c = 1.0 + aa / c
        if abs(c) < fpmin:
            c = fpmin
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < fpmin:
            d = fpmin
        c = 1.0 + aa / c
        if abs(c) < fpmin:
            c = fpmin
        d = 1.0 / d
        de = d * c
        h *= de
        if abs(de - 1.0) < eps:
            break
    return h


def betainc(a: float, b: float, x: float) -> float:
    """정규화 불완전 베타 I_x(a, b)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lbeta = math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
    bt = math.exp(lbeta + a * math.log(x) + b * math.log(1.0 - x))
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, 1.0 - x) / b


def t_sf2(t: float, df: float) -> float:
    """Student t 양측 p값 P(|T| ≥ |t|)."""
    if not np.isfinite(t) or df <= 0:
        return float("nan")
    x = df / (df + t * t)
    return betainc(df / 2.0, 0.5, x)


def one_sample_t(x) -> tuple[float, float, float, int]:
    """평균, t, 양측 p, n. n<2면 nan."""
    v = np.asarray([float(a) for a in x if np.isfinite(a)])
    n = len(v)
    if n < 2:
        return (float(v.mean()) if n else float("nan"), float("nan"), float("nan"), n)
    sd = v.std(ddof=1)
    if sd == 0.0:
        return float(v.mean()), float("nan"), float("nan"), n
    t = v.mean() / (sd / math.sqrt(n))
    return float(v.mean()), float(t), t_sf2(t, n - 1), n


def bh_qvalues(p: list[float]) -> list[float]:
    """Benjamini–Hochberg q값. nan은 그대로 둔다."""
    arr = np.asarray(p, dtype=float)
    ok = np.isfinite(arr)
    q = np.full_like(arr, np.nan)
    pv = arr[ok]
    m = len(pv)
    if m == 0:
        return q.tolist()
    order = np.argsort(pv)
    ranked = pv[order] * m / (np.arange(m) + 1)
    # 뒤에서부터 누적 최소로 단조성을 맞춘다
    ranked = np.minimum.accumulate(ranked[::-1])[::-1]
    out = np.empty(m)
    out[order] = np.minimum(ranked, 1.0)
    q[ok] = out
    return q.tolist()


def spearman(x: pd.Series, y: pd.Series) -> float:
    """Spearman ρ. 결측 제거 후 n<3이면 nan."""
    d = pd.concat([x, y], axis=1).dropna()
    if len(d) < 3:
        return float("nan")
    rx = d.iloc[:, 0].rank()
    ry = d.iloc[:, 1].rank()
    if rx.std() == 0 or ry.std() == 0:
        return float("nan")
    return float(np.corrcoef(rx, ry)[0, 1])
