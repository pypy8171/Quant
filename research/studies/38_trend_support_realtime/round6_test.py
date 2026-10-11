"""스터디 38 6회차 — 지지 저점 결함 수정, k 묶음을 한 규칙으로 고정, 절대 거래대금 구간, 시기 효과 분리, 청산 진단.

1–5회차 산출은 건드리지 않고 results/round6/ 에만 쓴다. 매수는 신호일 종가(동시호가) 매수, 다음 날 아님.
비용·충격 비용 식·비교 평균(B)·거리 안쪽 12 구간·칸 안 보유 중 무시·주문 금액 4 개 × 상한 6 개 × 계수 4 개는 5회차와 같다.
충격 비용·하한 기준 거래대금은 전날까지 20일 평균(판 4).

바뀐 것(조정 지시 2026-10-10 6회차):
  1) 결함 수정: 매수 조건이 "추세 = 상승(확정 저점·고점 기준)"만 보고 "종가 > 마지막 확정 저점"을 보지 않았다.
     새 저점은 k 일 뒤에야 확정되므로, 그 사이 종가가 이미 마지막 확정 저점 아래인데도 추세는 상승으로 남아 매수가 났다.
     가격 섞임은 아니다(저점·종가 모두 수정주가). 고친 판은 매수일 종가 > 그 k 의 마지막 확정 저점일 때만 산다(같으면 안 산다 — 내가 정함).
     고친 조건으로 5회차 판정(P1·거름 0·계수 0.3·상한 10% 이하, 10분위)을 다시 내고 차이를 적는다.
  2) k 묶음 단일 규칙(지시에서 정의, 이 정의는 지시자가 정함): 그날 k ∈ {2,3,5,7,10,15,20,30} 중 하나라도
     (고친) 상승 추세면 1 주문. 지지 저점 = 신호 난 k 들의 마지막 확정 저점 중 가장 높은 것(고친 조건으로 모두 매수가 아래).
     같은 값이면 작은 k(내가 정함). 매도 규칙 E1·E2·E3 은 고른 k 의 저점·추세를 따른다. E4 는 k 와 무관(이평 N).
     이평 N 7 개 × ATR 5 개 = 35 묶음은 남는다 — 판 Q0 = 35 묶음 합본, Q1 = 같은 종목·같은 매수일 1 건(판정 판),
     Q2 = Q1 을 날짜 평균(하루 한 값).
  3) 거래대금 구간: 전날까지 20일 평균 거래대금 {1억 미만, 1–2억, 2–5억, 5–10억, 10–30억, 30–100억, 100억 이상}
     (지시자가 정한 격자). 10분위 판(같은 단일 규칙)도 참고로 같이 낸다.
  4) 시기 효과: (a) 같은 달 대조 — 칸 매매 초과수익에서 같은 달 같은 규칙·같은 주문 조합·같은 계수의 전 구간 매매 평균을 뺀 값의 t
     (대조 평균에 그 칸 매매도 들어간다 — 내가 정함, 차이를 줄이는 쪽). (b) 시장 거래대금 국면 — 전 종목 전날까지 20일 평균
     거래대금 합을 2010-01 – 2026-10 전체 날로 3 등분(경계를 전체 기간으로 정함 — 내가 정함, 명목 증가 때문에 국면이 시기와 겹친다).
  5) 청산 진단(판정에 안 씀): 남는 칸(없으면 100만 원 판정 영역에서 24 개월 이상 칸 중 t 가 가장 큰 칸, 참고로 표시)에
     MAE·MFE(보유 중 최저 저가·최고 고가 기준), 실현 ÷ MFE, 이긴 매매의 고점 전 최대 손실, 추세 청산 + 손절 {3,5,8,12}%
     (먼저 걸리는 쪽, 장중 저가가 선을 찍으면 그 값, 갭이면 시가. 매도 규칙 날과 겹치면 종가 규칙은 손절 우선,
     E2 는 둘 중 낮은 값 — 내가 정함) 비교표.
  6) 판정: 판 Q1 · 계수 0.3 · 상한 10% 이하 · 주문 금액 4 개 각각(판정 금액은 사용자가 정할 값). (가) 거래 든 달 24 개월
     이상 칸만으로 구한 최대 t 95% 분위, (나) 자유도 보정(그 판 전 칸) — 둘 다 넘고, 옆 구간 부호 같고, 양수 연도 비율 ≥ 0.7,
     평균 초과수익 > 0. 판(family)마다 따로 뒤집기 1,000 회(seed 20261010).
  7) 남는 칸 매매 목록은 1 건 단위(같은 종목·같은 매수일 묶음).

산출(results/round6/):
  defect_counts.csv             k 별 상승 추세 행 중 종가 ≤ 마지막 확정 저점인 행 수
  round5_fixed_compare.csv      5회차 P1 판정 영역(거름 0) 고치기 전·후 칸별 n·평균·t·통과
  round5_fixed_judgement.csv    고친 판 금액별 판정 수
  decision_table.csv            단일 규칙 × 판(Q0·Q1·Q2) × 구간 방식(absolute·decile) × 규칙 × 구간 × 금액 × 상한 × 계수
  judgement_by_amount.csv       판 × 구간 방식 × 금액별 판정 수(계수 0.3·상한 10% 이하)
  multiple_comparison.json      판별 M·기준선
  period_control.csv            (a) 같은 달 대조 t
  regime_table.csv              (b) 시장 거래대금 국면별 표
  exit_diagnostics.csv          손절 덧붙임 비교표
  mae_mfe_quantiles.csv         MAE·MFE·실현/MFE 분위수
  survivors_trades.csv          남는 칸 매매(1 건 단위). 남는 칸이 없으면 머리줄만, 참고 칸은 reference_cell_trades.csv
  round6_metrics.json           재현 정보·시간·대조
재실행: py research/studies/38_trend_support_realtime/round6_test.py
"""
from __future__ import annotations

import argparse
import os
import time

import common     # numba 보다 먼저: common 이 NUMBA_NUM_THREADS(12)를 넣는다
import numpy as np
import pandas as pd
from numba import njit, prange
import round3_test
import round4_test
import round5_test

OUTPUT = common.STUDY / "results" / "round6"
ABSOLUTE_EDGES = np.array([1e8, 2e8, 5e8, 1e9, 3e9, 1e10])      # 지시자가 정한 격자
ABSOLUTE_LABELS = ("<1억", "1–2억", "2–5억", "5–10억", "10–30억", "30–100억", ">=100억")
ABSOLUTE_SLOTS = len(ABSOLUTE_LABELS) + 1                     # 마지막 = 값 없음
DECILE_SLOTS = round3_test.SLOTS                              # 0–9 + 값 없음
STOP_PERCENTS = (3.0, 5.0, 8.0, 12.0)                         # 지시자가 정한 격자
COEFFICIENTS = round4_test.IMPACT_COEFFICIENTS
JUDGE_COEFFICIENT = 0.3
JUDGE_CAP_MAX = 10.0
JUDGE_FAMILY = "Q1"
REGIME_COUNT = 3


# ── 결함 수정 ───────────────────────────────────────────────────────────────

def fixed_states(states: np.ndarray, latest_lows: np.ndarray, close: np.ndarray) -> np.ndarray:
    """상승(1)인데 종가가 마지막 확정 저점 이하인 행을 상승에서 뺀다(0). 매도 규칙 E3 는 −1 만 보므로 청산은 그대로다."""
    fixed = states.copy()

    with np.errstate(invalid="ignore"):
        broken = (states == 1) & ~(close[None, :] > latest_lows)

    fixed[broken] = 0
    return fixed


@njit(cache=True)
def choose_support(states, latest_lows):
    """행마다 (고친) 상승인 k 들 중 마지막 확정 저점이 가장 높은 k(같으면 작은 k). 반환 (고른 k 순번 또는 −1, 신호 난 k 비트)."""
    spans = states.shape[0]
    rows = states.shape[1]
    chosen = np.full(rows, -1, dtype=np.int8)
    mask = np.zeros(rows, dtype=np.int16)

    for row in range(rows):
        best = -np.inf

        for span in range(spans):
            if states[span, row] == 1:
                mask[row] |= np.int16(1 << span)
                value = latest_lows[span, row]

                if value > best:
                    best = value
                    chosen[row] = span

    return chosen, mask


# ── 단일 규칙 커널 ──────────────────────────────────────────────────────────

@njit(cache=True)
def ensemble_exit(rule, offset, length, first, span, next_exit_span, next_exit_window, open_price, close, latest_lows):
    if rule <= 2:
        return common.exit_of(rule, offset, length, next_exit_span[span], first, open_price, close, latest_lows[span])

    return common.exit_of(rule, offset, length, next_exit_window, first, open_price, close, latest_lows[0])


@njit(cache=True)
def run_ensemble_rule(rule, averages, atrs, open_price, low, close, starts, market_position, signal_ok, month_index, year_index,
                      slot, base_turnover, amounts, minimums, coefficients, chosen, states, latest_lows, distance_edges, bench,
                      signal_start, cost, raw_month, raw_month_n, raw_year, a_month, a_month_n, a_year, day_sum, day_n):
    """단일 규칙(k 묶음) 한 매도 규칙의 35 묶음(N × ATR) 매매. 칸 = 구간 슬롯 × 조합. Q0 = raw_*, Q1 = a_*, Q2 = day_*."""
    spans = states.shape[0]
    windows = averages.shape[0]
    periods = atrs.shape[0]
    combinations = amounts.shape[0]
    coefficient_count = coefficients.shape[0]
    groups = raw_month.shape[0]
    inner_last = distance_edges.shape[0] - 1
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit_span = np.empty((spans, longest + 1), dtype=np.int64)
    next_exit_window = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    next_allowed = np.zeros(groups, dtype=np.int64)
    a_sum = np.zeros((longest, combinations, coefficient_count))
    a_count = np.zeros((longest, combinations), dtype=np.int32)
    touched = np.empty(longest * combinations, dtype=np.int64)
    values = np.empty((combinations, coefficient_count))
    computed_values = np.zeros(combinations, dtype=np.bool_)
    used = 0

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first
        touched_count = 0

        if rule <= 2:
            for span in range(spans):
                common.fill_next_exit(rule, first, length, averages[0], close, low, states[span], latest_lows[span], run_length,
                                      next_exit_span[span])

        for window in range(windows):
            average = averages[window]

            if rule >= 3:
                common.fill_next_exit(rule, first, length, average, close, low, states[0], latest_lows[0], run_length,
                                      next_exit_window)

            for period in range(periods):
                atr = atrs[period]
                next_allowed[:] = 0

                for offset in range(length):
                    row = first + offset
                    span = chosen[row]

                    if not signal_ok[row] or span < 0:
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

                        group = row_slot * combinations + combination

                        if offset < next_allowed[group]:
                            continue

                        if not computed:
                            computed = True
                            exit_offset, exit_price = ensemble_exit(rule, offset, length, first, span, next_exit_span,
                                                                    next_exit_window, open_price, close, latest_lows)

                            if exit_offset < length:
                                exit_row = first + exit_offset
                            else:
                                exit_row = last - 1
                                exit_price = close[exit_row]

                            gross = exit_price / close[row]
                            market = bench[market_position[row] - signal_start, market_position[exit_row] - signal_start]

                        if not computed_values[combination]:
                            computed_values[combination] = True
                            round5_test.trade_values(gross, market, amounts[combination], base_turnover[row],
                                                     base_turnover[exit_row], coefficients, cost, values[combination])

                        next_allowed[group] = exit_offset + 1 if exit_offset < length else length + 1
                        raw_month_n[group, month] += 1.0

                        for index in range(coefficient_count):
                            value = values[combination, index]
                            raw_month[group, index, month] += value
                            raw_year[group, index, year, 0] += 1.0
                            raw_year[group, index, year, 1] += value
                            a_sum[offset, combination, index] += value

                        if a_count[offset, combination] == 0:
                            touched[touched_count] = offset * combinations + combination
                            touched_count += 1

                        a_count[offset, combination] += 1
                        used += 1

        for position in range(touched_count):
            offset = touched[position] // combinations
            combination = touched[position] % combinations
            row = first + offset
            group = slot[row] * combinations + combination
            month = month_index[row]
            year = year_index[row]
            day = market_position[row] - signal_start
            count = a_count[offset, combination]
            a_month_n[group, month] += 1.0
            day_n[group, day] += 1.0

            for index in range(coefficient_count):
                value = a_sum[offset, combination, index] / count
                a_month[group, index, month] += value
                a_year[group, index, year, 0] += 1.0
                a_year[group, index, year, 1] += value
                day_sum[group, index, day] += value
                a_sum[offset, combination, index] = 0.0

            a_count[offset, combination] = 0

    return used


@njit(parallel=True, cache=True)
def run_ensemble(rules, averages, atrs, open_price, low, close, starts, market_position, signal_ok, month_index, year_index, slot,
                 slot_count, base_turnover, amounts, minimums, coefficients, chosen, states, latest_lows, distance_edges, bench,
                 signal_start, cost, month_count, year_count, day_count):
    rule_count = rules.shape[0]
    groups = slot_count * amounts.shape[0]
    coefficient_count = coefficients.shape[0]
    raw_month = np.zeros((rule_count, groups, coefficient_count, month_count))
    raw_month_n = np.zeros((rule_count, groups, month_count))
    raw_year = np.zeros((rule_count, groups, coefficient_count, year_count, 2))
    a_month = np.zeros((rule_count, groups, coefficient_count, month_count))
    a_month_n = np.zeros((rule_count, groups, month_count))
    a_year = np.zeros((rule_count, groups, coefficient_count, year_count, 2))
    day_sum = np.zeros((rule_count, groups, coefficient_count, day_count), dtype=np.float32)
    day_n = np.zeros((rule_count, groups, day_count), dtype=np.float32)
    used = np.zeros(rule_count, dtype=np.int64)

    for index in prange(rule_count):
        used[index] = run_ensemble_rule(rules[index], averages, atrs, open_price, low, close, starts, market_position, signal_ok,
                                        month_index, year_index, slot, base_turnover, amounts, minimums, coefficients, chosen,
                                        states, latest_lows, distance_edges, bench, signal_start, cost, raw_month[index],
                                        raw_month_n[index], raw_year[index], a_month[index], a_month_n[index], a_year[index],
                                        day_sum[index], day_n[index])

    return raw_month, raw_month_n, raw_year, a_month, a_month_n, a_year, day_sum, day_n, used


@njit(cache=True)
def emit_ensemble(rule, want_slot, minimum, averages, atrs, open_price, low, close, starts, signal_ok, slot, base_turnover, chosen,
                  states, latest_lows, distance_edges, capacity):
    """한 칸(규칙·구간·조합)의 매매를 묶음(N, ATR)마다 한 줄씩. 보유 중 무시는 run_ensemble_rule 과 같다(칸마다)."""
    spans = states.shape[0]
    inner_last = distance_edges.shape[0] - 1
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit_span = np.empty((spans, longest + 1), dtype=np.int64)
    next_exit_window = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    output = np.empty((capacity, 7), dtype=np.int64)     # 종목, 매수 행, 매도 행, 청산 이유, N 순번, ATR 순번, 고른 k 순번
    exit_prices = np.empty(capacity)
    written = 0

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first

        if rule <= 2:
            for span in range(spans):
                common.fill_next_exit(rule, first, length, averages[0], close, low, states[span], latest_lows[span], run_length,
                                      next_exit_span[span])

        for window in range(averages.shape[0]):
            average = averages[window]

            if rule >= 3:
                common.fill_next_exit(rule, first, length, average, close, low, states[0], latest_lows[0], run_length,
                                      next_exit_window)

            for period in range(atrs.shape[0]):
                atr = atrs[period]
                allowed = 0

                for offset in range(length):
                    row = first + offset
                    span = chosen[row]

                    if offset < allowed or not signal_ok[row] or span < 0 or slot[row] != want_slot:
                        continue

                    if not (np.isfinite(average[row]) and np.isfinite(atr[row]) and atr[row] > 0):
                        continue

                    distance_index = common.distance_bin((low[row] - average[row]) / atr[row], distance_edges)

                    if distance_index < 1 or distance_index > inner_last:
                        continue

                    if minimum > 0.0 and not (base_turnover[row] >= minimum):
                        continue

                    exit_offset, exit_price = ensemble_exit(rule, offset, length, first, span, next_exit_span, next_exit_window,
                                                            open_price, close, latest_lows)
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
                        output[written, 4] = window
                        output[written, 5] = period
                        output[written, 6] = span

                        exit_prices[written] = exit_price

                    written += 1

    return output[:min(written, capacity)], exit_prices[:min(written, capacity)], written


@njit(cache=True)
def trade_paths(rows, exit_rows, exit_prices, is_gap_rule, open_price, high, low, close, stops):
    """매매마다 보유 중(매수 다음 행 – 매도 행) 최저 저가·최고 고가 기준 MAE·MFE(%), 고점 행, 고점 전 MAE,
    손절 선마다 바뀐 매도 행·가격. 매도 행 날 손절이 서면 종가 규칙은 손절 우선, E2(is_gap_rule)는 낮은 값(내가 정함)."""
    count = rows.shape[0]
    stop_count = stops.shape[0]
    mae = np.empty(count)
    mfe = np.empty(count)
    mae_before_peak = np.empty(count)
    peak_rows = np.empty(count, dtype=np.int64)
    stop_rows = np.empty((count, stop_count), dtype=np.int64)
    stop_prices = np.empty((count, stop_count))
    stopped = np.zeros((count, stop_count), dtype=np.bool_)

    for index in range(count):
        row = rows[index]
        exit_row = exit_rows[index]
        entry = close[row]
        lowest = np.inf
        highest = -np.inf
        peak_row = row
        lowest_before_peak = np.inf
        running_low = np.inf

        for day in range(row + 1, exit_row + 1):
            running_low = min(running_low, low[day])

            if high[day] > highest:
                highest = high[day]
                peak_row = day
                lowest_before_peak = running_low

            lowest = min(lowest, low[day])

        if exit_row <= row:
            mae[index] = 0.0
            mfe[index] = 0.0
            mae_before_peak[index] = 0.0
        else:
            mae[index] = (lowest / entry - 1.0) * 100.0
            mfe[index] = (highest / entry - 1.0) * 100.0
            mae_before_peak[index] = (lowest_before_peak / entry - 1.0) * 100.0

        peak_rows[index] = peak_row

        for stop_index in range(stop_count):
            level = entry * (1.0 - stops[stop_index] / 100.0)
            stop_rows[index, stop_index] = exit_row
            stop_prices[index, stop_index] = exit_prices[index]

            for day in range(row + 1, exit_row + 1):
                if low[day] <= level:
                    price = open_price[day] if open_price[day] < level else level

                    if day == exit_row and is_gap_rule:
                        price = min(price, exit_prices[index])

                    stop_rows[index, stop_index] = day
                    stop_prices[index, stop_index] = price
                    stopped[index, stop_index] = True
                    break

    return mae, mfe, mae_before_peak, peak_rows, stop_rows, stop_prices, stopped


# ── 표 ──────────────────────────────────────────────────────────────────────

def describe_cells(frame: pd.DataFrame, combinations: list[dict], scheme: str) -> pd.DataFrame:
    group = frame["group"].to_numpy()
    combination_count = len(combinations)
    slot = group // combination_count
    combination = group % combination_count
    real_slots = len(ABSOLUTE_LABELS) if scheme == "absolute" else 10
    frame["scheme"] = scheme
    frame["turnover_decile"] = np.where(slot < real_slots, slot, -1)     # round4_test.neighbour_columns 가 이 이름을 쓴다
    labels = np.array(list(ABSOLUTE_LABELS) + ["값 없음"] if scheme == "absolute" else [str(index) for index in range(10)] + ["값 없음"],
                      dtype=object)
    frame["turnover_bin_label"] = labels[slot]
    frame["filter"] = 0
    frame["order_amount_krw"] = np.array([row["order_amount_krw"] for row in combinations])[combination]
    frame["ratio_cap_percent"] = np.array([("none" if row["ratio_cap_percent"] is None else str(row["ratio_cap_percent"]))
                                           for row in combinations], dtype=object)[combination]
    frame["min_turnover_krw"] = np.array([row["min_turnover_krw"] for row in combinations])[combination]
    frame["rule"] = np.array(common.RULE_NAMES)[frame["rule_index"].to_numpy()]
    frame["impact_coefficient"] = np.array(COEFFICIENTS)[frame["coefficient_index"].to_numpy()]
    return frame


def cap_values(frame: pd.DataFrame) -> pd.Series:
    return frame["ratio_cap_percent"].astype(str).map(lambda value: np.inf if value == "none" else float(value))


def month_cluster_t(values: np.ndarray, months: np.ndarray) -> float:
    """매매 값의 평균을 달 묶음 t 로(common.cluster_t 와 같은 식)."""
    if len(values) < 3:
        return float("nan")

    frame = pd.DataFrame({"value": values, "month": months}).groupby("month")["value"].agg(["sum", "size"])

    if len(frame) < 2:
        return float("nan")

    mean = values.mean()
    residual = frame["sum"].to_numpy() - frame["size"].to_numpy() * mean
    squared = float((residual * residual).sum())

    if squared <= 0:
        return float("nan")

    groups = len(frame)
    return float(mean / (np.sqrt(groups / (groups - 1) * squared) / len(values)))


def keep_thread_count() -> None:
    """새 커널 컴파일 때 numba 가 환경 변수를 다시 읽는다. 앞 단계에서 값이 빠지면 16 으로 바꾸려다 멈추므로 다시 넣는다."""
    os.environ["NUMBA_NUM_THREADS"] = "12"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-round5-fix", action="store_true", help="시험 실행용: 5회차 다시 내기(약 7 분)를 건너뛴다")
    arguments = parser.parse_args()
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
    decile_slot = np.where(deciles[:, 1] >= 0, deciles[:, 1], 10).astype(np.int64)
    absolute_slot = np.where(np.isfinite(previous_20d), np.searchsorted(ABSOLUTE_EDGES, np.nan_to_num(previous_20d), side="right"),
                             len(ABSOLUTE_LABELS)).astype(np.int64)
    combinations = round4_test.combination_grid()
    amounts = np.array([row["order_amount_krw"] for row in combinations], np.float64)
    minimums = np.array([row["min_turnover_krw"] for row in combinations], np.float64)
    coefficients = np.array(COEFFICIENTS, np.float64)
    judge_index = int(np.flatnonzero(coefficients == JUDGE_COEFFICIENT)[0])
    span_values = list(common.K_VALUES)
    states, latest_lows, pivots = [], [], []

    for span in span_values:
        state, latest_low, pivots_by_stock = panel.trend(span)
        states.append(state)
        latest_lows.append(latest_low)
        pivots.append(pivots_by_stock)

    states = np.stack(states)
    latest_lows = np.stack(latest_lows)
    states_fixed = fixed_states(states, latest_lows, panel.close)
    market_dates = panel.market_dates[panel.signal_start:]
    day_count = len(market_dates)
    day_month = ((market_dates // 10000 - panel.first_year) * 12 + (market_dates // 100) % 100 - 1).astype(np.int64)
    day_year = (market_dates // 10000 - panel.first_year).astype(np.int64)
    month_starts = np.searchsorted(day_month, np.arange(panel.month_count))
    year_starts = np.searchsorted(day_year, np.arange(panel.year_count))
    years = list(range(panel.first_year, panel.first_year + panel.year_count))
    month_count = panel.month_count
    year_count = panel.year_count
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    timings["prepare"] = time.time() - started
    print(f"준비 끝 {timings['prepare']:.0f}초", flush=True)

    # 1) 결함 수 — k 별 상승 추세 행 중 종가 ≤ 마지막 확정 저점
    signal = panel.signal_ok
    defect_rows = []

    for index, span in enumerate(span_values):
        up = signal & (states[index] == 1)
        broken = up & (states_fixed[index] != 1)
        gap = np.where(broken, latest_lows[index] / panel.close - 1.0, np.nan) * 100.0
        defect_rows.append({"k": span, "up_trend_rows": int(up.sum()), "rows_close_at_or_below_last_low": int(broken.sum()),
                            "share": float(broken.sum() / max(up.sum(), 1)),
                            "support_above_close_percent_median": float(np.nanmedian(gap)) if broken.any() else None,
                            "support_above_close_percent_p90": float(np.nanpercentile(gap, 90)) if broken.any() else None})

    any_up = signal & (states == 1).any(axis=0)
    any_up_fixed = signal & (states_fixed == 1).any(axis=0)
    defect_rows.append({"k": "any", "up_trend_rows": int(any_up.sum()),
                        "rows_close_at_or_below_last_low": int((any_up & ~any_up_fixed).sum()),
                        "share": float((any_up & ~any_up_fixed).sum() / max(any_up.sum(), 1))})
    common.write_csv(OUTPUT / "defect_counts.csv", pd.DataFrame(defect_rows))

    # 1') 5회차 판정 다시 — 같은 커널·같은 칸(거름 0–4, 다중비교 칸 수도 같게), 고친 추세만 바꾼다
    round5_compare = {}

    if not arguments.skip_round5_fix:
        step = time.time()
        rules = np.arange(len(common.RULE_NAMES), dtype=np.int64)
        results = round5_test.run_all(rules, panel.averages, panel.atrs, panel.open_price, panel.high, panel.low, panel.close,
                                      panel.starts, panel.market_position, panel.signal_ok, panel.month_index, panel.year_index,
                                      decile_slot, previous_20d, surge, np.array(round5_test.SURGE_MULTIPLES), amounts, minimums,
                                      coefficients, judge_index, states_fixed, latest_lows, common.DISTANCE_EDGES, panel.bench,
                                      panel.signal_start, common.COST, month_count, year_count, day_count)
        raw_month_n_5, a_month_5, a_month_n_5, a_year_5 = results[1], results[6], results[7], results[8]
        del results
        groups_5 = a_month_5.shape[1]
        coefficient_count = len(coefficients)
        base_5 = round5_test.describe_cells(round5_test.key_frame((len(rules), groups_5, coefficient_count),
                                                                  ["rule_index", "group", "coefficient_index"]), combinations)
        raw_total_5 = np.repeat(raw_month_n_5.sum(axis=(1, 3))[:, :, None], coefficient_count, axis=2).reshape(-1)
        p1_n = np.repeat(a_month_n_5[:, :, None, :], coefficient_count, axis=2).reshape(-1, month_count)
        keys_5 = ["family", "rule", "filter", "order_amount_krw", "ratio_cap_percent", "impact_coefficient"]
        p1_fixed, p1_summary = round5_test.family_table("P1", a_month_5.reshape(-1, month_count), p1_n,
                                                        a_year_5.reshape(-1, year_count, 2), base_5, years, signs, keys_5, raw_total_5)
        del a_month_5, a_month_n_5, a_year_5, raw_month_n_5, p1_n
        before = pd.read_csv(common.STUDY / "results" / "round5" / "decision_table.csv", low_memory=False)
        before = before[before["family"] == "P1"].copy()
        before["ratio_cap_percent"] = before["ratio_cap_percent"].astype(str)
        keys = ["rule", "filter", "turnover_decile", "order_amount_krw", "ratio_cap_percent", "impact_coefficient"]
        shown = ["n", "n_before_dedupe", "mean_excess", "t_excess", "months_with_trades", "share_positive_excess_years",
                 "neighbours_all_same_sign", "over_bar_a", "over_bar_b", "passes", "passes_one_bar_only"]
        joined = before[keys + shown].merge(p1_fixed[keys + shown], on=keys, suffixes=("_round5", "_fixed"))
        region = joined[(cap_values(joined) <= JUDGE_CAP_MAX) & (joined["impact_coefficient"] == JUDGE_COEFFICIENT)
                        & (joined["filter"] == 0)]
        common.write_csv(OUTPUT / "round5_fixed_compare.csv", region)
        judgement_5 = region.groupby("order_amount_krw").agg(
            cells=("n_fixed", "size"), passes_round5=("passes_round5", "sum"), passes_fixed=("passes_fixed", "sum"),
            one_bar_round5=("passes_one_bar_only_round5", "sum"), one_bar_fixed=("passes_one_bar_only_fixed", "sum"),
            best_t_round5=("t_excess_round5", "max"), best_t_fixed=("t_excess_fixed", "max")).reset_index()
        common.write_csv(OUTPUT / "round5_fixed_judgement.csv", judgement_5)
        round5_compare = {"P1_bars_fixed": {key: p1_summary[key] for key in ("cells_M_all", "bar_a_24_months", "bar_b_normal_equivalent_z",
                                                                               "cells_passing")},
                          "seconds": time.time() - step}
        timings["round5_fix"] = time.time() - step
        print(f"5회차 고친 판 끝 {timings['round5_fix']:.0f}초", flush=True)

    # 2–3) 단일 규칙 × 구간 방식
    keep_thread_count()
    chosen, mask = choose_support(states_fixed, latest_lows)
    rules = np.arange(len(common.RULE_NAMES), dtype=np.int64)
    schemes = {"absolute": (absolute_slot, ABSOLUTE_SLOTS), "decile": (decile_slot, DECILE_SLOTS)}
    tables, summaries, kept = [], {}, {}
    coefficient_count = len(coefficients)

    for scheme, (slot, slot_count) in schemes.items():
        step = time.time()
        keep_thread_count()
        (raw_month, raw_month_n, raw_year, a_month, a_month_n, a_year, day_sum, day_n, used) = run_ensemble(
            rules, panel.averages, panel.atrs, panel.open_price, panel.low, panel.close, panel.starts, panel.market_position,
            panel.signal_ok, panel.month_index, panel.year_index, slot, slot_count, previous_20d, amounts, minimums, coefficients,
            chosen, states_fixed, latest_lows, common.DISTANCE_EDGES, panel.bench, panel.signal_start, common.COST, month_count,
            year_count, day_count)
        timings[f"kernel_{scheme}"] = time.time() - step
        print(f"커널 {scheme} {timings[f'kernel_{scheme}']:.0f}초, 묶음별 매매 {int(used.sum()):,}", flush=True)
        groups = raw_month.shape[1]
        base = describe_cells(round5_test.key_frame((len(rules), groups, coefficient_count), ["rule_index", "group", "coefficient_index"]),
                              combinations, scheme)
        keys = ["family", "rule", "filter", "order_amount_krw", "ratio_cap_percent", "impact_coefficient"]
        q0_n = np.repeat(raw_month_n[:, :, None, :], coefficient_count, axis=2).reshape(-1, month_count)
        q0, summaries[f"Q0_{scheme}"] = round5_test.family_table("Q0", raw_month.reshape(-1, month_count), q0_n,
                                                                 raw_year.reshape(-1, year_count, 2), base, years, signs, keys)
        q1_n = np.repeat(a_month_n[:, :, None, :], coefficient_count, axis=2).reshape(-1, month_count)
        q1, summaries[f"Q1_{scheme}"] = round5_test.family_table("Q1", a_month.reshape(-1, month_count), q1_n,
                                                                 a_year.reshape(-1, year_count, 2), base, years, signs, keys,
                                                                 q0["n"].to_numpy())
        q2_month, q2_month_n = round5_test.days_to_periods(day_sum, day_n[:, :, None, :], month_starts)
        q2_year_sum, q2_year_n = round5_test.days_to_periods(day_sum, day_n[:, :, None, :], year_starts)
        q2_n = np.ascontiguousarray(np.broadcast_to(q2_month_n, q2_month.shape).reshape(-1, month_count))
        q2_year = np.stack([np.broadcast_to(q2_year_n, q2_year_sum.shape), q2_year_sum], axis=-1).reshape(-1, year_count, 2)
        q2, summaries[f"Q2_{scheme}"] = round5_test.family_table("Q2", q2_month.reshape(-1, month_count), q2_n, q2_year, base, years,
                                                                 signs, keys, q0["n"].to_numpy())
        q2 = q2.rename(columns={"n": "n_days"})
        q2["n"] = q1["n"].to_numpy()
        tables += [q0, q1, q2]
        kept[scheme] = {"a_month": a_month, "a_month_n": a_month_n, "day_sum": day_sum, "day_n": day_n, "slot": slot,
                        "slot_count": slot_count, "q1": q1, "used": used}
        del raw_month, raw_month_n, raw_year, q0_n, q1_n, q2_month, q2_month_n, q2_year_sum, q2_year_n, q2_n, q2_year
        timings[f"families_{scheme}"] = time.time() - step - timings[f"kernel_{scheme}"]

    step = time.time()
    year_columns = [f"mean_excess_{year}" for year in years]
    columns = ["scheme", "family", "rule", "turnover_decile", "turnover_bin_label", "order_amount_krw", "ratio_cap_percent",
               "min_turnover_krw", "impact_coefficient", "n", "n_before_dedupe", "n_days", "mean_excess", "t_excess",
               "months_with_trades", "years_with_trades", "share_positive_excess_years", "neighbour_deciles", "neighbour_same_sign",
               "neighbours_all_same_sign", "log10_p", "over_bar_a", "over_bar_b", "over_both", "over_one_only", "passes",
               "passes_one_bar_only"] + year_columns + ["rule_index", "group", "coefficient_index"]
    everything = pd.concat(tables, ignore_index=True).reindex(columns=columns)
    everything = everything.rename(columns={"turnover_decile": "turnover_bin", "neighbour_deciles": "neighbour_bins"})
    common.write_csv(OUTPUT / "decision_table.csv", everything)
    region = everything[(cap_values(everything) <= JUDGE_CAP_MAX) & (everything["impact_coefficient"] == JUDGE_COEFFICIENT)]
    judgement = region.groupby(["scheme", "family", "order_amount_krw"]).agg(
        cells=("n", "size"), over_bar_a=("over_bar_a", "sum"), over_bar_b=("over_bar_b", "sum"), over_both=("over_both", "sum"),
        passes=("passes", "sum"), passes_one_bar_only=("passes_one_bar_only", "sum"), best_t=("t_excess", "max")).reset_index()
    common.write_csv(OUTPUT / "judgement_by_amount.csv", judgement)
    primary = region[(region["family"] == JUDGE_FAMILY) & (region["scheme"] == "absolute")]
    survivors = primary[primary["passes"]]
    reference = pd.DataFrame()

    if survivors.empty:
        pool = primary[(primary["order_amount_krw"] == 1_000_000.0) & (primary["months_with_trades"] >= round5_test.MONTHS_FOR_BAR_A)]
        reference = pool.sort_values("t_excess", ascending=False).head(1)

    # 10분위 판(같은 단일 규칙)에서 남는 칸도 진단·목록에 같이 싣는다 — 판정 방식은 절대 구간이 주, 10분위는 참고(내가 정함)
    decile_primary = region[(region["family"] == JUDGE_FAMILY) & (region["scheme"] == "decile")]
    decile_survivors = decile_primary[decile_primary["passes"]]
    diagnostic_cells = pd.concat([survivors.assign(cell_role="남는 칸"),
                                  decile_survivors.assign(cell_role="10분위 판 남는 칸(참고 방식)"),
                                  reference.assign(cell_role="참고(절대 구간 남는 칸 없음, 100만 원 최고 t)")], ignore_index=True)

    # 4) 시기 효과 — (a) 같은 달 대조, (b) 국면
    market_total = np.zeros(panel.market_count)
    np.add.at(market_total, panel.market_position, np.nan_to_num(previous_20d))
    market_total = market_total[panel.signal_start:]
    regime_edges = np.quantile(market_total, [1.0 / 3.0, 2.0 / 3.0])
    day_regime = np.searchsorted(regime_edges, market_total, side="right")
    control_rows, regime_rows = [], []
    combination_count = len(combinations)

    for _, cell in diagnostic_cells.iterrows():
        data = kept[cell["scheme"]]
        rule_index = int(cell["rule_index"])
        group = int(cell["group"])
        combination = group % combination_count
        coefficient_index = int(cell["coefficient_index"])
        same_combination = np.arange(data["slot_count"]) * combination_count + combination
        cell_sum = data["a_month"][rule_index, group, coefficient_index]
        cell_n = data["a_month_n"][rule_index, group]
        control_sum = data["a_month"][rule_index, same_combination, coefficient_index].sum(axis=0)
        control_n = data["a_month_n"][rule_index, same_combination].sum(axis=0)

        with np.errstate(invalid="ignore", divide="ignore"):
            control_mean = np.where(control_n > 0, control_sum / np.maximum(control_n, 1), 0.0)

        demeaned = cell_sum - cell_n * control_mean
        t_demeaned = round5_test.cluster_t_matrix(demeaned[None, :], cell_n[None, :])[0]
        t_cell = round5_test.cluster_t_matrix(cell_sum[None, :], cell_n[None, :])[0]
        control_rows.append({"cell_role": cell["cell_role"], "scheme": cell["scheme"], "rule": cell["rule"], "turnover_bin": cell["turnover_bin_label"],
                             "order_amount_krw": cell["order_amount_krw"], "ratio_cap_percent": cell["ratio_cap_percent"],
                             "n": float(cell_n.sum()), "mean_excess": float(cell_sum.sum() / cell_n.sum()), "t_excess": float(t_cell),
                             "control_trades_same_months": float(control_n[cell_n > 0].sum()),
                             "control_mean_trade_weighted": float((cell_n * control_mean).sum() / cell_n.sum()),
                             "mean_difference_vs_same_month_control": float(demeaned.sum() / cell_n.sum()),
                             "t_difference_vs_same_month_control": float(t_demeaned),
                             "control_definition": "같은 규칙·같은 주문 조합(같은 하한)·같은 계수, 거래대금 구간 전부(이 칸 포함), 같은 달 Q1 평균"})
        cell_day_sum = data["day_sum"][rule_index, group, coefficient_index].astype(np.float64)
        cell_day_n = data["day_n"][rule_index, group].astype(np.float64)
        control_day_sum = data["day_sum"][rule_index, same_combination, coefficient_index].astype(np.float64).sum(axis=0)
        control_day_n = data["day_n"][rule_index, same_combination].astype(np.float64).sum(axis=0)
        # day_sum 은 그날 Q1 매매 값의 합이다(같은 날 여러 건이면 합).

        for regime in range(REGIME_COUNT):
            days = day_regime == regime
            regime_month_sum = np.bincount(day_month[days], weights=cell_day_sum[days], minlength=month_count)
            regime_month_n = np.bincount(day_month[days], weights=cell_day_n[days], minlength=month_count)
            control_regime_sum = control_day_sum[days].sum()
            control_regime_n = control_day_n[days].sum()
            regime_trade_count = regime_month_n.sum()
            regime_rows.append({"cell_role": cell["cell_role"], "scheme": cell["scheme"], "rule": cell["rule"], "turnover_bin": cell["turnover_bin_label"],
                                "order_amount_krw": cell["order_amount_krw"], "ratio_cap_percent": cell["ratio_cap_percent"],
                                "regime": ["낮음", "중간", "높음"][regime],
                                "market_turnover_low_krw": float(market_total[days].min()),
                                "market_turnover_high_krw": float(market_total[days].max()), "market_days": int(days.sum()),
                                "first_year": int(market_dates[days].min() // 10000), "last_year": int(market_dates[days].max() // 10000),
                                "n": float(regime_trade_count), "mean_excess": float(regime_month_sum.sum() / regime_trade_count) if regime_trade_count > 0 else None,
                                "t_excess": float(round5_test.cluster_t_matrix(regime_month_sum[None, :], regime_month_n[None, :])[0]),
                                "control_n": float(control_regime_n),
                                "control_mean_excess": float(control_regime_sum / control_regime_n) if control_regime_n > 0 else None})

    common.write_csv(OUTPUT / "period_control.csv", pd.DataFrame(control_rows))
    common.write_csv(OUTPUT / "regime_table.csv", pd.DataFrame(regime_rows))

    # 5)·7) 남는 칸(또는 참고 칸) 매매 — 1 건 단위, 청산 진단
    exit_rows_table, quantile_rows, emission_checks = [], [], []
    trade_frames = {"남는 칸": [], "참고": []}
    stops = np.array(STOP_PERCENTS)
    k_array = np.array(span_values)

    for _, cell in diagnostic_cells.iterrows():
        rule_index = int(cell["rule_index"])
        group = int(cell["group"])
        combination = group % combination_count
        want_slot = group // combination_count
        q0_n = float(everything.loc[(everything["scheme"] == cell["scheme"]) & (everything["family"] == "Q0")
                                    & (everything["rule_index"] == rule_index) & (everything["group"] == group)
                                    & (everything["coefficient_index"] == judge_index), "n"].iloc[0])
        keep_thread_count()
        output, exit_prices, written = emit_ensemble(rule_index, want_slot, minimums[combination], panel.averages, panel.atrs,
                                                     panel.open_price, panel.low, panel.close, panel.starts, panel.signal_ok,
                                                     kept[cell["scheme"]]["slot"], previous_20d, chosen, states_fixed, latest_lows,
                                                     common.DISTANCE_EDGES, int(q0_n) + 10)
        trades = pd.DataFrame(output, columns=["stock", "row", "exit_row", "reason", "window_index", "period_index", "span_index"])
        trades["exit_price"] = exit_prices
        amount = float(cell["order_amount_krw"])
        keep_thread_count()
        mae, mfe, mae_before_peak, peak_rows, stop_rows, stop_prices, stopped = trade_paths(
            trades["row"].to_numpy(), trades["exit_row"].to_numpy(), trades["exit_price"].to_numpy(), rule_index == 1,
            panel.open_price, panel.high, panel.low, panel.close, stops)
        row_array = trades["row"].to_numpy()

        def excess_of(exit_row_array, exit_price_array):
            buy_ratio = amount / previous_20d[row_array]
            sell_base = previous_20d[exit_row_array]
            sell_ratio = np.where(np.isfinite(sell_base) & (sell_base > 0), amount / sell_base, buy_ratio)
            multiplier = np.maximum(0.0, 1.0 - JUDGE_COEFFICIENT * sell_ratio) / (1.0 + JUDGE_COEFFICIENT * buy_ratio)
            gross = exit_price_array / panel.close[row_array]
            market = panel.bench[panel.market_position[row_array] - panel.signal_start,
                                 panel.market_position[exit_row_array] - panel.signal_start]
            return (gross * multiplier * common.COST - 1.0) * 100.0 - market

        trades["excess_c0_3"] = excess_of(trades["exit_row"].to_numpy(), trades["exit_price"].to_numpy())
        trades["realized_percent"] = (trades["exit_price"].to_numpy() / panel.close[row_array] - 1.0) * 100.0
        trades["mae_percent"] = mae
        trades["mfe_percent"] = mfe
        trades["mae_before_peak_percent"] = mae_before_peak
        trades["hold_days"] = trades["exit_row"].to_numpy() - row_array

        for stop_index, stop in enumerate(STOP_PERCENTS):
            trades[f"excess_stop_{stop:g}"] = excess_of(stop_rows[:, stop_index], stop_prices[:, stop_index])
            trades[f"stopped_{stop:g}"] = stopped[:, stop_index]
            trades[f"hold_stop_{stop:g}"] = stop_rows[:, stop_index] - row_array

        # 1 건 단위: 같은 종목·같은 매수일(35 묶음 중 들어간 것) 평균
        numeric = ["excess_c0_3", "realized_percent", "mae_percent", "mfe_percent", "mae_before_peak_percent", "hold_days"]
        numeric += [f"excess_stop_{stop:g}" for stop in STOP_PERCENTS] + [f"stopped_{stop:g}" for stop in STOP_PERCENTS]
        numeric += [f"hold_stop_{stop:g}" for stop in STOP_PERCENTS]
        trades = trades.sort_values(["stock", "row", "window_index", "period_index"], kind="mergesort")
        grouped = trades.groupby(["stock", "row"], sort=True)
        single = grouped[numeric].mean()
        single["slices"] = grouped.size()
        first_slice = grouped.first()     # 가장 작은 N(그다음 ATR) 묶음을 대표 매도로(내가 정함)
        single["exit_row"] = first_slice["exit_row"]
        single["exit_price"] = first_slice["exit_price"]
        single["reason"] = first_slice["reason"]
        single["span_index"] = first_slice["span_index"]
        single["distinct_exits"] = grouped["exit_row"].nunique()
        single["windows"] = grouped["window_index"].agg(lambda values: "|".join(str(common.WINDOWS[value]) for value in sorted(set(values))))
        single["atr_periods"] = grouped["period_index"].agg(lambda values: "|".join(str(common.ATR_PERIODS[value]) for value in sorted(set(values))))
        single = single.reset_index()
        stock_array = single["stock"].to_numpy()
        rows = single["row"].to_numpy()
        exits = single["exit_row"].to_numpy()
        span_index = single["span_index"].to_numpy()
        support_dates, support_values = [], []

        for stock, row, span in zip(stock_array, rows, span_index):
            pivot = pivots[span][stock]
            offset = row - panel.starts[stock]
            position = int(np.searchsorted(pivot["low_confirm_rows"], offset, side="right")) - 1
            support_dates.append(int(panel.dates[panel.starts[stock] + pivot["low_rows"][position]]) if position >= 0 else -1)
            support_values.append(float(pivot["low_values"][position]) if position >= 0 else np.nan)

        reason_text = np.where(single["reason"].to_numpy() == common.REASON_RULE, common.REASON_TEXT[common.REASON_RULE],
                               np.where(panel.listed_at_end[stock_array], common.REASON_TEXT[common.REASON_OPEN],
                                        common.REASON_TEXT[common.REASON_END]))
        signal_k = ["|".join(str(k_array[bit]) for bit in range(len(span_values)) if value >> bit & 1) for value in mask[rows]]
        month_of_trade = panel.month_index[rows]
        listing = pd.DataFrame({
            "cell_role": cell["cell_role"], "scheme": cell["scheme"], "rule": cell["rule"], "turnover_bin": cell["turnover_bin_label"],
            "order_amount_krw": amount, "ratio_cap_percent": cell["ratio_cap_percent"],
            "code": np.array(panel.codes)[stock_array], "name": np.array(panel.names)[stock_array], "entry_date": panel.dates[rows],
            "entry_price_adjusted": panel.close[rows], "entry_price_raw": panel.raw_close[rows], "exit_date": panel.dates[exits],
            "exit_price_adjusted": single["exit_price"].to_numpy(),
            "exit_price_raw": single["exit_price"].to_numpy() * panel.raw_close[exits] / panel.close[exits],
            "exit_reason": reason_text, "signal_k_list": signal_k, "chosen_k": k_array[span_index],
            "support_low_date": support_dates, "support_low_value_adjusted": support_values,
            "moving_average_windows": single["windows"].to_numpy(), "atr_periods": single["atr_periods"].to_numpy(),
            "slices": single["slices"].to_numpy(), "distinct_exits_across_windows": single["distinct_exits"].to_numpy(),
            "previous_20d_turnover_krw": previous_20d[rows], "excess_c0_3": single["excess_c0_3"].to_numpy(),
            "mae_percent": single["mae_percent"].to_numpy(), "mfe_percent": single["mfe_percent"].to_numpy(),
            "realized_percent": single["realized_percent"].to_numpy(), "hold_days": single["hold_days"].to_numpy()})
        trade_frames["참고" if cell["cell_role"].startswith("참고") else "남는 칸"].append(listing)
        q1_mean = float(cell["mean_excess"])
        emission_checks.append({"cell_role": cell["cell_role"], "scheme": cell["scheme"], "rule": cell["rule"], "group": group, "slice_rows": int(written),
                                "q0_n": q0_n, "single_trades": int(len(single)), "q1_n": float(cell["n"]),
                                "q1_mean_difference": float(single["excess_c0_3"].mean() - q1_mean),
                                "support_at_or_above_entry": int((np.array(support_values) >= panel.close[rows]).sum())})

        # 청산 진단 표
        label = {"cell_role": cell["cell_role"], "scheme": cell["scheme"], "rule": cell["rule"], "turnover_bin": cell["turnover_bin_label"],
                 "order_amount_krw": amount, "ratio_cap_percent": cell["ratio_cap_percent"]}
        variants = [("추세 청산만", "excess_c0_3", None, "hold_days")]
        variants += [(f"추세 청산 + 손절 {stop:g}%", f"excess_stop_{stop:g}", f"stopped_{stop:g}", f"hold_stop_{stop:g}")
                     for stop in STOP_PERCENTS]
        years_of_trade = panel.years[rows]

        for name, column, stopped_column, hold_column in variants:
            values = single[column].to_numpy()
            by_year = pd.Series(values).groupby(years_of_trade).mean()
            exit_rows_table.append({**label, "variant": name, "n": len(values), "mean_excess": float(values.mean()),
                                    "t_excess": month_cluster_t(values, month_of_trade),
                                    "win_rate": float((values > 0).mean()),
                                    "share_stopped": float(single[stopped_column].mean()) if stopped_column else 0.0,
                                    "mean_hold_days": float(single[hold_column].mean()),
                                    "share_positive_excess_years": float((by_year > 0).mean()),
                                    "median_excess": float(np.median(values))})

        winners = single["realized_percent"].to_numpy() > 0
        ratio = np.where(single["mfe_percent"].to_numpy() > 0,
                         single["realized_percent"].to_numpy() / np.where(single["mfe_percent"].to_numpy() > 0,
                                                                          single["mfe_percent"].to_numpy(), 1.0), np.nan)
        measures = {"mae_percent_all": single["mae_percent"].to_numpy(), "mfe_percent_all": single["mfe_percent"].to_numpy(),
                    "realized_percent_all": single["realized_percent"].to_numpy(),
                    "realized_over_mfe_all": ratio[np.isfinite(ratio)],
                    "mae_before_peak_percent_winners": single["mae_before_peak_percent"].to_numpy()[winners],
                    "mfe_percent_winners": single["mfe_percent"].to_numpy()[winners],
                    "mae_percent_losers": single["mae_percent"].to_numpy()[~winners],
                    "hold_days_all": single["hold_days"].to_numpy().astype(float)}

        for name, values in measures.items():
            quantiles = np.quantile(values, [0.05, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95]) if len(values) else [np.nan] * 7
            quantile_rows.append({**label, "measure": name, "n": int(len(values)), "mean": float(np.mean(values)) if len(values) else None,
                                  **{f"p{int(level * 100):02d}": float(value) for level, value in
                                     zip((0.05, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95), quantiles)}})

    common.write_csv(OUTPUT / "exit_diagnostics.csv", pd.DataFrame(exit_rows_table))
    common.write_csv(OUTPUT / "mae_mfe_quantiles.csv", pd.DataFrame(quantile_rows))
    survivors_frame = pd.concat(trade_frames["남는 칸"], ignore_index=True) if trade_frames["남는 칸"] else pd.DataFrame(
        columns=["cell_role", "rule", "turnover_bin", "order_amount_krw", "ratio_cap_percent", "code", "name", "entry_date"])
    common.write_csv(OUTPUT / "survivors_trades.csv", survivors_frame)

    if trade_frames["참고"]:
        common.write_csv(OUTPUT / "reference_cell_trades.csv", pd.concat(trade_frames["참고"], ignore_index=True))

    timings["write"] = time.time() - step
    timings["total"] = time.time() - started
    common.write_json(OUTPUT / "multiple_comparison.json", {
        "families": summaries,
        "sign_flip": "달마다 ±1 한 부호를 그 판 모든 칸에 같이 곱한다. 시드 common.SEED, 1,000 회. 판·구간 방식마다 따로",
        "bar_a": f"거래 든 달 {round5_test.MONTHS_FOR_BAR_A} 개월 이상 칸만 뒤집은 최대 t 의 95% 분위",
        "bar_b": "그 판 전 칸, 자유도 보정 최소 확률 5% 분위"})
    common.write_json(OUTPUT / "round6_metrics.json", {
        "reproduction": panel.reproduction(), "corrections": correction_information, "spans": span_values,
        "timings_seconds": timings, "slice_trades_by_rule": {scheme: {common.RULE_NAMES[index]: int(value)
                                                                      for index, value in enumerate(data["used"])}
                                                             for scheme, data in kept.items()},
        "ensemble_signal_rows": int((panel.signal_ok & (chosen >= 0)).sum()),
        "market_regime_edges_krw": regime_edges.tolist(),
        "round5_fixed": round5_compare, "trade_list_checks": emission_checks,
        "judgement": {"rule": "판 Q1(단일 규칙, 절대 구간)·계수 0.3·상한 10% 이하·주문 금액마다: (가)·(나) 둘 다, 옆 구간 부호 같음, 양수 연도 ≥ 0.7, 평균 > 0",
                      "by_amount": {str(int(amount)): {"cells": int((primary["order_amount_krw"] == amount).sum()),
                                                       "passing": int(((primary["order_amount_krw"] == amount) & primary["passes"]).sum()),
                                                       "passing_one_bar_only": int(((primary["order_amount_krw"] == amount)
                                                                                    & primary["passes_one_bar_only"]).sum())}
                                    for amount in round3_test.ORDER_AMOUNTS},
                      "decile_passing_cells": decile_survivors[["rule", "turnover_bin_label", "order_amount_krw", "ratio_cap_percent", "n",
                                                                "mean_excess", "t_excess", "share_positive_excess_years"]].to_dict("records"),
                      "passing_cells": survivors[["rule", "turnover_bin_label", "order_amount_krw", "ratio_cap_percent", "n",
                                                  "mean_excess", "t_excess", "share_positive_excess_years"]].to_dict("records"),
                      "reference_cell": reference[["rule", "turnover_bin_label", "order_amount_krw", "ratio_cap_percent", "n",
                                                   "mean_excess", "t_excess", "share_positive_excess_years"]].to_dict("records")},
        "hand_set": {"entry_fix": "종가 > 마지막 확정 저점(같으면 안 산다) — 내가 정함(지시: 결함 수정)",
                     "ensemble": "k 중 하나라도 상승이면 1 주문, 지지 = 신호 난 k 중 가장 높은 확정 저점 — 지시자가 정함. 같은 값이면 작은 k — 내가 정함",
                     "absolute_bins": "1억·2억·5억·10억·30억·100억 경계 — 지시자가 정함",
                     "same_month_control": "같은 규칙·같은 주문 조합·같은 계수, 전 구간(이 칸 포함) — 내가 정함",
                     "regime_edges": "전 종목 전날까지 20일 평균 거래대금 합, 2010-01 – 2026-10 전체 날 3 등분 — 경계를 전체 기간으로 정함(내가 정함)",
                     "stop_grid": "3·5·8·12% — 지시자가 정함, 판정에 안 씀",
                     "stop_fill": "장중 저가가 선 이하면 선 값, 시가가 선 아래면 시가. 매도 규칙 날과 겹치면 종가 규칙은 손절 우선, E2 는 낮은 값 — 내가 정함",
                     "representative_exit": "1 건 목록의 매도일·가격 = 가장 작은 N(그다음 ATR) 묶음 — 내가 정함. 초과수익은 35 묶음 평균",
                     "judge_family": "Q1 — 지시(5회차 P1 과 같은 방식)", "judgement_numbers": "계수 0.3·양수 연도 0.7·상한 10% 이하 — 지시"}})
    print(f"전체 {timings['total']:.0f}초, 판정(Q1 절대 구간) 남는 칸 {len(survivors)}", flush=True)
    print(judgement.to_string(), flush=True)


if __name__ == "__main__":
    main()
