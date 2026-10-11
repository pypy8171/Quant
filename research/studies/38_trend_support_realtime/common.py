"""스터디 38 공용 코드 — 자료 읽기, 지표, 비교 평균표, 크기 분위, 매매 계산 커널(numba).

다른 스크립트(trade_test.py·random_test.py·intraday_test.py·build_dashboard.py·measure_structure.py)가 import 한다.
설계는 2026-10-10 사용자 승인안(조정 지시 포함): 주가·거래대금 거름 없음, ±35% 매매 빼지 않음(따로 표시),
보유 상한 없음, % 손절 없음, 익절 없음, 매도는 추세 붕괴 E1–E4 만, 자동 선택 없음(모든 조합 표).

자료: PYQuant/data/bars_all_pit_v2.parquet(수정주가, 상장폐지 포함) → 스터디 33 load_inputs 그대로
  (보통주만 — is_common_stock, 거래량 0 인 날 제외, 시가·고가·저가 0 정리). 신호일 2010-01-01 – 2026-10-08.
미래 정보 차단:
  - 확정 저점·고점·추세는 스터디 37 swing_definitions 그대로 — 저점 i 는 i+k 행 종가가 난 뒤부터 쓴다.
  - 이평 MA_N[t] = t 종가까지 N 개 종가의 단순 평균(종가 판단이라 그날 종가 포함).
  - ATR_p[t] = t−p .. t−1 행 참범위 평균(그날 이전 행까지만). 참범위 = max(고가, 전일 종가) − min(저가, 전일 종가), 첫 행은 고가 − 저가.
  - 매도 판단의 "마지막 확정 저점"은 그날 전(u−1 종가까지) 확정된 것.
가격이 모두 정수라 누적합 기반 평균은 정확히 같은 값이 나온다(대시보드 JS 재계산이 같은 결과를 내는 근거).
비용: 스터디 35 ma_sweep.COST(라이브 수수료 0.015%/편 + 매도세, PYQuant/backtest/costs.py LIVE) 왕복 비율.
"""
from __future__ import annotations

import importlib.util
import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

os.environ.setdefault("NUMBA_NUM_THREADS", "12")
import numba  # noqa: E402
from numba import njit, prange  # noqa: E402

STUDY = Path(__file__).resolve().parent
STUDIES = STUDY.parent
REPOSITORY = STUDIES.parent.parent
sys.path.insert(0, str(STUDIES / "37_swing_trend"))
import swing_definitions as swing  # noqa: E402

_specification = importlib.util.spec_from_file_location("study35_ma_sweep", STUDIES / "35_surge_box_breakout" / "ma_sweep.py")
sweep = importlib.util.module_from_spec(_specification)
sys.modules["study35_ma_sweep"] = sweep
_specification.loader.exec_module(sweep)
study33 = sweep.study33
COST = float(sweep.COST)

# ── 사용자가 정한 격자(2026-10-10) ───────────────────────────────────────────
LAST_DATE = 20261008
SIGNAL_FROM = 20100101
K_VALUES = (2, 3, 5, 7, 10, 15, 20, 30)
WINDOWS = (5, 10, 20, 40, 60, 120, 200)
ATR_PERIODS = (5, 10, 20, 40, 60)
DISTANCE_EDGES = np.round(np.arange(-2.0, 1.0001, 0.25), 4)     # 13 개 경계 → 안쪽 12 구간 + 양쪽 바깥 = 14
BOUNCE_EDGES = np.round(np.arange(0.0, 1.0001, 0.1), 4)         # 11 개 경계 → 10 구간
RUN_DAYS = (1, 2, 3, 4, 5)
RULE_NAMES = ("E1", "E2", "E3", "E4j1", "E4j2", "E4j3", "E4j4", "E4j5")
RULE_TEXT = {"E1": "종가가 마지막 확정 저점 아래", "E2": "장중 마지막 확정 저점 첫 이탈(갭이면 시가)",
             "E3": "추세 판정이 하락이 된 날 종가", "E4j1": "이평 N 아래 종가 1일", "E4j2": "이평 N 아래 종가 2일 연속",
             "E4j3": "이평 N 아래 종가 3일 연속", "E4j4": "이평 N 아래 종가 4일 연속", "E4j5": "이평 N 아래 종가 5일 연속"}
PRICE_BREAK = 0.35            # ±35% 넘는 날 표시(매매는 빼지 않는다)
TURNOVER_DAYS = 20            # 20일 평균 거래대금
POSITION_KRW = 5_000_000.0    # 포트폴리오 매매당 금액
STOP_BUCKET_EDGES = (0.0, -2.0, -4.0, -6.0, -8.0, -10.0, -15.0)
SEED = 20261010

DISTANCE_BINS = len(DISTANCE_EDGES) + 1     # 14
BOUNCE_BINS = len(BOUNCE_EDGES) - 1         # 10
CELLS = DISTANCE_BINS * BOUNCE_BINS         # 140
GROUP_UP = 0
GROUP_DOWN = CELLS
GROUP_NO_TOUCH = 2 * CELLS                  # (b) 상승 추세·저가가 이평 위(안 닿음)
GROUPS = 2 * CELLS + 1
POOL_ALL = 0                                # 상승 추세 모든 칸 매매 합(매수 조건 전체)
POOL_INNER = 1                              # 거리 안쪽 12 구간(−2.0 – +1.0) 칸만 합
POOLS = 2
REASON_RULE, REASON_OPEN, REASON_END = 1, 2, 3
REASON_TEXT = {REASON_RULE: "매도 규칙", REASON_OPEN: "미청산(2026-10-08 종가 평가)", REASON_END: "시세 끝(상장폐지·정지, 마지막 종가)"}

# 묶음 통계 열 번호
F_N, F_SUM, F_SUM2, F_WIN, F_HOLD, F_MAE, F_MFE, F_PATH, F_BENCH, F_EXCESS = range(10)
F_OPEN, F_END, F_BREAK, F_BREAK_SUM, F_SAME_DAY, F_EXCESS2, F_PEAK_DAYS, F_MFE_POSITIVE, F_RATIO, F_STOP, F_STOP_N = range(10, 21)
F_T_NET, F_T_EXCESS, F_MEDIAN = 21, 22, 23
FIELDS = 24
FIELD_NAMES = ("n", "sum_net", "sum_net2", "wins", "sum_hold", "sum_mae", "sum_mfe", "n_path", "sum_bench", "sum_excess",
               "n_open", "n_data_end", "n_break", "sum_net_break", "n_same_day_signal", "sum_excess2", "sum_peak_to_exit",
               "n_mfe_positive", "sum_ratio", "sum_stop_distance", "n_stop_distance", "t_net", "t_excess", "median_net")

# 분포 표(구간 셈)
HOLD_BINS = 4500
RATIO_LOW, RATIO_HIGH, RATIO_STEP = -20.0, 1.0, 0.001


def percent_bin_edges() -> np.ndarray:
    """수익·MFE·손절 거리(%) 구간 경계: −100 – 100 은 0.05, 100 – 1,000 은 0.5, 1,000 – 100,000 은 10(내가 정함 — 분위수 근사 폭)."""
    return np.round(np.concatenate([np.arange(-100.0, 100.0, 0.05), np.arange(100.0, 1000.0, 0.5),
                                    np.arange(1000.0, 100000.0001, 10.0)]), 6)


PERCENT_EDGES = percent_bin_edges()
PERCENT_BINS = len(PERCENT_EDGES) + 1          # 0 = −100 미만, 끝 = 100,000 이상
RATIO_EDGES = np.round(np.arange(RATIO_LOW, RATIO_HIGH + 1e-9, RATIO_STEP), 6)
RATIO_BINS = len(RATIO_EDGES) + 1


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPOSITORY, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def write_text(path: Path, text: str) -> None:
    with open(path, "wb") as handle:
        handle.write(text.encode("utf-8"))


def write_json(path: Path, payload, indent: int | None = 1) -> None:
    separators = None if indent is not None else (",", ":")
    write_text(path, json.dumps(payload, ensure_ascii=False, indent=indent, separators=separators, default=json_default) + "\n")


def write_csv(path: Path, frame: pd.DataFrame) -> None:
    write_text(path, frame.to_csv(index=False, float_format="%.10g", lineterminator="\n"))


def json_default(value):
    if isinstance(value, (np.integer,)):
        return int(value)

    if isinstance(value, (np.floating,)):
        return None if not np.isfinite(value) else float(value)

    if isinstance(value, np.ndarray):
        return value.tolist()

    if isinstance(value, (np.bool_,)):
        return bool(value)

    raise TypeError(type(value))


def clean_number(value, digits: int = 4):
    if value is None:
        return None

    value = float(value)
    return round(value, digits) if math.isfinite(value) else None


def distance_bin_label(index: int) -> str:
    if index == 0:
        return f"<{DISTANCE_EDGES[0]:g}"

    if index == DISTANCE_BINS - 1:
        return f">={DISTANCE_EDGES[-1]:g}"

    return f"{DISTANCE_EDGES[index - 1]:g}–{DISTANCE_EDGES[index]:g}"


def bounce_bin_label(index: int) -> str:
    return f"{BOUNCE_EDGES[index]:g}–{BOUNCE_EDGES[index + 1]:g}"


def stop_bucket_labels() -> list[str]:
    edges = STOP_BUCKET_EDGES
    labels = [f">={edges[0]:g}%"]
    labels += [f"{edges[index + 1]:g}–{edges[index]:g}%" for index in range(len(edges) - 1)]
    labels += [f"<{edges[-1]:g}%", "기준선 없음"]
    return labels


# ── 자료 ────────────────────────────────────────────────────────────────────

@njit(cache=True)
def rolling_mean_known(values, starts, window, include_today):
    """종목마다 누적합으로 낸 단순 평균. include_today=True 면 t 포함 window 개, False 면 t−window .. t−1."""
    result = np.full(values.shape[0], np.nan)

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        cumulative = np.zeros(last - first + 1)

        for offset in range(last - first):
            cumulative[offset + 1] = cumulative[offset] + values[first + offset]

        for offset in range(last - first):
            if include_today:
                if offset + 1 >= window:
                    result[first + offset] = (cumulative[offset + 1] - cumulative[offset + 1 - window]) / window
            else:
                if offset >= window:
                    result[first + offset] = (cumulative[offset] - cumulative[offset - window]) / window

    return result


class Panel:
    """전 종목 일봉을 종목 순서로 이어 붙인 배열(행 = 종목의 거래된 날). starts[s] .. starts[s+1]−1 이 종목 s."""

    def __init__(self, last_date: int = LAST_DATE):
        stocks, _, self.information = study33.load_inputs(last_date)
        self.stocks = stocks
        self.codes = [series.code for series in stocks]
        self.names = [series.name for series in stocks]
        lengths = np.array([len(series.dates) for series in stocks], dtype=np.int64)
        self.starts = np.r_[0, np.cumsum(lengths)].astype(np.int64)
        self.row_count = int(self.starts[-1])
        self.stock_count = len(stocks)
        self.dates = np.concatenate([series.dates for series in stocks]).astype(np.int64)
        self.open_price = np.concatenate([series.open_price for series in stocks]).astype(np.float64)
        self.high = np.concatenate([series.high for series in stocks]).astype(np.float64)
        self.low = np.concatenate([series.low for series in stocks]).astype(np.float64)
        self.close = np.concatenate([series.close for series in stocks]).astype(np.float64)
        self.volume = np.concatenate([series.volume for series in stocks]).astype(np.float64)
        self.factor = np.concatenate([series.factor for series in stocks]).astype(np.float64)
        self.stock_of_row = np.repeat(np.arange(self.stock_count), lengths).astype(np.int32)
        self.market_dates = np.unique(self.dates)
        self.market_count = len(self.market_dates)
        self.last_market_date = int(self.market_dates[-1])
        self.market_position = np.searchsorted(self.market_dates, self.dates).astype(np.int32)
        self.signal_start = int(np.searchsorted(self.market_dates, SIGNAL_FROM))
        self.signal_ok = (self.dates >= SIGNAL_FROM) & (self.dates <= LAST_DATE)
        self.years = (self.dates // 10000).astype(np.int32)
        self.first_year = SIGNAL_FROM // 10000
        self.year_count = LAST_DATE // 10000 - self.first_year + 1
        self.year_index = np.clip(self.years - self.first_year, 0, self.year_count - 1).astype(np.int32)
        month = (self.dates // 100) % 100
        self.month_index = ((self.years - self.first_year) * 12 + month - 1).astype(np.int32)
        self.month_count = int(self.month_index.max()) + 1
        self.listed_at_end = np.array([int(series.dates[-1]) == self.last_market_date for series in stocks])
        previous_close = np.r_[np.nan, self.close[:-1]]
        previous_close[self.starts[:-1]] = np.nan

        with np.errstate(invalid="ignore", divide="ignore"):
            broken = ((np.abs(self.open_price / previous_close - 1.0) > PRICE_BREAK)
                      | (np.abs(self.close / previous_close - 1.0) > PRICE_BREAK))

        self.break_day = broken
        self.break_cumulative = np.cumsum(broken).astype(np.int32)
        true_range = (np.maximum(self.high, np.where(np.isfinite(previous_close), previous_close, -np.inf))
                      - np.minimum(self.low, np.where(np.isfinite(previous_close), previous_close, np.inf)))
        self.true_range = true_range
        self.raw_close = self.close * self.factor
        self.turnover20 = rolling_mean_known(self.raw_close * self.volume, self.starts, TURNOVER_DAYS, True)
        self.averages = np.stack([rolling_mean_known(self.close, self.starts, window, True) for window in WINDOWS])
        self.atrs = np.stack([rolling_mean_known(true_range, self.starts, period, False) for period in ATR_PERIODS])
        self.bench = None
        self.deciles = None
        self.market_cap = None

    def trend(self, span: int) -> tuple[np.ndarray, np.ndarray, list]:
        """k 마다 추세 상태(int8)와 그날 종가까지 확정된 마지막 저점(float64), 종목별 확정 극점."""
        state = np.empty(self.row_count, dtype=np.int8)
        latest_low = np.empty(self.row_count)
        pivots_by_stock = []

        for stock, series in enumerate(self.stocks):
            first, last = self.starts[stock], self.starts[stock + 1]
            pivots = swing.confirmed_pivots(series.low, series.high, span)
            result = swing.trend_state(pivots, last - first)
            state[first:last] = result["state"]
            latest_low[first:last] = result["latest_low"]
            pivots_by_stock.append(pivots)

        return state, latest_low, pivots_by_stock

    def build_benchmark(self) -> np.ndarray:
        """B[s, e](시장 날짜 위치 − signal_start) = s 날 거래된 보통주 전부를 s 종가에 사서 e 종가에 판 순수익 평균(%).
        거래 없는 날은 전 종가를 이어 쓴다(상장폐지 뒤 = 마지막 종가). ±35% 종목도 빼지 않는다."""
        close = np.full((self.market_count, self.stock_count), np.nan)
        close[self.market_position, self.stock_of_row] = self.close
        alive = np.isfinite(close)
        filled = pd.DataFrame(close).ffill().to_numpy()
        part = slice(self.signal_start, self.market_count)
        weight = np.where(alive[part], 1.0 / np.where(alive[part], close[part], 1.0), 0.0)
        counts = alive[part].sum(axis=1)
        target = np.nan_to_num(filled[part], nan=0.0)
        total = weight @ target.T
        bench = (total / counts[:, None] * COST - 1.0) * 100.0
        bench[np.tril_indices(bench.shape[0], -1)] = np.nan
        self.bench = np.ascontiguousarray(bench)
        self.bench_counts = counts
        return self.bench

    def build_deciles(self) -> np.ndarray:
        """그날 거래된 전 종목 안 횡단면 순위 10분위(0 = 가장 작음). 열 0 주가(무수정 종가), 1 20일 평균 거래대금, 2 시가총액.
        시가총액 = 무수정 종가 × shares_point_in_time.shares_common_issued(effective_date ≤ 그날 최근값). 없으면 −1."""
        shares = pd.read_parquet(REPOSITORY / "PYQuant" / "data" / "fin" / "shares_point_in_time.parquet",
                                 columns=["ticker", "effective_date", "shares_common_issued"])
        shares = shares[np.isfinite(shares["shares_common_issued"]) & (shares["shares_common_issued"] > 0)].copy()
        shares["effective_int"] = shares["effective_date"].dt.strftime("%Y%m%d").astype(np.int64)
        shares = shares.sort_values(["effective_int", "ticker"], kind="mergesort")
        rows = pd.DataFrame({"ticker": np.array(self.codes)[self.stock_of_row], "date": self.dates, "row": np.arange(self.row_count)})
        rows = rows.sort_values(["date", "ticker"], kind="mergesort")
        merged = pd.merge_asof(rows, shares[["ticker", "effective_int", "shares_common_issued"]], left_on="date",
                               right_on="effective_int", by="ticker", direction="backward")
        issued = np.full(self.row_count, np.nan)
        issued[merged["row"].to_numpy()] = merged["shares_common_issued"].to_numpy()
        self.market_cap = self.raw_close * issued
        self.shares_issued = issued
        deciles = np.full((self.row_count, 3), -1, dtype=np.int8)
        frame = pd.DataFrame({"position": self.market_position, "price": self.raw_close, "turnover": self.turnover20,
                              "cap": self.market_cap})

        for column, name in enumerate(("price", "turnover", "cap")):
            values = frame[name].to_numpy()
            valid = np.isfinite(values)
            part = frame.loc[valid, ["position", name]]
            rank = part.groupby("position")[name].rank(method="first").to_numpy()
            size = part.groupby("position")[name].transform("size").to_numpy()
            deciles[np.flatnonzero(valid), column] = np.minimum((rank - 1) * 10 // size, 9).astype(np.int8)

        self.deciles = deciles
        return deciles

    def reproduction(self) -> dict:
        bars_path = study33.BARS_PATH
        return {"commit": git_commit(), "bars_path": str(bars_path.relative_to(REPOSITORY)).replace("\\", "/"),
                "bars_size": bars_path.stat().st_size, "bars_rows_loaded": self.information["bars_rows"],
                "common_codes": self.information["common_codes"], "stocks": self.stock_count, "rows": self.row_count,
                "last_market_date": self.last_market_date, "signal_from": SIGNAL_FROM, "cost_ratio": COST, "seed": SEED,
                "numba_threads": numba.config.NUMBA_NUM_THREADS}


# ── 매매 계산 커널 ──────────────────────────────────────────────────────────

@njit(cache=True)
def search_right(edges, value):
    """edges 중 value 이하인 개수(np.searchsorted side='right' 와 같음)."""
    low = 0
    high = edges.shape[0]

    while low < high:
        middle = (low + high) // 2

        if edges[middle] <= value:
            low = middle + 1
        else:
            high = middle

    return low


@njit(cache=True)
def distance_bin(distance, edges):
    if not np.isfinite(distance):
        return -1

    return search_right(edges, distance)


@njit(cache=True)
def bounce_bin(close, low, high, edges):
    if not (high > low):
        return -1

    value = (close - low) / (high - low)
    index = search_right(edges, value) - 1
    return min(max(index, 0), edges.shape[0] - 2)


@njit(cache=True)
def percent_bin(value, edges):
    if not np.isfinite(value):
        return -1

    return search_right(edges, value)


@njit(cache=True)
def exit_flag(rule, row, first, close, low, state, latest_low, run_length):
    """rule 0 E1·1 E2·2 E3·3–7 E4(j = rule−2). row 날 매도 조건이 섰는가."""
    if rule <= 1:
        if row <= first:
            return False

        level = latest_low[row - 1]

        if not np.isfinite(level):
            return False

        if rule == 0:
            return close[row] < level

        return low[row] < level

    if rule == 2:
        return state[row] == -1

    return run_length[row - first] >= rule - 2


@njit(cache=True)
def cluster_t(month_sum, month_count, total, count):
    """평균의 월 묶음(CR1) t."""
    if count < 3:
        return np.nan

    mean = total / count
    groups = 0
    squared = 0.0

    for month in range(month_sum.shape[0]):
        if month_count[month] > 0:
            groups += 1
            residual = month_sum[month] - month_count[month] * mean
            squared += residual * residual

    if groups < 2 or squared <= 0.0:
        return np.nan

    error = math.sqrt(groups / (groups - 1) * squared) / count
    return mean / error


@njit(cache=True)
def build_sparse(values, starts, leftmost_max):
    """구간 최고(leftmost_max=True, 같은 값이면 앞쪽 행)·최저 행 번호를 O(1)로 찾는 표. 구간은 한 종목 안에서만 묻는다."""
    rows = values.shape[0]
    longest = 1

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    levels = 1

    while (1 << levels) <= longest:
        levels += 1

    table = np.empty((levels, rows), dtype=np.int32)

    for row in range(rows):
        table[0, row] = row

    for level in range(1, levels):
        half = 1 << (level - 1)

        for row in range(rows):
            left = table[level - 1, row]

            if row + half < rows:
                right = table[level - 1, row + half]

                if leftmost_max:
                    table[level, row] = left if values[left] >= values[right] else right
                else:
                    table[level, row] = left if values[left] <= values[right] else right
            else:
                table[level, row] = left

    return table


@njit(cache=True)
def query_sparse(table, values, first, last, leftmost_max):
    """first .. last(포함) 구간의 최고(또는 최저) 행."""
    size = last - first + 1
    level = 0

    while (1 << (level + 1)) <= size:
        level += 1

    left = table[level, first]
    right = table[level, last - (1 << level) + 1]

    if leftmost_max:
        return left if values[left] >= values[right] else right

    return left if values[left] <= values[right] else right


@njit(cache=True)
def add_trade(statistics, net, hold, has_path, mae, mfe, peak_days, benchmark, excess, reason, broken, stop_distance):
    statistics[0] += 1.0
    statistics[1] += net
    statistics[2] += net * net
    statistics[3] += 1.0 if net > 0 else 0.0
    statistics[4] += hold

    if has_path:
        statistics[5] += mae
        statistics[6] += mfe
        statistics[7] += 1.0
        statistics[16] += peak_days

        if mfe > 0:
            statistics[17] += 1.0
            statistics[18] += net / mfe

    statistics[8] += benchmark
    statistics[9] += excess
    statistics[15] += excess * excess

    if reason == 2:
        statistics[10] += 1.0
    elif reason == 3:
        statistics[11] += 1.0

    if broken:
        statistics[12] += 1.0
        statistics[13] += net

    if np.isfinite(stop_distance):
        statistics[19] += stop_distance
        statistics[20] += 1.0


@njit(cache=True)
def exit_of(rule, offset, length, next_exit, first, open_price, close, latest_low):
    """offset 날 종가 매수의 매도 행 번호(종목 안 offset)와 가격. 매도 조건이 끝까지 안 서면 (length, NaN)."""
    days = rule - 2 if rule >= 3 else 1
    exit_offset = next_exit[offset + days] if offset + days < length else length

    if exit_offset >= length:
        return length, np.nan

    exit_row = first + exit_offset

    if rule == 1:
        level = latest_low[exit_row - 1]
        return exit_offset, (open_price[exit_row] if open_price[exit_row] < level else level)

    return exit_offset, close[exit_row]


@njit(cache=True)
def fill_next_exit(rule, first, length, average, close, low, state, latest_low, run_length, next_exit):
    if rule >= 3:
        for offset in range(length):
            row = first + offset

            if np.isfinite(average[row]) and close[row] < average[row]:
                run_length[offset] = (run_length[offset - 1] + 1) if offset > 0 else 1
            else:
                run_length[offset] = 0

    next_exit[length] = length

    for offset in range(length - 1, -1, -1):
        if exit_flag(rule, first + offset, first, close, low, state, latest_low, run_length):
            next_exit[offset] = offset
        else:
            next_exit[offset] = next_exit[offset + 1]


@njit(cache=True)
def run_slice(rule, average, atr, open_price, high, low, close, starts, market_position, signal_ok, month_index, year_index,
              break_cumulative, deciles, state, latest_low, distance_edges, bounce_edges, bench, signal_start, market_count,
              listed_at_end, cost, position_krw, percent_edges, ratio_edges, stop_edges, buffer_size, cover, cover_from,
              high_table, low_table, group_statistics, group_years, pool_statistics, pool_years, pool_deciles, pool_stops, pool_histograms,
              pool_portfolio, month_count, day_offset):
    """한 (k, N, ATR 기간, 매도 규칙) 묶음의 모든 칸 매매. 결과는 넘겨받은 배열에 더한다.
    칸 = 거리 구간 × 반등 구간(상승 추세 140 칸, 하락 추세 140 칸) + (b) 상승 추세·저가가 이평 위.
    같은 칸·같은 종목 안에서 보유 중 신호는 무시, 청산 다음 행부터 새 신호를 받는다(next_allowed = 청산 행 + 1)."""
    groups = group_statistics.shape[0]
    half_groups = (groups - 1) // 2
    bounce_count = bounce_edges.shape[0] - 1
    month_sum = np.zeros((groups + 2, month_count))
    month_excess = np.zeros((groups + 2, month_count))
    month_n = np.zeros((groups + 2, month_count))
    buffer_net = np.empty(buffer_size)
    buffer_group = np.empty(buffer_size, dtype=np.int32)
    used = 0
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    next_allowed = np.zeros(groups, dtype=np.int64)
    last_exit = np.zeros(groups, dtype=np.int64)
    count_change = np.zeros((2, longest + 1))
    inverse_change = np.zeros((2, longest + 1))
    member = np.empty(2, dtype=np.int64)

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first
        fill_next_exit(rule, first, length, average, close, low, state, latest_low, run_length, next_exit)

        for group in range(groups):
            next_allowed[group] = 0
            last_exit[group] = -10

        for offset in range(length + 1):
            count_change[0, offset] = 0.0
            count_change[1, offset] = 0.0
            inverse_change[0, offset] = 0.0
            inverse_change[1, offset] = 0.0

        for offset in range(length):
            row = first + offset

            if not signal_ok[row]:
                continue

            trend = state[row]

            if trend != 1 and trend != -1:
                continue

            distance = np.nan

            if np.isfinite(average[row]) and np.isfinite(atr[row]) and atr[row] > 0:
                distance = (low[row] - average[row]) / atr[row]

            distance_index = distance_bin(distance, distance_edges)
            bounce_index = bounce_bin(close[row], low[row], high[row], bounce_edges)
            members = 0

            if distance_index >= 0 and bounce_index >= 0:
                cell = distance_index * bounce_count + bounce_index
                member[members] = cell if trend == 1 else cell + half_groups
                members += 1

            if trend == 1 and np.isfinite(distance) and distance > 0.0:
                member[members] = groups - 1
                members += 1

            if members == 0:
                continue

            exit_offset, exit_price = exit_of(rule, offset, length, next_exit, first, open_price, close, latest_low)
            entry = close[row]

            if exit_offset < length:
                exit_row = first + exit_offset
                reason = 1
            else:
                exit_row = last - 1
                reason = 2 if listed_at_end[stock] else 3
                exit_price = close[exit_row]

            net = (exit_price / entry * cost - 1.0) * 100.0
            hold = exit_row - row
            benchmark = bench[market_position[row] - signal_start, market_position[exit_row] - signal_start]
            excess = net - benchmark
            broken = break_cumulative[exit_row] - break_cumulative[row] > 0
            has_path = exit_row > row
            mae = np.nan
            mfe = np.nan
            peak_days = 0

            if has_path:
                peak_row = query_sparse(high_table, high, row + 1, exit_row, True)
                low_row = query_sparse(low_table, low, row + 1, exit_row, False)
                mae = (low[low_row] / entry - 1.0) * 100.0
                mfe = (high[peak_row] / entry - 1.0) * 100.0
                peak_days = exit_row - peak_row

            stop_line = latest_low[row] if rule <= 2 else average[row]
            stop_distance = (stop_line / entry - 1.0) * 100.0 if np.isfinite(stop_line) else np.nan
            month = month_index[row]
            year = year_index[row]

            for index in range(members):
                group = member[index]

                if offset == last_exit[group]:
                    group_statistics[group, 14] += 1.0

                if offset < next_allowed[group]:
                    continue

                if reason == 1:
                    next_allowed[group] = exit_offset + 1
                    last_exit[group] = exit_offset
                else:
                    next_allowed[group] = length + 1
                    last_exit[group] = -10

                add_trade(group_statistics[group], net, hold, has_path, mae, mfe, peak_days, benchmark, excess, reason, broken,
                          stop_distance)
                month_sum[group, month] += net
                month_excess[group, month] += excess
                month_n[group, month] += 1.0
                group_years[group, year] += 1
                buffer_net[used] = net
                buffer_group[used] = group
                used += 1

                if group >= half_groups:
                    continue

                if cover_from >= 0:
                    path = exit_row

                    while path >= row and market_position[path] >= cover_from:
                        cover[path] = 1
                        path -= 1

                distance_cell = group // bounce_count
                pools = 2 if (distance_cell >= 1 and distance_cell <= distance_edges.shape[0] - 1) else 1

                for pool in range(pools):
                    add_trade(pool_statistics[pool], net, hold, has_path, mae, mfe, peak_days, benchmark, excess, reason, broken,
                              stop_distance)
                    month_sum[groups + pool, month] += net
                    month_excess[groups + pool, month] += excess
                    month_n[groups + pool, month] += 1.0
                    pool_years[pool, year, 0] += 1.0
                    pool_years[pool, year, 1] += net
                    pool_years[pool, year, 2] += 1.0 if net > 0 else 0.0
                    pool_years[pool, year, 3] += excess
                    pool_years[pool, year, 4] += hold
                    pool_years[pool, year, 5] += 1.0 if broken else 0.0
                    pool_years[pool, year, 6] += net if broken else 0.0

                    for measure in range(3):
                        decile = deciles[row, measure]
                        slot = decile if decile >= 0 else 10
                        pool_deciles[pool, measure, slot, year, 0] += 1.0
                        pool_deciles[pool, measure, slot, year, 1] += net
                        pool_deciles[pool, measure, slot, year, 2] += 1.0 if net > 0 else 0.0
                        pool_deciles[pool, measure, slot, year, 3] += excess

                    if np.isfinite(stop_distance):
                        bucket = 0

                        while bucket < stop_edges.shape[0] and stop_distance < stop_edges[bucket]:
                            bucket += 1
                    else:
                        bucket = stop_edges.shape[0] + 1

                    pool_stops[pool, bucket, 0] += 1.0
                    pool_stops[pool, bucket, 1] += net
                    pool_stops[pool, bucket, 2] += 1.0 if net > 0 else 0.0
                    pool_histograms[pool, 0, percent_bin(net, percent_edges)] += 1
                    pool_histograms[pool, 3, day_offset + min(hold, 4499)] += 1

                    if has_path:
                        pool_histograms[pool, 1, percent_bin(mfe, percent_edges)] += 1
                        pool_histograms[pool, 5, day_offset + min(peak_days, 4499)] += 1

                        if mfe > 0:
                            pool_histograms[pool, 2, percent_bin(net / mfe, ratio_edges)] += 1

                    if np.isfinite(stop_distance):
                        pool_histograms[pool, 4, percent_bin(stop_distance, percent_edges)] += 1

                    # 포트폴리오: 매수 행부터 청산 전 행까지 종가 평가, 청산 행에 실현(미청산은 끝 행까지 평가만)
                    end_offset = exit_offset if reason != 2 else length
                    count_change[pool, offset] += 1.0
                    count_change[pool, end_offset] -= 1.0
                    inverse_change[pool, offset] += 1.0 / entry
                    inverse_change[pool, end_offset] -= 1.0 / entry

                    if reason != 2:
                        pool_portfolio[pool, 2, market_position[exit_row]] += net / 100.0 * position_krw

        for pool in range(2):
            holding = 0.0
            inverse = 0.0

            for offset in range(length):
                holding += count_change[pool, offset]
                inverse += inverse_change[pool, offset]

                if holding > 0.5:
                    row = first + offset
                    mark = (close[row] * inverse - holding) * position_krw
                    until = market_position[row + 1] if offset + 1 < length else market_position[row] + 1

                    for position in range(market_position[row], until):
                        pool_portfolio[pool, 0, position] += holding
                        pool_portfolio[pool, 1, position] += mark

    for group in range(groups):
        statistics = group_statistics[group]
        statistics[21] = cluster_t(month_sum[group], month_n[group], statistics[1], statistics[0])
        statistics[22] = cluster_t(month_excess[group], month_n[group], statistics[9], statistics[0])

    for pool in range(pool_statistics.shape[0]):
        statistics = pool_statistics[pool]
        statistics[21] = cluster_t(month_sum[groups + pool], month_n[groups + pool], statistics[1], statistics[0])
        statistics[22] = cluster_t(month_excess[groups + pool], month_n[groups + pool], statistics[9], statistics[0])

    counts = np.zeros(groups + 1, dtype=np.int64)

    for index in range(used):
        counts[buffer_group[index] + 1] += 1

    offsets = np.cumsum(counts)
    place = offsets[:-1].copy()
    ordered = np.empty(used)

    for index in range(used):
        group = buffer_group[index]
        ordered[place[group]] = buffer_net[index]
        place[group] += 1

    for group in range(groups):
        size = offsets[group + 1] - offsets[group]

        if size == 0:
            group_statistics[group, 23] = np.nan
            continue

        part = np.sort(ordered[offsets[group]:offsets[group + 1]])

        if size % 2 == 1:
            group_statistics[group, 23] = part[size // 2]
        else:
            group_statistics[group, 23] = 0.5 * (part[size // 2 - 1] + part[size // 2])

    return used


@njit(parallel=True, cache=True)
def run_span(slice_window, slice_period, slice_rule, averages, atrs, open_price, high, low, close, starts, market_position,
             signal_ok, month_index, year_index, break_cumulative, deciles, state, latest_low, distance_edges, bounce_edges,
             bench, signal_start, market_count, listed_at_end, cost, position_krw, percent_edges, ratio_edges, stop_edges,
             buffer_size, cover, cover_from, high_table, low_table, groups, year_count, month_count, histogram_width, day_offset):
    """한 k 의 모든 (N, ATR 기간, 매도 규칙) 묶음을 병렬로 돈다."""
    slices = slice_window.shape[0]
    group_statistics = np.zeros((slices, groups, 24))
    group_years = np.zeros((slices, groups, year_count), dtype=np.int32)
    pool_statistics = np.zeros((slices, 2, 24))
    pool_years = np.zeros((slices, 2, year_count, 7))
    pool_deciles = np.zeros((slices, 2, 3, 11, year_count, 4))
    pool_stops = np.zeros((slices, 2, stop_edges.shape[0] + 2, 3))
    pool_histograms = np.zeros((slices, 2, 6, histogram_width), dtype=np.int32)
    pool_portfolio = np.zeros((slices, 2, 3, market_count))
    used = np.zeros(slices, dtype=np.int64)

    for index in prange(slices):
        used[index] = run_slice(slice_rule[index], averages[slice_window[index]], atrs[slice_period[index]], open_price, high, low,
                                close, starts, market_position, signal_ok, month_index, year_index, break_cumulative, deciles,
                                state, latest_low, distance_edges, bounce_edges, bench, signal_start, market_count, listed_at_end,
                                cost, position_krw, percent_edges, ratio_edges, stop_edges, buffer_size, cover, cover_from,
                                high_table, low_table, group_statistics[index], group_years[index], pool_statistics[index],
                                pool_years[index], pool_deciles[index], pool_stops[index], pool_histograms[index],
                                pool_portfolio[index], month_count, day_offset)

    return group_statistics, group_years, pool_statistics, pool_years, pool_deciles, pool_stops, pool_histograms, pool_portfolio, used


def histogram_width() -> int:
    return max(PERCENT_BINS, RATIO_BINS, len(PERCENT_EDGES) + 1 + HOLD_BINS)


def histogram_quantile(counts: np.ndarray, kind: str, quantile: float):
    """구간 셈에서 분위수(구간 가운데 값). kind: percent(수익·MFE·손절 거리), ratio(실현÷MFE), days(보유·최고점→매도 일수)."""
    total = counts.sum()

    if total <= 0:
        return None

    position = np.searchsorted(np.cumsum(counts), quantile * total, side="left")

    if kind == "days":
        offset = len(PERCENT_EDGES) + 1
        return float(position - offset)

    edges = PERCENT_EDGES if kind == "percent" else RATIO_EDGES

    if position == 0:
        return float(edges[0])

    if position >= len(edges):
        return float(edges[-1])

    return float(0.5 * (edges[position - 1] + edges[position]))


def histogram_mean_days(counts: np.ndarray) -> float | None:
    offset = len(PERCENT_EDGES) + 1
    days = np.arange(len(counts)) - offset
    usable = days >= 0
    total = counts[usable].sum()
    return float((counts[usable] * days[usable]).sum() / total) if total else None


# ── 순수 파이썬 기준 구현(검증용 — 커널과 다른 경로) ─────────────────────────

def reference_trades(panel: Panel, stock: int, span: int, window: int, period: int, distance_index: int, bounce_index: int,
                     rule: str, trend_wanted: int = 1, pivots=None) -> list[dict]:
    """한 종목·한 조합의 매매를 한 줄씩 다시 계산한다(커널과 독립된 경로, 느림)."""
    first, last = int(panel.starts[stock]), int(panel.starts[stock + 1])
    series = panel.stocks[stock]
    count = last - first
    pivots = pivots if pivots is not None else swing.confirmed_pivots(series.low, series.high, span)
    trend = swing.trend_state(pivots, count)
    state = trend["state"]
    latest = trend["latest_low"]
    close = series.close.astype(float)
    average = swing.moving_average(close, window)
    true_range = panel.true_range[first:last]
    atr = np.full(count, np.nan)

    for offset in range(period, count):
        atr[offset] = sum(true_range[offset - period:offset]) / period

    rule_index = RULE_NAMES.index(rule)
    days = rule_index - 2 if rule_index >= 3 else 0
    trades = []
    allowed = 0
    sequence = 0

    for offset in range(count):
        row = first + offset

        if not panel.signal_ok[row] or state[offset] != trend_wanted or offset < allowed:
            continue

        if not (np.isfinite(average[offset]) and np.isfinite(atr[offset]) and atr[offset] > 0 and series.high[offset] > series.low[offset]):
            continue

        distance = (series.low[offset] - average[offset]) / atr[offset]
        bounce = (close[offset] - series.low[offset]) / (series.high[offset] - series.low[offset])

        if int(np.searchsorted(DISTANCE_EDGES, distance, side="right")) != distance_index:
            continue

        if min(max(int(np.searchsorted(BOUNCE_EDGES, bounce, side="right")) - 1, 0), BOUNCE_BINS - 1) != bounce_index:
            continue

        exit_offset = None
        exit_price = None
        run = 0

        for later in range(offset + 1, count):
            level = latest[later - 1]

            if rule == "E1" and np.isfinite(level) and close[later] < level:
                exit_offset, exit_price = later, close[later]
            elif rule == "E2" and np.isfinite(level) and series.low[later] < level:
                exit_offset, exit_price = later, min(series.open_price[later], level)
            elif rule == "E3" and state[later] == swing.TREND_DOWN:
                exit_offset, exit_price = later, close[later]
            elif rule.startswith("E4"):
                run = run + 1 if (np.isfinite(average[later]) and close[later] < average[later]) else 0

                if run >= days:
                    exit_offset, exit_price = later, close[later]

            if exit_offset is not None:
                break

        if exit_offset is None:
            exit_offset = count - 1
            exit_price = close[-1]
            reason = REASON_OPEN if panel.listed_at_end[stock] else REASON_END
            allowed = count + 1
        else:
            reason = REASON_RULE
            allowed = exit_offset + 1

        sequence += 1
        net = (exit_price / close[offset] * COST - 1.0) * 100.0
        trades.append({"code": panel.codes[stock], "entry_date": int(series.dates[offset]), "entry_price": float(close[offset]),
                       "exit_date": int(series.dates[exit_offset]), "exit_price": float(exit_price), "reason": reason,
                       "hold_days": exit_offset - offset, "net": float(net), "sequence": sequence})

    return trades
