"""스터디 38 매매 시험 — 승인안(2026-10-10)의 모든 조합을 한 번에 돈다. 자동 선택 없음.

조합 = k(8) × 이평 N(7) × ATR 기간(5) × 거리 구간(14) × 반등 구간(10) × 매도 규칙(E1·E2·E3·E4 j=1–5, 8).
상승 추세 칸 313,600 개 + 비교군 (c) 하락 추세 같은 모양 313,600 개 + (b) 상승 추세·저가가 이평 위 2,240 개.
매수 = 신호일 종가. 매도 = 규칙이 선 날(E2 는 이탈 가격·갭이면 시가). 끝까지 안 서면 2026-10-08 종가 평가(미청산)
또는 시세가 끝난 날 마지막 종가(상장폐지·정지). 보유 중 같은 칸의 새 신호는 무시, 청산 다음 행부터 다시 받는다.

산출(results/ 아래, 실행마다 덮어씀):
  combinations.parquet / combinations.csv   조합마다 한 줄(모든 칸·비교군)
  marginals.csv                              상승 추세 칸 두 축 묶음 표(나머지 축 전부 합)
  pools.csv                                  (k, N, ATR 기간, 규칙)마다 상승 추세 칸 전체 합·안쪽 12 구간 합
  rule_table.csv / rule_split.csv            매도 규칙 비교(매수 조건 전체 합) / k·N 별
  mfe_table.csv                              규칙별 MFE·실현·실현÷MFE·최고점→매도 일수 분포
  years.csv / deciles.csv / decile_stocks.csv / stop_buckets.csv / controls.csv / portfolio*.csv / portfolio_daily.parquet
  breaks.csv                                 ±35% 넘는 날 목록(2010 이후)
  ../trades_for_minute.csv                   분봉 구간(2025-09-01 이후 보유 구간을 종목별로 이음)
  trade_metrics.json                         재현 정보·시간·요약
재실행: py research/studies/38_trend_support_realtime/trade_test.py
"""
from __future__ import annotations

import argparse
import time

import numpy as np
import pandas as pd

import common

RESULTS = common.STUDY / "results"
MINUTE_FROM = 20250901
POOL_NAMES = ("all_cells", "inner_cells")
YEAR_FIELDS = ("n", "sum_net", "wins", "sum_excess", "sum_hold", "n_break", "sum_net_break")
SUM_COLUMNS = list(common.FIELD_NAMES[:21])


def slice_plan() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    windows, periods, rules = [], [], []

    for window in range(len(common.WINDOWS)):
        for period in range(len(common.ATR_PERIODS)):
            for rule in range(len(common.RULE_NAMES)):
                windows.append(window)
                periods.append(period)
                rules.append(rule)

    return np.array(windows, np.int64), np.array(periods, np.int64), np.array(rules, np.int64)


def group_labels() -> pd.DataFrame:
    rows = []

    for group in range(common.GROUPS):
        if group == common.GROUP_NO_TOUCH:
            rows.append(("no_touch", -1, -1))
        else:
            side = "up" if group < common.CELLS else "down"
            cell = group % common.CELLS
            rows.append((side, cell // common.BOUNCE_BINS, cell % common.BOUNCE_BINS))

    return pd.DataFrame(rows, columns=["side", "distance_bin", "bounce_bin"])


def statistics_frame(statistics: np.ndarray) -> pd.DataFrame:
    """통계 합 배열(…, 24) → 표 열. 합 열은 그대로 두고(묶음 다시 합칠 때 씀) 평균 열을 붙인다."""
    flat = np.asarray(statistics, dtype=np.float64).reshape(-1, common.FIELDS)
    frame = pd.DataFrame(flat, columns=common.FIELD_NAMES)
    trade_count = frame["n"].to_numpy()
    path = frame["n_path"].to_numpy()

    with np.errstate(invalid="ignore", divide="ignore"):
        frame["mean_net"] = frame["sum_net"] / trade_count
        frame["win_rate"] = frame["wins"] / trade_count * 100.0
        frame["mean_hold"] = frame["sum_hold"] / trade_count
        frame["mean_mae"] = frame["sum_mae"] / path
        frame["mean_mfe"] = frame["sum_mfe"] / path
        frame["mean_bench"] = frame["sum_bench"] / trade_count
        frame["mean_excess"] = frame["sum_excess"] / trade_count
        frame["mean_net_without_break"] = (frame["sum_net"] - frame["sum_net_break"]) / (trade_count - frame["n_break"])
        frame["mean_net_break_only"] = frame["sum_net_break"] / frame["n_break"]
        frame["mean_peak_to_exit"] = frame["sum_peak_to_exit"] / path
        frame["mean_ratio"] = frame["sum_ratio"] / frame["n_mfe_positive"]
        frame["mean_stop_distance"] = frame["sum_stop_distance"] / frame["n_stop_distance"]

    for column in ("n", "wins", "n_path", "n_open", "n_data_end", "n_break", "n_same_day_signal", "n_mfe_positive", "n_stop_distance"):
        frame[column] = frame[column].round().astype(np.int64)

    return frame


def finish_sums(frame: pd.DataFrame) -> pd.DataFrame:
    """합 열과 열쇠 열만 있는 묶음 표에 평균 열을 붙인다(statistics_frame 과 같은 식)."""
    statistics = np.full((len(frame), common.FIELDS), np.nan)

    for index, column in enumerate(SUM_COLUMNS):
        statistics[:, index] = frame[column].to_numpy()

    result = statistics_frame(statistics).drop(columns=["t_net", "t_excess", "median_net"])
    keys = frame.drop(columns=SUM_COLUMNS).reset_index(drop=True)
    return pd.concat([keys, result], axis=1)


def histogram_summary(histograms: np.ndarray) -> dict:
    """한 묶음의 구간 셈 6 개 → 10·50·90 분위수(구간 가운데 값, 수익 0.05%p 폭)."""
    result = {}

    for name, index, kind in (("net", 0, "percent"), ("mfe", 1, "percent"), ("ratio", 2, "ratio"), ("hold", 3, "days"),
                              ("stop", 4, "percent"), ("peak_to_exit", 5, "days")):
        for label, quantile in (("p10", 0.1), ("median", 0.5), ("p90", 0.9)):
            result[f"{label}_{name}"] = common.histogram_quantile(histograms[index], kind, quantile)

    return result


def portfolio_summary(arrays: np.ndarray, market_dates: np.ndarray, signal_start: int) -> tuple[dict, pd.DataFrame]:
    """arrays (3, 시장일) = 보유 수·평가손익·실현손익(원). 필요 자본 = 보유 수 × 500만."""
    holding = arrays[0, signal_start:]
    mark = arrays[1, signal_start:]
    realized = np.cumsum(arrays[2, signal_start:])
    equity = realized + mark
    capital = holding * common.POSITION_KRW
    peak = np.maximum.accumulate(np.maximum(equity, 0.0))
    drawdown = peak - equity
    dates = market_dates[signal_start:]
    years = dates // 10000
    summary = {"peak_capital": float(capital.max()), "peak_capital_date": int(dates[int(np.argmax(capital))]),
               "peak_holdings": float(holding.max()), "final_pnl": float(equity[-1]), "realized_pnl": float(realized[-1]),
               "open_mark": float(mark[-1]), "max_drawdown": float(drawdown.max()),
               "max_drawdown_date": int(dates[int(np.argmax(drawdown))])}
    previous = 0.0

    for year in np.unique(years):
        last = float(equity[years == year][-1])
        summary[f"pnl_{int(year)}"] = last - previous
        previous = last

    daily = pd.DataFrame({"date": dates, "holdings": holding, "capital": capital, "equity": equity})
    return summary, daily


def cover_segments(panel: common.Panel, cover: np.ndarray) -> pd.DataFrame:
    """분봉 구간: 2025-09-01 이후 아무 상승 추세 칸 매매든 보유한 행을 종목별로 이어 붙인 (code, entry_date, exit_date)."""
    rows = []

    for stock in range(panel.stock_count):
        first, last = panel.starts[stock], panel.starts[stock + 1]
        flags = cover[first:last].astype(np.int8)

        if not flags.any():
            continue

        dates = panel.dates[first:last]
        change = np.diff(np.r_[0, flags, 0])
        begins = np.flatnonzero(change == 1)
        ends = np.flatnonzero(change == -1) - 1

        for begin, end in zip(begins, ends):
            rows.append((panel.codes[stock], int(dates[begin]), int(dates[end])))

    return pd.DataFrame(rows, columns=["code", "entry_date", "exit_date"])


def break_list(panel: common.Panel) -> pd.DataFrame:
    rows = np.flatnonzero(panel.break_day & panel.signal_ok)
    previous = panel.close[rows - 1]
    stocks = panel.stock_of_row[rows]
    return pd.DataFrame({"code": np.array(panel.codes)[stocks], "name": np.array(panel.names)[stocks], "date": panel.dates[rows],
                         "previous_close": previous, "open": panel.open_price[rows], "close": panel.close[rows],
                         "open_change_percent": (panel.open_price[rows] / previous - 1.0) * 100.0,
                         "close_change_percent": (panel.close[rows] / previous - 1.0) * 100.0, "raw_factor": panel.factor[rows]})


def decile_stock_counts(panel: common.Panel) -> pd.DataFrame:
    """해마다 분위마다 그 분위에 하루라도 든 종목 수와 종목·일 수. decile −1 = 값 없음(시가총액은 2015-06 전 없음)."""
    frames = []

    for measure, name in enumerate(("price", "turnover", "market_cap")):
        frame = pd.DataFrame({"year": panel.years, "stock": panel.stock_of_row, "decile": panel.deciles[:, measure]})[panel.signal_ok]
        counted = frame.groupby(["year", "decile"])["stock"].nunique().rename("stocks").reset_index()
        counted = counted.merge(frame.groupby(["year", "decile"]).size().rename("stock_days").reset_index(), on=["year", "decile"])
        counted.insert(0, "measure", name)
        frames.append(counted)

    return pd.concat(frames, ignore_index=True)


def same_day_counts(rule_frame: pd.DataFrame, up: pd.DataFrame) -> pd.DataFrame:
    """묶음(pool) 합에는 매도일 신호 수가 안 쌓이므로 칸별 합(combinations)에서 다시 채운다. inner = 거리 구간 1–12."""
    inner = up[(up["distance_bin"] >= 1) & (up["distance_bin"] <= common.DISTANCE_BINS - 2)]
    counts = {"all_cells": up.groupby("rule")["n_same_day_signal"].sum(), "inner_cells": inner.groupby("rule")["n_same_day_signal"].sum()}
    rule_frame["n_same_day_signal"] = [int(counts[pool].get(rule, 0)) for rule, pool in zip(rule_frame["rule"], rule_frame["pool"])]
    return rule_frame


def add_means(frame: pd.DataFrame) -> pd.DataFrame:
    with np.errstate(invalid="ignore", divide="ignore"):
        frame["mean_net"] = frame["sum_net"] / frame["n"]
        frame["win_rate"] = frame["wins"] / frame["n"] * 100.0

        if "sum_excess" in frame:
            frame["mean_excess"] = frame["sum_excess"] / frame["n"]

        if "sum_hold" in frame:
            frame["mean_hold"] = frame["sum_hold"] / frame["n"]

        if "n_break" in frame:
            frame["mean_net_without_break"] = (frame["sum_net"] - frame["sum_net_break"]) / (frame["n"] - frame["n_break"])

    return frame


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spans", default=",".join(str(span) for span in common.K_VALUES))
    arguments = parser.parse_args()
    spans = [int(value) for value in arguments.spans.split(",")]
    started = time.time()
    RESULTS.mkdir(exist_ok=True)
    timings = {}
    panel = common.Panel()
    timings["load"] = time.time() - started
    step = time.time()
    panel.build_benchmark()
    panel.build_deciles()
    high_table = common.build_sparse(panel.high, panel.starts, True)
    low_table = common.build_sparse(panel.low, panel.starts, False)
    timings["prepare"] = time.time() - step
    print(f"준비 끝 {time.time() - started:.0f}초", flush=True)
    slice_window, slice_period, slice_rule = slice_plan()
    slices = len(slice_rule)
    labels = group_labels()
    cover = np.zeros(panel.row_count, dtype=np.uint8)
    cover_from = int(np.searchsorted(panel.market_dates, MINUTE_FROM))
    day_offset = len(common.PERCENT_EDGES) + 1
    width = common.histogram_width()
    stop_edges = np.array(common.STOP_BUCKET_EDGES, dtype=np.float64)
    rule_count = len(common.RULE_NAMES)
    year_count = panel.year_count
    rule_statistics = np.zeros((rule_count, 2, common.FIELDS))
    rule_histograms = np.zeros((rule_count, 2, 6, width), dtype=np.int64)
    rule_years = np.zeros((rule_count, 2, year_count, 7))
    rule_deciles = np.zeros((rule_count, 2, 3, 11, year_count, 4))
    rule_stops = np.zeros((rule_count, 2, len(stop_edges) + 2, 3))
    rule_portfolio = np.zeros((rule_count, 3, panel.market_count))
    combination_frames, pool_rows, split_rows, slice_years = [], [], [], []

    for span in spans:
        step = time.time()
        state, latest_low, _ = panel.trend(span)
        trend_rows = int(((state == 1) | (state == -1))[panel.signal_ok].sum())
        up_rows = int((state == 1)[panel.signal_ok].sum())
        result = common.run_span(slice_window, slice_period, slice_rule, panel.averages, panel.atrs, panel.open_price, panel.high,
                                 panel.low, panel.close, panel.starts, panel.market_position, panel.signal_ok, panel.month_index,
                                 panel.year_index, panel.break_cumulative, panel.deciles, state, latest_low, common.DISTANCE_EDGES,
                                 common.BOUNCE_EDGES, panel.bench, panel.signal_start, panel.market_count, panel.listed_at_end,
                                 common.COST, common.POSITION_KRW, common.PERCENT_EDGES, common.RATIO_EDGES, stop_edges,
                                 trend_rows + up_rows + 16, cover, cover_from, high_table, low_table, common.GROUPS, year_count,
                                 panel.month_count, width, day_offset)
        group_statistics, group_years, pool_statistics, pool_years, pool_deciles, pool_stops, pool_histograms, pool_portfolio, used = result
        frame = statistics_frame(group_statistics)
        frame.insert(0, "k", span)
        frame.insert(1, "window", np.repeat([common.WINDOWS[index] for index in slice_window], common.GROUPS))
        frame.insert(2, "atr_period", np.repeat([common.ATR_PERIODS[index] for index in slice_period], common.GROUPS))
        frame.insert(3, "rule", np.repeat([common.RULE_NAMES[index] for index in slice_rule], common.GROUPS))
        frame.insert(4, "side", np.tile(labels["side"].to_numpy(), slices))
        frame.insert(5, "distance_bin", np.tile(labels["distance_bin"].to_numpy(), slices))
        frame.insert(6, "bounce_bin", np.tile(labels["bounce_bin"].to_numpy(), slices))
        years = group_years.reshape(-1, year_count)

        for year in range(year_count):
            frame[f"n_{panel.first_year + year}"] = years[:, year]

        combination_frames.append(frame)

        for index in range(slices):
            rule = int(slice_rule[index])
            rule_statistics[rule] += pool_statistics[index]
            rule_histograms[rule] += pool_histograms[index]
            rule_years[rule] += pool_years[index]
            rule_deciles[rule] += pool_deciles[index]
            rule_stops[rule] += pool_stops[index]
            rule_portfolio[rule] += pool_portfolio[index, 0]
            keys = {"k": span, "window": common.WINDOWS[slice_window[index]], "atr_period": common.ATR_PERIODS[slice_period[index]],
                    "rule": common.RULE_NAMES[rule]}

            for pool in range(2):
                row = dict(keys, pool=POOL_NAMES[pool])
                row.update(dict(zip(common.FIELD_NAMES, pool_statistics[index, pool])))
                row.update(histogram_summary(pool_histograms[index, pool]))
                summary, _ = portfolio_summary(pool_portfolio[index, pool], panel.market_dates, panel.signal_start)
                row.update({f"portfolio_{name}": value for name, value in summary.items()})
                pool_rows.append(row)

                for year in range(year_count):
                    slice_years.append(dict(keys, pool=POOL_NAMES[pool], year=panel.first_year + year,
                                            **dict(zip(YEAR_FIELDS, pool_years[index, pool, year]))))

        # k·N 별 매도 규칙 비교(ATR 기간 합, 상승 추세 칸 전체): 합은 더하고 분위수는 합친 구간 셈에서
        for window in range(len(common.WINDOWS)):
            for rule in range(rule_count):
                chosen = np.flatnonzero((slice_window == window) & (slice_rule == rule))
                statistics = pool_statistics[chosen, 0].sum(axis=0)
                row = {"k": span, "window": common.WINDOWS[window], "rule": common.RULE_NAMES[rule]}
                row.update(dict(zip(SUM_COLUMNS, statistics[:21])))
                row.update(histogram_summary(pool_histograms[chosen, 0].astype(np.int64).sum(axis=0)))
                split_rows.append(row)

        timings[f"k{span}"] = time.time() - step
        print(f"k={span} 끝 {timings[f'k{span}']:.0f}초, 매매 {int(used.sum()):,}", flush=True)
        del result, group_statistics, group_years, pool_histograms, pool_portfolio

    step = time.time()
    combinations = pd.concat(combination_frames, ignore_index=True)
    combinations.to_parquet(RESULTS / "combinations.parquet", index=False)
    common.write_csv(RESULTS / "combinations.csv", combinations)
    up = combinations[combinations["side"] == "up"]
    axes = ["k", "window", "atr_period", "rule", "distance_bin", "bounce_bin"]
    marginal_frames = []

    for first_index, first_axis in enumerate(axes):
        for second_axis in axes[first_index + 1:]:
            grouped = up.groupby([first_axis, second_axis], as_index=False)[SUM_COLUMNS].sum()
            grouped = grouped.rename(columns={first_axis: "first_value", second_axis: "second_value"})
            grouped.insert(0, "first_axis", first_axis)
            grouped.insert(1, "second_axis", second_axis)
            marginal_frames.append(finish_sums(grouped))

    common.write_csv(RESULTS / "marginals.csv", pd.concat(marginal_frames, ignore_index=True))
    pools = pd.DataFrame(pool_rows)
    pools = pd.concat([pools.drop(columns=list(common.FIELD_NAMES)), statistics_frame(pools[list(common.FIELD_NAMES)].to_numpy())], axis=1)
    common.write_csv(RESULTS / "pools.csv", pools)
    split_frame = pd.DataFrame(split_rows)
    quantile_columns = [column for column in split_frame.columns if column.startswith(("p10_", "median_", "p90_"))]
    split = finish_sums(split_frame.drop(columns=quantile_columns))
    split = pd.concat([split, split_frame[quantile_columns].reset_index(drop=True)], axis=1)
    common.write_csv(RESULTS / "rule_split.csv", split)

    # 매도 규칙 비교(매수 조건 전체 = 모든 k·N·ATR 기간·상승 추세 칸 매매 합)
    rule_rows, mfe_rows, year_rows, decile_rows, stop_rows, portfolio_rows, portfolio_daily = [], [], [], [], [], [], []
    stop_labels = common.stop_bucket_labels()

    for rule in range(rule_count):
        name = common.RULE_NAMES[rule]

        for pool in range(2):
            statistics = rule_statistics[rule, pool].copy()
            statistics[21:] = np.nan
            row = {"rule": name, "pool": POOL_NAMES[pool]}
            row.update(statistics_frame(statistics[None, :]).drop(columns=["t_net", "t_excess", "median_net"]).iloc[0].to_dict())
            quantiles = histogram_summary(rule_histograms[rule, pool])
            row.update(quantiles)
            rule_rows.append(row)
            mfe_row = {"rule": name, "pool": POOL_NAMES[pool], "n": int(statistics[0]), "n_path": int(statistics[7]),
                       "n_mfe_positive": int(statistics[17]), "n_mfe_not_positive": int(statistics[7] - statistics[17]),
                       "n_exit_on_entry_day": int(statistics[0] - statistics[7]),
                       "mean_mfe": statistics[6] / statistics[7] if statistics[7] else None, "mean_net": statistics[1] / statistics[0] if statistics[0] else None,
                       "mean_ratio": statistics[18] / statistics[17] if statistics[17] else None,
                       "mean_peak_to_exit": statistics[16] / statistics[7] if statistics[7] else None}
            mfe_row.update({key: value for key, value in quantiles.items() if key.split("_", 1)[1] in ("net", "mfe", "ratio", "peak_to_exit")})
            mfe_rows.append(mfe_row)

            for year in range(year_count):
                year_rows.append({"rule": name, "pool": POOL_NAMES[pool], "year": panel.first_year + year,
                                  **dict(zip(YEAR_FIELDS, rule_years[rule, pool, year]))})

            for measure, measure_name in enumerate(("price", "turnover", "market_cap")):
                for slot in range(11):
                    for year in range(year_count):
                        values = rule_deciles[rule, pool, measure, slot, year]

                        if values[0] > 0:
                            decile_rows.append({"rule": name, "pool": POOL_NAMES[pool], "measure": measure_name,
                                                "decile": slot if slot < 10 else -1, "year": panel.first_year + year,
                                                "n": values[0], "sum_net": values[1], "wins": values[2], "sum_excess": values[3]})

            for bucket, label in enumerate(stop_labels):
                values = rule_stops[rule, pool, bucket]
                stop_rows.append({"rule": name, "pool": POOL_NAMES[pool], "bucket": label, "n": values[0], "sum_net": values[1],
                                  "wins": values[2]})

        summary, daily = portfolio_summary(rule_portfolio[rule], panel.market_dates, panel.signal_start)
        portfolio_rows.append(dict(rule=name, scope="all_cells_all_k_window_atr", **summary))
        daily.insert(0, "rule", name)
        portfolio_daily.append(daily)

    rule_frame = same_day_counts(pd.DataFrame(rule_rows), up)
    common.write_csv(RESULTS / "rule_table.csv", rule_frame)
    common.write_csv(RESULTS / "mfe_table.csv", pd.DataFrame(mfe_rows))
    common.write_csv(RESULTS / "years.csv", add_means(pd.DataFrame(year_rows)))
    add_means(pd.DataFrame(slice_years)).to_parquet(RESULTS / "slice_years.parquet", index=False)
    common.write_csv(RESULTS / "deciles.csv", add_means(pd.DataFrame(decile_rows)))
    common.write_csv(RESULTS / "decile_stocks.csv", decile_stock_counts(panel))
    common.write_csv(RESULTS / "stop_buckets.csv", add_means(pd.DataFrame(stop_rows)))

    # 비교군: (a) 같은 보유 기간 전 종목 평균(mean_bench), (b) 이평 안 닿은 상승 추세 날(no_touch), (c) 하락 추세 같은 모양(down)
    controls = combinations.groupby(["rule", "side"], as_index=False)[SUM_COLUMNS].sum()
    common.write_csv(RESULTS / "controls.csv", finish_sums(controls))
    controls_by_k = combinations.groupby(["k", "window", "rule", "side"], as_index=False)[SUM_COLUMNS].sum()
    common.write_csv(RESULTS / "controls_by_k_window.csv", finish_sums(controls_by_k))
    common.write_csv(RESULTS / "portfolio.csv", pd.DataFrame(portfolio_rows))
    slice_portfolio = pools[["k", "window", "atr_period", "rule", "pool"] + [column for column in pools.columns if column.startswith("portfolio_")]]
    common.write_csv(RESULTS / "portfolio_slices.csv", slice_portfolio)
    pd.concat(portfolio_daily, ignore_index=True).to_parquet(RESULTS / "portfolio_daily.parquet", index=False)
    breaks = break_list(panel)
    common.write_csv(RESULTS / "breaks.csv", breaks)
    segments = cover_segments(panel, cover)
    common.write_csv(common.STUDY / "trades_for_minute.csv", segments)
    np.save(RESULTS / "rule_histograms.npy", rule_histograms)
    timings["write"] = time.time() - step
    timings["total"] = time.time() - started
    payload = {"reproduction": panel.reproduction(), "timings_seconds": timings, "spans": spans,
               "combination_rows": len(combinations), "up_combinations": int(len(up)),
               "up_combinations_with_trades": int((up["n"] > 0).sum()),
               "break_days": len(breaks), "minute_segments": len(segments),
               "same_day_signal_by_rule": {rule: int(part["n_same_day_signal"].sum()) for rule, part in up.groupby("rule")},
               "grid": {"k": common.K_VALUES, "window": common.WINDOWS, "atr_period": common.ATR_PERIODS,
                        "distance_edges": common.DISTANCE_EDGES, "bounce_edges": common.BOUNCE_EDGES, "rules": common.RULE_NAMES},
               "histogram": {"percent_edges": "−100–100 은 0.05, 100–1,000 은 0.5, 1,000 이상 10 폭(분위수는 구간 가운데 값)",
                             "ratio_step": common.RATIO_STEP}}
    common.write_json(RESULTS / "trade_metrics.json", payload)
    print(f"전체 {timings['total']:.0f}초", flush=True)


if __name__ == "__main__":
    main()
