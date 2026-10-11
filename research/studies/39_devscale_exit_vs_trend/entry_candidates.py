"""스터디 39 진입 후보 C1–C5 마스크(ENTRY_CANDIDATES.md 부록 A 와 같은 코드).

모든 신호 값은 t−1 행(전날 종가까지)에서 계산해 t 행으로 민다. t 행에서 쓰는 것은 그날 시가(가격 범위·진입가)와
그날 코스피 시가 갭(엔진 risk_off 와 같음)뿐이다. C1 만 엔진처럼 장중 존 가격에 사고, C2–C5 는 t 시가 + 1 호가에 산다.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

import run_round3 as round3
import common
import round4_test

round2, exit_test = round3.round2, round3.exit_test
FLOW = exit_test.REPOSITORY / "PYQuant" / "data" / "investor_flow_pit_2010.parquet"
DAILY_CAP = 30          # 내가 정함: C2–C5 는 그날 점수 상위 30 종목까지만 신호로 둔다(보드 안에서)


# ── 보조 ────────────────────────────────────────────────────────────────────

def shift_rows(values: np.ndarray, data, rows: int) -> np.ndarray:
    """같은 종목 안에서 rows 행 앞 값. 앞 행이 모자라면 NaN."""
    panel = data.panel
    out = np.full(values.shape[0], np.nan)
    out[rows:] = values[:-rows]
    offset = np.arange(panel.row_count) - panel.starts[panel.stock_of_row]
    out[offset < rows] = np.nan
    return out


def previous_known(values: np.ndarray, data) -> np.ndarray:
    """t−1 행 값을 t 행에 둔다. 전날 행이 바로 앞 시장일이 아니면(정지 뒤 첫날) NaN."""
    out = round4_test.shift_within_stock(values.astype(np.float64), data.panel.starts)
    out[~data.previous_traded] = np.nan
    return out


def day_rank(values: np.ndarray, data) -> np.ndarray:
    """그 행 날짜에 거래된 전 종목 안 백분위(0 에 가까울수록 작음). NaN 은 NaN."""
    frame = pd.DataFrame({"day": data.panel.market_position, "value": values})
    return frame.groupby("day")["value"].rank(pct=True).to_numpy()


def daily_top(mask: np.ndarray, score: np.ndarray, data, limit_per_day: int = DAILY_CAP) -> np.ndarray:
    """mask 안에서 시장일마다 score 큰 순 limit_per_day 개만 남긴다."""
    rows = np.flatnonzero(mask)
    frame = pd.DataFrame({"row": rows, "day": data.panel.market_position[rows], "score": score[rows]})
    order = frame.groupby("day")["score"].rank(ascending=False, method="first").to_numpy()
    out = np.zeros_like(mask)
    out[rows[order <= limit_per_day]] = True
    return out


def board_pool(data, flags: dict, extra: tuple) -> np.ndarray:
    """run_round3.random_entries 의 대조군 풀과 같은 바탕. 대조군이 같은 날 같은 풀에서 뽑히게 한다."""
    pool = flags["signal_period"] & flags["kospi_gap_gt_-1"] & np.isfinite(data.open)

    with np.errstate(invalid="ignore"):
        pool &= (data.raw_open >= exit_test.MIN_PRICE) & (data.raw_open <= exit_test.MAX_PRICE)

    for name in extra:
        pool &= flags[name]

    return pool


# ── 피처(한 번 계산) ────────────────────────────────────────────────────────

class Features:
    """C2–C5 가 쓰는 t−1 값. 이름 끝 _prev 는 전날 종가까지 확정된 값을 t 행에 둔 것."""

    def __init__(self, data, with_flow: bool = True):
        panel = data.panel
        starts = panel.starts
        close = data.close

        with np.errstate(invalid="ignore", divide="ignore"):
            r1d = close / shift_rows(close, data, 1) - 1.0
            round5 = close / shift_rows(close, data, 5) - 1.0
            r20 = close / shift_rows(close, data, 20) - 1.0
            r60 = close / shift_rows(close, data, 60) - 1.0

        sma120 = common.rolling_mean_known(close, starts, 120, True)
        filled = np.nan_to_num(r1d)
        mean = common.rolling_mean_known(filled, starts, 20, True)
        square = common.rolling_mean_known(filled * filled, starts, 20, True)
        offset = np.arange(panel.row_count) - starts[panel.stock_of_row]
        rv20 = np.sqrt(np.maximum(square - mean * mean, 0.0) * 20.0 / 19.0)
        rv20[offset < 21] = np.nan
        high250 = (pd.Series(data.high).groupby(panel.stock_of_row).rolling(250, min_periods=250).max()
                   .reset_index(level=0, drop=True).sort_index().to_numpy())

        self.close_prev = previous_known(close, data)
        self.sma20_prev = previous_known(data.sma[20], data)
        self.sma60_prev = previous_known(data.sma[60], data)
        self.sma120_prev = previous_known(sma120, data)
        self.r5_prev = previous_known(round5, data)
        self.r20_prev = previous_known(r20, data)
        self.r60_prev = previous_known(r60, data)
        self.rv20_prev = previous_known(rv20, data)
        self.high250_prev = previous_known(high250, data)
        self.r5_rank_prev = previous_known(day_rank(round5, data), data)
        self.r60_rank_prev = previous_known(day_rank(r60, data), data)
        self.rv20_rank_prev = previous_known(day_rank(rv20, data), data)
        self.flow20_prev = np.full(panel.row_count, np.nan)
        self.flow20_rank_prev = np.full(panel.row_count, np.nan)

        if with_flow:
            flow = pd.read_parquet(FLOW, columns=["date", "ticker", "foreign_net_quantity", "institution_net_quantity"])
            flow["date"] = flow["date"].dt.strftime("%Y%m%d").astype(np.int64)
            flow["net"] = (flow["foreign_net_quantity"] + flow["institution_net_quantity"]).astype(np.float64)
            rows = pd.DataFrame({"ticker": np.array(panel.codes)[panel.stock_of_row], "date": panel.dates})
            net = rows.merge(flow[["ticker", "date", "net"]], on=["ticker", "date"], how="left")["net"].to_numpy()
            value = net * panel.raw_close                          # 순매수 금액(무수정 주식 수 × 무수정 종가)
            known = np.isfinite(value).astype(np.float64)
            flow_mean = common.rolling_mean_known(np.nan_to_num(value), starts, 20, True)
            known20 = common.rolling_mean_known(known, starts, 20, True)
            turnover_mean = common.rolling_mean_known(np.nan_to_num(panel.day_turnover), starts, 20, True)

            with np.errstate(invalid="ignore", divide="ignore"):
                flow20 = np.where((known20 >= 1.0) & (turnover_mean > 0), flow_mean / turnover_mean, np.nan)

            self.flow20_prev = previous_known(flow20, data)
            self.flow20_rank_prev = previous_known(day_rank(flow20, data), data)
            self.flow_coverage = float(np.isfinite(value[panel.signal_ok]).mean())


# ── C1: 현행 존의 제거실험(엔진처럼 장중 존 가격에 산다) ─────────────────────

class ZoneView:
    """r2.Engine 과 같은 식, 존 상단·하단만 바꾼다. evaluate 에 view 로 넘긴다."""

    def __init__(self, data, engine, pullback: float, upper: float):
        s19 = data.before[20]
        low_ratio, high_ratio = 1.0 - pullback, 1.0 + upper
        band_low = low_ratio * s19 / (20.0 - low_ratio)
        band_high = high_ratio * s19 / (20.0 - high_ratio)
        floor = np.where(engine.align_floor > 0, engine.align_floor * (1.0 + round2.EPSILON), engine.align_floor + round2.EPSILON)
        self.lower = np.maximum(floor, band_low)
        self.upper = band_high

        with np.errstate(invalid="ignore"):
            self.interval = np.isfinite(s19) & (self.lower <= self.upper) & (self.upper > 0)
            self.touched = self.interval & (data.low <= self.upper) & (data.high >= self.lower)

        inside = (data.open >= self.lower) & (data.open <= self.upper)
        self.entry_mid = np.where(inside, data.open, np.where(data.open > self.upper, self.upper, self.lower))
        self.entry_raw = self.entry_mid * data.unadjust_clean
        self.entry_tick = exit_test.ticks(self.entry_raw, data.panel.dates)
        self.aligned_close = engine.aligned_close
        self.aligned_previous = engine.aligned_previous
        self.folded_open_aligned = engine.folded_open_aligned


def c1_zone(data, engine, flags, extra, upper: float, open_deviation_min: float | None):
    """upper: 존 상단(+0.05 라이브, 0, −0.03). open_deviation_min: −0.01 라이브, None 이면 끔."""
    view = ZoneView(data, engine, exit_test.PULLBACK, upper)

    with np.errstate(invalid="ignore"):
        mask = (flags["signal_period"] & view.interval & view.touched & flags["atr14_le_4.5"] & flags["kospi_gap_gt_-1"]
                & (view.entry_raw >= exit_test.MIN_PRICE) & (view.entry_raw <= exit_test.MAX_PRICE))

        if open_deviation_min is not None:
            mask &= data.open_deviation >= open_deviation_min

    for name in extra:
        mask &= flags[name]

    entries = np.flatnonzero(mask).astype(np.int64)
    open_price = data.open[entries]
    approach = np.where(open_price > view.upper[entries], 2, np.where(open_price < view.lower[entries], 1, 0)).astype(np.int64)
    return entries, approach, view


# ── C2–C5: t 시가에 산다 ────────────────────────────────────────────────────

def open_entries(mask: np.ndarray, score: np.ndarray, data, open_view):
    mask = daily_top(mask & np.isfinite(score), score, data)
    entries = np.flatnonzero(mask).astype(np.int64)
    return entries, np.zeros(len(entries), dtype=np.int64), open_view


def c2_reversal(data, features, pool, open_view, bottom: float, trend_window: int):
    """장기 추세 위 단기 과매도: 전날 5일 수익 하위 bottom AND 전날 종가 > 전날 SMA{trend_window}. 점수 = −5일 수익 백분위."""
    trend = features.sma60_prev if trend_window == 60 else features.sma120_prev

    with np.errstate(invalid="ignore"):
        mask = pool & (features.r5_rank_prev <= bottom) & (features.close_prev > trend)

    return open_entries(mask, -features.r5_rank_prev, data, open_view)


def c3_flow(data, features, pool, open_view, top: float, lookback: int):
    """수급 괴리: 전날 20일 (외국인+기관) 순매수 ÷ 20일 거래대금 상위 top AND 전날 lookback 일 수익 ≤ 0. 점수 = 순매수 백분위."""
    price = features.r5_prev if lookback == 5 else features.r20_prev

    with np.errstate(invalid="ignore"):
        mask = pool & (features.flow20_rank_prev >= 1.0 - top) & (price <= 0.0)

    return open_entries(mask, features.flow20_rank_prev, data, open_view)


def c4_low_volatility(data, features, pool, open_view, bottom: float, pullback: bool):
    """저변동: 전날 20일 실현변동성 하위 bottom. pullback 이면 SMA60 < 전날 종가 < SMA20 도. 점수 = −변동성 백분위."""
    with np.errstate(invalid="ignore"):
        mask = pool & (features.rv20_rank_prev <= bottom)

        if pullback:
            mask &= (features.close_prev < features.sma20_prev) & (features.close_prev > features.sma60_prev)

    return open_entries(mask, -features.rv20_rank_prev, data, open_view)


def c5_high(data, features, pool, open_view, near: float):
    """250 일 고가 근접: 전날 종가 ÷ 전날까지 250 행 최고가 ≥ near. 점수 = 그 비율."""
    with np.errstate(invalid="ignore", divide="ignore"):
        ratio = features.close_prev / features.high250_prev
        mask = pool & (ratio >= near)

    return open_entries(mask, ratio, data, open_view)


# ── 사전 등록 격자(19 칸) ───────────────────────────────────────────────────

VARIANTS = (
    ("C1", "존 상단+5·시가이격≥−1 (라이브 재현)", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.05, -0.01)),
    ("C1", "존 상단+5·시가이격 끔", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.05, None)),
    ("C1", "존 상단0·시가이격≥−1", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.0, -0.01)),
    ("C1", "존 상단0·시가이격 끔", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, 0.0, None)),
    ("C1", "존 상단−3·시가이격≥−1", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, -0.03, -0.01)),
    ("C1", "존 상단−3·시가이격 끔", lambda c: c1_zone(c.data, c.engine, c.flags, c.extra, -0.03, None)),
    ("C2", "5일 하위10%·SMA60 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.10, 60)),
    ("C2", "5일 하위10%·SMA120 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.10, 120)),
    ("C2", "5일 하위20%·SMA60 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.20, 60)),
    ("C2", "5일 하위20%·SMA120 위", lambda c: c2_reversal(c.data, c.features, c.pool, c.open_view, 0.20, 120)),
    ("C3", "순매수 상위10%·5일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.10, 5)),
    ("C3", "순매수 상위10%·20일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.10, 20)),
    ("C3", "순매수 상위20%·5일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.20, 5)),
    ("C3", "순매수 상위20%·20일 ≤0", lambda c: c3_flow(c.data, c.features, c.pool, c.open_view, 0.20, 20)),
    ("C4", "변동성 하위20%·눌림", lambda c: c4_low_volatility(c.data, c.features, c.pool, c.open_view, 0.20, True)),
    ("C4", "변동성 하위30%·눌림", lambda c: c4_low_volatility(c.data, c.features, c.pool, c.open_view, 0.30, True)),
    ("C4", "변동성 하위10%·눌림 없음", lambda c: c4_low_volatility(c.data, c.features, c.pool, c.open_view, 0.10, False)),
    ("C5", "250일 고가 90% 이상", lambda c: c5_high(c.data, c.features, c.pool, c.open_view, 0.90)),
    ("C5", "250일 고가 95% 이상", lambda c: c5_high(c.data, c.features, c.pool, c.open_view, 0.95)),
)
PRIMARY_CELL = {"C1": 55, "C2": 19, "C3": 61, "C4": 61, "C5": 60}   # 4회차 칸 번호(exit_test.build_rules 순서)


class Context:
    """VARIANTS 람다가 읽는 묶음."""

    def __init__(self, data, engine, flags, features, extra):
        self.data, self.engine, self.flags, self.features, self.extra = data, engine, flags, features, extra
        self.pool = board_pool(data, flags, extra)
        self.open_view = round3.OpenEntry(data, engine)
