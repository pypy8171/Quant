# -*- coding: utf-8 -*-
"""날짜맞춤 대조군 — 사건의 초과수익이 '사건 때문'인지 '유니버스 때문'인지 가른다.

사전등록 §1의 승격 기준은 지수 대비 초과수익이다. 그런데 동일가중 소형주 바스켓은
시가총액가중 지수 대비 평균적으로 밀리는 구간이 많다. 그래서 같은 날짜에 유동성
조건만 통과한 PIT 유니버스 전체의 평균 초과수익을 대조군으로 두고, 사건 평균에서
그 대조군을 뺀 값(사건 알파)을 함께 본다. 이건 사전등록 기준을 바꾸는 게 아니라
§5 진단이다 — 승격 판정은 그대로 지수 대비 초과수익으로 한다.

입력은 run_impulse_pullback.py와 같은 raw/, 출력은 out/.
"""
import sys
from pathlib import Path

import numpy as np
import pandas as pd

sys.stdout.reconfigure(encoding="utf-8")
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_impulse_pullback import (  # noqa: E402
    BANDS, BASE_BAND, COSTS, HORIZONS, MIN_CLOSE, MIN_TURNOVER20, OUT, PERIOD,
    RAW, load_inputs, pit_membership, stats, fmt,
)


def universe_forward(px, snaps, by_snap, idx):
    """유동성 조건을 통과한 PIT 유니버스 전 (code, t)의 전방 초과수익. 대조군 원장."""
    start, end = pd.Timestamp(PERIOD[0]), pd.Timestamp(PERIOD[1])
    parts = []
    for code, d in px.groupby("code", sort=True):
        d = d.sort_values("date")
        if len(d) < 80:
            continue
        dates = d["date"].to_numpy()
        close = d["close"].to_numpy(float)
        openp = d["open"].to_numpy(float)
        vol = d["volume"].to_numpy(float)
        n = len(close)
        turnover20 = (pd.Series(close) * pd.Series(vol)).rolling(20).mean().to_numpy()
        ok = np.zeros(n, bool)
        ok[20:n - 1] = True
        ok &= (turnover20 >= MIN_TURNOVER20) & (close >= MIN_CLOSE)
        dts = pd.DatetimeIndex(dates)
        ok &= np.asarray(dts >= start) & np.asarray(dts <= end)
        pos = np.flatnonzero(ok)
        if len(pos) == 0:
            continue
        si = np.searchsorted(snaps, dates[pos], side="right") - 1
        rows = []
        for t, s in zip(pos, si):
            if s < 0:
                continue
            mk = by_snap[snaps[s]].get(code)
            if mk not in ("KOSPI", "KOSDAQ"):
                continue
            entry, entry_date = openp[t + 1], dates[t + 1]
            b_entry = idx[mk]["open"].get(pd.Timestamp(entry_date))
            if not np.isfinite(entry) or entry <= 0 or not b_entry:
                continue
            r = {"code": code, "sig_date": dates[t], "market": mk}
            for h in HORIZONS:
                j = t + h
                if j >= n:
                    r[f"x{h}"] = np.nan
                    continue
                b_exit = idx[mk]["close"].get(pd.Timestamp(dates[j]))
                r[f"x{h}"] = (close[j] / entry - 1) - (b_exit / b_entry - 1) if b_exit else np.nan
            rows.append(r)
        if rows:
            parts.append(pd.DataFrame(rows))
    return pd.concat(parts, ignore_index=True)


def main():
    px, univ, idx = load_inputs()
    snaps, by_snap = pit_membership(univ)
    cache = OUT / "control_panel.parquet"
    if cache.exists():
        ctrl = pd.read_parquet(cache)
    else:
        ctrl = universe_forward(px, snaps, by_snap, idx)
        ctrl.to_parquet(cache, index=False)
    print(f"[대조군] 유동성 통과 (code,t) {len(ctrl):,}개 / 날짜 {ctrl['sig_date'].nunique()}일", flush=True)
    day = ctrl.groupby("sig_date")[[f"x{h}" for h in HORIZONS]].mean()
    day.to_csv(OUT / "control_daily_mean.csv", encoding="utf-8-sig")

    print("\n=== 대조군 자체(유니버스 동일가중 − 소속지수), 비용 0.21% ===")
    base_rows = []
    for h in HORIZONS:
        st = stats(ctrl[f"x{h}"] - 0.0021, ctrl["sig_date"])
        base_rows.append(dict(축="유니버스 전체", H=h, **st))
    print(fmt(pd.DataFrame(base_rows), cols=("축", "H", "n", "mean", "median", "win", "t", "t_clu")))

    out_rows = []
    for label in BANDS:
        f = OUT / f"events_{label.replace('~', '_').replace('%','')}.csv"
        ev = pd.read_csv(f, dtype={"code": str}, parse_dates=["sig_date"])
        ev = ev.merge(day.add_prefix("ctrl_"), left_on="sig_date", right_index=True, how="left")
        for h in HORIZONS:
            alpha = ev[f"x{h}"] - ev[f"ctrl_x{h}"]     # 같은 날 유니버스 평균 차감
            st = stats(alpha, ev["sig_date"])
            out_rows.append(dict(band=label, H=h, 기준="사건−같은날유니버스", **st))
        # 원본 events_*.csv는 건드리지 않는다(재실행 산출물이 실행 순서에 의존하면 안 된다)
        ev.to_csv(f.with_name(f.stem + "_with_control.csv"), index=False, encoding="utf-8-sig")
    A = pd.DataFrame(out_rows)
    A.to_csv(OUT / "control_matched_alpha.csv", index=False, encoding="utf-8-sig")
    print("\n=== 사건 알파 (같은 날짜 유니버스 평균 차감, 비용은 양쪽 상쇄되어 미적용) ===")
    print(fmt(A, cols=("band", "H", "기준", "n", "mean", "median", "win", "t", "ci_lo", "ci_hi", "t_clu")))


if __name__ == "__main__":
    main()
