"""스터디 38 무작위 진입 — 정규 근사판(시뮬레이션이 아님, random_test.py 1,000 회차가 46 시간 걸려 대신 내는 잠정 값).

뽑는 자리·매도 규칙·연도별 매매 수는 random_test.py 와 같다. 다른 점:
  - 같은 종목 보유 구간 겹침을 막지 않는다(복원 추출). 그래서 칸 평균의 분포를 회차로 돌리지 않고 식으로 낸다.
    그해 후보 행 전부의 순수익 평균 μ_y·분산 σ²_y 로 칸 c 의 무작위 평균 ~ 정규(Σ n_cy μ_y / n_c, Σ n_cy σ²_y / n_c²).
  - 칸 분위 = Φ((실제 − 평균) / 표준편차) × 100. 규칙·k·N 묶음은 칸 합의 평균·분산(칸끼리 독립)으로 같은 식.
  - 가장 좋은 칸 보정: 칸마다 위 정규에서 10,000 번 뽑아 회차마다 최고값을 잡은 분포에서 실제 최고 칸 평균의 분위.
한계: 순수익 분포 꼬리가 두꺼워 매매 수가 적은 칸은 정규 근사가 맞지 않는다. 겹침 제약이 없는 만큼 시뮬레이션과 다르다.
  k=5·N=20·E1 묶음은 random_test.py --measure --runs 1000 의 시뮬레이션 결과(random_measure_cells.parquet)와 칸마다 대조해
  results/random_approx_check.csv 로 남긴다.
산출: results/random_approx_cells.parquet, random_approx_rules.csv, random_approx_split.csv, random_approx_best.csv,
  random_approx_check.csv, random_approx_metrics.json
재실행: py research/studies/38_trend_support_realtime/random_approx.py   (trade_test.py 뒤)
"""
from __future__ import annotations

import time

import numpy as np
import pandas as pd
from math import erf, sqrt as square_root

import common
import random_test

RESULTS = common.STUDY / "results"
DRAWS = 10_000
POOL_NAMES = ("all_cells", "inner_cells")


def year_moments(net: np.ndarray, rows: np.ndarray, years: np.ndarray, year_count: int) -> tuple[np.ndarray, np.ndarray]:
    values = net[rows]
    counts = np.bincount(years, minlength=year_count).astype(np.float64)
    sums = np.bincount(years, weights=values, minlength=year_count)
    squares = np.bincount(years, weights=values * values, minlength=year_count)
    with np.errstate(invalid="ignore", divide="ignore"):
        mean = sums / counts
        variance = squares / counts - mean * mean
    return np.nan_to_num(mean), np.nan_to_num(np.maximum(variance, 0.0))


def percentile(actual, expected_sum, variance_sum, trade_count):
    with np.errstate(invalid="ignore", divide="ignore"):
        mean = expected_sum / trade_count
        standard_deviation = np.sqrt(variance_sum) / trade_count
        score = np.asarray((actual - mean) / standard_deviation, dtype=np.float64)
        cumulative_probability = 0.5 * (1.0 + np.vectorize(lambda value: erf(value / square_root(2.0)) if np.isfinite(value) else np.nan)(score))
        return mean, standard_deviation, cumulative_probability * 100.0


def main() -> None:
    started = time.time()
    combinations = pd.read_parquet(RESULTS / "combinations.parquet")
    up = combinations[combinations["side"] == "up"].reset_index(drop=True)
    del combinations
    year_columns = [column for column in up.columns if column.startswith("n_") and column[2:].isdigit()]
    panel = common.Panel()
    rows = np.flatnonzero(panel.signal_ok)
    years = panel.year_index[rows]
    random = np.random.default_rng(common.SEED)
    cell_frames, group_rows, split_rows = [], [], []
    rule_sums = {}
    all_maxima = np.full(DRAWS, -np.inf)
    rule_maxima = {rule: np.full(DRAWS, -np.inf) for rule in common.RULE_NAMES}

    for span in common.K_VALUES:
        state, latest_low, _ = panel.trend(span)

        for window_index, window in enumerate(common.WINDOWS):
            for rule_index, rule in enumerate(common.RULE_NAMES):
                _, net = random_test.exit_arrays(rule_index, panel.averages[window_index], panel.open_price, panel.high, panel.low,
                                                 panel.close, panel.starts, state, latest_low, panel.listed_at_end, common.COST)
                mean_y, variance_y = year_moments(net, rows, years, panel.year_count)
                part = up[(up["k"] == span) & (up["window"] == window) & (up["rule"] == rule)].reset_index(drop=True)
                counts = part[year_columns].to_numpy(np.float64)
                trade_count = counts.sum(axis=1)
                expected_sum = counts @ mean_y
                variance_sum = counts @ variance_y
                actual = part["mean_net"].to_numpy()
                mean, standard_deviation, cell_percentile = percentile(actual, expected_sum, variance_sum, trade_count)
                frame = part[["k", "window", "atr_period", "rule", "distance_bin", "bounce_bin", "n", "mean_net"]].copy()
                frame["random_mean"] = mean
                frame["random_sd"] = standard_deviation
                frame["percentile_vs_random"] = np.where(trade_count > 0, cell_percentile, np.nan)
                cell_frames.append(frame)
                has = (trade_count > 0) & np.isfinite(standard_deviation)
                group_max = np.full(DRAWS, -np.inf)

                for chunk in np.array_split(np.flatnonzero(has), max(1, int(has.sum()) // 100)):
                    draws = random.standard_normal((len(chunk), DRAWS)) * standard_deviation[chunk, None] + mean[chunk, None]
                    group_max = np.maximum(group_max, draws.max(axis=0))

                best = float(np.nanmax(np.where(has, actual, np.nan)))
                group_rows.append({"scope": "k_window_rule", "k": span, "window": window, "rule": rule, "best_actual": best,
                                   "random_max_median": float(np.median(group_max)),
                                   "percentile_vs_max": float((group_max < best).mean() * 100.0), "draws": DRAWS})
                all_maxima = np.maximum(all_maxima, group_max)
                rule_maxima[rule] = np.maximum(rule_maxima[rule], group_max)
                inner = ((part["distance_bin"] >= 1) & (part["distance_bin"] <= common.DISTANCE_BINS - 2)).to_numpy()

                for pool, mask in ((0, np.ones(len(part), bool)), (1, inner)):
                    key = (rule, pool)
                    totals = rule_sums.setdefault(key, np.zeros(4))
                    totals += [trade_count[mask].sum(), part["sum_net"].to_numpy()[mask].sum(), expected_sum[mask].sum(), variance_sum[mask].sum()]

                total_n = trade_count.sum()
                split_mean, split_standard_deviation, split_percentile = percentile(part["sum_net"].sum() / total_n, expected_sum.sum(), variance_sum.sum(), total_n)
                split_rows.append({"k": span, "window": window, "rule": rule, "actual_mean_net": part["sum_net"].sum() / total_n,
                                   "random_mean": split_mean, "random_sd": split_standard_deviation, "percentile_vs_random": split_percentile})

        print(f"k={span} 끝 {time.time() - started:.0f}초", flush=True)

    cells = pd.concat(cell_frames, ignore_index=True)
    cells.to_parquet(RESULTS / "random_approx_cells.parquet", index=False)
    best_rows = [{"scope": "all", "k": None, "window": None, "rule": None, "best_actual": float(np.nanmax(up["mean_net"])),
                  "random_max_median": float(np.median(all_maxima)),
                  "percentile_vs_max": float((all_maxima < np.nanmax(up["mean_net"])).mean() * 100.0), "draws": DRAWS}]

    for rule in common.RULE_NAMES:
        best = float(np.nanmax(up.loc[up["rule"] == rule, "mean_net"]))
        best_rows.append({"scope": "rule", "k": None, "window": None, "rule": rule, "best_actual": best,
                          "random_max_median": float(np.median(rule_maxima[rule])),
                          "percentile_vs_max": float((rule_maxima[rule] < best).mean() * 100.0), "draws": DRAWS})

    common.write_csv(RESULTS / "random_approx_best.csv", pd.DataFrame(best_rows + group_rows))
    rule_rows = []

    for (rule, pool), (trade_count, sum_net, expected_sum, variance_sum) in rule_sums.items():
        actual = sum_net / trade_count
        mean, standard_deviation, value = percentile(actual, expected_sum, variance_sum, trade_count)
        rule_rows.append({"rule": rule, "pool": POOL_NAMES[pool], "actual_mean_net": actual, "random_mean": mean, "random_sd": standard_deviation,
                          "percentile_vs_random": value, "method": "normal_approx"})

    common.write_csv(RESULTS / "random_approx_rules.csv", pd.DataFrame(rule_rows))
    common.write_csv(RESULTS / "random_approx_split.csv", pd.DataFrame(split_rows))
    check_path = RESULTS / "random_measure_cells.parquet"
    check_summary = None

    if check_path.exists():
        simulated = pd.read_parquet(check_path)
        keys = ["k", "window", "atr_period", "rule", "distance_bin", "bounce_bin"]
        merged = simulated.merge(cells, on=keys, suffixes=("_simulated", "_approx"))
        merged = merged[merged["n_simulated"] > 0]
        common.write_csv(RESULTS / "random_approx_check.csv", merged)
        difference = merged["percentile_vs_random_approx"] - merged["percentile_vs_random_simulated"]
        large = merged["n_simulated"] >= 1000
        check_summary = {"cells": int(len(merged)), "median_abs_percentile_gap": float(difference.abs().median()),
                         "p90_abs_percentile_gap": float(difference.abs().quantile(0.9)),
                         "cells_n_at_least_1000": int(large.sum()),
                         "median_abs_gap_n_at_least_1000": float(difference[large].abs().median()) if large.any() else None,
                         "median_mean_gap": float((merged["random_mean_approx"] - merged["random_mean_simulated"]).median()),
                         "median_sd_ratio": float((merged["random_sd_approx"] / merged["random_sd_simulated"]).median())
                         if "random_sd_simulated" in merged else None}

    common.write_json(RESULTS / "random_approx_metrics.json", {"method": "normal_approx_with_replacement", "draws_for_max": DRAWS,
                                                               "seconds": time.time() - started, "check_vs_simulation": check_summary,
                                                               "reproduction": panel.reproduction()})
    print(f"끝 {time.time() - started:.0f}초 {check_summary}", flush=True)


if __name__ == "__main__":
    main()
