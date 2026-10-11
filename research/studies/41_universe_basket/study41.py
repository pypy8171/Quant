"""스터디 41 공용 — 입력 읽기, 칸 표 만들기, 시뮬레이터 호출, 통계(달·해 묶음 t, 해마다 표)."""
from __future__ import annotations

import sys
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY.parent / "38_trend_support_realtime"))
sys.path.insert(0, str(STUDY.parent / "39_devscale_exit_vs_trend"))

import common     # noqa: E402  numba 보다 먼저
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402

import basket     # noqa: E402
import build_inputs     # noqa: E402

OUTPUT = STUDY / "results"
CACHE = build_inputs.CACHE
YEAR_DAYS = 250
FLIPS = 1000
SEED = common.SEED
RANDOM_SEEDS = tuple(SEED + number for number in range(1, 21))
UNIVERSE_NAMES = [name for name, _ in build_inputs.UNIVERSES]
UNIVERSE_TEXT = dict(build_inputs.UNIVERSES)
SIGNAL_NAMES = [name for name, _ in build_inputs.SIGNALS]
SIGNAL_TEXT = dict(build_inputs.SIGNALS)
EXEC_TEXT = {0: "같은 날 종가", 1: "다음 날 시가", 2: "다음 날 종가"}
REBALANCE_TEXT = {0: "주간", 1: "월간"}
COLUMNS = ("signal", "ubit", "slots", "rebalance", "execution", "buffer", "capital", "ticks", "cost_on", "integer", "seed")
MIN_YEAR_DAYS = 60


class Inputs:
    def __init__(self):
        data = np.load(CACHE / "inputs.npz", allow_pickle=False)
        self.data = data
        for name in data.files:
            setattr(self, name, data[name])

        self.day_count, self.stock_count = self.close.shape
        self.years = (self.dates // 10000).astype(np.int64)
        self.year_index = self.years - self.years[0]
        self.month_index = (self.year_index * 12 + (self.dates // 100) % 100 - 1).astype(np.int64)
        self.year_count = int(self.year_index.max()) + 1
        self.month_count = int(self.month_index.max()) + 1
        self.previous_20d = self.previous_20d.astype(np.float64)

    def run(self, cells: np.ndarray, contrib: bool = False):
        count = cells.shape[0]
        out_return = np.zeros((count, self.day_count))
        out_bench = np.zeros((count, self.day_count))
        out_scalar = np.zeros((count, basket.SCALARS))
        contribution = np.zeros((count if contrib else 1, self.stock_count))
        basket.run_cells(cells.astype(np.float64), self.open, self.close, self.raw_open, self.raw_close, self.previous_20d,
                         self.lock, self.market, self.stock_last, self.delist_multiplier, self.kospi_open, self.kospi_close,
                         self.kosdaq_open, self.kosdaq_close, self.tax, self.new_tick, self.decision_days, self.weekly_end,
                         self.monthly_end, self.scores, self.universe, out_return, out_bench, out_scalar, contribution, contrib)
        return out_return, out_bench, out_scalar, contribution

    def trade_log(self, cell: np.ndarray, capacity: int = 200_000):
        log_int = np.zeros((capacity, basket.LOG_INT), dtype=np.int64)
        log_float = np.zeros((capacity, basket.LOG_FLOAT))
        out_return = np.zeros(self.day_count)
        out_bench = np.zeros(self.day_count)
        out_scalar = np.zeros(basket.SCALARS)
        contribution = np.zeros(self.stock_count)
        row = cell.astype(np.float64)
        count = basket.simulate_one(int(row[0]), int(row[1]), int(row[2]), int(row[3]), int(row[4]), int(row[5]), row[6], row[7],
                                    int(row[8]), int(row[9]), int(row[10]), self.open, self.close, self.raw_open, self.raw_close,
                                    self.previous_20d, self.lock, self.market, self.stock_last, self.delist_multiplier,
                                    self.kospi_open, self.kospi_close, self.kosdaq_open, self.kosdaq_close, self.tax, self.new_tick,
                                    self.decision_days, self.weekly_end, self.monthly_end, self.scores, self.universe,
                                    out_return, out_bench, out_scalar, contribution, False, log_int, log_float, capacity)
        return log_int[:count], log_float[:count], out_return, out_bench


def cell_table(**axes) -> pd.DataFrame:
    """축 이름 → 값 목록의 곱집합. 순서는 COLUMNS 순서(앞 축이 바깥)."""
    grid = pd.MultiIndex.from_product([axes[name] for name in COLUMNS], names=COLUMNS).to_frame(index=False)
    return grid


def signs(inputs: Inputs) -> tuple[np.ndarray, np.ndarray]:
    generator = np.random.default_rng(SEED)
    signs_month = np.where(generator.random((FLIPS, inputs.month_count)) < 0.5, -1.0, 1.0)
    signs_year = np.where(generator.random((FLIPS, inputs.year_count)) < 0.5, -1.0, 1.0)
    return signs_month, signs_year


def cluster(values: np.ndarray, groups: np.ndarray, size: int) -> tuple[np.ndarray, np.ndarray]:
    """칸 × 날 값 → 칸 × 묶음 합·건수(NaN 뺌)."""
    valid = np.isfinite(values)
    filled = np.where(valid, values, 0.0)
    starts = np.flatnonzero(np.r_[True, groups[1:] != groups[:-1]])

    if np.all(np.diff(groups) >= 0) and len(starts) == size:
        # 묶음이 날짜 순으로 이어져 있으면 구간 합 한 번으로 끝낸다
        return np.add.reduceat(filled, starts, axis=1), np.add.reduceat(valid.astype(np.float64), starts, axis=1)

    sums = np.zeros((values.shape[0], size))
    counts = np.zeros((values.shape[0], size))

    for group in range(size):
        part = groups == group
        sums[:, group] = filled[:, part].sum(axis=1)
        counts[:, group] = valid[:, part].sum(axis=1)

    return sums, counts


def t_values(values: np.ndarray, inputs: Inputs, sign_pair=None):
    """달 t·해 t(관측) 와 뒤집기 행렬(있으면)."""
    import run_round2 as round2
    month_sums, month_counts = cluster(values, inputs.month_index, inputs.month_count)
    year_sums, year_counts = cluster(values, inputs.year_index, inputs.year_count)

    if sign_pair is None:
        empty = np.ones((1, inputs.month_count)), np.ones((1, inputs.year_count))
        t_month, _ = round2.flipped_t(month_sums, month_counts, empty[0])
        t_year, _ = round2.flipped_t(year_sums, year_counts, empty[1])
        return t_month, t_year, None, None

    t_month, flip_month = round2.flipped_t(month_sums, month_counts, sign_pair[0])
    t_year, flip_year = round2.flipped_t(year_sums, year_counts, sign_pair[1])
    return t_month, t_year, flip_month, flip_year


def family_bar(flip_month: np.ndarray, flip_year: np.ndarray) -> dict:
    import run_round2 as round2
    bars = {"month_up": round2.quantile_of_max(flip_month), "month_down": round2.quantile_of_max(-flip_month),
            "year_up": round2.quantile_of_max(flip_year), "year_down": round2.quantile_of_max(-flip_year)}
    bars["family_bar"] = max(bars.values())
    return bars


def yearly(values: np.ndarray, inputs: Inputs) -> tuple[np.ndarray, np.ndarray]:
    """칸 × 해 연환산 평균(유효일 MIN_YEAR_DAYS 미만이면 NaN)과 유효일 수."""
    sums, counts = cluster(values, inputs.year_index, inputs.year_count)

    with np.errstate(invalid="ignore", divide="ignore"):
        annual = np.where(counts >= MIN_YEAR_DAYS, sums / counts * YEAR_DAYS, np.nan)

    return annual, counts


def annual_mean(values: np.ndarray) -> np.ndarray:
    with np.errstate(invalid="ignore"):
        return np.nanmean(values, axis=1) * YEAR_DAYS


def beta_alpha(returns: np.ndarray, bench: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    alpha = np.full(returns.shape[0], np.nan)
    beta = np.full(returns.shape[0], np.nan)

    for cell in range(returns.shape[0]):
        valid = np.isfinite(returns[cell]) & np.isfinite(bench[cell])

        if valid.sum() < 60:
            continue

        bench_part, return_part = bench[cell, valid], returns[cell, valid]
        slope = np.cov(bench_part, return_part, ddof=1)[0, 1] / np.var(bench_part, ddof=1)
        beta[cell] = slope
        alpha[cell] = (return_part - slope * bench_part).mean() * YEAR_DAYS

    return alpha, beta


def curve_statistics(daily: np.ndarray) -> tuple[float, float, float]:
    """(연 복리 수익 %, MDD %, 샤프). NaN 날은 0."""
    values = np.nan_to_num(daily)
    curve = np.cumprod(1.0 + values)
    years = len(values) / YEAR_DAYS
    peak = np.maximum.accumulate(curve)
    sharpe = values.mean() / values.std(ddof=1) * np.sqrt(YEAR_DAYS) if values.std() > 0 else np.nan
    return float((curve[-1] ** (1.0 / years) - 1.0) * 100.0), float(((curve - peak) / peak).min() * 100.0), float(sharpe)
