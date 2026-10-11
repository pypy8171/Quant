"""스터디 39 — 실계좌 DeviationScale 진입을 일봉으로 근사하고, 같은 진입에 청산 63 칸(고정 % 격자·존 이탈·E3·E4j·보유 일수)을 붙여 비교한다.

질문: 실계좌 설정(Quant/config/config_live.json strategies[0])의 고정 % 청산(익절 평단 +5%·손절 6.5%·존 이탈)이
      추세 기준 청산(E3 스윙 추세 하락·E4j 종가 SMA20 아래 j 일)보다 나쁜가.
사전 등록: results/choices_log.csv(실행 전 31 행). 이 스크립트는 그 목록대로 한 번에 돈다.

자료: 스터디 38 Panel(PYQuant/data/bars_all_pit_v2.parquet 보통주, 상장폐지 포함) + round3 무수정 종가 보정
      + round11 정지 점프 고침·폐지 배수. 코스피 일봉은 스터디 38 data/kospi_daily_naver.parquet. 새로 받은 자료 없음.
미래 정보 차단(구조):
  - 진입 판정은 전날 종가까지 확정된 값(전날 SMA·ATR·거래대금·순위)과 그날 시가만 쓴다.
  - 접은 SMA(f_N(p) = (앞 N−1 개 종가 합 + p) / N)는 앞 행 종가만 합한다.
  - 청산 커널은 진입 행부터 앞으로만 읽는다. 장중 순서를 모르는 날은 아래(손절·존 하단) 먼저.
  - 대조는 그날 거래된 종목 전부(전날 20일 평균 거래대금 구간)를 같은 방식으로 사서 매도일 종가에 판다.
실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_exit_test.py
"""
from __future__ import annotations

import glob
import hashlib
import json
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY.parent / "38_trend_support_realtime"))

import common     # noqa: E402  numba 보다 먼저: NUMBA_NUM_THREADS 를 넣는다
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402

import round3_test     # noqa: E402
import round4_test     # noqa: E402
import round6_test     # noqa: E402
import round11_test     # noqa: E402

OUTPUT = STUDY / "results"
REPOSITORY = common.REPOSITORY
KOSPI = STUDY.parent / "38_trend_support_realtime" / "data" / "kospi_daily_naver.parquet"
CONFIG_LIVE = REPOSITORY / "Quant" / "config" / "config_live.json"

# ── config 값(실계좌 strategies[0], 2026-10-11 읽음) ─────────────────────────
PULLBACK = 0.08            # pullback_pct 8.0
ENTRY_UPPER = 0.05         # entry_upper_pct 5.0 (max_dev_pct 0.05 와 같음)
HOLD_WIDEN = 0.04          # 보유 중 존 폭 넓힘 4%p(엔진 상수) → −12% – +9%
OPEN_DEV_MIN = -0.01       # entry_open_dev_min_pct −1.0
ATR_MAX = 0.045            # entry_atr_max_pct 4.5
MIN_PRICE, MAX_PRICE = 2000.0, 100000.0
MIN_TURNOVER = 1e9         # min_turnover
TOP_N = 200                # turnover_top_n
RISK_OFF = -0.01           # risk_off_index_pct −1.0
AMOUNT = 500_000.0         # notional_floor = cap
RATIO_CAP = 0.005          # 주문 ÷ 전날 20일 평균 거래대금 상한(스터디 38)
LIVE_TP, LIVE_SL = 5.0, 6.5
SIGNAL_FIRST, SIGNAL_LAST = 20100104, 20261008

# ── 청산 칸(사전 등록) ──────────────────────────────────────────────────────
TP_GRID = (3.0, 5.0, 8.0, 12.0, 20.0, None)
SL_GRID = (3.0, 5.0, 6.5, 8.0, 10.0, 15.0, None)
E4_DAYS = tuple(range(1, 11))
HOLD_DAYS = (20, 60)
HOLD_EDGES = (0, 6, 21, 61)
HOLD_LABELS = ("0–5", "6–20", "21–60", "61+")
LIVE_RULE = "(a) 라이브: 익절5·손절6.5·존 이탈"
BAR_FLOOR = 3.5
JUDGE_POSITIVE_YEARS = 0.7
KIND_PRICE, KIND_ZONE, KIND_E3, KIND_E4, KIND_HOLD = 0, 1, 2, 3, 4
REASON_TEXT = {0: "규칙(종가)", 1: "익절", 2: "손절", 3: "존 하단", 4: "존 정배열 깨짐", 5: "존 상단", 8: "미청산(10-08 종가)",
               9: "상장폐지(배수)"}


def format_number(value) -> str:
    return "없음" if value is None else (f"{value:g}")


def build_rules() -> pd.DataFrame:
    rows = []

    for tp in TP_GRID:
        for sl in SL_GRID:
            if tp is None and sl is None:
                continue

            name = f"가격 익절{format_number(tp)}·손절{format_number(sl)}"

            if tp == LIVE_TP and sl == LIVE_SL:
                name += " (a')"
            elif tp is None and sl == LIVE_SL:
                name += " (b)"

            rows.append({"rule": name, "group": "가격 격자", "kind": KIND_PRICE, "tp": np.nan if tp is None else tp,
                         "sl": np.nan if sl is None else sl, "param": 0})

    rows.append({"rule": LIVE_RULE, "group": "존 이탈 포함", "kind": KIND_ZONE, "tp": LIVE_TP, "sl": LIVE_SL, "param": 0})
    rows.append({"rule": "(b') 손절6.5·존 이탈", "group": "존 이탈 포함", "kind": KIND_ZONE, "tp": np.nan, "sl": LIVE_SL, "param": 0})

    for index, span in enumerate(common.K_VALUES):
        rows.append({"rule": f"(c) E3 k={span}", "group": "E3", "kind": KIND_E3, "tp": np.nan, "sl": np.nan, "param": index})

    for days in E4_DAYS:
        rows.append({"rule": f"(d) E4j{days}", "group": "E4j", "kind": KIND_E4, "tp": np.nan, "sl": np.nan, "param": days})

    for days in HOLD_DAYS:
        rows.append({"rule": f"(e) 보유 {days}일", "group": "보유 일수", "kind": KIND_HOLD, "tp": np.nan, "sl": np.nan, "param": days})

    frame = pd.DataFrame(rows)
    frame["cell"] = np.arange(len(frame))
    return frame


def neighbours(rules: pd.DataFrame) -> list[list[int]]:
    """옆 칸: 가격 격자는 같은 손절의 앞뒤 익절·같은 익절의 앞뒤 손절. 존 판은 같은 익절·손절의 존 없는 칸.
    E3 는 앞뒤 k, E4j 는 앞뒤 j, 보유는 다른 보유."""
    tp_keys = [np.nan if value is None else value for value in TP_GRID]
    sl_keys = [np.nan if value is None else value for value in SL_GRID]

    def same(left, right):
        return (np.isnan(left) and np.isnan(right)) or left == right

    def find(kind, tp, sl, parameter=None):
        for row in rules.itertuples():
            if row.kind == kind and same(row.tp, tp) and same(row.sl, sl) and (parameter is None or row.param == parameter):
                return row.cell

        return None

    result = []

    for row in rules.itertuples():
        found = []

        if row.kind == KIND_PRICE:
            tp_index = next(position for position, value in enumerate(tp_keys) if same(value, row.tp))
            sl_index = next(position for position, value in enumerate(sl_keys) if same(value, row.sl))

            for step in (-1, 1):
                if 0 <= tp_index + step < len(tp_keys):
                    found.append(find(KIND_PRICE, tp_keys[tp_index + step], row.sl))

                if 0 <= sl_index + step < len(sl_keys):
                    found.append(find(KIND_PRICE, row.tp, sl_keys[sl_index + step]))
        elif row.kind == KIND_ZONE:
            found.append(find(KIND_PRICE, row.tp, row.sl))
        elif row.kind in (KIND_E3, KIND_E4):
            for step in (-1, 1):
                found.append(find(row.kind, np.nan, np.nan, row.param + step))
        else:
            found.extend(other.cell for other in rules.itertuples() if other.kind == KIND_HOLD and other.cell != row.cell)

        result.append([cell for cell in found if cell is not None])

    return result


# ── 지표 ────────────────────────────────────────────────────────────────────

def ticks(raw: np.ndarray, dates: np.ndarray) -> np.ndarray:
    safe = np.nan_to_num(raw, nan=1.0)
    return np.where(dates >= round11_test.NEW_TICK_FROM, round11_test.tick_of(safe, round11_test.NEW_TICK_FROM),
                    round11_test.tick_of(safe, 0))


def previous_sum(close: np.ndarray, starts: np.ndarray, count: int) -> np.ndarray:
    """앞 count 개 행 종가 합(그날 제외)."""
    return common.rolling_mean_known(close, starts, count, False) * count


def turnover_rank(panel, day_turnover: np.ndarray) -> np.ndarray:
    """그날 거래된 종목 안 하루 거래대금 순위(1 = 가장 큼)."""
    frame = pd.DataFrame({"position": panel.market_position, "value": np.nan_to_num(day_turnover, nan=-1.0)})
    return frame.groupby("position")["value"].rank(ascending=False, method="first").to_numpy()


class Data:
    """준비 단계 결과."""

    def __init__(self):
        started = time.time()
        panel = common.Panel()
        self.correction = round3_test.apply_corrections(panel)
        panel.build_deciles()
        close, jumps = round11_test.clean_close(panel, panel.shares_issued)
        self.jump_count = len(jumps)
        delist = round11_test.delisting_table(panel, close)
        self.delist = delist
        starts = panel.starts
        multiplier = close / panel.close
        self.panel = panel
        self.open = panel.open_price * multiplier
        self.high = panel.high * multiplier
        self.low = panel.low * multiplier
        self.close = close
        unadjust = panel.raw_close / panel.close        # 수정 → 무수정(그날 행)
        self.unadjust_clean = unadjust / multiplier      # 정제 가격 → 무수정
        self.raw_open = panel.open_price * unadjust
        self.raw_close = panel.raw_close
        self.sma = {window: common.rolling_mean_known(close, starts, window, True) for window in (5, 10, 20, 60)}
        self.before = {window: previous_sum(close, starts, window - 1) for window in (5, 10, 20, 60)}
        moving_averages = self.sma

        with np.errstate(invalid="ignore"):
            aligned = (moving_averages[5] > moving_averages[10]) & (moving_averages[10] > moving_averages[20]) & (moving_averages[20] > moving_averages[60])

        self.aligned_close = aligned
        self.aligned_previous = np.r_[False, aligned[:-1]]
        self.aligned_previous[starts[:-1]] = False
        self.folded_open_aligned, self.open_folded_sma20 = self.folded(self.open)
        self.sma20_previous = round4_test.shift_within_stock(moving_averages[20], starts)
        previous_close = round4_test.shift_within_stock(close, starts)
        true_range = (np.maximum(self.high, np.where(np.isfinite(previous_close), previous_close, -np.inf))
                      - np.minimum(self.low, np.where(np.isfinite(previous_close), previous_close, np.inf)))
        self.atr14 = common.rolling_mean_known(true_range, starts, 14, False)
        rank = turnover_rank(panel, panel.day_turnover)
        position = panel.market_position
        previous_row_ok = np.r_[False, position[1:] == position[:-1] + 1]
        previous_row_ok[starts[:-1]] = False
        self.previous_traded = previous_row_ok
        self.previous_day_turnover = np.where(previous_row_ok, round4_test.shift_within_stock(panel.day_turnover, starts), np.nan)
        self.previous_rank = np.where(previous_row_ok, round4_test.shift_within_stock(rank, starts), np.nan)
        self.previous_20d = round4_test.shift_within_stock(panel.turnover20, starts)
        self.bin = np.where(np.isfinite(self.previous_20d),
                            np.searchsorted(round6_test.ABSOLUTE_EDGES, np.nan_to_num(self.previous_20d), side="right"), -1)
        kospi = pd.read_parquet(KOSPI).sort_values("date")
        kospi_gap = kospi["open"].to_numpy() / kospi["close"].shift(1).to_numpy() - 1.0
        gap_by_date = dict(zip(kospi["date"].to_numpy(np.int64), kospi_gap))
        self.index_gap = np.array([gap_by_date.get(int(day), np.nan) for day in panel.market_dates])[position]
        self.index_missing_days = int(sum(1 for day in panel.market_dates
                                          if SIGNAL_FIRST <= day <= SIGNAL_LAST and not np.isfinite(gap_by_date.get(int(day), np.nan))))
        self.entry_tick = ticks(self.raw_open, panel.dates)
        self.close_tick = ticks(self.raw_close, panel.dates)
        self.stock_end = np.repeat(starts[1:], np.diff(starts)).astype(np.int64)
        self.listed_row = panel.listed_at_end[panel.stock_of_row]
        delist_multiplier = np.ones(panel.stock_count)
        delist_multiplier[delist["stock"].to_numpy()] = delist["multiplier"].to_numpy()
        self.delist_multiplier_stock = delist_multiplier
        self.delist_multiplier_row = delist_multiplier[panel.stock_of_row]
        states = []

        for span in common.K_VALUES:
            state, _, _ = panel.trend(span)
            states.append(state)

        self.states = np.ascontiguousarray(np.stack(states))
        self.prepare_seconds = time.time() - started

    def folded(self, price: np.ndarray):
        """price 를 그날 값으로 접은 SMA 정배열과 접은 SMA20."""
        folded = {window: (self.before[window] + price) / window for window in (5, 10, 20, 60)}

        with np.errstate(invalid="ignore"):
            aligned = (folded[5] > folded[10]) & (folded[10] > folded[20]) & (folded[20] > folded[60])

        return aligned, folded[20]

    def filters(self) -> dict:
        """진입 조건(행마다). 이름 → bool 배열. 순서가 보고 순서."""
        panel = self.panel

        with np.errstate(invalid="ignore", divide="ignore"):
            deviation = self.open / self.open_folded_sma20 - 1.0
            open_deviation = (self.open - self.sma20_previous) / self.sma20_previous
            atr_ratio = self.atr14 / self.sma20_previous
            liquidity = AMOUNT / self.previous_20d
            result = {
                "signal_period": (panel.dates >= SIGNAL_FIRST) & (panel.dates <= SIGNAL_LAST),
                "aligned_previous_close": self.aligned_previous,
                "aligned_folded_open": self.folded_open_aligned,
                "zone_dev_-8_+5": (deviation >= -PULLBACK) & (deviation <= ENTRY_UPPER),
                "open_dev_ge_-1": open_deviation >= OPEN_DEV_MIN,
                "atr14_le_4.5": atr_ratio <= ATR_MAX,
                "price_2000_100000": (self.raw_open >= MIN_PRICE) & (self.raw_open <= MAX_PRICE),
                "previous_turnover_ge_10억": self.previous_day_turnover >= MIN_TURNOVER,
                "previous_rank_le_200": self.previous_rank <= TOP_N,
                "kospi_gap_gt_-1": ~(self.index_gap <= RISK_OFF),
                "order_le_0.5pct_of_20d": liquidity <= RATIO_CAP,
            }

        self.deviation = deviation
        self.open_deviation = open_deviation
        self.atr_ratio = atr_ratio
        return result


# ── 청산 커널 ───────────────────────────────────────────────────────────────

@njit(cache=True)
def exit_rule(kind, tp, sl, parameter, entry, end, close_mode, fill, open_price, high, low, close, before19, aligned_close,
              aligned_previous, folded_open, sma20, states, position, listed, delist_multiplier):
    """한 진입·한 규칙의 (매도 행, 매도 가격(정제 수정가), 사유)."""
    tp_level = fill * (1.0 + tp / 100.0) if tp == tp else np.inf
    sl_level = fill * (1.0 - sl / 100.0) if sl == sl else -np.inf
    lower_band = (1.0 - PULLBACK - HOLD_WIDEN)
    upper_band = (1.0 + ENTRY_UPPER + HOLD_WIDEN)
    first = entry + 1 if close_mode else entry
    run = 0

    if close_mode and close[entry] < sma20[entry]:
        run = 1

    for row in range(first, end):
        if kind <= 1:
            lower = sl_level
            upper = tp_level
            lower_reason = 2
            upper_reason = 1

            if kind == 1:
                zone_low = lower_band * before19[row] / (20.0 - lower_band)
                zone_high = upper_band * before19[row] / (20.0 - upper_band)

                if zone_low > lower:
                    lower = zone_low
                    lower_reason = 3

                if zone_high < upper:
                    upper = zone_high
                    upper_reason = 5

            if row > entry:
                if kind == 1 and not aligned_previous[row] and not folded_open[row]:
                    return row, open_price[row], 14

                if open_price[row] <= lower:
                    return row, open_price[row], 10 + lower_reason

                if open_price[row] >= upper:
                    return row, open_price[row], 10 + upper_reason

            if low[row] <= lower:
                return row, min(lower, open_price[row]), lower_reason

            if high[row] >= upper:
                return row, max(upper, open_price[row]), upper_reason

            if kind == 1 and not aligned_previous[row] and not aligned_close[row]:
                return row, close[row], 4
        elif kind == 2:
            if states[parameter, row] == -1:
                return row, close[row], 0
        elif kind == 3:
            if close[row] < sma20[row]:
                run += 1
            else:
                run = 0

            if run >= parameter:
                return row, close[row], 0
        else:
            if position[row] >= position[entry] + parameter:
                return row, close[row], 0

    last = end - 1

    if listed:
        return last, close[last], 8

    return last, close[last] * delist_multiplier, 9


@njit(parallel=True, cache=True)
def run_exits(entries, close_mode, fills, ends, listed, delist_multiplier, kinds, tps, sls, parameters, open_price, high, low, close,
              before19, aligned_close, aligned_previous, folded_open, sma20, states, position):
    count = entries.shape[0]
    rules = kinds.shape[0]
    exit_row = np.empty((count, rules), dtype=np.int64)
    exit_price = np.empty((count, rules))
    reason = np.empty((count, rules), dtype=np.int8)

    for index in prange(count):
        for rule in range(rules):
            row, price, code = exit_rule(kinds[rule], tps[rule], sls[rule], parameters[rule], entries[index], ends[index], close_mode,
                                         fills[index], open_price, high, low, close, before19, aligned_close, aligned_previous,
                                         folded_open, sma20, states, position, listed[index], delist_multiplier[index])
            exit_row[index, rule] = row
            exit_price[index, rule] = price
            reason[index, rule] = code

    return exit_row, exit_price, reason


@njit(cache=True)
def dedupe(candidates, stock_of_row, fills, ends, listed, delist_multiplier, open_price, high, low, close, before19,
           aligned_close, aligned_previous, folded_open, sma20, states, position):
    """종목마다 (a) 라이브 청산으로 보유 중이면 건너뛴다. 판 날 다음 행부터 다시 산다. candidates 는 행 오름차순."""
    keep = np.zeros(candidates.shape[0], dtype=np.bool_)
    blocked_stock = -1
    blocked_until = -1

    for index in range(candidates.shape[0]):
        row = candidates[index]
        stock = stock_of_row[row]

        if stock != blocked_stock:
            blocked_stock = stock
            blocked_until = -1

        if row <= blocked_until:
            continue

        keep[index] = True
        exit_row, _, code = exit_rule(KIND_ZONE, LIVE_TP, LIVE_SL, 0, row, ends[index], False, fills[index], open_price, high, low,
                                      close, before19, aligned_close, aligned_previous, folded_open, sma20, states, position,
                                      listed[index], delist_multiplier[index])
        blocked_until = ends[index] if code == 8 or code == 9 else exit_row     # 시가 갭 매도(10+)도 청산이다

    return keep


@njit(parallel=True, cache=True)
def control_returns(days, exits, bins, close_mode, open_matrix, close_matrix, filled_close, turnover_matrix, last_position,
                    delist_multiplier, edges_low, edges_high):
    """(날, 매도일, 구간)마다 그날 거래된 구간 종목 전부를 시가(또는 종가)에 사서 매도일 종가에 판 총수익 평균(배)과 종목 수."""
    count = days.shape[0]
    stocks = open_matrix.shape[1]
    result = np.full(count, np.nan)
    members = np.zeros(count, dtype=np.int64)

    for index in prange(count):
        day = days[index]
        exit_day = exits[index]
        low = edges_low[bins[index]]
        high = edges_high[bins[index]]
        total = 0.0
        number = 0

        for stock in range(stocks):
            turnover = turnover_matrix[day, stock]

            if not (turnover >= low and turnover < high):
                continue

            buy = close_matrix[day, stock] if close_mode else open_matrix[day, stock]
            sell = filled_close[exit_day, stock]

            if not (buy > 0 and sell > 0):
                continue

            if last_position[stock] < exit_day:
                sell *= delist_multiplier[stock]

            total += sell / buy
            number += 1

        if number > 0:
            result[index] = total / number

        members[index] = number

    return result, members


@njit(parallel=True, cache=True)
def excursions(entries, fills, ends, window, high, low, close, position):
    """진입마다 window 시장일 창의 최고 상승(MFE)·고점 전 최대 하락(MAE)·창 끝 수익(%) · 고점까지 시장일."""
    count = entries.shape[0]
    output = np.full((count, 4), np.nan)

    for index in prange(count):
        entry = entries[index]
        fill = fills[index]
        peak = -np.inf
        peak_row = entry
        last = entry

        for row in range(entry, ends[index]):
            if position[row] > position[entry] + window:
                break

            last = row

            if high[row] > peak:
                peak = high[row]
                peak_row = row

        trough = np.inf

        for row in range(entry, peak_row + 1):
            if low[row] < trough:
                trough = low[row]

        output[index, 0] = (peak / fill - 1.0) * 100.0
        output[index, 1] = (min(trough, fill) / fill - 1.0) * 100.0
        output[index, 2] = (close[last] / fill - 1.0) * 100.0
        output[index, 3] = position[peak_row] - position[entry]

    return output


# ── 계산 묶음 ───────────────────────────────────────────────────────────────

class Matrices:
    """대조용 시장일 × 종목 표."""

    def __init__(self, data: Data):
        panel = data.panel
        shape = (panel.market_count, panel.stock_count)
        stock = panel.stock_of_row.astype(np.int64)
        self.open = np.full(shape, np.nan)
        self.open[panel.market_position, stock] = data.open
        self.close = np.full(shape, np.nan)
        self.close[panel.market_position, stock] = data.close
        self.filled = pd.DataFrame(self.close).ffill().to_numpy()
        self.turnover = np.full(shape, np.nan)
        self.turnover[panel.market_position, stock] = data.previous_20d
        self.last_position = np.array([panel.market_position[panel.starts[stock + 1] - 1] for stock in range(panel.stock_count)], dtype=np.int64)
        low_edges = np.r_[0.0, round6_test.ABSOLUTE_EDGES]
        high_edges = np.r_[round6_test.ABSOLUTE_EDGES, np.inf]
        self.edges_low = np.maximum(low_edges, AMOUNT / RATIO_CAP)
        self.edges_high = high_edges


def evaluate(data: Data, matrices: Matrices, rules: pd.DataFrame, entries: np.ndarray, close_mode: bool) -> dict:
    """진입 행 배열에 모든 규칙을 붙여 매매 값 행렬을 낸다."""
    panel = data.panel
    dates = panel.dates
    entry_raw = data.raw_close[entries] if close_mode else data.raw_open[entries]
    entry_tick = data.close_tick[entries] if close_mode else data.entry_tick[entries]
    entry_mid = data.close[entries] if close_mode else data.open[entries]
    fills = entry_mid * (entry_raw + entry_tick) / entry_raw
    ends = data.stock_end[entries]
    listed = data.listed_row[entries]
    delist = data.delist_multiplier_row[entries]
    exit_row, exit_price, reason = run_exits(
        entries, close_mode, fills, ends, listed, delist, rules["kind"].to_numpy(np.int64), rules["tp"].to_numpy(np.float64),
        rules["sl"].to_numpy(np.float64), rules["param"].to_numpy(np.int64), data.open, data.high, data.low, data.close,
        data.before[20], data.aligned_close, data.aligned_previous, data.folded_open_aligned, data.sma[20], data.states,
        panel.market_position.astype(np.int64))
    gross = exit_price / entry_mid[:, None]
    buy_ratio = AMOUNT / data.previous_20d[entries]
    buy_cost = (entry_raw / (entry_raw + entry_tick) / (1.0 + round11_test.COMMISSION + round11_test.FLAT_IMPACT)
                / (1.0 + round11_test.LINEAR * buy_ratio))
    sell_raw = exit_price * data.unadjust_clean[exit_row]
    sell_dates = dates[exit_row]
    sell_tick = ticks(sell_raw.ravel(), sell_dates.ravel()).reshape(sell_raw.shape)
    sell_tax = round11_test.sell_tax(sell_dates.ravel()).reshape(sell_raw.shape)
    sell_base = data.previous_20d[exit_row]
    sell_ratio = np.where(np.isfinite(sell_base) & (np.nan_to_num(sell_base) > 0), AMOUNT / np.where(sell_base > 0, sell_base, 1.0),
                          buy_ratio[:, None])
    sell_cost = (np.maximum(sell_raw - sell_tick, 0.0) / np.where(sell_raw > 0, sell_raw, 1.0)
                 * (1.0 - round11_test.COMMISSION - sell_tax - round11_test.FLAT_IMPACT)
                 * np.maximum(0.0, 1.0 - round11_test.LINEAR * sell_ratio))
    cost = buy_cost[:, None] * sell_cost
    net = (gross * cost - 1.0) * 100.0
    # 대조
    day = panel.market_position[entries].astype(np.int64)
    exit_day = panel.market_position[exit_row].astype(np.int64)
    bins = data.bin[entries].astype(np.int64)
    key = pd.DataFrame({"day": np.repeat(day, exit_row.shape[1]), "exit": exit_day.ravel(), "bin": np.repeat(bins, exit_row.shape[1])})
    unique = key.drop_duplicates().reset_index(drop=True)
    control, members = control_returns(unique["day"].to_numpy(), unique["exit"].to_numpy(), unique["bin"].to_numpy(), close_mode,
                                       matrices.open, matrices.close, matrices.filled, matrices.turnover, matrices.last_position,
                                       data.delist_multiplier_stock, matrices.edges_low, matrices.edges_high)
    unique["control"] = control
    unique["members"] = members
    joined = key.merge(unique, on=["day", "exit", "bin"], how="left")
    control_gross = joined["control"].to_numpy().reshape(exit_row.shape)
    control_members = joined["members"].to_numpy().reshape(exit_row.shape)
    control_net = (control_gross * cost - 1.0) * 100.0
    return {"entries": entries, "fills": fills, "exit_row": exit_row, "exit_price": exit_price, "reason": reason, "net": net,
            "control_net": control_net, "value": net - control_net, "gross_pct": (gross - 1.0) * 100.0,
            "value_no_cost": (gross - control_gross) * 100.0, "control_members": control_members,
            "hold": exit_day - day[:, None], "month": panel.month_index[entries], "year": panel.years[entries],
            "unique_controls": len(unique)}


def summarize(result: dict, rules: pd.DataFrame, family: str, years: list[int]) -> tuple[pd.DataFrame, np.ndarray, np.ndarray, pd.DataFrame]:
    """칸 표, 달 합 행렬(칸 × 달), 달 건수 행렬, 연도 표."""
    month = result["month"]
    year = result["year"]
    rows, year_rows = [], []
    month_sum = np.zeros((len(rules), MONTHS))
    month_n = np.zeros((len(rules), MONTHS))
    live_cell = int(rules.loc[rules["rule"] == LIVE_RULE, "cell"].iloc[0])

    for cell in rules["cell"]:
        value = result["value"][:, cell]
        valid = np.isfinite(value)
        net = result["net"][:, cell]
        hold = result["hold"][:, cell]
        reason = result["reason"][:, cell]
        np.add.at(month_sum[cell], month[valid], value[valid])
        np.add.at(month_n[cell], month[valid], 1.0)
        pair = value - result["value"][:, live_cell]
        pair_valid = np.isfinite(pair)
        year_positive, year_with = 0, 0
        record = {"family": family, "cell": cell, "rule": rules.at[cell, "rule"], "group": rules.at[cell, "group"],
                  "n": int(valid.sum()), "n_unclosed": int((reason == 8).sum()), "n_delisted": int((reason == 9).sum()),
                  "hold_mean": float(np.mean(hold)), "hold_median": float(np.median(hold)),
                  "net_mean": float(np.nanmean(net)), "net_t": round6_test.month_cluster_t(net[np.isfinite(net)], month[np.isfinite(net)]),
                  "net_median": float(np.nanmedian(net)), "win_rate": float(np.mean(net > 0)),
                  "control_net_mean": float(np.nanmean(result["control_net"][:, cell])),
                  "value_mean": float(np.mean(value[valid])), "value_t": round6_test.month_cluster_t(value[valid], month[valid]),
                  "value_no_cost_mean": float(np.nanmean(result["value_no_cost"][:, cell])),
                  "net_per_hold_day": float(np.nanmean(net) / max(np.mean(np.maximum(hold, 1)), 1e-9)),
                  "net_total_krw_per_500k": float(np.nansum(net) / 100.0 * AMOUNT),
                  "control_members_median": float(np.nanmedian(result["control_members"][:, cell])),
                  "pair_vs_a_mean": float(np.mean(pair[pair_valid])) if cell != live_cell else 0.0,
                  "pair_vs_a_t": round6_test.month_cluster_t(pair[pair_valid], month[pair_valid]) if cell != live_cell else np.nan}

        for code, text in REASON_TEXT.items():
            record[f"share_{text}"] = float(np.mean((reason % 10) == code)) if code < 8 else float(np.mean(reason == code))

        record["share_gap_open"] = float(np.mean(reason >= 10))
        recent = valid & (year >= 2025)
        record["n_2025_2026"] = int(recent.sum())
        record["value_mean_2025_2026"] = float(np.mean(value[recent])) if recent.any() else np.nan
        record["value_t_2025_2026"] = round6_test.month_cluster_t(value[recent], month[recent]) if recent.sum() >= 3 else np.nan
        record["net_mean_2025_2026"] = float(np.nanmean(net[recent])) if recent.any() else np.nan

        for one in years:
            part = valid & (year == one)

            if part.any():
                year_with += 1
                year_positive += int(value[part].mean() > 0)

            year_rows.append({"family": family, "cell": cell, "rule": rules.at[cell, "rule"], "year": one, "n": int(part.sum()),
                              "net_mean": float(np.mean(net[part])) if part.any() else np.nan,
                              "control_net_mean": float(np.mean(result["control_net"][part, cell])) if part.any() else np.nan,
                              "value_mean": float(np.mean(value[part])) if part.any() else np.nan,
                              "value_t": round6_test.month_cluster_t(value[part], month[part]) if part.sum() >= 3 else np.nan,
                              "pair_vs_a_mean": float(np.mean(pair[part & pair_valid])) if (part & pair_valid).any() else np.nan,
                              "hold_mean": float(np.mean(hold[part])) if part.any() else np.nan})

        record["years_with_trades"] = year_with
        record["share_positive_years"] = year_positive / year_with if year_with else np.nan
        rows.append(record)

    return pd.DataFrame(rows), month_sum, month_n, pd.DataFrame(year_rows)


def hold_table(result: dict, rules: pd.DataFrame, family: str) -> pd.DataFrame:
    rows = []
    bins = np.searchsorted(np.array(HOLD_EDGES[1:]), result["hold"], side="right")

    for cell in rules["cell"]:
        value = result["value"][:, cell]

        for index, label in enumerate(HOLD_LABELS):
            part = (bins[:, cell] == index) & np.isfinite(value)
            rows.append({"family": family, "cell": cell, "rule": rules.at[cell, "rule"], "hold_bin": label, "n": int(part.sum()),
                         "share": float(part.mean()),
                         "net_mean": float(np.mean(result["net"][part, cell])) if part.any() else np.nan,
                         "control_net_mean": float(np.mean(result["control_net"][part, cell])) if part.any() else np.nan,
                         "value_mean": float(np.mean(value[part])) if part.any() else np.nan,
                         "value_t": round6_test.month_cluster_t(value[part], result["month"][part]) if part.sum() >= 3 else np.nan,
                         "value_sum_share": float(np.sum(value[part]) / np.sum(value[np.isfinite(value)])) if part.any() else np.nan})

    return pd.DataFrame(rows)


def live_buys() -> pd.DataFrame:
    """매매 장부(trades_*.csv)의 '정배열눌림' 매수 체결. 계좌 = 모의(logs)·실계좌(logs_live)."""
    frames = []

    for account, folder in (("모의", "logs"), ("실계좌", "logs_live")):
        for path in sorted(glob.glob(str(REPOSITORY / "Quant" / "build_win" / folder / "trades_*.csv"))):
            table = pd.read_csv(path, dtype=str)
            text = table.get("entry_reason", pd.Series("", index=table.index)).fillna("") + table.get("reason", pd.Series("", index=table.index)).fillna("")
            part = table[(table["event"] == "FILL") & (table["side"] == "BUY") & text.str.contains("정배열눌림")].copy()

            if len(part) == 0:
                continue

            part["account"] = account
            part["source"] = str(Path(path).relative_to(REPOSITORY)).replace("\\", "/")
            frames.append(part)

    if not frames:
        return pd.DataFrame()

    buys = pd.concat(frames, ignore_index=True)
    buys["date"] = buys["ts_kst"].str.slice(0, 10).str.replace("-", "").astype(np.int64)
    buys["time"] = buys["ts_kst"].str.slice(11, 16)
    buys["fill_price"] = buys["fill_price"].astype(float)
    buys["ticker"] = buys["ticker"].str.zfill(6)
    first = buys.sort_values("ts_kst").groupby(["account", "date", "ticker"], as_index=False).agg(
        time=("time", "first"), fill_price=("fill_price", "first"), fills=("fill_price", "size"), source=("source", "first"))
    return first


def live_compare(data: Data, flags: dict, p_rows: set, d1_rows: set, d2_rows: set) -> tuple[pd.DataFrame, pd.DataFrame]:
    panel = data.panel
    buys = live_buys()

    if len(buys) == 0:
        return pd.DataFrame(), pd.DataFrame()

    row_of = {(panel.codes[panel.stock_of_row[row]], int(panel.dates[row])): row for row in
              np.flatnonzero(panel.dates >= int(buys["date"].min()))}
    names = list(flags)
    records = []

    for buy in buys.itertuples():
        row = row_of.get((buy.ticker, int(buy.date)))
        record = {"account": buy.account, "date": int(buy.date), "ticker": buy.ticker, "time": buy.time, "fill_price": buy.fill_price,
                  "fills": int(buy.fills), "source": buy.source, "in_panel": row is not None}

        if row is not None:
            raw_to_clean = 1.0 / data.unadjust_clean[row]
            fill_clean = buy.fill_price * raw_to_clean
            folded = {window: (data.before[window][row] + fill_clean) / window for window in (5, 10, 20, 60)}
            record["name"] = panel.names[panel.stock_of_row[row]]
            record["raw_open"] = float(data.raw_open[row])
            record["fill_vs_open_pct"] = (buy.fill_price / data.raw_open[row] - 1.0) * 100.0
            record["dev_at_open_pct"] = float(data.deviation[row] * 100.0)
            record["dev_at_fill_pct"] = (fill_clean / folded[20] - 1.0) * 100.0
            record["aligned_at_fill"] = bool(folded[5] > folded[10] > folded[20] > folded[60])
            record["open_dev_pct"] = float(data.open_deviation[row] * 100.0)
            record["atr_ratio_pct"] = float(data.atr_ratio[row] * 100.0)
            record["previous_day_turnover_억"] = float(data.previous_day_turnover[row] / 1e8)
            record["previous_rank"] = float(data.previous_rank[row])
            record["kospi_gap_pct"] = float(data.index_gap[row] * 100.0)
            failed = [name for name in names if not flags[name][row]]
            record["failed_filters"] = "·".join(failed)
            record["first_failed"] = failed[0] if failed else ""
            record["in_D2_all_signals"] = row in d2_rows
            record["in_D1_no_rank"] = row in d1_rows
            record["in_P"] = row in p_rows

        records.append(record)

    detail = pd.DataFrame(records)
    summary_rows = []

    for account, part in detail.groupby("account"):
        inside = part[part["in_panel"]]
        dates = sorted(part["date"].unique())
        date_rows = np.flatnonzero(np.isin(panel.dates, dates))
        signals_on_dates = int(sum(1 for row in date_rows if row in d2_rows))
        p_on_dates = int(sum(1 for row in date_rows if row in p_rows))
        summary_rows.append({"account": account, "buy_days": len(dates), "first_date": int(min(dates)), "last_date": int(max(dates)),
                             "live_buys_stock_days": len(part), "in_panel": int(len(inside)),
                             "share_in_D2_all_signals": float(inside["in_D2_all_signals"].mean()),
                             "share_in_D1_no_rank": float(inside["in_D1_no_rank"].mean()),
                             "share_in_P": float(inside["in_P"].mean()),
                             "share_aligned_at_fill": float(inside["aligned_at_fill"].mean()),
                             "share_dev_at_fill_in_zone": float(((inside["dev_at_fill_pct"] >= -8) & (inside["dev_at_fill_pct"] <= 5)).mean()),
                             "median_fill_vs_open_pct": float(inside["fill_vs_open_pct"].median()),
                             "D2_signals_on_those_days": signals_on_dates, "P_entries_on_those_days": p_on_dates,
                             "share_D2_signals_bought_live": float(inside["in_D2_all_signals"].sum() / signals_on_dates) if signals_on_dates else np.nan})

        for name in names:
            summary_rows[-1][f"fail_{name}"] = int(inside["failed_filters"].fillna("").str.split("·").apply(lambda items: name in items).sum())

    return detail, pd.DataFrame(summary_rows)


def approximation_gaps(live_summary: pd.DataFrame, filter_counts: dict) -> pd.DataFrame:
    measured = {}

    for row in live_summary.itertuples():
        measured[row.account] = f"{row.account}: 근사 신호(겹침 허용) 안에 든 매수 {row.share_in_D2_all_signals:.0%}, 진입 묶음 P {row.share_in_P:.0%}"

    overlap = " / ".join(measured.values())
    rows = [
        ("진입 시각", "장중 아무 때(3분봉 기준선 지정가, 09:00–19:40)", "그날 시가 + 1호가", "장중 눌림을 시가로 사면 진입가가 다르다. 방향은 정해지지 않음", overlap),
        ("정배열 판정", "그날 현재가를 접은 SMA5>10>20>60(틱마다)", "전날 확정 정배열 AND 시가를 접은 정배열", "근사가 더 좁다(보수)", ""),
        ("이격 존", "접은 SMA20 대비 −8 – +5%(매 틱)", "시가 기준 −8 – +5%", "시가 밖·장중 안 인 날을 놓친다", ""),
        ("개장 이격·ATR", "09:03 가격·전날 ATR14", "시가·전날까지 ATR14", "거의 같음", ""),
        ("후보 목록", "당일 누적 거래대금 상위 200 ∪ 시세판 100 ∪ 거래량 급증 40 ∪ 등락률 100(≥1%), 그 뒤 당일 누적 거래대금 ≥ 10억", "전날 거래대금 ≥ 10억·전날 순위 ≤ 200(보통주 안 순위)",
         "엔진은 당일 값(미래 정보가 아니라 장중 실시간). ETF 를 뺀 순위라 근사가 조금 넓다. 순위 없는 판은 진단 D1", ""),
        ("지수 게이트", "코스피·코스닥 각 지수 −1% 이하면 신규 진입 없음(장중 실시간)", "코스피 시가 갭 ≤ −1% 인 날만 막음(코스닥도 코스피로)", "근사가 덜 막는다", f"코스피 자료 없는 날 {filter_counts.get('index_missing_days', 0)}"),
        ("슬롯·자본", "자본 200만·종목 50만 → 동시 약 4 종목, 먼저 잡힌 순서", "신호마다 한 건(슬롯 없음)", "라이브는 신호의 일부만 산다. 평균은 같아도 분산·순서 효과 없음", ""),
        ("익절·손절 체결", "익절은 평단 +5% 지정가, 손절·존 이탈은 시장가(틱 단위)", "일봉 고가·저가 닿으면 그 가격. 둘 다 닿으면 아래 먼저, 갭이면 시가. 양쪽 1호가 비용", "아래 먼저는 보수. 지정가 익절에 1호가를 물린 것도 보수", ""),
        ("존 이탈 정배열", "보유 중 틱마다 (전날 확정 OR 접은 정배열)", "시가에 (전날 확정 OR 시가 접은 값), 장중은 종가로만", "장중 정배열 깨짐 매도를 종가까지 늦춘다", ""),
        ("국면 청산·재기동 승계", "있음(국면 청산 조건·재기동 때 20일 자기 매수분 승계)", "없음", "빠진 청산 경로", ""),
        ("같은 종목 재진입", "그날 재진입 없음(reentry_cooldown 43200초)", "(a) 청산 뒤 다음 거래일부터", "같음", ""),
        ("정수 주 절사", "50만 ÷ 가격을 내림", "무시", "작음(가격 ≤ 10만 → 5 주 이상)", ""),
    ]
    return pd.DataFrame(rows, columns=["item", "engine", "daily_approximation", "bias_direction", "measured"])


@njit(cache=True)
def own_dedupe(candidates, stock_of_row, exit_rows, reasons, ends):
    """칸마다 자기 청산으로 보유 중이면 건너뛴다(D4). exit_rows·reasons 는 (후보, 칸)."""
    count, cells = exit_rows.shape
    keep = np.zeros((count, cells), dtype=np.bool_)

    for cell in range(cells):
        blocked_stock = -1
        blocked_until = -1

        for index in range(count):
            row = candidates[index]
            stock = stock_of_row[row]

            if stock != blocked_stock:
                blocked_stock = stock
                blocked_until = -1

            if row <= blocked_until:
                continue

            keep[index, cell] = True
            code = reasons[index, cell]
            blocked_until = ends[index] if code == 8 or code == 9 else exit_rows[index, cell]

    return keep


def group_cluster_t(values: np.ndarray, groups: np.ndarray) -> float:
    """평균의 묶음 t(묶음 = 종목 등). month_cluster_t 와 같은 식."""
    return round6_test.month_cluster_t(values, groups)


def robustness(results: dict, rules: pd.DataFrame, panel, candidates: np.ndarray, data) -> pd.DataFrame:
    """사후 진단(사전 등록 밖): 꼬리 쏠림·종목 묶음 t, 그리고 칸마다 자기 청산으로 겹침을 막은 판(D4)."""
    rows = []
    stock_codes = panel.stock_of_row

    def describe(family, cell, value, net, month, stocks, hold):
        ordered = np.sort(value)
        cut = max(1, len(value) // 100)
        total = value.sum()
        return {"family": family, "cell": cell, "rule": rules.at[cell, "rule"], "n": len(value), "distinct_stocks": int(len(np.unique(stocks))),
                "hold_mean": float(hold.mean()), "net_mean": float(net.mean()), "net_median": float(np.median(net)),
                "net_t_month": round6_test.month_cluster_t(net, month), "net_t_stock": group_cluster_t(net, stocks),
                "value_mean": float(value.mean()), "value_median": float(np.median(value)),
                "value_trim1_mean": float(ordered[cut:-cut].mean()), "value_mean_without_top10": float(ordered[:-10].mean()),
                "top10_share_of_value_sum": float(ordered[-10:].sum() / total) if total != 0 else np.nan,
                "value_t_month": round6_test.month_cluster_t(value, month), "value_t_stock": group_cluster_t(value, stocks)}

    p_result = results["P"]
    p_stocks = stock_codes[p_result["entries"]]

    for cell in rules["cell"]:
        valid = np.isfinite(p_result["value"][:, cell])
        rows.append(describe("P", cell, p_result["value"][valid, cell], p_result["net"][valid, cell], p_result["month"][valid], p_stocks[valid],
                             p_result["hold"][valid, cell]))

    result_d2 = results["D2 겹침 허용"]
    keep = own_dedupe(candidates, stock_codes.astype(np.int64), result_d2["exit_row"], result_d2["reason"], data.stock_end[candidates])
    d2_stocks = stock_codes[candidates]

    for cell in rules["cell"]:
        valid = keep[:, cell] & np.isfinite(result_d2["value"][:, cell])
        rows.append(describe("D4 칸마다 자기 청산으로 겹침 막음", cell, result_d2["value"][valid, cell], result_d2["net"][valid, cell], result_d2["month"][valid],
                             d2_stocks[valid], result_d2["hold"][valid, cell]))

    return pd.DataFrame(rows)


def flip_bar(month_sum: np.ndarray, month_n: np.ndarray, signs: np.ndarray, label: str) -> dict:
    _, summary, _, _ = round3_test.flip_null(month_sum, month_n, signs, label)
    bar = max(summary["a_max_t_null_p95_bar"], summary["b_bar_normal_equivalent_z"], BAR_FLOOR)
    return {"bar": bar, **summary}


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16]


MONTHS = 0


def main() -> None:
    global MONTHS
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    rules = build_rules()
    near = neighbours(rules)
    data = Data()
    panel = data.panel
    MONTHS = panel.month_count
    years = list(range(SIGNAL_FIRST // 10000, SIGNAL_LAST // 10000 + 1))
    print(f"준비 {data.prepare_seconds:.0f}초, 종목 {panel.stock_count}, 행 {panel.row_count}, 칸 {len(rules)}", flush=True)
    flags = data.filters()
    names = list(flags)
    all_signal = np.ones(panel.row_count, dtype=bool)

    for name in names:
        all_signal &= flags[name]

    no_rank = np.ones(panel.row_count, dtype=bool)

    for name in names:
        if name != "previous_rank_le_200":
            no_rank &= flags[name]

    filter_funnel = []
    running = np.ones(panel.row_count, dtype=bool)

    for name in names:
        running &= flags[name]
        filter_funnel.append({"filter": name, "rows_passing_alone": int(flags[name].sum()), "rows_after_cumulative": int(running.sum())})

    candidates = np.flatnonzero(all_signal).astype(np.int64)
    entry_raw = data.raw_open[candidates]
    fills = data.open[candidates] * (entry_raw + data.entry_tick[candidates]) / entry_raw
    keep = dedupe(candidates, panel.stock_of_row.astype(np.int64), fills, data.stock_end[candidates], data.listed_row[candidates],
                  data.delist_multiplier_row[candidates], data.open, data.high, data.low, data.close, data.before[20],
                  data.aligned_close, data.aligned_previous, data.folded_open_aligned, data.sma[20], data.states,
                  panel.market_position.astype(np.int64))
    p_entries = candidates[keep]
    d1_candidates = np.flatnonzero(no_rank).astype(np.int64)
    d1_raw = data.raw_open[d1_candidates]
    d1_fills = data.open[d1_candidates] * (d1_raw + data.entry_tick[d1_candidates]) / d1_raw
    d1_keep = dedupe(d1_candidates, panel.stock_of_row.astype(np.int64), d1_fills, data.stock_end[d1_candidates],
                     data.listed_row[d1_candidates], data.delist_multiplier_row[d1_candidates], data.open, data.high, data.low,
                     data.close, data.before[20], data.aligned_close, data.aligned_previous, data.folded_open_aligned, data.sma[20],
                     data.states, panel.market_position.astype(np.int64))
    d1_entries = d1_candidates[d1_keep]
    print(f"신호 {len(candidates)} → P {len(p_entries)}, D1(순위 없음) {len(d1_entries)} ({time.time() - started:.0f}초)", flush=True)
    matrices = Matrices(data)
    results = {"P": evaluate(data, matrices, rules, p_entries, False)}
    print(f"P 계산 끝 {time.time() - started:.0f}초", flush=True)
    results["D1 순위 조건 없음"] = evaluate(data, matrices, rules, d1_entries, False)
    results["D2 겹침 허용"] = evaluate(data, matrices, rules, candidates, False)
    results["D3 그날 종가 진입"] = evaluate(data, matrices, rules, p_entries, True)
    print(f"진단 판 끝 {time.time() - started:.0f}초", flush=True)
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, MONTHS)) < 0.5, -1.0, 1.0)
    cell_frames, year_frames, hold_frames = [], [], []
    comparison = {}

    for family, result in results.items():
        table, month_sum, month_n, year_table = summarize(result, rules, family, years)
        hold_frames.append(hold_table(result, rules, family))
        year_frames.append(year_table)

        if family == "P":
            bars = flip_bar(month_sum, month_n, signs, "P 63 칸 값(순수익 − 대조)")
            live_cell = int(rules.loc[rules["rule"] == LIVE_RULE, "cell"].iloc[0])
            pair = result["value"] - result["value"][:, [live_cell]]
            pair_sum = np.zeros((len(rules), MONTHS))
            pair_n = np.zeros((len(rules), MONTHS))
            month = result["month"]

            for cell in rules["cell"]:
                if cell == live_cell:
                    continue

                valid = np.isfinite(pair[:, cell])
                np.add.at(pair_sum[cell], month[valid], pair[valid, cell])
                np.add.at(pair_n[cell], month[valid], 1.0)

            others = rules["cell"].to_numpy() != live_cell
            pair_up = flip_bar(pair_sum[others], pair_n[others], signs, "짝 차이(칸 − (a)) 62 칸, 위쪽")
            pair_down = flip_bar(-pair_sum[others], pair_n[others], signs, "짝 차이(칸 − (a)) 62 칸, 아래쪽")
            pair_bar = max(pair_up["bar"], pair_down["bar"])
            sign = np.sign(table["value_mean"].to_numpy())
            table["neighbours"] = ["·".join(str(cell) for cell in cells) for cells in near]
            table["neighbours_same_sign"] = [bool(len(cells) > 0 and all(np.sign(table.at[cell, "value_mean"]) == sign[index] for cell in cells))
                                             for index, cells in enumerate(near)]
            table["bar"] = bars["bar"]
            table["passes"] = ((table["value_t"] >= bars["bar"]) & table["neighbours_same_sign"]
                               & (table["share_positive_years"] >= JUDGE_POSITIVE_YEARS) & (table["value_mean"] > 0))
            table["pair_bar"] = pair_bar
            table["pair_better_than_a"] = table["pair_vs_a_t"] >= pair_bar
            table["pair_worse_than_a"] = table["pair_vs_a_t"] <= -pair_bar
            comparison = {"cells": len(rules), "value_bar": bars, "pair_bar": pair_bar, "pair_up": pair_up, "pair_down": pair_down,
                          "cells_passing": table.loc[table["passes"], "rule"].tolist(),
                          "cells_t_over_bar": table.loc[table["value_t"] >= bars["bar"], "rule"].tolist(),
                          "pairs_better_than_a": table.loc[table["pair_better_than_a"], "rule"].tolist(),
                          "pairs_worse_than_a": table.loc[table["pair_worse_than_a"], "rule"].tolist(),
                          "flips": round3_test.FLIPS, "seed": common.SEED, "months": MONTHS,
                          "rule": "통과 = 값 t ≥ 기준선 · 옆 칸 같은 부호 · 양수 해 비율 ≥ 0.7 · 평균 > 0. 기준선 = max(뒤집기 (가) 최대 t 95%, (나) 정규 환산 z, 3.5)"}
        else:
            table["passes"] = False

        cell_frames.append(table)

    cell_table = pd.concat(cell_frames, ignore_index=True)
    common.write_csv(OUTPUT / "cell_table.csv", cell_table)
    common.write_csv(OUTPUT / "year_table.csv", pd.concat(year_frames, ignore_index=True))
    common.write_csv(OUTPUT / "hold_breakdown.csv", pd.concat(hold_frames, ignore_index=True))
    common.write_json(OUTPUT / "multiple_comparison.json", comparison)
    common.write_csv(OUTPUT / "robustness.csv", robustness(results, rules, panel, candidates, data))
    # MAE·MFE
    p_result = results["P"]
    mae_rows = []

    for window in HOLD_DAYS:
        output = excursions(p_entries, p_result["fills"], data.stock_end[p_entries], window, data.high, data.low, data.close,
                            panel.market_position.astype(np.int64))

        for subset, mask in (("전체", np.ones(len(output), dtype=bool)), ("창 끝 이긴 매매", output[:, 2] > 0),
                             ("창 끝 진 매매", output[:, 2] <= 0)):
            for column, label in enumerate(("MFE 최고 상승 %", "MAE 고점 전 최대 하락 %", "창 끝 수익 %", "고점까지 시장일")):
                values = output[mask, column]
                mae_rows.append({"window_days": window, "subset": subset, "measure": label, "n": int(mask.sum()),
                                 **{f"p{percentile}": float(np.nanpercentile(values, percentile)) for percentile in (5, 10, 25, 50, 75, 90, 95)},
                                 "mean": float(np.nanmean(values))})

        mfe, mae = output[:, 0], output[:, 1]
        mae_rows.append({"window_days": window, "subset": "전체", "measure": "MFE ≥ 5% 이면서 고점 전 MAE > −6.5% 비율",
                         "n": len(output), "mean": float(np.mean((mfe >= LIVE_TP) & (mae > -LIVE_SL)))})

    common.write_csv(OUTPUT / "mae_mfe.csv", pd.DataFrame(mae_rows))
    # 라이브 대조
    detail, live_summary = live_compare(data, flags, set(p_entries.tolist()), set(d1_entries.tolist()), set(candidates.tolist()))
    common.write_csv(OUTPUT / "live_entry_filters.csv", detail)
    common.write_csv(OUTPUT / "live_overlap.csv", live_summary)
    common.write_csv(OUTPUT / "approximation_gaps.csv", approximation_gaps(live_summary, {"index_missing_days": data.index_missing_days}))
    common.write_csv(OUTPUT / "entry_funnel.csv", pd.DataFrame(filter_funnel))
    # 매매 기록(P)
    count, cells = p_result["value"].shape
    trades = pd.DataFrame({
        "code": np.repeat(np.array(panel.codes)[panel.stock_of_row[p_entries]], cells),
        "entry_date": np.repeat(panel.dates[p_entries], cells),
        "cell": np.tile(np.arange(cells), count).astype(np.int16),
        "exit_date": panel.dates[p_result["exit_row"]].ravel(),
        "reason": p_result["reason"].ravel(),
        "hold_days": p_result["hold"].ravel().astype(np.int32),
        "gross_pct": p_result["gross_pct"].ravel().astype(np.float32),
        "net_pct": p_result["net"].ravel().astype(np.float32),
        "control_net_pct": p_result["control_net"].ravel().astype(np.float32),
        "value_pct": p_result["value"].ravel().astype(np.float32)})
    trades.to_parquet(OUTPUT / "trades.parquet", index=False)
    common.write_csv(OUTPUT / "rules.csv", rules)
    command = "py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_exit_test.py"
    common.write_text(OUTPUT / "run_command.txt", command + "\n")
    hashed = {name: file_hash(OUTPUT / name) for name in ("cell_table.csv", "year_table.csv", "hold_breakdown.csv", "mae_mfe.csv",
                                                          "multiple_comparison.json", "live_overlap.csv", "rules.csv", "robustness.csv")}
    metrics = {"reproduction": panel.reproduction(), "command": command, "seconds": round(time.time() - started, 1),
               "corrections": data.correction, "halt_jump_fixes": data.jump_count, "delisted_stocks": int(len(data.delist)),
               "kospi_missing_signal_days": data.index_missing_days,
               "entries": {"signals_D2": int(len(candidates)), "P": int(len(p_entries)), "D1": int(len(d1_entries))},
               "control_unique_keys": {family: result["unique_controls"] for family, result in results.items()},
               "config_live_strategy0": {key: value for key, value in json.loads(CONFIG_LIVE.read_text(encoding="utf-8"))["strategies"][0].items()
                                         if key in ("pullback_pct", "entry_upper_pct", "dev_buy_pct", "dev_sell_pct", "stop_loss_pct",
                                                    "entry_atr_max_pct", "entry_open_dev_min_pct", "min_price", "max_price", "min_turnover",
                                                    "turnover_top_n", "risk_off_index_pct", "notional_cap_krw", "buy_split_steps",
                                                    "market_close_exit_hhmm", "reentry_cooldown_sec", "sma_period", "max_dev_pct")},
               "cost": {"commission": round11_test.COMMISSION, "flat_impact": round11_test.FLAT_IMPACT, "slippage_ticks": 1,
                        "linear": round11_test.LINEAR, "amount": AMOUNT, "sell_tax": round11_test.SELL_TAX_SCHEDULE},
               "output_sha256_16": hashed}
    common.write_json(OUTPUT / "metrics.json", metrics)
    print(f"끝 {time.time() - started:.0f}초", flush=True)
    print(cell_table.loc[cell_table["family"] == "P", ["rule", "n", "hold_mean", "net_mean", "value_mean", "value_t", "pair_vs_a_mean",
                                                       "pair_vs_a_t", "passes"]].to_string(index=False), flush=True)


if __name__ == "__main__":
    main()
