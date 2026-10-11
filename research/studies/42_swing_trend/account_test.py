"""스터디 42-5 — 고점·저점 추세 매수(실매매 규칙)를 슬롯 계좌로 굴려 코스피·코스닥·무작위 대조와 비교한다(2026-10-11).

시험 목록·판정 기준·내가 정함은 README.md 2절에 실행 전에 고정했다. 기간은 2010-01-04 – 2026-10-08 한 번에 돌린다.
시장 자료(38 격자·41 종목·가격 비 보정·정지·폐지·매도세)는 스터디 43 harness.Market 을 그대로 쓰고, 통계(달·해 t, 묶음 통과선,
지표 표)는 스터디 41 study41·43 run_stages 함수를 import 해 쓴다. 매수·매도 판정과 계좌 커널만 여기 새로 둔다.

매수(candidates.py 실매매 규칙): 확정 고점·저점(w20, swing_trend.is_extreme)으로 L2 > L1 이고 종가가 H2 를 처음 넘으면 대기.
  다음 날부터 고가 > H2·저가 ≤ H2 인 날 대기를 끝내고, 그날 시가 > H2 면 H2(호가 내림) 지정가에 산다. 대기 중 종가 < L2 면 취소.
  거름(F): 신호일 year_high ≥ 0.809, above240 0.0169–0.32, run20 ≤ 0.264, atr_percent ≤ 5.9. 무수정 매수가 ≥ 2,000원.
매도: 매수 다음 날부터 종가 < 손절선이면 그날 종가(종가 하한가 잠김이면 다음 가능한 종가). 손절선 = L2, 더 높은 저점이 확정되면 올림.
하루 순서: 폐지 현금화 → 매수(전략 장중 지정가, 대조 시가) → 종가 매도 → 종가 평가. 보유 중·판 날의 신호는 무시한다.

실행: py -I -X utf8 research/studies/42_swing_trend/account_test.py
산출: results/*.csv·run_info.json(작은 표), data/account_curves.parquet·data/account_trades/*.parquet(큰 배열, git 제외)
"""
from __future__ import annotations

import hashlib
import json
import math
import subprocess
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
STUDIES = STUDY.parent
sys.path.insert(0, str(STUDIES / "43_devscale_real_rules"))

import harness     # noqa: E402  41·38 경로와 numba 스레드 수를 먼저 정한다
import run_stages     # noqa: E402
from harness import basket, grid, study41     # noqa: E402

sys.path.insert(0, str(STUDY))

import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402
from swing_trend import is_extreme     # noqa: E402

RESULTS = STUDY / "results"
DATA = STUDY / "data"
WINDOW = 20
SLOT_AMOUNT = harness.SLOT_AMOUNT
COMMISSION = harness.COMMISSION
MIN_PRICE = harness.MIN_PRICE
SLOT_CHOICES = (10, 20, 50, 100)
AMOUNT_CHOICES = (harness.AMOUNT_FIXED, harness.AMOUNT_COMPOUND)
SLIPPAGE_CHOICES = (0, 1, 2)
FILTER_CHOICES = (1, 0)
FILTER_TEXT = {1: "(F) 거름 4개", 0: "(R) 거름 없음"}
ORDER_TURNOVER, ORDER_RANDOM = 0, 1
CONTROL_SEEDS = run_stages.CONTROL_SEEDS
CONTROL_WIN_NEED = run_stages.CONTROL_WIN_NEED
TOP_SHARE = 0.01
# 거름 문턱(filter_rule.py STEPS 앞 4개)과 이웃 칸(42-2 data/buckets.csv w20 breakout 20% 단위 경계). inf = 그 쪽 문턱 없음
CENTER = {"year_high_min": 0.809, "above240_min": 0.0169, "above240_max": 0.32, "run20_max": 0.264,
          "atr_percent_max": 5.9}
NEIGHBORS = {"year_high_min": (0.700, 0.809, 0.897), "above240_min": (-np.inf, 0.0169, 0.0873),
             "above240_max": (0.173, 0.32, np.inf), "run20_max": (0.175, 0.264, np.inf),
             "atr_percent_max": (4.45, 5.9, np.inf)}
THRESHOLD_NAMES = tuple(CENTER)
REPRESENTATIVE = {"filtered": 1, "slots": 100, "amount_mode": harness.AMOUNT_FIXED, "slippage": 1}
PARAMETER_FIELDS = ("filtered", "slots", "amount_mode", "slippage", "order", "seed", "control", "pair_row", "log",
                    *THRESHOLD_NAMES, "strict_touch", "block_row")
FIELD = {name: number for number, name in enumerate(PARAMETER_FIELDS)}
# 앞 12개는 43 SCALAR_FIELDS 와 같은 자리(run_stages.summary_rows 를 그대로 쓴다)
SCALAR_FIELDS = (*harness.SCALAR_FIELDS, "missed_no_slot", "exact_touch_buys")
SCALAR = {name: number for number, name in enumerate(SCALAR_FIELDS)}
LOG_FIELDS = ("stock", "entry_day", "exit_day", "reason", "spend", "proceeds", "entry_price", "exit_price", "signal_day")
REASON_STOP, REASON_DELIST, REASON_END, REASON_HOLD = 1, harness.REASON_DELIST, harness.REASON_END, 15
REASON_TEXT = {REASON_STOP: "손절선 아래 종가", REASON_DELIST: "상장폐지 현금화", REASON_END: "시험 끝 보유",
               REASON_HOLD: "짝 보유일 끝(대조)"}
LOG_CAPACITY = 60_000
BATCH_LOGGED = 24
BATCH_PLAIN = 480


# ───────────────────────── 신호·조건값(계좌와 무관, 한 번 계산) ─────────────────────────

@njit(parallel=True, cache=True)
def swing_events(high, low, close, halted, start_index, window, active, signal_level, signal_cancel, low_event):
    """종목 × 날짜: 판정하는 날(active), 그날 아침 생긴 추세 확인(H2·L2), 그날 아침 확정된 저점 값(low_event).

    swing_trend.ticker_trades 의 고점·저점 갱신과 신호 조건을 그대로 옮겼다. 확정은 어제 − w 봉까지라 앞을 보지 않는다.
    """
    for stock in prange(close.shape[0]):
        high1 = high2 = low1 = low2 = np.nan
        row_high = high[stock]
        row_low = low[stock]

        for day in range(2 * window + 2, close.shape[1]):
            yesterday = day - 1
            candidate = yesterday - window

            if is_extreme(row_high, candidate, window, 1.0):
                high1, high2 = high2, row_high[candidate]

            if is_extreme(row_low, candidate, window, -1.0):
                low1, low2 = low2, row_low[candidate]
                low_event[stock, day] = low2

            if not (high1 == high1 and low1 == low1) or halted[stock, day] or not close[stock, day] > 0:
                continue

            if day < start_index:
                continue

            active[stock, day] = True

            if low2 > low1 and close[stock, yesterday] > high2 and close[stock, yesterday - 1] <= high2:
                signal_level[stock, day] = high2
                signal_cancel[stock, day] = low2


def feature_matrices(market: harness.Market) -> dict:
    """swing_trend.features 와 같은 정의의 종목 × 날짜 행렬(그날 종가까지의 자료)."""
    close_frame = pd.DataFrame(market.close.T).ffill()
    close_filled = close_frame.to_numpy()
    year_high = pd.DataFrame(market.high.T).rolling(250, min_periods=120).max().shift(1).to_numpy()
    sma240 = close_frame.rolling(240).mean().to_numpy()
    sma20 = close_frame.rolling(20).mean().to_numpy()
    average_range = np.full((market.stock_count, market.day_count), np.nan)
    harness.atr_matrix(market.high, market.low, market.close, average_range)

    with np.errstate(invalid="ignore", divide="ignore"):
        features = {"year_high": close_filled / year_high,
                    "above240": close_filled / sma240 - 1.0,
                    "run20": close_filled / close_frame.shift(20).to_numpy() - 1.0,
                    "atr_percent": average_range.T / sma20 * 100.0}

    return {name: np.ascontiguousarray(values.T) for name, values in features.items()}


def close_lock_matrix(market: harness.Market) -> np.ndarray:
    """41 lock 비트 8(종가가 하한가) — 종가 매도를 미룬다."""
    inputs = np.load(study41.CACHE / "inputs.npz", allow_pickle=False)
    lock = np.zeros((market.stock_count, market.day_count), dtype=np.int64)
    lock[:, market.first_day:] = inputs["lock"].T.astype(np.int64)
    return np.ascontiguousarray((lock & 8) != 0)


# ───────────────────────── 매수·매도 판정 ─────────────────────────

@njit(cache=True)
def touch_step(stock, day, open_price, high, low, close, static_market, new_from, signal_level, signal_cancel, armed,
               level, cancel, signal_of, live_rule, strict):
    """대기 상태를 하루 진행하고, 그날 사면 매수가(수정주가), 아니면 NaN.

    live_rule: candidates.py — 고가 > H2·저가 ≤ H2 면 대기 끝, 시가 > H2 일 때만 산다.
    아니면 42-1 ticker_trades — 시가 > H2·저가 ≤ H2 인 날만 대기 끝·매수. strict: 저가 < H2 일 때만 체결.
    """
    value = signal_level[stock, day]

    if value == value:
        armed[stock] = True
        level[stock] = value
        cancel[stock] = signal_cancel[stock, day]
        signal_of[stock] = day - 1

    if armed[stock] and close[stock, day - 1] < cancel[stock]:
        armed[stock] = False

    if not armed[stock]:
        return np.nan

    target = level[stock]
    above_open = open_price[stock, day] > target
    reached = low[stock, day] <= target

    if live_rule:
        if not (high[stock, day] > target and reached):
            return np.nan
    elif not (above_open and reached):
        return np.nan

    armed[stock] = False

    if not above_open or (strict and not low[stock, day] < target):
        return np.nan

    step = grid.tick_size(target, static_market[stock], day >= new_from)
    return np.floor(target / step) * step


@njit(cache=True)
def exit_of(stock, entry_day, stop, close, halted, low_event, close_lock, last_valid, end_index, defer_lock):
    """(매도일, 매도가(수정), 이유). 매수 다음 날부터 종가 < 손절선. 저점이 더 높게 확정되면 손절선을 올린다."""
    last = last_valid[stock]
    pending = False

    for day in range(entry_day + 1, last + 1):
        event = low_event[stock, day]

        if event == event and event > stop:
            stop = event

        if halted[stock, day] or not close[stock, day] > 0:
            continue

        if pending or close[stock, day] < stop:
            if defer_lock and close_lock[stock, day]:
                pending = True
                continue

            return day, close[stock, day], REASON_STOP

    if last < end_index:
        return last + 1, close[stock, last], REASON_DELIST

    return end_index + 1, close[stock, last], REASON_END


@njit(cache=True)
def control_exit(stock, target_day, close, halted, close_lock, last_valid, end_index):
    """대조: 짝 보유일이 끝나는 날(또는 그 뒤 첫 가능한 날) 종가."""
    last = last_valid[stock]

    for day in range(target_day, last + 1):
        if halted[stock, day] or not close[stock, day] > 0 or close_lock[stock, day]:
            continue

        return day, close[stock, day], REASON_HOLD

    if last < end_index:
        return last + 1, close[stock, last], REASON_DELIST

    return end_index + 1, close[stock, last], REASON_END


@njit(cache=True)
def passes_filter(stock, day, year_high, above240, run20, atr_percent, year_high_min, above240_min, above240_max,
                  run20_max, atr_percent_max):
    """신호일 조건값이 문턱 안인가. 문턱이 inf 면 그 쪽은 보지 않는다. 값이 NaN 이면 탈락."""
    if not np.isinf(year_high_min) and not year_high[stock, day] >= year_high_min:
        return False

    if not np.isinf(above240_min) and not above240[stock, day] >= above240_min:
        return False

    if not np.isinf(above240_max) and not above240[stock, day] <= above240_max:
        return False

    if not np.isinf(run20_max) and not run20[stock, day] <= run20_max:
        return False

    if not np.isinf(atr_percent_max) and not atr_percent[stock, day] <= atr_percent_max:
        return False

    return True


@njit(parallel=True, cache=True)
def free_trades(open_price, high, low, close, halted, static_market, new_from, last_valid, active, signal_level,
                signal_cancel, low_event, close_lock, out, counts):
    """재현 대조용 — 42-1 ticker_trades 와 같은 조건(보유 수 제한 없음, 42-1 대기 끝 판정, 수정주가 2,000원, 잠김 미룸 없음).

    out 열: entry_day, entry_price, exit_day, exit_price, reason, signal_day, stop.
    """
    stock_count, day_count = close.shape
    end_index = day_count - 1

    for stock in prange(stock_count):
        armed = np.zeros(stock_count, dtype=np.bool_)
        level = np.zeros(stock_count)
        cancel = np.zeros(stock_count)
        signal_of = np.zeros(stock_count, dtype=np.int64)
        hold_until = -1
        filled = 0

        for day in range(1, day_count):
            if day <= hold_until or not active[stock, day]:
                continue

            price = touch_step(stock, day, open_price, high, low, close, static_market, new_from, signal_level,
                               signal_cancel, armed, level, cancel, signal_of, False, False)

            if not price >= MIN_PRICE:
                continue

            exit_day, exit_price, reason = exit_of(stock, day, cancel[stock], close, halted, low_event, close_lock,
                                                   last_valid, end_index, False)

            if filled < out.shape[1]:
                out[stock, filled, 0] = day
                out[stock, filled, 1] = price
                out[stock, filled, 2] = exit_day
                out[stock, filled, 3] = exit_price
                out[stock, filled, 4] = reason
                out[stock, filled, 5] = signal_of[stock]
                out[stock, filled, 6] = cancel[stock]
                filled += 1

            hold_until = exit_day

        counts[stock] = filled


# ───────────────────────── 슬롯 계좌 ─────────────────────────

@njit(cache=True)
def simulate(filtered, slots, amount_mode, slippage, order, seed, control, year_high_min, above240_min, above240_max,
             run20_max, atr_percent_max, strict, pair_first, pair_last, block_first, block_last,
             open_price, high, low, close, halted, raw_factor, market, static_market, tax, new_from, first_day,
             last_valid, delist_multiplier, close_lock, previous_20d, active, signal_level, signal_cancel, low_event,
             year_high, above240, run20, atr_percent, eligible_start, eligible_stock, pair_entry_day, pair_hold,
             block_keys, out_equity, scalars, log, log_capacity):
    stock_count, day_count = close.shape
    end_index = day_count - 1
    held = np.zeros(stock_count, dtype=np.bool_)
    units = np.zeros(stock_count)
    spend_of = np.zeros(stock_count)
    entry_day_of = np.zeros(stock_count, dtype=np.int64)
    entry_price_of = np.zeros(stock_count)
    exit_day_of = np.zeros(stock_count, dtype=np.int64)
    exit_price_of = np.zeros(stock_count)
    reason_of = np.zeros(stock_count, dtype=np.int64)
    signal_day_of = np.zeros(stock_count, dtype=np.int64)
    last_price = np.zeros(stock_count)
    sold_day = np.full(stock_count, -1, dtype=np.int64)
    held_list = np.empty(stock_count, dtype=np.int64)
    held_count = 0
    armed = np.zeros(stock_count, dtype=np.bool_)
    level = np.zeros(stock_count)
    cancel = np.zeros(stock_count)
    signal_of = np.zeros(stock_count, dtype=np.int64)
    candidate_stock = np.empty(stock_count, dtype=np.int64)
    candidate_key = np.empty(stock_count)
    candidate_price = np.empty(stock_count)
    candidate_exact = np.zeros(stock_count, dtype=np.bool_)
    pick_size = max(64, int(slots))
    pick_key = np.empty(pick_size)
    pick_stock = np.empty(pick_size, dtype=np.int64)
    pick_hold = np.empty(pick_size, dtype=np.int64)
    capital = slots * SLOT_AMOUNT
    cash = capital
    equity_previous = capital
    log_count = 0
    pair_pointer = pair_first

    for field in range(scalars.shape[0]):
        scalars[field] = 0.0

    for day in range(first_day, day_count):
        position = day - first_day
        new_table = day >= new_from

        for phase in range(2):
            # phase 0: 아침 폐지 현금화 / phase 1: 종가 매도(매수 뒤)
            index = 0

            while index < held_count:
                stock = held_list[index]
                is_delist = reason_of[stock] == REASON_DELIST

                if exit_day_of[stock] != day or is_delist != (phase == 0):
                    index += 1
                    continue

                price = exit_price_of[stock]

                if is_delist:
                    gross = units[stock] * price * delist_multiplier[stock]
                    slip_cost = 0.0
                else:
                    fraction = harness.slip_fraction(price, raw_factor[stock, day], market[stock, day], new_table,
                                                     slippage)
                    gross = units[stock] * price * max(1.0 - fraction, 0.0)
                    slip_cost = units[stock] * price - gross

                commission_cost = gross * COMMISSION
                tax_cost = gross * tax[day]
                proceeds = gross - commission_cost - tax_cost
                cash += proceeds
                scalars[6] += commission_cost
                scalars[7] += tax_cost
                scalars[8] += slip_cost
                scalars[1] += 1.0
                scalars[2] += proceeds / spend_of[stock] - 1.0
                scalars[4] += day - entry_day_of[stock] + 1

                if log_capacity > 0:
                    if log_count < log_capacity:
                        log[log_count, 0] = stock
                        log[log_count, 1] = entry_day_of[stock]
                        log[log_count, 2] = day
                        log[log_count, 3] = reason_of[stock]
                        log[log_count, 4] = spend_of[stock]
                        log[log_count, 5] = proceeds
                        log[log_count, 6] = entry_price_of[stock]
                        log[log_count, 7] = price
                        log[log_count, 8] = signal_day_of[stock]
                        log_count += 1
                    else:
                        scalars[9] = 1.0

                held[stock] = False
                units[stock] = 0.0
                sold_day[stock] = day
                held_count -= 1
                held_list[index] = held_list[held_count]

            if phase == 1:
                break

            # 매수 후보 모으기
            count = 0

            if control:
                while pair_pointer < pair_last and pair_entry_day[pair_pointer] < day:
                    pair_pointer += 1

                today_first = pair_pointer

                while pair_pointer < pair_last and pair_entry_day[pair_pointer] == day:
                    pair_pointer += 1

                wanted = min(pair_pointer - today_first, slots - held_count, pick_size)

                for slot in range(wanted):
                    pick_key[slot] = np.inf
                    pick_stock[slot] = -1

                for place in range(eligible_start[day], eligible_start[day + 1]):
                    stock = eligible_stock[place]

                    if held[stock] or sold_day[stock] == day:
                        continue

                    key = basket.random_key(seed, day, stock)

                    if wanted > 0 and key < pick_key[wanted - 1]:
                        slot = wanted - 1

                        while slot > 0 and pick_key[slot - 1] > key:
                            pick_key[slot] = pick_key[slot - 1]
                            pick_stock[slot] = pick_stock[slot - 1]
                            slot -= 1

                        pick_key[slot] = key
                        pick_stock[slot] = stock

                for slot in range(wanted):
                    if pick_stock[slot] >= 0:
                        candidate_stock[count] = pick_stock[slot]
                        candidate_price[count] = open_price[pick_stock[slot], day]
                        candidate_exact[count] = False
                        pick_hold[count] = pair_hold[today_first + slot]
                        count += 1
            else:
                for stock in range(stock_count):
                    if held[stock] or not active[stock, day]:
                        continue

                    price = touch_step(stock, day, open_price, high, low, close, static_market, new_from, signal_level,
                                       signal_cancel, armed, level, cancel, signal_of, True, strict)

                    if not price > 0 or not price * raw_factor[stock, day] >= MIN_PRICE:
                        continue

                    if filtered and not passes_filter(stock, signal_of[stock], year_high, above240, run20, atr_percent,
                                                      year_high_min, above240_min, above240_max, run20_max,
                                                      atr_percent_max):
                        continue

                    if block_last > block_first:
                        key_value = stock * day_count + day
                        found = np.searchsorted(block_keys[block_first:block_last], key_value)

                        if found < block_last - block_first and block_keys[block_first + found] == key_value:
                            continue

                    if order == ORDER_RANDOM:
                        key = basket.random_key(seed, day, stock)
                    else:
                        turnover = previous_20d[stock, day]
                        key = -turnover if np.isfinite(turnover) else np.inf

                    candidate_stock[count] = stock
                    candidate_key[count] = key
                    candidate_price[count] = price
                    candidate_exact[count] = low[stock, day] == level[stock]
                    count += 1

                if count > 1:
                    order_index = np.argsort(candidate_key[:count], kind="mergesort")
                    sorted_stock = candidate_stock[:count][order_index].copy()
                    sorted_price = candidate_price[:count][order_index].copy()
                    sorted_exact = candidate_exact[:count][order_index].copy()
                    candidate_stock[:count] = sorted_stock
                    candidate_price[:count] = sorted_price
                    candidate_exact[:count] = sorted_exact

            # 매수
            for place in range(count):
                if held_count >= slots:
                    scalars[12] += count - place
                    break

                stock = candidate_stock[place]
                price = candidate_price[place]

                if amount_mode == harness.AMOUNT_FIXED:
                    amount = SLOT_AMOUNT
                else:
                    amount = equity_previous / slots

                amount = min(amount, cash)
                raw_fill = price * raw_factor[stock, day]
                per_share = raw_fill * (1.0 + COMMISSION)
                shares = np.floor(amount / per_share) if per_share > 0 else 0.0

                if shares < 1.0:
                    scalars[10] += 1.0
                    continue

                spend = shares * per_share
                cash -= spend
                units[stock] = shares * raw_factor[stock, day]
                spend_of[stock] = spend
                scalars[6] += shares * raw_fill * COMMISSION
                scalars[0] += 1.0
                scalars[3] += spend

                if candidate_exact[place]:
                    scalars[13] += 1.0

                entry_day_of[stock] = day
                entry_price_of[stock] = price
                last_price[stock] = price

                if control:
                    exit_day, exit_price, reason = control_exit(stock, day + pick_hold[place], close, halted,
                                                                close_lock, last_valid, end_index)
                    signal_day_of[stock] = day
                else:
                    exit_day, exit_price, reason = exit_of(stock, day, cancel[stock], close, halted, low_event,
                                                           close_lock, last_valid, end_index, True)
                    signal_day_of[stock] = signal_of[stock]

                exit_day_of[stock] = exit_day
                exit_price_of[stock] = exit_price
                reason_of[stock] = reason
                held[stock] = True
                held_list[held_count] = stock
                held_count += 1

        # 종가 평가
        value = cash

        for index in range(held_count):
            stock = held_list[index]
            price = close[stock, day]

            if np.isfinite(price):
                last_price[stock] = price

            value += units[stock] * last_price[stock]

        out_equity[position] = value
        equity_previous = value
        scalars[5] += held_count

    # 시험 끝 보유: 마지막 종가에서 매도 비용을 뗀 값으로 매매 통계에 넣는다(평가금에는 비용 없음)
    for index in range(held_count):
        stock = held_list[index]
        gross = units[stock] * last_price[stock]
        proceeds = gross * (1.0 - COMMISSION - tax[day_count - 1])
        scalars[1] += 1.0
        scalars[2] += proceeds / spend_of[stock] - 1.0
        scalars[4] += day_count - entry_day_of[stock]

        if log_capacity > 0:
            if log_count < log_capacity:
                log[log_count, 0] = stock
                log[log_count, 1] = entry_day_of[stock]
                log[log_count, 2] = day_count - 1
                log[log_count, 3] = REASON_END
                log[log_count, 4] = spend_of[stock]
                log[log_count, 5] = proceeds
                log[log_count, 6] = entry_price_of[stock]
                log[log_count, 7] = last_price[stock]
                log[log_count, 8] = signal_day_of[stock]
                log_count += 1
            else:
                scalars[9] = 1.0

    return log_count


@njit(parallel=True, cache=True)
def run_batch(parameters, pair_first, pair_last, block_first, block_last, open_price, high, low, close, halted,
              raw_factor, market, static_market, tax, new_from, first_day, last_valid, delist_multiplier, close_lock,
              previous_20d, active, signal_level, signal_cancel, low_event, year_high, above240, run20, atr_percent,
              eligible_start, eligible_stock, pair_entry_day, pair_hold, block_keys, out_equity, out_scalars, logs,
              log_counts, log_capacity):
    """parameters 열: PARAMETER_FIELDS. 짝·막기 구간은 행마다 따로 받는다."""
    for run in prange(parameters.shape[0]):
        row = parameters[run]
        capacity = log_capacity if row[8] > 0 else 0
        log_counts[run] = simulate(row[0] > 0, int(row[1]), int(row[2]), int(row[3]), int(row[4]), int(row[5]),
                                   row[6] > 0, row[9], row[10], row[11], row[12], row[13], row[14] > 0,
                                   pair_first[run], pair_last[run], block_first[run], block_last[run],
                                   open_price, high, low, close, halted, raw_factor, market, static_market, tax,
                                   new_from, first_day, last_valid, delist_multiplier, close_lock, previous_20d,
                                   active, signal_level, signal_cancel, low_event, year_high, above240, run20,
                                   atr_percent, eligible_start, eligible_stock, pair_entry_day, pair_hold, block_keys,
                                   out_equity[run], out_scalars[run], logs[run], capacity)


# ───────────────────────── 실행 ─────────────────────────

class Account:
    """시장·신호 배열을 한 번 만들고 조합 표를 배치로 돌린다."""

    def __init__(self):
        self.market = harness.Market()
        market = self.market
        self.span = market.day_count - market.first_day
        shape = (market.stock_count, market.day_count)
        self.active = np.zeros(shape, dtype=np.bool_)
        self.signal_level = np.full(shape, np.nan)
        self.signal_cancel = np.full(shape, np.nan)
        self.low_event = np.full(shape, np.nan)
        swing_events(market.high, market.low, market.close, market.halted, market.start_index, WINDOW, self.active,
                     self.signal_level, self.signal_cancel, self.low_event)
        self.features = feature_matrices(market)
        self.close_lock = close_lock_matrix(market)
        self.pair_entry_day = np.zeros(1, dtype=np.int64)
        self.pair_hold = np.zeros(1, dtype=np.int64)
        self.pair_ranges = {}
        self.block_keys = np.zeros(1, dtype=np.int64)
        self.block_ranges = {}

    def set_pairs(self, logs: dict) -> None:
        """대조 짝: 조합 번호 → 그 전략 계좌의 (매수일, 보유 거래일 수), 매수일 순."""
        days, holds, start = [], [], 0

        for row, log in logs.items():
            ordered = log.sort_values(["entry_day", "stock"], kind="stable")
            days.append(ordered["entry_day"].to_numpy(np.int64))
            holds.append((ordered["exit_day"] - ordered["entry_day"]).to_numpy(np.int64))
            self.pair_ranges[row] = (start, start + len(ordered))
            start += len(ordered)

        self.pair_entry_day = np.concatenate(days + [np.zeros(1, dtype=np.int64)])
        self.pair_hold = np.concatenate(holds + [np.zeros(1, dtype=np.int64)])

    def set_blocks(self, blocks: dict) -> None:
        keys, start = [], 0

        for row, values in blocks.items():
            ordered = np.sort(np.asarray(values, dtype=np.int64))
            keys.append(ordered)
            self.block_ranges[row] = (start, start + len(ordered))
            start += len(ordered)

        self.block_keys = np.concatenate(keys + [np.zeros(1, dtype=np.int64)])

    def run(self, table: pd.DataFrame, logged: bool):
        market = self.market
        parameters = table[list(PARAMETER_FIELDS)].to_numpy(np.float64)
        count = len(table)
        equity = np.zeros((count, self.span))
        scalars = np.zeros((count, len(SCALAR_FIELDS)))
        capacity = LOG_CAPACITY if logged else 0
        logs = np.zeros((count, max(capacity, 1), len(LOG_FIELDS)))
        log_counts = np.zeros(count, dtype=np.int64)
        pair = np.array([self.pair_ranges.get(int(row), (0, 0)) if control else (0, 0)
                         for row, control in zip(table["pair_row"], table["control"])], dtype=np.int64).reshape(count, 2)
        block = np.array([self.block_ranges.get(int(row), (0, 0)) for row in table["block_row"]],
                         dtype=np.int64).reshape(count, 2)
        features = self.features
        run_batch(parameters, pair[:, 0].copy(), pair[:, 1].copy(), block[:, 0].copy(), block[:, 1].copy(),
                  market.open, market.high, market.low, market.close, market.halted, market.raw_factor, market.market,
                  market.static_market, market.tax, market.new_from, market.first_day, market.last_valid,
                  market.delist_multiplier, self.close_lock, market.previous_20d, self.active, self.signal_level,
                  self.signal_cancel, self.low_event, features["year_high"], features["above240"], features["run20"],
                  features["atr_percent"], market.eligible_start, market.eligible_stock, self.pair_entry_day,
                  self.pair_hold, self.block_keys, equity, scalars, logs, log_counts, capacity)
        return equity, scalars, logs, log_counts

    def run_all(self, table: pd.DataFrame, logged: bool, label: str, started: float):
        count = len(table)
        equity = np.zeros((count, self.span))
        scalars = np.zeros((count, len(SCALAR_FIELDS)))
        logs = {}
        batch = BATCH_LOGGED if logged else BATCH_PLAIN

        for begin in range(0, count, batch):
            end = min(begin + batch, count)
            part_equity, part_scalars, part_logs, part_counts = self.run(table.iloc[begin:end], logged)
            equity[begin:end], scalars[begin:end] = part_equity, part_scalars

            if logged:
                for offset in range(end - begin):
                    log = pd.DataFrame(part_logs[offset, :part_counts[offset]], columns=LOG_FIELDS)

                    for column in ("stock", "entry_day", "exit_day", "reason", "signal_day"):
                        log[column] = log[column].astype(np.int64)

                    log["net_return"] = log["proceeds"] / log["spend"] - 1.0
                    logs[begin + offset] = log

            print(f"  {label} {end}/{count} — {time.time() - started:.0f}초", flush=True)

        return equity, scalars, logs


def base_row(**values) -> dict:
    row = {"filtered": 1, "slots": 100, "amount_mode": 0, "slippage": 1, "order": ORDER_TURNOVER, "seed": 0,
           "control": 0, "pair_row": -1, "log": 0, **CENTER, "strict_touch": 0, "block_row": -1}
    row.update(values)
    return row


def main_table() -> pd.DataFrame:
    rows = [base_row(filtered=filtered, slots=slots, amount_mode=amount_mode, slippage=slippage, log=1)
            for filtered in FILTER_CHOICES for slots in SLOT_CHOICES for amount_mode in AMOUNT_CHOICES
            for slippage in SLIPPAGE_CHOICES]
    return pd.DataFrame(rows)


def describe(table: pd.DataFrame) -> pd.DataFrame:
    return pd.DataFrame({
        "규칙": table["filtered"].map(FILTER_TEXT).to_numpy(),
        "슬롯": table["slots"].to_numpy(),
        "금액": table["amount_mode"].map(harness.AMOUNT_TEXT).to_numpy(),
        "미끄러짐 호가": table["slippage"].to_numpy(),
        "순서": np.where(table["order"].to_numpy() == ORDER_RANDOM, "무작위", "거래대금")})


def threshold_text(row) -> str:
    def shown(value):
        return "없음" if np.isinf(value) else f"{value:g}"

    return (f"1년고점≥{shown(row['year_high_min'])} · 240일선 {shown(row['above240_min'])}–{shown(row['above240_max'])}"
            f" · 20일상승≤{shown(row['run20_max'])} · ATR≤{shown(row['atr_percent_max'])}")


def medians_of(logs: dict, count: int) -> np.ndarray:
    return np.array([logs[row]["net_return"].median() * 100.0 if row in logs and len(logs[row]) else np.nan
                     for row in range(count)])


def reproduction(account: Account) -> pd.DataFrame:
    """42-1 trades_w20_retest.parquet 와 같은 규칙(보유 수 제한 없음)으로 다시 내 매수·매도가 같은지 본다."""
    market = account.market
    out = np.zeros((market.stock_count, 400, 7))
    counts = np.zeros(market.stock_count, dtype=np.int64)
    free_trades(market.open, market.high, market.low, market.close, market.halted, market.static_market,
                market.new_from, market.last_valid, account.active, account.signal_level, account.signal_cancel,
                account.low_event, account.close_lock, out, counts)
    rows = np.concatenate([out[stock, :counts[stock]] for stock in range(market.stock_count) if counts[stock]])
    mine = pd.DataFrame(rows, columns=["entry_t", "entry_price", "exit_t", "exit_price", "reason", "signal_t", "stop"])
    mine.insert(0, "ticker", np.repeat(market.panel_rows, counts))
    for column in ("ticker", "entry_t", "exit_t", "reason", "signal_t"):
        mine[column] = mine[column].astype(np.int64)

    features = account.features
    stock_of_row = pd.Series(np.arange(market.stock_count), index=market.panel_rows)
    stock = stock_of_row[mine["ticker"]].to_numpy()
    signal = mine["signal_t"].to_numpy()
    mine["passed"] = ((features["year_high"][stock, signal] >= 0.809)
                      & (features["above240"][stock, signal] >= 0.0169) & (features["above240"][stock, signal] <= 0.32)
                      & (features["run20"][stock, signal] <= 0.264) & (features["atr_percent"][stock, signal] <= 5.9))
    original = pd.read_parquet(DATA / "trades_w20_retest.parquet")
    original = original[original["ticker"].isin(market.panel_rows)]
    rows_out = []

    for label, mine_part, original_part in (
            ("거름 없음", mine, original),
            ("거름 4개", mine[mine["passed"]], original[(original["year_high"] >= 0.809)
                                                    & original["above240"].between(0.0169, 0.32)
                                                    & (original["run20"] <= 0.264)
                                                    & (original["atr_percent"] <= 5.9)])):
        merged = original_part.merge(mine_part, on=["ticker", "entry_t"], how="outer", suffixes=("_42", "_mine"),
                                     indicator=True)
        both = merged[merged["_merge"] == "both"]
        mine_only = merged[merged["_merge"] == "right_only"]
        rows_out.append({
            "대조": label,
            "42-1 건수(41 종목 안, 자료 오류 포함)": len(original_part),
            "다시 낸 건수": len(mine_part),
            "둘 다 있음": len(both),
            "42-1에만": int((merged["_merge"] == "left_only").sum()),
            "다시 낸 쪽에만": len(mine_only),
            "다시 낸 쪽에만 중 상장폐지 보유": int((mine_only["reason"] == REASON_DELIST).sum()),
            "매수가 일치율 %": float(np.isclose(both["entry_price_42"], both["entry_price_mine"], rtol=1e-6).mean() * 100),
            "매도일 일치율 %": float((both["exit_t_42"] == both["exit_t_mine"]).mean() * 100),
            "다시 낸 쪽 상장폐지 보유 건": int((mine_part["reason"] == REASON_DELIST).sum())})

    return pd.DataFrame(rows_out)


def to_csv(frame: pd.DataFrame, name: str) -> None:
    frame.to_csv(RESULTS / name, index=False, encoding="utf-8-sig", lineterminator="\n")


def main() -> None:
    started = time.time()
    RESULTS.mkdir(exist_ok=True)
    (DATA / "account_trades").mkdir(parents=True, exist_ok=True)
    account = Account()
    market = account.market
    runner = run_stages.Runner(market)
    built = time.time() - started
    print(f"시장 자료·신호 {built:.0f}초, 추세 확인 {int(np.isfinite(account.signal_level).sum()):,}건", flush=True)

    reproduced = reproduction(account)
    to_csv(reproduced, "reproduction.csv")
    print(reproduced.to_string(), flush=True)

    # 1) 순서 (i) 48조합
    table = main_table()
    count = len(table)
    equity, scalars, logs = account.run_all(table, True, "순서 (i)", started)
    medians = medians_of(logs, count)
    strategy, daily, yearly = run_stages.summary_rows(runner, table, equity, None, scalars, medians)
    strategy["후보 놓침(빈 슬롯 없음)"] = scalars[:, SCALAR["missed_no_slot"]]
    strategy["딱 닿아 산 건 %"] = scalars[:, SCALAR["exact_touch_buys"]] / np.maximum(scalars[:, SCALAR["buys"]], 1) * 100

    for row, log in logs.items():
        if table["slippage"].iloc[row] == 1:
            log.assign(code=market.codes[log["stock"].to_numpy()],
                       entry_date=market.dates[log["entry_day"].to_numpy()],
                       exit_date=market.dates[np.minimum(log["exit_day"].to_numpy(), market.day_count - 1)]
                       ).to_parquet(DATA / "account_trades" / f"combination_{row:02d}.parquet", index=False)

    # 2) 상위 1% 뺀 계좌 48조합
    blocks = {}

    for row, log in logs.items():
        top_count = math.ceil(len(log) * TOP_SHARE)
        top = log.nlargest(top_count, "net_return")
        blocks[row] = (top["stock"].to_numpy(np.int64) * market.day_count + top["entry_day"].to_numpy(np.int64))

    account.set_blocks(blocks)
    top_table = table.assign(block_row=np.arange(count))
    top_equity, top_scalars, top_logs = account.run_all(top_table, True, "상위 1% 뺀 계좌", started)
    top_summary, top_daily, _ = run_stages.summary_rows(runner, top_table, top_equity, None, top_scalars,
                                                        medians_of(top_logs, count))

    # 3) 이웃 문턱 243조합 + 닿기 엄격 1
    grid_rows = []

    for values in np.array(np.meshgrid(*[NEIGHBORS[name] for name in THRESHOLD_NAMES], indexing="ij")).reshape(
            len(THRESHOLD_NAMES), -1).T:
        grid_rows.append(base_row(**REPRESENTATIVE, **dict(zip(THRESHOLD_NAMES, values)), log=1))

    grid_rows.append(base_row(**REPRESENTATIVE, strict_touch=1, log=1))
    neighbor_table = pd.DataFrame(grid_rows)
    neighbor_equity, neighbor_scalars, neighbor_logs = account.run_all(neighbor_table, True, "이웃 문턱·닿기 엄격",
                                                                       started)
    neighbor_summary, neighbor_daily, _ = run_stages.summary_rows(runner, neighbor_table, neighbor_equity, None,
                                                                  neighbor_scalars,
                                                                  medians_of(neighbor_logs, len(neighbor_table)))
    del neighbor_logs, top_logs

    # 4) 순서 (ii) 무작위 순서 960회
    random_table = pd.concat([table.assign(order=ORDER_RANDOM, seed=seed, log=0, base_row=np.arange(count))
                              for seed in study41.RANDOM_SEEDS], ignore_index=True)
    random_equity, random_scalars, _ = account.run_all(random_table, False, "순서 (ii)", started)
    random_summary, random_daily, _ = run_stages.summary_rows(runner, random_table, random_equity, None,
                                                              random_scalars, np.full(len(random_table), np.nan))

    # 5) 무작위 종목 대조 960회(같은 날 같은 건수, 짝과 같은 보유일)
    account.set_pairs(logs)
    control_table = pd.concat([table.assign(control=1, seed=seed, log=0, pair_row=np.arange(count))
                               for seed in CONTROL_SEEDS], ignore_index=True)
    control_equity, control_scalars, _ = account.run_all(control_table, False, "대조", started)
    control_daily = run_stages.daily_returns(control_equity, control_table["slots"].to_numpy(np.float64) * SLOT_AMOUNT)
    control_annual = np.array([study41.curve_statistics(values)[0] for values in control_daily])
    control_frame = pd.concat([describe(table).iloc[control_table["pair_row"]].reset_index(drop=True),
                               pd.DataFrame({"조합 번호": control_table["pair_row"], "대조 시드": control_table["seed"],
                                             "대조 연 수익 %": control_annual,
                                             "대조 매수 수": control_scalars[:, SCALAR["buys"]],
                                             "전략 매수 수": scalars[control_table["pair_row"], SCALAR["buys"]],
                                             **{f"대조 {name} 대비 연 초과 %p":
                                                study41.annual_mean(control_daily - returns[None, :]) * 100.0
                                                for name, returns in runner.index_returns.items()}})], axis=1)
    to_csv(control_frame, "controls.csv")
    pair_annual = control_annual.reshape(len(CONTROL_SEEDS), count)
    strategy["무작위 대조 이긴 횟수"] = (strategy["연 수익 %"].to_numpy()[None, :] > pair_annual).sum(axis=0)
    strategy["무작위 대조 연 수익 중앙값 %"] = np.median(pair_annual, axis=0)
    strategy["대조 매수 수 ÷ 전략 매수 수"] = np.median(
        control_scalars[:, SCALAR["buys"]].reshape(len(CONTROL_SEEDS), count), axis=0) / np.maximum(
        scalars[:, SCALAR["buys"]], 1)

    # 6) t 값과 묶음 통과선: 순서 (i) 48 + 상위 1% 뺀 48 + 이웃 243 + 닿기 엄격 1 = 340조합 × 지수 2
    names = list(runner.index_returns)
    sign_pair = study41.signs(runner.calendar)
    family_daily = np.concatenate([daily, top_daily, neighbor_daily])
    family_count = family_daily.shape[0]
    excess = np.concatenate([family_daily - runner.index_returns[name][None, :] for name in names])
    t_month, t_year, flip_month, flip_year = study41.t_values(excess, runner.calendar, sign_pair)
    main_mask = np.tile(np.arange(family_count) < count, len(names))
    bars = {f"순서 (i)만({count * 2}계열)": study41.family_bar(flip_month[main_mask], flip_year[main_mask]),
            f"이번 시험 전체({family_count * 2}계열)": study41.family_bar(flip_month, flip_year)}
    to_csv(pd.DataFrame([{"묶음": label, **values} for label, values in bars.items()]), "family_bars.csv")
    bar_all = list(bars.values())[1]["family_bar"]
    bar_main = list(bars.values())[0]["family_bar"]
    t_columns = [f"{name} {kind} t" for name in names for kind in ("월", "연")]
    frames = {"main": strategy, "top": top_summary, "neighbor": neighbor_summary}
    offsets = {"main": 0, "top": count, "neighbor": 2 * count}

    for key, frame in frames.items():
        for number, name in enumerate(names):
            part = slice(number * family_count + offsets[key], number * family_count + offsets[key] + len(frame))
            frame[f"{name} 월 t"] = t_month[part]
            frame[f"{name} 연 t"] = t_year[part]

        frame["가장 낮은 t"] = frame[t_columns].min(axis=1)

    strategy["t 통과(전체 통과선)"] = strategy["가장 낮은 t"] >= bar_all
    strategy["통과"] = strategy["t 통과(전체 통과선)"] & (strategy["무작위 대조 이긴 횟수"] >= CONTROL_WIN_NEED)
    strategy["통과(순서 (i) 통과선)"] = ((strategy["가장 낮은 t"] >= bar_main)
                                   & (strategy["무작위 대조 이긴 횟수"] >= CONTROL_WIN_NEED))
    full = pd.concat([describe(table), strategy], axis=1)
    full.insert(0, "조합 번호", np.arange(count))
    to_csv(full, "main_table.csv")
    top_out = pd.concat([describe(top_table), top_summary], axis=1)
    top_out.insert(0, "조합 번호", np.arange(count))
    top_out["막은 매매 수"] = [len(blocks[row]) for row in range(count)]
    top_out["원래 연 수익 %"] = strategy["연 수익 %"].to_numpy()
    to_csv(top_out, "top1_removed.csv")
    neighbor_out = pd.concat([neighbor_table[list(THRESHOLD_NAMES) + ["strict_touch"]].reset_index(drop=True),
                              neighbor_summary], axis=1)
    neighbor_out.insert(0, "문턱", [threshold_text(row) for _, row in neighbor_table.iterrows()])
    neighbor_out.insert(0, "이웃 칸 수", [sum(value != CENTER[name] for name, value in zip(THRESHOLD_NAMES, values))
                                       for values in neighbor_table[list(THRESHOLD_NAMES)].to_numpy()])
    to_csv(neighbor_out, "neighbors.csv")

    # 순서 (ii) — 같은 통과선으로 시드마다
    random_months, random_years, _, _ = study41.t_values(
        np.concatenate([random_daily - runner.index_returns[name][None, :] for name in names]), runner.calendar)
    random_out = pd.concat([describe(random_table), random_table[["seed", "base_row"]].reset_index(drop=True)
                            .rename(columns={"seed": "순서 시드", "base_row": "조합 번호"}), random_summary], axis=1)

    for number, name in enumerate(names):
        part = slice(number * len(random_table), (number + 1) * len(random_table))
        random_out[f"{name} 월 t"] = random_months[part]
        random_out[f"{name} 연 t"] = random_years[part]

    random_out["가장 낮은 t"] = random_out[t_columns].min(axis=1)
    random_out["t 통과(전체 통과선)"] = random_out["가장 낮은 t"] >= bar_all
    to_csv(random_out, "order_random.csv")

    # 해마다 표
    index_yearly = {name: runner.yearly_compound(returns[None, :])[0] for name, returns in runner.index_returns.items()}
    year_rows = []

    for row in range(count):
        for number, year in enumerate(runner.years):
            year_rows.append({"조합 번호": row, "해": int(year), "전략 %": yearly[row, number],
                              **{f"{name} %": index_yearly[name][number] for name in names},
                              **{f"{name} 대비 초과 %p": yearly[row, number] - index_yearly[name][number]
                                 for name in names}})

    to_csv(pd.DataFrame(year_rows), "years.csv")
    index_rows = []

    for name, returns in runner.index_returns.items():
        annual, drawdown, sharpe = study41.curve_statistics(returns)
        index_rows.append({"지수": name, "연 수익 %": annual, "MDD %": drawdown, "샤프": sharpe})

    to_csv(pd.DataFrame(index_rows), "index_reference.csv")
    curves = pd.DataFrame(equity.T, columns=[f"combination_{row:02d}" for row in range(count)])
    curves.insert(0, "date", market.input_dates)
    curves.to_parquet(DATA / "account_curves.parquet", index=False)

    # 판정 요약
    judge = []

    for (rule, ), group in full.groupby(["규칙"], sort=False):
        judge.append({"규칙": rule, "조합 수": len(group), "통과(전체 통과선)": int(group["통과"].sum()),
                      "통과(순서 (i) 통과선)": int(group["통과(순서 (i) 통과선)"].sum()),
                      "t만 통과(전체)": int(group["t 통과(전체 통과선)"].sum()),
                      "대조 19승 이상": int((group["무작위 대조 이긴 횟수"] >= CONTROL_WIN_NEED).sum()),
                      "가장 높은 '가장 낮은 t'": group["가장 낮은 t"].max(),
                      "가장 높은 코스피 대비 연 초과 %p": group["코스피 대비 연 초과 %p"].max(),
                      "통과선 전체": bar_all, "통과선 순서 (i)": bar_main})

    judge_frame = pd.DataFrame(judge)
    to_csv(judge_frame, "judge.csv")
    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                            cwd=harness.REPOSITORY).stdout.strip()
    run_info = {"commit": commit,
                "inputs_info": json.loads((study41.CACHE / "inputs_info.json").read_text(encoding="utf-8")),
                "panel_sha256": hashlib.sha256(grid.PANEL_PATH.read_bytes()).hexdigest(),
                "period": [str(market.input_dates[0].date()), str(market.input_dates[-1].date())],
                "stock_count": market.stock_count, "window": WINDOW, "center": CENTER,
                "neighbors": {name: [None if np.isinf(value) else value for value in values]
                              for name, values in NEIGHBORS.items()},
                "order_seeds": list(study41.RANDOM_SEEDS), "control_seeds": list(CONTROL_SEEDS),
                "flip_seed": study41.SEED, "flips": study41.FLIPS, "family_bar_all": bar_all,
                "family_bar_main": bar_main, "control_win_need": CONTROL_WIN_NEED,
                "log_overflow": int(scalars[:, SCALAR["log_overflow"]].sum()),
                "seconds": {"build": built, "total": time.time() - started}}
    (RESULTS / "run_info.json").write_bytes(json.dumps(run_info, ensure_ascii=False, indent=1, default=str)
                                            .encode("utf-8"))
    pd.set_option("display.width", 250)
    pd.set_option("display.max_columns", 40)
    print(judge_frame.to_string())
    print(f"전체 {time.time() - started:.0f}초")


if __name__ == "__main__":
    main()
