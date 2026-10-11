"""스터디 38 무작위 진입 시험 — 조합마다 같은 매도 규칙·해마다 같은 매매 수로 종목·날짜만 무작위로 뽑는다.

규칙(승인안 2026-10-10):
  - 뽑는 자리 = 그해 거래된 보통주의 모든 (종목, 날) — 신호 조건 없음. 매수는 그날 종가, 매도는 그 조합의 규칙 그대로.
  - 한 회차 안에서 같은 종목 보유 구간이 겹치면 다시 뽑는다(보유 중 재매수 없음). 다시 뽑기는 그해 매매 수 × 100 번까지
    (내가 정함 — 못 채운 수는 shortfall 열로 센다).
  - 비교 값 = 조합 평균 순수익(%). 조합마다 1,000 회차.
  - 가장 좋은 조합 보정: 회차마다 모든 상승 추세 칸 중 가장 높은 무작위 평균을 뽑아 최고값 분포를 만들고,
    실제 가장 높은 조합 평균이 그 분포의 몇 분위인지 낸다. k·N·규칙 묶음마다, 규칙마다, 전체로 낸다.
  - 매도 규칙 비교 표의 "무작위 대비 분위": 규칙의 모든 칸 무작위 합(같은 회차끼리 더함)으로 만든 1,000 개 평균 분포에서
    실제 규칙 평균의 분위.
  - 통과선 없음.
산출: results/random_cells.parquet, results/random_maxima.npz, results/random_rules.csv, results/random_metrics.json
재실행: py research/studies/38_trend_support_realtime/random_test.py [--runs 1000] [--measure]
"""
from __future__ import annotations

import argparse
import time

import numpy as np
import pandas as pd
from numba import njit, prange

import common

RESULTS = common.STUDY / "results"
REDRAW_LIMIT = 100


@njit(parallel=True, cache=True)
def exit_arrays(rule, average, open_price, high, low, close, starts, state, latest_low, listed_at_end, cost):
    """모든 행에서 종가 매수했을 때의 매도 행과 순수익(%). 규칙이 끝까지 안 서면 마지막 행 종가."""
    rows = close.shape[0]
    exit_row = np.empty(rows, dtype=np.int32)
    net = np.empty(rows)

    for stock in prange(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first
        next_exit = np.empty(length + 1, dtype=np.int64)
        run_length = np.zeros(length + 1, dtype=np.int64)
        common.fill_next_exit(rule, first, length, average, close, low, state, latest_low, run_length, next_exit)

        for offset in range(length):
            exit_offset, price = common.exit_of(rule, offset, length, next_exit, first, open_price, close, latest_low)

            if exit_offset >= length:
                exit_offset = length - 1
                price = close[last - 1]

            exit_row[first + offset] = first + exit_offset
            net[first + offset] = (price / close[first + offset] * cost - 1.0) * 100.0

    return exit_row, net


@njit(parallel=True, cache=True)
def random_sums(counts, rows_by_year, year_offsets, exit_row, net, stock_of_row, stock_count, runs, seeds, redraw_limit):
    """counts (조합, 해) 매매 수. 반환: 회차별 순수익 합(조합, 회차), 뽑힌 수(조합, 회차), 못 채운 수(조합)."""
    combos = counts.shape[0]
    years = counts.shape[1]
    sums = np.zeros((combos, runs))
    taken = np.zeros((combos, runs), dtype=np.int32)
    shortfall = np.zeros(combos, dtype=np.int64)

    for combo in prange(combos):
        total = 0

        for year in range(years):
            total += counts[combo, year]

        if total == 0:
            continue

        np.random.seed(seeds[combo])
        head = np.full(stock_count, -1, dtype=np.int64)
        entries = np.empty(total, dtype=np.int64)
        exits = np.empty(total, dtype=np.int64)
        links = np.empty(total, dtype=np.int64)
        stocks = np.empty(total, dtype=np.int64)

        for run in range(runs):
            used = 0
            value = 0.0

            for year in range(years):
                need = counts[combo, year]
                size = year_offsets[year + 1] - year_offsets[year]

                if need == 0 or size == 0:
                    continue

                drawn_count = 0
                attempts = 0

                while drawn_count < need and attempts < redraw_limit * need:
                    attempts += 1
                    row = rows_by_year[year_offsets[year] + np.random.randint(0, size)]
                    stock = stock_of_row[row]
                    end = exit_row[row]
                    other = head[stock]
                    clash = False

                    while other >= 0:
                        if row <= exits[other] and entries[other] <= end:
                            clash = True
                            break

                        other = links[other]

                    if clash:
                        continue

                    entries[used] = row
                    exits[used] = end
                    stocks[used] = stock
                    links[used] = head[stock]
                    head[stock] = used
                    used += 1
                    drawn_count += 1
                    value += net[row]

                shortfall[combo] += need - drawn_count

            for index in range(used):
                head[stocks[index]] = -1

            sums[combo, run] = value
            taken[combo, run] = used

    return sums, taken, shortfall


def candidate_rows(panel: common.Panel) -> tuple[np.ndarray, np.ndarray]:
    rows = np.flatnonzero(panel.signal_ok)
    years = panel.year_index[rows]
    order = np.argsort(years, kind="stable")
    rows = rows[order].astype(np.int64)
    offsets = np.r_[0, np.cumsum(np.bincount(years, minlength=panel.year_count))].astype(np.int64)
    return rows, offsets


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=1000)
    parser.add_argument("--measure", action="store_true", help="k=5·N=20·E1 묶음 하나로 속도를 재고 그 칸 결과를 남긴다")
    parser.add_argument("--target-runs", type=int, default=1000, help="--measure 때 걸린 시간을 이 회차 수로 늘려 추정한다")
    parser.add_argument("--spans", default=",".join(str(span) for span in common.K_VALUES), help="나눠 돌릴 때 k 목록")
    arguments = parser.parse_args()
    started = time.time()
    combinations = pd.read_parquet(RESULTS / "combinations.parquet")
    up = combinations[combinations["side"] == "up"].reset_index(drop=True)
    year_columns = [column for column in up.columns if column.startswith("n_") and column[2:].isdigit()]
    panel = common.Panel()
    rows_by_year, year_offsets = candidate_rows(panel)
    stock_of_row = panel.stock_of_row.astype(np.int64)
    runs = arguments.runs
    cell_frames = []
    maxima_rows = []
    group_maxima = []
    pool_sums = []
    timings = {"load": time.time() - started}
    spans = (5,) if arguments.measure else tuple(int(value) for value in arguments.spans.split(","))
    total_shortfall = 0

    for span in spans:
        state, latest_low, _ = panel.trend(span)

        for window_index, window in enumerate(common.WINDOWS):
            if arguments.measure and window != 20:
                continue

            for rule_index, rule in enumerate(common.RULE_NAMES):
                if arguments.measure and rule != "E1":
                    continue

                step = time.time()
                exit_row, net = exit_arrays(rule_index, panel.averages[window_index], panel.open_price, panel.high, panel.low,
                                            panel.close, panel.starts, state, latest_low, panel.listed_at_end, common.COST)
                part = up[(up["k"] == span) & (up["window"] == window) & (up["rule"] == rule)].reset_index(drop=True)
                counts = part[year_columns].to_numpy(np.int64)
                seeds = (common.SEED + span * 1_000_003 + window * 10_007 + rule_index * 101 + part.index.to_numpy()).astype(np.int64)
                sums, taken, shortfall = random_sums(counts, rows_by_year, year_offsets, exit_row, net, stock_of_row,
                                                     panel.stock_count, runs, seeds, REDRAW_LIMIT)
                total_shortfall += int(shortfall.sum())
                elapsed = time.time() - step

                with np.errstate(invalid="ignore", divide="ignore"):
                    means = sums / taken

                actual = part["mean_net"].to_numpy()
                has = part["n"].to_numpy() > 0
                frame = part[["k", "window", "atr_period", "rule", "distance_bin", "bounce_bin", "n", "mean_net"]].copy()
                frame["random_mean"] = np.nanmean(np.where(has[:, None], means, np.nan), axis=1)
                frame["random_sd"] = np.nanstd(np.where(has[:, None], means, np.nan), axis=1)
                frame["random_p05"] = np.nanpercentile(np.where(has[:, None], means, np.nan), 5, axis=1)
                frame["random_p95"] = np.nanpercentile(np.where(has[:, None], means, np.nan), 95, axis=1)
                frame["percentile_vs_random"] = np.where(has, (means < actual[:, None]).mean(axis=1) * 100.0, np.nan)
                frame["shortfall"] = shortfall
                cell_frames.append(frame)
                masked = np.where(has[:, None], means, -np.inf)
                group_maxima.append(masked.max(axis=0))
                maxima_rows.append({"k": span, "window": window, "rule": rule, "best_actual": float(np.nanmax(np.where(has, actual, np.nan))),
                                    "trades": int(part["n"].sum()), "seconds": elapsed})

                for pool, mask in ((0, np.ones(len(part), bool)), (1, (part["distance_bin"] >= 1) & (part["distance_bin"] <= 12))):
                    for atr_period in common.ATR_PERIODS:
                        chosen = (mask & (part["atr_period"] == atr_period)).to_numpy()
                        pool_sums.append({"k": span, "window": window, "atr_period": atr_period, "rule": rule, "pool": pool,
                                          "sums": sums[chosen].sum(axis=0), "taken": taken[chosen].sum(axis=0)})

                print(f"k={span} N={window} {rule}: 매매 {int(part['n'].sum()):,} × {runs} 회차 {elapsed:.1f}초", flush=True)

    timings["total"] = time.time() - started

    if arguments.measure:
        row = maxima_rows[0]
        draws = row["trades"] * runs
        total_trades = int(up["n"].sum())
        target = arguments.target_runs
        estimate = row["seconds"] / max(draws, 1) * total_trades * target
        payload = {"measured": row, "runs": runs, "draws": draws, "seconds_per_million_draws": row["seconds"] / max(draws, 1) * 1e6,
                   "all_up_trades": total_trades, "target_runs": target, "estimate_seconds_target_runs": estimate,
                   "estimate_hours_target_runs": estimate / 3600.0}
        common.write_json(RESULTS / "random_measure.json", payload)
        cell_frames[0].to_parquet(RESULTS / "random_measure_cells.parquet", index=False)
        print(payload, flush=True)
        return

    cells = pd.concat(cell_frames, ignore_index=True)
    cells.to_parquet(RESULTS / "random_cells.parquet", index=False)
    maxima = np.vstack(group_maxima)
    keys = pd.DataFrame(maxima_rows)
    np.savez_compressed(RESULTS / "random_maxima.npz", maxima=maxima, keys=keys[["k", "window"]].to_numpy(),
                        rules=keys["rule"].to_numpy().astype(str))
    best_rows = []

    def best_row(scope: str, chosen: np.ndarray) -> dict:
        distribution = maxima[chosen].max(axis=0)
        actual = float(keys.loc[chosen, "best_actual"].max())
        return {"scope": scope, "best_actual_mean_net": actual, "random_best_median": float(np.median(distribution)),
                "random_best_p95": float(np.percentile(distribution, 95)),
                "percentile_of_actual_best": float((distribution < actual).mean() * 100.0)}

    best_rows.append(best_row("all", np.ones(len(keys), bool)))

    for rule in common.RULE_NAMES:
        best_rows.append(best_row(rule, (keys["rule"] == rule).to_numpy()))

    for index in range(len(keys)):
        chosen = np.zeros(len(keys), bool)
        chosen[index] = True
        best_rows.append(best_row(f"k{keys.loc[index, 'k']}_N{keys.loc[index, 'window']}_{keys.loc[index, 'rule']}", chosen))

    common.write_csv(RESULTS / "random_best.csv", pd.DataFrame(best_rows))
    rule_table = pd.read_csv(RESULTS / "rule_table.csv")
    rule_rows = []

    for rule in common.RULE_NAMES:
        for pool in (0, 1):
            parts = [item for item in pool_sums if item["rule"] == rule and item["pool"] == pool]
            sums = np.sum([item["sums"] for item in parts], axis=0)
            taken = np.sum([item["taken"] for item in parts], axis=0)
            distribution = sums / taken
            pool_name = ("all_cells", "inner_cells")[pool]
            actual = float(rule_table[(rule_table["rule"] == rule) & (rule_table["pool"] == pool_name)]["mean_net"].iloc[0])
            rule_rows.append({"rule": rule, "pool": pool_name, "actual_mean_net": actual, "random_mean": float(distribution.mean()),
                              "random_p05": float(np.percentile(distribution, 5)), "random_p95": float(np.percentile(distribution, 95)),
                              "percentile_vs_random": float((distribution < actual).mean() * 100.0), "runs": runs})

    common.write_csv(RESULTS / "random_rules.csv", pd.DataFrame(rule_rows))
    split_rows = []
    split_keys = sorted({(item["k"], item["window"], item["rule"]) for item in pool_sums})
    rule_split = pd.read_csv(RESULTS / "rule_split.csv")

    for span, window, rule in split_keys:
        parts = [item for item in pool_sums if (item["k"], item["window"], item["rule"], item["pool"]) == (span, window, rule, 0)]
        distribution = np.sum([item["sums"] for item in parts], axis=0) / np.sum([item["taken"] for item in parts], axis=0)
        match = rule_split[(rule_split["k"] == span) & (rule_split["window"] == window) & (rule_split["rule"] == rule)]
        actual = float(match["mean_net"].iloc[0])
        split_rows.append({"k": span, "window": window, "rule": rule, "actual_mean_net": actual,
                           "random_mean": float(distribution.mean()), "percentile_vs_random": float((distribution < actual).mean() * 100.0)})

    common.write_csv(RESULTS / "random_split.csv", pd.DataFrame(split_rows))
    common.write_json(RESULTS / "random_metrics.json", {"runs": runs, "redraw_limit": REDRAW_LIMIT, "shortfall_total": total_shortfall,
                                                        "timings_seconds": timings, "groups": maxima_rows,
                                                        "reproduction": panel.reproduction()})
    print(f"끝 {timings['total']:.0f}초, shortfall {total_shortfall}", flush=True)


if __name__ == "__main__":
    main()
