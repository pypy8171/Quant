"""스터디 42-3 — 42-2에서 나쁜 건이 몰린 묶음을 빼는 매수 거름을 하나씩 더해 본다.

문턱은 42-2 w20 breakout의 20% 단위 경계(분위)를 그대로 쓴다(손으로 고른 값 아님). 같은 문턱을 w10·retest에도 그대로 건다.
산출: data/filter_steps.csv. 사용: py research/studies/42_swing_trend/filter_rule.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import daily_stop_grid as grid  # noqa: E402
from feature_buckets import describe  # noqa: E402

# (이름, 조건) — 42-2 w20 breakout 다섯 묶음 중 건당 마이너스·이익/손실 1 미만이었던 끝 묶음을 뺀다.
STEPS = [
    ("1년 고점 대비 81% 이상(멀리 떨어진 바닥 돌파 제외)", lambda f: f["year_high"] >= 0.809),
    ("240일선 위 1.7–32%(240일선 아래·너무 뜬 것 제외)", lambda f: f["above240"].between(0.0169, 0.32)),
    ("직전 20일 상승 26% 이하(이미 급등한 것 제외)", lambda f: f["run20"] <= 0.264),
    ("하루 등락폭(ATR) 5.9% 이하(변동 큰 것 제외)", lambda f: f["atr_percent"] <= 5.9),
    ("앞 고점보다 14.7% 넘게 낮은 고점 돌파 제외", lambda f: f["high_rise"] >= -0.147),
    ("손절선까지 31% 이하", lambda f: f["stop_gap"] <= 0.309),
]


def main() -> None:
    dates = grid.Panel().dates
    rows = []

    for path in sorted((HERE / "data").glob("trades_w*.parquet")):
        trades = pd.read_parquet(path)
        trades = trades[~trades["data_error"]]
        years = pd.Series(dates.year.to_numpy()[trades["entry_t"].to_numpy()], index=trades.index)
        top = trades["net"] >= trades["net"].quantile(0.99)
        keep = pd.Series(True, index=trades.index)
        rows.append({"rule": path.stem[7:], "step": "(거름 없음)", **describe(trades["net"], years),
                     "top1_kept": 100.0, "no_top1": trades.loc[~top, "net"].mean()})

        for label, condition in STEPS:
            keep &= condition(trades).fillna(False)
            part = trades[keep]
            ordered = part["net"].sort_values()
            rows.append({"rule": path.stem[7:], "step": label, **describe(part["net"], years[keep]),
                         "top1_kept": top[keep].sum() / top.sum() * 100,
                         "no_top1": ordered.iloc[:int(len(ordered) * 0.99)].mean()})

    frame = pd.DataFrame(rows)
    frame.to_csv(HERE / "data" / "filter_steps.csv", index=False)
    pd.set_option("display.width", 250)
    print(frame.round(2).to_string(index=False))


if __name__ == "__main__":
    main()
