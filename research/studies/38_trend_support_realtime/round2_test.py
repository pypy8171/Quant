"""스터디 38 2회차 — 매수 모양 축 대신 종목 구간(거래대금·시가총액 10분위)과 시장 국면(코스피 추세) 축으로 다시 돈다.

1회차(trade_test.py, results/)는 그대로 두고 이 스크립트는 results/round2/ 에만 쓴다(1회차 산출 재현을 깨지 않으려고 따로 둔 파일).
매수·매도 규칙은 1회차와 같다(common.py 커널 함수 재사용): 상승 추세(state = 1)인 날 종가 매수, 매도 E1·E2·E3·E4 j=1–5,
익절 없음, 끝까지 안 서면 2026-10-08 종가 평가 또는 시세 끝 종가. 비용·비교 평균(B 행렬)도 같다.

바뀐 것(조정 지시 2026-10-10 2회차):
  - 반등 구간(bounce) 10 칸은 하나로 합친다 — 고가 = 저가인 날(반등 구간 없음)도 넣는다.
  - 칸 = k(8) × 이평 N(7) × ATR 기간(5) × 매도 규칙(8) × 거리 구간(14) × 거래대금 분위(10 + 값 없음)
         × 시가총액 분위(10 + 값 없음, 2015-06 전은 값 없음 −1) × 코스피 국면(상승·하락·횡보·모름).
    같은 칸·같은 종목 안에서 보유 중 신호는 무시, 청산 다음 행부터 다시 받는다(1회차와 같은 규칙, 칸이 잘게 나뉜 만큼 매매 수는 늘어난다).
  - 분위 = 매수일 그날 거래된 전 종목 안 횡단면 순위(common.Panel.build_deciles 그대로). 거래대금 = 20일 평균(그날 포함).
  - 코스피 국면 = 코스피 지수 일봉(data/kospi_daily_naver.parquet, fetch_kospi_daily.py)에 종목과 같은 swing_definitions.trend_state 를
    같은 k 로 적용한 매수일 값. 새 상수 없음.
  - 비교군 (b) 이평 안 닿음·(c) 하락 추세, MAE·MFE, 포트폴리오, 분봉 구간은 이번에 계산하지 않는다(1회차 산출에 있음).

산출(results/round2/):
  cells.parquet            주 표. 칸 = k × N × ATR × 규칙 × 거래대금 × 시가총액 × 국면(거리 안쪽 12 구간 −2.0 – +1.0 ATR 합).
                           전체 n·평균·월 묶음 t, 연도마다 n·합·t_excess, 양수 연도 수.
  cells_fine_k{k}.parquet  거리 구간까지 나눈 가장 잘은 칸(연도별 열 없이 전체 값·월 묶음 t·양수 연도 수). n = 0 인 칸은 뺀다.
  axis_by_rule.csv         규칙 × 축 하나(k·N·ATR·거래대금·시가총액·국면) 합, 연도별 평균 초과수익 열 포함
  axis_pairs_by_rule.csv   규칙 × 축 둘(거래대금·시가총액·국면 짝) 합
  distance_by_rule.csv     규칙 × 거리 구간 합(가장 잘은 칸에서)
  persistent_cells.csv     t_excess 문턱별 칸 수와 그 칸들의 양수 연도 비율 분포(주 표 칸·잘은 칸 각각)
  persistent_by_axis.csv   t_excess > 2 칸을 규칙·거래대금·시가총액·국면별로 센 표
  fill_turnover_bounds.csv 해마다 거래대금 분위별 20일 평균 거래대금(원) 최소·중앙·최대(신호 기간 전 종목·일)
  fill_ratio.csv           규칙 × 거래대금 분위 × 연도: 500만 원 ÷ 매수일 거래대금 비율 중앙·p90, 1%·5% 넘는 매매 수
  regime_days.csv          k × 연도 × 국면 시장일 수
  round2_metrics.json      재현 정보·시간·1회차 합계 대조
재실행: py research/studies/38_trend_support_realtime/round2_test.py
"""
from __future__ import annotations

import argparse
import time

import numpy as np
import pandas as pd
from numba import njit, prange

import common

OUTPUT = common.STUDY / "results" / "round2"
KOSPI_PATH = common.STUDY / "data" / "kospi_daily_naver.parquet"
SLOTS = 11                       # 분위 0–9 + 값 없음(−1 → 10)
REGIMES = ("up", "down", "flat", "unknown")
REGIME_SLOTS = len(REGIMES)
POOL_GROUPS = SLOTS * SLOTS * REGIME_SLOTS          # 484
FINE_GROUPS = common.DISTANCE_BINS * POOL_GROUPS     # 6,776
POSITION_KRW = common.POSITION_KRW                   # 500만 원(1회차와 같은 매매당 금액)
RATIO_LOG_LOW = -6.0             # 비율 분포 표 범위: log10(비율 %) −6 – +4, 폭 0.01 (내가 정함 — 분위수 해상도만 정하고 결과 선택에 안 쓴다)
RATIO_LOG_STEP = 0.01
RATIO_BINS = 1002                # 0 = 범위 아래, 1–1000 = 안, 1001 = 범위 위
STAT_NAMES = ("n", "sum_net", "sum_excess", "wins", "sum_hold", "n_break", "sum_net_break", "t_net", "t_excess",
              "years_with_trades", "years_positive_excess", "years_positive_net")
STATS = len(STAT_NAMES)
YEAR_STAT_NAMES = ("n", "sum_net", "sum_excess", "wins", "t_excess", "t_net")
YEAR_STATS = len(YEAR_STAT_NAMES)


@njit(cache=True)
def finish_group(statistics, month_net, month_excess, month_count, year_n, year_net, year_excess):
    statistics[7] = common.cluster_t(month_net, month_count, statistics[1], statistics[0])
    statistics[8] = common.cluster_t(month_excess, month_count, statistics[2], statistics[0])

    for year in range(year_n.shape[0]):
        if year_n[year] > 0:
            statistics[9] += 1.0

            if year_excess[year] > 0:
                statistics[10] += 1.0

            if year_net[year] > 0:
                statistics[11] += 1.0


@njit(cache=True)
def run_slice_round2(rule, average, atr, open_price, low, close, starts, market_position, signal_ok, month_index, year_index,
                     break_cumulative, pool_slot, ratio_bin, ratio_over_one, ratio_over_five, state, latest_low,
                     distance_edges, bench, signal_start, listed_at_end, cost, fine_statistics, pool_statistics, pool_years,
                     ratio_counts, ratio_over, month_count):
    fine_groups = fine_statistics.shape[0]
    pool_groups = pool_statistics.shape[0]
    year_count = pool_years.shape[1]
    inner_last = distance_edges.shape[0] - 1
    fine_month = np.zeros((3, fine_groups, month_count))
    fine_year = np.zeros((3, fine_groups, year_count))
    pool_month = np.zeros((3, pool_groups, month_count))
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    next_allowed = np.zeros(fine_groups, dtype=np.int64)
    used = 0

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first
        common.fill_next_exit(rule, first, length, average, close, low, state, latest_low, run_length, next_exit)
        next_allowed[:] = 0

        for offset in range(length):
            row = first + offset

            if not signal_ok[row] or state[row] != 1:
                continue

            if not (np.isfinite(average[row]) and np.isfinite(atr[row]) and atr[row] > 0):
                continue

            distance_index = common.distance_bin((low[row] - average[row]) / atr[row], distance_edges)
            pool = pool_slot[row]
            group = distance_index * pool_groups + pool

            if offset < next_allowed[group]:
                continue

            exit_offset, exit_price = common.exit_of(rule, offset, length, next_exit, first, open_price, close, latest_low)
            entry = close[row]

            if exit_offset < length:
                exit_row = first + exit_offset
                reason = 1
                next_allowed[group] = exit_offset + 1
            else:
                exit_row = last - 1
                reason = 2 if listed_at_end[stock] else 3
                exit_price = close[exit_row]
                next_allowed[group] = length + 1

            net = (exit_price / entry * cost - 1.0) * 100.0
            hold = exit_row - row
            excess = net - bench[market_position[row] - signal_start, market_position[exit_row] - signal_start]
            broken = break_cumulative[exit_row] - break_cumulative[row] > 0
            month = month_index[row]
            year = year_index[row]
            win = 1.0 if net > 0 else 0.0
            used += 1
            statistics = fine_statistics[group]
            statistics[0] += 1.0
            statistics[1] += net
            statistics[2] += excess
            statistics[3] += win
            statistics[4] += hold

            if broken:
                statistics[5] += 1.0
                statistics[6] += net

            fine_month[0, group, month] += net
            fine_month[1, group, month] += excess
            fine_month[2, group, month] += 1.0
            fine_year[0, group, year] += 1.0
            fine_year[1, group, year] += net
            fine_year[2, group, year] += excess

            if distance_index < 1 or distance_index > inner_last:
                continue

            statistics = pool_statistics[pool]
            statistics[0] += 1.0
            statistics[1] += net
            statistics[2] += excess
            statistics[3] += win
            statistics[4] += hold

            if broken:
                statistics[5] += 1.0
                statistics[6] += net

            pool_month[0, pool, month] += net
            pool_month[1, pool, month] += excess
            pool_month[2, pool, month] += 1.0
            pool_years[pool, year, 0] += 1.0
            pool_years[pool, year, 1] += net
            pool_years[pool, year, 2] += excess
            pool_years[pool, year, 3] += win
            turnover_slot = pool // (pool_groups // ratio_counts.shape[0])
            ratio_counts[turnover_slot, year, ratio_bin[row]] += 1
            ratio_over[turnover_slot, year, 0] += 1
            ratio_over[turnover_slot, year, 1] += ratio_over_one[row]
            ratio_over[turnover_slot, year, 2] += ratio_over_five[row]

    for group in range(fine_groups):
        if fine_statistics[group, 0] > 0:
            finish_group(fine_statistics[group], fine_month[0, group], fine_month[1, group], fine_month[2, group],
                         fine_year[0, group], fine_year[1, group], fine_year[2, group])
        else:
            fine_statistics[group, 7] = np.nan
            fine_statistics[group, 8] = np.nan

    for pool in range(pool_groups):
        if pool_statistics[pool, 0] > 0:
            finish_group(pool_statistics[pool], pool_month[0, pool], pool_month[1, pool], pool_month[2, pool],
                         pool_years[pool, :, 0], pool_years[pool, :, 1], pool_years[pool, :, 2])
        else:
            pool_statistics[pool, 7] = np.nan
            pool_statistics[pool, 8] = np.nan

        for year in range(year_count):
            begin = year * 12
            end = min(begin + 12, month_count)
            pool_years[pool, year, 4] = common.cluster_t(pool_month[1, pool, begin:end], pool_month[2, pool, begin:end],
                                                         pool_years[pool, year, 2], pool_years[pool, year, 0])
            pool_years[pool, year, 5] = common.cluster_t(pool_month[0, pool, begin:end], pool_month[2, pool, begin:end],
                                                         pool_years[pool, year, 1], pool_years[pool, year, 0])

    return used


@njit(parallel=True, cache=True)
def run_span_round2(slice_window, slice_period, slice_rule, averages, atrs, open_price, low, close, starts, market_position,
                    signal_ok, month_index, year_index, break_cumulative, pool_slot, ratio_bin, ratio_over_one, ratio_over_five,
                    state, latest_low, distance_edges, bench, signal_start, listed_at_end, cost, fine_groups, pool_groups,
                    turnover_slots, year_count, month_count, ratio_bins):
    slices = slice_window.shape[0]
    fine_statistics = np.zeros((slices, fine_groups, 12))
    pool_statistics = np.zeros((slices, pool_groups, 12))
    pool_years = np.zeros((slices, pool_groups, year_count, 6))
    ratio_counts = np.zeros((slices, turnover_slots, year_count, ratio_bins), dtype=np.int32)
    ratio_over = np.zeros((slices, turnover_slots, year_count, 3), dtype=np.int64)
    used = np.zeros(slices, dtype=np.int64)

    for index in prange(slices):
        used[index] = run_slice_round2(slice_rule[index], averages[slice_window[index]], atrs[slice_period[index]], open_price,
                                       low, close, starts, market_position, signal_ok, month_index, year_index,
                                       break_cumulative, pool_slot, ratio_bin, ratio_over_one, ratio_over_five, state,
                                       latest_low, distance_edges, bench, signal_start, listed_at_end, cost, fine_statistics[index],
                                       pool_statistics[index], pool_years[index], ratio_counts[index], ratio_over[index],
                                       month_count)

    return fine_statistics, pool_statistics, pool_years, ratio_counts, ratio_over, used


def kospi_states(panel: common.Panel, spans: list[int]) -> tuple[dict, dict]:
    """k 마다 시장일 위치 → 국면 칸(0 상승·1 하락·2 횡보·3 모름). 시장일에 코스피 값이 없으면 그 전 코스피 날 값을 쓴다."""
    kospi = pd.read_parquet(KOSPI_PATH)
    dates = kospi["date"].to_numpy(np.int64)
    position = np.searchsorted(dates, panel.market_dates, side="right") - 1
    missing = np.setdiff1d(panel.market_dates[panel.signal_start:], dates)
    information = {"kospi_rows": len(kospi), "kospi_first": int(dates[0]), "kospi_last": int(dates[-1]),
                   "market_days_missing_in_kospi": len(missing), "missing_sample": missing[:20].tolist()}
    slot_of_state = {1: 0, -1: 1, 0: 2, -9: 3}
    result = {}

    for span in spans:
        pivots = common.swing.confirmed_pivots(kospi["low"].to_numpy(float), kospi["high"].to_numpy(float), span)
        state = common.swing.trend_state(pivots, len(kospi))["state"]
        slots = np.array([slot_of_state[int(value)] for value in state], dtype=np.int64)
        by_market = np.where(position >= 0, slots[np.maximum(position, 0)], 3)
        result[span] = by_market.astype(np.int64)

    return result, information


def ratio_inputs(panel: common.Panel) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """매수일 거래대금(무수정 종가 × 거래량) 대비 500만 원 비율(%)의 분포 칸, 1%·5% 초과 여부."""
    turnover = panel.raw_close * panel.volume

    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = POSITION_KRW / turnover * 100.0
        logarithm = np.log10(ratio)

    bins = np.floor((logarithm - RATIO_LOG_LOW) / RATIO_LOG_STEP).astype(np.float64) + 1.0
    bins = np.where(np.isfinite(bins), np.clip(bins, 0, RATIO_BINS - 1), RATIO_BINS - 1).astype(np.int64)
    return ratio, bins, (ratio > 1.0).astype(np.int64), (ratio > 5.0).astype(np.int64)


def ratio_quantile(counts: np.ndarray, quantile: float) -> float:
    total = counts.sum()

    if total == 0:
        return np.nan

    index = int(np.searchsorted(np.cumsum(counts), quantile * total, side="left"))

    if index == 0:
        return 10 ** RATIO_LOG_LOW

    if index >= RATIO_BINS - 1:
        return np.inf

    return 10 ** (RATIO_LOG_LOW + (index - 0.5) * RATIO_LOG_STEP)


def slot_label(slot: int) -> int:
    return slot if slot < 10 else -1


def pool_keys() -> pd.DataFrame:
    pool = np.arange(POOL_GROUPS)
    return pd.DataFrame({"turnover_decile": [slot_label(value) for value in pool // (SLOTS * REGIME_SLOTS)],
                         "cap_decile": [slot_label(value) for value in (pool // REGIME_SLOTS) % SLOTS],
                         "regime": [REGIMES[value] for value in pool % REGIME_SLOTS]})


def stat_columns(frame: pd.DataFrame) -> pd.DataFrame:
    with np.errstate(invalid="ignore", divide="ignore"):
        frame["mean_net"] = frame["sum_net"] / frame["n"]
        frame["mean_excess"] = frame["sum_excess"] / frame["n"]
        frame["win_rate"] = frame["wins"] / frame["n"] * 100.0
        frame["mean_hold"] = frame["sum_hold"] / frame["n"]
        frame["share_positive_excess_years"] = frame["years_positive_excess"] / frame["years_with_trades"]
        frame["share_positive_net_years"] = frame["years_positive_net"] / frame["years_with_trades"]

    return frame


def summed_table(frame: pd.DataFrame, keys: list[str], years: list[int]) -> pd.DataFrame:
    """칸 합(서로 다른 칸 매매를 더함 — 1회차 묶음 표와 같은 방식). 연도별 평균과 양수 연도 비율을 붙인다."""
    sum_columns = ["n", "sum_net", "sum_excess", "wins", "sum_hold"]
    year_columns = [f"{name}_{year}" for year in years for name in ("n", "sum_net", "sum_excess")]
    grouped = frame.groupby(keys, as_index=False, observed=True)[sum_columns + year_columns].sum()

    with np.errstate(invalid="ignore", divide="ignore"):
        grouped["mean_net"] = grouped["sum_net"] / grouped["n"]
        grouped["mean_excess"] = grouped["sum_excess"] / grouped["n"]
        grouped["win_rate"] = grouped["wins"] / grouped["n"] * 100.0
        grouped["mean_hold"] = grouped["sum_hold"] / grouped["n"]
        with_trades = np.zeros(len(grouped))
        positive_excess = np.zeros(len(grouped))
        positive_net = np.zeros(len(grouped))

        for year in years:
            count = grouped[f"n_{year}"].to_numpy()
            grouped[f"mean_net_{year}"] = grouped[f"sum_net_{year}"] / count
            grouped[f"mean_excess_{year}"] = grouped[f"sum_excess_{year}"] / count
            with_trades += count > 0
            positive_excess += (count > 0) & (grouped[f"sum_excess_{year}"].to_numpy() > 0)
            positive_net += (count > 0) & (grouped[f"sum_net_{year}"].to_numpy() > 0)

        grouped["years_with_trades"] = with_trades
        grouped["share_positive_excess_years"] = positive_excess / with_trades
        grouped["share_positive_net_years"] = positive_net / with_trades

    drop = [f"{name}_{year}" for year in years for name in ("sum_net", "sum_excess")]
    return grouped.drop(columns=drop)


def persistence_rows(frame: pd.DataFrame, table: str) -> list[dict]:
    rows = []
    valid = frame[np.isfinite(frame["t_excess"])]

    for direction, threshold in (("above", 2.0), ("above", 3.0), ("above", 3.5), ("below", -2.0), ("below", -3.0)):
        chosen = valid[valid["t_excess"] > threshold] if direction == "above" else valid[valid["t_excess"] < threshold]
        share = chosen["share_positive_excess_years"]
        rows.append({"table": table, "cells_with_t": len(valid), "cells_with_trades": int((frame["n"] > 0).sum()),
                     "condition": f"t_excess {'>' if direction == 'above' else '<'} {threshold}", "cells": len(chosen),
                     "share_of_cells_with_t": len(chosen) / len(valid) if len(valid) else np.nan,
                     "median_n": float(chosen["n"].median()) if len(chosen) else np.nan,
                     "cells_n_at_least_100": int((chosen["n"] >= 100).sum()),
                     "median_years_with_trades": float(chosen["years_with_trades"].median()) if len(chosen) else np.nan,
                     "positive_year_share_p10": float(share.quantile(0.1)) if len(chosen) else np.nan,
                     "positive_year_share_median": float(share.median()) if len(chosen) else np.nan,
                     "positive_year_share_p90": float(share.quantile(0.9)) if len(chosen) else np.nan,
                     "cells_positive_year_share_over_half": int((share > 0.5).sum()),
                     "cells_every_year_positive": int((share == 1.0).sum()),
                     "cells_every_year_positive_and_10_years": int(((share == 1.0) & (chosen["years_with_trades"] >= 10)).sum())})

    return rows


def turnover_bounds(panel: common.Panel) -> pd.DataFrame:
    frame = pd.DataFrame({"year": panel.years, "decile": panel.deciles[:, 1], "turnover20": panel.turnover20})[panel.signal_ok]
    frame = frame[frame["decile"] >= 0]
    grouped = frame.groupby(["year", "decile"])["turnover20"]
    table = grouped.agg(stock_days="size", minimum="min", median="median", maximum="max").reset_index()
    every = frame.groupby("decile")["turnover20"].agg(stock_days="size", minimum="min", median="median", maximum="max").reset_index()
    every.insert(0, "year", "all")
    return pd.concat([table.astype({"year": str}), every], ignore_index=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spans", default=",".join(str(span) for span in common.K_VALUES))
    arguments = parser.parse_args()
    spans = [int(value) for value in arguments.spans.split(",")]
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    timings = {}
    panel = common.Panel()
    timings["load"] = time.time() - started
    step = time.time()
    panel.build_benchmark()
    panel.build_deciles()
    turnover_slot = np.where(panel.deciles[:, 1] >= 0, panel.deciles[:, 1], 10).astype(np.int64)
    cap_slot = np.where(panel.deciles[:, 2] >= 0, panel.deciles[:, 2], 10).astype(np.int64)
    regimes, kospi_information = kospi_states(panel, spans)
    _, ratio_bin, over_one, over_five = ratio_inputs(panel)
    years = list(range(panel.first_year, panel.first_year + panel.year_count))
    windows, periods, rules = [], [], []

    for window in range(len(common.WINDOWS)):
        for period in range(len(common.ATR_PERIODS)):
            for rule in range(len(common.RULE_NAMES)):
                windows.append(window)
                periods.append(period)
                rules.append(rule)

    slice_window, slice_period, slice_rule = (np.array(values, np.int64) for values in (windows, periods, rules))
    slices = len(slice_rule)
    timings["prepare"] = time.time() - step
    print(f"준비 끝 {time.time() - started:.0f}초", flush=True)
    keys = pool_keys()
    pool_frames, persistence_fine, axis_fine, regime_rows = [], [], [], []
    ratio_total = np.zeros((len(common.RULE_NAMES), SLOTS, panel.year_count, RATIO_BINS), dtype=np.int64)
    ratio_over_total = np.zeros((len(common.RULE_NAMES), SLOTS, panel.year_count, 3), dtype=np.int64)
    used_total = {}
    fine_rows = 0

    for span in spans:
        step = time.time()
        state, latest_low, _ = panel.trend(span)
        regime_row = regimes[span][panel.market_position]
        pool_slot = ((turnover_slot * SLOTS + cap_slot) * REGIME_SLOTS + regime_row).astype(np.int64)
        result = run_span_round2(slice_window, slice_period, slice_rule, panel.averages, panel.atrs, panel.open_price, panel.low,
                                 panel.close, panel.starts, panel.market_position, panel.signal_ok, panel.month_index,
                                 panel.year_index, panel.break_cumulative, pool_slot, ratio_bin, over_one, over_five, state,
                                 latest_low, common.DISTANCE_EDGES, panel.bench, panel.signal_start, panel.listed_at_end,
                                 common.COST, FINE_GROUPS, POOL_GROUPS, SLOTS, panel.year_count, panel.month_count, RATIO_BINS)
        fine_statistics, pool_statistics, pool_years, ratio_counts, ratio_over, used = result
        used_total[span] = int(used.sum())

        for index in range(slices):
            ratio_total[slice_rule[index]] += ratio_counts[index]
            ratio_over_total[slice_rule[index]] += ratio_over[index]

        slice_keys = pd.DataFrame({"k": span, "window": [common.WINDOWS[value] for value in slice_window],
                                   "atr_period": [common.ATR_PERIODS[value] for value in slice_period],
                                   "rule": [common.RULE_NAMES[value] for value in slice_rule]})

        # 가장 잘은 칸(거리 구간 포함)
        fine = pd.DataFrame(fine_statistics.reshape(-1, STATS), columns=STAT_NAMES)
        fine_keys = slice_keys.loc[np.repeat(np.arange(slices), FINE_GROUPS)].reset_index(drop=True)
        group = np.tile(np.arange(FINE_GROUPS), slices)
        fine_keys["distance_bin"] = (group // POOL_GROUPS).astype(np.int8)
        fine_keys = pd.concat([fine_keys, keys.loc[group % POOL_GROUPS].reset_index(drop=True)], axis=1)
        fine = pd.concat([fine_keys, fine], axis=1)
        fine = stat_columns(fine[fine["n"] > 0].reset_index(drop=True))
        fine.to_parquet(OUTPUT / f"cells_fine_k{span}.parquet", index=False)
        fine_rows += len(fine)
        persistence_fine.append(fine[["rule", "turnover_decile", "cap_decile", "regime", "n", "t_excess", "years_with_trades",
                                      "share_positive_excess_years"]].copy())
        axis_fine.append(fine.groupby(["rule", "distance_bin"], as_index=False)[["n", "sum_net", "sum_excess", "wins"]].sum())
        del fine, fine_keys, fine_statistics

        # 주 표 칸(거리 안쪽 12 구간 합)
        pool = pd.DataFrame(pool_statistics.reshape(-1, STATS), columns=STAT_NAMES)
        pool_index = np.tile(np.arange(POOL_GROUPS), slices)
        pool = pd.concat([slice_keys.loc[np.repeat(np.arange(slices), POOL_GROUPS)].reset_index(drop=True),
                          keys.loc[pool_index].reset_index(drop=True), pool], axis=1)
        flat_years = pool_years.reshape(-1, panel.year_count, YEAR_STATS)
        year_frame = {}

        for year_index, year in enumerate(years):
            year_frame[f"n_{year}"] = flat_years[:, year_index, 0].astype(np.int32)
            year_frame[f"sum_net_{year}"] = flat_years[:, year_index, 1]
            year_frame[f"sum_excess_{year}"] = flat_years[:, year_index, 2]
            year_frame[f"t_excess_{year}"] = flat_years[:, year_index, 4].astype(np.float32)

        pool = pd.concat([pool, pd.DataFrame(year_frame)], axis=1)
        pool = pool[pool["n"] > 0].reset_index(drop=True)
        pool_frames.append(pool)
        regime_frame = pd.DataFrame({"year": panel.market_dates[panel.signal_start:] // 10000,
                                     "regime": [REGIMES[value] for value in regimes[span][panel.signal_start:]]})
        counted = regime_frame.groupby(["year", "regime"]).size().rename("market_days").reset_index()
        counted.insert(0, "k", span)
        regime_rows.append(counted)
        timings[f"k{span}"] = time.time() - step
        print(f"k={span} 끝 {timings[f'k{span}']:.0f}초, 매매 {used_total[span]:,}", flush=True)
        del result, pool_statistics, pool_years, ratio_counts, ratio_over

    step = time.time()
    cells = stat_columns(pd.concat(pool_frames, ignore_index=True))
    del pool_frames
    for column in ("rule", "regime"):
        cells[column] = cells[column].astype("category")

    cells.to_parquet(OUTPUT / "cells.parquet", index=False)

    # 축별 표(규칙마다): 축 하나, 축 둘
    axis_frames = []

    for axis in ("k", "window", "atr_period", "turnover_decile", "cap_decile", "regime"):
        table = summed_table(cells, ["rule", axis], years).rename(columns={axis: "value"})
        table.insert(1, "axis", axis)
        table["value"] = table["value"].astype(str)
        axis_frames.append(table)

    every = summed_table(cells.assign(axis="all", value="all"), ["rule", "axis", "value"], years)
    axis_frames.append(every)
    common.write_csv(OUTPUT / "axis_by_rule.csv", pd.concat(axis_frames, ignore_index=True))
    pair_frames = []

    for first, second in (("turnover_decile", "cap_decile"), ("turnover_decile", "regime"), ("cap_decile", "regime"),
                          ("turnover_decile", "k"), ("turnover_decile", "window")):
        table = summed_table(cells, ["rule", first, second], years).rename(columns={first: "first_value", second: "second_value"})
        table.insert(1, "first_axis", first)
        table.insert(2, "second_axis", second)
        table["first_value"] = table["first_value"].astype(str)
        table["second_value"] = table["second_value"].astype(str)
        pair_frames.append(table)

    common.write_csv(OUTPUT / "axis_pairs_by_rule.csv", pd.concat(pair_frames, ignore_index=True))
    distance = pd.concat(axis_fine).groupby(["rule", "distance_bin"], as_index=False).sum()
    distance["distance_label"] = [common.distance_bin_label(int(value)) for value in distance["distance_bin"]]
    distance["mean_net"] = distance["sum_net"] / distance["n"]
    distance["mean_excess"] = distance["sum_excess"] / distance["n"]
    distance["win_rate"] = distance["wins"] / distance["n"] * 100.0
    common.write_csv(OUTPUT / "distance_by_rule.csv", distance)

    # 해마다 유지되는 칸
    fine_all = pd.concat(persistence_fine, ignore_index=True)
    persistence = persistence_rows(cells, "cells(k×N×ATR×규칙×거래대금×시가총액×국면, 거리 안쪽 합)")
    persistence += persistence_rows(fine_all, "cells_fine(거리 구간까지)")
    common.write_csv(OUTPUT / "persistent_cells.csv", pd.DataFrame(persistence))
    by_axis = []

    for name, frame in (("cells", cells), ("cells_fine", fine_all)):
        valid = frame[np.isfinite(frame["t_excess"])]

        for axis in ("rule", "turnover_decile", "cap_decile", "regime"):
            grouped = valid.groupby(axis, observed=True)
            table = pd.DataFrame({"cells_with_t": grouped.size(),
                                  "cells_t_over_2": grouped["t_excess"].apply(lambda values: int((values > 2).sum())),
                                  "cells_t_over_3_5": grouped["t_excess"].apply(lambda values: int((values > 3.5).sum())),
                                  "cells_t_under_minus_2": grouped["t_excess"].apply(lambda values: int((values < -2).sum()))})
            strong = valid[valid["t_excess"] > 2].groupby(axis, observed=True)["share_positive_excess_years"]
            table["t_over_2_positive_year_share_median"] = strong.median()
            table = table.reset_index().rename(columns={axis: "value"})
            table.insert(0, "axis", axis)
            table.insert(0, "table", name)
            table["value"] = table["value"].astype(str)
            by_axis.append(table)

    common.write_csv(OUTPUT / "persistent_by_axis.csv", pd.concat(by_axis, ignore_index=True))
    del fine_all

    # 체결 가능성
    common.write_csv(OUTPUT / "fill_turnover_bounds.csv", turnover_bounds(panel))
    ratio_rows = []

    for rule_index, rule in enumerate(list(common.RULE_NAMES) + ["all_rules"]):
        counts_rule = ratio_total[rule_index] if rule_index < len(common.RULE_NAMES) else ratio_total.sum(axis=0)
        over_rule = ratio_over_total[rule_index] if rule_index < len(common.RULE_NAMES) else ratio_over_total.sum(axis=0)

        for slot in range(SLOTS):
            for year_index, year in enumerate(years + ["all"]):
                counts = counts_rule[slot, year_index] if year != "all" else counts_rule[slot].sum(axis=0)
                over = over_rule[slot, year_index] if year != "all" else over_rule[slot].sum(axis=0)

                if over[0] == 0:
                    continue

                ratio_rows.append({"rule": rule, "turnover_decile": slot_label(slot), "year": year, "trades": int(over[0]),
                                   "median_ratio_percent": ratio_quantile(counts, 0.5),
                                   "p90_ratio_percent": ratio_quantile(counts, 0.9),
                                   "trades_over_1_percent": int(over[1]), "trades_over_5_percent": int(over[2]),
                                   "share_over_1_percent": over[1] / over[0], "share_over_5_percent": over[2] / over[0]})

    common.write_csv(OUTPUT / "fill_ratio.csv", pd.DataFrame(ratio_rows))
    common.write_csv(OUTPUT / "regime_days.csv", pd.concat(regime_rows, ignore_index=True))

    # 1회차 대조: 규칙별 안쪽 합(1회차 inner_cells) — 칸이 잘게 나뉘어 보유 중 무시가 줄어 매매 수는 늘어야 한다
    first_round = pd.read_csv(common.STUDY / "results" / "rule_table.csv")
    first_round = first_round[first_round["pool"] == "inner_cells"].set_index("rule")
    second_round = cells.groupby("rule", observed=True)[["n", "sum_net", "sum_excess"]].sum()
    comparison = {rule: {"round1_n": int(first_round.loc[rule, "n"]), "round2_n": int(second_round.loc[rule, "n"]),
                         "round1_mean_net": float(first_round.loc[rule, "mean_net"]),
                         "round2_mean_net": float(second_round.loc[rule, "sum_net"] / second_round.loc[rule, "n"]),
                         "round1_mean_excess": float(first_round.loc[rule, "mean_excess"]),
                         "round2_mean_excess": float(second_round.loc[rule, "sum_excess"] / second_round.loc[rule, "n"])}
                  for rule in common.RULE_NAMES}
    timings["write"] = time.time() - step
    timings["total"] = time.time() - started
    payload = {"reproduction": panel.reproduction(), "kospi": kospi_information,
               "kospi_source": "research/studies/38_trend_support_realtime/data/kospi_daily_naver.parquet (fetch_kospi_daily.py)",
               "timings_seconds": timings, "spans": spans, "trades_by_k": used_total, "cells_rows": len(cells),
               "fine_rows": fine_rows, "round1_comparison_inner": comparison,
               "cap_first_year": int(panel.years[(panel.deciles[:, 2] >= 0) & panel.signal_ok].min()),
               "hand_set": {"ratio_histogram": "log10(비율 %) −6 – +4, 폭 0.01 (분위수 해상도만, 내가 정함)",
                            "positive_year": "그해 매매 1건 이상이면 센다(내가 정함)",
                            "pool_inner": "주 표는 거리 안쪽 12 구간(−2.0 – +1.0 ATR) 합(1회차 inner_cells 와 같은 정의)"}}
    common.write_json(OUTPUT / "round2_metrics.json", payload)
    print(f"전체 {timings['total']:.0f}초", flush=True)


if __name__ == "__main__":
    main()
