"""스터디 36 — 업종 묶음 모멘텀(회의 2 B안, 사용자 결정 2026-10-02). 규칙 정본은 같은 폴더 SPEC.md.

매주 마지막 거래일 종가까지의 정보로
  1) 대상 종목 = 보통주, 종가 × f ≥ 2,000원, 60일 평균 시장 거래대금 비중 ≥ 0.02%, 250일 수익 200일 이상, 20일 전 종가 있음
  2) 250일 일간 수익 상관으로 Ward 계층 군집 → 나무 잎 순서로 10개씩 끊어 묶음
  3) 묶음 20일 수익 평균 상위 10% 묶음의 20일 수익 1위 종목을 다음 거래일 시가 + 1틱에 산다.
청산 32칸 = (W 주간 교체 | P 최대 12주) × 손절 {5, 8, 10, 없음} × 익절 {10, 15, 20, 없음}.
비교: B0 대상 동일가중(비용 없음, 초과수익 기준), B1 개별 20일 상위 10%, B2 개별 상위 같은 개수.

재실행(저장소 루트): py -X utf8 research/studies/36_cluster_momentum/backtest.py
산출: grid.tsv, yearly.tsv, mae_mfe.tsv, metrics.json, result.txt, (gitignore) trades.parquet, nav.parquet, picks.parquet
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
REPO = STUDY.parents[2]
_specification = importlib.util.spec_from_file_location("study33_backtest", STUDY.parent / "33_post_surge_pullback/backtest.py")
study33 = importlib.util.module_from_spec(_specification)
sys.modules["study33_backtest"] = study33
_specification.loader.exec_module(study33)
from backtest.costs import LIVE   # noqa: E402  study33 import 가 PYQuant 를 sys.path 에 넣는다

LOAD_FROM = pd.Timestamp("2008-11-01")
LAST_DATE = 20261001
FIRST_REBALANCE_YEAR = 2010
MARKET_DAY_MIN_STOCKS = 100
MIN_RAW_PRICE = 2_000.0
SHARE_MIN = 0.0002            # 60일 평균 시장 거래대금 비중 0.02%(내가 정함)
SHARE_DAYS = 60
CORRELATION_DAYS = 250
CORRELATION_MIN_DAYS = 200
RETURN_CLIP = 0.30
MOMENTUM_DAYS = 20
GROUP_SIZE = 10
TOP_FRACTION = 0.10
SLEEVES = 12                  # P 갈래: 12칸 돌려 쓰기 = 최대 12주 보유
STOPS = [5, 8, 10, None]      # 축 순서 = 이웃 판정 순서
TAKES = [10, 15, 20, None]
FAMILIES = ["W", "P"]
COMMISSION = LIVE.commission_percent / 100.0
SELL_TAX = LIVE.sell_tax_percent / 100.0
SELL_TAX_SENSITIVITY = 0.0030
PASS_T = 3.0
PASS_MDD = 15.0
PASS_TRADES = 200
SPLIT_YEAR = 2018             # 2010–2017 / 2018–2026
PRICE_EPSILON = 1e-6


# ── 시세 행렬 ───────────────────────────────────────────────────────────────

@dataclass
class Market:
    dates: np.ndarray          # int YYYYMMDD, 시장 달력
    codes: list
    names: list
    open_price: np.ndarray     # (날, 종목), 거래 안 한 날 NaN
    high: np.ndarray
    low: np.ndarray
    close: np.ndarray
    close_filled: np.ndarray   # 앞 값으로 채운 종가(상장 전 NaN)
    factor: np.ndarray         # 무수정 ÷ 수정
    is_kospi: np.ndarray
    share_average: np.ndarray  # 60일 평균 시장 거래대금 비중
    returns: np.ndarray        # 일간 수익(이어진 날만, ±30% 자름)
    return_counts: np.ndarray  # 누적 유효 수익 개수(행 0 = 0개)
    last_traded_row: np.ndarray
    delisted: np.ndarray


def forward_fill(matrix: np.ndarray) -> np.ndarray:
    rows = np.where(np.isfinite(matrix), np.arange(matrix.shape[0])[:, None], 0)
    np.maximum.accumulate(rows, axis=0, out=rows)
    filled = matrix[rows, np.arange(matrix.shape[1])[None, :]]
    started = np.maximum.accumulate(np.isfinite(matrix), axis=0)
    filled[~started] = np.nan
    return filled


def load_market() -> tuple[Market, dict]:
    study33.LOAD_FROM = LOAD_FROM
    stocks, _, information = study33.load_inputs(LAST_DATE)
    counts: dict[int, int] = {}

    for series in stocks:
        for date_int in series.dates:
            counts[int(date_int)] = counts.get(int(date_int), 0) + 1

    dates = np.array(sorted(date for date, count in counts.items() if count >= MARKET_DAY_MIN_STOCKS), dtype=np.int64)
    day_count, stock_count = len(dates), len(stocks)
    shape = (day_count, stock_count)
    open_price, high, low, close = (np.full(shape, np.nan) for _ in range(4))
    factor = np.ones(shape, dtype=np.float32)
    is_kospi = np.zeros(shape, dtype=bool)
    turnover = np.zeros(shape, dtype=np.float64)
    delisted = np.zeros(stock_count, dtype=bool)
    codes, names = [], []

    for column, series in enumerate(stocks):
        positions = np.searchsorted(dates, series.dates)
        inside = (positions < day_count)
        inside[inside] = dates[positions[inside]] == series.dates[inside]
        rows = positions[inside]
        open_price[rows, column] = series.open_price[inside]
        high[rows, column] = series.high[inside]
        low[rows, column] = series.low[inside]
        close[rows, column] = series.close[inside]
        factor[:, column] = series.factor[0] if len(series.factor) else 1.0
        factor[rows, column] = series.factor[inside]
        is_kospi[rows, column] = series.is_kospi[inside]
        turnover[rows, column] = series.close[inside] * series.factor[inside] * series.volume[inside]
        delisted[column] = series.delisted
        codes.append(series.code)
        names.append(series.name)

    del stocks
    # f 는 거래한 날 사이에서 앞 값으로 잇는다(정지일 호가 단위용).
    factor_filled = np.where(np.isfinite(close), factor, np.nan)
    factor = np.nan_to_num(forward_fill(factor_filled), nan=1.0).astype(np.float32)
    total = turnover.sum(axis=1, keepdims=True)
    share = (turnover / total).astype(np.float64)
    del turnover
    cumulative = np.cumsum(np.vstack([np.zeros((1, stock_count)), share]), axis=0)
    share_average = np.full(shape, np.nan, dtype=np.float32)
    share_average[SHARE_DAYS - 1:] = (cumulative[SHARE_DAYS:] - cumulative[:-SHARE_DAYS]) / SHARE_DAYS
    del cumulative, share
    returns = np.full(shape, np.nan, dtype=np.float32)
    returns[1:] = np.clip(close[1:] / close[:-1] - 1.0, -RETURN_CLIP, RETURN_CLIP)
    return_counts = np.vstack([np.zeros((1, stock_count), dtype=np.int32),
                               np.cumsum(np.isfinite(returns), axis=0, dtype=np.int32)])
    traded = np.isfinite(close)
    last_traded_row = np.where(traded.any(axis=0), day_count - 1 - np.argmax(traded[::-1], axis=0), -1)
    market = Market(dates, codes, names, open_price, high, low, close, forward_fill(close), factor, is_kospi,
                    share_average, returns, return_counts, last_traded_row, delisted)
    information["calendar_days"] = int(day_count)
    information["stocks"] = int(stock_count)
    return market, information


def tick(market: Market, price: float, row: int, column: int, below: bool) -> float:
    factor = float(market.factor[row, column])
    raw = price * factor - (PRICE_EPSILON if below else 0.0)
    return study33.raw_tick_size(raw, bool(market.is_kospi[row, column]), int(market.dates[row])) / factor


def buy_fill(market: Market, price: float, row: int, column: int) -> float:
    return price + tick(market, price, row, column, False)


def sell_fill(market: Market, price: float, row: int, column: int) -> float:
    unit = tick(market, price, row, column, True)
    return max(price - unit, unit * 0.5)


# ── Ward 계층 군집(최근접 이웃 사슬) ──────────────────────────────────────────

def ward_leaf_order(points: np.ndarray) -> list[int]:
    """행 = 종목 벡터. 끝까지 합친 Ward 나무의 잎 순서. 왼쪽 가지 = 원래 번호가 가장 작은 잎이 든 쪽."""
    count = len(points)

    if count == 1:
        return [0]

    gram = points @ points.T
    squared = np.diag(gram).copy()
    distance = squared[:, None] + squared[None, :] - 2.0 * gram
    np.maximum(distance, 0.0, out=distance)
    np.fill_diagonal(distance, np.inf)
    size = np.ones(count)
    active = np.ones(count, dtype=bool)
    node_of_slot = list(range(count))
    smallest_leaf = list(range(count))
    children: dict[int, tuple[int, int]] = {}
    next_node = count
    chain: list[int] = []
    remaining = count

    while remaining > 1:
        if not chain:
            chain.append(int(np.flatnonzero(active)[0]))

        current = chain[-1]
        row = distance[current]
        nearest = int(np.argmin(row))

        if len(chain) >= 2 and row[chain[-2]] <= row[nearest]:
            nearest = chain[-2]

        if len(chain) >= 2 and nearest == chain[-2]:
            chain.pop()
            chain.pop()
            keep, drop = min(current, nearest), max(current, nearest)
            keep_size, drop_size = size[keep], size[drop]
            pair_distance = distance[keep, drop]
            with np.errstate(invalid="ignore"):
                merged = ((keep_size + size) * distance[keep] + (drop_size + size) * distance[drop]
                          - size * pair_distance) / (keep_size + drop_size + size)
            merged[~active] = np.inf
            merged[keep] = np.inf
            merged[drop] = np.inf
            distance[keep, :] = merged
            distance[:, keep] = merged
            distance[drop, :] = np.inf
            distance[:, drop] = np.inf
            active[drop] = False
            size[keep] = keep_size + drop_size
            node_a, node_b = node_of_slot[keep], node_of_slot[drop]
            left, right = (node_a, node_b) if smallest_leaf[node_a] < smallest_leaf[node_b] else (node_b, node_a)
            children[next_node] = (left, right)
            smallest_leaf.append(min(smallest_leaf[node_a], smallest_leaf[node_b]))
            node_of_slot[keep] = next_node
            next_node += 1
            remaining -= 1

        else:
            chain.append(nearest)

    order: list[int] = []
    stack = [next_node - 1]

    while stack:
        node = stack.pop()

        if node < count:
            order.append(node)

        else:
            left, right = children[node]
            stack.append(right)
            stack.append(left)

    return order


# ── 주간 선택 ───────────────────────────────────────────────────────────────

def rebalance_rows(market: Market) -> np.ndarray:
    stamps = pd.to_datetime(market.dates.astype(str))
    calendar = stamps.isocalendar()
    keys = (calendar["year"].to_numpy() * 100 + calendar["week"].to_numpy())
    last_of_week = np.r_[keys[1:] != keys[:-1], True]
    rows = np.flatnonzero(last_of_week)
    rows = rows[(market.dates[rows] // 10000 >= FIRST_REBALANCE_YEAR) & (rows + 1 <= len(market.dates) - 1)]
    return rows


def select_week(market: Market, row: int) -> dict:
    raw_price = market.close[row] * market.factor[row]
    window_count = market.return_counts[row + 1] - market.return_counts[row + 1 - CORRELATION_DAYS]
    past_close = market.close_filled[row - MOMENTUM_DAYS]
    eligible = (np.isfinite(market.close[row]) & (raw_price >= MIN_RAW_PRICE) & (market.share_average[row] >= SHARE_MIN)
                & (window_count >= CORRELATION_MIN_DAYS) & np.isfinite(past_close) & (past_close > 0))
    universe = np.flatnonzero(eligible)
    momentum = market.close[row, universe] / past_close[universe] - 1.0
    window = market.returns[row + 1 - CORRELATION_DAYS:row + 1, universe].astype(np.float64)
    valid = np.isfinite(window)
    mean = np.nanmean(window, axis=0)
    deviation = np.nanstd(window, axis=0, ddof=1)
    deviation[deviation <= 0] = np.inf
    standardized = np.where(valid, (window - mean) / deviation, 0.0) / math.sqrt(CORRELATION_DAYS)
    order = ward_leaf_order(standardized.T)
    group_count = len(universe) // GROUP_SIZE
    groups = [order[index * GROUP_SIZE:(index + 1) * GROUP_SIZE] for index in range(group_count - 1)]
    groups.append(order[(group_count - 1) * GROUP_SIZE:])
    scores = np.array([momentum[group].mean() for group in groups])
    chosen = int(math.ceil(group_count * TOP_FRACTION))
    ranking = sorted(range(group_count), key=lambda index: (-scores[index], index))[:chosen]
    leader, liquid = [], []

    for group_index in ranking:
        members = groups[group_index]
        leader.append(int(universe[min(members, key=lambda member: (-momentum[member], member))]))
        liquid.append(int(universe[min(members, key=lambda member: (-market.share_average[row, universe[member]], member))]))

    individual = sorted(range(len(universe)), key=lambda member: (-momentum[member], member))
    top_tenth = [int(universe[member]) for member in individual[:int(math.ceil(len(universe) * TOP_FRACTION))]]
    same_count = [int(universe[member]) for member in individual[:chosen]]
    return {"universe": universe.astype(int).tolist(), "cluster": leader, "cluster_liquid": liquid, "B1": top_tenth,
            "B2": same_count, "group_count": group_count, "group_scores": [float(scores[index]) for index in ranking]}


# ── 매매 한 건 ──────────────────────────────────────────────────────────────

@dataclass
class Trade:
    column: int
    entry_row: int
    entry: float
    exit_row: int              # 이 행에 판 돈이 현금으로 들어온다. 끝까지 보유면 날 수(=행 밖)
    exit_price: float
    reason: str                # stop, take, date, delist, open
    exit_at_open: bool
    lowest: float
    highest: float
    lowest_before_peak: float
    sleeve: int = 0
    week: int = 0
    amount: float = 0.0
    quantity: float = 0.0


def run_trade(market: Market, column: int, entry_row: int, forced_row: int | None, stop: int | None, take: int | None,
              sell_tax: float) -> Trade | None:
    opening = market.open_price[entry_row, column]

    if not np.isfinite(opening):
        return None

    entry = buy_fill(market, opening, entry_row, column)
    day_count = len(market.dates)
    last_row = int(market.last_traded_row[column])
    sale_row = None

    if forced_row is not None:
        candidates = np.flatnonzero(np.isfinite(market.open_price[forced_row:, column]))
        sale_row = forced_row + int(candidates[0]) if len(candidates) else None

    check_end = (sale_row - 1) if sale_row is not None else min(last_row, day_count - 1)
    lows = market.low[entry_row:check_end + 1, column]
    highs = market.high[entry_row:check_end + 1, column]
    opens = market.open_price[entry_row:check_end + 1, column]
    stop_level = entry * (1 - stop / 100) if stop else -math.inf
    take_level = entry * (1 + take / 100) if take else math.inf
    stop_hits = np.flatnonzero(lows <= stop_level) if stop else np.array([], dtype=int)
    take_hits = np.flatnonzero(highs >= take_level) if take else np.array([], dtype=int)
    stop_day = int(stop_hits[0]) if len(stop_hits) else None
    take_day = int(take_hits[0]) if len(take_hits) else None
    reason, exit_row, raw_exit, at_open = None, None, None, False

    if stop_day is not None and (take_day is None or stop_day <= take_day):
        exit_row = entry_row + stop_day
        raw_exit = stop_level if stop_day == 0 else min(opens[stop_day], stop_level)
        reason = "stop"

    elif take_day is not None:
        exit_row = entry_row + take_day
        raw_exit = take_level if take_day == 0 else max(opens[take_day], take_level)
        reason = "take"

    elif sale_row is not None:
        exit_row, raw_exit, reason, at_open = sale_row, market.open_price[sale_row, column], "date", True

    elif last_row < day_count - 1:
        exit_row, raw_exit, reason = last_row, market.close[last_row, column], "delist"

    if reason is None:
        exit_row, exit_price, reason = day_count, market.close_filled[day_count - 1, column], "open"

    else:
        exit_price = sell_fill(market, raw_exit, exit_row, column)

    span_end = min(exit_row, day_count) if reason in ("date", "open") else exit_row + 1
    span_lows = market.low[entry_row:span_end, column]
    span_highs = market.high[entry_row:span_end, column]

    if reason in ("stop", "take"):
        span_lows = span_lows.copy()
        span_highs = span_highs.copy()
        span_lows[-1] = min(span_lows[-1], raw_exit) if np.isfinite(span_lows[-1]) else raw_exit
        span_highs[-1] = raw_exit if reason == "stop" else min(span_highs[-1], raw_exit)

    if len(span_lows) == 0 or not np.isfinite(span_lows).any():
        lowest = highest = before_peak = 0.0

    else:
        peak = int(np.nanargmax(span_highs))
        lowest = float(np.nanmin(span_lows) / entry - 1.0)
        highest = float(np.nanmax(span_highs) / entry - 1.0)
        before_peak = float(np.nanmin(span_lows[:peak + 1]) / entry - 1.0)

    return Trade(column, entry_row, entry, exit_row, float(exit_price), reason, at_open, lowest, highest, before_peak)


def net_return(trade: Trade, sell_tax: float) -> float:
    return trade.exit_price * (1 - COMMISSION - sell_tax) / (trade.entry * (1 + COMMISSION)) - 1.0


# ── 포트폴리오 ──────────────────────────────────────────────────────────────

def generate_trades(market: Market, picks: list[list[int]], execution: np.ndarray, family: str, stop, take,
                    sell_tax: float) -> list[list[Trade]]:
    """주마다 새로 시작하는 매매 목록. W 는 이미 들고 있으면 새로 안 산다(가격 청산 뒤면 다시 산다)."""
    weeks = len(picks)
    started: list[list[Trade]] = [[] for _ in range(weeks)]

    if family == "P":
        for week in range(weeks):
            forced = int(execution[week + SLEEVES]) if week + SLEEVES < weeks else None

            for column in picks[week]:
                trade = run_trade(market, column, int(execution[week]), forced, stop, take, sell_tax)

                if trade is not None:
                    trade.week, trade.sleeve = week, week % SLEEVES
                    started[week].append(trade)

        return started

    picked_weeks: dict[int, list[int]] = {}

    for week, columns in enumerate(picks):
        for column in columns:
            picked_weeks.setdefault(column, []).append(week)

    rank_of = [{column: rank for rank, column in enumerate(columns)} for columns in picks]

    for column, weeks_picked in picked_weeks.items():
        picked = set(weeks_picked)
        held_until = -1

        for week in weeks_picked:
            row = int(execution[week])

            if held_until >= row:
                continue

            forced_week = week + 1

            while forced_week < weeks and forced_week in picked:
                forced_week += 1

            forced = int(execution[forced_week]) if forced_week < weeks else None
            trade = run_trade(market, column, row, forced, stop, take, sell_tax)

            if trade is None:
                continue

            trade.week = week
            held_until = trade.exit_row
            started[week].append(trade)

    for week in range(weeks):
        started[week].sort(key=lambda trade: rank_of[week][trade.column])

    return started


def open_value(market: Market, trade: Trade, row: int) -> float:
    price = market.open_price[row, trade.column]

    if not np.isfinite(price):
        price = market.close_filled[row - 1, trade.column]

    return trade.quantity * price


def simulate(market: Market, picks: list[list[int]], execution: np.ndarray, family: str, stop, take,
             sell_tax: float = SELL_TAX) -> tuple[np.ndarray, list[Trade]]:
    """일별 종가 평가액(첫 실행일 전 = 1)과 실제로 산 매매 목록."""
    started = generate_trades(market, picks, execution, family, stop, take, sell_tax)
    day_count = len(market.dates)
    cash_events = np.zeros(day_count + 1)
    holding_value = np.zeros(day_count)
    pockets = [1.0] if family == "W" else [1.0 / SLEEVES] * SLEEVES
    open_trades: list[Trade] = []
    done: list[Trade] = []

    for week, trades in enumerate(started):
        row = int(execution[week])
        still_open = []

        for trade in open_trades:
            if trade.exit_row < row or (trade.exit_row == row and trade.exit_at_open):
                pockets[trade.sleeve] += trade.quantity * trade.exit_price * (1 - COMMISSION - sell_tax)

            else:
                still_open.append(trade)

        open_trades = still_open
        pocket = 0 if family == "W" else week % SLEEVES

        if family == "W":
            value = pockets[0] + sum(open_value(market, trade, row) for trade in open_trades
                                     if not (trade.exit_row == row and trade.exit_at_open))
            target = value / max(len(picks[week]), 1)

        else:
            target = pockets[pocket] / max(len(picks[week]), 1)

        for trade in trades:
            amount = min(target, pockets[pocket])

            if amount <= 1e-12:
                continue

            pockets[pocket] -= amount
            trade.sleeve = pocket
            trade.amount = amount
            trade.quantity = amount / (trade.entry * (1 + COMMISSION))
            cash_events[row] -= amount

            if trade.exit_row < day_count:
                cash_events[trade.exit_row] += trade.quantity * trade.exit_price * (1 - COMMISSION - sell_tax)

            end = min(trade.exit_row, day_count)
            holding_value[row:end] += trade.quantity * market.close_filled[row:end, trade.column]
            open_trades.append(trade)
            done.append(trade)

    cash = 1.0 + np.cumsum(cash_events[:day_count])
    return cash + holding_value, done


def equal_weight_benchmark(market: Market, universes: list[list[int]], execution: np.ndarray) -> np.ndarray:
    day_count = len(market.dates)
    value = np.ones(day_count)
    level = 1.0

    for week, members in enumerate(universes):
        start = int(execution[week])
        end = int(execution[week + 1]) if week + 1 < len(universes) else day_count
        columns = np.array(members, dtype=int)
        base = market.open_price[start, columns]
        columns, base = columns[np.isfinite(base)], base[np.isfinite(base)]
        relative = market.close_filled[start:end, columns] / base
        value[start:end] = level * relative.mean(axis=1)

        if end < day_count:
            exit_open = market.open_price[end, columns]
            exit_open = np.where(np.isfinite(exit_open), exit_open, market.close_filled[end - 1, columns])
            level *= float((exit_open / base).mean())

    return value


# ── 성과 ────────────────────────────────────────────────────────────────────

def t_value(values) -> float:
    values = np.asarray(values, dtype=float)
    values = values[np.isfinite(values)]

    if len(values) < 2 or values.std(ddof=1) == 0:
        return float("nan")

    return float(values.mean() / (values.std(ddof=1) / math.sqrt(len(values))))


def month_returns(market: Market, value: np.ndarray, first_row: int) -> pd.Series:
    months = market.dates // 100
    month_end = np.flatnonzero(np.r_[months[1:] != months[:-1], True])
    month_end = month_end[month_end >= first_row]
    levels = np.r_[value[first_row - 1], value[month_end]]
    return pd.Series(levels[1:] / levels[:-1] - 1.0, index=months[month_end])


def max_drawdown(value: np.ndarray) -> float:
    peak = np.maximum.accumulate(value)
    return float((1.0 - value / peak).max() * 100.0)


def cell_metrics(market: Market, value: np.ndarray, trades: list[Trade], first_row: int, base_months: pd.Series,
                 sell_tax: float) -> tuple[dict, pd.Series]:
    months = month_returns(market, value, first_row)
    excess = months - base_months
    years = (pd.Timestamp(str(market.dates[-1])) - pd.Timestamp(str(market.dates[first_row]))).days / 365.25
    closed = [trade for trade in trades if trade.reason != "open"]
    nets = np.array([net_return(trade, sell_tax) for trade in closed]) * 100.0
    early = excess[excess.index // 100 < SPLIT_YEAR]
    late = excess[excess.index // 100 >= SPLIT_YEAR]
    reasons = pd.Series([trade.reason for trade in trades]).value_counts().to_dict() if trades else {}
    metrics = {
        "cagr_pct": ((value[-1] / value[first_row - 1]) ** (1 / years) - 1) * 100.0,
        "mdd_pct": max_drawdown(value[first_row - 1:]),
        "excess_month_mean_pct": excess.mean() * 100.0, "excess_t": t_value(excess),
        "excess_2010_2017_pct": early.mean() * 100.0, "excess_2018_2026_pct": late.mean() * 100.0,
        "trades": len(closed), "open_at_end": len(trades) - len(closed),
        "win_rate_pct": float((nets > 0).mean() * 100.0) if len(nets) else float("nan"),
        "net_mean_pct": float(nets.mean()) if len(nets) else float("nan"),
        "net_median_pct": float(np.median(nets)) if len(nets) else float("nan"),
        "trade_t": t_value(nets),
        "stop_exits": reasons.get("stop", 0), "take_exits": reasons.get("take", 0), "date_exits": reasons.get("date", 0),
        "delist_exits": reasons.get("delist", 0)}
    return metrics, months


def yearly_returns(market: Market, value: np.ndarray, first_row: int) -> pd.Series:
    years = market.dates // 10000
    year_end = np.flatnonzero(np.r_[years[1:] != years[:-1], True])
    year_end = year_end[year_end >= first_row]
    levels = np.r_[value[first_row - 1], value[year_end]]
    return pd.Series(levels[1:] / levels[:-1] - 1.0, index=years[year_end])


def label(stop, take) -> str:
    return f"손절{stop if stop else '없음'}·익절{take if take else '없음'}"


def neighbors(stop, take) -> list[tuple]:
    stop_index, take_index = STOPS.index(stop), TAKES.index(take)
    result = []

    for delta in (-1, 1):
        if 0 <= stop_index + delta < len(STOPS):
            result.append((STOPS[stop_index + delta], take))

        if 0 <= take_index + delta < len(TAKES):
            result.append((stop, TAKES[take_index + delta]))

    return result


def distribution(values: np.ndarray) -> dict:
    values = values * 100.0
    return {f"p{quantile}": float(np.percentile(values, quantile)) for quantile in (10, 25, 50, 75, 90)}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()

    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)

    return digest.hexdigest()


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()

    except Exception:
        return "unknown"


def write_tsv(frame: pd.DataFrame, path: Path) -> None:
    path.write_bytes(frame.to_csv(sep="\t", index=False, float_format="%.4f", lineterminator="\n").encode("utf-8"))


def clean(value):
    if isinstance(value, dict):
        return {str(key): clean(item) for key, item in value.items()}

    if isinstance(value, (list, tuple)):
        return [clean(item) for item in value]

    if isinstance(value, (np.floating, float)):
        return None if (math.isnan(value) or math.isinf(value)) else round(float(value), 6)

    if isinstance(value, np.integer):
        return int(value)

    if isinstance(value, np.bool_):
        return bool(value)

    return value


# ── 실행 ────────────────────────────────────────────────────────────────────

def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    started_at = time.time()
    market, information = load_market()
    print(f"시세 {information['calendar_days']}일 × {information['stocks']}종목, {time.time() - started_at:.0f}초")
    rows = rebalance_rows(market)
    execution = rows + 1
    weeks = [select_week(market, int(row)) for row in rows]
    print(f"주 {len(weeks)}개 선택 끝, {time.time() - started_at:.0f}초")
    first_row = int(execution[0])
    pick_records = [{"rebalance": int(market.dates[row]), "execution": int(market.dates[execution[index]]),
                     "universe": len(week["universe"]), "groups": week["group_count"],
                     "cluster": ",".join(market.codes[column] for column in week["cluster"]),
                     "cluster_liquid": ",".join(market.codes[column] for column in week["cluster_liquid"]),
                     "B1_count": len(week["B1"]), "B2": ",".join(market.codes[column] for column in week["B2"]),
                     "group_scores": ",".join(f"{score:.4f}" for score in week["group_scores"])}
                    for index, (row, week) in enumerate(zip(rows, weeks))]
    pd.DataFrame(pick_records).to_parquet(STUDY / "picks.parquet", index=False)
    benchmark = equal_weight_benchmark(market, [week["universe"] for week in weeks], execution)
    base_months = month_returns(market, benchmark, first_row)
    portfolios = {"cluster": [week["cluster"] for week in weeks], "B1": [week["B1"] for week in weeks],
                  "B2": [week["B2"] for week in weeks]}
    grid, months_by_cell, values, trade_rows = [], {}, {"B0": benchmark}, []

    for portfolio, picks in portfolios.items():
        for family in FAMILIES:
            for stop in STOPS:
                for take in TAKES:
                    value, trades = simulate(market, picks, execution, family, stop, take)
                    metrics, months = cell_metrics(market, value, trades, first_row, base_months, SELL_TAX)
                    key = (portfolio, family, stop, take)
                    months_by_cell[key] = months
                    grid.append({"portfolio": portfolio, "family": family, "stop": stop or 0, "take": take or 0,
                                 "cell": label(stop, take), **metrics})

                    if portfolio == "cluster":
                        values[f"{family}_{label(stop, take)}"] = value

                    if portfolio == "cluster" or (stop is None and take is None):
                        for trade in trades:
                            trade_rows.append({"portfolio": portfolio, "family": family, "stop": stop or 0, "take": take or 0,
                                               "code": market.codes[trade.column], "name": market.names[trade.column],
                                               "entry_date": int(market.dates[trade.entry_row]),
                                               "exit_date": int(market.dates[trade.exit_row]) if trade.exit_row < len(market.dates) else None,
                                               "entry": trade.entry, "exit": trade.exit_price, "reason": trade.reason,
                                               "net_pct": net_return(trade, SELL_TAX) * 100.0, "mae_pct": trade.lowest * 100.0,
                                               "mfe_pct": trade.highest * 100.0, "mae_before_peak_pct": trade.lowest_before_peak * 100.0,
                                               "amount": trade.amount, "sleeve": trade.sleeve})

            print(f"{portfolio} {family} 끝, {time.time() - started_at:.0f}초")

    grid_frame = pd.DataFrame(grid)

    # 비교 기준 대비 월 수익 차, 판정
    for index, record in grid_frame.iterrows():
        if record["portfolio"] != "cluster":
            continue

        stop, take = (record["stop"] or None), (record["take"] or None)
        key = ("cluster", record["family"], stop, take)

        for other in ("B1", "B2"):
            difference = months_by_cell[key] - months_by_cell[(other, record["family"], stop, take)]
            grid_frame.loc[index, f"minus_{other}_pct"] = difference.mean() * 100.0
            grid_frame.loc[index, f"minus_{other}_t"] = t_value(difference)

        neighbor_means = [grid_frame[(grid_frame["portfolio"] == "cluster") & (grid_frame["family"] == record["family"])
                                     & (grid_frame["stop"] == (neighbor_stop or 0)) & (grid_frame["take"] == (neighbor_take or 0))
                                     ]["excess_month_mean_pct"].iloc[0] for neighbor_stop, neighbor_take in neighbors(stop, take)]
        rule_t = record["excess_t"] >= PASS_T
        rule_neighbor = record["excess_month_mean_pct"] > 0 and all(mean > 0 for mean in neighbor_means)
        rule_halves = record["excess_2010_2017_pct"] >= 0 and record["excess_2018_2026_pct"] >= 0
        rule_mdd = record["mdd_pct"] <= PASS_MDD
        rule_trades = record["trades"] >= PASS_TRADES
        grid_frame.loc[index, "pass_t"] = rule_t
        grid_frame.loc[index, "pass_neighbor"] = rule_neighbor
        grid_frame.loc[index, "pass_halves"] = rule_halves
        grid_frame.loc[index, "pass_mdd"] = rule_mdd
        grid_frame.loc[index, "pass_trades"] = rule_trades
        grid_frame.loc[index, "verdict"] = ("보조선까지 통과" if rule_t and rule_neighbor and rule_halves and rule_mdd and rule_trades
                                            else "통과" if rule_t and rule_neighbor and rule_halves else "미달")

    write_tsv(grid_frame, STUDY / "grid.tsv")

    # 연도별(기본 두 칸 + 통과 칸)
    passed = grid_frame[(grid_frame["portfolio"] == "cluster") & (grid_frame["verdict"] != "미달")]
    yearly_cells = [("W", None, None), ("P", None, None)] + [(record["family"], record["stop"] or None, record["take"] or None)
                                                          for _, record in passed.iterrows()]
    base_years = yearly_returns(market, benchmark, first_row)
    yearly = pd.DataFrame({"year": base_years.index, "B0_pct": base_years.to_numpy() * 100.0})

    for family, stop, take in dict.fromkeys(yearly_cells):
        name = f"{family}_{label(stop, take)}"
        returns = yearly_returns(market, values[name], first_row)
        yearly[f"{name}_pct"] = returns.to_numpy() * 100.0
        yearly[f"{name}_excess_pct"] = (returns.to_numpy() - base_years.to_numpy()) * 100.0

    write_tsv(yearly, STUDY / "yearly.tsv")

    # MAE·MFE(가격 청산 없는 두 칸)
    trades_frame = pd.DataFrame(trade_rows)
    excursion_rows = []

    for family in FAMILIES:
        subset = trades_frame[(trades_frame["portfolio"] == "cluster") & (trades_frame["family"] == family)
                              & (trades_frame["stop"] == 0) & (trades_frame["take"] == 0) & (trades_frame["reason"] != "open")]
        winners = subset[subset["net_pct"] > 0]

        for measure, values_array in (("MAE", subset["mae_pct"]), ("MFE", subset["mfe_pct"]),
                                      ("이긴 매매 고점 전 MAE", winners["mae_before_peak_pct"]),
                                      ("이긴 매매 MFE", winners["mfe_pct"]), ("진 매매 MFE", subset[subset["net_pct"] <= 0]["mfe_pct"])):
            record = {"family": family, "measure": measure, "count": len(values_array)}
            record.update(distribution(values_array.to_numpy() / 100.0))
            excursion_rows.append(record)

        for level in (10, 15, 20):
            excursion_rows.append({"family": family, "measure": f"MFE ≥ +{level}% 비율(%)", "count": len(subset),
                                   "p50": float((subset["mfe_pct"] >= level).mean() * 100.0)})

        for level in (5, 8, 10):
            excursion_rows.append({"family": family, "measure": f"MAE ≤ −{level}% 비율(%)", "count": len(subset),
                                   "p50": float((subset["mae_pct"] <= -level).mean() * 100.0)})
            excursion_rows.append({"family": family, "measure": f"이긴 매매 중 고점 전 MAE ≤ −{level}% 비율(%)",
                                   "count": len(winners), "p50": float((winners["mae_before_peak_pct"] <= -level).mean() * 100.0)})

    write_tsv(pd.DataFrame(excursion_rows), STUDY / "mae_mfe.tsv")

    # 감도(보고): 매도세 0.30%, 대표 = 거래대금 1위
    sensitivity = {}

    for family in FAMILIES:
        value, trades = simulate(market, portfolios["cluster"], execution, family, None, None, SELL_TAX_SENSITIVITY)
        sensitivity[f"{family}_매도세0.30"] = cell_metrics(market, value, trades, first_row, base_months, SELL_TAX_SENSITIVITY)[0]
        value, trades = simulate(market, [week["cluster_liquid"] for week in weeks], execution, family, None, None)
        sensitivity[f"{family}_대표거래대금1위"] = cell_metrics(market, value, trades, first_row, base_months, SELL_TAX)[0]

    trades_frame.to_parquet(STUDY / "trades.parquet", index=False)
    pd.DataFrame({"date": market.dates, **values}).to_parquet(STUDY / "nav.parquet", index=False)
    benchmark_years = (pd.Timestamp(str(market.dates[-1])) - pd.Timestamp(str(market.dates[first_row]))).days / 365.25
    universe_sizes = pd.Series([len(week["universe"]) for week in weeks], index=market.dates[rows])
    pick_sizes = pd.Series([len(week["cluster"]) for week in weeks])
    metrics = {
        "study": "36_cluster_momentum", "first_execution": int(market.dates[first_row]), "last_date": int(market.dates[-1]),
        "weeks": len(weeks), "universe_min": int(universe_sizes.min()), "universe_max": int(universe_sizes.max()),
        "universe_median": float(universe_sizes.median()), "picks_min": int(pick_sizes.min()), "picks_max": int(pick_sizes.max()),
        "B0_cagr_pct": ((benchmark[-1] / benchmark[first_row - 1]) ** (1 / benchmark_years) - 1) * 100.0,
        "B0_mdd_pct": max_drawdown(benchmark[first_row - 1:]),
        "sensitivity": sensitivity,
        "reproduce": {"command": "py -X utf8 research/studies/36_cluster_momentum/backtest.py", "commit": git_commit(),
                      "bars_sha256": sha256_file(study33.BARS_PATH), "load": information,
                      "grid_sha256": sha256_file(STUDY / "grid.tsv"), "seed": "난수 없음"}}
    (STUDY / "metrics.json").write_text(json.dumps(clean(metrics), ensure_ascii=False, indent=1), encoding="utf-8", newline="\n")
    print(f"끝, {time.time() - started_at:.0f}초")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
