"""스터디 44 · 가치 바스켓 손실 제한 — 종목별 가격 청산(S)·국면 연동(A)·바스켓 낙폭 한도(B) 233칸을 일별 순자산으로 잰다.

사전등록 `research/studies/44_value_basket_drawdown_guard/PREREG.md`를 그대로 옮긴 하네스다.
신호는 새로 짜지 않는다 — 스터디 22 하네스(안에서 19를 읽는다)의 `month_calendar`·`build_signals`를 그대로 쓰고,
거래대금 하한은 스터디 23처럼 `fundamental.MIN_TURNOVER_KRW`를 30억으로 바꾼다. 상위 N·가치 비중·하한 값은 라이브
`PYQuant/live/basket_forward.py::ValueTiltSleeve` 기본값에서 읽는다. 비용은 `PYQuant/backtest/costs.py` RESEARCH_MID,
통계는 `PYQuant/backtest/stats.py`.

흐름: 달력(기준일 = 전월 마지막 거래일 종가 판정, 체결일 = 당월 첫 거래일 종가) → 분기 첫 달에만 리밸(22의 position % 3 == 0)
      → 일별 루프(그날 종가로 평가 → 전날 판정한 주문을 그날 종가에 체결 → 그날 종가로 다음 날 주문을 판정)
      → 칸별 연 수익률·샤프·최대 낙폭·칼마·회전·비용·월 초과수익 t·연도별 → 개선 칸·채택 후보 판정.
look-ahead 차단: 판정은 day_position의 종가까지만 읽고, 주문은 pending에 넣어 day_position + 1에서 체결한다.

산출물(같은 폴더): cells.csv, yearly.csv, mae_mfe.csv, breadth_forward.csv, drawdown_episodes.csv, metrics.json, README.md.

재현: py research/studies/44_value_basket_drawdown_guard/run_drawdown_guard.py   (저장소 루트에서, seed 20261016, 난수는 쓰지 않는다)
"""
from __future__ import annotations

import datetime
import importlib.util
import json
import math
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "PYQuant"))

from PYQuant.features import fundamental  # noqa: E402
from PYQuant.live.basket_forward import ValueTiltSleeve  # noqa: E402
from backtest import stats  # noqa: E402
from backtest.costs import RESEARCH_BY_LEVEL, side_cost_percent_at_price  # noqa: E402

STUDY_DIR = Path(__file__).resolve().parent
STUDY22_PATH = REPO_ROOT / "research" / "studies" / "22_pbr_roe_value_tilt" / "run_value_tilt.py"
STUDY23_METRICS = REPO_ROOT / "research" / "studies" / "23_value_tilt_liquidity_floor" / "floor_30" / "metrics.json"
STUDY22_METRICS = STUDY22_PATH.parent / "metrics.json"
SEED = 20261016
COST_LEVEL = "mid"
REBALANCE_EVERY_MONTHS = 3
TRADING_DAYS_PER_YEAR = 252
FORWARD_DAYS = 20

# ── 사전등록 격자(PREREG.md 시험 칸, 결과 전 확정) ─────────────────────────────
STOP_LOSS_GRID = (0.05, 0.075, 0.10, 0.125, 0.15, 0.20, 0.25, 0.30, 0.40)
TAKE_PROFIT_GRID = (None, 0.20, 0.30, 0.50, 0.75, 1.00)
PROCEEDS_GRID = ("cash", "redistribute")
MOVING_AVERAGE_GRID = (50, 100, 150, 200, 250)
BREADTH_THRESHOLD_GRID = (0.30, 0.35, 0.40, 0.45, 0.50, 0.55, 0.60)
REDUCED_EXPOSURE_GRID = (0.0, 0.5)
DRAWDOWN_LIMIT_GRID = (0.05, 0.075, 0.10, 0.125, 0.15, 0.175, 0.20, 0.25, 0.30)
RECOVERY_GRID = ("rebalance", "rebound_5", "rebound_10")
REBOUND_OF = {"rebound_5": 0.05, "rebound_10": 0.10}
GRID_AXES = 3

# 하락 구간 낙폭 창(판정에 안 쓴다). 2020-03은 2020-02-01–2020-04-30으로 잡았다(고른 값).
EPISODE_WINDOWS = {"2018": ("2018-01-01", "2018-12-31"), "2020-03": ("2020-02-01", "2020-04-30"), "2022": ("2022-01-01", "2022-12-31")}
MAE_BUCKETS = ((-np.inf, -0.20, "<-20%"), (-0.20, 0.0, "-20–0%"), (0.0, 0.20, "0–20%"), (0.20, np.inf, ">20%"))
BREADTH_BUCKET_WIDTH = 0.05


def load_study22():
    """스터디 22 하네스를 모듈로 읽는다(22는 안에서 19를 읽는다). 달력·신호·git·지문 함수를 빌려 쓴다."""
    specification = importlib.util.spec_from_file_location("study22", STUDY22_PATH)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


# ── 칸 정의 ──────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Cell:
    branch: str                                   # base | S | A | B
    stop_loss: float | None = None
    take_profit: float | None = None
    proceeds: str | None = None
    moving_average_length: int | None = None
    breadth_threshold: float | None = None
    reduced_exposure: float | None = None
    drawdown_limit: float | None = None
    recovery: str | None = None

    @property
    def label(self) -> str:
        if self.branch == "base":
            return "base"

        if self.branch == "S":
            take = "none" if self.take_profit is None else f"{self.take_profit:g}"
            return f"S_L{self.stop_loss:g}_T{take}_{self.proceeds}"

        if self.branch == "A":
            return f"A_ma{self.moving_average_length}_th{self.breadth_threshold:.2f}_x{self.reduced_exposure:g}"

        return f"B_N{self.drawdown_limit:g}_x{self.reduced_exposure:g}_{self.recovery}"

    def grid_key(self, take_profit_order: tuple = TAKE_PROFIT_GRID[1:] + (None,)) -> tuple:
        """이웃 판정용 축 위치(정수). 익절 축은 기본으로 20→100→없음(없음 = 무한대) 순서다(고른 값, PREREG 나열 순서는 대안으로 같이 잰다)."""
        if self.branch == "S":
            return (STOP_LOSS_GRID.index(self.stop_loss), take_profit_order.index(self.take_profit), PROCEEDS_GRID.index(self.proceeds))

        if self.branch == "A":
            return (MOVING_AVERAGE_GRID.index(self.moving_average_length), BREADTH_THRESHOLD_GRID.index(self.breadth_threshold),
                    REDUCED_EXPOSURE_GRID.index(self.reduced_exposure))

        if self.branch == "B":
            return (DRAWDOWN_LIMIT_GRID.index(self.drawdown_limit), REDUCED_EXPOSURE_GRID.index(self.reduced_exposure),
                    RECOVERY_GRID.index(self.recovery))

        return ()


def all_cells() -> list:
    cells = [Cell("base")]
    cells += [Cell("S", stop_loss=loss, take_profit=take, proceeds=proceeds)
              for loss in STOP_LOSS_GRID for take in TAKE_PROFIT_GRID for proceeds in PROCEEDS_GRID]
    cells += [Cell("A", moving_average_length=length, breadth_threshold=threshold, reduced_exposure=exposure)
              for length in MOVING_AVERAGE_GRID for threshold in BREADTH_THRESHOLD_GRID for exposure in REDUCED_EXPOSURE_GRID]
    cells += [Cell("B", drawdown_limit=limit, reduced_exposure=exposure, recovery=recovery)
              for limit in DRAWDOWN_LIMIT_GRID for exposure in REDUCED_EXPOSURE_GRID for recovery in RECOVERY_GRID]
    return cells


# ── 입력 정리 ────────────────────────────────────────────────────────────────

@dataclass
class Context:
    days: pd.DatetimeIndex                 # 루프가 도는 거래일(첫 기준일 – 마지막 청산일)
    tickers: list
    close_filled: np.ndarray               # days × tickers, 앞 값으로 채운 종가
    has_bar: np.ndarray                    # days × tickers, 그날 봉이 있는지
    rebalance_rank: dict                   # 체결일 위치 → (점수순 종목 열 목록, 열 → 순위)
    month_rows: list                       # (month, entry_position, exit_position, benchmark)
    breadth: dict                          # 이동평균 길이 → days 길이 배열
    top_n: int
    buffer_multiple: int
    cost_rates: dict = field(default_factory=dict)   # (열, 일 위치, side) → 비용률(소수) 캐시


def build_context(study22, data, signals: dict, calendar: pd.DataFrame, top_n: int, value_weight: float) -> Context:
    base = study22.base
    months = sorted(signals)
    calendar = calendar.set_index("month").loc[months]
    first_as_of, last_exit = calendar["as_of"].iloc[0], calendar["exit"].iloc[-1]
    full_close = data.bars.close
    tickers = sorted(set().union(*[set(frame.index) for frame in signals.values()]))
    panel = full_close.reindex(columns=tickers)
    filled_panel = panel.ffill()
    days = full_close.loc[first_as_of:last_exit].index
    day_position = {day: position for position, day in enumerate(days)}
    column_of = {ticker: column for column, ticker in enumerate(tickers)}

    rebalance_rank = {}

    for position, month in enumerate(months):
        if position % REBALANCE_EVERY_MONTHS != 0:
            continue

        frame = signals[month]
        score = value_weight * frame["z_value"] + (1.0 - value_weight) * frame["z_quality"]
        ranked = [column_of[ticker] for ticker in score.sort_values(ascending=False).index]
        rebalance_rank[day_position[calendar.loc[month, "entry"]]] = (ranked, {column: rank for rank, column in enumerate(ranked, start=1)})

    month_rows = [(month, day_position[calendar.loc[month, "entry"]], day_position[calendar.loc[month, "exit"]],
                   float(signals[month]["forward_return"].mean())) for month in months]

    # 폭: 그날 보이는 가장 최근 기준일의 유니버스 안에서 (종가 > 이동평균) 비율. 그날 봉이 없거나 이동평균이 안 찬 종목은 뺀다.
    universe_mask = np.zeros((len(days), len(tickers)), dtype=bool)
    as_of_positions = [(full_close.index.get_loc(calendar.loc[month, "as_of"]) - full_close.index.get_loc(first_as_of), month) for month in months]

    for index, (start_position, month) in enumerate(as_of_positions):
        end_position = as_of_positions[index + 1][0] if index + 1 < len(as_of_positions) else len(days)
        universe_mask[start_position:end_position, [column_of[ticker] for ticker in signals[month].index]] = True

    raw_slice = panel.loc[days].to_numpy()
    has_bar = ~np.isnan(raw_slice)
    breadth = {}

    for length in MOVING_AVERAGE_GRID:
        moving_average = filled_panel.rolling(length, min_periods=length).mean().loc[days].to_numpy()
        valid = universe_mask & has_bar & ~np.isnan(moving_average)
        above = valid & (np.nan_to_num(raw_slice) > np.nan_to_num(moving_average))
        counts = valid.sum(axis=1)
        breadth[length] = np.where(counts > 0, above.sum(axis=1) / np.maximum(counts, 1), np.nan)

    return Context(days, tickers, filled_panel.loc[days].to_numpy(), has_bar, rebalance_rank, month_rows, breadth,
                   top_n, base.BUFFER_MULTIPLE)


def cost_rate(context: Context, column: int, position: int, side: str, price: float) -> float:
    key = (column, position, side)
    rate = context.cost_rates.get(key)

    if rate is None:
        rate = side_cost_percent_at_price(RESEARCH_BY_LEVEL[COST_LEVEL], side, price) / 100.0
        context.cost_rates[key] = rate

    return rate


# ── 장부 ─────────────────────────────────────────────────────────────────────

class Book:
    """현금·종목 수량(순자산 1.0 기준 단위). 매매는 그날 종가에 비용을 붙여 낸다. 비용·매수 금액은 체결 직전 순자산 대비로 누적한다."""

    def __init__(self, context: Context):
        self.context = context
        self.cash = 1.0
        self.units: dict = {}
        self.cost_fraction = 0.0
        self.buy_fraction = 0.0

    def value(self, prices: np.ndarray) -> float:
        return self.cash + sum(quantity * prices[column] for column, quantity in self.units.items())

    def set_value(self, column: int, target: float, prices: np.ndarray, position: int, reference_value: float) -> float:
        """종목 하나를 목표 금액으로 맞춘다. 돌려주는 값은 현금 변화(매도면 비용 뺀 수령액, 매수면 음수)."""
        price = prices[column]
        current = self.units.get(column, 0.0) * price
        change = target - current

        if abs(change) <= 1e-12:
            return 0.0

        if change > 0:
            cost = change * cost_rate(self.context, column, position, "BUY", price)
            self.units[column] = self.units.get(column, 0.0) + change / price
            self.buy_fraction += change / reference_value
            cash_change = -change - cost
        else:
            cost = -change * cost_rate(self.context, column, position, "SELL", price)

            if target <= 1e-12:
                self.units.pop(column, None)
            else:
                self.units[column] += change / price

            cash_change = -change - cost

        self.cash += cash_change
        self.cost_fraction += cost / reference_value
        return cash_change

    def liquidate_frozen(self, column: int, prices: np.ndarray) -> None:
        """그날 봉이 없는 보유 종목 — 22·23처럼 마지막 종가로 이미 청산된 것으로 보고 비용 없이 현금으로 돌린다."""
        self.cash += self.units.pop(column) * prices[column]


def select_members(context: Context, ranked: list, rank_of: dict, held: list) -> list:
    """22 simulate의 리밸 규칙 그대로 — 보유 중 순위 ≤ 버퍼(2N)는 남기고 나머지를 점수순으로 채워 N."""
    limit = context.buffer_multiple * context.top_n
    kept = [column for column in held if rank_of.get(column, math.inf) <= limit]
    kept_set = set(kept)
    fill = [column for column in ranked if column not in kept_set][: max(context.top_n - len(kept), 0)]
    return kept + fill


# ── 시뮬레이터 ───────────────────────────────────────────────────────────────

@dataclass
class Run:
    value_pre: np.ndarray        # 그날 종가 평가(체결 전)
    value_post: np.ndarray       # 체결 뒤
    exposure: np.ndarray         # 체결 뒤 노출(현금 아닌 비율)
    cost_fraction: float
    buy_fraction: float
    episodes: list               # A·B 축소 구간 [{cut_day, restore_day, trough_day}]
    exits: int                   # S 가격 청산 건수
    segments: list               # 기준 칸만 — 보유 구간별 MAE·MFE·끝 수익률


def simulate(context: Context, cell: Cell, record_segments: bool = False, buffer_multiple: int | None = None) -> Run:
    if buffer_multiple is not None:
        context = Context(**{**context.__dict__, "buffer_multiple": buffer_multiple})

    day_count = len(context.days)
    book = Book(context)
    value_pre = np.empty(day_count)
    value_post = np.empty(day_count)
    exposure_path = np.empty(day_count)
    average_cost: dict = {}
    pending_exits: set = set()
    shadow: dict = {}                    # A·B: 줄이지 않은 바스켓의 수량(가상 순자산 = 수량 × 종가)
    exposure = 1.0
    pending_exposure = None
    peak = 1.0
    cut_active = False
    trough_value = math.inf
    trough_day = None
    episodes: list = []
    exits = 0
    open_segments: dict = {}
    segments: list = []
    basket_branch = cell.branch in ("A", "B")

    for position in range(day_count):
        prices = context.close_filled[position]
        bars_today = context.has_bar[position]
        current_value = book.value(prices)
        value_pre[position] = current_value
        rebalance = context.rebalance_rank.get(position)

        if record_segments:
            for column, segment in open_segments.items():
                change = prices[column] / segment["entry_price"] - 1.0
                segment["mae"] = min(segment["mae"], change)
                segment["mfe"] = max(segment["mfe"], change)

        # ── 체결(전날 판정 + 오늘 리밸) ──
        if not basket_branch:
            if rebalance is not None:
                pending_exits.clear()

                for column in [column for column in book.units if not bars_today[column]]:
                    book.liquidate_frozen(column, prices)

                current_value = book.value(prices)
                members = select_members(context, rebalance[0], rebalance[1], list(book.units))
                target = current_value / len(members)
                member_set = set(members)

                if record_segments:
                    for column, segment in open_segments.items():
                        segment["end"] = prices[column] / segment["entry_price"] - 1.0
                        segments.append(segment)

                    open_segments = {column: {"column": column, "start": position, "entry_price": prices[column], "mae": 0.0, "mfe": 0.0}
                                     for column in members}

                for column in [column for column in book.units if column not in member_set]:
                    book.set_value(column, 0.0, prices, position, current_value)
                    average_cost.pop(column, None)

                for column in members:
                    previous_units = book.units.get(column, 0.0)
                    book.set_value(column, target, prices, position, current_value)
                    new_units = book.units.get(column, 0.0)

                    if previous_units <= 0 or column not in average_cost:
                        average_cost[column] = prices[column]
                    elif new_units > previous_units:
                        average_cost[column] = (previous_units * average_cost[column] + (new_units - previous_units) * prices[column]) / new_units

            elif pending_exits:
                proceeds = 0.0
                executed = []

                for column in sorted(pending_exits):
                    if column in book.units and bars_today[column]:
                        proceeds += book.set_value(column, 0.0, prices, position, current_value)
                        average_cost.pop(column, None)
                        executed.append(column)

                    elif column not in book.units:
                        executed.append(column)

                pending_exits.difference_update(executed)
                exits += len(executed)
                remaining = [column for column in book.units if bars_today[column] and column not in pending_exits]

                if cell.proceeds == "redistribute" and proceeds > 0 and remaining:
                    share = proceeds / len(remaining)

                    for column in remaining:
                        previous_units = book.units[column]
                        book.set_value(column, previous_units * prices[column] + share, prices, position, current_value)
                        new_units = book.units[column]
                        average_cost[column] = (previous_units * average_cost[column] + (new_units - previous_units) * prices[column]) / new_units

        else:
            changed = False
            restored = False

            if pending_exposure is not None:
                if pending_exposure < 1.0:
                    cut_active = True
                    episodes.append({"cut_day": position, "restore_day": None, "trough_day": position})
                    trough_value = sum(quantity * prices[column] for column, quantity in shadow.items()) or math.inf
                    trough_day = position
                else:
                    cut_active = False
                    restored = True
                    episodes[-1]["restore_day"] = position
                    episodes[-1]["trough_day"] = trough_day

                exposure = pending_exposure
                pending_exposure = None
                changed = True

            if rebalance is not None:
                for column in [column for column in book.units if not bars_today[column]]:
                    book.liquidate_frozen(column, prices)

                shadow_value = sum(quantity * prices[column] for column, quantity in shadow.items()) if shadow else book.value(prices)
                members = select_members(context, rebalance[0], rebalance[1], list(shadow))
                shadow = {column: shadow_value / len(members) / prices[column] for column in members}

                if cell.branch == "B" and cell.recovery == "rebalance" and cut_active:
                    cut_active = False
                    restored = True
                    exposure = 1.0
                    episodes[-1]["restore_day"] = position
                    episodes[-1]["trough_day"] = trough_day

                changed = True

            if changed:
                current_value = book.value(prices)
                shadow_values = {column: quantity * prices[column] for column, quantity in shadow.items()}
                shadow_total = sum(shadow_values.values())

                for column in sorted(set(book.units) | set(shadow_values)):
                    if not bars_today[column]:
                        continue

                    target = exposure * current_value * shadow_values.get(column, 0.0) / shadow_total if shadow_total > 0 else 0.0
                    book.set_value(column, target, prices, position, current_value)

                if cell.branch == "B" and restored:
                    peak = book.value(prices)          # 복귀하면 고점을 그 자리로 다시 잡는다(고른 값, 안 하면 바로 다시 걸린다)

        post_value = book.value(prices)
        value_post[position] = post_value
        exposure_path[position] = 1.0 - book.cash / post_value if post_value > 0 else 0.0

        # ── 판정(오늘 종가 → 다음 거래일 종가 체결) ──
        if cell.branch == "S":
            for column, quantity in book.units.items():
                if not bars_today[column] or column in pending_exits:
                    continue

                change = prices[column] / average_cost[column] - 1.0

                if change <= -cell.stop_loss or (cell.take_profit is not None and change >= cell.take_profit):
                    pending_exits.add(column)

        elif cell.branch in ("A", "B"):
            shadow_now = sum(quantity * prices[column] for column, quantity in shadow.items())

            if cut_active and shadow and shadow_now < trough_value:
                trough_value, trough_day = shadow_now, position

            if cell.branch == "A":
                breadth = context.breadth[cell.moving_average_length][position]

                if np.isfinite(breadth):
                    desired = cell.reduced_exposure if breadth < cell.breadth_threshold else 1.0

                    if desired != exposure and pending_exposure is None:
                        pending_exposure = desired

            elif shadow:
                if not cut_active and pending_exposure is None:
                    peak = max(peak, post_value)

                    if (peak - post_value) / peak >= cell.drawdown_limit:
                        pending_exposure = cell.reduced_exposure

                elif cut_active and pending_exposure is None and cell.recovery in REBOUND_OF:
                    if shadow_now / trough_value - 1.0 >= REBOUND_OF[cell.recovery]:
                        pending_exposure = 1.0

    if record_segments:
        last = day_count - 1

        for column, segment in open_segments.items():
            segment["end"] = context.close_filled[last][column] / segment["entry_price"] - 1.0
            segments.append(segment)

    return Run(value_pre, value_post, exposure_path, book.cost_fraction, book.buy_fraction, episodes, exits, segments)


# ── 요약 ─────────────────────────────────────────────────────────────────────

def summarize(context: Context, cell: Cell, run: Run) -> tuple:
    days = context.days
    daily = pd.Series(run.value_post, index=days).pct_change().dropna()
    performance = stats.performance_summary(daily, TRADING_DAYS_PER_YEAR)
    years = len(daily) / TRADING_DAYS_PER_YEAR
    monthly = np.array([run.value_pre[exit_position] / run.value_pre[entry_position] - 1.0 for _, entry_position, exit_position, _ in context.month_rows])
    benchmark = np.array([row[3] for row in context.month_rows])
    excess = monthly - benchmark
    excess_test = stats.one_sample_t(excess)
    monthly_performance = stats.performance_summary(monthly, 12)
    lags = [episode["restore_day"] - episode["trough_day"] for episode in run.episodes if episode["restore_day"] is not None]
    row = {
        "cell": cell.label, "branch": cell.branch, "stop_loss": cell.stop_loss, "take_profit": cell.take_profit, "proceeds": cell.proceeds,
        "moving_average_length": cell.moving_average_length, "breadth_threshold": cell.breadth_threshold,
        "reduced_exposure": cell.reduced_exposure, "drawdown_limit": cell.drawdown_limit, "recovery": cell.recovery,
        "annual_return": performance["annual_return"], "sharpe": performance["sharpe"],
        "max_drawdown_percent": performance["max_drawdown_percent"], "calmar": performance["calmar"],
        "turnover_annual": run.buy_fraction / years, "cost_annual": run.cost_fraction / years,
        "excess_monthly_mean": excess_test.mean, "excess_t": excess_test.t_statistic, "excess_annual": float(excess.mean() * 12),
        "monthly_sharpe": monthly_performance["sharpe"], "monthly_max_drawdown_percent": monthly_performance["max_drawdown_percent"],
        "months": int(len(monthly)), "average_exposure": float(run.exposure.mean()),
        "price_exits": run.exits, "cut_episodes": len(run.episodes),
        "mean_restore_lag_days": float(np.mean(lags)) if lags else np.nan,
    }
    year_end = pd.Series(run.value_post, index=days).groupby(days.year).last()
    previous = year_end.shift(1).fillna(1.0)
    yearly = [{"cell": cell.label, "year": int(year), "return": float(year_end[year] / previous[year] - 1.0)} for year in year_end.index]
    episodes = []
    value_series = pd.Series(run.value_post, index=days)

    for name, (start, end) in EPISODE_WINDOWS.items():
        window = value_series.loc[start:end]
        drawdown = float(((window.cummax() - window) / window.cummax()).max() * 100.0) if len(window) else np.nan
        episodes.append({"cell": cell.label, "branch": cell.branch, "window": name, "start": start, "end": end,
                         "max_drawdown_percent": drawdown, "window_return": float(window.iloc[-1] / window.iloc[0] - 1.0) if len(window) else np.nan})

    return row, yearly, episodes, monthly, benchmark


def mae_mfe_table(context: Context, segments: list) -> pd.DataFrame:
    frame = pd.DataFrame(segments)
    rows = []

    for low, high, label in MAE_BUCKETS + ((-np.inf, np.inf, "전체"),):
        part = frame[(frame["end"] > low) & (frame["end"] <= high)] if label != "전체" else frame
        row = {"end_bucket": label, "segments": int(len(part)), "end_median": float(part["end"].median()),
               "mae_p10": float(part["mae"].quantile(0.10)), "mae_p25": float(part["mae"].quantile(0.25)), "mae_median": float(part["mae"].median()),
               "mfe_median": float(part["mfe"].median()), "mfe_p75": float(part["mfe"].quantile(0.75)), "mfe_p90": float(part["mfe"].quantile(0.90))}

        for loss in STOP_LOSS_GRID:
            row[f"share_mae_le_minus_{loss * 100:g}pct"] = float((part["mae"] <= -loss).mean())

        for take in TAKE_PROFIT_GRID[1:]:
            row[f"share_mfe_ge_{take * 100:g}pct"] = float((part["mfe"] >= take).mean())

        rows.append(row)

    return pd.DataFrame(rows)


def breadth_forward_table(context: Context, baseline: Run) -> pd.DataFrame:
    values = baseline.value_post
    forward = np.full(len(values), np.nan)
    forward[:-FORWARD_DAYS] = values[FORWARD_DAYS:] / values[:-FORWARD_DAYS] - 1.0
    rows = []
    edges = np.round(np.arange(0.0, 1.0 + BREADTH_BUCKET_WIDTH / 2, BREADTH_BUCKET_WIDTH), 4)

    for length in MOVING_AVERAGE_GRID:
        breadth = context.breadth[length]
        valid = np.isfinite(breadth)
        total = int(valid.sum())

        for low, high in zip(edges[:-1], edges[1:]):
            inside = valid & (breadth >= low) & ((breadth < high) if high < 1.0 else (breadth <= high))
            forward_inside = forward[inside & np.isfinite(forward)]
            rows.append({"moving_average_length": length, "breadth_low": float(low), "breadth_high": float(high), "days": int(inside.sum()),
                         "share_of_days": float(inside.sum() / total) if total else np.nan,
                         "forward20_mean": float(forward_inside.mean()) if len(forward_inside) else np.nan,
                         "forward20_median": float(np.median(forward_inside)) if len(forward_inside) else np.nan,
                         "forward20_positive_share": float((forward_inside > 0).mean()) if len(forward_inside) else np.nan,
                         "breadth_p10": float(np.nanquantile(breadth, 0.10)), "breadth_median": float(np.nanmedian(breadth)),
                         "breadth_p90": float(np.nanquantile(breadth, 0.90))})

    return pd.DataFrame(rows)


# ── 판정 ─────────────────────────────────────────────────────────────────────

def judge(cells: list, rows: pd.DataFrame, take_profit_order: tuple) -> pd.Series:
    """개선 칸 = 샤프 > 기준 그리고 최대 낙폭 < 기준. 채택 후보 = 개선 칸이면서 같은 갈래 격자의 각 축 ±1 이웃이 전부 개선 칸."""
    improved = rows.set_index("cell")["improved"]
    candidate = {}

    for branch in ("S", "A", "B"):
        branch_cells = [cell for cell in cells if cell.branch == branch]
        grid = {cell.grid_key(take_profit_order) if branch == "S" else cell.grid_key(): (1.0 if improved[cell.label] else -1.0) for cell in branch_cells}

        for cell in branch_cells:
            key = cell.grid_key(take_profit_order) if branch == "S" else cell.grid_key()
            positive, count, _ = stats.grid_robustness(grid, key)
            candidate[cell.label] = bool(improved[cell.label] and count > 0 and positive == count)

    candidate["base"] = False
    return rows["cell"].map(candidate)


def neighbor_counts(cells: list, rows: pd.DataFrame) -> tuple:
    improved = rows.set_index("cell")["improved"]
    positive_of, count_of = {"base": 0}, {"base": 0}

    for branch in ("S", "A", "B"):
        branch_cells = [cell for cell in cells if cell.branch == branch]
        grid = {cell.grid_key(): (1.0 if improved[cell.label] else -1.0) for cell in branch_cells}

        for cell in branch_cells:
            positive, count, _ = stats.grid_robustness(grid, cell.grid_key())
            positive_of[cell.label], count_of[cell.label] = positive, count

    return rows["cell"].map(positive_of), rows["cell"].map(count_of)


# ── README ───────────────────────────────────────────────────────────────────

def percent(value: float, digits: int = 1, sign: bool = False) -> str:
    if value is None or not np.isfinite(value):
        return "–"

    return f"{value * 100:+.{digits}f}%" if sign else f"{value * 100:.{digits}f}%"


def describe(row: pd.Series) -> str:
    if row["branch"] == "S":
        take = "없음" if pd.isna(row["take_profit"]) else f"{row['take_profit'] * 100:g}%"
        return f"손절 {row['stop_loss'] * 100:g}% · 익절 {take} · {'현금 대기' if row['proceeds'] == 'cash' else '재배분'}"

    if row["branch"] == "A":
        return f"{int(row['moving_average_length'])}일선 · 문턱 {row['breadth_threshold']:.2f} · 노출 {row['reduced_exposure'] * 100:g}%"

    if row["branch"] == "B":
        recovery = {"rebalance": "다음 분기 리밸", "rebound_5": "가상 +5% 반등", "rebound_10": "가상 +10% 반등"}[row["recovery"]]
        return f"한도 {row['drawdown_limit'] * 100:g}% · 노출 {row['reduced_exposure'] * 100:g}% · 복귀 {recovery}"

    return "손절 없음(현 라이브)"


def table_line(row: pd.Series, episodes: pd.DataFrame) -> str:
    drawdowns = episodes[episodes["cell"] == row["cell"]].set_index("window")["max_drawdown_percent"]
    return (f"| {row['branch']} | {describe(row)} | {percent(row['annual_return'], 2)} | {row['sharpe']:.2f} | {row['max_drawdown_percent']:.1f}% | "
            f"{row['calmar']:.2f} | {row['turnover_annual'] * 100:.0f}% | {percent(row['cost_annual'], 2)} | {row['excess_t']:.2f} | "
            f"{drawdowns.get('2018', np.nan):.1f}% · {drawdowns.get('2020-03', np.nan):.1f}% · {drawdowns.get('2022', np.nan):.1f}% | "
            f"{'예' if row['improved'] else '아니오'} | {int(row['neighbors_improved'])}/{int(row['neighbor_count'])} |")


def write_readme(metrics: dict, rows: pd.DataFrame, episodes: pd.DataFrame, mae_mfe: pd.DataFrame, breadth_forward: pd.DataFrame,
                 yearly: pd.DataFrame) -> None:
    base_row = rows[rows["branch"] == "base"].iloc[0]
    header = ("| 갈래 | 칸 | 연 수익률 | 샤프 | 최대 낙폭 | 칼마 | 연 회전 | 연 비용 | 월 초과 t | 낙폭 2018 · 2020-03 · 2022 | 개선 | 이웃 개선 |\n"
              "|---|---|---|---|---|---|---|---|---|---|---|---|")
    lines = [table_line(base_row, episodes)]

    for branch in ("S", "A", "B"):
        top = rows[rows["branch"] == branch].sort_values("sharpe", ascending=False).head(3)
        lines += [table_line(row, episodes) for _, row in top.iterrows()]

    candidates = rows[rows["candidate"]]
    candidate_lines = "\n".join(table_line(row, episodes) for _, row in candidates.iterrows()) if len(candidates) else "| – | 채택 후보 없음 | | | | | | | | | | |"

    improved_counts = {branch: (int(rows[(rows['branch'] == branch) & rows['improved']].shape[0]), int((rows['branch'] == branch).sum()))
                       for branch in ("S", "A", "B")}

    # 꺾이는 지점: 축 하나씩, 나머지 축 평균
    def axis_table(branch: str, column: str, values: tuple, render) -> str:
        part = rows[rows["branch"] == branch]
        out = []

        for value in values:
            selected = part[part[column].isna()] if value is None else part[np.isclose(part[column].astype(float), value)] if not isinstance(value, str) else part[part[column] == value]
            out.append(f"| {render(value)} | {len(selected)} | {selected['annual_return'].mean() * 100:.2f}% | {selected['sharpe'].mean():.2f} | "
                       f"{selected['max_drawdown_percent'].mean():.1f}% | {int(selected['improved'].sum())} |")

        return "\n".join(out)

    axis_header = "| 값 | 칸 수 | 연 수익률 평균 | 샤프 평균 | 최대 낙폭 평균 | 개선 칸 |\n|---|---|---|---|---|---|"
    stop_axis = axis_table("S", "stop_loss", STOP_LOSS_GRID, lambda value: f"손절 {value * 100:g}%")
    take_axis = axis_table("S", "take_profit", TAKE_PROFIT_GRID, lambda value: "익절 없음" if value is None else f"익절 {value * 100:g}%")
    threshold_axis = axis_table("A", "breadth_threshold", BREADTH_THRESHOLD_GRID, lambda value: f"문턱 {value:.2f}")
    length_axis = axis_table("A", "moving_average_length", MOVING_AVERAGE_GRID, lambda value: f"{value}일선")
    limit_axis = axis_table("B", "drawdown_limit", DRAWDOWN_LIMIT_GRID, lambda value: f"한도 {value * 100:g}%")

    forward_200 = breadth_forward[(breadth_forward["moving_average_length"] == 200) & (breadth_forward["days"] > 0)]
    forward_lines = "\n".join(f"| {row.breadth_low:.2f}–{row.breadth_high:.2f} | {row.days} | {row.share_of_days * 100:.1f}% | "
                              f"{percent(row.forward20_mean, 2, True)} | {percent(row.forward20_median, 2, True)} | {row.forward20_positive_share * 100:.0f}% |"
                              for row in forward_200.itertuples(index=False))
    mae_lines = "\n".join(f"| {row.end_bucket} | {row.segments} | {percent(row.mae_median)} | {percent(row.mae_p10)} | {percent(row.mfe_median)} | "
                          f"{row.share_mae_le_minus_10pct * 100:.0f}% | {row.share_mae_le_minus_20pct * 100:.0f}% | {row.share_mae_le_minus_30pct * 100:.0f}% |"
                          for row in mae_mfe.itertuples(index=False))
    reconcile = metrics["reconciliation"]
    verdict = metrics["verdict"]
    period = metrics["period"].replace("~", "–")
    yearly_cells = ["base"] + rows[rows["candidate"]]["cell"].tolist() + [rows[rows["branch"] == branch].sort_values("sharpe", ascending=False).iloc[0]["cell"]
                                                                           for branch in ("S", "A", "B")]
    yearly_cells = list(dict.fromkeys(yearly_cells))
    yearly_pivot = yearly.pivot(index="year", columns="cell", values="return")[yearly_cells]
    yearly_header = "| 연도 | " + " | ".join(yearly_cells) + " |\n|---|" + "---|" * len(yearly_cells)
    yearly_lines = "\n".join(f"| {year} | " + " | ".join(percent(yearly_pivot.loc[year, cell], 1, True) for cell in yearly_cells) + " |"
                             for year in yearly_pivot.index)
    candidate_notes = []

    for _, row in rows[rows["candidate"]].iterrows():
        edge = row["neighbor_count"] < 2 * GRID_AXES          # 세 갈래 모두 축 3개 — 양쪽 이웃이 다 있으면 6칸
        candidate_notes.append(f"- `{row['cell']}`: 연 수익률은 기준보다 {(row['annual_return'] - base_row['annual_return']) * 100:+.2f}%p, 연 회전 {row['turnover_annual'] * 100:.0f}%·비용 "
                               f"{row['cost_annual'] * 100:.2f}%, 월 초과수익 t {row['excess_t']:.2f}(기준 {base_row['excess_t']:.2f}). 이웃 {int(row['neighbor_count'])}칸"
                               f"{' — 격자 끝 칸이라 한쪽 이웃이 없다' if edge else ''}.")

    candidate_note_text = "\n".join(candidate_notes) if candidate_notes else "- 없음."
    text = f"""# 44. 가치 바스켓 손실 제한 — 종목별 손절·국면 연동·바스켓 낙폭 한도 (연구군: 종목레벨 · 재무 팩터)

> 한 줄 요약: {verdict['one_line']} 상태: **{verdict['overall']}**.
> 사전등록 `research/studies/44_value_basket_drawdown_guard/PREREG.md`(결과 전 확정). 233칸(기준 1 + S 108 + A 70 + B 54)을 {period} 일별 순자산으로 한 번에 돌렸다.
> 비용 MID(수수료 0.015%·매도세 0.2%·1틱·충격 0.05%), 판정은 그날 종가·매매는 다음 거래일 종가. 배당 제외. 데이터 등급 B(재무, 스터디 22와 같다).

## 1. 판정 (기준 + 갈래별 샤프 상위 3칸)

개선 칸 = 샤프가 기준보다 높고 최대 낙폭이 기준보다 작은 칸. 채택 후보 = 개선 칸이면서 각 축 ±1 이웃이 전부 개선 칸. 샤프·낙폭은 일별 순자산 기준.

{header}
{chr(10).join(lines)}

개선 칸 수: S {improved_counts['S'][0]}/{improved_counts['S'][1]} · A {improved_counts['A'][0]}/{improved_counts['A'][1]} · B {improved_counts['B'][0]}/{improved_counts['B'][1]}.

### 채택 후보

{header}
{candidate_lines}

{candidate_note_text}

233칸을 본 결과라 헌장 기준(100칸 넘게 비교하면 t 3.5)으로는 월 초과수익 t도 같이 본다. 이 값은 PREREG 판정에는 쓰지 않는다.

익절 축 순서를 PREREG 나열 순서(없음·20·30·50·75·100)로 바꿔 이웃을 잡아도 채택 후보는 {metrics['candidates_alternative_take_profit_order']}개다.

## 2. 꺾이는 지점

축 하나를 고정하고 나머지 축 칸을 평균한 값이다. 기준 칸은 연 {percent(base_row['annual_return'], 2)} · 샤프 {base_row['sharpe']:.2f} · 최대 낙폭 {base_row['max_drawdown_percent']:.1f}%.

S 손절선

{axis_header}
{stop_axis}

S 익절선

{axis_header}
{take_axis}

A 문턱

{axis_header}
{threshold_axis}

A 이동평균 길이

{axis_header}
{length_axis}

B 낙폭 한도

{axis_header}
{limit_axis}

A 폭(200일선) 구간별 다음 20거래일 기준 바스켓 수익률(전체는 `breadth_forward.csv`, 5개 길이)

| 폭 구간 | 일수 | 비중 | 20일 평균 | 20일 중앙값 | 양수 비율 |
|---|---|---|---|---|---|
{forward_lines}

S 보유 구간(리밸–리밸) MAE·MFE — 기준 칸 보유, 끝 수익률 구간별(전체는 `mae_mfe.csv`)

| 끝 수익률 | 구간 수 | MAE 중앙값 | MAE 하위 10% | MFE 중앙값 | MAE ≤ −10% | MAE ≤ −20% | MAE ≤ −30% |
|---|---|---|---|---|---|---|---|
{mae_lines}

## 3. 연도별 수익률 (기준·채택 후보·갈래별 샤프 1위, 전체는 `yearly.csv`)

{yearly_header}
{yearly_lines}

## 4. 기준 칸과 스터디 22·23 정합

| | 연 수익률 | 샤프 | 최대 낙폭 | 월 초과수익 연율 | t |
|---|---|---|---|---|---|
| 이 스터디 기준 칸(일별) | {percent(reconcile['daily']['annual_return'], 2)} | {reconcile['daily']['sharpe']:.2f} | {reconcile['daily']['max_drawdown_percent']:.1f}% | {percent(reconcile['daily']['excess_annual'], 2, True)} | {reconcile['daily']['excess_t']:.2f} |
| 이 스터디 기준 칸(월 수익으로 다시 잼) | {percent(reconcile['monthly']['annual_return'], 2)} | {reconcile['monthly']['sharpe']:.2f} | {reconcile['monthly']['max_drawdown_percent']:.1f}% | {percent(reconcile['monthly']['excess_annual'], 2, True)} | {reconcile['monthly']['excess_t']:.2f} |
| 스터디 23 하한 30억 전 구간(월) | {percent(reconcile['study23']['net_annual_return'], 2)} | {reconcile['study23']['sharpe']:.2f} | {reconcile['study23']['max_drawdown_percent']:.1f}% | {percent(reconcile['study23']['excess_annual'], 2, True)} | {reconcile['study23']['excess_t']:.2f} |
| 스터디 22 하한 10억 전 구간(월, 참고) | {percent(reconcile['study22']['net_annual_return'], 2)} | {reconcile['study22']['sharpe']:.2f} | {reconcile['study22']['max_drawdown_percent']:.1f}% | {percent(reconcile['study22']['excess_annual'], 2, True)} | {reconcile['study22']['excess_t']:.2f} |
| 참고: 버퍼 없이 상위 30(라이브 작성기 규칙, 일별) | {percent(reconcile['no_buffer']['annual_return'], 2)} | {reconcile['no_buffer']['sharpe']:.2f} | {reconcile['no_buffer']['max_drawdown_percent']:.1f}% | {percent(reconcile['no_buffer']['excess_annual'], 2, True)} | {reconcile['no_buffer']['excess_t']:.2f} |

차이가 나는 곳: {reconcile['note']}

## 5. 한계

- 재무 데이터는 2016-04부터라 2010–2015는 돌릴 수 없다(등급 B).
- 일봉 종가 판정이라 장중 급락 중 손절은 재지 않는다. 손절·익절 기준가는 평단(체결 종가, 비용 미포함)이다.
- 종목 선정은 스터디 22·23의 버퍼 규칙(보유 중 순위 60 안이면 남김)을 따랐다. 라이브 작성기(`PYQuant/live/basket_forward.py`)는 버퍼 없이 상위 30을 쓴다 — 4절 마지막 행.
- 내가 고른 값: A 폭의 유니버스는 그날 보이는 가장 최근 월말 기준일의 유니버스, A·B 축소·복귀 때 종목 비중은 줄이지 않은 바스켓의 그날 비중을 따른다,
  B는 복귀(세 방식 모두) 때 고점을 그 자리로 다시 잡는다(안 하면 복귀 다음 날 바로 다시 걸린다), S 재배분은 판 돈을 남은 종목에 같은 금액씩 나눈다,
  S에서 리밸 날과 겹친 청산 주문은 리밸이 대신한다, 익절 축 이웃 순서는 20→100→없음, 2020-03 낙폭 창은 2020-02-01–2020-04-30.
- 거래정지로 리밸 날 봉이 없는 보유 종목은 22·23처럼 마지막 종가에 비용 없이 현금으로 돌렸다(상폐 손실을 낮게 잡는 쪽).
- 매수 비용은 현금에서 빠져 현금이 잠깐 순자산의 0.1% 안팎 음수가 될 수 있다(22의 "수익 − 비용"과 같은 회계).

## 6. 재현

`{metrics['rerun_command']}` · 커밋 {metrics['git_commit']} · seed {SEED}(난수 없음) · {metrics['elapsed_seconds']:.0f}초. 데이터 지문은 `metrics.json` `data_fingerprint`.
"""
    (STUDY_DIR / "README.md").write_bytes(text.encode("utf-8"))


def write_csv(frame: pd.DataFrame, name: str) -> None:
    (STUDY_DIR / name).write_bytes(frame.to_csv(index=False, lineterminator="\n").encode("utf-8"))


# ── main ─────────────────────────────────────────────────────────────────────

def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    np.random.seed(SEED)
    started = time.time()
    sleeve = ValueTiltSleeve(data=None)
    study22 = load_study22()
    base = study22.base

    # [why PREREG 44] 신호는 스터디 23 하한 30억 유니버스 — 23과 같은 방식으로 하한만 바꾼다(값은 라이브 ValueTiltSleeve 기본값).
    fundamental.MIN_TURNOVER_KRW = sleeve.turnover_floor_krw
    base.fundamental.MIN_TURNOVER_KRW = sleeve.turnover_floor_krw

    data = fundamental.FundamentalData.load()
    calendar = base.month_calendar(data.bars.trading_days)
    signals, _ = base.build_signals(data, calendar)
    months = sorted(signals)
    context = build_context(study22, data, signals, calendar, sleeve.top_n, sleeve.value_weight)
    print(f"신호 {len(months)}개월 {months[0]}–{months[-1]}, 거래일 {len(context.days)} "
          f"({context.days[0]:%Y-%m-%d}–{context.days[-1]:%Y-%m-%d}), 종목 열 {len(context.tickers)}, 적재 {time.time() - started:.0f}초", flush=True)

    cells = all_cells()
    rows, yearly, episodes = [], [], []
    baseline_run = None
    baseline_monthly = None

    for index, cell in enumerate(cells):
        run = simulate(context, cell, record_segments=(cell.branch == "base"))
        row, year_rows, episode_rows, monthly, benchmark = summarize(context, cell, run)
        rows.append(row)
        yearly += year_rows
        episodes += episode_rows

        if cell.branch == "base":
            baseline_run, baseline_monthly, baseline_benchmark = run, monthly, benchmark

        if index % 40 == 0:
            print(f"  {index + 1}/{len(cells)} {cell.label} 연 {row['annual_return'] * 100:.2f}% 샤프 {row['sharpe']:.2f} "
                  f"낙폭 {row['max_drawdown_percent']:.1f}% ({time.time() - started:.0f}초)", flush=True)

    frame = pd.DataFrame(rows)
    base_row = frame[frame["branch"] == "base"].iloc[0]
    frame["improved"] = (frame["branch"] != "base") & (frame["sharpe"] > base_row["sharpe"]) & (frame["max_drawdown_percent"] < base_row["max_drawdown_percent"])
    frame["neighbors_improved"], frame["neighbor_count"] = neighbor_counts(cells, frame)
    frame["candidate"] = judge(cells, frame, TAKE_PROFIT_GRID[1:] + (None,))
    alternative = judge(cells, frame, TAKE_PROFIT_GRID)
    alternative_count = int(alternative.sum())

    # 정합: 기준 칸을 월 수익으로 다시 재고, 22·23 metrics와 나란히 둔다. 라이브 작성기 규칙(버퍼 없음)도 참고로 한 번 돌린다.
    no_buffer_row = summarize(context, Cell("base"), simulate(context, Cell("base"), buffer_multiple=1))[0]
    study23 = json.loads(STUDY23_METRICS.read_text(encoding="utf-8"))["center_full"]
    study22_metrics = json.loads(STUDY22_METRICS.read_text(encoding="utf-8"))["center_full"]
    monthly_performance = stats.performance_summary(baseline_monthly, 12)
    reconciliation = {
        "daily": {key: base_row[key] for key in ("annual_return", "sharpe", "max_drawdown_percent", "excess_annual", "excess_t")},
        "monthly": {"annual_return": monthly_performance["annual_return"], "sharpe": monthly_performance["sharpe"],
                    "max_drawdown_percent": monthly_performance["max_drawdown_percent"], "excess_annual": base_row["excess_annual"],
                    "excess_t": base_row["excess_t"], "benchmark_annual_return": stats.performance_summary(baseline_benchmark, 12)["annual_return"]},
        "study23": {key: study23[key] for key in ("net_annual_return", "sharpe", "max_drawdown_percent", "excess_annual", "excess_t", "benchmark_annual_return")},
        "study22": {key: study22_metrics[key] for key in ("net_annual_return", "sharpe", "max_drawdown_percent", "excess_annual", "excess_t")},
        "no_buffer": {key: no_buffer_row[key] for key in ("annual_return", "sharpe", "max_drawdown_percent", "excess_annual", "excess_t")},
    }
    reconciliation["note"] = (
        f"월 수익으로 다시 잰 기준 칸은 연 {monthly_performance['annual_return'] * 100:.2f}%·샤프 {monthly_performance['sharpe']:.2f}·낙폭 "
        f"{monthly_performance['max_drawdown_percent']:.1f}%, 스터디 23은 연 {study23['net_annual_return'] * 100:.2f}%·샤프 {study23['sharpe']:.2f}·낙폭 "
        f"{study23['max_drawdown_percent']:.1f}%다. 이 하네스는 분기 사이 달에 리밸하지 않고 일별로 들고 가며, 22·23은 매월 첫 거래일에 그날 봉이 없는 "
        f"보유 종목을 현금으로 돌린다(여기는 분기 리밸 날에만). 일별 샤프·낙폭은 월 단위보다 낙폭이 크게 잡힌다(월 사이 저점까지 본다).")

    mae_mfe = mae_mfe_table(context, baseline_run.segments)
    breadth_forward = breadth_forward_table(context, baseline_run)
    episode_frame = pd.DataFrame(episodes)
    yearly_frame = pd.DataFrame(yearly)

    candidates = frame[frame["candidate"]]
    candidate_branches = sorted(set(candidates["branch"]))

    if not candidate_branches:
        overall = "손절 없음 유지"
    elif candidate_branches == ["S"]:
        overall = "종목별 손절 라이브 후보(리스크 한도 변경, 사용자 결정)"
    else:
        overall = "바스켓 단위 제한 라이브 후보(리스크 한도 변경, 사용자 결정)"

    improved_counts = {branch: int(frame[(frame["branch"] == branch) & frame["improved"]].shape[0]) for branch in ("S", "A", "B")}
    one_line = (f"기준(손절 없음) 연 {base_row['annual_return'] * 100:.2f}%·샤프 {base_row['sharpe']:.2f}·최대 낙폭 {base_row['max_drawdown_percent']:.1f}%. "
                f"개선 칸 S {improved_counts['S']}/108·A {improved_counts['A']}/70·B {improved_counts['B']}/54, "
                f"채택 후보 {len(candidates)}칸({', '.join(candidates['cell']) if len(candidates) else '없음'}).")
    elapsed = round(time.time() - started, 1)
    metrics = {
        "study": "44_value_basket_drawdown_guard/stop_regime_drawdown_233",
        "period": f"{context.days[0]:%Y-%m-%d}~{context.days[-1]:%Y-%m-%d}",
        "sample": {"trading_days": int(len(context.days)), "months": int(len(months)), "rebalances": int(len(context.rebalance_rank)),
                   "holding_segments": int(len(baseline_run.segments)), "universe_size_median": int(np.median([len(signal_frame) for signal_frame in signals.values()]))},
        "sample_n": int(len(context.days)),
        "trials_prior": len(cells),
        "cost_level": COST_LEVEL,
        "cost_spec": {"commission_percent": RESEARCH_BY_LEVEL[COST_LEVEL].commission_percent, "sell_tax_percent": RESEARCH_BY_LEVEL[COST_LEVEL].sell_tax_percent,
                      "slippage_ticks": RESEARCH_BY_LEVEL[COST_LEVEL].slippage_ticks, "impact_percent": RESEARCH_BY_LEVEL[COST_LEVEL].impact_percent},
        "sharpe": base_row["sharpe"],
        "mdd": base_row["max_drawdown_percent"],
        "t_stat": base_row["excess_t"],
        "grade": "B",
        "verdict": {"overall": overall, "one_line": one_line, "candidates": candidates["cell"].tolist(),
                    "improved_cells": improved_counts, "candidate_branches": candidate_branches},
        "candidates_alternative_take_profit_order": alternative_count,
        "candidates_alternative_list": frame.loc[alternative.astype(bool), "cell"].tolist(),
        "baseline": {key: base_row[key] for key in ("annual_return", "sharpe", "max_drawdown_percent", "calmar", "turnover_annual", "cost_annual",
                                                     "excess_annual", "excess_t")},
        "best_by_branch": {branch: frame[frame["branch"] == branch].sort_values("sharpe", ascending=False).head(3)[
            ["cell", "annual_return", "sharpe", "max_drawdown_percent", "calmar", "improved", "candidate"]].to_dict("records") for branch in ("S", "A", "B")},
        "reconciliation": reconciliation,
        "live_parameters": {"top_n": sleeve.top_n, "value_weight": sleeve.value_weight, "turnover_floor_krw": sleeve.turnover_floor_krw,
                            "rebalance_months": REBALANCE_EVERY_MONTHS, "buffer_rank": context.buffer_multiple * context.top_n},
        "benchmark": "같은 달 유니버스 동일가중(체결일 종가 → 다음 달 체결일 종가), 비용 없음",
        "dividends": "제외(일봉에 배당 없음, 보수적)",
        "seed": SEED,
        "git_commit": base.git_commit(),
        "data_fingerprint": base.data_fingerprint(),
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "rerun_command": "py research/studies/44_value_basket_drawdown_guard/run_drawdown_guard.py",
        "elapsed_seconds": elapsed,
    }

    write_csv(frame, "cells.csv")
    write_csv(yearly_frame, "yearly.csv")
    write_csv(mae_mfe, "mae_mfe.csv")
    write_csv(breadth_forward, "breadth_forward.csv")
    write_csv(episode_frame, "drawdown_episodes.csv")
    (STUDY_DIR / "metrics.json").write_bytes(json.dumps(base.clean(metrics), ensure_ascii=False, indent=2).encode("utf-8"))
    write_readme(metrics, frame, episode_frame, mae_mfe, breadth_forward, yearly_frame)
    print(one_line, f"→ {overall}. {elapsed:.0f}초")
    return 0


if __name__ == "__main__":
    sys.exit(main())
