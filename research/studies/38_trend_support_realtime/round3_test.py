"""스터디 38 3회차 — 보정한 거래대금으로 분위를 다시 매기고, 체결 제약(주문 금액 × 주문 비율 상한)을 축으로 넣고,
칸 수에 맞춘 다중비교 기준선(월 묶음 부호 뒤집기 1,000 회의 최대 t 95% 분위)을 낸다.

1·2회차 산출(results/, results/round2/)은 건드리지 않고 results/round3/ 에만 쓴다.
매수·매도 규칙은 1·2회차와 같다(common.py 커널 재사용): 상승 추세(state = 1)인 날 종가 매수, 매도 E1·E2·E3·E4 j=1–5,
익절 없음, 끝까지 안 서면 2026-10-08 종가 평가 또는 시세 끝 종가. 비용·비교 평균(B 행렬)도 같다.
거리는 안쪽 12 구간(−2.0 – +1.0 ATR)만 매매한다(2회차 주 표와 같은 범위).

바뀐 것(조정 지시 2026-10-10 3회차):
  - 거래대금: data/raw_close_corrections.parquet(round3_factor_fix.py)의 그날 실제 거래대금(다음·data.go.kr, 없는 행은 대용값).
    20일 평균 거래대금 = 그날 포함 20 행 평균(창 안에 값 없는 행이 있으면 값 없음).
    거래대금 분위 = common.Panel.build_deciles 그대로(보정 값으로 다시 매김).
  - 체결 제약 축: 주문 금액 {100만, 500만, 2,000만, 1억 원} × 주문 비율 상한 {0.5, 1, 2, 5, 10 %, 상한 없음}.
    주문 비율 = 주문 금액 ÷ 매수일(신호일 = 종가 매수일) 그날 거래대금(보정표). 상한을 넘는 신호는 매매에서 뺀다.
    두 값 모두 내가 정한 격자이고 고르는 데 쓰지 않는다. 주문 금액 ÷ 상한 = 최소 거래대금 문턱이라 24 조합은 문턱 15 개
    (14 개 + 상한 없음)로 줄어든다 — 같은 문턱인 조합은 숫자가 같다. 칸 계산·M 은 서로 다른 문턱 기준.
  - 칸 = k(8) × 이평 N(7) × ATR 기간(5) × 매도 규칙(8) × 거래대금 분위(10 + 값 없음) × 문턱(15).
    시가총액 분위·코스피 국면·거리 구간은 2회차에서 차이가 작아 이번 칸에서 뺀다(같은 칸 안 보유 중 무시 규칙이 달라져 매매 수가 다르다).
    같은 칸·같은 종목 안에서 실제로 산 매매가 보유 중이면 신호를 무시하고, 청산 다음 행부터 다시 받는다.
    문턱에 걸려 못 산 신호는 보유를 만들지 않는다.
  - 다중비교 기준선: 칸마다 월별 초과수익 합 s_m·매매 수 c_m 을 두고, 달마다 부호 ε_m(±1, 모든 칸에 같은 부호 —
    칸 사이 상관을 그대로 둔다)을 1,000 번 뽑아 Σε_m s_m 로 월 묶음 t 를 다시 계산해 칸들의 최대 t 를 모은다.
    그 95% 분위를 기준선으로 쓴다. 가장 잘은 칸(위 칸)과 판정 표 칸(규칙 × 분위 × 문턱, k·N·ATR 합) 두 층에서 각각 낸다.
    정규분포 가정의 우연 기대 칸 수(M × P(Z > 3.5))와 뒤집기 평균 칸 수도 같이 낸다.

산출(results/round3/):
  cells.parquet          가장 잘은 칸(n > 0): 합·평균·월 묶음 t·양수 연도 수·연도별 n/초과수익 합
  decision_table.csv     규칙 × 거래대금 분위 × 주문 금액 × 상한(24 조합, 문턱 열 포함): 평균 초과수익·양수 연도 비율·
                         옆 분위 부호·t·기준선 넘음 여부
  pooled_cells.csv       판정 표 칸(규칙 × 분위 × 문턱) 원 숫자와 연도별 평균 초과수익
  max_t_null.csv         층별 뒤집기 1,000 회의 최대 t(정렬 전 원 순서)
  multiple_comparison.json  층별 M·우연 기대 칸 수·실제 t > 3.5 칸 수·기준선·기준선 넘는 칸 수(문턱별)
  fill_excluded.csv      규칙 × 분위 × 문턱: 문턱 때문에 뺀 신호 비율(상한 없음 대비 매매 수)
  round3_metrics.json    재현 정보·시간·보정 적용 수·2회차 대조
재실행: py research/studies/38_trend_support_realtime/round3_test.py
"""
from __future__ import annotations

import argparse
import math
import time

import numpy as np
import pandas as pd
from numba import njit, prange

import common

OUTPUT = common.STUDY / "results" / "round3"
CORRECTIONS = common.STUDY / "data" / "raw_close_corrections.parquet"
SLOTS = 11                                         # 분위 0–9 + 값 없음(10)
ORDER_AMOUNTS = (1_000_000.0, 5_000_000.0, 20_000_000.0, 100_000_000.0)   # 내가 정한 격자, 고르는 데 쓰지 않음
RATIO_CAPS = (0.5, 1.0, 2.0, 5.0, 10.0, None)      # 주문 비율 상한 %(None = 상한 없음). 내가 정한 격자, 고르는 데 쓰지 않음
FLIPS = 1000
FLIP_BATCH = 50
NULL_QUANTILE = 0.95
NORMAL_REFERENCE_T = 3.5
STAT_NAMES = ("n", "sum_net", "sum_excess", "wins", "sum_hold", "t_net", "t_excess", "years_with_trades",
              "years_positive_excess", "years_positive_net")
STATS = len(STAT_NAMES)


def threshold_grid() -> tuple[np.ndarray, list[dict]]:
    """서로 다른 최소 거래대금 문턱(원)과 24 조합 → 문턱 번호. 0 = 상한 없음."""
    combinations = []

    for amount in ORDER_AMOUNTS:
        for ratio_cap in RATIO_CAPS:
            combinations.append({"order_amount_krw": amount, "ratio_cap_percent": ratio_cap,
                                 "min_turnover_krw": 0.0 if ratio_cap is None else amount / (ratio_cap / 100.0)})

    thresholds = np.array(sorted({row["min_turnover_krw"] for row in combinations}), dtype=np.float64)

    for row in combinations:
        row["filter"] = int(np.searchsorted(thresholds, row["min_turnover_krw"]))

    return thresholds, combinations


def rolling_mean_missing(values: np.ndarray, starts: np.ndarray, window: int) -> np.ndarray:
    """그날 포함 window 행 평균. 창 안에 값 없는 행이 하나라도 있으면 값 없음(그 뒤 행은 다시 계산된다)."""
    result = np.full(values.shape[0], np.nan)
    known = np.isfinite(values)
    filled = np.where(known, values, 0.0)

    for stock in range(starts.shape[0] - 1):
        first, last = starts[stock], starts[stock + 1]

        if last - first < window:
            continue

        cumulative = np.r_[0.0, np.cumsum(filled[first:last])]
        missing = np.r_[0, np.cumsum(~known[first:last])]
        total = cumulative[window:] - cumulative[:-window]
        gaps = missing[window:] - missing[:-window]
        result[first + window - 1:last] = np.where(gaps == 0, total / window, np.nan)

    return result


@njit(cache=True)
def finish_cell(statistics, month_net, month_excess, month_count, year_n, year_net, year_excess):
    statistics[5] = common.cluster_t(month_net, month_count, statistics[1], statistics[0])
    statistics[6] = common.cluster_t(month_excess, month_count, statistics[2], statistics[0])

    for year in range(year_n.shape[0]):
        if year_n[year] > 0:
            statistics[7] += 1.0

            if year_excess[year] > 0:
                statistics[8] += 1.0

            if year_net[year] > 0:
                statistics[9] += 1.0


@njit(cache=True)
def run_slice_round3(rule, average, atr, open_price, low, close, starts, market_position, signal_ok, month_index, year_index,
                     turnover_slot, day_turnover, thresholds, state, latest_low, distance_edges, bench, signal_start,
                     listed_at_end, cost, statistics, month_sums, year_sums):
    """한 (k, N, ATR, 규칙) 의 칸(분위 × 문턱)별 합. month_sums[g, m] = (순수익 합, 초과수익 합, 매매 수),
    year_sums[g, y] = (매매 수, 순수익 합, 초과수익 합)."""
    filters = thresholds.shape[0]
    groups = statistics.shape[0]
    inner_last = distance_edges.shape[0] - 1
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    next_allowed = np.zeros(groups, dtype=np.int64)
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

            if distance_index < 1 or distance_index > inner_last:
                continue

            base = turnover_slot[row] * filters
            computed = False
            exit_offset = length
            net = 0.0
            excess = 0.0
            hold = 0
            win = 0.0

            for filter_index in range(filters):
                threshold = thresholds[filter_index]

                if threshold > 0.0 and not (day_turnover[row] >= threshold):
                    continue

                group = base + filter_index

                if offset < next_allowed[group]:
                    continue

                if not computed:
                    computed = True
                    exit_offset, exit_price = common.exit_of(rule, offset, length, next_exit, first, open_price, close,
                                                             latest_low)

                    if exit_offset < length:
                        exit_row = first + exit_offset
                    else:
                        exit_row = last - 1
                        exit_price = close[exit_row]

                    net = (exit_price / close[row] * cost - 1.0) * 100.0
                    hold = exit_row - row
                    excess = net - bench[market_position[row] - signal_start, market_position[exit_row] - signal_start]
                    win = 1.0 if net > 0 else 0.0

                next_allowed[group] = exit_offset + 1 if exit_offset < length else length + 1
                month = month_index[row]
                year = year_index[row]
                cell = statistics[group]
                cell[0] += 1.0
                cell[1] += net
                cell[2] += excess
                cell[3] += win
                cell[4] += hold
                month_sums[group, month, 0] += net
                month_sums[group, month, 1] += excess
                month_sums[group, month, 2] += 1.0
                year_sums[group, year, 0] += 1.0
                year_sums[group, year, 1] += net
                year_sums[group, year, 2] += excess
                used += 1

    for group in range(groups):
        if statistics[group, 0] > 0:
            finish_cell(statistics[group], month_sums[group, :, 0], month_sums[group, :, 1], month_sums[group, :, 2],
                        year_sums[group, :, 0], year_sums[group, :, 1], year_sums[group, :, 2])
        else:
            statistics[group, 5] = np.nan
            statistics[group, 6] = np.nan

    return used


@njit(parallel=True, cache=True)
def run_span_round3(slice_window, slice_period, slice_rule, averages, atrs, open_price, low, close, starts, market_position,
                    signal_ok, month_index, year_index, turnover_slot, day_turnover, thresholds, state, latest_low,
                    distance_edges, bench, signal_start, listed_at_end, cost, groups, year_count, month_count):
    slices = slice_window.shape[0]
    statistics = np.zeros((slices, groups, STATS))
    month_sums = np.zeros((slices, groups, month_count, 3))
    year_sums = np.zeros((slices, groups, year_count, 3))
    used = np.zeros(slices, dtype=np.int64)

    for index in prange(slices):
        used[index] = run_slice_round3(slice_rule[index], averages[slice_window[index]], atrs[slice_period[index]],
                                       open_price, low, close, starts, market_position, signal_ok, month_index, year_index,
                                       turnover_slot, day_turnover, thresholds, state, latest_low, distance_edges, bench,
                                       signal_start, listed_at_end, cost, statistics[index], month_sums[index], year_sums[index])

    return statistics, month_sums, year_sums, used


def apply_corrections(panel: common.Panel) -> dict:
    """보정표(패널 행 순서)로 무수정 종가·그날 거래대금을 바꾸고 20일 평균 거래대금을 다시 낸다. 공용 parquet 은 고치지 않는다."""
    table = pd.read_parquet(CORRECTIONS)
    codes = np.array(panel.codes)[panel.stock_of_row]

    if len(table) != panel.row_count or not (np.array_equal(table["date"].to_numpy(np.int64), panel.dates)
                                             and np.array_equal(table["code"].to_numpy(), codes)):
        raise SystemExit("보정표 행이 패널과 다르다 — round3_factor_fix.py 를 다시 돌린다")

    panel.raw_close_original = panel.raw_close
    panel.turnover20_original = panel.turnover20
    panel.raw_close = table["raw_close_corrected"].to_numpy(np.float64)
    panel.day_turnover = table["turnover_corrected"].to_numpy(np.float64)
    panel.turnover20 = rolling_mean_missing(panel.day_turnover, panel.starts, common.TURNOVER_DAYS)
    status = table["status"].astype(str).to_numpy()
    signal = panel.signal_ok
    return {"rows": int(len(table)),
            "rows_by_status_signal_period": {key: int(value) for key, value in zip(*np.unique(status[signal], return_counts=True))},
            "turnover20_changed_over_2x_signal_rows": int(np.nansum(
                np.abs(np.log(panel.turnover20_original[signal] / panel.turnover20[signal])) > np.log(2)))}


@njit(cache=True)
def log_beta_continued(first_shape, second_shape, point):
    """정칙 불완전 베타의 연분수 부분(Numerical Recipes betacf)."""
    tiny = 1e-300
    qab = first_shape + second_shape
    qap = first_shape + 1.0
    qam = first_shape - 1.0
    numerator_ratio = 1.0
    denominator_ratio = 1.0 - qab * point / qap

    if abs(denominator_ratio) < tiny:
        denominator_ratio = tiny

    denominator_ratio = 1.0 / denominator_ratio
    fraction = denominator_ratio

    for term_index in range(1, 300):
        m2 = 2 * term_index
        aa = term_index * (second_shape - term_index) * point / ((qam + m2) * (first_shape + m2))
        denominator_ratio = 1.0 + aa * denominator_ratio
        denominator_ratio = tiny if abs(denominator_ratio) < tiny else denominator_ratio
        numerator_ratio = 1.0 + aa / numerator_ratio
        numerator_ratio = tiny if abs(numerator_ratio) < tiny else numerator_ratio
        denominator_ratio = 1.0 / denominator_ratio
        fraction *= denominator_ratio * numerator_ratio
        aa = -(first_shape + term_index) * (qab + term_index) * point / ((first_shape + m2) * (qap + m2))
        denominator_ratio = 1.0 + aa * denominator_ratio
        denominator_ratio = tiny if abs(denominator_ratio) < tiny else denominator_ratio
        numerator_ratio = 1.0 + aa / numerator_ratio
        numerator_ratio = tiny if abs(numerator_ratio) < tiny else numerator_ratio
        denominator_ratio = 1.0 / denominator_ratio
        delta = denominator_ratio * numerator_ratio
        fraction *= delta

        if abs(delta - 1.0) < 1e-14:
            break

    return math.log(fraction)


@njit(cache=True)
def log_t_upper(t_value, df):
    """t 분포(자유도 df) 위쪽 꼬리 확률의 log. P(T > t)."""
    if not np.isfinite(t_value):
        return 0.0

    point = df / (df + t_value * t_value)
    first_shape = df / 2.0
    second_shape = 0.5
    log_front = math.lgamma(first_shape + second_shape) - math.lgamma(first_shape) - math.lgamma(second_shape) + first_shape * math.log(point) + second_shape * math.log1p(-point)

    if point < (first_shape + 1.0) / (first_shape + second_shape + 2.0):
        log_incomplete = log_front + log_beta_continued(first_shape, second_shape, point) - math.log(first_shape)
        half = math.log(0.5) + log_incomplete
        return half if t_value > 0 else math.log1p(-math.exp(half))

    incomplete = 1.0 - math.exp(log_front + log_beta_continued(second_shape, first_shape, 1.0 - point) - math.log(second_shape))
    half = 0.5 * incomplete
    return math.log(half) if t_value > 0 else math.log1p(-half)


@njit(parallel=True, cache=True)
def log_t_upper_matrix(values, degrees):
    result = np.zeros(values.shape)

    for row in prange(values.shape[0]):
        for column in range(values.shape[1]):
            result[row, column] = log_t_upper(values[row, column], degrees[row])

    return result


def normal_equivalent(log_p: float) -> float:
    """위쪽 꼬리 확률 exp(log_p) 에 맞는 표준정규 z(이분법)."""
    low, high = -10.0, 40.0

    for _ in range(200):
        middle = (low + high) / 2.0
        tail = 0.5 * math.erfc(middle / math.sqrt(2.0))
        value = math.log(tail) if tail > 0 else -1e300

        if value > log_p:
            low = middle
        else:
            high = middle

    return (low + high) / 2.0


def flip_null(month_excess: np.ndarray, month_count: np.ndarray, signs: np.ndarray, label: str):
    """칸 × 월 행렬에서 부호 뒤집기 분포를 낸다. 두 가지 기준선:
      (가) 지시 그대로: 칸들의 최대 t 의 95% 분위.
      (나) 칸마다 t 를 자유도(묶음 달 수 − 1) t 분포 꼬리 확률로 바꾼 뒤 칸들의 최소 확률(= 최대 정규 환산 z)의 5% 분위.
           달이 2–3 개뿐인 칸은 t 가 쉽게 수십이 나와 (가)를 끌어올린다 — (나)는 그 칸의 t 를 자유도만큼 깎는다.
    관측 t 도 같은 식으로 다시 내 커널 값과 맞는지 본다."""
    count = month_count.sum(axis=1)
    groups = (month_count > 0).sum(axis=1)
    valid = (count >= 3) & (groups >= 2)
    excess = month_excess[valid].astype(np.float64)
    trades = month_count[valid].astype(np.float64)
    count = count[valid].astype(np.float64)
    degrees = (groups[valid] - 1).astype(np.float64)
    scale = groups[valid] / (groups[valid] - 1.0)
    squared = (excess ** 2).sum(axis=1)
    count_squared = (trades ** 2).sum(axis=1)
    weighted = excess * trades

    def t_of(total, cross):
        mean = total / count[:, None]
        residual = squared[:, None] - 2.0 * mean * cross + mean ** 2 * count_squared[:, None]
        residual = np.maximum(residual, 0.0)
        error = np.sqrt(scale[:, None] * residual) / count[:, None]

        with np.errstate(invalid="ignore", divide="ignore"):
            return np.where(residual > 0, mean / error, np.nan)

    observed = t_of(excess.sum(axis=1)[:, None], weighted.sum(axis=1)[:, None])[:, 0]
    observed_log_p = log_t_upper_matrix(observed[:, None], degrees)[:, 0]
    maxima = np.empty(signs.shape[0])
    minimum_log_p = np.empty(signs.shape[0])
    over = np.empty(signs.shape[0])

    for begin in range(0, signs.shape[0], FLIP_BATCH):
        part = signs[begin:begin + FLIP_BATCH].T.astype(np.float64)
        flipped = t_of(excess @ part, weighted @ part)
        maxima[begin:begin + FLIP_BATCH] = np.nanmax(flipped, axis=0)
        over[begin:begin + FLIP_BATCH] = (flipped > NORMAL_REFERENCE_T).sum(axis=0)
        minimum_log_p[begin:begin + FLIP_BATCH] = log_t_upper_matrix(flipped, degrees).min(axis=0)

    cells = int(valid.sum())
    bar = float(np.quantile(maxima, NULL_QUANTILE))
    log_p_bar = float(np.quantile(minimum_log_p, 1.0 - NULL_QUANTILE))
    summary = {"layer": label, "cells_M": cells, "flips": int(signs.shape[0]),
               "normal_expected_cells_t_over_3_5": cells * 0.5 * math.erfc(NORMAL_REFERENCE_T / math.sqrt(2.0)),
               "flip_mean_cells_t_over_3_5": float(over.mean()), "flip_p95_cells_t_over_3_5": float(np.quantile(over, 0.95)),
               "actual_cells_t_over_3_5": int(np.nansum(observed > NORMAL_REFERENCE_T)),
               "a_max_t_null_p50": float(np.median(maxima)), "a_max_t_null_p95_bar": bar,
               "a_max_t_null_p99": float(np.quantile(maxima, 0.99)), "observed_max_t": float(np.nanmax(observed)),
               "a_cells_over_bar": int(np.nansum(observed > bar)),
               "b_min_log10_p_null_p05": log_p_bar / math.log(10.0),
               "b_bar_normal_equivalent_z": normal_equivalent(log_p_bar),
               "b_null_median_normal_equivalent_z": normal_equivalent(float(np.median(minimum_log_p))),
               "b_cells_over_bar": int((observed_log_p < log_p_bar).sum()),
               "valid_cells_months_median": float(np.median(degrees + 1)),
               "valid_cells_under_24_months": int((degrees + 1 < 24).sum())}
    detail = pd.DataFrame({"position": np.flatnonzero(valid), "t_matrix": observed, "log10_p": observed_log_p / math.log(10.0),
                           "over_bar_a": observed > bar, "over_bar_b": observed_log_p < log_p_bar})
    return detail, summary, maxima, minimum_log_p


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spans", default=",".join(str(span) for span in common.K_VALUES))
    arguments = parser.parse_args()
    spans = [int(value) for value in arguments.spans.split(",")]
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    timings = {}
    panel = common.Panel()
    correction_information = apply_corrections(panel)
    timings["load"] = time.time() - started
    step = time.time()
    panel.build_benchmark()
    panel.build_deciles()
    turnover_slot = np.where(panel.deciles[:, 1] >= 0, panel.deciles[:, 1], 10).astype(np.int64)
    day_turnover = panel.day_turnover
    thresholds, combinations = threshold_grid()
    filters = len(thresholds)
    groups = SLOTS * filters
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
    print(f"준비 끝 {time.time() - started:.0f}초, 문턱 {filters} 개", flush=True)
    rule_count = len(common.RULE_NAMES)
    pooled_statistics = np.zeros((rule_count, groups, 5))
    pooled_month = np.zeros((rule_count, groups, panel.month_count, 3))
    pooled_year = np.zeros((rule_count, groups, panel.year_count, 3))
    fine_month_excess, fine_month_count, cell_frames = [], [], []
    used_total = {}

    for span in spans:
        step = time.time()
        state, latest_low, _ = panel.trend(span)
        statistics, month_sums, year_sums, used = run_span_round3(
            slice_window, slice_period, slice_rule, panel.averages, panel.atrs, panel.open_price, panel.low, panel.close,
            panel.starts, panel.market_position, panel.signal_ok, panel.month_index, panel.year_index, turnover_slot,
            day_turnover, thresholds, state, latest_low, common.DISTANCE_EDGES, panel.bench, panel.signal_start,
            panel.listed_at_end, common.COST, groups, panel.year_count, panel.month_count)
        used_total[span] = int(used.sum())

        for rule in range(rule_count):
            chosen = slice_rule == rule
            pooled_statistics[rule] += statistics[chosen][:, :, :5].sum(axis=0)
            pooled_month[rule] += month_sums[chosen].sum(axis=0)
            pooled_year[rule] += year_sums[chosen].sum(axis=0)

        fine_month_excess.append(month_sums[:, :, :, 1].reshape(-1, panel.month_count).astype(np.float32))
        fine_month_count.append(month_sums[:, :, :, 2].reshape(-1, panel.month_count).astype(np.float32))
        frame = pd.DataFrame(statistics.reshape(-1, STATS), columns=STAT_NAMES)
        group = np.tile(np.arange(groups), slices)
        frame.insert(0, "k", span)
        frame.insert(1, "window", np.repeat([common.WINDOWS[value] for value in slice_window], groups))
        frame.insert(2, "atr_period", np.repeat([common.ATR_PERIODS[value] for value in slice_period], groups))
        frame.insert(3, "rule", np.repeat([common.RULE_NAMES[value] for value in slice_rule], groups))
        frame.insert(4, "turnover_decile", np.where(group // filters < 10, group // filters, -1))
        frame.insert(5, "min_turnover_krw", thresholds[group % filters])
        flat_years = year_sums.reshape(-1, panel.year_count, 3)

        for year_index, year in enumerate(years):
            frame[f"n_{year}"] = flat_years[:, year_index, 0].astype(np.int32)
            frame[f"sum_excess_{year}"] = flat_years[:, year_index, 2].astype(np.float32)

        frame["cell_index"] = np.arange(len(frame)) + sum(len(part) for part in cell_frames)
        cell_frames.append(frame)
        timings[f"k{span}"] = time.time() - step
        print(f"k={span} 끝 {timings[f'k{span}']:.0f}초, 매매 {used_total[span]:,}", flush=True)
        del statistics, month_sums, year_sums

    step = time.time()
    cells = pd.concat(cell_frames, ignore_index=True)
    del cell_frames

    with np.errstate(invalid="ignore", divide="ignore"):
        cells["mean_net"] = cells["sum_net"] / cells["n"]
        cells["mean_excess"] = cells["sum_excess"] / cells["n"]
        cells["share_positive_excess_years"] = cells["years_positive_excess"] / cells["years_with_trades"]

    # 다중비교: 두 층
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((FLIPS, panel.month_count)) < 0.5, -1.0, 1.0)
    fine_excess = np.concatenate(fine_month_excess)
    fine_count = np.concatenate(fine_month_count)
    del fine_month_excess, fine_month_count
    fine_detail, fine_summary, fine_maxima, fine_minimum = flip_null(fine_excess, fine_count, signs,
                                                                     "가장 잘은 칸(k × N × ATR × 규칙 × 분위 × 문턱)")
    del fine_excess, fine_count
    timings["flip_fine"] = time.time() - step
    print(f"잘은 칸 뒤집기 끝 {timings['flip_fine']:.0f}초", flush=True)
    step = time.time()
    pool_excess = pooled_month[:, :, :, 1].reshape(-1, panel.month_count)
    pool_count = pooled_month[:, :, :, 2].reshape(-1, panel.month_count)
    pool_detail, pool_summary, pool_maxima, pool_minimum = flip_null(pool_excess, pool_count, signs,
                                                                     "판정 표 칸(규칙 × 분위 × 문턱, k·N·ATR 합)")
    timings["flip_pool"] = time.time() - step

    # 커널 t 와 행렬 t 대조(같은 식인지), 칸마다 기준선 판정 붙이기
    position = fine_detail["position"].to_numpy()
    difference = float(np.nanmax(np.abs(cells["t_excess"].to_numpy()[position] - fine_detail["t_matrix"].to_numpy())))
    log10_p = np.full(len(cells), np.nan)
    log10_p[position] = fine_detail["log10_p"].to_numpy()
    cells["log10_p"] = log10_p

    for column in ("over_bar_a", "over_bar_b"):
        flags = np.zeros(len(cells), dtype=bool)
        flags[position] = fine_detail[column].to_numpy()
        cells[column] = flags

    cells.to_parquet(OUTPUT / "cells.parquet", index=False)

    # 판정 표 칸
    pool_rows = []

    for rule in range(rule_count):
        for group in range(groups):
            values = pooled_statistics[rule, group]
            month = pooled_month[rule, group]
            year = pooled_year[rule, group]
            count = values[0]
            t_excess = common.cluster_t(month[:, 1], month[:, 2], values[2], count) if count > 0 else np.nan
            t_net = common.cluster_t(month[:, 0], month[:, 2], values[1], count) if count > 0 else np.nan
            with_trades = int((year[:, 0] > 0).sum())
            row = {"rule": common.RULE_NAMES[rule], "turnover_decile": group // filters if group // filters < 10 else -1,
                   "min_turnover_krw": thresholds[group % filters], "n": count,
                   "mean_net": values[1] / count if count else np.nan, "mean_excess": values[2] / count if count else np.nan,
                   "win_rate": values[3] / count * 100.0 if count else np.nan, "mean_hold": values[4] / count if count else np.nan,
                   "t_net": t_net, "t_excess": t_excess, "months_with_trades": int((month[:, 2] > 0).sum()),
                   "years_with_trades": with_trades,
                   "share_positive_excess_years": ((year[:, 0] > 0) & (year[:, 2] > 0)).sum() / with_trades if with_trades else np.nan}

            for year_index, year_value in enumerate(years):
                row[f"mean_excess_{year_value}"] = year[year_index, 2] / year[year_index, 0] if year[year_index, 0] else np.nan

            pool_rows.append(row)

    pooled = pd.DataFrame(pool_rows)
    position = pool_detail["position"].to_numpy()
    pool_t_check = float(np.nanmax(np.abs(pooled["t_excess"].to_numpy()[position] - pool_detail["t_matrix"].to_numpy())))
    log10_p = np.full(len(pooled), np.nan)
    log10_p[position] = pool_detail["log10_p"].to_numpy()
    pooled["log10_p"] = log10_p

    for column in ("over_bar_a", "over_bar_b"):
        flags = np.zeros(len(pooled), dtype=bool)
        flags[position] = pool_detail[column].to_numpy()
        pooled[column] = flags

    unlimited = pooled[pooled["min_turnover_krw"] == 0][["rule", "turnover_decile", "n"]].rename(columns={"n": "n_unlimited"})
    pooled = pooled.merge(unlimited, on=["rule", "turnover_decile"], how="left")
    pooled["share_kept_vs_unlimited"] = pooled["n"] / pooled["n_unlimited"]
    common.write_csv(OUTPUT / "pooled_cells.csv", pooled)

    # 옆 분위 부호(같은 규칙·문턱, 분위 d−1·d+1 의 평균 초과수익 부호가 같은지)
    lookup = pooled.set_index(["rule", "min_turnover_krw", "turnover_decile"])["mean_excess"]
    decision_rows = []

    for _, row in pooled.iterrows():
        decile = int(row["turnover_decile"])
        neighbours = []

        if decile >= 0:
            for other in (decile - 1, decile + 1):
                key = (row["rule"], row["min_turnover_krw"], other)

                if 0 <= other <= 9 and key in lookup.index and np.isfinite(lookup[key]):
                    neighbours.append(np.sign(lookup[key]))

        sign = np.sign(row["mean_excess"]) if np.isfinite(row["mean_excess"]) else np.nan
        same = [value == sign for value in neighbours]

        for combination in combinations:
            if combination["min_turnover_krw"] != row["min_turnover_krw"]:
                continue

            ratio_cap = combination["ratio_cap_percent"]
            decision_rows.append({"rule": row["rule"], "turnover_decile": decile,
                                  "order_amount_krw": combination["order_amount_krw"],
                                  "ratio_cap_percent": "none" if ratio_cap is None else ratio_cap,
                                  "min_turnover_krw": row["min_turnover_krw"], "n": row["n"],
                                  "share_kept_vs_unlimited": row["share_kept_vs_unlimited"],
                                  "mean_excess": row["mean_excess"], "mean_net": row["mean_net"], "t_excess": row["t_excess"],
                                  "months_with_trades": row["months_with_trades"],
                                  "share_positive_excess_years": row["share_positive_excess_years"],
                                  "neighbour_deciles": len(neighbours), "neighbour_same_sign": int(sum(same)),
                                  "neighbours_all_same_sign": bool(neighbours) and all(same),
                                  "log10_p": row["log10_p"], "over_bar_a": bool(row["over_bar_a"]),
                                  "over_bar_b": bool(row["over_bar_b"])})

    decision = pd.DataFrame(decision_rows).sort_values(["rule", "turnover_decile", "order_amount_krw", "min_turnover_krw"])
    common.write_csv(OUTPUT / "decision_table.csv", decision)

    # 기준선을 넘는 칸이 사지 못하는 구간(상한 없음) 밖에 있는지: 문턱·분위·규칙별로 센다
    passing = {}

    for name, frame in (("fine", cells), ("pool", pooled)):
        for bar_name in ("over_bar_a", "over_bar_b"):
            chosen = frame[frame[bar_name] & (frame["n"] > 0)]
            passing[f"{name}_{bar_name}"] = {
                "cells_over_bar": int(len(chosen)),
                "in_unlimited_filter": int((chosen["min_turnover_krw"] == 0).sum()),
                "in_capped_filters": int((chosen["min_turnover_krw"] > 0).sum()),
                "positive_mean_excess": int((chosen["mean_excess"] > 0).sum()),
                "by_min_turnover_krw": {str(int(key)): int(value) for key, value in chosen.groupby("min_turnover_krw").size().items()},
                "by_turnover_decile": {str(int(key)): int(value) for key, value in chosen.groupby("turnover_decile").size().items()},
                "by_rule": {str(key): int(value) for key, value in chosen.groupby("rule", observed=True).size().items()}}

    common.write_csv(OUTPUT / "max_t_null.csv", pd.DataFrame({
        "flip": np.arange(FLIPS), "fine_max_t": fine_maxima, "fine_min_log10_p": fine_minimum / math.log(10.0),
        "pool_max_t": pool_maxima, "pool_min_log10_p": pool_minimum / math.log(10.0)}))
    common.write_csv(OUTPUT / "fill_excluded.csv",
                     pooled[["rule", "turnover_decile", "min_turnover_krw", "n", "n_unlimited", "share_kept_vs_unlimited"]])
    multiple = {"fine": fine_summary, "pool": pool_summary, "passing": passing,
                "sign_flip": "달마다 ±1 한 부호를 모든 칸에 같이 곱한다(칸 사이 상관 보존). 시드 common.SEED",
                "bar_a": "지시 그대로 — 칸들의 최대 t 95% 분위",
                "bar_b": "칸 t 를 자유도(묶음 달 수 − 1) t 분포 꼬리 확률로 바꾼 최소 확률의 5% 분위(정규 환산 z 로도 적음)",
                "kernel_vs_matrix_t_max_abs_difference": {"fine": difference, "pool": pool_t_check}}
    common.write_json(OUTPUT / "multiple_comparison.json", multiple)

    # 2회차 대조(상한 없음 = 2회차와 같은 매매 조건, 칸 묶음만 다름)
    second = pd.read_csv(common.STUDY / "results" / "round2" / "axis_by_rule.csv")
    second = second[second["axis"] == "turnover_decile"][["rule", "value", "n", "mean_excess"]]
    timings["write"] = time.time() - step
    timings["total"] = time.time() - started
    payload = {"reproduction": panel.reproduction(), "corrections": correction_information,
               "corrections_path": "research/studies/38_trend_support_realtime/data/raw_close_corrections.parquet",
               "timings_seconds": timings, "spans": spans, "trades_by_k": used_total, "cells_rows": len(cells),
               "thresholds_krw": thresholds.tolist(), "combinations": combinations,
               "round2_turnover_axis": second.to_dict("records"),
               "hand_set": {"order_amounts": "100만·500만·2,000만·1억 원 — 내가 정한 격자, 고르는 데 쓰지 않음",
                            "ratio_caps": "0.5·1·2·5·10 %·상한 없음 — 내가 정한 격자, 고르는 데 쓰지 않음",
                            "flips": "1,000 회(지시), 기준선 (가) 최대 t 의 95% 분위(지시), (나) 자유도 보정 최소 확률 5% 분위(내가 더함)",
                            "positive_year": "그해 매매 1건 이상이면 센다(내가 정함)",
                            "inner_distance": "거리 안쪽 12 구간만 매매(2회차 주 표와 같은 범위)"}}
    common.write_json(OUTPUT / "round3_metrics.json", payload)
    print(f"전체 {timings['total']:.0f}초, 기준선 (가) 잘은 {fine_summary['a_max_t_null_p95_bar']:.2f}·판정 "
          f"{pool_summary['a_max_t_null_p95_bar']:.2f}, (나) z 잘은 {fine_summary['b_bar_normal_equivalent_z']:.2f}·판정 "
          f"{pool_summary['b_bar_normal_equivalent_z']:.2f}", flush=True)


if __name__ == "__main__":
    main()
