"""스터디 38 5회차 — 같은 매매를 여러 번 세는 것을 걷어내고 판정을 다시 한다. 판정 판은 4회차 판 4 그대로다.

1–4회차 산출은 건드리지 않고 results/round5/ 에만 쓴다. 매수는 신호일 종가(동시호가) 매수, 다음 날 아님.
판 4 = 문턱·충격 비용 기준 거래대금은 전날까지 20일 평균, 분위도 전날까지 20일 평균. 매도 규칙·비용·비교 평균(B)·
거리 안쪽 12 구간·칸 안 보유 중 무시·충격 비용 식은 4회차와 같다(round4_test.py 머리말).

바뀐 것(조정 지시 2026-10-10 5회차):
  1) 판정을 주문 금액 4 개(100만·500만·2,000만·1억 원) 전부에서 낸다. 판정 금액은 사용자가 정할 값이다.
  2) 같은 매매를 여러 번 세는 것 걷어내기. 4회차 칸은 k 8 × 이평 N 7 × ATR 5 = 280 묶음을 합쳐, 같은 종목·같은 매수일이
     묶음마다 따로 세졌다. 판(family)을 이렇게 낸다:
       P0  4회차와 같은 합본(재현 대조용)
       P1  칸마다 같은 종목·같은 매수일 매매를 1 건으로 줄인 판 — 그 1 건의 초과수익 = 겹친 매매들 초과수익의 단순 평균(내가 정함)
       P2  P1 의 매매를 날짜로 묶은 판 — 그날 산 매매들의 평균을 하루 한 값으로, t 는 그 하루 값들의 월 묶음 t
       K0  k 별로 따로 낸 판(N·ATR 35 묶음은 합본, 줄이지 않음)
       K1  k 별로 따로 + 그 k 안에서 같은 종목·같은 매수일을 1 건으로 줄인 판
       K2  K1 을 날짜로 묶은 판(메모리 때문에 급증 조건 없음·계수 0.3 칸만)
  3) 기준선 (가)는 거래 든 달 24 개월 이상인 칸만으로 다시 구한다(판마다). (나)는 4회차와 같은 방식(자유도 보정, 그 판 전 칸).
     둘 다 넘어야 통과, 하나만 넘으면 따로 센다. 24 개월 미만 칸은 (가) 모집단 밖이라 (가)를 넘은 것으로 치지 않는다(내가 정함).
  3') 급증 신호: 전날 거래대금 ÷ 그 앞 20일 평균(t−21..t−2, 급증한 날이 기준에 안 들게 한 칸 더 민다 — 내가 정함) ≥ 배수.
     배수 격자 {2, 3, 5, 10}(지시에서 정한 격자, 고르는 데 쓰지 않음). 같은 칸 구조(규칙 × 분위 × 조합 × 계수)에
     거름(filter) 차원으로 넣는다: 거름 0 = 급증 조건 없음(판 4 그대로), 1–4 = 배수 2·3·5·10 이상. 급증 칸도 같은 판 안에서
     다중비교 칸 수에 들어간다. 충격 비용 기준은 판 4 와 같은 전날까지 20일 평균(급증한 전날을 포함).
  4) 1 분위·하한 2억 원(100만 원·상한 0.5%) 칸의 거래 든 달 수가 적은 이유: 연도별 1 분위 거래대금 분포·2억 이상 비율·
     연도별 초과수익 표.
  5) 판정(지시): 판 P1 · 거름 0 · 계수 0.3 · 상한 10% 이하 · 주문 금액 4 개 각각에서 (가)·(나) 둘 다 넘고, 옆 분위 부호 같고,
     양수 연도 비율 ≥ 0.7, 평균 초과수익 > 0. 0.3·0.7 은 지시에서 정한 판정 숫자. P1 을 판정 판으로 둔 것은 내가 정함 —
     다른 판의 통과 여부도 같은 표에 낸다. 남는 칸이 있으면 그 칸의 매매(겹친 묶음 전부, k·N·ATR 표시)를
     survivors_trades.csv 로 남긴다.

산출(results/round5/):
  decision_table.csv     P0·P1·P2 × 규칙 × 거름 × 분위 × 주문 금액 × 상한 × 계수
  per_k_table.parquet    K0·K1·K2 × k × (같은 칸)   per_k_judgement_region.csv 는 판정 영역만
  judgement.csv          판 × 거름 × 계수 × 주문 금액 × 상한: 칸 수·(가)·(나)·둘 다·하나만·통과
  coordinator_cells.csv  지시에 든 칸(판 4, 1 분위, 100만 원·상한 0.5%, 계수 0.3, E1·E3·E4j5)을 모든 판·k 로
  decile1_years.csv      연도별 1 분위 거래대금 분포와 그 칸의 연도별 매매 수·초과수익
  multiple_comparison.json 판별 M·기준선
  survivors_trades.csv   남는 칸의 매매 목록(없으면 머리줄만)
  coordinator_cells_trades.parquet 지시에 든 세 칸(판정 못 넘어도)의 매매 목록, 같은 형식
  round5_metrics.json    재현 정보·시간·4회차 재현 대조
재실행: py research/studies/38_trend_support_realtime/round5_test.py
"""
from __future__ import annotations

import argparse
import json
import math
import time

import numpy as np
import pandas as pd
from numba import njit, prange

import common
import round3_test
import round4_test

OUTPUT = common.STUDY / "results" / "round5"
SLOTS = round3_test.SLOTS
SURGE_MULTIPLES = (2.0, 3.0, 5.0, 10.0)            # 지시에서 정한 격자, 고르는 데 쓰지 않음
FILTERS = len(SURGE_MULTIPLES) + 1
COEFFICIENTS = round4_test.IMPACT_COEFFICIENTS
JUDGE_COEFFICIENT = 0.3                            # 지시
JUDGE_POSITIVE_YEARS = 0.7                         # 지시
JUDGE_CAP_MAX = 10.0                               # 지시(4회차와 같음)
MONTHS_FOR_BAR_A = 24                              # 지시
JUDGE_FAMILY = "P1"                                # 내가 정함
COORDINATOR_RULES = ("E1", "E3", "E4j5")


# ── 커널 ────────────────────────────────────────────────────────────────────

@njit(cache=True)
def trade_values(gross, market, amount, buy_base, sell_base, coefficients, cost, out):
    buy_ratio = amount / buy_base if (np.isfinite(buy_base) and buy_base > 0) else np.inf
    sell_ratio = amount / sell_base if (np.isfinite(sell_base) and sell_base > 0) else buy_ratio

    for index in range(coefficients.shape[0]):
        coefficient = coefficients[index]

        if coefficient == 0.0:
            multiplier = 1.0
        else:
            buy_multiplier = 1.0 + coefficient * buy_ratio
            sell_multiplier = max(0.0, 1.0 - coefficient * sell_ratio)
            multiplier = sell_multiplier / buy_multiplier if np.isfinite(buy_multiplier) else 0.0

        out[index] = (gross * multiplier * cost - 1.0) * 100.0 - market


@njit(cache=True)
def run_rule(rule, averages, atrs, open_price, high, low, close, starts, market_position, signal_ok, month_index,
             year_index, slot, base_turnover, surge, surge_multiples, amounts, minimums, coefficients, judge_index, states,
             latest_lows, distance_edges, bench, signal_start, cost,
             raw_month, raw_month_n, raw_year, k_month, k_month_n, k_year, a_month, a_month_n, a_year, day_sum, day_n,
             k_day_sum, k_day_n):
    """한 규칙의 모든 (k, N, ATR) 매매를 종목마다 돌며 판 P0·K0(raw_*, [k, 칸, ...]), K1(k_*), P1(a_*), P2(day_*), K2(k_day_*)를 쌓는다.
    칸(group) = 거름 × 분위 슬롯 × 조합. 분위 슬롯은 행마다 정해져 있어 (행, 거름 × 조합) 이 같은 종목·같은 매수일의 한 칸이다."""
    spans = states.shape[0]
    windows = averages.shape[0]
    periods = atrs.shape[0]
    combinations = amounts.shape[0]
    coefficient_count = coefficients.shape[0]
    filters = surge_multiples.shape[0] + 1
    filter_combinations = filters * combinations
    groups = filters * SLOTS * combinations
    base_groups = SLOTS * combinations
    inner_last = distance_edges.shape[0] - 1
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    next_allowed = np.zeros(groups, dtype=np.int64)
    k_sum = np.zeros((longest, filter_combinations, coefficient_count))
    k_count = np.zeros((longest, filter_combinations), dtype=np.int32)
    a_sum = np.zeros((longest, filter_combinations, coefficient_count))
    a_count = np.zeros((longest, filter_combinations), dtype=np.int32)
    k_touched = np.empty(longest * filter_combinations, dtype=np.int64)
    a_touched = np.empty(longest * filter_combinations, dtype=np.int64)
    values = np.empty((combinations, coefficient_count))
    computed_values = np.zeros(combinations, dtype=np.bool_)
    used = 0

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first
        a_touched_count = 0

        for span in range(spans):
            state = states[span]
            latest_low = latest_lows[span]
            k_touched_count = 0

            for window in range(windows):
                average = averages[window]
                common.fill_next_exit(rule, first, length, average, close, low, state, latest_low, run_length, next_exit)

                for period in range(periods):
                    atr = atrs[period]
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

                        row_slot = slot[row]
                        computed = False
                        exit_offset = length
                        gross = 1.0
                        market = 0.0
                        exit_row = row
                        computed_values[:] = False
                        month = month_index[row]
                        year = year_index[row]

                        for combination in range(combinations):
                            minimum = minimums[combination]

                            if minimum > 0.0 and not (base_turnover[row] >= minimum):
                                continue

                            for filter_index in range(filters):
                                if filter_index > 0 and not (surge[row] >= surge_multiples[filter_index - 1]):
                                    break

                                group = filter_index * base_groups + row_slot * combinations + combination

                                if offset < next_allowed[group]:
                                    continue

                                if not computed:
                                    computed = True
                                    exit_offset, exit_price = common.exit_of(rule, offset, length, next_exit, first,
                                                                             open_price, close, latest_low)

                                    if exit_offset < length:
                                        exit_row = first + exit_offset
                                    else:
                                        exit_row = last - 1
                                        exit_price = close[exit_row]

                                    gross = exit_price / close[row]
                                    market = bench[market_position[row] - signal_start, market_position[exit_row] - signal_start]

                                if not computed_values[combination]:
                                    computed_values[combination] = True
                                    trade_values(gross, market, amounts[combination], base_turnover[row],
                                                 base_turnover[exit_row], coefficients, cost, values[combination])

                                next_allowed[group] = exit_offset + 1 if exit_offset < length else length + 1
                                key = filter_index * combinations + combination
                                raw_month_n[span, group, month] += 1.0

                                for index in range(coefficient_count):
                                    value = values[combination, index]
                                    raw_month[span, group, index, month] += value
                                    raw_year[span, group, index, year, 0] += 1.0
                                    raw_year[span, group, index, year, 1] += value
                                    k_sum[offset, key, index] += value
                                    a_sum[offset, key, index] += value

                                if k_count[offset, key] == 0:
                                    k_touched[k_touched_count] = offset * filter_combinations + key
                                    k_touched_count += 1

                                if a_count[offset, key] == 0:
                                    a_touched[a_touched_count] = offset * filter_combinations + key
                                    a_touched_count += 1

                                k_count[offset, key] += 1
                                a_count[offset, key] += 1
                                used += 1

            # k 안에서 같은 종목·같은 매수일을 1 건으로(K1·K2)
            for position in range(k_touched_count):
                offset = k_touched[position] // filter_combinations
                key = k_touched[position] % filter_combinations
                row = first + offset
                filter_index = key // combinations
                combination = key % combinations
                group = filter_index * base_groups + slot[row] * combinations + combination
                month = month_index[row]
                year = year_index[row]
                day = market_position[row] - signal_start
                count = k_count[offset, key]
                k_month_n[span, group, month] += 1.0

                for index in range(coefficient_count):
                    value = k_sum[offset, key, index] / count
                    k_month[span, group, index, month] += value
                    k_year[span, group, index, year, 0] += 1.0
                    k_year[span, group, index, year, 1] += value

                    if filter_index == 0 and index == judge_index:
                        k_day_sum[span, group, day] += value
                        k_day_n[span, group, day] += 1.0

                    k_sum[offset, key, index] = 0.0

                k_count[offset, key] = 0

        # 모든 k 를 합쳐 같은 종목·같은 매수일을 1 건으로(P1·P2)
        for position in range(a_touched_count):
            offset = a_touched[position] // filter_combinations
            key = a_touched[position] % filter_combinations
            row = first + offset
            filter_index = key // combinations
            combination = key % combinations
            group = filter_index * base_groups + slot[row] * combinations + combination
            month = month_index[row]
            year = year_index[row]
            day = market_position[row] - signal_start
            count = a_count[offset, key]
            a_month_n[group, month] += 1.0
            day_n[group, day] += 1.0

            for index in range(coefficient_count):
                value = a_sum[offset, key, index] / count
                a_month[group, index, month] += value
                a_year[group, index, year, 0] += 1.0
                a_year[group, index, year, 1] += value
                day_sum[group, index, day] += value
                a_sum[offset, key, index] = 0.0

            a_count[offset, key] = 0

    return used


@njit(parallel=True, cache=True)
def run_all(rules, averages, atrs, open_price, high, low, close, starts, market_position, signal_ok, month_index, year_index,
            slot, base_turnover, surge, surge_multiples, amounts, minimums, coefficients, judge_index, states, latest_lows,
            distance_edges, bench, signal_start, cost, month_count, year_count, day_count):
    rule_count = rules.shape[0]
    spans = states.shape[0]
    coefficient_count = coefficients.shape[0]
    combinations = amounts.shape[0]
    groups = (surge_multiples.shape[0] + 1) * SLOTS * combinations
    base_groups = SLOTS * combinations
    raw_month = np.zeros((rule_count, spans, groups, coefficient_count, month_count))
    raw_month_n = np.zeros((rule_count, spans, groups, month_count))
    raw_year = np.zeros((rule_count, spans, groups, coefficient_count, year_count, 2))
    k_month = np.zeros((rule_count, spans, groups, coefficient_count, month_count))
    k_month_n = np.zeros((rule_count, spans, groups, month_count))
    k_year = np.zeros((rule_count, spans, groups, coefficient_count, year_count, 2))
    a_month = np.zeros((rule_count, groups, coefficient_count, month_count))
    a_month_n = np.zeros((rule_count, groups, month_count))
    a_year = np.zeros((rule_count, groups, coefficient_count, year_count, 2))
    day_sum = np.zeros((rule_count, groups, coefficient_count, day_count), dtype=np.float32)
    day_n = np.zeros((rule_count, groups, day_count), dtype=np.float32)
    k_day_sum = np.zeros((rule_count, spans, base_groups, day_count), dtype=np.float32)
    k_day_n = np.zeros((rule_count, spans, base_groups, day_count), dtype=np.float32)
    used = np.zeros(rule_count, dtype=np.int64)

    for index in prange(rule_count):
        used[index] = run_rule(rules[index], averages, atrs, open_price, high, low, close, starts, market_position, signal_ok,
                               month_index, year_index, slot, base_turnover, surge, surge_multiples, amounts, minimums,
                               coefficients, judge_index, states, latest_lows, distance_edges, bench, signal_start, cost,
                               raw_month[index], raw_month_n[index], raw_year[index], k_month[index], k_month_n[index],
                               k_year[index], a_month[index], a_month_n[index], a_year[index], day_sum[index], day_n[index],
                               k_day_sum[index], k_day_n[index])

    return (raw_month, raw_month_n, raw_year, k_month, k_month_n, k_year, a_month, a_month_n, a_year, day_sum, day_n,
            k_day_sum, k_day_n, used)


@njit(cache=True)
def emit_trades(rule, filter_index, want_slot, combination, averages, atrs, open_price, high, low, close, starts,
                market_position, signal_ok, slot, base_turnover, surge, surge_multiples, minimum, states, latest_lows,
                distance_edges, capacity):
    """한 칸(규칙·거름·분위·조합)의 매매를 묶음(k, N, ATR)마다 한 줄씩 낸다. 칸 안 보유 중 무시는 run_rule 과 같다."""
    inner_last = distance_edges.shape[0] - 1
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    output = np.empty((capacity, 7), dtype=np.int64)     # 종목, 매수 행, 매도 행, 청산 이유, k 순번, N 순번, ATR 순번
    exit_prices = np.empty(capacity)
    written = 0

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first

        for span in range(states.shape[0]):
            state = states[span]
            latest_low = latest_lows[span]

            for window in range(averages.shape[0]):
                average = averages[window]
                common.fill_next_exit(rule, first, length, average, close, low, state, latest_low, run_length, next_exit)

                for period in range(atrs.shape[0]):
                    atr = atrs[period]
                    allowed = 0

                    for offset in range(length):
                        row = first + offset

                        if offset < allowed or not signal_ok[row] or state[row] != 1 or slot[row] != want_slot:
                            continue

                        if not (np.isfinite(average[row]) and np.isfinite(atr[row]) and atr[row] > 0):
                            continue

                        distance_index = common.distance_bin((low[row] - average[row]) / atr[row], distance_edges)

                        if distance_index < 1 or distance_index > inner_last:
                            continue

                        if minimum > 0.0 and not (base_turnover[row] >= minimum):
                            continue

                        if filter_index > 0 and not (surge[row] >= surge_multiples[filter_index - 1]):
                            continue

                        exit_offset, exit_price = common.exit_of(rule, offset, length, next_exit, first, open_price, close,
                                                                 latest_low)
                        reason = common.REASON_RULE

                        if exit_offset < length:
                            exit_row = first + exit_offset
                            allowed = exit_offset + 1
                        else:
                            exit_row = last - 1
                            exit_price = close[exit_row]
                            reason = 0
                            allowed = length + 1

                        if written < capacity:
                            output[written, 0] = stock
                            output[written, 1] = row
                            output[written, 2] = exit_row
                            output[written, 3] = reason
                            output[written, 4] = span
                            output[written, 5] = window
                            output[written, 6] = period
                            exit_prices[written] = exit_price

                        written += 1

    return output[:min(written, capacity)], exit_prices[:min(written, capacity)], written


# ── 집계 ────────────────────────────────────────────────────────────────────

def cluster_t_matrix(month_sum: np.ndarray, month_n: np.ndarray) -> np.ndarray:
    """common.cluster_t 를 칸 × 달 행렬로. 매매 3 건 미만·묶음 달 2 개 미만·잔차 0 이면 NaN."""
    count = month_n.sum(axis=1)
    total = month_sum.sum(axis=1)
    groups = (month_n > 0).sum(axis=1)

    with np.errstate(invalid="ignore", divide="ignore"):
        mean = total / count
        residual = month_sum - month_n * mean[:, None]
        squared = np.where(month_n > 0, residual * residual, 0.0).sum(axis=1)
        error = np.sqrt(groups / np.maximum(groups - 1, 1) * squared) / count
        t_value = mean / error

    return np.where((count >= 3) & (groups >= 2) & (squared > 0), t_value, np.nan)


def days_to_periods(day_sum: np.ndarray, day_n: np.ndarray, starts_of_period: np.ndarray):
    """하루 값(그날 산 매매 평균)을 달·해로 묶는다. 반환 (하루 값 합, 매매 든 날 수)."""
    sums, counts = [], []

    for index in range(day_sum.shape[0]):     # 첫 축(규칙)마다 나눠 메모리를 줄인다
        counts_today = day_n[index].astype(np.float64)

        with np.errstate(invalid="ignore", divide="ignore"):
            daily = np.where(counts_today > 0, day_sum[index] / np.where(counts_today > 0, counts_today, 1.0), 0.0)

        sums.append(np.add.reduceat(daily, starts_of_period, axis=-1))
        counts.append(np.add.reduceat((counts_today > 0).astype(np.float64), starts_of_period, axis=-1))

    return np.stack(sums), np.stack(counts)


def key_frame(shape: tuple, names: list[str]) -> pd.DataFrame:
    grids = np.meshgrid(*[np.arange(size) for size in shape], indexing="ij")
    return pd.DataFrame({name: grid.ravel() for name, grid in zip(names, grids)})


def describe_cells(frame: pd.DataFrame, combinations: list[dict]) -> pd.DataFrame:
    group = frame["group"].to_numpy()
    combination_count = len(combinations)
    base_groups = SLOTS * combination_count
    frame["filter"] = group // base_groups
    frame["surge_multiple"] = np.r_[np.nan, SURGE_MULTIPLES][frame["filter"].to_numpy()]
    slot = (group % base_groups) // combination_count
    frame["turnover_decile"] = np.where(slot < 10, slot, -1)
    combination = group % combination_count
    frame["order_amount_krw"] = np.array([row["order_amount_krw"] for row in combinations])[combination]
    frame["ratio_cap_percent"] = np.array([("none" if row["ratio_cap_percent"] is None else str(row["ratio_cap_percent"]))
                                           for row in combinations], dtype=object)[combination]
    frame["min_turnover_krw"] = np.array([row["min_turnover_krw"] for row in combinations])[combination]
    frame["rule"] = np.array(common.RULE_NAMES)[frame["rule_index"].to_numpy()]
    frame["impact_coefficient"] = np.array(COEFFICIENTS)[frame["coefficient_index"].to_numpy()]
    return frame


def family_table(name: str, month_sum: np.ndarray, month_n: np.ndarray, year_sum: np.ndarray, frame: pd.DataFrame,
                 years: list[int], signs: np.ndarray, neighbour_keys: list[str], raw_n: np.ndarray | None = None):
    """한 판의 칸 표와 기준선. month_sum (칸, 달), month_n (칸, 달), year_sum (칸, 해, 2 = 건수·합)."""
    frame = frame.copy()
    frame["family"] = name
    count = month_n.sum(axis=1)
    frame["n"] = count

    if raw_n is not None:
        frame["n_before_dedupe"] = raw_n

    with np.errstate(invalid="ignore", divide="ignore"):
        frame["mean_excess"] = month_sum.sum(axis=1) / count

    frame["t_excess"] = cluster_t_matrix(month_sum, month_n)
    months = (month_n > 0).sum(axis=1)
    frame["months_with_trades"] = months
    with_trades = (year_sum[:, :, 0] > 0).sum(axis=1)
    positive = ((year_sum[:, :, 0] > 0) & (year_sum[:, :, 1] > 0)).sum(axis=1)
    frame["years_with_trades"] = with_trades

    with np.errstate(invalid="ignore", divide="ignore"):
        frame["share_positive_excess_years"] = np.where(with_trades > 0, positive / np.maximum(with_trades, 1), np.nan)

        for index, year in enumerate(years):
            frame[f"mean_excess_{year}"] = year_sum[:, index, 1] / year_sum[:, index, 0]

    frame = round4_test.neighbour_columns(frame, neighbour_keys)
    # (나): 그 판 전 칸
    detail_all, summary_all, _, _ = round3_test.flip_null(month_sum, month_n, signs, f"{name} 전 칸")
    over_b = np.zeros(len(frame), dtype=bool)
    over_b[detail_all["position"].to_numpy()] = detail_all["over_bar_b"].to_numpy()
    log10_p = np.full(len(frame), np.nan)
    log10_p[detail_all["position"].to_numpy()] = detail_all["log10_p"].to_numpy()
    # (가): 거래 든 달 24 개월 이상 칸만
    long_enough = months >= MONTHS_FOR_BAR_A
    detail_long, summary_long, _, _ = round3_test.flip_null(month_sum[long_enough], month_n[long_enough], signs,
                                                            f"{name} 거래 든 달 {MONTHS_FOR_BAR_A} 개월 이상 칸")
    bar_a = summary_long["a_max_t_null_p95_bar"]
    t_values = frame["t_excess"].to_numpy()
    frame["log10_p"] = log10_p
    frame["over_bar_a"] = long_enough & (np.nan_to_num(t_values, nan=-np.inf) > bar_a)
    frame["over_bar_b"] = over_b
    frame["over_both"] = frame["over_bar_a"] & frame["over_bar_b"]
    frame["over_one_only"] = frame["over_bar_a"] ^ frame["over_bar_b"]
    frame["other_conditions"] = (frame["neighbours_all_same_sign"] & (frame["share_positive_excess_years"] >= JUDGE_POSITIVE_YEARS)
                                 & (frame["mean_excess"] > 0))
    frame["passes"] = frame["over_both"] & frame["other_conditions"]
    frame["passes_one_bar_only"] = frame["over_one_only"] & frame["other_conditions"]
    t_check = float(np.nanmax(np.abs(t_values[detail_all["position"].to_numpy()] - detail_all["t_matrix"].to_numpy())))
    summary = {"family": name, "cells_M_all": summary_all["cells_M"], "bar_b_normal_equivalent_z": summary_all["b_bar_normal_equivalent_z"],
               "bar_a_all_cells_reference": summary_all["a_max_t_null_p95_bar"], "cells_M_24_months": summary_long["cells_M"],
               "bar_a_24_months": bar_a, "bar_a_24_months_null_p50": summary_long["a_max_t_null_p50"],
               "normal_expected_cells_t_over_3_5": summary_all["normal_expected_cells_t_over_3_5"],
               "flip_mean_cells_t_over_3_5": summary_all["flip_mean_cells_t_over_3_5"],
               "actual_cells_t_over_3_5": summary_all["actual_cells_t_over_3_5"], "observed_max_t": summary_all["observed_max_t"],
               "observed_max_t_24_months": summary_long["observed_max_t"],
               "cells_over_a": int(frame["over_bar_a"].sum()), "cells_over_b": int(frame["over_bar_b"].sum()),
               "cells_over_both": int(frame["over_both"].sum()), "cells_over_one_only": int(frame["over_one_only"].sum()),
               "cells_passing": int(frame["passes"].sum()), "cells_passing_one_bar_only": int(frame["passes_one_bar_only"].sum()),
               "kernel_vs_matrix_t_max_abs_difference": t_check}
    return frame, summary


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spans", default=",".join(str(span) for span in common.K_VALUES))
    arguments = parser.parse_args()
    span_values = [int(value) for value in arguments.spans.split(",")]
    full_run = tuple(span_values) == tuple(common.K_VALUES)
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    timings = {}
    panel = common.Panel()
    correction_information = round3_test.apply_corrections(panel)
    panel.build_benchmark()
    previous_20d = round4_test.shift_within_stock(panel.turnover20, panel.starts)
    previous_day = round4_test.shift_within_stock(panel.day_turnover, panel.starts)
    surge_baseline = round4_test.shift_within_stock(previous_20d, panel.starts)

    with np.errstate(invalid="ignore", divide="ignore"):
        surge = np.where(surge_baseline > 0, previous_day / surge_baseline, np.nan)

    panel.turnover20 = previous_20d
    deciles = panel.build_deciles()
    slot = np.where(deciles[:, 1] >= 0, deciles[:, 1], 10).astype(np.int64)
    combinations = round4_test.combination_grid()
    amounts = np.array([row["order_amount_krw"] for row in combinations], np.float64)
    minimums = np.array([row["min_turnover_krw"] for row in combinations], np.float64)
    coefficients = np.array(COEFFICIENTS, np.float64)
    judge_index = int(np.flatnonzero(coefficients == JUDGE_COEFFICIENT)[0])
    states, latest_lows, pivots = [], [], []

    for span in span_values:
        state, latest_low, pivots_by_stock = panel.trend(span)
        states.append(state)
        latest_lows.append(latest_low)
        pivots.append(pivots_by_stock)

    states = np.stack(states)
    latest_lows = np.stack(latest_lows)
    market_dates = panel.market_dates[panel.signal_start:]
    day_count = len(market_dates)
    day_month = ((market_dates // 10000 - panel.first_year) * 12 + (market_dates // 100) % 100 - 1).astype(np.int64)
    day_year = (market_dates // 10000 - panel.first_year).astype(np.int64)
    month_starts = np.searchsorted(day_month, np.arange(panel.month_count))
    year_starts = np.searchsorted(day_year, np.arange(panel.year_count))
    years = list(range(panel.first_year, panel.first_year + panel.year_count))
    timings["prepare"] = time.time() - started
    print(f"준비 끝 {timings['prepare']:.0f}초, k {span_values}, 날 {day_count}", flush=True)
    step = time.time()
    rules = np.arange(len(common.RULE_NAMES), dtype=np.int64)
    (raw_month, raw_month_n, raw_year, k_month, k_month_n, k_year, a_month, a_month_n, a_year, day_sum, day_n, k_day_sum, k_day_n,
     used) = run_all(rules, panel.averages, panel.atrs, panel.open_price, panel.high, panel.low, panel.close, panel.starts,
                     panel.market_position, panel.signal_ok, panel.month_index, panel.year_index, slot, previous_20d, surge,
                     np.array(SURGE_MULTIPLES), amounts, minimums, coefficients, judge_index, states, latest_lows,
                     common.DISTANCE_EDGES, panel.bench, panel.signal_start, common.COST, panel.month_count, panel.year_count,
                     day_count)
    timings["kernel"] = time.time() - step
    print(f"커널 끝 {timings['kernel']:.0f}초, 매매(묶음별) {int(used.sum()):,}", flush=True)
    step = time.time()
    rule_count = len(rules)
    span_count = len(span_values)
    groups = raw_month.shape[2]
    coefficient_count = len(coefficients)
    month_count = panel.month_count
    year_count = panel.year_count
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    summaries = {}
    keys_all = ["family", "rule", "filter", "order_amount_krw", "ratio_cap_percent", "impact_coefficient"]

    # 모든 k 합본 판: P0·P1·P2
    base = describe_cells(key_frame((rule_count, groups, coefficient_count), ["rule_index", "group", "coefficient_index"]),
                          combinations)
    p0_month = raw_month.sum(axis=1).reshape(-1, month_count)
    p0_n = np.repeat(raw_month_n.sum(axis=1)[:, :, None, :], coefficient_count, axis=2).reshape(-1, month_count)
    p0_year = raw_year.sum(axis=1).reshape(-1, year_count, 2)
    p0_table, summaries["P0"] = family_table("P0", p0_month, p0_n, p0_year, base, years, signs, keys_all)
    raw_total = p0_table["n"].to_numpy()
    del p0_month, p0_n, p0_year
    p1_n = np.repeat(a_month_n[:, :, None, :], coefficient_count, axis=2).reshape(-1, month_count)
    p1_table, summaries["P1"] = family_table("P1", a_month.reshape(-1, month_count), p1_n, a_year.reshape(-1, year_count, 2), base,
                                       years, signs, keys_all, raw_total)
    del p1_n
    p2_month, p2_month_n = days_to_periods(day_sum, day_n[:, :, None, :], month_starts)
    p2_year_sum, p2_year_n = days_to_periods(day_sum, day_n[:, :, None, :], year_starts)
    p2_n = np.broadcast_to(p2_month_n, p2_month.shape).reshape(-1, month_count)
    p2_year = np.stack([np.broadcast_to(p2_year_n, p2_year_sum.shape), p2_year_sum], axis=-1).reshape(-1, year_count, 2)
    p2_table, summaries["P2"] = family_table("P2", p2_month.reshape(-1, month_count), np.ascontiguousarray(p2_n), p2_year, base,
                                       years, signs, keys_all, raw_total)
    p2_table = p2_table.rename(columns={"n": "n_days"})
    p2_table["n"] = p1_table["n"].to_numpy()
    del p2_month, p2_month_n, p2_year_sum, p2_year_n, p2_n, p2_year, day_sum, day_n
    timings["families_pooled"] = time.time() - step
    print(f"합본 판 끝 {timings['families_pooled']:.0f}초", flush=True)

    # k 별 판: K0·K1·K2
    step = time.time()
    per_k_base = describe_cells(key_frame((rule_count, span_count, groups, coefficient_count),
                                          ["rule_index", "span_index", "group", "coefficient_index"]), combinations)
    per_k_base["k"] = np.array(span_values)[per_k_base["span_index"].to_numpy()]
    keys_k = keys_all + ["k"]
    k0_n = np.repeat(raw_month_n[:, :, :, None, :], coefficient_count, axis=3).reshape(-1, month_count)
    k0, summaries["K0"] = family_table("K0", raw_month.reshape(-1, month_count), k0_n, raw_year.reshape(-1, year_count, 2),
                                       per_k_base, years, signs, keys_k)
    k_raw_total = k0["n"].to_numpy()
    del k0_n, raw_month, raw_month_n, raw_year
    k1_n = np.repeat(k_month_n[:, :, :, None, :], coefficient_count, axis=3).reshape(-1, month_count)
    k1, summaries["K1"] = family_table("K1", k_month.reshape(-1, month_count), k1_n, k_year.reshape(-1, year_count, 2),
                                       per_k_base, years, signs, keys_k, k_raw_total)
    del k1_n, k_month, k_month_n, k_year
    k2_base = describe_cells(key_frame((rule_count, span_count, SLOTS * len(combinations)), ["rule_index", "span_index", "group"]).assign(
        coefficient_index=judge_index), combinations)
    k2_base["k"] = np.array(span_values)[k2_base["span_index"].to_numpy()]
    k2_month, k2_month_n = days_to_periods(k_day_sum, k_day_n, month_starts)
    k2_year_sum, k2_year_n = days_to_periods(k_day_sum, k_day_n, year_starts)
    k2, summaries["K2"] = family_table("K2", k2_month.reshape(-1, month_count), k2_month_n.reshape(-1, month_count),
                                       np.stack([k2_year_n, k2_year_sum], axis=-1).reshape(-1, year_count, 2), k2_base, years,
                                       signs, keys_k)
    k2 = k2.rename(columns={"n": "n_days"})
    judge_rows = (k1["filter"] == 0) & (k1["coefficient_index"] == judge_index)
    k2["n"] = k1.loc[judge_rows, "n"].to_numpy()
    del k2_month, k2_month_n, k2_year_sum, k2_year_n, k_day_sum, k_day_n
    timings["families_per_k"] = time.time() - step
    print(f"k 별 판 끝 {timings['families_per_k']:.0f}초", flush=True)

    # 표 쓰기
    step = time.time()
    year_columns = [f"mean_excess_{year}" for year in years]
    columns = ["family", "rule", "filter", "surge_multiple", "turnover_decile", "order_amount_krw", "ratio_cap_percent",
               "min_turnover_krw", "impact_coefficient", "n", "n_before_dedupe", "n_days", "mean_excess", "t_excess",
               "months_with_trades", "years_with_trades", "share_positive_excess_years", "neighbour_deciles",
               "neighbour_same_sign", "neighbours_all_same_sign", "log10_p", "over_bar_a", "over_bar_b", "over_both",
               "over_one_only", "passes", "passes_one_bar_only"] + year_columns + ["rule_index", "group", "coefficient_index"]
    pooled = pd.concat([p0_table, p1_table, p2_table], ignore_index=True).reindex(columns=columns)
    common.write_csv(OUTPUT / "decision_table.csv", pooled)
    per_k = pd.concat([k0, k1, k2], ignore_index=True).reindex(columns=["k"] + columns)
    per_k.to_parquet(OUTPUT / "per_k_table.parquet", index=False)
    caps = lambda frame: frame["ratio_cap_percent"].map(lambda value: np.inf if value == "none" else float(value))
    region = per_k[(caps(per_k) <= JUDGE_CAP_MAX) & (per_k["impact_coefficient"] == JUDGE_COEFFICIENT)]
    common.write_csv(OUTPUT / "per_k_judgement_region.csv", region)
    everything = pd.concat([pooled.assign(k="all"), per_k.assign(k=per_k["k"].astype(str))], ignore_index=True)
    everything["cap_in_region"] = caps(everything) <= JUDGE_CAP_MAX
    judgement = everything.groupby(["family", "filter", "impact_coefficient", "order_amount_krw", "ratio_cap_percent"]).agg(
        cells=("n", "size"), cells_with_trades=("n", lambda values: int((values > 0).sum())), over_bar_a=("over_bar_a", "sum"),
        over_bar_b=("over_bar_b", "sum"), over_both=("over_both", "sum"), over_one_only=("over_one_only", "sum"),
        passes=("passes", "sum"), passes_one_bar_only=("passes_one_bar_only", "sum"), best_t=("t_excess", "max")).reset_index()
    common.write_csv(OUTPUT / "judgement.csv", judgement)
    region_all = everything[everything["cap_in_region"] & (everything["impact_coefficient"] == JUDGE_COEFFICIENT)]
    judgement_by_amount = region_all.groupby(["family", "filter", "order_amount_krw"]).agg(
        cells=("n", "size"), over_bar_a=("over_bar_a", "sum"), over_bar_b=("over_bar_b", "sum"), over_both=("over_both", "sum"),
        passes=("passes", "sum"), passes_one_bar_only=("passes_one_bar_only", "sum"), best_t=("t_excess", "max")).reset_index()
    common.write_csv(OUTPUT / "judgement_by_amount.csv", judgement_by_amount)
    primary = region_all[(region_all["family"] == JUDGE_FAMILY) & (region_all["filter"] == 0)]
    survivors = primary[primary["passes"]]

    # 지시에 든 칸
    chosen = ((everything["filter"] == 0) & (everything["turnover_decile"] == 1) & (everything["order_amount_krw"] == 1_000_000.0)
              & (everything["ratio_cap_percent"] == "0.5") & everything["rule"].isin(COORDINATOR_RULES)
              & everything["impact_coefficient"].isin([0.0, JUDGE_COEFFICIENT]))
    common.write_csv(OUTPUT / "coordinator_cells.csv", everything[chosen].sort_values(["rule", "impact_coefficient", "family", "k"]))

    # 1 분위·하한 2억 원 연도별
    rows = panel.signal_ok & (slot == 1)
    universe = pd.DataFrame({"year": panel.years[rows], "previous_20d": previous_20d[rows], "date": panel.dates[rows]})
    universe["at_least_200m"] = universe["previous_20d"] >= 2e8
    by_year = universe.groupby("year").agg(decile1_rows=("previous_20d", "size"),
                                           decile1_turnover_median_krw=("previous_20d", "median"),
                                           decile1_turnover_p90_krw=("previous_20d", lambda values: values.quantile(0.9)),
                                           decile1_turnover_max_krw=("previous_20d", "max"),
                                           share_at_least_200m=("at_least_200m", "mean"),
                                           rows_at_least_200m=("at_least_200m", "sum")).reset_index()
    upper = universe.groupby("date")["previous_20d"].max().rename("decile1_upper").reset_index()
    upper["year"] = upper["date"] // 10000
    by_year = by_year.merge(upper.groupby("year")["decile1_upper"].median().rename("decile1_upper_bound_median_krw").reset_index(),
                            on="year")

    for family_name, table in (("P0", p0_table), ("P1", p1_table)):
        for rule_name in COORDINATOR_RULES:
            cell = table[(table["rule"] == rule_name) & (table["filter"] == 0) & (table["turnover_decile"] == 1)
                         & (table["order_amount_krw"] == 1_000_000.0) & (table["ratio_cap_percent"] == "0.5")
                         & (table["impact_coefficient"] == JUDGE_COEFFICIENT)]
            position = cell.index[0]
            source = (a_year if family_name == "P1" else None)
            year_values = cell[[f"mean_excess_{year}" for year in years]].to_numpy()[0]
            by_year[f"{family_name}_{rule_name}_mean_excess_c0.3"] = year_values[by_year["year"].to_numpy() - panel.first_year]

            if family_name == "P1":
                rule_index = common.RULE_NAMES.index(rule_name)
                group = int(cell["group"].iloc[0])
                by_year[f"P1_{rule_name}_trades"] = source[rule_index, group, judge_index, by_year["year"].to_numpy()
                                                           - panel.first_year, 0]
                months = a_month_n[rule_index, group]
                first_month = int(np.flatnonzero(months > 0)[0]) if (months > 0).any() else -1
                summaries[f"first_month_{rule_name}"] = (panel.first_year + first_month // 12) * 100 + first_month % 12 + 1

    common.write_csv(OUTPUT / "decile1_years.csv", by_year)

    # 남는 칸 매매
    trade_columns = ["rule", "turnover_decile", "order_amount_krw", "ratio_cap_percent", "code", "name", "entry_date",
                     "entry_price_adjusted", "entry_price_raw", "exit_date", "exit_price_adjusted", "exit_price_raw", "exit_reason",
                     "k", "window", "atr_period", "support_low_date", "support_low_value_adjusted", "duplicates_same_stock_date",
                     "excess_c0_3"]
    emission_checks = []

    def trades_of(cells: pd.DataFrame) -> pd.DataFrame:
        """칸마다 매매를 묶음(k·N·ATR)별로 한 줄씩. 같은 종목·같은 매수일 겹침 수를 같이 적는다."""
        trade_frames = []

        for _, cell in cells.iterrows():
            rule_index = common.RULE_NAMES.index(cell["rule"])
            group = int(cell["group"])
            combination = group % len(combinations)
            capacity = int(p0_table.loc[(p0_table["rule_index"] == rule_index) & (p0_table["group"] == group) & (p0_table["coefficient_index"] == 0), "n"].iloc[0]) + 10
            output, exit_prices, written = emit_trades(
                rule_index, 0, int(cell["turnover_decile"]), combination, panel.averages, panel.atrs, panel.open_price, panel.high,
                panel.low, panel.close, panel.starts, panel.market_position, panel.signal_ok, slot, previous_20d, surge,
                np.array(SURGE_MULTIPLES), minimums[combination], states, latest_lows, common.DISTANCE_EDGES, capacity)
            trades = pd.DataFrame(output, columns=["stock", "row", "exit_row", "reason", "span_index", "window_index", "period_index"])
            trades["exit_price"] = exit_prices
            support_dates, support_values = [], []

            for stock, row, span_index in trades[["stock", "row", "span_index"]].itertuples(index=False):
                pivot = pivots[span_index][stock]
                offset = row - panel.starts[stock]
                position = int(np.searchsorted(pivot["low_confirm_rows"], offset, side="right")) - 1
                support_dates.append(int(panel.dates[panel.starts[stock] + pivot["low_rows"][position]]) if position >= 0 else -1)
                support_values.append(float(pivot["low_values"][position]) if position >= 0 else np.nan)

            row_array = trades["row"].to_numpy()
            exit_array = trades["exit_row"].to_numpy()
            stock_array = trades["stock"].to_numpy()
            reason = np.where(trades["reason"].to_numpy() == common.REASON_RULE, common.REASON_TEXT[common.REASON_RULE],
                              np.where(panel.listed_at_end[stock_array], common.REASON_TEXT[common.REASON_OPEN],
                                       common.REASON_TEXT[common.REASON_END]))
            amount = cell["order_amount_krw"]
            buy_ratio = amount / previous_20d[row_array]
            sell_base = previous_20d[exit_array]
            sell_ratio = np.where(np.isfinite(sell_base) & (sell_base > 0), amount / sell_base, buy_ratio)
            gross = trades["exit_price"].to_numpy() / panel.close[row_array]
            multiplier = np.maximum(0.0, 1.0 - JUDGE_COEFFICIENT * sell_ratio) / (1.0 + JUDGE_COEFFICIENT * buy_ratio)
            market = panel.bench[panel.market_position[row_array] - panel.signal_start, panel.market_position[exit_array] - panel.signal_start]
            frame = pd.DataFrame({
                "rule": cell["rule"], "turnover_decile": cell["turnover_decile"], "order_amount_krw": amount,
                "ratio_cap_percent": cell["ratio_cap_percent"], "code": np.array(panel.codes)[stock_array],
                "name": np.array(panel.names)[stock_array], "entry_date": panel.dates[row_array],
                "entry_price_adjusted": panel.close[row_array], "entry_price_raw": panel.raw_close[row_array],
                "exit_date": panel.dates[exit_array], "exit_price_adjusted": trades["exit_price"].to_numpy(),
                "exit_price_raw": trades["exit_price"].to_numpy() * panel.raw_close[exit_array] / panel.close[exit_array],
                "exit_reason": reason, "k": np.array(span_values)[trades["span_index"].to_numpy()],
                "window": np.array(common.WINDOWS)[trades["window_index"].to_numpy()],
                "atr_period": np.array(common.ATR_PERIODS)[trades["period_index"].to_numpy()],
                "support_low_date": support_dates, "support_low_value_adjusted": support_values,
                "excess_c0_3": (gross * multiplier * common.COST - 1.0) * 100.0 - market})
            frame["duplicates_same_stock_date"] = frame.groupby(["code", "entry_date"])["k"].transform("size")
            expected = p0_table.loc[(p0_table["rule_index"] == rule_index) & (p0_table["group"] == group) & (p0_table["coefficient_index"] == judge_index)]
            emission_checks.append({"rule": cell["rule"], "group": group, "rows_written": int(written),
                                    "p0_n": float(expected["n"].iloc[0]),
                                    "mean_excess_difference": float(frame["excess_c0_3"].mean() - expected["mean_excess"].iloc[0])})
            trade_frames.append(frame[trade_columns])

        return pd.concat(trade_frames, ignore_index=True) if trade_frames else pd.DataFrame(columns=trade_columns)

    survivors_trades = trades_of(survivors)
    common.write_csv(OUTPUT / "survivors_trades.csv", survivors_trades)
    # 지시에 든 칸(판정 못 넘음)의 매매도 같은 형식으로 — 판정과 무관한 참고, 차트 재료
    named = primary[(primary["turnover_decile"] == 1) & (primary["order_amount_krw"] == 1_000_000.0)
                    & (primary["ratio_cap_percent"] == "0.5") & primary["rule"].isin(COORDINATOR_RULES)]
    named_trades = trades_of(named)
    named_trades.to_parquet(OUTPUT / "coordinator_cells_trades.parquet", index=False)     # 수백만 줄이라 parquet

    # 4회차 재현 대조: P0 거름 0 = 4회차 판 4
    reproduction_check = {}

    if full_run:
        fourth = pd.read_csv(common.STUDY / "results" / "round4" / "decision_table.csv", low_memory=False)
        fourth = fourth[fourth["variant"] == 4].copy()
        fourth["ratio_cap_percent"] = fourth["ratio_cap_percent"].astype(str)
        mine = p0_table[p0_table["filter"] == 0][["rule", "turnover_decile", "order_amount_krw", "ratio_cap_percent", "impact_coefficient", "n",
                                       "mean_excess", "t_excess"]]
        joined = mine.merge(fourth, on=["rule", "turnover_decile", "order_amount_krw", "ratio_cap_percent", "impact_coefficient"],
                            suffixes=("", "_round4"))
        reproduction_check = {"cells_matched": int(len(joined)),
                              "n_max_abs_difference": float((joined["n"] - joined["n_round4"]).abs().max()),
                              "mean_excess_max_abs_difference": float((joined["mean_excess"] - joined["mean_excess_round4"]).abs().max()),
                              "t_max_abs_difference": float((joined["t_excess"] - joined["t_excess_round4"]).abs().max())}
    else:
        fine = pd.read_parquet(common.STUDY / "results" / "round4" / "cells_judgement.parquet")
        fine = fine[fine["k"].isin(span_values)].groupby(["k", "rule", "turnover_decile", "order_amount_krw", "min_turnover_krw"])[
            ["n", "sum_excess"]].sum().reset_index()
        mine = k0[(k0["filter"] == 0) & (k0["coefficient_index"] == judge_index)][
            ["k", "rule", "turnover_decile", "order_amount_krw", "min_turnover_krw", "n", "mean_excess"]]
        joined = mine.merge(fine, on=["k", "rule", "turnover_decile", "order_amount_krw", "min_turnover_krw"], suffixes=("", "_round4"))
        reproduction_check = {"cells_matched_k_subset": int(len(joined)),
                              "n_max_abs_difference": float((joined["n"] - joined["n_round4"]).abs().max()),
                              "mean_excess_max_abs_difference": float(
                                  (joined["mean_excess"] - joined["sum_excess"] / joined["n_round4"]).abs().max())}

    timings["write"] = time.time() - step
    timings["total"] = time.time() - started
    common.write_json(OUTPUT / "multiple_comparison.json", {
        "families": {name: value for name, value in summaries.items() if name in ("P0", "P1", "P2", "K0", "K1", "K2")},
        "sign_flip": "달마다 ±1 한 부호를 그 판 모든 칸에 같이 곱한다. 시드 common.SEED, 1,000 회",
        "bar_a": f"거래 든 달 {MONTHS_FOR_BAR_A} 개월 이상 칸만 뒤집은 최대 t 의 95% 분위. 24 개월 미만 칸은 (가)를 넘은 것으로 치지 않는다",
        "bar_b": "그 판 전 칸, 자유도 보정 최소 확률 5% 분위(4회차와 같음)"})
    common.write_json(OUTPUT / "round5_metrics.json", {
        "reproduction": panel.reproduction(), "corrections": correction_information, "spans": span_values,
        "timings_seconds": timings, "slice_trades_by_rule": {common.RULE_NAMES[index]: int(value) for index, value in enumerate(used)},
        "first_month_with_trades": {key: value for key, value in summaries.items() if key.startswith("first_month")},
        "round4_reproduction_check": reproduction_check, "trade_list_checks": emission_checks,
        "judgement": {"rule": "판 P1·거름 0·계수 0.3·상한 10% 이하·주문 금액마다: (가)·(나) 둘 다 넘음, 옆 분위 부호 같음, 양수 연도 비율 ≥ 0.7, 평균 초과수익 > 0",
                      "by_amount": {str(int(amount)): {"cells": int((primary["order_amount_krw"] == amount).sum()),
                                                       "passing": int(((primary["order_amount_krw"] == amount) & primary["passes"]).sum()),
                                                       "passing_one_bar_only": int(((primary["order_amount_krw"] == amount)
                                                                                    & primary["passes_one_bar_only"]).sum())}
                                    for amount in round3_test.ORDER_AMOUNTS},
                      "passing_cells": survivors[["rule", "turnover_decile", "order_amount_krw", "ratio_cap_percent", "n", "mean_excess",
                                                  "t_excess", "share_positive_excess_years"]].to_dict("records"),
                      "survivor_trade_rows": int(len(survivors_trades))},
        "hand_set": {"judge_amounts": "100만·500만·2,000만·1억 원 각각 — 판정 금액은 사용자가 정할 값",
                     "judge_family": "P1(모든 k 합본, 같은 종목·같은 매수일 1 건) — 내가 정함",
                     "dedupe_value": "겹친 매매들 초과수익 단순 평균 — 내가 정함",
                     "surge_baseline": "t−21..t−2 20일 평균(급증한 전날을 기준에서 뺌) — 내가 정함",
                     "surge_multiples": "2·3·5·10 배 — 지시에서 정한 격자",
                     "bar_a_months": "24 개월 — 지시. 24 개월 미만 칸은 (가) 판정 밖 — 내가 정함",
                     "judgement_numbers": "계수 0.3·양수 연도 비율 0.7·상한 10% 이하 — 지시",
                     "k2_scope": "K2 는 거름 0·계수 0.3 칸만(메모리) — 내가 정함"}})
    print(f"전체 {timings['total']:.0f}초, 판정(P1) 남는 칸 {len(survivors)}", flush=True)
    print(json.dumps({name: {key: value for key, value in summary.items() if key.startswith(("bar", "cells_over", "cells_passing"))}
                      for name, summary in summaries.items() if isinstance(summary, dict)}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
