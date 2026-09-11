#!/usr/bin/env python3
"""TRENDX 게이트 하락장 일봉 근사 백테스트 — 선정 층만 잰다.

라이브 TRENDX(DeviationScale 추세확장 슬리브)는 3분봉 시점정합(PIT) 재현이 안 된다. 여기서는
일봉으로 "정배열 ∧ 이격 밴드" 게이트가 고른 종목이 같은 날 PIT 풀보다 나은지만 본다.
실행 층(현재가 기준점 rung 구간·물타기·매도가능 0·재기동)은 이 스크립트가 못 잰다 — README.md 첫 절.

귀무가설: 정배열 단독 월초과 −0.089R (docs/DECISIONS.md D-013). 주검정은 현행 설정 1셀
(이격 5~35 · 저항 off · 손절 없음), 나머지 11셀은 탐색이라 BH(FDR) 0.1로 본다.

입력  PYQuant/data/bars_all_pit.parquet (2019-01-02~2026-09-04, 상폐 포함)
출력  results.tsv · monthly_excess.csv · gate_pairs.jsonl (B2-1 백필 대상)

초과R 열 세 가지(results.tsv). 판정(t·p·beat·q_bh)은 excess_r 하나로만 한다.
  excess_r   일 정합 — 날짜별 (그날 gate 거래 평균 r − 그날 풀 평균 r)을 gate 거래가 있는 날만 내고,
             그 일별 초과를 월 평균한 뒤 월 시계열에 1표본 t. 09-12 bias-auditor 감사 반영.
  excess_mw  월가중 — 월별 gate 평균 r − 월별 풀 평균 r의 평균(감사 전 방식). 풀 쪽에 gate 거래가 없는
             날도 들어가 신호 타이밍 효과가 섞인다. 비교용으로 남긴다.
  excess_tw  거래가중 — 구간 전체 gate_r − base_r. 거래가 많은 날·달이 무겁게 들어간다.
monthly_excess.csv는 excess_r(일 정합)·excess_mw(월가중) 두 열을 월별로 남긴다.

    py research/studies/13_trendx_gate/run_trendx_gate.py              # 전체
    py research/studies/13_trendx_gate/run_trendx_gate.py --sma-prev   # 전일 확정 SMA 감도 → results_smaprev.tsv
    py research/studies/13_trendx_gate/run_trendx_gate.py --pairs-only  # gate_pairs.jsonl만
    py research/studies/13_trendx_gate/run_trendx_gate.py --no-pairs    # gate_pairs.jsonl을 다시 쓰지 않는다(백필이 읽는 중일 때)
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd
from numpy.lib.stride_tricks import sliding_window_view

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parents[2]
sys.path.insert(0, str(_HERE))
from stats_util import bh_qvalues, one_sample_t  # noqa: E402

DATA = _ROOT / "PYQuant" / "data" / "bars_all_pit.parquet"

# 구간은 research/studies/10_regime_scorer/ablate.py와 같은 날짜를 쓴다(06_bear_market 정의).
PERIODS = {
    "2020covid":    ("2020-01-02", "2020-12-30"),
    "2022bear":     ("2022-01-03", "2022-12-29"),   # 홀드아웃 — 설계에 보지 않았다
    "2024blackmon": ("2023-12-28", "2024-12-30"),
    "2026now":      ("2025-12-30", "2026-08-04"),
}
HOLDOUT = "2022bear"

POOL_N = 500          # 날짜별 20일 평균 거래대금 상위
MAX_HOLD = 20         # 거래일
TP_PCT = 3.0          # 익절 rung (dev_sell_pct=3.0)
COST_PCT = 0.31       # 왕복 비용 — PYQuant/backtest/metrics.py ROUNDTRIP_COST_PCT, D-013과 같은 값
R_DENOM = 6.0         # R 분모(%). 손절 없는 셀도 같은 분모로 두어 셀끼리 비교되게 한다
RES_WINDOW = 250      # 저항 필터 롤링 고가 창
RES_MIN = 120         # 창이 이보다 짧으면 저항 필터를 판정하지 않는다(시작 구간 결손 — 감사 항목)
RES_DIST_PCT = 15.0   # 250일 고가 대비 −15% 안쪽만 진입

# 격자 — (밴드, 저항, 손절). 첫 셀이 주검정.
BANDS = {"5~35": (5.0, 35.0), "5~15": (5.0, 15.0)}
STOPS = {"none": 0.0, "2.5": 2.5, "6": 6.0}
MAIN_CELL = ("5~35", "off", "none")


def krx_tick(px: np.ndarray) -> np.ndarray:
    """KRX 호가단위(2023-01 개편 기준). 2023 이전은 1만~5만 구간이 달랐지만 +1틱 판정에는 영향이 작다."""
    return np.select(
        [px < 2000, px < 5000, px < 20000, px < 50000, px < 200000, px < 500000],
        [1, 5, 10, 50, 100, 500], default=1000).astype(float)


def load_bars() -> pd.DataFrame:
    d = pd.read_parquet(DATA, columns=["Date", "code", "Open", "High", "Low", "Close", "Volume", "delisted"])
    d = d.sort_values(["code", "Date"]).reset_index(drop=True)
    for c in ("Open", "High", "Low", "Close"):
        d[c] = d[c].astype(float)
    return d


def add_features(d: pd.DataFrame, sma_prev: bool) -> pd.DataFrame:
    """정배열·이격·거래대금·저항 거리. sma_prev=True면 SMA를 전일 확정값으로 한 칸 민다(감도)."""
    g = d.groupby("code", sort=False)
    c = d["Close"]
    for n in (5, 10, 20, 60):
        d[f"sma{n}"] = g["Close"].transform(lambda s, n=n: s.rolling(n).mean())
    if sma_prev:
        for n in (5, 10, 20, 60):
            d[f"sma{n}"] = g[f"sma{n}"].shift(1)
    d["tv20"] = g["Close"].transform(lambda s: (s * d.loc[s.index, "Volume"]).rolling(20).mean())
    d["hi250"] = g["High"].transform(lambda s: s.rolling(RES_WINDOW, min_periods=RES_MIN).max())
    d["hi250_n"] = g["High"].transform(lambda s: s.rolling(RES_WINDOW, min_periods=1).count())
    d["aligned"] = (d["sma5"] > d["sma10"]) & (d["sma10"] > d["sma20"]) & (d["sma20"] > d["sma60"])
    d["pull_pct"] = (c - d["sma20"]) / d["sma20"] * 100.0
    d["res_dist_pct"] = (c / d["hi250"] - 1.0) * 100.0          # 0이면 고가, −15면 고가 대비 −15%
    d["res_ok"] = d["res_dist_pct"] >= -RES_DIST_PCT             # hi250이 nan이면 False
    # PIT 풀 — 그날 tv20 상위 500. Volume이 분할 미조정이라 분할 전후는 흔들린다(감사 항목)
    rank = d.groupby("Date")["tv20"].rank(ascending=False, method="first")
    d["in_pool"] = rank <= POOL_N
    return d


def simulate_code(o, h, l, c, stop_pct: float, tp_close: bool):
    """한 종목의 모든 신호일 i에 대해 익일 시가 진입 → 손절/익절/20일 청산을 벡터로 푼다.

    같은 날 안의 우선순위는 D-013과 같다: 갭 하락(시가≤손절가) → 손절(저가≤손절가, 최악 가정)
    → 익절(고가≥목표+1틱). 익절은 목표가 체결(touch), tp_close=True면 그날 종가 체결(감도).
    데이터가 끝나면(상폐·자료 끝) 마지막 종가로 청산한다.
    """
    n = len(c)
    K = MAX_HOLD
    pad = np.full(K, np.nan)
    ow = sliding_window_view(np.concatenate([o[1:], pad]), K)[:n]
    hw = sliding_window_view(np.concatenate([h[1:], pad]), K)[:n]
    lw = sliding_window_view(np.concatenate([l[1:], pad]), K)[:n]
    cw = sliding_window_view(np.concatenate([c[1:], pad]), K)[:n]
    entry = ow[:, 0]
    valid = np.isfinite(entry)
    with np.errstate(invalid="ignore"):
        stop_px = entry * (1.0 - stop_pct / 100.0) if stop_pct > 0 else np.full(n, -np.inf)
        tp_px = entry * (1.0 + TP_PCT / 100.0)
        tp_trig = tp_px + krx_tick(tp_px)
        gap = ow <= stop_px[:, None]
        stp = lw <= stop_px[:, None]
        tp = hw >= tp_trig[:, None]
    any_ex = gap | stp | tp
    has = any_ex.any(axis=1)
    first = np.where(has, any_ex.argmax(axis=1), K - 1)
    n_avail = np.isfinite(cw).sum(axis=1)                       # 남은 거래일 수(연속 가정)
    last = np.maximum(n_avail - 1, 0)
    k = np.minimum(first, last)
    rows = np.arange(n)
    is_gap = gap[rows, k] & (first <= last)
    is_stp = stp[rows, k] & ~is_gap & (first <= last)
    is_tp = tp[rows, k] & ~is_gap & ~is_stp & (first <= last)
    exit_px = cw[rows, k]                                       # 기본: 20일 종가 또는 자료 끝
    exit_px = np.where(is_tp, (cw[rows, k] if tp_close else tp_px), exit_px)
    exit_px = np.where(is_stp, stop_px, exit_px)
    exit_px = np.where(is_gap, ow[rows, k], exit_px)
    why = np.full(n, "hold", dtype=object)
    why[(k == K - 1) & ~is_gap & ~is_stp & ~is_tp] = "hold"
    why[(k < K - 1) & ~is_gap & ~is_stp & ~is_tp] = "trunc"    # 자료 끝·상폐
    why[is_tp] = "tp"
    why[is_stp] = "stop"
    why[is_gap] = "gap"
    lo_min = np.fmin.accumulate(lw, axis=1)[rows, k]
    hi_max = np.fmax.accumulate(hw, axis=1)[rows, k]
    with np.errstate(invalid="ignore", divide="ignore"):
        gross = (exit_px / entry - 1.0) * 100.0
        mae = (lo_min / entry - 1.0) * 100.0
        mfe = (hi_max / entry - 1.0) * 100.0
    return entry, exit_px, k + 1, gross, mae, mfe, why, valid


def simulate_all(d: pd.DataFrame, stop_pct: float, tp_close: bool) -> pd.DataFrame:
    parts = []
    for code, idx in d.groupby("code", sort=False).indices.items():
        sub = d.iloc[idx]
        entry, exit_px, hold, gross, mae, mfe, why, valid = simulate_code(
            sub["Open"].to_numpy(), sub["High"].to_numpy(), sub["Low"].to_numpy(),
            sub["Close"].to_numpy(), stop_pct, tp_close)
        parts.append(pd.DataFrame({"entry": entry, "exit_px": exit_px, "hold": hold, "gross": gross,
                                   "mae": mae, "mfe": mfe, "why": why, "valid": valid}, index=sub.index))
    out = pd.concat(parts).sort_index()
    out["net"] = out["gross"] - COST_PCT
    out["r"] = out["net"] / R_DENOM
    return out


def daily_aligned_excess(t: pd.DataFrame, b: pd.DataFrame) -> pd.Series:
    """일 정합 초과R의 월 시계열. 같은 날의 gate 평균 r − 풀 평균 r을 gate 거래가 있는 날만 내고 월 평균한다."""
    gd = t.groupby("Date")["r"].mean()
    bd = b.groupby("Date")["r"].mean()
    ex_d = (gd - bd.reindex(gd.index)).dropna()
    return ex_d.groupby(ex_d.index.to_period("M")).mean()


def month_stats(tr: pd.DataFrame, base: pd.DataFrame, lo: str, hi: str) -> dict:
    """구간 안의 초과R 세 가지(머리 주석)와 부수 지표. tr·base는 Date·r·net·gross·mae·hold·why 열.

    판정 통계(t·p·beat)는 일 정합 excess_r의 월 시계열에서만 낸다. excess_mw는 월별 gate 평균 − 월별 풀 평균
    (감사 전 방식), excess_tw는 구간 전체 거래가중 gate_r − base_r로 비교용이다.
    """
    t = tr[(tr["Date"] >= lo) & (tr["Date"] <= hi)]
    b = base[(base["Date"] >= lo) & (base["Date"] <= hi)]
    if t.empty:
        return {"n": 0}
    ex = daily_aligned_excess(t, b)
    tm = t.groupby(t["Date"].dt.to_period("M"))["r"].mean()
    bm = b.groupby(b["Date"].dt.to_period("M"))["r"].mean()
    ex_mw = (tm - bm).dropna()
    mean, tstat, p, nm = one_sample_t(ex.to_numpy())
    beat = float((ex > 0).mean()) if nm else float("nan")
    gate_r = float(t["r"].mean())
    base_r = float(b["r"].mean()) if len(b) else float("nan")
    # 최대낙폭: 월 동일가중 순수익률(%)의 누적합 곡선
    curve = t.groupby(t["Date"].dt.to_period("M"))["net"].mean().cumsum()
    mdd = float((curve - curve.cummax()).min()) if len(curve) else float("nan")
    gross_abs = t["gross"].abs().mean()
    return {
        "n": int(len(t)), "n_days": int(t["Date"].nunique()), "n_months": nm,
        "gate_r": gate_r, "base_r": base_r,
        "excess_tw": gate_r - base_r, "excess_mw": float(ex_mw.mean()) if len(ex_mw) else float("nan"),
        "excess_r": mean, "t": tstat, "p": p, "beat": beat,
        "mae_med": float(t["mae"].median()), "mae_p25": float(t["mae"].quantile(0.25)),
        "mae_p75": float(t["mae"].quantile(0.75)), "mdd_pct": mdd,
        "hold_mean": float(t["hold"].mean()), "turnover_yr": 252.0 / float(t["hold"].mean()),
        "cost_share": float(COST_PCT / gross_abs) if gross_abs > 0 else float("nan"),
        "tp_rate": float((t["why"] == "tp").mean()), "stop_rate": float(t["why"].isin(["stop", "gap"]).mean()),
        "trunc_rate": float((t["why"] == "trunc").mean()),
        "monthly": ex, "monthly_mw": ex_mw,
    }


def write_gate_pairs(d: pd.DataFrame, path: Path, since: str = "2025-09-11", cap: int = 60) -> int:
    """B2-1 백필 대상: 날짜별 정배열 ∧ 5≤pull≤35 통과 종목(PIT 풀 안), 60 초과면 tv20 상위 60."""
    lo, hi = BANDS["5~35"]
    m = (d["Date"] >= since) & d["in_pool"] & d["aligned"] & (d["pull_pct"] >= lo) & (d["pull_pct"] <= hi)
    sel = d.loc[m, ["Date", "code", "tv20"]]
    n = 0
    with path.open("w", encoding="utf-8") as f:
        for day, g in sel.groupby("Date"):
            tks = g.sort_values("tv20", ascending=False)["code"].head(cap).tolist()
            f.write(json.dumps({"ymd": day.strftime("%Y%m%d"), "tickers": tks}) + "\n")
            n += len(tks)
    return n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs-only", action="store_true")
    ap.add_argument("--sma-prev", action="store_true", help="SMA를 전일 확정값으로(감도)")
    ap.add_argument("--tag", default="", help="출력 파일 접미사")
    ap.add_argument("--no-pairs", action="store_true", help="gate_pairs.jsonl을 다시 쓰지 않는다(백필이 읽는 중)")
    args = ap.parse_args()
    t0 = time.time()

    d = load_bars()
    d = add_features(d, args.sma_prev)
    print(f"[load] {len(d):,} rows · {d['code'].nunique()} codes · {time.time()-t0:.0f}s", flush=True)

    if not args.sma_prev and not args.no_pairs:
        n_pairs = write_gate_pairs(d, _HERE / "gate_pairs.jsonl")
        print(f"[pairs] gate_pairs.jsonl {n_pairs:,} (종목,날짜)쌍", flush=True)
    if args.pairs_only:
        return 0

    # 격자 셀에 필요한 청산 규칙 6개(손절 3 × 익절체결 2)를 한 번씩만 시뮬레이션한다
    sims: dict[tuple, pd.DataFrame] = {}
    for stop_name, stop_pct in STOPS.items():
        for tp_close in (False, True):
            ts = time.time()
            sims[(stop_name, tp_close)] = simulate_all(d, stop_pct, tp_close)
            print(f"[sim] stop={stop_name} tp_close={tp_close} {time.time()-ts:.0f}s", flush=True)

    base_cols = ["Date"]
    rows, monthly = [], {}
    for tp_close in (False, True):
        for band_name, (lo_b, hi_b) in BANDS.items():
            for res_name in ("off", "on"):
                for stop_name in STOPS:
                    s = sims[(stop_name, tp_close)]
                    ok = s["valid"] & d["in_pool"]
                    gate = ok & d["aligned"] & (d["pull_pct"] >= lo_b) & (d["pull_pct"] <= hi_b)
                    if res_name == "on":
                        gate &= d["res_ok"]
                    cols = ["r", "net", "gross", "mae", "hold", "why"]
                    tr = pd.concat([d.loc[gate, base_cols], s.loc[gate, cols]], axis=1)
                    base = pd.concat([d.loc[ok, base_cols], s.loc[ok, cols]], axis=1)
                    cell = (band_name, res_name, stop_name)
                    per_stats = {per: month_stats(tr, base, lo, hi) for per, (lo, hi) in PERIODS.items()}
                    # 4구간 합산: 구간별 월 초과R 시계열을 이어 붙여 하나의 1표본 t로 본다(일 정합 기준)
                    parts = [st["monthly"] for st in per_stats.values() if st.get("n", 0)]
                    ex = pd.concat(parts) if parts else pd.Series(dtype=float)
                    parts_mw = [st["monthly_mw"] for st in per_stats.values() if st.get("n", 0)]
                    ex_mw = pd.concat(parts_mw) if parts_mw else pd.Series(dtype=float)
                    in_any = pd.Series(False, index=tr.index)
                    for lo, hi in PERIODS.values():
                        in_any |= (tr["Date"] >= lo) & (tr["Date"] <= hi)
                    t4 = tr[in_any]
                    b_any = pd.Series(False, index=base.index)
                    for lo, hi in PERIODS.values():
                        b_any |= (base["Date"] >= lo) & (base["Date"] <= hi)
                    mean, tstat, p, nm = one_sample_t(ex.to_numpy())
                    gate_r4 = float(t4["r"].mean()) if len(t4) else float("nan")
                    base_r4 = float(base.loc[b_any, "r"].mean()) if b_any.any() else float("nan")
                    all4 = {"n": int(len(t4)), "n_days": int(t4["Date"].nunique()), "n_months": nm,
                            "gate_r": gate_r4, "base_r": base_r4,
                            "excess_tw": gate_r4 - base_r4,
                            "excess_mw": float(ex_mw.mean()) if len(ex_mw) else float("nan"),
                            "excess_r": mean, "t": tstat, "p": p,
                            "beat": float((ex > 0).mean()) if nm else float("nan"),
                            "mae_med": float(t4["mae"].median()) if len(t4) else float("nan"),
                            "mae_p25": float(t4["mae"].quantile(0.25)) if len(t4) else float("nan"),
                            "mae_p75": float(t4["mae"].quantile(0.75)) if len(t4) else float("nan"),
                            "hold_mean": float(t4["hold"].mean()) if len(t4) else float("nan"),
                            "tp_rate": float((t4["why"] == "tp").mean()) if len(t4) else float("nan"),
                            "stop_rate": float(t4["why"].isin(["stop", "gap"]).mean()) if len(t4) else float("nan"),
                            "cost_share": float(COST_PCT / t4["gross"].abs().mean()) if len(t4) else float("nan"),
                            "monthly": ex, "monthly_mw": ex_mw}
                    per_stats["all4"] = all4
                    for per, st in per_stats.items():
                        monthly[(tp_close, *cell, per)] = (st.pop("monthly", pd.Series(dtype=float)),
                                                           st.pop("monthly_mw", pd.Series(dtype=float)))
                        rows.append({"tp_close": tp_close, "band": band_name, "res": res_name, "stop": stop_name,
                                     "period": per, "main": (cell == MAIN_CELL and not tp_close),
                                     "holdout": per == HOLDOUT, **st})
    res = pd.DataFrame(rows)

    # 탐색 11셀 BH — 4구간 합산 월 시계열, 익절 touch 기준
    expl = res[(~res["tp_close"]) & (res["period"] == "all4") & (~res["main"])].copy()
    q = bh_qvalues(expl["p"].tolist())
    res["q_bh"] = np.nan
    res.loc[expl.index, "q_bh"] = q
    tag = args.tag or ("_smaprev" if args.sma_prev else "")
    out = _HERE / f"results{tag}.tsv"
    res.to_csv(out, sep="\t", index=False, float_format="%.4f")
    # 월 초과R 시계열(전 셀·구간) — excess_r 일 정합, excess_mw 월가중
    mrows = []
    for (tp_close, band, res_, stop, per), (ser, ser_mw) in monthly.items():
        for m in ser.index.union(ser_mw.index):
            mrows.append({"tp_close": tp_close, "band": band, "res": res_, "stop": stop, "period": per,
                          "month": str(m), "excess_r": ser.get(m, np.nan), "excess_mw": ser_mw.get(m, np.nan)})
    pd.DataFrame(mrows).to_csv(_HERE / f"monthly_excess{tag}.csv", index=False, float_format="%.5f")

    # 저항 필터 시작 구간 결손 — 감사용
    res_short = d.loc[d["in_pool"] & d["aligned"], "hi250_n"]
    print(f"[audit] 저항 창<250일 비율(풀∧정배열 행 기준) {(res_short < RES_WINDOW).mean():.3f}, "
          f"<{RES_MIN}일(판정 불가) {(res_short < RES_MIN).mean():.3f}", flush=True)
    view = res[(~res["tp_close"])][["band", "res", "stop", "period", "n", "n_months", "gate_r", "base_r",
                                    "excess_tw", "excess_mw", "excess_r", "t", "p", "q_bh", "beat", "mae_med",
                                    "hold_mean", "tp_rate", "main"]]
    pd.set_option("display.width", 220)
    pd.set_option("display.max_rows", 200)
    print(view.to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    print(f"[done] {out} {time.time()-t0:.0f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
