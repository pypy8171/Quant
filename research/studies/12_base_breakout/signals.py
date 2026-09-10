# -*- coding: utf-8 -*-
"""바닥다지기→재상승 진입 후보 트리거 정의와 평가.

모든 피처는 t 시점까지의 정보만 쓴다(shift 없이 rolling만 사용하고, 신호는 종가 확정 후
익일 시가 체결로 평가한다). 저점일·회복 여부 같은 사후 정보는 피처에 넣지 않는다.
"""
import numpy as np
import pandas as pd


def add_features(df: pd.DataFrame) -> pd.DataFrame:
    """단일 종목 일봉(date 오름차순)에 피처를 붙인다. 입력 컬럼: open/high/low/close/volume."""
    d = df.sort_values("date").copy()
    c, v, h, l = d["close"], d["volume"], d["high"], d["low"]
    d["tv"] = c * v                                    # 거래대금
    for w in (5, 20, 60, 120, 200):
        d[f"ma{w}"] = c.rolling(w).mean()
    d["tv20"] = d["tv"].rolling(20).mean()
    d["vol20"] = v.rolling(20).mean()
    d["ret1"] = c.pct_change()
    d["atr20"] = (h - l).rolling(20).mean() / c        # 정규화 변동폭
    d["hi250"] = c.rolling(250, min_periods=60).max()
    d["dd"] = c / d["hi250"] - 1                       # 250일 고점 대비 낙폭
    d["lo60"] = l.rolling(60).min()
    d["lo20"] = l.rolling(20).min()
    d["hi20"] = h.rolling(20).max()
    # 박스폭 수축(VCP 대용): 20일 (고-저)/저
    d["box20"] = (h.rolling(20).max() - l.rolling(20).min()) / l.rolling(20).min()
    d["box20_prev"] = d["box20"].shift(20)
    # 60일 창 안에서 최저가를 찍은 뒤 며칠 지났나
    idx = np.arange(len(d), dtype=float)
    argmin_pos = l.rolling(60).apply(lambda x: float(np.argmin(x)), raw=True)
    d["days_since_lo60"] = 59.0 - argmin_pos
    return d


def add_market(d: pd.DataFrame, mkt: pd.DataFrame) -> pd.DataFrame:
    """지수 컬럼과 상대강도(RS)를 붙인다. mkt: date, mclose."""
    d = d.merge(mkt, on="date", how="left")
    d["rs"] = d["close"] / d["mclose"]
    d["rs_hi40"] = d["rs"].rolling(40).max()
    d["rs_new_hi"] = d["rs"] >= d["rs_hi40"] - 1e-12
    d["m_ma20"] = d["mclose"].rolling(20).mean()
    d["m_ma60"] = d["mclose"].rolling(60).mean()
    d["m_above20"] = d["mclose"] > d["m_ma20"]
    d["m_dd"] = d["mclose"] / d["mclose"].rolling(250, min_periods=60).max() - 1
    return d


# --- 트리거들. 각 함수는 bool Series를 돌려준다(그날 종가 확정 시 발생). ---

def t1_ma20_reclaim(d):
    """20MA 상향 회복 첫날. 가장 단순한 반등 진입."""
    return (d["close"] > d["ma20"]) & (d["close"].shift(1) <= d["ma20"].shift(1))


def t2_ma60_reclaim(d):
    return (d["close"] > d["ma60"]) & (d["close"].shift(1) <= d["ma60"].shift(1))


def t3_golden20_60(d):
    return (d["ma20"] > d["ma60"]) & (d["ma20"].shift(1) <= d["ma60"].shift(1))


def t4_double_bottom(d, tol=0.05, min_gap=8, look=70):
    """쌍바닥 넥라인 돌파. 두 저점이 tol 안, 그 사이 반등고점(넥라인)을 종가로 상향돌파."""
    n = len(d)
    lo, cl, hi = d["low"].values, d["close"].values, d["high"].values
    out = np.zeros(n, dtype=bool)
    for i in range(look, n):
        w_lo = lo[i - look:i + 1]
        j1 = int(np.argmin(w_lo))                      # 1차 저점(창 내 최저)
        if j1 > look - min_gap:                        # 최저가 너무 최근이면 2차 저점 없음
            continue
        seg = w_lo[j1 + min_gap:]
        if len(seg) == 0:
            continue
        j2 = j1 + min_gap + int(np.argmin(seg))        # 2차 저점
        if w_lo[j2] > w_lo[j1] * (1 + tol):            # 2차 저점이 너무 높으면 쌍바닥 아님
            continue
        neck = hi[i - look + j1: i - look + j2 + 1].max()
        if cl[i] > neck and cl[i - 1] <= neck:
            out[i] = True
    return pd.Series(out, index=d.index)


def t5_pocket_pivot(d, box_shrink=0.7, vol_mult=1.8):
    """변동성 수축 후 거래량 동반 상승. 박스폭이 20일 전보다 줄고 거래량이 20일평균 대비 급증."""
    return (
        (d["box20"] < d["box20_prev"] * box_shrink)
        & (d["volume"] > d["vol20"] * vol_mult)
        & (d["ret1"] > 0.03)
        & (d["close"] > d["ma20"])
    )


def t6_rs_new_high(d):
    """상대강도 40일 신고가 — 지수보다 먼저 도는 종목."""
    return d["rs_new_hi"] & (~d["rs_new_hi"].shift(1).fillna(False)) & (d["dd"] < -0.15)


def t7_higher_low_breakout(d):
    """저점 높이기 후 20일 고점 돌파. 하락 추세선 이탈의 대용."""
    return (
        (d["close"] > d["hi20"].shift(1))
        & (d["lo20"] > d["lo60"].shift(20) * 1.02)
        & (d["dd"] < -0.15)
    )


TRIGGERS = {
    "T1_ma20_reclaim": t1_ma20_reclaim,
    "T2_ma60_reclaim": t2_ma60_reclaim,
    "T3_golden_20_60": t3_golden20_60,
    "T4_double_bottom": t4_double_bottom,
    "T5_pocket_pivot": t5_pocket_pivot,
    "T6_rs_new_high": t6_rs_new_high,
    "T7_hl_breakout": t7_higher_low_breakout,
}


def forward_stats(d, sig, horizons=(5, 10, 20, 40)):
    """신호 다음날 시가 진입 가정. 각 구간 수익률과 진입 후 최대낙폭(MAE)."""
    o = d["open"].shift(-1)                            # 익일 시가 = 체결가
    rows = []
    for i in np.flatnonzero(sig.values):
        if i + 1 >= len(d):
            continue
        entry = o.iloc[i]
        if not np.isfinite(entry) or entry <= 0:
            continue
        r = {"date": d["date"].iloc[i], "entry": entry}
        for hz in horizons:
            j = min(i + hz, len(d) - 1)
            r[f"r{hz}"] = d["close"].iloc[j] / entry - 1
            r[f"mae{hz}"] = d["low"].iloc[i + 1:j + 1].min() / entry - 1
        rows.append(r)
    return pd.DataFrame(rows)
