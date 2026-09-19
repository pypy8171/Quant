#!/usr/bin/env python3
"""TRENDX 유지 게이트(hold_zone) 정배열 축 완화 — 일봉 근사 백테스트.

라이브 DeviationScaleStrategy(TRENDX 슬리브)는 hold_zone = aligned_hold AND band 로 유지를 판정한다.
 - aligned_hold : 전일 확정 일봉 SMA5>10>20>60 (느린 축, D-033)
 - band         : (현재가 - 일봉SMA20)/SMA20 이 [entry_lower-hyst, entry_upper+hyst] = [1%, 39%]
GS건설 사례는 band가 맞는데 aligned_hold가 깨져 청산으로 빠진 경우다. 이 스크립트는 진입을 고정하고
유지 축만 갈아끼워(V1~V4) 같은 진입집합에서 실현손익이 어떻게 달라지는지 짝지어 비교한다.

측정하는 것 : 유지 축(느린 정배열 / 당일 정배열 / 이격 단독)이 청산 시점과 왕복 횟수에 주는 영향.
못 재는 것  : 장중 체결(틱 단위 존 이탈), 분할 매수 물타기, 매도가능 0, 재기동, 유니버스 등록층 경합.
             band는 일봉 종가로만 평가하므로 장중에만 밴드를 벗어났다 돌아온 날은 전 변형에서 똑같이 놓친다.

입력  PYQuant/data/bars_all_pit.parquet (2019-01-02~2026-09-04, 상폐 포함, delisted 열)
출력  results.tsv · trades_<변형>.csv.gz · paired.tsv

    py research/studies/14_hold_axis/run_hold_axis.py
    py research/studies/14_hold_axis/run_hold_axis.py --no-score-top   # 점수 상위 25 필터 없이(감도)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parents[2]
sys.path.insert(0, str(_ROOT / "research" / "studies" / "13_trendx_gate"))
from stats_util import one_sample_t  # noqa: E402

# 일봉 스냅샷은 gitignore라 worktree에는 없다. 없으면 메인 트리(git worktree 본체)에서 읽는다 — 읽기만 한다.
_REL = Path("PYQuant") / "data" / "bars_all_pit.parquet"
DATA = _ROOT / _REL
if not DATA.exists():
    DATA = _ROOT.parent / "Quant" / _REL

# 구간은 13_trendx_gate와 같은 날짜(06_bear_market 정의). 2022bear는 홀드아웃.
PERIODS = {
    "2020covid":    ("2020-01-02", "2020-12-30"),
    "2022bear":     ("2022-01-03", "2022-12-29"),
    "2024blackmon": ("2023-12-28", "2024-12-30"),
    "2026now":      ("2025-12-30", "2026-08-04"),
}

# ── config_dev_paper.json TRENDX 블록에서 그대로 옮긴 값 ─────────────────────────
POOL_N = 500            # 날짜별 20일 평균 거래대금 상위(시점정합 풀, 13_trendx_gate와 같은 정의)
ENTRY_LOWER = 5.0       # entry_lower_pct
ENTRY_UPPER = 35.0      # entry_upper_pct
ZONE_HYST = 4.0         # zone_hyst_pct — 유지 밴드는 [lower-hyst, upper+hyst]
STOP_PCT = 2.5          # stop_loss_pct (평단=진입가, 물타기 없음 buy_rungs=0)
TP_PCT = 3.0            # dev_sell_pct, sell_base_average=true
SCORE_TOP_N = 25        # score_top_n
W_TREND, W_PULL, W_VOL, W_LIQ = 1.0, 1.0, 0.5, 0.7   # score_w_* (w_supply 미적용)
Z_CLIP = 2.0            # 횡단면 z ±2σ 클립(UniverseScanner.score_cross_section)

COST_PCT = 0.31         # 왕복 비용 — PYQuant/backtest/metrics.py ROUNDTRIP_COST_PCT, 13_trendx_gate와 같은 값
R_DENOM = 6.0           # R 분모(%) — 13_trendx_gate와 같게 둬 두 스터디 수치를 나란히 읽게 한다
ATR_N = 14

HOLD_BAND_LO = ENTRY_LOWER - ZONE_HYST     # 1.0
HOLD_BAND_HI = ENTRY_UPPER + ZONE_HYST     # 39.0

VARIANTS = {
    "V1_current":   "aligned_prev AND band (현행)",
    "V2_or_today":  "(aligned_prev OR aligned_today) AND band (당일봉도 허용)",
    "V3_band_only": "band 단독 (이격 기준만)",
    "V4_today":     "aligned_today AND band (느린 축을 빠른 축으로 교체, D-033 되돌림)",
}


def krx_tick(px: float) -> float:
    """KRX 호가단위(2023-01 개편 기준). 13_trendx_gate와 같은 표."""
    for lim, t in ((2000, 1), (5000, 5), (20000, 10), (50000, 50), (200000, 100), (500000, 500)):
        if px < lim:
            return float(t)
    return 1000.0


def load_bars() -> pd.DataFrame:
    d = pd.read_parquet(DATA, columns=["Date", "code", "Open", "High", "Low", "Close", "Volume", "delisted"])
    d = d.sort_values(["code", "Date"]).reset_index(drop=True)
    for c in ("Open", "High", "Low", "Close"):
        d[c] = d[c].astype(float)
    return d


def add_features(d: pd.DataFrame) -> pd.DataFrame:
    g = d.groupby("code", sort=False)
    close = d["Close"]
    for n in (5, 10, 20, 60):
        d[f"sma{n}"] = g["Close"].transform(lambda s, n=n: s.rolling(n).mean())
    d["aligned_today"] = (d["sma5"] > d["sma10"]) & (d["sma10"] > d["sma20"]) & (d["sma20"] > d["sma60"])
    # aligned_hold = 전일 확정 SMA 정배열. 오늘 종가를 접지 않은 값이므로 한 칸 민다.
    d["aligned_prev"] = g["aligned_today"].shift(1).fillna(False).astype(bool)
    d["pull_pct"] = (close - d["sma20"]) / d["sma20"] * 100.0
    d["band_entry"] = (d["pull_pct"] >= ENTRY_LOWER) & (d["pull_pct"] <= ENTRY_UPPER)
    d["band_hold"] = (d["pull_pct"] >= HOLD_BAND_LO) & (d["pull_pct"] <= HOLD_BAND_HI)
    d["turnover"] = close * d["Volume"]
    d["tv20"] = g["turnover"].transform(lambda s: s.rolling(20).mean())
    rank = d.groupby("Date")["tv20"].rank(ascending=False, method="first")
    d["in_pool"] = rank <= POOL_N
    # 점수 입력: 추세·눌림·변동성(ATR%)·유동성
    tr = pd.concat([(d["High"] - d["Low"]),
                    (d["High"] - g["Close"].shift(1)).abs(),
                    (d["Low"] - g["Close"].shift(1)).abs()], axis=1).max(axis=1)
    d["_tr"] = tr
    d["atr_pct"] = d.groupby("code", sort=False)["_tr"].transform(
        lambda s: s.rolling(ATR_N).mean()) / close * 100.0
    d.drop(columns=["_tr"], inplace=True)
    d["trend"] = (d["sma5"] - d["sma60"]) / d["sma60"]
    return d


def add_score_rank(d: pd.DataFrame) -> pd.DataFrame:
    """진입 게이트를 통과한 종목만 모아 그날의 횡단면 z 점수·순위를 만든다(UniverseScanner와 같은 식)."""
    passed = d["in_pool"] & d["aligned_today"] & d["band_entry"] & d["atr_pct"].notna() & (d["turnover"] > 0)
    d["score"] = np.nan
    d["score_rank"] = np.nan
    sub = d.loc[passed, ["Date", "trend", "pull_pct", "atr_pct", "turnover"]].copy()
    sub["logtv"] = np.log(sub["turnover"])

    def zc(s: pd.Series) -> pd.Series:
        sd = s.std(ddof=0)
        if not np.isfinite(sd) or sd == 0.0:
            return pd.Series(0.0, index=s.index)
        return ((s - s.mean()) / sd).clip(-Z_CLIP, Z_CLIP)

    gb = sub.groupby("Date", sort=False)
    zt = gb["trend"].transform(zc)
    zp = gb["pull_pct"].transform(zc)          # 추세확장 슬리브는 전부 양수 → 부호 뒤집어 "덜 벌어진 쪽 우대"
    zv = gb["atr_pct"].transform(zc)
    zl = gb["logtv"].transform(zc)
    sub["score"] = W_TREND * zt - W_PULL * zp - W_VOL * zv + W_LIQ * zl
    d.loc[sub.index, "score"] = sub["score"]
    d.loc[sub.index, "score_rank"] = sub.groupby("Date", sort=False)["score"].rank(
        ascending=False, method="first")
    return d


def hold_mask(sub: pd.DataFrame, variant: str) -> np.ndarray:
    band = sub["band_hold"].to_numpy()
    ap = sub["aligned_prev"].to_numpy()
    at = sub["aligned_today"].to_numpy()
    if variant == "V1_current":
        return ap & band
    if variant == "V2_or_today":
        return (ap | at) & band
    if variant == "V3_band_only":
        return band
    if variant == "V4_today":
        return at & band
    raise ValueError(variant)


def walk_code(dates, o, h, l, c, entry_sig, hold_ok, bracket: bool = True,
              tp_first: bool = False) -> list[dict]:
    """한 종목을 하루씩 앞으로만 훑는다. 진입은 신호 다음날 시가, 재진입은 청산 다음날부터.

    하루 안 우선순위는 13_trendx_gate와 같다: 갭(시가≤손절가) → 손절(저가≤손절가) → 익절(고가≥목표+1틱)
    → 존 이탈(그날 종가). 존 이탈을 맨 뒤에 둔 것은 일봉에서 장중 순서를 못 가리기 때문이고,
    네 변형 모두 같은 규칙이라 비교에는 같은 방향으로 들어간다.
    """
    n = len(c)
    out: list[dict] = []
    pos = False
    entry_px = stop_px = tp_px = tp_trig = 0.0
    entry_i = 0

    for i in range(n):
        exited = False
        if pos:
            why, px = None, None
            if bracket and tp_first and h[i] >= tp_trig and o[i] > stop_px:
                why, px = "tp", tp_px
            elif bracket and o[i] <= stop_px:
                why, px = "gap", o[i]
            elif bracket and l[i] <= stop_px:
                why, px = "stop", stop_px
            elif bracket and h[i] >= tp_trig:
                why, px = "tp", tp_px
            elif not hold_ok[i]:
                why, px = "zone", c[i]
            if why is not None:
                out.append({"entry_date": dates[entry_i], "exit_date": dates[i], "entry": entry_px,
                            "exit_px": px, "hold_days": i - entry_i + 1, "why": why})
                pos, exited = False, True

        if (not pos) and (not exited) and i >= 1 and entry_sig[i - 1] and np.isfinite(o[i]) and o[i] > 0:
            entry_px = o[i]
            entry_i = i
            stop_px = entry_px * (1.0 - STOP_PCT / 100.0)
            tp_px = entry_px * (1.0 + TP_PCT / 100.0)
            tp_trig = tp_px + krx_tick(tp_px)
            pos = True
            # 진입 당일도 청산 판정을 받는다(갭은 진입가 자신이라 제외).
            why, px = None, None
            if bracket and tp_first and h[i] >= tp_trig:
                why, px = "tp", tp_px
            elif bracket and l[i] <= stop_px:
                why, px = "stop", stop_px
            elif bracket and h[i] >= tp_trig:
                why, px = "tp", tp_px
            elif not hold_ok[i]:
                why, px = "zone", c[i]
            if why is not None:
                out.append({"entry_date": dates[i], "exit_date": dates[i], "entry": entry_px,
                            "exit_px": px, "hold_days": 1, "why": why})
                pos = False

    if pos:
        out.append({"entry_date": dates[entry_i], "exit_date": dates[n - 1], "entry": entry_px,
                    "exit_px": c[n - 1], "hold_days": n - entry_i, "why": "trunc"})
    return out


def run_period(d: pd.DataFrame, variant: str, use_score_top: bool,
               bracket: bool = True, tp_first: bool = False) -> pd.DataFrame:
    rows = []
    for code, idx in d.groupby("code", sort=False).indices.items():
        sub = d.iloc[idx]
        sig = (sub["in_pool"] & sub["aligned_today"] & sub["band_entry"]).to_numpy()
        if use_score_top:
            sig = sig & (sub["score_rank"].to_numpy() <= SCORE_TOP_N)
        if not sig.any():
            continue
        trades = walk_code(sub["Date"].to_numpy(), sub["Open"].to_numpy(), sub["High"].to_numpy(),
                           sub["Low"].to_numpy(), sub["Close"].to_numpy(), sig, hold_mask(sub, variant),
                           bracket=bracket, tp_first=tp_first)
        for t in trades:
            t["code"] = code
            rows.append(t)
    if not rows:
        return pd.DataFrame(columns=["code", "entry_date", "exit_date", "entry", "exit_px",
                                     "hold_days", "why", "gross", "net", "r"])
    out = pd.DataFrame(rows)
    out["gross"] = (out["exit_px"] / out["entry"] - 1.0) * 100.0
    out["net"] = out["gross"] - COST_PCT
    out["r"] = out["net"] / R_DENOM
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-score-top", action="store_true", help="점수 상위 25 필터 없이(감도)")
    ap.add_argument("--no-bracket", action="store_true",
                    help="손절·익절을 끄고 유지 게이트만으로 청산(유지 축 분리)")
    ap.add_argument("--tp-first", action="store_true",
                    help="같은 날 손절·익절이 둘 다 닿으면 익절 우선(최선 가정 감도)")
    args = ap.parse_args()
    use_score_top = not args.no_score_top
    bracket, tp_first = not args.no_bracket, args.tp_first
    suffix = ("" if use_score_top else "_notop") + ("_nobracket" if not bracket else "") \
        + ("_tpfirst" if tp_first else "")

    t0 = time.time()
    print(f"[load] {DATA}")
    bars = load_bars()
    bars = add_features(bars)
    if use_score_top:
        bars = add_score_rank(bars)
    print(f"[load] {len(bars):,}행 {bars.code.nunique()}종목 {time.time()-t0:.1f}s")

    res_rows, paired_rows = [], []
    all_trades: dict[str, list[pd.DataFrame]] = {v: [] for v in VARIANTS}

    for pname, (lo, hi) in PERIODS.items():
        win = bars[(bars["Date"] >= lo) & (bars["Date"] <= hi)].copy()
        per_variant = {}
        for variant in VARIANTS:
            tr = run_period(win, variant, use_score_top, bracket=bracket, tp_first=tp_first)
            tr["period"] = pname
            tr["variant"] = variant
            per_variant[variant] = tr
            all_trades[variant].append(tr)
            why = tr["why"].value_counts().to_dict() if len(tr) else {}
            res_rows.append({
                "period": pname, "variant": variant, "trades": len(tr),
                "net_mean": tr["net"].mean() if len(tr) else np.nan,
                "r_mean": tr["r"].mean() if len(tr) else np.nan,
                "win_rate": (tr["net"] > 0).mean() if len(tr) else np.nan,
                "hold_days_mean": tr["hold_days"].mean() if len(tr) else np.nan,
                "cost_total_pct": len(tr) * COST_PCT,
                "sum_net": tr["net"].sum() if len(tr) else np.nan,
                "ex_zone": why.get("zone", 0), "ex_stop": why.get("stop", 0),
                "ex_gap": why.get("gap", 0), "ex_tp": why.get("tp", 0), "ex_trunc": why.get("trunc", 0),
            })
            print(f"  {pname:12s} {variant:13s} 거래 {len(tr):6d} 평균순손익 "
                  f"{tr['net'].mean() if len(tr) else float('nan'):+.3f}% 평균보유 "
                  f"{tr['hold_days'].mean() if len(tr) else float('nan'):.1f}일")

        # 짝지은 비교 — 같은 (종목, 진입일)에 두 변형이 모두 진입한 거래만 차이를 낸다.
        base = per_variant["V1_current"]
        for variant in ("V2_or_today", "V3_band_only", "V4_today"):
            alt = per_variant[variant]
            if not len(base) or not len(alt):
                continue
            key = ["code", "entry_date"]
            m = base.merge(alt, on=key, suffixes=("_v1", "_alt"))
            m = m.drop_duplicates(subset=key)
            m["diff"] = m["net_alt"] - m["net_v1"]
            diff = m["diff"].to_numpy()
            # 거래끼리는 같은 날 횡단면으로 묶여 독립이 아니다. 판정은 진입일별 평균차의
            #  일 시계열에 1표본 t로 한다(거래 단위 t는 참고로만 남긴다). 13_trendx_gate 감사와 같은 취지.
            daily = m.groupby("entry_date")["diff"].mean().to_numpy()
            _, t_tr, p_tr, _ = one_sample_t(diff) if len(diff) > 1 else (np.nan,) * 4
            _, t_d, p_d, n_d = one_sample_t(daily) if len(daily) > 1 else (np.nan,) * 4
            paired_rows.append({
                "period": pname, "variant": variant, "paired_n": len(diff),
                "mean_diff_pct": float(np.mean(diff)) if len(diff) else np.nan,
                "mean_diff_r": float(np.mean(diff)) / R_DENOM if len(diff) else np.nan,
                "t_daily": t_d, "p_daily": p_d, "days": n_d,
                "t_trade": t_tr, "p_trade": p_tr,
                "alt_better": float((diff > 0).mean()) if len(diff) else np.nan,
                "same": float((diff == 0).mean()) if len(diff) else np.nan,
                "trades_v1": len(base), "trades_alt": len(alt),
            })

    res = pd.DataFrame(res_rows)
    res.to_csv(_HERE / f"results{suffix}.tsv", sep="\t", index=False, float_format="%.4f")
    pr = pd.DataFrame(paired_rows)
    pr.to_csv(_HERE / f"paired{suffix}.tsv", sep="\t", index=False, float_format="%.5f")
    for variant, parts in all_trades.items():
        pd.concat(parts).to_csv(_HERE / f"trades_{variant}{suffix}.csv.gz", index=False)

    meta = {
        "run_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "data": str(DATA), "data_sha1_16": hashlib.sha1(DATA.read_bytes()).hexdigest()[:16],
        "data_bytes": DATA.stat().st_size,
        "git_commit": subprocess.run(["git", "rev-parse", "HEAD"], cwd=_ROOT, capture_output=True,
                                     text=True, encoding="utf-8", errors="replace").stdout.strip(),
        "git_dirty": bool(subprocess.run(["git", "status", "--porcelain"], cwd=_ROOT, capture_output=True,
                                         text=True, encoding="utf-8", errors="replace").stdout.strip()),
        "seed": "없음 — 난수 미사용, 전부 결정론",
        "params": {"POOL_N": POOL_N, "ENTRY": [ENTRY_LOWER, ENTRY_UPPER], "HOLD_BAND": [HOLD_BAND_LO, HOLD_BAND_HI],
                   "STOP_PCT": STOP_PCT, "TP_PCT": TP_PCT, "COST_PCT": COST_PCT, "R_DENOM": R_DENOM,
                   "SCORE_TOP_N": SCORE_TOP_N if use_score_top else None,
                   "score_w": [W_TREND, W_PULL, W_VOL, W_LIQ],
                   "bracket": bracket, "tp_first": tp_first},
        "periods": PERIODS, "variants": VARIANTS,
        "cmd": f"py research/studies/14_hold_axis/run_hold_axis.py{' --no-score-top' if not use_score_top else ''}",
        "elapsed_sec": round(time.time() - t0, 1),
    }
    (_HERE / f"run_meta{suffix}.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n[done] {time.time()-t0:.1f}s → results{suffix}.tsv / paired{suffix}.tsv")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
