#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""스터디 33 급등 뒤 눌림 매수 백테스트 — research/studies/33_post_surge_pullback/SPEC.md를 그대로 옮긴다.

시점(look-ahead 차단):
  - 구간마다 그 구간 끝 날까지의 행만 남긴 배열로 계산한다(StockSeries.rows_until). 뒤 구간 시세는 배열에 없다.
  - 지켜보기(watch_event)는 하루씩 앞으로만 걷는다. 그날 종가가 확정된 뒤 판정하고, 체결은 다음 거래일 시가다.
  - 무수정 환산 비율 f는 그 날짜보다 앞선 월말 값만 쓴다(2020-01 이전 날짜는 첫 월말 값을 거꾸로 늘림 — SPEC 1.1 한계).
  - 인자 --open-holdout이 없으면 parquet 읽기 필터가 2022-12-31에서 끊는다. 검증·최근 1년 시세는 읽지도 않는다.
비용: PYQuant/backtest/costs.py LIVE(수수료 0.015%/편, 매도세 0.20%)를 fill_result로 그대로 쓴다. 체결 1틱은 가격에 직접 얹는다.
호가 단위: costs.tick_size는 2023-01-25 개편 표만 있어, 그 전 날짜는 개편 전 코스피·코스닥 표(raw_tick_size)를 쓴다.
난수: C2만. 건마다 numpy default_rng([0, 종목코드, D0 날짜, W]).

재실행(저장소 루트):
  py -X utf8 research/studies/33_post_surge_pullback/backtest.py                  # 개발 구간만
  py -X utf8 research/studies/33_post_surge_pullback/backtest.py --open-holdout   # 검증·최근 1년까지
산출(같은 폴더): events.tsv, trades.tsv, grid.tsv, bands.tsv, regime_year.tsv, portfolio.tsv, metrics.json.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[3]
STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(REPO / "PYQuant"))
sys.path.insert(0, str(REPO / "research" / "studies" / "30_strong_stock_strategies"))
from backtest.costs import LIVE, CostSpec, fill_result, tick_size   # noqa: E402  비용·호가 단위는 라이브 원장과 한 소스
from backtest_b import is_common_stock, load_name_rules   # noqa: E402  ETF·스팩·우선주·리츠 제외는 스터디 30과 같은 규칙

sys.stdout.reconfigure(encoding="utf-8")
SEED = 0

BARS_PATH = REPO / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
DATAGOKR_DIR = REPO / "PYQuant" / "data" / "cache" / "datagokr_shares"
LOAD_FROM = pd.Timestamp("2009-06-01")   # 2010-01 첫 D0의 직전 거래일·20일 평균에 충분한 앞쪽

# SPEC 5절 기간: (이름, D0 시작, D0 끝 = 읽는 시세 끝)
DEVELOPMENT = ("development", 20100101, 20221231)
VALIDATION = ("validation", 20230101, 20250930)
RECENT = ("recent_1y", 20251001, 20260929)

# SPEC 2·4절 고정값
GAINS = (10.0, 15.0)
WINDOWS = (3, 5, 10)
HOLDS = (5, 10)
BASE_CELL = (10.0, 5, 10)
NEIGHBOR_CELLS = ((15.0, 5, 10), (10.0, 3, 10), (10.0, 10, 10), (10.0, 5, 5))
MIN_TURNOVER_KRW = 1e10
MIN_PREVIOUS_CLOSE_KRW = 2_000.0
PULLBACK_BAND = 0.02
PULLBACK_VOLUME_RATIO = 0.5
STOP_BELOW_LOW = 0.99
C2_DRAWS = 200
PORTFOLIO_SLOTS = 10
INDEX_RETURN_CLIP = 0.30
REGIME_AVERAGE_DAYS = 20
FACTOR_TOLERANCE = 0.02
CAP_FIRST_D0 = 20200201    # 상장주식수 첫 월말 2020-01-31 뒤(SPEC 9.2)
PRICE_EPSILON = 1e-6

# 2023-01-25 전 호가 단위(주권). 코스피 7구간, 코스닥 5구간.
TICK_REFORM_DATE = 20230125
OLD_KOSPI_TABLE = ((1_000, 1), (5_000, 5), (10_000, 10), (50_000, 50), (100_000, 100), (500_000, 500))
OLD_KOSPI_ABOVE = 1_000
OLD_KOSDAQ_TABLE = ((1_000, 1), (5_000, 5), (10_000, 10), (50_000, 50))
OLD_KOSDAQ_ABOVE = 100

TAX_030 = CostSpec(commission_percent=0.015, sell_tax_percent=0.30, slippage_ticks=0, impact_percent=0.0)

# SPEC 9절 구간
EOK = 1e8
TURNOVER_BANDS_100 = [(100 + 100 * step, 200 + 100 * step) for step in range(19)] + [(2_000, math.inf)]
TURNOVER_BANDS_200 = [(100 + 200 * step, min(300 + 200 * step, 2_000)) for step in range(10)] + [(2_000, math.inf)]
CAP_BANDS = [(0, 500), (500, 1_000), (1_000, 3_000), (3_000, 10_000), (10_000, math.inf)]   # 억 원
TURNOVER_RATIO_BANDS = [(0, 5), (5, 10), (10, 20), (20, 40), (40, math.inf)]                # %
SMALL_CELL = 30
RULE_PROMOTION_T = 3.1


# ── 호가 단위 ───────────────────────────────────────────────────────────────

def raw_tick_size(raw_price: float, is_kospi: bool, date_int: int) -> float:
    """무수정 가격의 호가 단위. 2023-01-25부터는 costs.tick_size(라이브와 한 소스), 그 전은 개편 전 표."""
    if date_int >= TICK_REFORM_DATE:
        return tick_size(raw_price)

    table, above = (OLD_KOSPI_TABLE, OLD_KOSPI_ABOVE) if is_kospi else (OLD_KOSDAQ_TABLE, OLD_KOSDAQ_ABOVE)

    for upper_bound, tick in table:
        if raw_price < upper_bound:
            return tick

    return above


# ── 종목 시세 ───────────────────────────────────────────────────────────────

@dataclass
class StockSeries:
    """한 종목의 거래된 날(거래량 > 0) 배열. 가격은 수정주가, 거래량은 무수정, factor = 무수정/수정."""
    code: str
    name: str
    delisted: bool
    dates: np.ndarray            # int YYYYMMDD
    open_price: np.ndarray
    high: np.ndarray
    low: np.ndarray
    close: np.ndarray
    volume: np.ndarray
    factor: np.ndarray
    factor_source: np.ndarray    # 0 측정, 1 첫 월말 값을 거꾸로 늘림, 2 없음(1로 둠)
    is_kospi: np.ndarray
    present_dates: np.ndarray    # 거래정지 날까지 포함한, 행이 있는 날
    month_dates: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.int64))
    month_cap: np.ndarray = field(default_factory=lambda: np.array([], dtype=float))
    month_close: np.ndarray = field(default_factory=lambda: np.array([], dtype=float))

    def rows_until(self, cutoff: int) -> int:
        return int(np.searchsorted(self.dates, cutoff, side="right"))

    def ended_before(self, cutoff: int, period_last_date: int) -> bool:
        """구간 끝 전에 행이 끊겼는가(상장폐지·시세 끝). 끝 날에 정지 행이라도 있으면 아직 상장된 것으로 본다."""
        position = int(np.searchsorted(self.present_dates, cutoff, side="right")) - 1
        return position < 0 or int(self.present_dates[position]) < period_last_date

    def tick(self, price: float, row: int, below: bool = False) -> float:
        factor = self.factor[row]
        raw = price * factor - (PRICE_EPSILON if below else 0.0)
        return raw_tick_size(raw, bool(self.is_kospi[row]), int(self.dates[row])) / factor

    def tick_up(self, price: float, row: int) -> float:
        return price + self.tick(price, row)

    def tick_down(self, price: float, row: int) -> float:
        return max(price - self.tick(price, row, below=True), self.tick(price, row) * 0.5)

    def floor_to_tick(self, price: float, row: int) -> float:
        factor = self.factor[row]
        raw = price * factor
        unit = raw_tick_size(raw, bool(self.is_kospi[row]), int(self.dates[row]))
        return math.floor(raw / unit + PRICE_EPSILON) * unit / factor

    def market_cap_at(self, row: int) -> float:
        """시가총액(D0) = 시총(m) × C(D0)/C(m). m은 D0보다 앞선 마지막 월말(SPEC 9.2). 없으면 NaN."""
        date_int = int(self.dates[row])

        if date_int < CAP_FIRST_D0 or len(self.month_dates) == 0:
            return float("nan")

        position = int(np.searchsorted(self.month_dates, date_int, side="left")) - 1

        if position < 0 or self.month_close[position] <= 0:
            return float("nan")

        return float(self.month_cap[position] * self.close[row] / self.month_close[position])


def load_inputs(last_date: int) -> tuple[list[StockSeries], pd.DataFrame, dict]:
    """일봉·월말 무수정 표를 읽어 종목별 배열로 편다. last_date 뒤 행은 읽지 않는다."""
    last_timestamp = pd.Timestamp(str(last_date))
    columns = ["Date", "Open", "High", "Low", "Close", "Volume", "code", "name", "market", "delisted"]
    bars = pd.read_parquet(BARS_PATH, columns=columns,
                           filters=[("Date", ">=", LOAD_FROM), ("Date", "<=", last_timestamp)])
    name_rules = load_name_rules()
    names = bars.drop_duplicates("code").set_index("code")["name"]
    common_codes = sorted(code for code, name in names.items() if is_common_stock(code, name, name_rules))
    bars = bars[bars["code"].isin(common_codes)].sort_values(["code", "Date"], kind="mergesort").reset_index(drop=True)
    bars["date_int"] = bars["Date"].dt.strftime("%Y%m%d").astype(np.int64)

    # 월말 무수정 종가·시총 (SPEC 1·9.2). last_date 뒤 월말 파일은 읽지 않는다.
    frames = []
    files = []

    for path in sorted(DATAGOKR_DIR.glob("univ_*.parquet")):
        stamp = int(path.stem.split("_")[1])

        if stamp <= last_date:
            frames.append(pd.read_parquet(path, columns=["ticker", "close", "market_cap", "date"]))
            files.append(path)

    month = pd.concat(frames, ignore_index=True)
    month = month.merge(bars[["code", "Date", "Close"]], left_on=["ticker", "date"], right_on=["code", "Date"], how="inner")
    month = month[(month["close"] > 0) & (month["Close"] > 0)].copy()
    ratio = month["close"].to_numpy(float) / month["Close"].to_numpy(float)
    month["factor"] = np.where(np.abs(ratio - 1.0) < FACTOR_TOLERANCE, 1.0, ratio)
    month["month_int"] = month["date"].dt.strftime("%Y%m%d").astype(np.int64)
    month = month.sort_values(["ticker", "month_int"], kind="mergesort")
    month_by_code = {code: group for code, group in month.groupby("ticker", sort=True)}

    # 결측 정리(SPEC 1.2): 시가 0 → 전 거래일 종가, 저가 0 → min(시가, 종가), 고가 0 → max(시가, 종가)
    traded = bars[bars["Volume"] > 0].copy()
    previous_close = traded.groupby("code")["Close"].shift(1)
    open_zero = traded["Open"] <= 0
    traded.loc[open_zero, "Open"] = previous_close[open_zero].fillna(traded.loc[open_zero, "Close"])
    low_zero = traded["Low"] <= 0
    traded.loc[low_zero, "Low"] = np.minimum(traded.loc[low_zero, "Open"], traded.loc[low_zero, "Close"])
    high_zero = traded["High"] <= 0
    traded.loc[high_zero, "High"] = np.maximum(traded.loc[high_zero, "Open"], traded.loc[high_zero, "Close"])
    cleaning = {"open_zero_filled": int(open_zero.sum()), "low_zero_filled": int(low_zero.sum()),
                "high_zero_filled": int(high_zero.sum()), "halt_rows_dropped": int((bars["Volume"] <= 0).sum())}

    present_by_code = {code: group.to_numpy(np.int64) for code, group in bars.groupby("code", sort=True)["date_int"]}
    stocks: list[StockSeries] = []

    for code, group in traded.groupby("code", sort=True):
        dates = group["date_int"].to_numpy(np.int64)
        factor = np.ones(len(dates))
        source = np.full(len(dates), 2, dtype=np.int8)
        month_group = month_by_code.get(code)
        series_months = (np.array([], dtype=np.int64), np.array([], dtype=float), np.array([], dtype=float))

        if month_group is not None and len(month_group):
            month_dates = month_group["month_int"].to_numpy(np.int64)
            month_factor = month_group["factor"].to_numpy(float)
            position = np.searchsorted(month_dates, dates, side="left") - 1
            source = np.where(position < 0, 1, 0).astype(np.int8)
            factor = month_factor[np.maximum(position, 0)]
            series_months = (month_dates, month_group["market_cap"].to_numpy(float), month_group["Close"].to_numpy(float))

        stocks.append(StockSeries(
            code=code, name=str(group["name"].iloc[-1]), delisted=bool(group["delisted"].iloc[-1]), dates=dates,
            open_price=group["Open"].to_numpy(float), high=group["High"].to_numpy(float), low=group["Low"].to_numpy(float),
            close=group["Close"].to_numpy(float), volume=group["Volume"].to_numpy(float), factor=factor, factor_source=source,
            is_kospi=(group["market"] == "KOSPI").to_numpy(), present_dates=present_by_code[code],
            month_dates=series_months[0], month_cap=series_months[1], month_close=series_months[2]))

    information = {"bars_rows": int(len(bars)), "common_codes": len(common_codes), "datagokr_files": len(files),
                   "datagokr_first": files[0].stem if files else None, "datagokr_last": files[-1].stem if files else None,
                   "cleaning": cleaning}
    return stocks, traded[["code", "date_int", "Close", "market"]], information


def equal_weight_index(traded: pd.DataFrame, kospi_only: bool) -> pd.Series:
    """보통주 동일가중 지수(일간 수익률 ±30%로 자름). 날짜 int → 지수값."""
    frame = traded[traded["market"] == "KOSPI"] if kospi_only else traded
    returns = frame.groupby("code")["Close"].pct_change().clip(-INDEX_RETURN_CLIP, INDEX_RETURN_CLIP)
    counts = frame.groupby("date_int").size()
    daily = returns.groupby(frame["date_int"]).mean().fillna(0.0)
    daily = daily[counts.reindex(daily.index) >= 100]
    return (1.0 + daily).cumprod()


# ── 규칙 A ──────────────────────────────────────────────────────────────────

def surge_flags(series: StockSeries, rows: int, gain_percent: float) -> np.ndarray:
    """D0 조건(SPEC 2-1). 그날 종가까지의 값만 쓴다."""
    close = series.close[:rows]
    flags = np.zeros(rows, dtype=bool)

    if rows < 2:
        return flags

    change = close[1:] / close[:-1] - 1.0
    turnover = close[1:] * series.factor[1:rows] * series.volume[1:rows]
    previous_raw = close[:-1] * series.factor[:rows - 1]
    flags[1:] = ((change * 100.0 >= gain_percent - 1e-9) & (turnover >= MIN_TURNOVER_KRW)
                 & (previous_raw >= MIN_PREVIOUS_CLOSE_KRW) & (series.open_price[1:rows] > 0))
    return flags


@dataclass
class WatchResult:
    status: str                 # signal | discard_low | replaced | expired | window_cut_period | window_cut_series
    pullback_row: int = -1      # 처음 눌림 표시된 날
    signal_row: int = -1
    lowest_low: float = float("nan")


def watch_event(series: StockSeries, rows: int, surge_row: int, window: int, flags: np.ndarray, ended: bool) -> WatchResult:
    """지켜보기 창을 하루씩 걷는다(SPEC 2-4 순서). 미래 행은 보지 않는다."""
    middle = (series.open_price[surge_row] + series.close[surge_row]) / 2.0
    surge_open = series.open_price[surge_row]
    surge_volume = series.volume[surge_row]
    pullback_row = -1
    lowest_low = math.inf

    for row in range(surge_row + 1, surge_row + window + 1):
        if row >= rows:
            return WatchResult("window_cut_series" if ended else "window_cut_period", pullback_row)

        if series.low[row] < surge_open:
            return WatchResult("discard_low", pullback_row)

        if flags[row]:
            return WatchResult("replaced", pullback_row)

        lowest_low = min(lowest_low, series.low[row])

        if pullback_row >= 0 and series.close[row] > series.high[row - 1]:
            return WatchResult("signal", pullback_row, row, lowest_low)

        if (pullback_row < 0 and abs(series.close[row] - middle) / middle <= PULLBACK_BAND
                and series.volume[row] <= PULLBACK_VOLUME_RATIO * surge_volume):
            pullback_row = row

    return WatchResult("expired", pullback_row)


@dataclass
class ExitResult:
    exit_row: int
    exit_price: float
    reason: str        # stop | take | time | delisted | series_end | open_at_period_end
    closed: bool


def run_exit(series: StockSeries, rows: int, buy_row: int, entry: float, stop: float, target: float, hold: int,
             ended: bool) -> ExitResult:
    """매수일부터 하루씩 손절·익절·시간 청산(SPEC 2-8). 같은 날 둘 다 닿으면 손절."""
    target_on = target > entry

    for row in range(buy_row, buy_row + hold + 1):
        if row >= rows:
            last = rows - 1
            price = series.tick_down(series.close[last], last)

            if ended:
                return ExitResult(last, price, "delisted" if series.delisted else "series_end", True)

            return ExitResult(last, price, "open_at_period_end", False)

        if series.low[row] <= stop:
            return ExitResult(row, series.tick_down(min(stop, series.open_price[row]), row), "stop", True)

        if target_on and series.high[row] >= target:
            base = series.open_price[row] if (row > buy_row and series.open_price[row] >= target) else target
            return ExitResult(row, series.tick_down(base, row), "take", True)

        if row == buy_row + hold:
            return ExitResult(row, series.tick_down(series.close[row], row), "time", True)

    raise AssertionError("도달하지 않는 경로")


def net_percent(entry: float, exit_price: float, specification: CostSpec = LIVE) -> float:
    buy = fill_result("BUY", entry, 1, specification).net
    sell = fill_result("SELL", exit_price, 1, specification).net
    return (sell / buy - 1.0) * 100.0


# ── 성과 ────────────────────────────────────────────────────────────────────

def t_value(values) -> float:
    values = np.asarray(values, dtype=float)

    if len(values) < 2 or values.std(ddof=1) == 0:
        return float("nan")

    return float(values.mean() / (values.std(ddof=1) / math.sqrt(len(values))))


def profit_factor(values: np.ndarray) -> float:
    losses = -values[values < 0].sum()

    if losses <= 0:
        return float("inf") if values[values > 0].sum() > 0 else float("nan")

    return float(values[values > 0].sum() / losses)


def summarize(trades: pd.DataFrame, column: str = "net_pct") -> dict:
    """건 단위 수치. 판정은 하지 않는다."""
    if trades.empty:
        return {"trades": 0}

    values = trades[column].to_numpy(float)
    by_date = trades.groupby("buy_date")[column].mean().to_numpy(float)
    by_month = trades.groupby(trades["buy_date"] // 100)[column].mean().to_numpy(float)
    return {
        "trades": int(len(values)),
        "win_rate_pct": round(float((values > 0).mean() * 100.0), 2),
        "mean_net_pct": round(float(values.mean()), 4),
        "median_net_pct": round(float(np.median(values)), 4),
        "mean_hold_days": round(float(trades["hold_days"].mean()), 2),
        "profit_factor": round(profit_factor(values), 3),
        "t_by_buy_date": round(t_value(by_date), 3),
        "buy_dates": int(len(by_date)),
        "t_by_month": round(t_value(by_month), 3),
    }


def portfolio_curve(trades: pd.DataFrame, references: dict, calendar: np.ndarray) -> tuple[pd.DataFrame, dict]:
    """동시 10종목·평가금액 1/10 포트폴리오의 일별 평가 곡선(보고만, SPEC 6절)."""
    ordered = trades.sort_values(["buy_date", "code", "surge_date"], kind="mergesort")
    entries_by_date: dict[int, list] = {}

    for record in ordered.itertuples(index=False):
        entries_by_date.setdefault(int(record.buy_date), []).append(record)

    cash = 1.0
    positions: dict[str, dict] = {}
    curve = []
    skipped = 0
    taken = 0

    for date_int in calendar:
        date_int = int(date_int)
        equity_before = cash + sum(position["shares"] * position["mark"] for position in positions.values())

        for record in entries_by_date.get(date_int, []):
            if len(positions) >= PORTFOLIO_SLOTS or record.code in positions:
                skipped += 1
                continue

            allocation = min(equity_before / PORTFOLIO_SLOTS, cash)

            if allocation <= 1e-12:
                skipped += 1
                continue

            buy_unit = fill_result("BUY", record.entry, 1, LIVE).net
            shares = allocation / buy_unit
            cash -= allocation
            series = references[record.trade_key]
            positions[record.code] = {"shares": shares, "mark": record.entry, "series": series, "row": int(record.buy_row),
                                      "exit_row": int(record.exit_row), "exit_date": int(record.exit_date),
                                      "exit_price": float(record.exit_price)}
            taken += 1

        for code in sorted(positions):
            position = positions[code]

            if position["exit_date"] == date_int:
                cash += position["shares"] * fill_result("SELL", position["exit_price"], 1, LIVE).net
                del positions[code]
                continue

            series = position["series"]

            while position["row"] < position["exit_row"] and int(series.dates[position["row"]]) <= date_int:
                position["mark"] = float(series.close[position["row"]])
                position["row"] += 1

        equity = cash + sum(position["shares"] * position["mark"] for position in positions.values())
        curve.append((date_int, equity))

    frame = pd.DataFrame(curve, columns=["date", "equity"])

    if frame.empty:
        return frame, {"taken": 0, "skipped_full_or_duplicate": skipped}

    peak = frame["equity"].cummax()
    drawdown = float(((frame["equity"] / peak) - 1.0).min() * 100.0)
    years = max(len(frame) / 250.0, 1e-9)
    final = float(frame["equity"].iloc[-1])
    return frame, {"taken": taken, "skipped_full_or_duplicate": skipped, "final_multiple": round(final, 4),
                   "cagr_pct": round((final ** (1.0 / years) - 1.0) * 100.0, 2) if final > 0 else None,
                   "mdd_pct": round(drawdown, 2)}


# ── 구간 실행 ───────────────────────────────────────────────────────────────

def control_result(series: StockSeries, rows: int, entry_row: int, surge_row: int, width: float, hold: int, ended: bool) -> float:
    """C1·C2 한 건: entry_row 시가 + 1틱 매수, 손절 = 매수가 × (1 − w̄), 익절 H(D0), 시간 H. 구간 끝이면 평가값."""
    entry = series.tick_up(series.open_price[entry_row], entry_row)
    stop = series.floor_to_tick(entry * (1.0 - width), entry_row)
    result = run_exit(series, rows, entry_row, entry, stop, series.high[surge_row], hold, ended)
    return net_percent(entry, result.exit_price)


def run_period(period: tuple, stocks: list[StockSeries], market_index: pd.Series, kospi_index: pd.Series,
               calendar_all: np.ndarray) -> dict:
    period_name, first_date, last_date = period
    calendar = calendar_all[(calendar_all >= first_date) & (calendar_all <= last_date)]
    period_last_trading = int(calendar_all[calendar_all <= last_date].max())
    kospi_average = kospi_index.rolling(REGIME_AVERAGE_DAYS).mean()
    above_average = (kospi_index > kospi_average) & kospi_average.notna()
    event_rows = []
    trade_rows = []
    references: dict[str, StockSeries] = {}
    c1_all: dict[tuple, list] = {}

    for gain in GAINS:
        events = []

        for series in stocks:
            rows = series.rows_until(last_date)
            flags = surge_flags(series, rows, gain)
            ended = series.ended_before(last_date, period_last_trading)

            for surge_row in np.nonzero(flags)[0]:
                surge_date = int(series.dates[surge_row])

                if first_date <= surge_date <= last_date:
                    events.append((series, rows, int(surge_row), flags, ended))

        for series, rows, surge_row, flags, ended in events:
            turnover = series.close[surge_row] * series.factor[surge_row] * series.volume[surge_row]
            market_cap = series.market_cap_at(surge_row)
            record = {"period": period_name, "gain": gain, "code": series.code, "name": series.name,
                      "surge_date": int(series.dates[surge_row]),
                      "surge_change_pct": (series.close[surge_row] / series.close[surge_row - 1] - 1.0) * 100.0,
                      "turnover_eok": turnover / EOK, "market_cap_eok": market_cap / EOK,
                      "factor_source": int(series.factor_source[surge_row]), "delisted": series.delisted}

            for window in WINDOWS:
                watched = watch_event(series, rows, surge_row, window, flags, ended)
                status = watched.status

                if status == "signal" and watched.signal_row + 1 >= rows:
                    status = "no_entry_series_end" if ended else "no_entry_period_end"

                record[f"status_w{window}"] = status

                if status != "signal":
                    continue

                buy_row = watched.signal_row + 1
                entry = series.tick_up(series.open_price[buy_row], buy_row)
                stop = series.floor_to_tick(watched.lowest_low * STOP_BELOW_LOW, watched.signal_row)
                target = float(series.high[surge_row])

                for hold in HOLDS:
                    result = run_exit(series, rows, buy_row, entry, stop, target, hold, ended)
                    trade_key = f"{period_name}|{series.code}"
                    references[trade_key] = series
                    signal_date = int(series.dates[watched.signal_row])
                    trade_rows.append({
                        "period": period_name, "gain": gain, "window": window, "hold": hold, "code": series.code,
                        "name": series.name, "surge_date": record["surge_date"], "surge_change_pct": record["surge_change_pct"],
                        "turnover_eok": record["turnover_eok"], "market_cap_eok": record["market_cap_eok"],
                        "turnover_ratio_pct": turnover / market_cap * 100.0 if market_cap > 0 else float("nan"),
                        "factor_source": record["factor_source"],
                        "pullback_date": int(series.dates[watched.pullback_row]), "signal_date": signal_date,
                        "buy_date": int(series.dates[buy_row]), "exit_date": int(series.dates[result.exit_row]),
                        "entry": entry, "stop": stop, "target": target, "target_on": target > entry,
                        "stop_width_pct": (entry - stop) / entry * 100.0, "exit_price": result.exit_price,
                        "exit_reason": result.reason, "closed": result.closed, "hold_days": result.exit_row - buy_row,
                        "net_pct": net_percent(entry, result.exit_price),
                        "net_pct_tax030": net_percent(entry, result.exit_price, TAX_030),
                        "net_pct_plus1tick": net_percent(series.tick_up(entry, buy_row),
                                                         series.tick_down(result.exit_price, result.exit_row)),
                        "regime": ("above" if bool(above_average.get(signal_date, False)) else "below")
                        if signal_date in kospi_index.index else "unknown",
                        "c3_market_pct": (market_index.get(int(series.dates[result.exit_row]), np.nan)
                                          / market_index.get(signal_date, np.nan) - 1.0) * 100.0,
                        "trade_key": trade_key, "buy_row": buy_row, "exit_row": result.exit_row, "surge_row": surge_row,
                        "rows": rows, "ended": ended,
                    })

            event_rows.append(record)

        # 대조군 C1·C2 — 칸마다 A의 평균 손절 폭 w̄가 정해진 뒤 계산(SPEC 3절)
        events_by_key = {(series.code, surge_row): (series, rows, surge_row, ended) for series, rows, surge_row, _, ended in events}

        for window in WINDOWS:
            for hold in HOLDS:
                cell = [row for row in trade_rows if row["period"] == period_name and row["gain"] == gain
                        and row["window"] == window and row["hold"] == hold]
                closed_widths = [row["stop_width_pct"] for row in cell if row["closed"]]
                width = float(np.mean(closed_widths)) / 100.0 if closed_widths else 0.05

                for row in cell:
                    series, rows, surge_row, ended = events_by_key[(row["code"], row["surge_row"])]
                    row["c_width_pct"] = width * 100.0
                    row["c1_net_pct"] = control_result(series, rows, surge_row + 1, surge_row, width, hold, ended)
                    available = min(window, rows - 1 - surge_row)
                    by_offset = {offset: control_result(series, rows, surge_row + offset, surge_row, width, hold, ended)
                                 for offset in range(1, available + 1)}
                    generator = np.random.default_rng([SEED, int(series.code, 36), int(row["surge_date"]), window])
                    draws = generator.integers(1, window + 1, size=C2_DRAWS)
                    draws = np.minimum(draws, available)
                    row["c2_net_pct"] = float(np.mean([by_offset[int(offset)] for offset in draws]))

                all_values = [control_result(series, rows, surge_row + 1, surge_row, width, hold, ended)
                              for series, rows, surge_row, ended in events_by_key.values() if surge_row + 1 < rows]
                c1_all[(gain, window, hold)] = all_values

    return {"events": pd.DataFrame(event_rows), "trades": pd.DataFrame(trade_rows), "references": references,
            "c1_all": c1_all, "calendar": calendar}


def band_label(low: float, high: float, unit: str) -> str:
    return f"{low:g}{unit}+" if math.isinf(high) else f"{low:g}~{high:g}{unit}"


def band_table(trades: pd.DataFrame, column: str, bands: list, unit: str, kind: str, period_name: str) -> list[dict]:
    rows = []

    for low, high in bands:
        selected = trades[(trades[column] >= low) & (trades[column] < high)]
        rows.append({"kind": kind, "period": period_name, "band": band_label(low, high, unit), "small": len(selected) < SMALL_CELL,
                     **summarize(selected)})

    return rows


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
    path.write_bytes(frame.to_csv(sep="\t", index=False, float_format="%.6f", lineterminator="\n").encode("utf-8"))


def clean_json(value):
    if isinstance(value, dict):
        return {str(key): clean_json(item) for key, item in value.items()}

    if isinstance(value, (list, tuple)):
        return [clean_json(item) for item in value]

    if isinstance(value, (np.floating, float)):
        return None if (math.isnan(value) or math.isinf(value)) else round(float(value), 6)

    if isinstance(value, (np.integer,)):
        return int(value)

    if isinstance(value, np.bool_):
        return bool(value)

    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="스터디 33 급등 뒤 눌림 매수")
    parser.add_argument("--open-holdout", action="store_true", help="검증·최근 1년 시세까지 읽는다")
    arguments = parser.parse_args()
    periods = [DEVELOPMENT] + ([VALIDATION, RECENT] if arguments.open_holdout else [])
    last_date = max(period[2] for period in periods)

    stocks, traded, information = load_inputs(last_date)
    market_index = equal_weight_index(traded, kospi_only=False)
    kospi_index = equal_weight_index(traded, kospi_only=True)
    calendar_all = market_index.index.to_numpy(np.int64)
    print(f"[적재] 보통주 {information['common_codes']}종목, 시세 끝 {last_date}, 월말 파일 {information['datagokr_files']}개")

    all_events, all_trades, grid_rows, band_rows, regime_rows, curves = [], [], [], [], [], []
    metrics: dict = {"periods": {}, "spec": "research/studies/33_post_surge_pullback/SPEC.md", "holdout_opened": arguments.open_holdout}

    for period in periods:
        period_name = period[0]
        result = run_period(period, stocks, market_index, kospi_index, calendar_all)
        events, trades = result["events"], result["trades"]
        all_events.append(events)
        period_metrics: dict = {"events": {}, "cells": {}}

        for gain in GAINS:
            selected = events[events["gain"] == gain]
            period_metrics["events"][f"g{gain:g}"] = {
                "surge_events": int(len(selected)),
                "factor_source": {str(key): int(value) for key, value in selected["factor_source"].value_counts().sort_index().items()},
                **{f"w{window}": {str(key): int(value) for key, value in selected[f"status_w{window}"].value_counts().sort_index().items()}
                   for window in WINDOWS},
            }

        for gain in GAINS:
            for window in WINDOWS:
                for hold in HOLDS:
                    cell = trades[(trades["gain"] == gain) & (trades["window"] == window) & (trades["hold"] == hold)]
                    closed = cell[cell["closed"]]
                    unclosed = cell[~cell["closed"]]
                    _, portfolio = portfolio_curve(cell, result["references"], result["calendar"])
                    summary = summarize(closed)
                    key = f"g{gain:g}_w{window}_h{hold}"
                    entry = {
                        **summary,
                        "unclosed": int(len(unclosed)),
                        "unclosed_mark_mean_net_pct": round(float(unclosed["net_pct"].mean()), 4) if len(unclosed) else None,
                        "exit_reasons": {str(reason): int(count) for reason, count in closed["exit_reason"].value_counts().sort_index().items()},
                        "target_off": int((~closed["target_on"]).sum()),
                        "mean_stop_width_pct": round(float(closed["stop_width_pct"].mean()), 4) if len(closed) else None,
                        "paired_minus_c1": round(float((closed["net_pct"] - closed["c1_net_pct"]).mean()), 4) if len(closed) else None,
                        "paired_minus_c1_t": round(t_value(closed["net_pct"] - closed["c1_net_pct"]), 3),
                        "paired_minus_c2": round(float((closed["net_pct"] - closed["c2_net_pct"]).mean()), 4) if len(closed) else None,
                        "paired_minus_c2_t": round(t_value(closed["net_pct"] - closed["c2_net_pct"]), 3),
                        "c1_mean_net_pct": round(float(closed["c1_net_pct"].mean()), 4) if len(closed) else None,
                        "c2_mean_net_pct": round(float(closed["c2_net_pct"].mean()), 4) if len(closed) else None,
                        "c1_all_events_mean_net_pct": round(float(np.mean(result["c1_all"][(gain, window, hold)])), 4)
                        if result["c1_all"][(gain, window, hold)] else None,
                        "c1_all_events": len(result["c1_all"][(gain, window, hold)]),
                        "c3_market_mean_pct": round(float(closed["c3_market_pct"].mean()), 4) if len(closed) else None,
                        "portfolio": portfolio,
                    }
                    period_metrics["cells"][key] = entry
                    grid_rows.append({"period": period_name, "cell": key, "gain": gain, "window": window, "hold": hold,
                                      **summary, "portfolio_mdd_pct": portfolio.get("mdd_pct"),
                                      "portfolio_cagr_pct": portfolio.get("cagr_pct"), "unclosed": int(len(unclosed)),
                                      "minus_c1": entry["paired_minus_c1"], "minus_c2": entry["paired_minus_c2"],
                                      "c3_market_mean_pct": entry["c3_market_mean_pct"]})

        gain, window, hold = BASE_CELL
        base = trades[(trades["gain"] == gain) & (trades["window"] == window) & (trades["hold"] == hold)]
        base_closed = base[base["closed"]].copy()
        curve, _ = portfolio_curve(base, result["references"], result["calendar"])
        curve.insert(0, "period", period_name)
        curves.append(curve)

        # 감도(SPEC 8절)
        values = np.sort(base_closed["net_pct"].to_numpy(float))[::-1]
        drop = math.ceil(len(values) * 0.01)
        period_metrics["base_sensitivity"] = {
            "tax_030": summarize(base_closed, "net_pct_tax030"),
            "plus_1tick_each_side": summarize(base_closed, "net_pct_plus1tick"),
            "d0_from_2020_02": summarize(base_closed[base_closed["surge_date"] >= CAP_FIRST_D0]),
            "d0_before_2020_02": summarize(base_closed[base_closed["surge_date"] < CAP_FIRST_D0]),
            "dedupe_code_buy_date": summarize(base_closed.drop_duplicates(["code", "buy_date"], keep="first")),
            "without_top_1pct_count": drop,
            "without_top_1pct_mean_net_pct": round(float(values[drop:].mean()), 4) if len(values) > drop else None,
            "factor_source_trades": {str(source): int(count) for source, count in base_closed["factor_source"].value_counts().sort_index().items()},
        }

        # 국면·연도(보고)
        for regime, group in base_closed.groupby("regime", sort=True):
            regime_rows.append({"period": period_name, "split": "regime", "value": regime, **summarize(group)})

        for year, group in base_closed.groupby(base_closed["buy_date"] // 10000, sort=True):
            regime_rows.append({"period": period_name, "split": "year", "value": str(year), **summarize(group)})

        # 구간 표(SPEC 9절, 보고)
        band_rows += band_table(base_closed, "turnover_eok", TURNOVER_BANDS_100, "억", "turnover_100", period_name)
        band_rows += band_table(base_closed, "turnover_eok", TURNOVER_BANDS_200, "억", "turnover_200", period_name)
        capped = base_closed[(base_closed["surge_date"] >= CAP_FIRST_D0) & base_closed["market_cap_eok"].notna()]
        band_rows += band_table(capped, "market_cap_eok", CAP_BANDS, "억", "market_cap", period_name)
        band_rows += band_table(capped, "turnover_ratio_pct", TURNOVER_RATIO_BANDS, "%", "turnover_ratio", period_name)

        for turnover_low, turnover_high in TURNOVER_BANDS_200:
            for cap_low, cap_high in CAP_BANDS:
                selected = capped[(capped["turnover_eok"] >= turnover_low) & (capped["turnover_eok"] < turnover_high)
                                  & (capped["market_cap_eok"] >= cap_low) & (capped["market_cap_eok"] < cap_high)]
                band_rows.append({"kind": "turnover_200_x_market_cap", "period": period_name,
                                  "band": f"{band_label(turnover_low, turnover_high, '억')} | 시총 {band_label(cap_low, cap_high, '억')}",
                                  "small": len(selected) < SMALL_CELL, **summarize(selected)})

        period_metrics["market_cap_axis_trades"] = int(len(capped))
        metrics["periods"][period_name] = period_metrics
        all_trades.append(trades)
        base_summary = period_metrics["cells"][f"g{gain:g}_w{window}_h{hold}"]
        print(f"[{period_name}] 급등 g10 {period_metrics['events']['g10']['surge_events']}건, 기본 칸 매수 {base_summary['trades']}건"
              f"(미청산 {base_summary['unclosed']}), 건당 순 {base_summary.get('mean_net_pct')}%, t {base_summary.get('t_by_buy_date')},"
              f" PF {base_summary.get('profit_factor')}")

    # 합격선 대조(SPEC 7절) — 값과 참/거짓만 적는다
    development = metrics["periods"]["development"]
    base_key = "g10_w5_h10"
    base_development = development["cells"][base_key]
    neighbors = {f"g{gain:g}_w{window}_h{hold}": development["cells"][f"g{gain:g}_w{window}_h{hold}"].get("mean_net_pct") for gain, window, hold in NEIGHBOR_CELLS}
    checks = {
        "1_mean_net_ge_0.30": base_development.get("mean_net_pct") is not None and base_development["mean_net_pct"] >= 0.30,
        "2_t_by_buy_date_ge_2.0": base_development.get("t_by_buy_date") is not None and base_development["t_by_buy_date"] >= 2.0,
        "3_neighbors_positive_ge_3_of_4": sum(1 for value in neighbors.values() if value is not None and value > 0) >= 3,
        "4_paired_c1_c2_positive": (base_development.get("paired_minus_c1") or 0) > 0 and (base_development.get("paired_minus_c2") or 0) > 0,
        "5_without_top_1pct_positive": (development["base_sensitivity"]["without_top_1pct_mean_net_pct"] or 0) > 0,
    }

    if "validation" in metrics["periods"]:
        validation = metrics["periods"]["validation"]["cells"][base_key]
        checks["6_validation_mean_gt_0_and_pf_ge_1.15"] = (validation.get("mean_net_pct") or 0) > 0 and (validation.get("profit_factor") or 0) >= 1.15
        recent = metrics["periods"]["recent_1y"]["cells"][base_key]
        metrics["recent_1y_check_mean_gt_0_and_pf_ge_1.15"] = (recent.get("mean_net_pct") or 0) > 0 and (recent.get("profit_factor") or 0) >= 1.15

    metrics["criteria"] = {"neighbors_mean_net_pct": neighbors, "checks": checks}

    events_frame = pd.concat(all_events, ignore_index=True).sort_values(["period", "gain", "surge_date", "code"], kind="mergesort")
    trades_frame = pd.concat(all_trades, ignore_index=True).drop(columns=["trade_key", "buy_row", "exit_row", "surge_row", "rows", "ended"])
    trades_frame = trades_frame.sort_values(["period", "gain", "window", "hold", "buy_date", "code", "surge_date"], kind="mergesort")
    outputs = {
        "events.tsv": events_frame, "trades.tsv": trades_frame, "grid.tsv": pd.DataFrame(grid_rows),
        "bands.tsv": pd.DataFrame(band_rows), "regime_year.tsv": pd.DataFrame(regime_rows),
        "portfolio.tsv": pd.concat(curves, ignore_index=True),
    }

    for name, frame in outputs.items():
        write_tsv(frame, STUDY / name)

    metrics["reproduce"] = {
        "command": "py -X utf8 research/studies/33_post_surge_pullback/backtest.py" + (" --open-holdout" if arguments.open_holdout else ""),
        "seed": SEED, "commit": git_commit(), "last_date_read": last_date, "inputs": information,
        "bars_sha256": sha256_file(BARS_PATH),
        "costs": {"commission_percent": LIVE.commission_percent, "sell_tax_percent": LIVE.sell_tax_percent, "fill_ticks": 1},
        "outputs_sha256": {name: sha256_file(STUDY / name) for name in outputs},
    }
    (STUDY / "metrics.json").write_bytes(json.dumps(clean_json(metrics), ensure_ascii=False, indent=1).encode("utf-8") + b"\n")
    print("[합격선 대조]", json.dumps(checks, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
