"""스터디 38 일봉 손절 시험 — 2010-01-04 ~ 2026-10-08 전체를 한 번에, 보통주 전 종목(daily_panel.py).

무엇을 재나: DevScale 의 평단 −6.5% 손절(지금 규칙, 고칠 대상)을 "추세가 깨진 사건"으로 파는 규칙으로 바꾸면 매매가
어떻게 달라지는지. 매수는 엔진과 같은 규칙으로 신호가 난 그날 장중에 한다(다음 날 시가 아님). 이 파일은 아무 변형도
고르지 않는다 — 변형마다 결과를 다 내고, 구간·연도·무작위 비교 표를 붙인다.

매수(Quant/src/strategy/DeviationScaleStrategy.cpp 를 일봉으로 옮긴 것. 값은 config_dev_paper 그대로 — 엔진 재현이 목적):
  - 당일 필터(day_entry_allowed): 전일 ATR14 / 전일 SMA20 ≤ 4.5%, (시가 − 전일 SMA20) / 전일 SMA20 ≥ −1%, 시가 ≥ 2,000원
  - 존(judge_zone): 오늘 가격 p 를 접어 넣은 SMA5 > SMA10 > SMA20, 이격 (p − SMA20(p)) / SMA20(p) ∈ [−8%, +5%].
    접은 이평은 p 의 일차식이라 존은 가격 구간 [zlo, zhi] 로 풀린다. p_align = max(A9 − 2·A4, A19 − 2·A9),
    이격 d 의 가격 = A19·(1+d)/(19−d) (A_k = 전일까지 k 개 종가 합).
  - 매수 지정가(plan_split_steps, 개장 60분 일봉 기준선): 지정가 = 접은 SMA20. 가격이 그 위면 p* = A19/19 까지 내려와야
    체결되고, 아래면 현재가 한 호가 아래에 낸다.
      시가가 존 안이고 시가 ≤ p*  → 시가 − 1호가(저가가 닿을 때)
      시가가 존 안·위이고 p* > 존 하단 → p*(호가 내림)에 저가가 닿을 때
      시가가 존 아래 → 고가가 zlo 에 닿고 zlo ≤ p* 일 때 zlo(호가 올림)
    [limit] 개장 60분 뒤 기준선은 3분봉 SMA20 이라 일봉으로 재현되지 않는다. 2단계 분봉 대조에서 건별로 맞춰 본다.
  - 종목당 보유 중에는 다시 사지 않는다. 판 다음 날부터 다시 살 수 있다(stop_cooldown 6시간·reentry 12시간).

청산 변형(VARIANT_FIELDS):
  - kind 0 = 평단 −x% (지금 규칙). 비교 기준으로만 넣는다.
  - kind 1 = 확정 저점 이탈. 저점 k: 저가가 좌우 k 봉보다 낮은 봉, k 봉 뒤에 확정. 매수일 전날까지 확정된 저점 중
    매수가보다 낮은 가장 최근 것이 손절선이다.
  - kind 2 = 직전 N 일 최저 저가 이탈. 그 값이 매수가 이상이면(매수 순간 이미 깨져 있음) 매수가보다 낮은 가장 최근
    저가로 내려가 쓰고 ref_fallback 으로 센다.
  - kind 3 = 추세 판정 뒤집힘. 매수 뒤 새로 확정된 저점이 직전 확정 저점보다 낮으면(저점이 낮아짐 = 하락) 판다.
  - 깊이(저점에서 얼마나 아래서 끊나)는 변형으로 두지 않는다 — 깊이별 결과는 depth 표에만 낸다.
  - 판정 시점: 0 = 장중(저가가 닿으면 min(시가, 손절선)), 1 = 종가가 아래면 종가, 2 = 그 다음 날 시가
  - 손절선 올림(kind 1·2): 1 이면 매수 뒤 새 저점이 생겨 더 높아지면 손절선을 올린다(내리지 않음)
  - 익절 %(평단 기준 지정가, 0 이면 없음), 존 이탈 청산(보유 존: 전일 정배열 또는 접은 정배열, 이격 [−12%, +9%])
  - 최대 보유 없음. 거래가 끝난 종목(상장폐지 등)은 마지막 종가로, 시험 끝까지 들고 있는 건은 마지막 날 종가로 평가.
불리하게 처리하는 경우와 건수: 같은 날 내리는 선과 오르는 선이 둘 다 닿으면 내리는 쪽(ambiguous=1). 매수한 날 익절은
  entry_day_tp=0 이면 보지 않고, 1 이면 손절선이 안 닿은 날만 본다(둘 다 닿으면 ambiguous=2).
비용: 수수료 0.015% 양쪽, 매도세는 매도일의 실제 세율(SELL_TAX_SCHEDULE), 슬리피지 0·1·2 호가를 따로 낸다.
  호가 단위는 2023-01-25 전후 표와 시장(코스피·코스닥)을 따른다. [limit] 수정주가에 매기므로 분할 전 구간은 실제와 다르다.

사용:
  py research/studies/38_trend_stop_intraday/daily_stop_grid.py grid      # 순차 매매·짝 비교 격자 → data/grid_*.csv
  py research/studies/38_trend_stop_intraday/daily_stop_grid.py depth     # 깊이별 회복 표 → data/depth.npz
  py research/studies/38_trend_stop_intraday/daily_stop_grid.py random N  # 무작위 진입 N 회 → data/random.csv
"""
import json
import sys
import time
from pathlib import Path

import numba
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
DATA_DIR = HERE / "data"
PANEL_PATH = DATA_DIR / "daily_panel.npz"
KOSPI_PATH = DATA_DIR / "kospi_daily.parquet"

TEST_START = "2010-01-01"
NEW_TICK_TABLE_FROM = "2023-01-25"
COMMISSION = 0.015 / 100
# 매도일 기준 증권거래세(농특세 포함), (이 날부터, %)
SELL_TAX_SCHEDULE = [("2000-01-01", 0.30), ("2019-05-30", 0.25), ("2021-01-01", 0.23), ("2023-01-01", 0.20),
                     ("2024-01-01", 0.18), ("2025-01-01", 0.15), ("2026-01-01", 0.20)]
SLIPPAGE_TICKS = (0, 1, 2)
ATR_PERIOD = 14
ENTRY_ATR_MAX_PERCENT = 4.5
ENTRY_OPEN_DEVIATION_MIN_PERCENT = -1.0
MIN_PRICE = 2000.0
PULLBACK = 0.08
ENTRY_UPPER = 0.05
HOLD_LOW = 0.12
HOLD_HIGH = 0.09
CAPACITY = 1500

VARIANT_FIELDS = ["kind", "param", "timing", "raise", "take_profit", "zone_exit", "entry_day_tp"]
CURRENT_RULE = [0, 6.5, 0, 0, 3.0, 1, 0]
RECORD_FIELDS = ["entry_t", "entry_price", "stop_entry", "exit_t", "exit_price", "reason", "ambiguous", "ref_entry",
                 "ref_day", "ref_fallback", "atr_percent", "stop_exit", "entry_path", "atr_entry"]
REASONS = {1: "손절선 이탈", 2: "익절", 3: "존 이탈(아래)", 4: "존 이탈(위)", 5: "추세 뒤집힘", 6: "시험 끝 보유",
           8: "거래 끝(상장폐지 등)"}
ENTRY_PATHS = {1: "시가 매수(존 안·기준선 아래)", 2: "기준선까지 눌려 매수", 3: "존 하단 돌파 매수"}
TIMINGS = {0: "장중", 1: "종가", 2: "다음 날 시가"}
KINDS = {0: "평단 −x%", 1: "확정 저점 k 이탈", 2: "N일 최저 이탈", 3: "추세 뒤집힘(저점 k 낮아짐)"}


@numba.njit(cache=True)
def tick_size(price, market, new_table):
    if new_table:
        if price < 2000:
            return 1.0

        if price < 5000:
            return 5.0

        if price < 20000:
            return 10.0

        if price < 50000:
            return 50.0

        if price < 200000:
            return 100.0

        if price < 500000:
            return 500.0

        return 1000.0

    if price < 1000:
        return 1.0

    if price < 5000:
        return 5.0

    if price < 10000:
        return 10.0

    if price < 50000:
        return 50.0

    if market == 1:
        return 100.0

    if price < 100000:
        return 100.0

    if price < 500000:
        return 500.0

    return 1000.0


@numba.njit(cache=True)
def pivot_lows(low, side_bars):
    """확정 저점 목록(봉 번호, 저가, 확정일). 확정일 = 봉 번호 + k — 그날 종가 뒤에 알 수 있다."""
    count = low.shape[0]
    days = np.empty(count, np.int64)
    levels = np.empty(count)
    filled = 0

    for pivot in range(side_bars, count - side_bars):
        value = low[pivot]

        if value != value:
            continue

        is_pivot = True

        for offset in range(1, side_bars + 1):
            left = low[pivot - offset]
            right = low[pivot + offset]

            if not (left == left and right == right) or left <= value or right < value:
                is_pivot = False
                break

        if is_pivot:
            days[filled] = pivot
            levels[filled] = value
            filled += 1

    return days[:filled], levels[:filled]


@numba.njit(cache=True)
def atr_series(high, low, close):
    """날짜 t 에 쓰는 ATR14 = t-14..t-1 봉의 TR 단순평균(devscale_rules::average_true_range)."""
    count = close.shape[0]
    atr = np.full(count, np.nan)

    for day_index in range(ATR_PERIOD + 1, count):
        total = 0.0

        for bar in range(day_index - ATR_PERIOD, day_index):
            previous = close[bar - 1]
            total += max(high[bar] - low[bar], abs(high[bar] - previous), abs(low[bar] - previous))

        atr[day_index] = total / ATR_PERIOD

    return atr


@numba.njit(cache=True)
def window_sum(close, day_index, length):
    total = 0.0

    for back in range(day_index - length, day_index):
        total += close[back]

    return total


@numba.njit(cache=True)
def hold_zone_levels(close, day_index):
    """보유 존 이탈 가격(아래, 위). 전일 정배열이면 이격 띠만, 아니면 접은 정배열 하한도 건다."""
    a4 = window_sum(close, day_index, 4)
    a9 = window_sum(close, day_index, 9)
    a19 = window_sum(close, day_index, 19)
    average_5 = (a4 + close[day_index - 5]) / 5.0
    average_10 = (a9 + close[day_index - 10]) / 10.0
    average_20 = (a19 + close[day_index - 20]) / 20.0
    lower = a19 * (1.0 - HOLD_LOW) / (19.0 + HOLD_LOW)
    upper = a19 * (1.0 + HOLD_HIGH) / (19.0 - HOLD_HIGH)

    if not (average_5 > average_10 and average_10 > average_20):
        # 정배열은 엄격한 부등호라 경계 가격 자체는 존 밖이다(그 가격에 닿으면 판다)
        align_floor = max(a9 - 2.0 * a4, a19 - 2.0 * a9)
        lower = max(lower, align_floor * (1.0 + 1e-12) + 1e-9)

    return lower, upper


@numba.njit(cache=True)
def entry_price(open_, high, low, close, day_index, market, new_table):
    """그날 엔진이 샀을 가격과 경로. 못 사면 (nan, 0)."""
    previous_sma20 = window_sum(close, day_index, 20) / 20.0
    a4 = window_sum(close, day_index, 4)
    a9 = window_sum(close, day_index, 9)
    a19 = window_sum(close, day_index, 19)
    base = a19 / 19.0
    align_floor = max(a9 - 2.0 * a4, a19 - 2.0 * a9)
    # 정배열은 엄격한 부등호(p > align_floor)라 경계 바로 위부터 존이다. 호가 격자 맞춤은 경로별로 따로 한다.
    zone_low = max(align_floor * (1.0 + 1e-12) + 1e-9, a19 * (1.0 - PULLBACK) / (19.0 + PULLBACK))
    zone_high = a19 * (1.0 + ENTRY_UPPER) / (19.0 - ENTRY_UPPER)
    price_open = open_[day_index]

    if (price_open - previous_sma20) / previous_sma20 * 100.0 < ENTRY_OPEN_DEVIATION_MIN_PERCENT:
        return np.nan, 0

    if zone_low > zone_high:
        return np.nan, 0

    if price_open >= zone_low and price_open <= zone_high and price_open <= base:
        price = price_open - tick_size(price_open, market, new_table)

        if low[day_index] <= price:
            return price, 1

        return np.nan, 0

    if price_open >= zone_low:
        step = tick_size(base, market, new_table)
        price = np.floor(base / step) * step

        if price >= zone_low and low[day_index] <= price:
            return price, 2

        return np.nan, 0

    if zone_low <= base:
        step = tick_size(zone_low, market, new_table)
        price = np.ceil(zone_low / step) * step

        if high[day_index] >= price and price <= zone_high:
            return price, 3

    return np.nan, 0


@numba.njit(cache=True)
def entry_reference(kind, parameter, low, day_index, price, pivot_days, pivot_levels):
    """매수일 t 의 손절 기준 저점(값, 그 저가가 난 날, 대체 여부). 없으면 nan."""
    if kind == 1 or kind == 3:
        for index in range(pivot_days.shape[0] - 1, -1, -1):
            if pivot_days[index] + int(parameter) <= day_index - 1 and pivot_levels[index] < price:
                return pivot_levels[index], pivot_days[index], 0

        return np.nan, -1, 0

    if kind == 2:
        window = int(parameter)
        lowest = np.inf
        lowest_day = -1

        for back in range(max(0, day_index - window), day_index):
            if low[back] == low[back] and low[back] < lowest:
                lowest = low[back]
                lowest_day = back

        if lowest < price:
            return lowest, lowest_day, 0

        for back in range(day_index - 1, -1, -1):
            if low[back] == low[back] and low[back] < price:
                return low[back], back, 1

    return np.nan, -1, 0


@numba.njit(cache=True)
def exit_for_entry(open_, high, low, close, halted, last_valid, end_index, day_index, price, variant, pivot_days,
                   pivot_levels, stop):
    """t 에 price 로 산 건의 매도(날, 가격, 이유, 불리 처리, 최종 손절선)."""
    kind = int(variant[0])
    window = int(variant[1])
    timing = int(variant[2])
    raise_stop = variant[3] > 0
    take_profit = variant[4] / 100.0
    zone_exit = variant[5] > 0
    entry_day_tp = variant[6] > 0
    target = price * (1.0 + take_profit)
    price_line = kind == 0 or kind == 1 or kind == 2
    intraday = price_line and timing == 0

    # 추세 뒤집힘: 매수 전에 확정된 마지막 저점부터 시작해 새로 확정되는 저점과 비교한다.
    pivot_cursor = 0
    previous_pivot = np.nan

    if kind == 3 or (kind == 1 and raise_stop):
        while pivot_cursor < pivot_days.shape[0] and pivot_days[pivot_cursor] + window <= day_index - 1:
            previous_pivot = pivot_levels[pivot_cursor]
            pivot_cursor += 1

    ambiguous = 0

    for day in range(day_index, last_valid + 1):
        if day > day_index and (halted[day] or close[day] != close[day]):
            continue

        # 오늘 쓸 손절선 올림(전날까지 확정된 새 저점)
        if day > day_index and raise_stop:
            if kind == 1:
                scan = pivot_cursor

                while scan < pivot_days.shape[0] and pivot_days[scan] + window <= day - 1:
                    if pivot_days[scan] + window >= day_index and pivot_levels[scan] > stop:
                        stop = pivot_levels[scan]

                    scan += 1
            elif kind == 2:
                lowest = np.inf

                for back in range(day - window, day):
                    if low[back] == low[back] and low[back] < lowest:
                        lowest = low[back]

                if lowest < np.inf and lowest > stop:
                    stop = lowest

        zone_low = -np.inf
        zone_high = np.inf

        if zone_exit and day > day_index:
            zone_low, zone_high = hold_zone_levels(close, day)

        line = stop if intraday else -np.inf

        if day > day_index:
            price_open = open_[day]

            if price_open <= line:
                return day, price_open, 1, ambiguous, stop

            if price_open < zone_low:
                return day, price_open, 3, ambiguous, stop

            if take_profit > 0 and price_open >= target:
                return day, price_open, 2, ambiguous, stop

            if price_open > zone_high:
                return day, price_open, 4, ambiguous, stop

        down = -np.inf
        down_reason = 0

        if low[day] <= line:
            down, down_reason = line, 1

        if low[day] < zone_low and zone_low > down:
            down, down_reason = zone_low, 3

        up = np.inf
        up_reason = 0

        if take_profit > 0 and high[day] >= target and (day > day_index or entry_day_tp):
            up, up_reason = target, 2

        if high[day] > zone_high and zone_high < up:
            up, up_reason = zone_high, 4

        if down_reason > 0:
            if up_reason > 0:
                ambiguous = 1 if day > day_index else 2

            return day, down, down_reason, ambiguous, stop

        if up_reason > 0:
            return day, up, up_reason, ambiguous, stop

        # 종가에 판정하는 것들
        flip = False

        if kind == 3:
            while pivot_cursor < pivot_days.shape[0] and pivot_days[pivot_cursor] + window <= day:
                level = pivot_levels[pivot_cursor]

                if pivot_days[pivot_cursor] + window >= day_index and previous_pivot == previous_pivot and level < previous_pivot:
                    flip = True

                previous_pivot = level
                pivot_cursor += 1

        broke = price_line and timing != 0 and close[day] < stop

        if flip or broke:
            reason = 5 if flip else 1

            if timing == 1:
                return day, close[day], reason, ambiguous, stop

            for later in range(day + 1, last_valid + 1):
                if not halted[later] and close[later] == close[later]:
                    return later, open_[later], reason, ambiguous, stop

            return day, close[day], reason, ambiguous, stop

    reason = 6 if last_valid >= end_index else 8
    return last_valid, close[last_valid], reason, ambiguous, stop


@numba.njit(cache=True)
def last_valid_index(close):
    for day_index in range(close.shape[0] - 1, -1, -1):
        if close[day_index] == close[day_index]:
            return day_index

    return -1


@numba.njit(cache=True)
def entry_allowed(open_, close, halted, atr, day_index, start_index):
    if day_index < start_index or day_index < 21 or halted[day_index] or close[day_index] != close[day_index] or atr[day_index] != atr[day_index] or open_[day_index] < MIN_PRICE:
        return False

    for back in range(day_index - 21, day_index):
        if close[back] != close[back]:
            return False

    return atr[day_index] / (window_sum(close, day_index, 20) / 20.0) * 100.0 <= ENTRY_ATR_MAX_PERCENT


@numba.njit(cache=True)
def write_record(record, day_index, price, stop_entry, exit_t, exit_price, reason, ambiguous, reference, reference_day, fallback, atr_t,
                 sma20, stop_exit, path):
    record[0] = day_index
    record[1] = price
    record[2] = stop_entry
    record[3] = exit_t
    record[4] = exit_price
    record[5] = reason
    record[6] = ambiguous
    record[7] = reference
    record[8] = reference_day
    record[9] = fallback
    record[10] = atr_t / sma20 * 100.0
    record[11] = stop_exit
    record[12] = path
    record[13] = atr_t


@numba.njit(cache=True)
def open_position(kind, parameter, low, day_index, price, pivot_days, pivot_levels):
    """매수 순간의 손절선과 기준 저점. kind 3 은 가격 손절선이 없다(−inf)."""
    if kind == 0:
        return price * (1.0 - parameter / 100.0), np.nan, -1, 0

    reference, reference_day, fallback = entry_reference(kind, parameter, low, day_index, price, pivot_days, pivot_levels)

    if kind == 3:
        return -np.inf, reference, reference_day, fallback

    return reference, reference, reference_day, fallback


@numba.njit(cache=True)
def splitmix(seed):
    value = (seed + np.uint64(0x9E3779B97F4A7C15)) & np.uint64(0xFFFFFFFFFFFFFFFF)
    value = ((value ^ (value >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)) & np.uint64(0xFFFFFFFFFFFFFFFF)
    value = ((value ^ (value >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)) & np.uint64(0xFFFFFFFFFFFFFFFF)
    return value ^ (value >> np.uint64(31))


@numba.njit(cache=True)
def simulate_ticker(open_, high, low, close, halted, market, new_from, start_index, end_index, variant, records,
                    skipped, random_probability, random_seed):
    """한 종목 순차 매매. random_probability ≥ 0 이면 존 신호 대신 그 확률로 시가에 산다(무작위 비교)."""
    kind = int(variant[0])
    pivot_days, pivot_levels = pivot_lows(low, int(variant[1]) if (kind == 1 or kind == 3) else 1)
    atr = atr_series(high, low, close)
    last_valid = last_valid_index(close)
    filled = 0
    day_index = 21
    skipped[0] = 0
    skipped[1] = 0

    while day_index <= last_valid:
        if not entry_allowed(open_, close, halted, atr, day_index, start_index):
            day_index += 1
            continue

        new_table = day_index >= new_from

        if random_probability >= 0:
            draw = splitmix(np.uint64(random_seed) * np.uint64(1000003) + np.uint64(day_index))
            if (draw >> np.uint64(11)) * (1.0 / 9007199254740992.0) >= random_probability:
                day_index += 1
                continue

            price, path = open_[day_index], 1
        else:
            price, path = entry_price(open_, high, low, close, day_index, market, new_table)

        if path == 0:
            day_index += 1
            continue

        stop, reference, reference_day, fallback = open_position(kind, variant[1], low, day_index, price, pivot_days, pivot_levels)

        if stop != stop or (kind == 3 and reference != reference):
            skipped[0] += 1
            day_index += 1
            continue

        if filled >= records.shape[0]:
            skipped[1] = 1
            return filled

        exit_t, exit_raw, reason, ambiguous, stop_exit = exit_for_entry(
            open_, high, low, close, halted, last_valid, end_index, day_index, price, variant, pivot_days, pivot_levels, stop)
        write_record(records[filled], day_index, price, stop, exit_t, exit_raw, reason, ambiguous, reference, reference_day, fallback,
                     atr[day_index], window_sum(close, day_index, 20) / 20.0, stop_exit, path)
        filled += 1
        day_index = exit_t + 1

    return filled


@numba.njit(parallel=True, cache=True)
def simulate_all(open_, high, low, close, halted, market, new_from, start_index, end_index, variant, records, counts,
                 skipped, random_probability, random_seed):
    for ticker in numba.prange(close.shape[0]):
        counts[ticker] = simulate_ticker(open_[ticker], high[ticker], low[ticker], close[ticker], halted[ticker],
                                         market[ticker], new_from, start_index, end_index, variant, records[ticker],
                                         skipped[ticker], random_probability[ticker],
                                         random_seed * 100003 + ticker)


@numba.njit(parallel=True, cache=True)
def exits_fixed(open_, high, low, close, halted, ticker_starts, entry_t, entry_raw, end_index, variant, out):
    """정해진 매수 건(지금 규칙의 매수)마다 이 변형으로 팔았을 때. 겹침은 보지 않는다(짝 비교용)."""
    kind = int(variant[0])

    for ticker in numba.prange(ticker_starts.shape[0] - 1):
        if ticker_starts[ticker + 1] == ticker_starts[ticker]:
            continue

        pivot_days, pivot_levels = pivot_lows(low[ticker], int(variant[1]) if (kind == 1 or kind == 3) else 1)
        last_valid = last_valid_index(close[ticker])

        for trade in range(ticker_starts[ticker], ticker_starts[ticker + 1]):
            day_index = entry_t[trade]
            price = entry_raw[trade]
            stop, reference, reference_day, fallback = open_position(kind, variant[1], low[ticker], day_index, price, pivot_days,
                                                         pivot_levels)

            if stop != stop or (kind == 3 and reference != reference):
                out[trade, 0] = -1
                continue

            exit_t, exit_raw, reason, ambiguous, stop_exit = exit_for_entry(
                open_[ticker], high[ticker], low[ticker], close[ticker], halted[ticker], last_valid, end_index, day_index,
                price, variant, pivot_days, pivot_levels, stop)
            out[trade, 0] = exit_t
            out[trade, 1] = exit_raw
            out[trade, 2] = reason
            out[trade, 3] = ambiguous
            out[trade, 4] = stop
            out[trade, 5] = reference_day
            out[trade, 6] = fallback


class Panel:
    def __init__(self):
        loaded = np.load(PANEL_PATH, allow_pickle=True)
        self.dates = pd.DatetimeIndex(loaded["dates"])
        self.tickers = loaded["tickers"]
        self.names = loaded["names"]
        self.market = loaded["market"].astype(np.int64)
        self.open = np.ascontiguousarray(loaded["open"])
        self.high = np.ascontiguousarray(loaded["high"])
        self.low = np.ascontiguousarray(loaded["low"])
        self.close = np.ascontiguousarray(loaded["close"])
        self.halted = np.ascontiguousarray(loaded["halted"])
        self.big_move = loaded["big_move"]
        self.slices = {name: loaded[name] for name in loaded.files if name.endswith("_decile") or name == "in_1500"}
        self.new_from = int(np.searchsorted(self.dates, pd.Timestamp(NEW_TICK_TABLE_FROM)))
        self.start_index = int(np.searchsorted(self.dates, pd.Timestamp(TEST_START)))
        self.end_index = len(self.dates) - 1
        self.records = np.empty((len(self.tickers), CAPACITY, len(RECORD_FIELDS)))
        self.counts = np.zeros(len(self.tickers), np.int64)
        self.skipped = np.zeros((len(self.tickers), 2), np.int64)
        no_random = np.full(len(self.tickers), -1.0)
        self.no_random = no_random
        tax_dates = pd.DatetimeIndex([day for day, _ in SELL_TAX_SCHEDULE])
        self.tax = np.array([rate for _, rate in SELL_TAX_SCHEDULE])[np.searchsorted(tax_dates, self.dates, "right") - 1] / 100

    def run(self, variant, random_probability=None, random_seed=0) -> tuple[pd.DataFrame, dict]:
        probability = self.no_random if random_probability is None else random_probability
        simulate_all(self.open, self.high, self.low, self.close, self.halted, self.market, self.new_from,
                     self.start_index, self.end_index, np.asarray(variant, float), self.records, self.counts,
                     self.skipped, probability, random_seed)
        ticker_index = np.repeat(np.arange(len(self.tickers)), self.counts)
        trades = pd.DataFrame(np.concatenate([self.records[index, :count] for index, count in enumerate(self.counts)
                                              if count]), columns=RECORD_FIELDS)
        trades.insert(0, "ticker_index", ticker_index)
        extra = {"skip_no_reference": int(self.skipped[:, 0].sum()), "overflow": int(self.skipped[:, 1].sum())}
        return trades, extra

    def ticks(self, price: np.ndarray, ticker_index: np.ndarray, day: np.ndarray) -> np.ndarray:
        market = self.market[ticker_index]
        new = day >= self.new_from
        new_tick = np.select([price < 2000, price < 5000, price < 20000, price < 50000, price < 200000, price < 500000],
                             [1, 5, 10, 50, 100, 500], 1000)
        old_tick = np.select([price < 1000, price < 5000, price < 10000, price < 50000, market == 1, price < 100000,
                              price < 500000], [1, 5, 10, 50, 100, 100, 500], 1000)
        return np.where(new, new_tick, old_tick).astype(float)

    def net_return(self, ticker_index, entry_t, entry_raw, exit_t, exit_raw, slippage_ticks) -> np.ndarray:
        entry_fill = entry_raw + slippage_ticks * self.ticks(entry_raw, ticker_index, entry_t)
        exit_fill = exit_raw - slippage_ticks * self.ticks(exit_raw, ticker_index, exit_t)
        cost_in = entry_fill * (1.0 + COMMISSION)
        proceeds = exit_fill * (1.0 - COMMISSION - self.tax[exit_t])
        return proceeds / cost_in - 1.0


def summarize(panel: Panel, trades: pd.DataFrame) -> dict:
    result = {"trades": len(trades)}

    if trades.empty:
        return result

    entry_t = trades["entry_t"].to_numpy(int)
    exit_t = trades["exit_t"].to_numpy(int)
    hold = exit_t - entry_t + 1
    years = panel.dates[entry_t].year

    for slip in SLIPPAGE_TICKS:
        returns = panel.net_return(trades["ticker_index"].to_numpy(), entry_t, trades["entry_price"].to_numpy(),
                                   exit_t, trades["exit_price"].to_numpy(), slip)
        gains = returns[returns > 0].sum()
        losses = -returns[returns < 0].sum()
        result.update({
            f"s{slip}_win_rate": (returns > 0).mean() * 100, f"s{slip}_mean": returns.mean() * 100,
            f"s{slip}_median": float(np.median(returns) * 100), f"s{slip}_sum": returns.sum() * 100,
            f"s{slip}_profit_factor": gains / losses if losses > 0 else np.inf,
            f"s{slip}_per_hold_day": returns.sum() / hold.sum() * 100, f"s{slip}_worst": returns.min() * 100})

        if slip == 1:
            for year in range(2010, 2027):
                result[f"y{year}"] = returns[years == year].sum() * 100

    stop_entry = trades["stop_entry"].to_numpy()
    finite = np.isfinite(stop_entry)
    result.update({
        "mean_hold_days": hold.mean(), "median_hold_days": float(np.median(hold)),
        "ambiguous_both_lines": int((trades["ambiguous"] == 1).sum()),
        "ambiguous_entry_day": int((trades["ambiguous"] == 2).sum()),
        "ref_fallback": int(trades["ref_fallback"].sum()),
        "mean_stop_distance": float(((trades["entry_price"] - stop_entry) / trades["entry_price"])[finite].mean() * 100)
        if finite.any() else np.nan})

    for code in REASONS:
        result[f"reason_{code}"] = int((trades["reason"] == code).sum())

    return result


def stop_variants() -> list[list[float]]:
    """깊이 없는 사건 손절 변형 전부(익절·존 이탈 없음)."""
    variants = []

    for kind, parameters in ((1, range(1, 11)), (2, range(3, 41))):
        for parameter in parameters:
            for timing in (0, 1, 2):
                for raise_stop in (0, 1):
                    variants.append([kind, parameter, timing, raise_stop, 0, 0, 0])

    for parameter in range(1, 11):
        for timing in (1, 2):
            variants.append([3, parameter, timing, 0, 0, 0, 0])

    return variants


def all_variants() -> list[list[float]]:
    variants = [CURRENT_RULE, [0, 6.5, 0, 0, 3.0, 1, 1], [0, 6.5, 0, 0, 0, 0, 0]]

    for base in stop_variants():
        for take_profit in range(0, 16):
            for zone_exit in (0, 1):
                variants.append(base[:4] + [take_profit, zone_exit, 0])

    return variants


def variant_key(variant) -> str:
    return "_".join(f"{value:g}" for value in variant)


def stored_variant(variant) -> bool:
    """매매 건을 파일로 남기는 변형 — 지금 규칙 3개와 익절·존 이탈 없는 사건 손절 전부."""
    return variant[0] == 0 or (variant[4] == 0 and variant[5] == 0)


def current_entries(panel: Panel) -> pd.DataFrame:
    path = DATA_DIR / "entries_current.parquet"

    if path.exists():
        return pd.read_parquet(path)

    current, _ = panel.run(CURRENT_RULE)
    current = current.sort_values(["ticker_index", "entry_t"]).reset_index(drop=True)
    current.to_parquet(path)
    return current


def run_grid(panel: Panel) -> None:
    variants = all_variants()
    current = current_entries(panel)
    ticker_starts = np.searchsorted(current["ticker_index"].to_numpy(), np.arange(len(panel.tickers) + 1)).astype(np.int64)
    entry_t = current["entry_t"].to_numpy(np.int64)
    entry_raw = current["entry_price"].to_numpy()
    trades_directory = DATA_DIR / "trades"
    trades_directory.mkdir(exist_ok=True)
    sequential_rows = []
    paired_rows = []
    out = np.zeros((len(current), 7))
    started = time.time()

    for number, variant in enumerate(variants):
        trades, extra = panel.run(variant)
        row = dict(zip(VARIANT_FIELDS, variant))
        row.update(summarize(panel, trades))
        row.update(extra)
        sequential_rows.append(row)

        exits_fixed(panel.open, panel.high, panel.low, panel.close, panel.halted, ticker_starts, entry_t, entry_raw,
                    panel.end_index, np.asarray(variant, float), out)
        valid = out[:, 0] >= 0
        paired = pd.DataFrame({"trade_id": np.arange(len(current)), "ticker_index": current["ticker_index"],
                               "entry_t": entry_t, "entry_price": entry_raw, "exit_t": out[:, 0].astype(int),
                               "exit_price": out[:, 1], "reason": out[:, 2], "ambiguous": out[:, 3],
                               "stop_entry": out[:, 4], "ref_day": out[:, 5], "ref_fallback": out[:, 6]})[valid]
        paired_row = dict(zip(VARIANT_FIELDS, variant))
        paired_row.update(summarize(panel, paired))
        paired_row["no_reference"] = int((~valid).sum())
        paired_rows.append(paired_row)

        if stored_variant(variant):
            trades.to_parquet(trades_directory / f"seq_{variant_key(variant)}.parquet")
            paired.to_parquet(trades_directory / f"pair_{variant_key(variant)}.parquet")

        if number % 500 == 0:
            print(f"  {number + 1}/{len(variants)} · {time.time() - started:.0f}초", flush=True)

    pd.DataFrame(sequential_rows).to_csv(DATA_DIR / "grid_sequential.csv", index=False, encoding="utf-8-sig")
    pd.DataFrame(paired_rows).to_csv(DATA_DIR / "grid_paired.csv", index=False, encoding="utf-8-sig")
    print(f"→ grid_sequential.csv · grid_paired.csv · trades/ ({len(variants)}개, {time.time() - started:.0f}초)")


# ── 깊이별 회복 표 ────────────────────────────────────────────────────────────────────
# 지금 규칙의 매수 건마다, 기준 저점 아래 깊이 d 의 선에 처음 닿은 뒤 무슨 일이 있었는지. 추적은 데이터 끝까지.
DEPTH_DEFINITIONS = [(1, side_bars) for side_bars in range(1, 11)] + [(2, window_days) for window_days in range(3, 41)]
DEPTH_PERCENT = np.round(np.arange(0, 10.0001, 0.1), 2)
DEPTH_ATR = np.round(np.arange(0, 3.0001, 0.05), 2)
RECLAIM_WINDOWS = (1, 2, 3, 5, 10, 20)
RECLAIM_WINDOWS_NB = np.array(RECLAIM_WINDOWS, np.int64)
DISTANCE_EDGES = (0, 2, 4, 6, 8, 10, 15)
YEARS = tuple(range(2010, 2027))
BUCKET_LABELS = (["전체"] + [f"ATR% {decile + 1}분위" for decile in range(10)] + [f"전일 코스피 {decile + 1}분위" for decile in range(10)]
                 + [f"손절 거리 {low}–{high}%" for low, high in zip(DISTANCE_EDGES, DISTANCE_EDGES[1:])]
                 + [f"손절 거리 {DISTANCE_EDGES[-1]}% 이상"] + [f"{year}년" for year in YEARS])
DEPTH_METRICS = (["기준 있음", "닿음", "그날 종가 회복"] + [f"{window_days}일 안 선 회복" for window_days in RECLAIM_WINDOWS] + ["언젠가 선 회복"]
                 + [f"{window_days}일 안 매수가 회복" for window_days in RECLAIM_WINDOWS] + ["언젠가 매수가 회복", "추가 하락 합(%)",
                 "비교 건(지금 규칙이 아직 보유)", "들고 있음−팖 합(%p)", "닿기 전 지금 규칙이 이미 팖", "팖 수익 합(%)",
                 "들고 있음 수익 합(%)", "이 선 손절만 쓴 수익 합(%)", "이 선 손절만 쓴 보유일 합"])


@numba.njit(cache=True)
def trading_days_between(halted, start, end):
    total = 0

    for day in range(start + 1, end + 1):
        if not halted[day]:
            total += 1

    return total


@numba.njit(cache=True)
def depth_outcome(open_, low, close, halted, last_valid, suffix_max_close, suffix_min_low, day_index, price, line, touch,
                  current_exit_t, current_exit_raw, cell):
    """선 line 에 touch 날 처음 닿은 건 하나를 cell(지표 벡터)에 더한다. 회복 = 종가가 선(또는 매수가) 이상."""
    cell[1] += 1
    reclaim = -1

    if close[touch] >= line:
        cell[2] += 1
        reclaim = touch
    elif touch < last_valid and suffix_max_close[touch + 1] >= line:
        for day in range(touch + 1, last_valid + 1):
            if close[day] >= line:
                reclaim = day
                break

    if reclaim >= 0:
        lowest = low[touch]

        for day in range(touch, reclaim + 1):
            lowest = min(lowest, low[day])

        cell[9] += 1
        waited = trading_days_between(halted, touch, reclaim)

        for index in range(RECLAIM_WINDOWS_NB.shape[0]):
            if waited <= RECLAIM_WINDOWS_NB[index]:
                cell[3 + index] += 1
    else:
        lowest = suffix_min_low[touch]

    cell[17] += (lowest / line - 1.0) * 100.0
    entry_back = -1

    if suffix_max_close[touch] >= price:
        for day in range(touch, last_valid + 1):
            if close[day] >= price:
                entry_back = day
                break

    if entry_back >= 0:
        cell[16] += 1
        waited = trading_days_between(halted, touch, entry_back)

        for index in range(RECLAIM_WINDOWS_NB.shape[0]):
            if waited <= RECLAIM_WINDOWS_NB[index]:
                cell[10 + index] += 1

    sold = line if touch == day_index else min(open_[touch], line)
    cell[23] += (sold / price - 1.0) * 100.0
    cell[24] += touch - day_index + 1

    if current_exit_t < touch:
        cell[20] += 1
        return

    held = (current_exit_raw / price - 1.0) * 100.0
    sold_return = (sold / price - 1.0) * 100.0
    cell[18] += 1
    cell[19] += held - sold_return
    cell[21] += sold_return
    cell[22] += held


@numba.njit(parallel=True, cache=True)
def depth_tables(open_, low, close, halted, suffix_max_close, suffix_min_low, ticker_starts, entry_t, entry_raw,
                 current_exit_t, current_exit_raw, atr_entry, buckets_fixed, definitions, percents, multiples,
                 distance_edges, table):
    """table[정의, 구간, 깊이(퍼센트 다음 ATR 배수), 지표]. 정의마다 한 스레드라 쓰기가 겹치지 않는다."""
    percent_count = percents.shape[0]
    level_count = percent_count + multiples.shape[0]
    fixed_columns = buckets_fixed.shape[1]

    for definition in numba.prange(definitions.shape[0]):
        kind = definitions[definition, 0]
        parameter = definitions[definition, 1]
        lines = np.empty(level_count)
        touches = np.empty(level_count, np.int64)
        buckets = np.empty(fixed_columns + 1, np.int64)

        for ticker in range(ticker_starts.shape[0] - 1):
            if ticker_starts[ticker + 1] == ticker_starts[ticker]:
                continue

            pivot_days, pivot_levels = pivot_lows(low[ticker], parameter if kind == 1 else 1)
            last_valid = last_valid_index(close[ticker])

            for trade in range(ticker_starts[ticker], ticker_starts[ticker + 1]):
                day_index = entry_t[trade]
                price = entry_raw[trade]
                reference, reference_day, fallback = entry_reference(kind, parameter, low[ticker], day_index, price, pivot_days, pivot_levels)

                if reference != reference:
                    continue

                distance = (price - reference) / price * 100.0
                distance_bucket = 0

                for edge in range(distance_edges.shape[0]):
                    if distance >= distance_edges[edge]:
                        distance_bucket = edge

                for column in range(fixed_columns):
                    buckets[column] = buckets_fixed[trade, column]

                buckets[fixed_columns] = 21 + distance_bucket

                for level in range(percent_count):
                    lines[level] = reference * (1.0 - percents[level] / 100.0)

                for level in range(multiples.shape[0]):
                    lines[percent_count + level] = reference - multiples[level] * atr_entry[trade]

                for level in range(level_count):
                    touches[level] = -1

                # 퍼센트 선과 ATR 선은 각각 깊을수록 낮다 — 날마다 아직 안 닿은 가장 높은 선만 보면 된다.
                pointer_percent = 0
                pointer_atr = percent_count

                for day in range(day_index, last_valid + 1):
                    if day > day_index and halted[ticker, day]:
                        continue

                    while pointer_percent < percent_count and low[ticker, day] <= lines[pointer_percent]:
                        touches[pointer_percent] = day
                        pointer_percent += 1

                    while pointer_atr < level_count and low[ticker, day] <= lines[pointer_atr]:
                        touches[pointer_atr] = day
                        pointer_atr += 1

                    if pointer_percent == percent_count and pointer_atr == level_count:
                        break

                for level in range(level_count):
                    for column in range(fixed_columns + 1):
                        if buckets[column] >= 0:
                            table[definition, buckets[column], level, 0] += 1

                    if touches[level] < 0 or lines[level] <= 0:
                        # 끝까지 안 닿음 — 이 선 손절만 쓰면 마지막 종가까지 들고 있다.
                        for column in range(fixed_columns + 1):
                            if buckets[column] >= 0:
                                cell = table[definition, buckets[column], level]
                                cell[23] += (close[ticker, last_valid] / price - 1.0) * 100.0
                                cell[24] += last_valid - day_index + 1

                        continue

                    for column in range(fixed_columns + 1):
                        if buckets[column] >= 0:
                            depth_outcome(open_[ticker], low[ticker], close[ticker], halted[ticker], last_valid,
                                          suffix_max_close[ticker], suffix_min_low[ticker], day_index, price, lines[level],
                                          touches[level], current_exit_t[trade], current_exit_raw[trade],
                                          table[definition, buckets[column], level])


def decile_of(values: np.ndarray) -> np.ndarray:
    ranks = pd.Series(values).rank(method="first", pct=True).to_numpy()
    return np.where(np.isfinite(values), np.minimum((np.nan_to_num(ranks) * 10).astype(np.int64), 9), -1)


def kospi_previous_change(panel: Panel) -> np.ndarray:
    """날짜 t 에 붙는 값 = t-1 의 코스피 등락률(%). 매수 순간에 알 수 있던 값."""
    kospi = pd.read_parquet(KOSPI_PATH)["close"]
    kospi.index = pd.DatetimeIndex(kospi.index)
    kospi = kospi.reindex(panel.dates).ffill()
    return ((kospi / kospi.shift(1) - 1).shift(1) * 100).to_numpy()


def suffix_arrays(panel: Panel) -> tuple[np.ndarray, np.ndarray]:
    close = np.where(np.isfinite(panel.close), panel.close, -np.inf)
    low = np.where(np.isfinite(panel.low), panel.low, np.inf)
    suffix_max_close = np.ascontiguousarray(np.maximum.accumulate(close[:, ::-1], axis=1)[:, ::-1])
    suffix_min_low = np.ascontiguousarray(np.minimum.accumulate(low[:, ::-1], axis=1)[:, ::-1])
    return suffix_max_close, suffix_min_low


def run_depth(panel: Panel) -> None:
    current = current_entries(panel)
    ticker_index = current["ticker_index"].to_numpy()
    entry_t = current["entry_t"].to_numpy(np.int64)
    ticker_starts = np.searchsorted(ticker_index, np.arange(len(panel.tickers) + 1)).astype(np.int64)
    kospi = kospi_previous_change(panel)[entry_t]
    year = panel.dates[entry_t].year.to_numpy()
    kospi_bucket = np.where(np.isfinite(kospi), 11 + decile_of(kospi), -1)
    buckets_fixed = np.ascontiguousarray(np.stack([
        np.zeros(len(current), np.int64), 1 + decile_of(current["atr_percent"].to_numpy()), kospi_bucket,
        np.where((year >= YEARS[0]) & (year <= YEARS[-1]), 28 + year - YEARS[0], -1)], axis=1).astype(np.int64))
    definitions = np.array(DEPTH_DEFINITIONS, np.int64)
    table = np.zeros((len(definitions), len(BUCKET_LABELS), len(DEPTH_PERCENT) + len(DEPTH_ATR), len(DEPTH_METRICS)))
    suffix_max_close, suffix_min_low = suffix_arrays(panel)
    started = time.time()
    depth_tables(panel.open, panel.low, panel.close, panel.halted, suffix_max_close, suffix_min_low, ticker_starts,
                 entry_t, current["entry_price"].to_numpy(), current["exit_t"].to_numpy(np.int64),
                 current["exit_price"].to_numpy(), current["atr_entry"].to_numpy(), buckets_fixed, definitions,
                 DEPTH_PERCENT, DEPTH_ATR, np.array(DISTANCE_EDGES, float), table)
    np.savez_compressed(DATA_DIR / "depth.npz", table=table, definitions=definitions, percents=DEPTH_PERCENT,
                        multiples=DEPTH_ATR, buckets=np.array(BUCKET_LABELS), metrics=np.array(DEPTH_METRICS),
                        kospi_edges=np.nanpercentile(kospi, np.arange(0, 101, 10)),
                        atr_edges=np.nanpercentile(current["atr_percent"], np.arange(0, 101, 10)))
    print(f"→ depth.npz {table.shape} ({time.time() - started:.0f}초)")


# ── 무작위 진입 비교 ──────────────────────────────────────────────────────────────────
# 신호 대신 무작위로 산다. 종목마다 확률 = 지금 규칙의 매수 건수 / 매수 가능한 날 수, 매수가는 그날 시가, 보유 중엔 안 산다.
# 회차마다 손절 변형 전부를 같은 무작위 날짜로 돌려, 실제 신호의 "가장 좋은 변형"이 무작위의 "가장 좋은 변형"보다 나은지 본다.
@numba.njit(parallel=True, cache=True)
def eligible_days(open_, high, low, close, halted, start_index, counts):
    for ticker in numba.prange(close.shape[0]):
        atr = atr_series(high[ticker], low[ticker], close[ticker])
        total = 0

        for day_index in range(21, close.shape[1]):
            if entry_allowed(open_[ticker], close[ticker], halted[ticker], atr, day_index, start_index):
                total += 1

        counts[ticker] = total


def random_metrics(panel: Panel, trades: pd.DataFrame) -> dict:
    if trades.empty:
        return {"trades": 0}

    entry_t = trades["entry_t"].to_numpy(int)
    exit_t = trades["exit_t"].to_numpy(int)
    returns = panel.net_return(trades["ticker_index"].to_numpy(), entry_t, trades["entry_price"].to_numpy(), exit_t,
                               trades["exit_price"].to_numpy(), 1)
    losses = -returns[returns < 0].sum()
    return {"trades": len(trades), "s1_mean": returns.mean() * 100, "s1_sum": returns.sum() * 100,
            "s1_per_hold_day": returns.sum() / (exit_t - entry_t + 1).sum() * 100,
            "s1_profit_factor": returns[returns > 0].sum() / losses if losses > 0 else np.inf}


def random_variants() -> list[list[float]]:
    return [CURRENT_RULE] + stop_variants()


def run_random(panel: Panel, rounds: int) -> None:
    current = current_entries(panel)
    counts = np.zeros(len(panel.tickers), np.int64)
    eligible_days(panel.open, panel.high, panel.low, panel.close, panel.halted, panel.start_index, counts)
    entries = np.bincount(current["ticker_index"], minlength=len(panel.tickers))
    probability = np.where(counts > 0, entries / np.maximum(counts, 1), 0.0)
    path = DATA_DIR / "random.csv"
    done = int(pd.read_csv(path)["seed"].max()) + 1 if path.exists() else 0
    started = time.time()

    for seed in range(done, rounds):
        rows = []

        for variant in random_variants():
            trades, _ = panel.run(variant, probability, seed + 1)
            rows.append({"seed": seed, "variant": variant_key(variant), **random_metrics(panel, trades)})

        pd.DataFrame(rows).to_csv(path, mode="a", header=not path.exists(), index=False)
        print(f"  회차 {seed + 1}/{rounds} · {time.time() - started:.0f}초", flush=True)


def main() -> None:
    command = sys.argv[1] if len(sys.argv) > 1 else "grid"
    panel = Panel()

    if command == "grid":
        run_grid(panel)
        return

    if command == "depth":
        run_depth(panel)
        return

    if command == "random":
        run_random(panel, int(sys.argv[2]))
        return

    if command == "one":
        trades, extra = panel.run(json.loads(sys.argv[2]))
        print(json.dumps({**summarize(panel, trades), **extra}, ensure_ascii=False, indent=1, default=float))
        return

    raise SystemExit(f"모르는 명령: {command}")


if __name__ == "__main__":
    main()
