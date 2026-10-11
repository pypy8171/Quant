"""스터디 38 4회차 — 문턱·분위를 매수 전에 아는 값으로 바꾸고, 동시호가 충격 비용을 칸으로 넣고, 종가 되튐을 진단한다.

1·2·3회차 산출은 건드리지 않고 results/round4/ 에만 쓴다. 매수는 3회차와 같다 — 신호일 종가(동시호가) 매수, 다음 날 아님.
매도 규칙 E1·E2·E3·E4 j=1–5, 비용 common.COST, 비교 평균(B 행렬), 거리 안쪽 12 구간, 칸 안 보유 중 무시(실제 산 매매 기준)도 같다.
거래대금은 3회차 보정표(data/raw_close_corrections.parquet, round3_factor_fix.py)를 쓴다.

바뀐 것(조정 지시 2026-10-10 4회차):
  1) 문턱 기준 거래대금 세 판: 그날(3회차) / (a) 전날까지 20일 평균 / (b) 전날. 주문 비율 = 주문 금액 ÷ 기준 거래대금.
     분위 기준 두 판: 그날 포함 20일 평균(3회차) / 전날까지 20일 평균. 3 × 2 = 6 판(variant).
     "전날" = 그 종목의 바로 앞 거래 행(정지일은 행이 없다).
  2) 동시호가 충격 비용: 체결가 불리분 = 계수 × 주문 비율. 매수가 = 종가 × (1 + 계수 × 매수일 비율),
     매도가 = 매도가 × max(0, 1 − 계수 × 매도일 비율). 매도일 비율은 같은 기준의 매도일 거래대금으로 낸다(값이 없으면 매수일 비율).
     기준 거래대금이 0·없음이면 비율은 무한대 → 계수 > 0 칸에서 그 매매는 −100%. 계수 격자 {0, 0.1, 0.3, 1.0}(내가 정한 격자,
     고르는 데 쓰지 않음). 비교 평균(B)에는 충격 비용을 붙이지 않는다. 계수는 매매 여부를 바꾸지 않는다(같은 매매, 수익만 다름).
  3) 종가 되튐 진단(체결 규칙은 바꾸지 않음): 매수일 종가 위치 = (종가 − 저가) ÷ (고가 − 저가), 하단 근처 = 0.2 이하(내가 정함).
     첫날 초과수익 = 매수 다음 행 종가까지 순수익 − 같은 날짜 구간 B. 전체 초과수익 중 첫날 몫을 분위별로 낸다.
  4) 대용값 행(보정표 status = proxy_adjusted_close_times_volume)에서 나온 매수를 뺀 판(판 6)을 판정 판(판 4)과 같은 조건으로 돌려
     차이를 낸다. 판정에는 넣은 판을 쓴다. 판 6 은 다중비교 칸 수에 넣지 않는다.
  5) 다중비교: 3회차와 같은 방식(월 묶음 부호 뒤집기 1,000 회, 달마다 한 부호를 모든 칸에 같이 곱함). 판정 표 칸 =
     판(6) × 규칙(8) × 분위(10 + 값 없음) × 조합(주문 금액 4 × 상한 6 = 24) × 계수(4). 기준선 (가) 최대 t 95% 분위,
     (나) 자유도 보정 최소 확률 5% 분위. 가장 잘은 칸(+k·N·ATR)은 판정 판·계수 0.3 만 뒤집는다(전 판은 1,400 만 칸 × 202 달이라 메모리에 안 든다).
  6) 판정(지시): 판 4(문턱 (a)·분위 전날 기준), 계수 0.3, 주문 1억 원·상한 10% 이하에서 기준선 (가)를 넘고 옆 분위 부호 같고
     양수 연도 비율 0.7 이상이고 평균 초과수익 > 0 인 칸이 남는지. 0.3·0.7 은 지시에서 정한 판정 숫자. 다른 계수·판도 같은 표에 낸다.

산출(results/round4/):
  decision_table.csv       판 × 규칙 × 분위 × 주문 금액 × 상한 × 계수 — n·평균 초과수익·t·양수 연도 비율·옆 분위 부호·기준선 넘음
  judgement.csv            판정 조건별(판 × 계수 × 주문 금액 × 상한) 남는 칸 수
  bounce_diagnostic.csv    판 × 규칙 × 분위 × 조합: 종가 위치 평균·하단 근처 비율·첫날 초과수익·전체 초과수익·첫날 몫
  proxy_difference.csv     판 4 대 판 6(대용값 매수 뺌): n·평균 초과수익·t 와 차이
  cells_judgement.parquet  가장 잘은 칸(판 4, 계수 0.3)
  max_t_null.csv           층별 뒤집기 1,000 회 최대 t·최소 확률
  multiple_comparison.json 층별 M·우연 기대 칸 수·실제 t > 3.5 칸 수·기준선·넘은 칸 수
  round4_metrics.json      재현 정보·시간·3회차 재현 대조
재실행: py research/studies/38_trend_support_realtime/round4_test.py
"""
from __future__ import annotations

import argparse
import math
import time

import numpy as np
import pandas as pd
from numba import njit, prange

import common
import round3_test

OUTPUT = common.STUDY / "results" / "round4"
SLOTS = round3_test.SLOTS
ORDER_AMOUNTS = round3_test.ORDER_AMOUNTS          # 내가 정한 격자, 고르는 데 쓰지 않음
RATIO_CAPS = round3_test.RATIO_CAPS                # 내가 정한 격자, 고르는 데 쓰지 않음
IMPACT_COEFFICIENTS = (0.0, 0.1, 0.3, 1.0)         # 체결가 불리분 = 계수 × 주문 비율. 내가 정한 격자, 고르는 데 쓰지 않음
LOW_CLOSE_LINE = 0.2                               # 종가 위치 하단 근처 기준(내가 정함, 진단에만 씀)
JUDGE_VARIANT = 4
JUDGE_COEFFICIENT = 0.3                            # 판정 계수(지시)
JUDGE_POSITIVE_YEARS = 0.7                         # 판정 양수 연도 비율(지시)
JUDGE_AMOUNT = 100_000_000.0                       # 판정 주문 금액(지시)
JUDGE_CAP_MAX = 10.0                               # 판정 상한 이하(지시)
COEFFICIENT_FIELDS = 9    # n, sum_net, sum_excess, wins, t_net, t_excess, years_with_trades, years_positive_excess, years_positive_net
SHARED_FIELDS = 6         # sum_hold, sum_close_location, n_close_location, n_low_close, sum_day1_excess, n_day1
VARIANTS = (  # (문턱 기준, 분위 기준, 대용값 매수 뺌)
    ("same_day", "same_day_20d", False),
    ("previous_20d_average", "same_day_20d", False),
    ("previous_day", "same_day_20d", False),
    ("same_day", "previous_20d", False),
    ("previous_20d_average", "previous_20d", False),
    ("previous_day", "previous_20d", False),
    ("previous_20d_average", "previous_20d", True),
)


def combination_grid() -> list[dict]:
    rows = []

    for amount in ORDER_AMOUNTS:
        for ratio_cap in RATIO_CAPS:
            rows.append({"order_amount_krw": amount, "ratio_cap_percent": ratio_cap,
                         "min_turnover_krw": 0.0 if ratio_cap is None else amount / (ratio_cap / 100.0)})

    return rows


def shift_within_stock(values: np.ndarray, starts: np.ndarray) -> np.ndarray:
    shifted = np.r_[np.nan, values[:-1]]
    shifted[starts[:-1]] = np.nan
    return shifted


@njit(cache=True)
def run_slice_round4(rule, average, atr, open_price, high, low, close, starts, market_position, signal_ok, month_index,
                     year_index, turnover_slot, base_turnover, amounts, minimums, coefficients, state, latest_low,
                     distance_edges, bench, signal_start, cost, low_close_line, statistics, shared, month_sums, month_count,
                     year_sums):
    """한 (k, N, ATR, 규칙) 의 칸(분위 × 조합)별 합. statistics[g, c] 계수별, shared[g] 계수 무관 진단,
    month_sums[g, c, m] = (순수익 합, 초과수익 합), month_count[g, m] = 매매 수, year_sums[g, c, y] = (매매 수, 순수익 합, 초과수익 합)."""
    combinations = amounts.shape[0]
    coefficient_count = coefficients.shape[0]
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

            base = turnover_slot[row] * combinations
            computed = False
            exit_offset = length
            exit_row = row
            gross = 1.0
            market = 0.0
            hold = 0
            location = np.nan
            day1 = np.nan

            for combination in range(combinations):
                minimum = minimums[combination]

                if minimum > 0.0 and not (base_turnover[row] >= minimum):
                    continue

                group = base + combination

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

                    gross = exit_price / close[row]
                    hold = exit_row - row
                    market = bench[market_position[row] - signal_start, market_position[exit_row] - signal_start]

                    if high[row] > low[row]:
                        location = (close[row] - low[row]) / (high[row] - low[row])

                    if row + 1 < last:
                        day1 = ((close[row + 1] / close[row] * cost - 1.0) * 100.0
                                - bench[market_position[row] - signal_start, market_position[row + 1] - signal_start])

                next_allowed[group] = exit_offset + 1 if exit_offset < length else length + 1
                amount = amounts[combination]
                buy_base = base_turnover[row]
                buy_ratio = amount / buy_base if (np.isfinite(buy_base) and buy_base > 0) else np.inf
                sell_base = base_turnover[exit_row]
                sell_ratio = amount / sell_base if (np.isfinite(sell_base) and sell_base > 0) else buy_ratio
                month = month_index[row]
                year = year_index[row]
                month_count[group, month] += 1.0
                cell_shared = shared[group]
                cell_shared[0] += hold

                if np.isfinite(location):
                    cell_shared[1] += location
                    cell_shared[2] += 1.0

                    if location <= low_close_line:
                        cell_shared[3] += 1.0

                if np.isfinite(day1):
                    cell_shared[4] += day1
                    cell_shared[5] += 1.0

                for index in range(coefficient_count):
                    coefficient = coefficients[index]

                    if coefficient == 0.0:
                        multiplier = 1.0
                    else:
                        buy_multiplier = 1.0 + coefficient * buy_ratio
                        sell_multiplier = max(0.0, 1.0 - coefficient * sell_ratio)
                        multiplier = sell_multiplier / buy_multiplier if np.isfinite(buy_multiplier) else 0.0

                    net = (gross * multiplier * cost - 1.0) * 100.0
                    excess = net - market
                    cell = statistics[group, index]
                    cell[0] += 1.0
                    cell[1] += net
                    cell[2] += excess
                    cell[3] += 1.0 if net > 0 else 0.0
                    month_sums[group, index, month, 0] += net
                    month_sums[group, index, month, 1] += excess
                    year_sums[group, index, year, 0] += 1.0
                    year_sums[group, index, year, 1] += net
                    year_sums[group, index, year, 2] += excess

                used += 1

    for group in range(groups):
        for index in range(coefficient_count):
            cell = statistics[group, index]

            if cell[0] > 0:
                cell[4] = common.cluster_t(month_sums[group, index, :, 0], month_count[group], cell[1], cell[0])
                cell[5] = common.cluster_t(month_sums[group, index, :, 1], month_count[group], cell[2], cell[0])

                for year in range(year_sums.shape[2]):
                    if year_sums[group, index, year, 0] > 0:
                        cell[6] += 1.0

                        if year_sums[group, index, year, 2] > 0:
                            cell[7] += 1.0

                        if year_sums[group, index, year, 1] > 0:
                            cell[8] += 1.0
            else:
                cell[4] = np.nan
                cell[5] = np.nan

    return used


@njit(parallel=True, cache=True)
def run_span_round4(slice_window, slice_period, slice_rule, averages, atrs, open_price, high, low, close, starts,
                    market_position, signal_ok, month_index, year_index, turnover_slot, base_turnover, amounts, minimums,
                    coefficients, state, latest_low, distance_edges, bench, signal_start, cost, low_close_line, groups,
                    year_count, month_count):
    slices = slice_window.shape[0]
    coefficient_count = coefficients.shape[0]
    statistics = np.zeros((slices, groups, coefficient_count, COEFFICIENT_FIELDS))
    shared = np.zeros((slices, groups, SHARED_FIELDS))
    month_sums = np.zeros((slices, groups, coefficient_count, month_count, 2))
    month_counts = np.zeros((slices, groups, month_count))
    year_sums = np.zeros((slices, groups, coefficient_count, year_count, 3))
    used = np.zeros(slices, dtype=np.int64)

    for index in prange(slices):
        used[index] = run_slice_round4(slice_rule[index], averages[slice_window[index]], atrs[slice_period[index]],
                                       open_price, high, low, close, starts, market_position, signal_ok, month_index,
                                       year_index, turnover_slot, base_turnover, amounts, minimums, coefficients, state,
                                       latest_low, distance_edges, bench, signal_start, cost, low_close_line, statistics[index],
                                       shared[index], month_sums[index], month_counts[index], year_sums[index])

    return statistics, shared, month_sums, month_counts, year_sums, used


def neighbour_columns(frame: pd.DataFrame, keys: list[str]) -> pd.DataFrame:
    """같은 (keys) 안에서 분위 d−1·d+1 의 평균 초과수익 부호가 이 칸과 같은지. 값 없음 분위(−1)는 옆 분위가 없다."""
    sign = np.sign(frame["mean_excess"])
    base = frame[keys + ["turnover_decile"]].copy()
    base["sign"] = sign
    count = np.zeros(len(frame), dtype=np.int64)
    same = np.zeros(len(frame), dtype=np.int64)

    for step in (-1, 1):
        other = base.copy()
        other["turnover_decile"] = other["turnover_decile"] - step
        other = other.rename(columns={"sign": "other_sign"})
        joined = base.merge(other, on=keys + ["turnover_decile"], how="left")
        valid = (frame["turnover_decile"].to_numpy() >= 0) & np.isfinite(joined["other_sign"].to_numpy())
        valid &= (frame["turnover_decile"].to_numpy() + step >= 0) & (frame["turnover_decile"].to_numpy() + step <= 9)
        count += valid
        same += valid & (joined["other_sign"].to_numpy() == sign.to_numpy())

    frame = frame.copy()
    frame["neighbour_deciles"] = count
    frame["neighbour_same_sign"] = same
    frame["neighbours_all_same_sign"] = (count > 0) & (same == count)
    return frame


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spans", default=",".join(str(span) for span in common.K_VALUES))
    arguments = parser.parse_args()
    spans = [int(value) for value in arguments.spans.split(",")]
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    timings = {}
    panel = common.Panel()
    correction_information = round3_test.apply_corrections(panel)
    status = pd.read_parquet(round3_test.CORRECTIONS, columns=["status"])["status"].astype(str).to_numpy()
    proxy_row = status == "proxy_adjusted_close_times_volume"
    timings["load"] = time.time() - started
    step = time.time()
    panel.build_benchmark()
    same_day_20d = panel.turnover20
    previous_20d = shift_within_stock(same_day_20d, panel.starts)
    previous_day = shift_within_stock(panel.day_turnover, panel.starts)
    bases = {"same_day": panel.day_turnover, "previous_20d_average": previous_20d, "previous_day": previous_day}
    slots = {}

    for name, values in (("same_day_20d", same_day_20d), ("previous_20d", previous_20d)):
        panel.turnover20 = values
        deciles = panel.build_deciles()
        slots[name] = np.where(deciles[:, 1] >= 0, deciles[:, 1], 10).astype(np.int64)

    panel.turnover20 = same_day_20d
    combinations = combination_grid()
    amounts = np.array([row["order_amount_krw"] for row in combinations], np.float64)
    minimums = np.array([row["min_turnover_krw"] for row in combinations], np.float64)
    coefficients = np.array(IMPACT_COEFFICIENTS, np.float64)
    combination_count = len(combinations)
    coefficient_count = len(coefficients)
    groups = SLOTS * combination_count
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
    rule_count = len(common.RULE_NAMES)
    variant_count = len(VARIANTS)
    timings["prepare"] = time.time() - step
    print(f"준비 끝 {time.time() - started:.0f}초, 판 {variant_count}, 조합 {combination_count}, 계수 {coefficient_count}", flush=True)
    pooled_statistics = np.zeros((variant_count, rule_count, groups, coefficient_count, 4))
    pooled_shared = np.zeros((variant_count, rule_count, groups, SHARED_FIELDS))
    pooled_month = np.zeros((variant_count, rule_count, groups, coefficient_count, panel.month_count, 2))
    pooled_count = np.zeros((variant_count, rule_count, groups, panel.month_count))
    pooled_year = np.zeros((variant_count, rule_count, groups, coefficient_count, panel.year_count, 3))
    judge_index = int(np.flatnonzero(coefficients == JUDGE_COEFFICIENT)[0])
    fine_excess, fine_count, fine_frames = [], [], []
    used_total = {}

    for span in spans:
        state, latest_low, _ = panel.trend(span)

        for variant, (threshold_basis, decile_basis, drop_proxy) in enumerate(VARIANTS):
            step = time.time()
            signal_ok = panel.signal_ok & ~proxy_row if drop_proxy else panel.signal_ok
            base_turnover = bases[threshold_basis]
            turnover_slot = slots[decile_basis]
            statistics, shared, month_sums, month_counts, year_sums, used = run_span_round4(
                slice_window, slice_period, slice_rule, panel.averages, panel.atrs, panel.open_price, panel.high, panel.low,
                panel.close, panel.starts, panel.market_position, signal_ok, panel.month_index, panel.year_index,
                turnover_slot, base_turnover, amounts, minimums, coefficients, state, latest_low, common.DISTANCE_EDGES,
                panel.bench, panel.signal_start, common.COST, LOW_CLOSE_LINE, groups, panel.year_count, panel.month_count)
            used_total[f"{variant}_{span}"] = int(used.sum())

            for rule in range(rule_count):
                chosen = slice_rule == rule
                pooled_statistics[variant, rule] += statistics[chosen][..., :4].sum(axis=0)
                pooled_shared[variant, rule] += shared[chosen].sum(axis=0)
                pooled_month[variant, rule] += month_sums[chosen].sum(axis=0)
                pooled_count[variant, rule] += month_counts[chosen].sum(axis=0)
                pooled_year[variant, rule] += year_sums[chosen].sum(axis=0)

            if variant == JUDGE_VARIANT:
                fine_excess.append(month_sums[:, :, judge_index, :, 1].reshape(-1, panel.month_count).astype(np.float32))
                fine_count.append(month_counts.reshape(-1, panel.month_count).astype(np.float32))
                group = np.tile(np.arange(groups), slices)
                frame = pd.DataFrame({
                    "k": span, "window": np.repeat([common.WINDOWS[value] for value in slice_window], groups),
                    "atr_period": np.repeat([common.ATR_PERIODS[value] for value in slice_period], groups),
                    "rule": np.repeat([common.RULE_NAMES[value] for value in slice_rule], groups),
                    "turnover_decile": np.where(group // combination_count < 10, group // combination_count, -1),
                    "order_amount_krw": amounts[group % combination_count],
                    "min_turnover_krw": minimums[group % combination_count]})
                values = statistics[:, :, judge_index, :].reshape(-1, COEFFICIENT_FIELDS)

                for column, name in enumerate(("n", "sum_net", "sum_excess", "wins", "t_net", "t_excess", "years_with_trades",
                                               "years_positive_excess", "years_positive_net")):
                    frame[name] = values[:, column]

                fine_frames.append(frame)

            timings[f"v{variant}_k{span}"] = time.time() - step
            print(f"판 {variant} k={span} 끝 {timings[f'v{variant}_k{span}']:.0f}초", flush=True)
            del statistics, shared, month_sums, month_counts, year_sums

    # 판정 표 칸
    step = time.time()
    index = pd.MultiIndex.from_product([range(variant_count), range(rule_count), range(groups), range(coefficient_count)],
                                       names=["variant", "rule_index", "group", "coefficient_index"])
    pool = index.to_frame(index=False)
    flat = pooled_statistics.reshape(-1, 4)
    pool["n"] = flat[:, 0]

    with np.errstate(invalid="ignore", divide="ignore"):
        pool["mean_net"] = flat[:, 1] / flat[:, 0]
        pool["mean_excess"] = flat[:, 2] / flat[:, 0]
        pool["win_rate"] = flat[:, 3] / flat[:, 0] * 100.0

    pool["threshold_basis"] = [VARIANTS[value][0] for value in pool["variant"]]
    pool["decile_basis"] = [VARIANTS[value][1] for value in pool["variant"]]
    pool["proxy_buys_dropped"] = [VARIANTS[value][2] for value in pool["variant"]]
    pool["rule"] = np.array(common.RULE_NAMES)[pool["rule_index"]]
    pool["turnover_decile"] = np.where(pool["group"] // combination_count < 10, pool["group"] // combination_count, -1)
    pool["order_amount_krw"] = amounts[pool["group"] % combination_count]
    pool["ratio_cap_percent"] = [("none" if combinations[value]["ratio_cap_percent"] is None
                                  else combinations[value]["ratio_cap_percent"]) for value in pool["group"] % combination_count]
    pool["min_turnover_krw"] = minimums[pool["group"] % combination_count]
    pool["impact_coefficient"] = coefficients[pool["coefficient_index"]]
    month_net = pooled_month[..., 0].reshape(-1, panel.month_count)
    month_excess = pooled_month[..., 1].reshape(-1, panel.month_count)
    month_count = np.repeat(pooled_count.reshape(-1, panel.month_count)[:, None, :], coefficient_count, axis=1).reshape(
        -1, panel.month_count)
    year_flat = pooled_year.reshape(-1, panel.year_count, 3)
    totals = flat
    pool["t_net"] = [common.cluster_t(month_net[row], month_count[row], totals[row, 1], totals[row, 0]) for row in range(len(pool))]
    pool["t_excess"] = [common.cluster_t(month_excess[row], month_count[row], totals[row, 2], totals[row, 0])
                        for row in range(len(pool))]
    pool["months_with_trades"] = (month_count > 0).sum(axis=1)
    with_trades = (year_flat[:, :, 0] > 0).sum(axis=1)
    positive = ((year_flat[:, :, 0] > 0) & (year_flat[:, :, 2] > 0)).sum(axis=1)

    with np.errstate(invalid="ignore", divide="ignore"):
        pool["years_with_trades"] = with_trades
        pool["share_positive_excess_years"] = np.where(with_trades > 0, positive / np.maximum(with_trades, 1), np.nan)

        for year_index, year_value in enumerate(years):
            pool[f"mean_excess_{year_value}"] = year_flat[:, year_index, 2] / year_flat[:, year_index, 0]

    unlimited = pool[pool["ratio_cap_percent"] == "none"][["variant", "rule", "turnover_decile", "order_amount_krw",
                                                           "impact_coefficient", "n"]].rename(columns={"n": "n_unlimited"})
    pool = pool.merge(unlimited, on=["variant", "rule", "turnover_decile", "order_amount_krw", "impact_coefficient"], how="left")
    pool["share_kept_vs_unlimited"] = pool["n"] / pool["n_unlimited"]
    pool = neighbour_columns(pool, ["variant", "rule", "order_amount_krw", "ratio_cap_percent", "impact_coefficient"])

    # 다중비교: 판 0–5 전부(판 6 은 확인용이라 뺀다)
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, panel.month_count)) < 0.5, -1.0, 1.0)
    candidate = (pool["variant"] < 6).to_numpy()
    detail, pool_summary, pool_maxima, pool_minimum = round3_test.flip_null(
        month_excess[candidate], month_count[candidate], signs, "판정 표 칸(판 0–5 × 규칙 × 분위 × 조합 × 계수, k·N·ATR 합)")
    distinct = pool[candidate & (pool["n"] > 0)].drop_duplicates(
        ["variant", "rule", "turnover_decile", "min_turnover_krw", "n", "mean_excess"])
    pool_summary["distinct_cells_M"] = int(len(distinct))
    candidate_positions = np.flatnonzero(candidate)
    pool["log10_p"] = np.nan
    pool["over_bar_a"] = False
    pool["over_bar_b"] = False
    rows = candidate_positions[detail["position"].to_numpy()]
    pool.loc[rows, "log10_p"] = detail["log10_p"].to_numpy()
    pool.loc[rows, "over_bar_a"] = detail["over_bar_a"].to_numpy()
    pool.loc[rows, "over_bar_b"] = detail["over_bar_b"].to_numpy()
    t_check = float(np.nanmax(np.abs(pool.loc[rows, "t_excess"].to_numpy() - detail["t_matrix"].to_numpy())))
    bar_a = pool_summary["a_max_t_null_p95_bar"]
    # 판 6 은 같은 기준선으로 표시만 한다
    sixth = pool["variant"] == 6
    pool.loc[sixth, "over_bar_a"] = pool.loc[sixth, "t_excess"] > bar_a
    per_variant = {}

    for variant in range(6):
        chosen = (pool["variant"] == variant).to_numpy()
        _, summary, _, _ = round3_test.flip_null(month_excess[chosen], month_count[chosen], signs, f"판 {variant} 만")
        per_variant[variant] = {"variant": VARIANTS[variant], "cells_M": summary["cells_M"],
                                "a_max_t_null_p95_bar": summary["a_max_t_null_p95_bar"],
                                "b_bar_normal_equivalent_z": summary["b_bar_normal_equivalent_z"],
                                "actual_cells_t_over_3_5": summary["actual_cells_t_over_3_5"]}

    timings["flip_pool"] = time.time() - step
    print(f"판정 표 칸 뒤집기 끝, 기준선 (가) {bar_a:.2f}", flush=True)

    step = time.time()
    fine_matrix = np.concatenate(fine_excess)
    fine_counts = np.concatenate(fine_count)
    del fine_excess, fine_count
    fine_detail, fine_summary, fine_maxima, fine_minimum = round3_test.flip_null(
        fine_matrix, fine_counts, signs, "가장 잘은 칸(판 4, 계수 0.3: k × N × ATR × 규칙 × 분위 × 조합)")
    del fine_matrix, fine_counts
    cells = pd.concat(fine_frames, ignore_index=True)
    del fine_frames

    with np.errstate(invalid="ignore", divide="ignore"):
        cells["mean_excess"] = cells["sum_excess"] / cells["n"]
        cells["share_positive_excess_years"] = cells["years_positive_excess"] / cells["years_with_trades"]

    for column in ("log10_p", "over_bar_a", "over_bar_b"):
        values = np.full(len(cells), np.nan if column == "log10_p" else False, dtype=np.float64 if column == "log10_p" else bool)
        values[fine_detail["position"].to_numpy()] = fine_detail[column].to_numpy()
        cells[column] = values

    cells.to_parquet(OUTPUT / "cells_judgement.parquet", index=False)
    timings["flip_fine"] = time.time() - step
    print(f"잘은 칸 뒤집기 끝 {timings['flip_fine']:.0f}초", flush=True)

    # 표 쓰기
    step = time.time()
    columns = ["variant", "threshold_basis", "decile_basis", "proxy_buys_dropped", "rule", "turnover_decile", "order_amount_krw",
               "ratio_cap_percent", "min_turnover_krw", "impact_coefficient", "n", "n_unlimited", "share_kept_vs_unlimited",
               "mean_net", "mean_excess", "win_rate", "t_net", "t_excess", "months_with_trades", "years_with_trades",
               "share_positive_excess_years", "neighbour_deciles", "neighbour_same_sign", "neighbours_all_same_sign",
               "log10_p", "over_bar_a", "over_bar_b"] + [f"mean_excess_{year}" for year in years]
    decision = pool[columns].sort_values(["variant", "rule", "turnover_decile", "order_amount_krw", "min_turnover_krw",
                                          "impact_coefficient"])
    common.write_csv(OUTPUT / "decision_table.csv", decision)
    decision["capped"] = decision["ratio_cap_percent"] != "none"
    decision["passes"] = (decision["over_bar_a"] & decision["neighbours_all_same_sign"]
                          & (decision["share_positive_excess_years"] >= JUDGE_POSITIVE_YEARS) & (decision["mean_excess"] > 0))
    judgement = decision.groupby(["variant", "threshold_basis", "decile_basis", "impact_coefficient", "order_amount_krw",
                                  "ratio_cap_percent"]).agg(
        cells=("n", "size"), over_bar_a=("over_bar_a", "sum"), over_bar_b=("over_bar_b", "sum"), passes=("passes", "sum"),
        best_t=("t_excess", "max")).reset_index()
    common.write_csv(OUTPUT / "judgement.csv", judgement)
    caps = decision["ratio_cap_percent"].map(lambda value: np.inf if value == "none" else float(value))
    judged = decision[(decision["variant"] == JUDGE_VARIANT) & (decision["impact_coefficient"] == JUDGE_COEFFICIENT)
                      & (decision["order_amount_krw"] == JUDGE_AMOUNT) & (caps <= JUDGE_CAP_MAX)]
    judged_pass = judged[judged["passes"]]

    shared = pooled_shared.reshape(-1, SHARED_FIELDS)
    diagnostic = pool[pool["impact_coefficient"] == 0.0][["variant", "threshold_basis", "decile_basis", "proxy_buys_dropped",
                                                          "rule", "turnover_decile", "order_amount_krw", "ratio_cap_percent",
                                                          "min_turnover_krw", "n", "mean_excess", "t_excess"]].copy()
    position = (pool["impact_coefficient"] == 0.0).to_numpy()
    shared_rows = shared[(pool.loc[position, "variant"].to_numpy() * rule_count + pool.loc[position, "rule_index"].to_numpy())
                         * groups + pool.loc[position, "group"].to_numpy()]

    with np.errstate(invalid="ignore", divide="ignore"):
        diagnostic["mean_hold"] = shared_rows[:, 0] / diagnostic["n"].to_numpy()
        diagnostic["mean_close_location"] = shared_rows[:, 1] / shared_rows[:, 2]
        diagnostic["share_low_close"] = shared_rows[:, 3] / shared_rows[:, 2]
        diagnostic["mean_day1_excess"] = shared_rows[:, 4] / shared_rows[:, 5]
        diagnostic["mean_excess_after_day1"] = diagnostic["mean_excess"] - diagnostic["mean_day1_excess"]
        diagnostic["day1_share_of_excess"] = diagnostic["mean_day1_excess"] / diagnostic["mean_excess"]

    common.write_csv(OUTPUT / "bounce_diagnostic.csv", diagnostic)
    keys = ["rule", "turnover_decile", "order_amount_krw", "ratio_cap_percent", "impact_coefficient"]
    kept = decision[decision["variant"] == JUDGE_VARIANT][keys + ["n", "mean_excess", "t_excess", "share_positive_excess_years"]]
    dropped = decision[decision["variant"] == 6][keys + ["n", "mean_excess", "t_excess", "share_positive_excess_years"]]
    difference = kept.merge(dropped, on=keys, suffixes=("_with_proxy", "_without_proxy"))
    difference["n_dropped"] = difference["n_with_proxy"] - difference["n_without_proxy"]
    difference["mean_excess_change"] = difference["mean_excess_without_proxy"] - difference["mean_excess_with_proxy"]
    difference["t_change"] = difference["t_excess_without_proxy"] - difference["t_excess_with_proxy"]
    common.write_csv(OUTPUT / "proxy_difference.csv", difference)

    # 3회차 재현 대조: 판 0·계수 0 은 3회차 판정 표 칸과 같아야 한다
    third = pd.read_csv(common.STUDY / "results" / "round3" / "pooled_cells.csv")
    mine = decision[(decision["variant"] == 0) & (decision["impact_coefficient"] == 0.0)]
    mine = mine.drop_duplicates(["rule", "turnover_decile", "min_turnover_krw"])
    joined = mine.merge(third, on=["rule", "turnover_decile", "min_turnover_krw"], suffixes=("", "_round3"))
    reproduction_check = {"cells_matched": int(len(joined)),
                          "n_max_abs_difference": float((joined["n"] - joined["n_round3"]).abs().max()),
                          "mean_excess_max_abs_difference": float((joined["mean_excess"] - joined["mean_excess_round3"]).abs().max()),
                          "t_max_abs_difference": float((joined["t_excess"] - joined["t_excess_round3"]).abs().max())}

    common.write_csv(OUTPUT / "max_t_null.csv", pd.DataFrame({
        "flip": np.arange(round3_test.FLIPS), "pool_max_t": pool_maxima, "pool_min_log10_p": pool_minimum / math.log(10.0),
        "fine_max_t": fine_maxima, "fine_min_log10_p": fine_minimum / math.log(10.0)}))
    passing = {}

    for name, frame in (("pool_variants_0_5", decision[decision["variant"] < 6]), ("judgement_slice", judged)):
        for bar in ("over_bar_a", "over_bar_b"):
            chosen = frame[frame[bar] & (frame["n"] > 0)]
            passing[f"{name}_{bar}"] = {
                "cells": int(len(chosen)), "positive_mean_excess": int((chosen["mean_excess"] > 0).sum()),
                "by_variant": {str(key): int(value) for key, value in chosen.groupby("variant").size().items()},
                "by_coefficient": {str(key): int(value) for key, value in chosen.groupby("impact_coefficient").size().items()},
                "by_turnover_decile": {str(int(key)): int(value) for key, value in chosen.groupby("turnover_decile").size().items()}}

    common.write_json(OUTPUT / "multiple_comparison.json", {
        "pool": pool_summary, "pool_per_variant": per_variant, "fine_judgement_variant_coefficient_0_3": fine_summary,
        "passing": passing, "kernel_vs_matrix_t_max_abs_difference": t_check,
        "sign_flip": "달마다 ±1 한 부호를 모든 칸에 같이 곱한다(칸 사이 상관 보존). 시드 common.SEED",
        "note": "판정 기준선은 판 0–5 전부를 한 번에 뒤집은 판정 표 칸 층의 (가). 판별 기준선은 참고"})
    timings["write"] = time.time() - step
    timings["total"] = time.time() - started
    judged_columns = ["rule", "turnover_decile", "ratio_cap_percent", "n", "share_kept_vs_unlimited", "mean_excess", "t_excess",
                      "share_positive_excess_years", "neighbours_all_same_sign", "over_bar_a"]
    common.write_json(OUTPUT / "round4_metrics.json", {
        "reproduction": panel.reproduction(), "corrections": correction_information,
        "corrections_path": "research/studies/38_trend_support_realtime/data/raw_close_corrections.parquet",
        "proxy_rows_signal_period": int((proxy_row & panel.signal_ok).sum()),
        "timings_seconds": timings, "spans": spans, "trades_by_variant_k": used_total,
        "variants": [{"variant": index, "threshold_basis": value[0], "decile_basis": value[1], "proxy_buys_dropped": value[2]}
                     for index, value in enumerate(VARIANTS)],
        "combinations": combinations, "impact_coefficients": list(IMPACT_COEFFICIENTS),
        "round3_reproduction_check": reproduction_check,
        "judgement": {"rule": "판 4, 계수 0.3, 주문 1억 원, 상한 10% 이하, 기준선 (가) 넘음, 옆 분위 부호 같음, 양수 연도 비율 ≥ 0.7, 평균 초과수익 > 0",
                      "bar_a": bar_a, "cells_checked": int(len(judged)), "cells_passing": int(len(judged_pass)),
                      "passing_cells": judged_pass[judged_columns].to_dict("records")},
        "hand_set": {"order_amounts": "100만·500만·2,000만·1억 원 — 내가 정한 격자, 고르는 데 쓰지 않음",
                     "ratio_caps": "0.5·1·2·5·10 %·상한 없음 — 내가 정한 격자, 고르는 데 쓰지 않음",
                     "impact_coefficients": "0·0.1·0.3·1.0 — 내가 정한 격자, 고르는 데 쓰지 않음",
                     "judgement_numbers": "계수 0.3·양수 연도 비율 0.7·1억 원·상한 10% 이하 — 지시에서 정한 판정 숫자",
                     "low_close_line": "종가 위치 0.2 이하 = 하단 근처(내가 정함, 진단에만 씀)",
                     "sell_ratio_fallback": "매도일 기준 거래대금이 없으면 매수일 비율을 쓴다(내가 정함)",
                     "positive_year": "그해 매매 1건 이상이면 센다(3회차와 같음)"}})
    print(f"전체 {timings['total']:.0f}초, 기준선 (가) {bar_a:.2f}, 판정 칸 {len(judged)} 중 남는 칸 {len(judged_pass)}", flush=True)


if __name__ == "__main__":
    main()
