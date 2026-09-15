# -*- coding: utf-8 -*-
"""15. 임펄스 후 눌림 추격 — 사전등록(README §1) 그대로의 이벤트 백테스트 하네스.

사전등록을 코드로 옮긴 것이고, 결과를 보고 임계값을 바꾸지 않는다.
밴드 민감도는 §4 스윕(20~40 / 30~55 / 40~65)으로만 표현한다.

룩어헤드 삼중차단
  1) 신호는 t일 종가까지의 값만 본다 — 40일 창(t 포함)의 고가·종가, SMA20/60[t],
     거래량비는 vol_ma20.shift(1) 기준, 거래대금 20일 평균[t].
  2) 진입은 t+1 시가, 청산은 t+H 종가. 지수도 같은 날짜·같은 방식으로 맞춘다.
  3) 전역통계(전체기간 mean/std/quantile) 미사용 — 롤링 창과 고정 상수만 쓴다.
     유니버스도 평가일 t 이하 최신 PIT 단면만 쓴다(정적 리스트 금지).

재현
  py research/studies/15_impulse_pullback/fetch_pit_panel.py   # 입력 준비(1회)
  py research/studies/15_impulse_pullback/run_impulse_pullback.py
  seed=15(부트스트랩 전용). 그 외 난수 없음 — 같은 입력이면 같은 숫자가 나온다.
"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
OUT = HERE / "out"
OUT.mkdir(exist_ok=True)
SEED = 15
rng = np.random.default_rng(SEED)

# ── 사전등록 상수 (README §1). 결과를 보고 고치지 않는다 ────────────────────
LOOKBACK = 40            # 스윙 탐색 창(거래일, t 포함)
MIN_MOVE = 0.15          # 임펄스 최소 상승폭
VOL_MULT = 2.5           # 거래량 확인 배수 (volume / vol_ma20.shift(1))
MA_TOL = 0.05            # SMA20 > SMA60 * (1 - tol)
MIN_TURNOVER20 = 5e8     # 20일 평균 거래대금 하한
MIN_CLOSE = 1000         # 종가 하한
COOLDOWN = 20            # 같은 종목 재신호 쿨다운(거래일)
HORIZONS = [1, 3, 5, 10]  # 보유 거래일 (진입 t+1 시가 → 청산 t+H 종가)
COSTS = [0.0021, 0.005, 0.010]   # 왕복 비용 감도
BANDS = {"20~40%": (0.20, 0.40), "30~55%": (0.30, 0.55), "40~65%": (0.40, 0.65)}
BASE_BAND = "30~55%"     # 사전등록 기본 밴드
PERIOD = ("2020-01-01", "2026-09-01")
HOLDOUT_YEAR = 2022      # 2022는 잠금 — train은 2022 제외


# ── 입력 로드 ────────────────────────────────────────────────────────────────
def load_inputs():
    px = pd.read_parquet(RAW / "px.parquet")
    px["date"] = pd.to_datetime(px["date"])
    univ = pd.read_csv(RAW / "pit_universe.csv", dtype={"code": str})
    univ["snapshot_date"] = pd.to_datetime(univ["snapshot_date"])
    idx = {}
    for f, m in (("KS11.csv", "KOSPI"), ("KQ11.csv", "KOSDAQ")):
        d = pd.read_csv(RAW / f, parse_dates=["date"]).sort_values("date").reset_index(drop=True)
        idx[m] = {"open": dict(zip(d["date"], d["open"])), "close": dict(zip(d["date"], d["close"]))}
    return px, univ, idx


def pit_membership(univ: pd.DataFrame):
    """평가일 t → (t 이하 최신 스냅샷의) {code: market}. 미래 단면은 절대 쓰지 않는다."""
    by_snap = {np.datetime64(pd.Timestamp(s), "ns"): dict(zip(g["code"], g["market"]))
               for s, g in univ.groupby("snapshot_date")}
    snaps = np.array(sorted(by_snap), dtype="datetime64[ns]")
    return snaps, by_snap


# ── 사건 후보 스캔 (밴드 무관 부분까지 한 번에) ──────────────────────────────
def scan_candidates(px, snaps, by_snap):
    """밴드 조건을 제외한 사전등록 1~7번을 만족하는 후보 + retrace_ratio를 돌려준다.

    밴드(5번)와 쿨다운(8번)은 밴드마다 달라지므로 뒤에서 건다.
    """
    rows = []
    start, end = pd.Timestamp(PERIOD[0]), pd.Timestamp(PERIOD[1])
    for code, d in px.groupby("code", sort=True):
        d = d.sort_values("date")
        if len(d) < LOOKBACK + 60 + 2:
            continue
        dates = d["date"].values
        high = d["high"].to_numpy(float)
        close = d["close"].to_numpy(float)
        openp = d["open"].to_numpy(float)
        vol = d["volume"].to_numpy(float)
        n = len(close)

        cs = pd.Series(close)
        sma20 = cs.rolling(20).mean().to_numpy()
        sma60 = cs.rolling(60).mean().to_numpy()
        volma20_prev = pd.Series(vol).rolling(20).mean().shift(1).to_numpy()
        with np.errstate(divide="ignore", invalid="ignore"):
            volratio = vol / volma20_prev
        turnover20 = (cs * pd.Series(vol)).rolling(20).mean().to_numpy()

        # 밴드와 무관한 값싼 필터부터 — t 시점 정보만 쓴다
        ok = np.zeros(n, bool)
        lo_i = max(LOOKBACK - 1, 60)
        ok[lo_i:n - 1] = True                       # t+1 시가가 있어야 진입 가능
        ok &= (sma20 > sma60 * (1 - MA_TOL))
        ok &= (turnover20 >= MIN_TURNOVER20)
        ok &= (close >= MIN_CLOSE)
        ok &= np.isfinite(sma60)
        dts = pd.DatetimeIndex(dates)
        ok &= np.asarray(dts >= start) & np.asarray(dts <= end)
        cand = np.flatnonzero(ok)
        if len(cand) == 0:
            continue

        # PIT 유니버스 편입 여부 (t 이하 최신 단면)
        si = np.searchsorted(snaps, dates[cand], side="right") - 1
        keep, markets = [], []
        for pos, s in zip(cand, si):
            if s < 0:
                continue
            mk = by_snap[snaps[s]].get(code)
            if mk in ("KOSPI", "KOSDAQ"):
                keep.append(pos)
                markets.append(mk)
        if not keep:
            continue

        for t, mk in zip(keep, markets):
            w0 = t - LOOKBACK + 1
            hseg = high[w0:t + 1]
            hd = w0 + int(np.argmax(hseg))
            if hd <= w0:                                  # 고점이 창 시작 → 앞선 저점 없음
                continue
            ld = w0 + int(np.argmin(close[w0:hd]))        # 창시작~고점 이전 구간의 종가 최저
            swing_high, swing_low = high[hd], close[ld]
            if not np.isfinite(swing_low) or swing_low <= 0:
                continue
            move = swing_high - swing_low
            if move <= 0 or move / swing_low < MIN_MOVE:  # 3) 임펄스 최소 상승폭
                continue
            vseg = volratio[ld:hd + 1]                    # 4) 거래량 확인
            if not np.any(np.isfinite(vseg) & (vseg >= VOL_MULT)):
                continue
            retrace = (swing_high - close[t]) / move      # 5) 밴드는 뒤에서
            if not np.isfinite(retrace):
                continue
            rows.append({
                "code": code, "market": mk, "sig_date": dates[t], "sig_idx": t,
                "retrace": retrace, "move_pct": move / swing_low,
                "impulse_days": hd - ld, "days_since_high": t - hd,
                "close_t": close[t], "entry_open": openp[t + 1],
                "entry_gap": openp[t + 1] / close[t] - 1,
                "turnover20": turnover20[t], "vol_peak": float(np.nanmax(vseg)),
                "sma_gap": sma20[t] / sma60[t] - 1,
            })
    return pd.DataFrame(rows)


def attach_returns(ev, px, idx):
    """진입 t+1 시가 → 청산 t+H 종가. 지수도 같은 날짜로 맞춰 초과수익을 만든다."""
    bars = {c: g.sort_values("date").reset_index(drop=True) for c, g in px.groupby("code", sort=True)}
    recs = []
    for r in ev.itertuples():
        d = bars[r.code]
        i = r.sig_idx
        entry = d["open"].iloc[i + 1]
        entry_date = d["date"].iloc[i + 1]
        if not np.isfinite(entry) or entry <= 0:
            continue
        ib = idx[r.market]
        b_entry = ib["open"].get(entry_date)
        if b_entry is None or not np.isfinite(b_entry) or b_entry <= 0:
            continue
        rec = {"exit_ok": True}
        for h in HORIZONS:
            j = i + h
            if j >= len(d):
                rec[f"r{h}"] = np.nan
                rec[f"x{h}"] = np.nan
                continue
            exit_date = d["date"].iloc[j]
            b_exit = ib["close"].get(exit_date)
            stock = d["close"].iloc[j] / entry - 1
            rec[f"r{h}"] = stock
            rec[f"b{h}"] = (b_exit / b_entry - 1) if b_exit else np.nan
            rec[f"x{h}"] = stock - rec[f"b{h}"] if b_exit else np.nan
            rec[f"exit_date{h}"] = exit_date
        rec.update(code=r.code, market=r.market, sig_date=r.sig_date, sig_idx=r.sig_idx,
                   entry_date=entry_date, entry=entry, retrace=r.retrace,
                   move_pct=r.move_pct, impulse_days=r.impulse_days,
                   days_since_high=r.days_since_high, entry_gap=r.entry_gap,
                   turnover20=r.turnover20, vol_peak=r.vol_peak, sma_gap=r.sma_gap)
        recs.append(rec)
    out = pd.DataFrame(recs)
    if len(out):
        out["year"] = pd.to_datetime(out["sig_date"]).dt.year
        out["sample"] = np.where(out["year"] == HOLDOUT_YEAR, "holdout2022", "train")
    return out


def apply_band_and_cooldown(cand, lo, hi):
    """밴드(5번) → 같은 종목 20거래일 쿨다운(8번). 시간순으로만 훑는다."""
    sel = cand[(cand["retrace"] >= lo) & (cand["retrace"] <= hi)].sort_values(["code", "sig_idx"])
    keep = []
    last = {}
    for r in sel.itertuples():
        if r.sig_idx - last.get(r.code, -10**9) >= COOLDOWN:
            keep.append(r.Index)
            last[r.code] = r.sig_idx
    return cand.loc[keep].sort_values(["sig_date", "code"]).reset_index(drop=True)


# ── 통계 ─────────────────────────────────────────────────────────────────────
def stats(x, dates=None):
    x = np.asarray(x, float)
    m = np.isfinite(x)
    x = x[m]
    n = len(x)
    if n < 2:
        return dict(n=n, mean=np.nan, median=np.nan, win=np.nan, t=np.nan,
                    ci_lo=np.nan, ci_hi=np.nan, t_clu=np.nan)
    se = x.std(ddof=1) / np.sqrt(n)
    t = x.mean() / se if se > 0 else np.nan
    if n <= 100_000:     # 부트스트랩 95% 구간(seed 고정 → 재실행 동일)
        bs = rng.choice(x, size=(4000, n), replace=True).mean(axis=1)
        lo, hi = np.percentile(bs, [2.5, 97.5])
    else:                # 표본이 10만 초과면 메모리상 정규근사로 대체(CLT)
        lo, hi = x.mean() - 1.96 * se, x.mean() + 1.96 * se
    t_clu = np.nan
    if dates is not None:
        dd = np.asarray(dates)[m]
        g = pd.Series(x).groupby(pd.Series(dd)).mean().values   # 신호일 클러스터 평균
        if len(g) >= 2 and g.std(ddof=1) > 0:
            t_clu = g.mean() / (g.std(ddof=1) / np.sqrt(len(g)))
    return dict(n=n, mean=float(x.mean()), median=float(np.median(x)),
                win=float((x > 0).mean()), t=float(t), ci_lo=float(lo), ci_hi=float(hi),
                t_clu=float(t_clu))


def summarize(ev, label):
    rows = []
    for h in HORIZONS:
        for c in COSTS:
            for samp in ("전체", "train(2022제외)", "holdout2022"):
                s = ev if samp == "전체" else ev[ev["sample"] == ("train" if samp.startswith("train") else "holdout2022")]
                if len(s) == 0:
                    continue
                st = stats(s[f"x{h}"] - c, s["sig_date"])
                rows.append(dict(band=label, H=h, cost=c, sample=samp, **st))
    return pd.DataFrame(rows)


def fmt(df, cols=("band", "H", "cost", "sample", "n", "mean", "median", "win", "t", "ci_lo", "ci_hi", "t_clu")):
    d = df[[c for c in cols if c in df.columns]].copy()
    for c in ("mean", "median", "ci_lo", "ci_hi"):
        if c in d:
            d[c] = (d[c] * 100).round(2)
    for c in ("win",):
        if c in d:
            d[c] = (d[c] * 100).round(1)
    for c in ("t", "t_clu"):
        if c in d:
            d[c] = d[c].round(2)
    return d.to_string(index=False)


def main():
    px, univ, idx = load_inputs()
    snaps, by_snap = pit_membership(univ)
    print(f"[입력] 일봉 {len(px):,}행 / 코드 {px['code'].nunique()} / "
          f"PIT 단면 {len(snaps)}개 ({pd.Timestamp(snaps[0]).date()}~{pd.Timestamp(snaps[-1]).date()})", flush=True)

    cand = scan_candidates(px, snaps, by_snap)
    print(f"[후보] 밴드·쿨다운 전 조건(1~4,6,7) 통과 {len(cand):,}건 "
          f"/ 종목 {cand['code'].nunique()}개", flush=True)
    cand.to_csv(OUT / "candidates_prebands.csv", index=False, encoding="utf-8-sig")

    all_sum, all_ev = [], {}
    for label, (lo, hi) in BANDS.items():
        sel = apply_band_and_cooldown(cand, lo, hi)
        ev = attach_returns(sel, px, idx)
        all_ev[label] = ev
        ev.to_csv(OUT / f"events_{label.replace('~', '_').replace('%','')}.csv",
                  index=False, encoding="utf-8-sig")
        all_sum.append(summarize(ev, label))
        print(f"[밴드 {label}] 쿨다운 후 사건 {len(sel):,} → 수익계산 가능 {len(ev):,} "
              f"(train {int((ev['sample']=='train').sum())} / holdout2022 {int((ev['sample']=='holdout2022').sum())})",
              flush=True)

    S = pd.concat(all_sum, ignore_index=True)
    S.to_csv(OUT / "summary_sweep.csv", index=False, encoding="utf-8-sig")

    base = all_ev[BASE_BAND]
    print(f"\n=== §3 사전등록 기본 밴드 {BASE_BAND} — 비용 0.21% ===")
    print(fmt(S[(S["band"] == BASE_BAND) & (S["cost"] == 0.0021)]))
    print(f"\n=== 기본 밴드 {BASE_BAND} — 비용 감도(전체 표본) ===")
    print(fmt(S[(S["band"] == BASE_BAND) & (S["sample"] == "전체")]))
    print("\n=== §4 밴드 3종 스윕 (전체 표본, 비용 0.21%) ===")
    print(fmt(S[(S["cost"] == 0.0021) & (S["sample"] == "전체")]))
    print("\n=== §4 밴드 3종 × 홀드아웃 (비용 0.21%) ===")
    print(fmt(S[(S["cost"] == 0.0021) & (S["sample"] == "holdout2022")]))

    # 진단: 연도별·시장별·되돌림 구간별 분포 + 진입 갭
    diag = []
    for key, g in base.groupby("year"):
        st = stats(g["x5"] - 0.0021, g["sig_date"])
        diag.append(dict(축="연도", 값=int(key), **st))
    for key, g in base.groupby("market"):
        st = stats(g["x5"] - 0.0021, g["sig_date"])
        diag.append(dict(축="시장", 값=key, **st))
    if len(base) >= 10:
        b = base.assign(q=pd.qcut(base["retrace"], min(4, base["retrace"].nunique()), duplicates="drop"))
        for key, g in b.groupby("q", observed=True):
            st = stats(g["x5"] - 0.0021, g["sig_date"])
            diag.append(dict(축="되돌림4분위", 값=str(key), **st))
    D = pd.DataFrame(diag)
    D.to_csv(OUT / "diagnostics_base.csv", index=False, encoding="utf-8-sig")
    print("\n=== §5 진단 (기본 밴드, H=5, 비용 0.21%) ===")
    print(fmt(D, cols=("축", "값", "n", "mean", "median", "win", "t")))
    print(f"\n진입 갭(t+1 시가/ t 종가 - 1): 평균 {base['entry_gap'].mean()*100:+.2f}% / "
          f"+10% 이상 갭 {(base['entry_gap'] >= 0.10).mean()*100:.1f}% "
          f"(체결 난이도 — 사전등록 밖 진단)")

    # 수익곡선: 신호일별 동일가중 초과수익의 누적합 (H=5, 비용 0.21%)
    eq = (base.assign(x=base["x5"] - 0.0021).dropna(subset=["x"]).groupby("sig_date")["x"]
          .agg(["size", "mean"]).rename(columns={"size": "n", "mean": "일평균초과"}))
    eq["누적초과"] = eq["일평균초과"].cumsum()      # 청산 미완료일(NaN)은 제외하고 누적
    eq.to_csv(OUT / "equity_curve_base_h5.csv", encoding="utf-8-sig")

    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=HERE,
                                capture_output=True, text=True).stdout.strip()
    except Exception:
        commit = ""
    (OUT / "run_manifest.json").write_text(json.dumps({
        "seed": SEED, "period": PERIOD, "holdout_year": HOLDOUT_YEAR,
        "bands": BANDS, "horizons": HORIZONS, "costs": COSTS,
        "params": dict(lookback=LOOKBACK, min_move=MIN_MOVE, vol_mult=VOL_MULT,
                       ma_tol=MA_TOL, min_turnover20=MIN_TURNOVER20,
                       min_close=MIN_CLOSE, cooldown=COOLDOWN),
        "commit": commit,
        "n_events": {k: int(len(v)) for k, v in all_ev.items()},
        "inputs": json.loads((RAW / "fetch_manifest.json").read_text(encoding="utf-8")),
    }, ensure_ascii=False, indent=2, default=str), encoding="utf-8")
    print(f"\n[산출물] {OUT}")


if __name__ == "__main__":
    main()
