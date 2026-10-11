"""스터디 42-2 — 조건 값마다 매매 건을 크기 순 다섯 묶음으로 나눠 어느 묶음에 좋은 건이 몰리는지 본다.

문턱은 손으로 정하지 않고 자료의 20% 단위 경계(분위)로 나눈다. 자료 오류 건(data_error)은 뺀다.
묶음마다: 건수, 건당 평균, 중앙값, 이긴 비율, +50% 넘은 건 비율, 손실 건 평균, 이익 합 ÷ 손실 합, 플러스인 해.
산출: data/buckets.csv. 사용: py research/studies/42_swing_trend/feature_buckets.py (swing_trend.py 다음)
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import daily_stop_grid as grid  # noqa: E402

FEATURES = ["base_days", "base_depth", "low_rise", "high_rise", "year_high", "run20", "stop_gap", "market_trend",
            "relative60", "above240", "atr_percent", "cap_decile", "turnover20_decile"]


def describe(net: pd.Series, years: pd.Series) -> dict:
    gains, losses = net[net > 0].sum(), -net[net <= 0].sum()
    by_year = net.groupby(years).mean()
    return {"trades": len(net), "mean": net.mean(), "median": net.median(), "win": (net > 0).mean() * 100,
            "big50": (net > 50).mean() * 100, "loss_mean": net[net <= 0].mean(),
            "profit_factor": gains / losses if losses > 0 else np.nan,
            "years_plus": f"{int((by_year > 0).sum())}/{len(by_year)}"}


def main() -> None:
    dates = grid.Panel().dates
    rows = []

    for path in sorted((HERE / "data").glob("trades_w*.parquet")):
        trades = pd.read_parquet(path)
        trades = trades[~trades["data_error"]]
        years = pd.Series(dates.year.to_numpy()[trades["entry_t"].to_numpy()], index=trades.index)
        rule = path.stem.replace("trades_", "")
        rows.append({"rule": rule, "feature": "(전체)", "bucket": "", **describe(trades["net"], years)})

        for feature in FEATURES:
            values = trades[feature]
            buckets = pd.qcut(values.rank(method="first"), 5, labels=False)

            for bucket in range(5):
                part = buckets == bucket
                low, high = values[part].min(), values[part].max()
                rows.append({"rule": rule, "feature": feature, "bucket": f"{bucket + 1}: {low:.3g} – {high:.3g}",
                             **describe(trades.loc[part, "net"], years[part])})

    frame = pd.DataFrame(rows)
    frame.to_csv(HERE / "data" / "buckets.csv", index=False)
    pd.set_option("display.width", 250)
    pd.set_option("display.max_rows", 500)
    print(frame.round(2).to_string(index=False))


if __name__ == "__main__":
    main()
