"""스터디 38 k 구조 표 — k 마다 확정 지연, 종목·해당 저점 수, 손절선(마지막 확정 저점) 거리.

정의(스터디 37 swing_definitions 그대로): 저점 i 는 i−k .. i+k 중 최저(왼쪽 엄격, 오른쪽 같아도 됨), i+k 행 종가 뒤부터 쓴다.
열:
  confirm_delay_rows          확정까지 거래일 수(정의상 k)
  confirm_delay_calendar_*    저점 날부터 확정 날까지 달력 일수 중앙값·평균
  lows_per_stock_year         2010-01-01 – 2026-10-08 에 확정된 저점 수 ÷ 종목·연(거래일 수 ÷ 그해 시장 거래일 수 합)
  up_days                     상승 추세 종목일 수(2010 이후)
  median_stop_distance        상승 추세 날 (마지막 확정 저점 ÷ 종가 − 1) × 100 중앙값, p10·p90 같이
매매 수·수익은 trade_test.py 의 rule_split.csv 를 k 로 묶어 붙인다(results/k_table.csv).
재실행: py research/studies/38_trend_support_realtime/measure_structure.py   (trade_test.py 뒤)
"""
from __future__ import annotations

import numpy as np
import pandas as pd

import common

RESULTS = common.STUDY / "results"


def main() -> None:
    RESULTS.mkdir(exist_ok=True)
    panel = common.Panel()
    market_days_by_year = pd.Series(panel.market_dates // 10000).value_counts()
    rows = []

    for span in common.K_VALUES:
        state, latest_low, pivots_by_stock = panel.trend(span)
        delays = []
        lows = 0

        for stock, pivots in enumerate(pivots_by_stock):
            first = panel.starts[stock]
            dates = panel.dates[first:panel.starts[stock + 1]]
            confirm = pivots["low_confirm_rows"]
            usable = confirm < len(dates)
            low_rows = pivots["low_rows"][usable]
            confirm = confirm[usable]
            chosen = (dates[confirm] >= common.SIGNAL_FROM) & (dates[confirm] <= common.LAST_DATE)
            lows += int(chosen.sum())
            low_dates = pd.to_datetime(dates[low_rows[chosen]].astype(str))
            confirm_dates = pd.to_datetime(dates[confirm[chosen]].astype(str))
            delays.append((confirm_dates - low_dates).days.to_numpy())

        delays = np.concatenate(delays)
        signal_rows = panel.signal_ok
        stock_years = 0.0

        for year, days in market_days_by_year.items():
            if common.SIGNAL_FROM // 10000 <= year <= common.LAST_DATE // 10000:
                stock_years += ((panel.years == year) & signal_rows).sum() / days

        up = signal_rows & (state == 1)

        with np.errstate(invalid="ignore", divide="ignore"):
            distance = (latest_low[up] / panel.close[up] - 1.0) * 100.0

        distance = distance[np.isfinite(distance)]
        rows.append({"k": span, "confirm_delay_rows": span, "confirm_delay_calendar_median": float(np.median(delays)),
                     "confirm_delay_calendar_mean": float(delays.mean()), "confirmed_lows": lows, "stock_years": stock_years,
                     "lows_per_stock_year": lows / stock_years, "up_days": int(up.sum()),
                     "down_days": int((signal_rows & (state == -1)).sum()),
                     "median_stop_distance": float(np.median(distance)), "p10_stop_distance": float(np.percentile(distance, 10)),
                     "p90_stop_distance": float(np.percentile(distance, 90))})
        print(f"k={span} 끝", flush=True)

    structure = pd.DataFrame(rows)
    common.write_csv(RESULTS / "k_structure.csv", structure)
    split_path = RESULTS / "rule_split.csv"

    if split_path.exists():
        split = pd.read_csv(split_path)
        grouped = split.groupby(["k", "rule"], as_index=False)[["n", "sum_net", "wins"]].sum()
        grouped["mean_net"] = grouped["sum_net"] / grouped["n"]
        grouped["win_rate"] = grouped["wins"] / grouped["n"] * 100.0
        wide = grouped.pivot(index="k", columns="rule", values=["n", "mean_net"])
        wide.columns = [f"{measure}_{rule}" for measure, rule in wide.columns]
        common.write_csv(RESULTS / "k_table.csv", structure.merge(wide.reset_index(), on="k"))


if __name__ == "__main__":
    main()
