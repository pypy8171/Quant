#!/usr/bin/env python3
"""스터디 37 — DevScale(정배열 눌림목 매수)에 추세 거름("하향이면 안 삼"·"상승일 때만 삼")을 붙이면 나아지는가.

DevScale 하네스는 3분봉 리플레이(PYQuant/backtest/devscale_replay.py)뿐이고 분봉은 1년치라, 2010 – 2026을 보려고
일봉 근사를 새로 만들었다. 시험 목록·고정 조건·통과 기준은 같은 폴더 devscale_trend_filter.txt 머리말에 실행 전에 적었다.

옮긴 규칙(정본 Quant/src/strategy/DeviationScaleStrategy.cpp judge_zone·day_entry_allowed, DevScaleRules.cpp,
Quant/src/universe/MaAlign.cpp, 설정 Quant/config/config_dev_paper.json):
  - 진입: 오늘 가격을 접어 넣은 SMA5 > SMA10 > SMA20, 접은 SMA20 대비 이격 −8 – +5%, 전날 ATR14/SMA20 ≤ 4.5%,
    개장 이격 ≥ −1%, 가격 2,000원 이상(모의 설정, 상한 없음), 전날 거래대금 상위 200. 시가에서 맞으면 시가, 아니면 종가에서 맞으면 종가 체결.
  - 청산(넘김 모드): 익절 평단 +3%(지정가), 손절 −6.5%, 존 이탈(띠 −12 – +9%, 정배열 = 전날 확정 OR 접은 값).
    하루 안에서는 시가 판정 → 저가·고가로 아래쪽·위쪽 선 판정, 둘 다 닿으면 아래쪽 먼저.
  - 추세: swing_definitions.trend_state(확정 저점·고점 k=3·5)의 매수 전날 값.
미래 정보 차단: 진입 판정은 전날까지의 이평·ATR·추세 + 그날 시가(또는 그날 종가로 그 자리 체결)만 쓴다.

    py -X utf8 research/studies/37_swing_trend/devscale_trend_filter.py
"""
from __future__ import annotations

import hashlib
import json
import math
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = STUDY_DIR.parents[2]
sys.path.insert(0, str(STUDY_DIR))
sys.path.insert(0, str(REPOSITORY_ROOT / "PYQuant"))
import swing_definitions as swing  # noqa: E402
from backtest.costs import RESEARCH_LOW, RESEARCH_MID, side_cost_percent_at_price, tick_size  # noqa: E402

BARS_PATH = REPOSITORY_ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
CONFIG_DIR = REPOSITORY_ROOT / "Quant" / "config"
LIVE_LOG_DIR = REPOSITORY_ROOT / "strategies" / "DeviationScale" / "live"
REPORT_PATH = STUDY_DIR / "devscale_trend_filter.txt"
TRADES_PATH = STUDY_DIR / "devscale_trend_trades.parquet"
TRADES_TAKE_PROFIT_5_PATH = STUDY_DIR / "devscale_trend_trades_tp5.parquet"
TRADES_WIDE_PATH = STUDY_DIR / "devscale_trend_trades_wide.parquet"

LOAD_FROM = "2009-01-01"            # 이평·확정점 예열
ENTRY_FROM = pd.Timestamp("2010-01-01")
PERIOD_SPLIT = pd.Timestamp("2018-01-01")
WALK_FORWARD_FIRST_YEAR = 2013

UNIVERSE_TOP = 200
UNIVERSE_TOP_WIDE = 1000           # 참고 표: 라이브 진입의 전날 거래대금 순위 중앙값이 325라 넓힌 후보
PULLBACK_PERCENT = 8.0
ENTRY_UPPER_PERCENT = 5.0
HYSTERESIS_PERCENT = 4.0
ATR_MAX_PERCENT = 4.5
OPEN_DEVIATION_MIN_PERCENT = -1.0
PRICE_MIN = 2_000
PRICE_MAX_LIVE_ACCOUNT = 100_000   # 실계좌 config_live.json max_price. 모의 config_dev_paper.json은 0(상한 없음)
STOP_LOSS_PERCENT = 6.5
TAKE_PROFIT_PERCENT = 3.0
TAKE_PROFIT_REFERENCE_PERCENT = 5.0
NOTIONAL_WON = 500_000.0
ATR_PERIOD = 14
SPANS = (3, 5)
TSTAT_PASS = 2.5

TREND_NAMES = {swing.TREND_UP: "상승", swing.TREND_SIDEWAYS: "횡보", swing.TREND_DOWN: "하향", swing.TREND_UNKNOWN: "모름"}
CELLS = [
    ("C0", "거름 없음", None, None),
    ("C1", "k=3 하향이면 안 삼", 3, "not_down"),
    ("C2", "k=5 하향이면 안 삼", 5, "not_down"),
    ("C3", "k=3 상승일 때만 삼", 3, "up_only"),
    ("C4", "k=5 상승일 때만 삼", 5, "up_only"),
]


# ── 자료 ──────────────────────────────────────────────────────────────────────

def excluded_name_mask(names: pd.Series) -> pd.Series:
    """ETF·ETN·리츠 이름. 라이브 스캐너가 쓰는 같은 표(Quant/config/etf_prefixes.json 등)."""
    prefixes = json.loads((CONFIG_DIR / "etf_prefixes.json").read_text(encoding="utf-8"))
    tokens = json.loads((CONFIG_DIR / "etf_name_tokens.json").read_text(encoding="utf-8"))
    reit_suffixes = json.loads((CONFIG_DIR / "reit_name_suffixes.json").read_text(encoding="utf-8"))
    reit_names = set(json.loads((CONFIG_DIR / "reit_names.json").read_text(encoding="utf-8")))
    text = names.fillna("")
    mask = text.str.startswith(tuple(prefixes))

    for token in tokens:
        mask |= text.str.contains(token, regex=False)

    mask |= text.str.endswith(tuple(reit_suffixes)) | text.isin(reit_names)
    return mask


def load_bars() -> pd.DataFrame:
    columns = ["Date", "Open", "High", "Low", "Close", "Volume", "code", "name"]
    bars = pd.read_parquet(BARS_PATH, columns=columns, filters=[("Date", ">=", pd.Timestamp(LOAD_FROM))])
    bars = bars[(bars["Volume"] > 0) & (bars["Open"] > 0) & (bars["Low"] > 0)]
    bars = bars[~excluded_name_mask(bars["name"])]
    bars = bars.sort_values(["code", "Date"], kind="mergesort").reset_index(drop=True)
    turnover = (bars["Close"].astype(float) * bars["Volume"].astype(float))
    bars["turnover_previous"] = turnover.groupby(bars["code"]).shift(1)
    bars["turnover_rank"] = bars.groupby("Date")["turnover_previous"].rank(ascending=False, method="first")
    bars["in_universe"] = bars["turnover_rank"] <= UNIVERSE_TOP
    bars["in_universe_wide"] = bars["turnover_rank"] <= UNIVERSE_TOP_WIDE
    return bars


# ── 종목 하나의 지표(전날까지) ─────────────────────────────────────────────────

def trailing_sum_previous(values: np.ndarray, window: int) -> np.ndarray:
    """행 t 에 values[t−window .. t−1] 합. 모자라면 NaN."""
    cumulative = np.r_[0.0, np.cumsum(values)]
    result = np.full(len(values), np.nan)

    if len(values) > window:
        rows = np.arange(window, len(values))
        result[window:] = cumulative[rows] - cumulative[rows - window]

    return result


def ticker_features(open_price, high, low, close) -> dict:
    count = len(close)
    sum5 = trailing_sum_previous(close, 5)
    sum10 = trailing_sum_previous(close, 10)
    sum20 = trailing_sum_previous(close, 20)
    # 접기 바탕 = 오늘 가격을 넣으면 빠지는 가장 오래된 종가를 뺀 합(엔진 fold_today와 같은 식)
    fold_base5 = sum5 - np.r_[np.full(5, np.nan), close[:-5]][:count] if count > 5 else np.full(count, np.nan)
    fold_base10 = sum10 - np.r_[np.full(10, np.nan), close[:-10]][:count] if count > 10 else np.full(count, np.nan)
    fold_base20 = sum20 - np.r_[np.full(20, np.nan), close[:-20]][:count] if count > 20 else np.full(count, np.nan)
    previous_close = np.r_[np.nan, close[:-1]]
    true_range = np.nanmax(np.vstack([high - low, np.abs(high - previous_close), np.abs(low - previous_close)]), axis=0)
    true_range[0] = np.nan
    atr_previous = trailing_sum_previous(np.nan_to_num(true_range, nan=0.0), ATR_PERIOD) / ATR_PERIOD
    atr_previous[: ATR_PERIOD + 1] = np.nan
    average20_previous = sum20 / 20.0
    aligned_hold = (sum5 / 5.0 > sum10 / 10.0) & (sum10 / 10.0 > average20_previous)
    # 접은 정배열은 가격에 대해 단조 — 이 값보다 높으면 정배열(MaAlign.cpp aligned, 허용오차 0)
    align_threshold = np.maximum(fold_base10 - 2.0 * fold_base5, fold_base20 - 2.0 * fold_base10)
    with np.errstate(invalid="ignore", divide="ignore"):
        atr_percent = atr_previous / average20_previous * 100.0
        open_deviation_percent = (open_price / average20_previous - 1.0) * 100.0
    return {"fold_base20": fold_base20, "align_threshold": align_threshold, "aligned_hold": aligned_hold,
            "atr_percent": atr_percent, "open_deviation_percent": open_deviation_percent}


def folded_deviation_percent(price, fold_base20):
    """오늘 가격 price 를 접은 SMA20 대비 이격(%)."""
    return (20.0 * price / (fold_base20 + price) - 1.0) * 100.0


def band_price(fold_base20, deviation_percent):
    """접은 SMA20 대비 이격이 정확히 deviation_percent 인 가격(이격은 가격에 대해 증가)."""
    ratio = 1.0 + deviation_percent / 100.0
    return ratio * fold_base20 / (20.0 - ratio)


def entry_zone(price, features, price_max: float) -> np.ndarray:
    deviation = folded_deviation_percent(price, features["fold_base20"])
    with np.errstate(invalid="ignore"):
        return ((price > features["align_threshold"]) & (deviation >= -PULLBACK_PERCENT) & (deviation <= ENTRY_UPPER_PERCENT)
                & (price >= PRICE_MIN) & ((price_max <= 0) | (price <= price_max)))


def ceil_to_tick(price: float) -> float:
    tick = tick_size(price)
    return math.ceil(price / tick - 1e-9) * tick


# ── 매매 재현 ─────────────────────────────────────────────────────────────────

def simulate_ticker(frame: pd.DataFrame, take_profit_percent: float, price_max: float, universe_column: str,
                    collect_flags: bool):
    open_price = frame["Open"].to_numpy(float)
    high = frame["High"].to_numpy(float)
    low = frame["Low"].to_numpy(float)
    close = frame["Close"].to_numpy(float)
    dates = frame["Date"].to_numpy()
    count = len(close)
    features = ticker_features(open_price, high, low, close)
    with np.errstate(invalid="ignore"):
        day_allowed = (frame[universe_column].to_numpy() & (features["atr_percent"] <= ATR_MAX_PERCENT)
                       & (features["open_deviation_percent"] >= OPEN_DEVIATION_MIN_PERCENT)
                       & np.isfinite(features["fold_base20"]) & (dates >= np.datetime64(ENTRY_FROM)))
    open_entry = day_allowed & entry_zone(open_price, features, price_max)
    close_entry = day_allowed & entry_zone(close, features, price_max)
    trend_labels = {}

    for span in SPANS:
        state = swing.trend_state(swing.confirmed_pivots(low, high, span), count)["state"]
        trend_labels[span] = np.r_[swing.TREND_UNKNOWN, state[:-1]].astype(np.int8)   # 매수 전날 마감 기준

    band_low = band_price(features["fold_base20"], -(PULLBACK_PERCENT + HYSTERESIS_PERCENT))
    band_high = band_price(features["fold_base20"], ENTRY_UPPER_PERCENT + HYSTERESIS_PERCENT)
    trades = []
    marks = []
    next_allowed = 0

    for entry_row in np.flatnonzero(open_entry | close_entry):
        if entry_row < next_allowed:
            continue

        entry_kind = "open" if open_entry[entry_row] else "close"
        entry_price = open_price[entry_row] if entry_kind == "open" else close[entry_row]
        take_profit_price = ceil_to_tick(entry_price * (1.0 + take_profit_percent / 100.0))
        stop_price = entry_price * (1.0 - STOP_LOSS_PERCENT / 100.0)
        exit_row, exit_price, reason = -1, float("nan"), ""
        maximum_high, minimum_low = entry_price, entry_price

        for row in range(entry_row, count):
            if row > entry_row:
                price = open_price[row]
                aligned_now = features["aligned_hold"][row] or price > features["align_threshold"][row]
                deviation = folded_deviation_percent(price, features["fold_base20"][row])
                in_hold_band = -(PULLBACK_PERCENT + HYSTERESIS_PERCENT) <= deviation <= ENTRY_UPPER_PERCENT + HYSTERESIS_PERCENT

                if price <= stop_price:
                    exit_row, exit_price, reason = row, price, "stop_gap"
                elif price >= take_profit_price:
                    exit_row, exit_price, reason = row, price, "take_profit_gap"
                elif not (aligned_now and in_hold_band):
                    exit_row, exit_price, reason = row, price, "zone_open"

                if exit_row >= 0:
                    break

            if row == entry_row and entry_kind == "close":
                continue

            maximum_high = max(maximum_high, high[row])
            minimum_low = min(minimum_low, low[row])
            down_lines = [(stop_price, "stop"), (band_low[row], "zone_band_low")]

            if not features["aligned_hold"][row]:
                down_lines.append((features["align_threshold"][row], "zone_align"))

            down_price, down_reason = max(down_lines, key=lambda pair: pair[0])
            up_price, up_reason = min([(take_profit_price, "take_profit"), (band_high[row], "zone_band_high")],
                                      key=lambda pair: pair[0])

            if low[row] <= down_price:
                exit_row, exit_price, reason = row, min(down_price, open_price[row]) if row > entry_row else down_price, down_reason
                break

            if high[row] >= up_price:
                exit_row, exit_price, reason = row, up_price, up_reason
                break

        if exit_row < 0:
            exit_row, exit_price, reason = count - 1, close[count - 1], "end_of_data"

        shares = NOTIONAL_WON / entry_price
        buy_cost = {level: NOTIONAL_WON * side_cost_percent_at_price(specification, "BUY", entry_price) / 100.0
                    for level, specification in (("mid", RESEARCH_MID), ("low", RESEARCH_LOW))}
        sell_cost = {level: shares * exit_price * side_cost_percent_at_price(specification, "SELL", exit_price) / 100.0
                     for level, specification in (("mid", RESEARCH_MID), ("low", RESEARCH_LOW))}
        gross_won = shares * (exit_price - entry_price)
        trade_number = len(trades)
        last_mark = entry_price

        for row in range(entry_row, exit_row + 1):
            mark_price = exit_price if row == exit_row else close[row]
            increment = shares * (mark_price - last_mark)

            if row == entry_row:
                increment -= buy_cost["mid"]

            if row == exit_row:
                increment -= sell_cost["mid"]

            marks.append((trade_number, dates[row], increment))
            last_mark = mark_price

        trades.append({
            "entry_date": dates[entry_row], "entry_kind": entry_kind, "entry_price": entry_price,
            "exit_date": dates[exit_row], "exit_price": exit_price, "exit_reason": reason,
            "hold_rows": int(exit_row - entry_row),
            "net_won_mid": gross_won - buy_cost["mid"] - sell_cost["mid"],
            "net_won_low": gross_won - buy_cost["low"] - sell_cost["low"],
            "maximum_favorable_percent": (maximum_high / entry_price - 1.0) * 100.0,
            "maximum_adverse_percent": (minimum_low / entry_price - 1.0) * 100.0,
            "trend_k3": int(trend_labels[3][entry_row]), "trend_k5": int(trend_labels[5][entry_row]),
        })
        next_allowed = exit_row + 1   # 청산한 날 재진입 없음

    flags = None

    if collect_flags:
        recent = dates >= np.datetime64("2026-09-01")
        flags = pd.DataFrame({"Date": dates[recent], "in_universe": frame["in_universe"].to_numpy()[recent],
                              "day_allowed": day_allowed[recent], "open_entry": open_entry[recent],
                              "close_entry": close_entry[recent]})

    return trades, marks, flags


def run_all(bars: pd.DataFrame, take_profit_percent: float, collect_flags: bool, price_max: float = 0.0,
            universe_column: str = "in_universe"):
    trade_rows, mark_rows, flag_frames = [], [], {}

    for code, frame in bars.groupby("code", sort=True):
        trades, marks, flags = simulate_ticker(frame, take_profit_percent, price_max, universe_column, collect_flags)
        offset = len(trade_rows)

        for trade in trades:
            trade["code"] = code
            trade["name"] = frame["name"].iloc[-1]

        trade_rows.extend(trades)
        mark_rows.extend((number + offset, date, increment) for number, date, increment in marks)

        if flags is not None:
            flag_frames[code] = flags

    trades = pd.DataFrame(trade_rows)
    trades["return_percent_mid"] = trades["net_won_mid"] / NOTIONAL_WON * 100.0
    trades["entry_month"] = pd.to_datetime(trades["entry_date"]).dt.to_period("M").astype(str)
    marks = pd.DataFrame(mark_rows, columns=["trade_number", "Date", "increment"])
    return trades, marks, flag_frames


# ── 통계 ──────────────────────────────────────────────────────────────────────

def keep_mask(trades: pd.DataFrame, span, rule) -> np.ndarray:
    if span is None:
        return np.ones(len(trades), dtype=bool)

    label = trades[f"trend_k{span}"].to_numpy()
    return label != swing.TREND_DOWN if rule == "not_down" else label == swing.TREND_UP


class Curve:
    """매일 종가 평가 누적 손익. 매매 선택(mask)마다 bincount 한 번."""

    def __init__(self, marks: pd.DataFrame, trade_count: int):
        self.calendar = np.sort(marks["Date"].unique())
        self.date_position = np.searchsorted(self.calendar, marks["Date"].to_numpy())
        self.trade_number = marks["trade_number"].to_numpy()
        self.increment = marks["increment"].to_numpy()
        self.trade_count = trade_count

    def daily(self, mask: np.ndarray) -> np.ndarray:
        chosen = mask[self.trade_number]
        return np.bincount(self.date_position[chosen], weights=self.increment[chosen], minlength=len(self.calendar))

    def drawdown(self, mask: np.ndarray) -> float:
        cumulative = np.cumsum(self.daily(mask))
        peak = np.maximum.accumulate(np.r_[0.0, cumulative])[1:]
        return float((peak - cumulative).max()) if len(cumulative) else 0.0


def cluster_difference_t(returns: np.ndarray, removed: np.ndarray, clusters: np.ndarray) -> tuple[float, float]:
    """returns = a + b·removed, 매수 월 묶음 CR1 표준오차. (b, t)."""
    if removed.sum() == 0 or (~removed).sum() == 0:
        return float("nan"), float("nan")

    design = np.column_stack([np.ones(len(returns)), removed.astype(float)])
    inverse = np.linalg.inv(design.T @ design)
    coefficient = inverse @ design.T @ returns
    residual = returns - design @ coefficient
    codes, group = np.unique(clusters, return_inverse=True)
    scores = np.zeros((len(codes), 2))
    np.add.at(scores, group, design * residual[:, None])
    meat = scores.T @ scores
    groups, observations = len(codes), len(returns)
    correction = groups / (groups - 1) * (observations - 1) / (observations - 2)
    variance = correction * inverse @ meat @ inverse
    return float(coefficient[1]), float(coefficient[1] / math.sqrt(variance[1, 1]))


def won_to_ten_thousand(value: float) -> str:
    return f"{value / 10_000:,.0f}만"


def summarize_cell(trades, curve, mask, base_total, base_drawdown) -> dict:
    kept = trades[mask]
    removed = trades[~mask]
    returns = trades["return_percent_mid"].to_numpy()
    difference, tstat = cluster_difference_t(returns, ~mask, trades["entry_month"].to_numpy())
    drawdown = curve.drawdown(mask)
    total = float(kept["net_won_mid"].sum())
    early = pd.to_datetime(trades["entry_date"]) < PERIOD_SPLIT
    periods = {}

    for label, period_mask in (("2010–2017", early.to_numpy()), ("2018–2026", ~early.to_numpy())):
        kept_part = trades[mask & period_mask]["return_percent_mid"]
        removed_part = trades[~mask & period_mask]["return_percent_mid"]
        periods[label] = (len(kept_part), kept_part.mean(), len(removed_part), removed_part.mean() if len(removed_part) else float("nan"))

    return {"n": len(kept), "mean": kept["return_percent_mid"].mean(), "win": (kept["net_won_mid"] > 0).mean() * 100.0,
            "total": total, "total_low": float(kept["net_won_low"].sum()), "drawdown": drawdown,
            "removed_n": len(removed), "removed_mean": removed["return_percent_mid"].mean() if len(removed) else float("nan"),
            "difference": difference, "tstat": tstat, "periods": periods,
            "less_buying": base_total * drawdown / base_drawdown if base_drawdown > 0 else float("nan")}


def walk_forward(trades, curve, masks: dict) -> dict:
    entry_year = pd.to_datetime(trades["entry_date"]).dt.year.to_numpy()
    exit_dates = pd.to_datetime(trades["exit_date"]).to_numpy()
    stitched = np.zeros(len(trades), dtype=bool)
    choices = []

    for year in range(WALK_FORWARD_FIRST_YEAR, int(entry_year.max()) + 1):
        finished_before = exit_dates < np.datetime64(f"{year}-01-01")
        best_name, best_score = None, -float("inf")

        for name, mask in masks.items():
            chosen = mask & finished_before
            total = trades.loc[chosen, "net_won_mid"].sum()
            drawdown = curve.drawdown(chosen)
            score = total / drawdown if drawdown > 0 else float("inf") * np.sign(total)

            if score > best_score + 1e-12:
                best_name, best_score = name, score

        choices.append((year, best_name, best_score))
        stitched |= masks[best_name] & (entry_year == year)

    base_window = masks["C0"] & (entry_year >= WALK_FORWARD_FIRST_YEAR)
    base_total = trades.loc[base_window, "net_won_mid"].sum()
    base_drawdown = curve.drawdown(base_window)
    stitched_total = trades.loc[stitched, "net_won_mid"].sum()
    stitched_drawdown = curve.drawdown(stitched)
    return {"choices": choices, "n": int(stitched.sum()), "total": stitched_total, "drawdown": stitched_drawdown,
            "base_n": int(base_window.sum()), "base_total": base_total, "base_drawdown": base_drawdown,
            "less_buying": base_total * stitched_drawdown / base_drawdown}


# ── 실매매 대조 ───────────────────────────────────────────────────────────────

LIVE_HEADER_RE = re.compile(r"^\*\*(\d{6}) ")
LIVE_BUY_RE = re.compile(r"^- 매수 \d+건 \((\d{2}:\d{2})\) — 정배열눌림진입 이격=(-?[\d.]+)%")


def live_entries() -> pd.DataFrame:
    rows = []

    for path in sorted(LIVE_LOG_DIR.glob("2026-*.md")):
        current = None

        for line in path.read_text(encoding="utf-8").splitlines():
            header = LIVE_HEADER_RE.match(line)

            if header:
                current = header.group(1)
                continue

            buy = LIVE_BUY_RE.match(line)

            if buy and current:
                rows.append({"Date": pd.Timestamp(path.stem), "code": current, "time": buy.group(1),
                             "live_deviation": float(buy.group(2))})

    entries = pd.DataFrame(rows)
    return entries.drop_duplicates(["Date", "code"], keep="first") if len(entries) else entries


def live_cross_check(flag_frames: dict, trades: pd.DataFrame) -> list[str]:
    entries = live_entries()
    entries = entries[entries["Date"] >= pd.Timestamp("2026-09-22")]   # 청산선 정본(09-22) 이후
    simulated = set(zip(pd.to_datetime(trades["entry_date"]), trades["code"]))
    found = {"n": 0, "universe": 0, "day_allowed": 0, "open": 0, "close": 0, "either": 0, "simulated": 0, "missing": 0}
    examples = []

    for entry in entries.itertuples(index=False):
        found["n"] += 1
        frame = flag_frames.get(entry.code)
        row = frame[frame["Date"] == entry.Date] if frame is not None else None

        if row is None or row.empty:
            found["missing"] += 1
            continue

        flags = row.iloc[0]
        found["universe"] += bool(flags["in_universe"])
        found["day_allowed"] += bool(flags["day_allowed"])
        found["open"] += bool(flags["open_entry"])
        found["close"] += bool(flags["close_entry"])
        found["either"] += bool(flags["open_entry"] or flags["close_entry"])
        found["simulated"] += (entry.Date, entry.code) in simulated

        if len(examples) < 8:
            examples.append(f"  {entry.Date:%m-%d} {entry.code} {entry.time} 라이브 이격 {entry.live_deviation:+.1f}% | "
                            f"후보 {'Y' if flags['in_universe'] else 'N'} 하루필터 {'Y' if flags['day_allowed'] else 'N'} "
                            f"시가존 {'Y' if flags['open_entry'] else 'N'} 종가존 {'Y' if flags['close_entry'] else 'N'} "
                            f"재현매수 {'Y' if (entry.Date, entry.code) in simulated else 'N'}")

    total = max(found["n"] - found["missing"], 1)
    lines = [f"라이브 진입(2026-09-22 – 10-06 매매일지, 종목·날 단위) {found['n']}건, 일봉 없음 {found['missing']}건",
             f"  전날 거래대금 상위 200 안: {found['universe']}/{total}  하루 필터 통과: {found['day_allowed']}/{total}",
             f"  시가 존: {found['open']}/{total}  종가 존: {found['close']}/{total}  둘 중 하나: {found['either']}/{total}  "
             f"재현에서 그날 실제 매수: {found['simulated']}/{total}"]
    return lines + examples


MINUTE_REPLAY_TRADES = REPOSITORY_ROOT / "research" / "studies" / "31_devscale_slot_cap" / "slot_cap_trades.tsv"


def minute_replay_check(bars: pd.DataFrame) -> list[str]:
    """스터디 31 3분봉 리플레이(실계좌 설정, 상한 없음 칸 172건)에 같은 추세 꼬리표를 붙여 본다."""
    replay = pd.read_csv(MINUTE_REPLAY_TRADES, sep="	", dtype={"ticker": str})
    replay = replay[replay["slots"] == 0].copy()
    replay["entry_date"] = pd.to_datetime(replay["entry_day"].astype(str))
    replay["return_percent"] = replay["net_pnl"] / replay["cost_basis"] * 100.0
    replay["entry_month"] = replay["entry_date"].dt.to_period("M").astype(str)

    for span in SPANS:
        replay[f"trend_k{span}"] = swing.TREND_UNKNOWN

    for code, part in replay.groupby("ticker"):
        frame = bars[bars["code"] == code]

        if frame.empty:
            continue

        low = frame["Low"].to_numpy(float)
        high = frame["High"].to_numpy(float)
        dates = frame["Date"].to_numpy()
        rows = np.searchsorted(dates, part["entry_date"].to_numpy())   # 매수일 행, 전날 = rows − 1

        for span in SPANS:
            state = swing.trend_state(swing.confirmed_pivots(low, high, span), len(low))["state"]
            replay.loc[part.index, f"trend_k{span}"] = np.where(rows >= 1, state[np.maximum(rows - 1, 0)], swing.TREND_UNKNOWN)

    lines = [f"3분봉 리플레이 {len(replay)}건(2025-09-19 – 2026-09-18, 비용 LOW, 건당 평균 {replay['return_percent'].mean():+.3f}%)"]

    for span in SPANS:
        counts = " · ".join(f"{TREND_NAMES[label]} n {int((replay[f'trend_k{span}'] == label).sum())} 평균 "
                            f"{replay.loc[replay[f'trend_k{span}'] == label, 'return_percent'].mean():+.2f}%"
                            for label in (swing.TREND_UP, swing.TREND_SIDEWAYS, swing.TREND_DOWN, swing.TREND_UNKNOWN)
                            if (replay[f"trend_k{span}"] == label).any())
        lines.append(f"  k={span}: {counts}")

    for name, description, span, rule in CELLS[1:]:
        mask = keep_mask(replay.rename(columns={"return_percent": "return_percent_mid"}), span, rule)
        difference, tstat = cluster_difference_t(replay["return_percent"].to_numpy(), ~mask, replay["entry_month"].to_numpy())
        lines.append(f"  {name} {description}: 남김 n {int(mask.sum())} 합 {won_to_ten_thousand(replay.loc[mask, 'net_pnl'].sum())} · "
                     f"걸러냄 n {int((~mask).sum())} 합 {won_to_ten_thousand(replay.loc[~mask, 'net_pnl'].sum())} · "
                     f"차이 {format_number(difference)}%p · 묶음 t {format_number(tstat)}(13개월이라 참고)")

    return lines


# ── 보고 ──────────────────────────────────────────────────────────────────────

def format_number(value, digits=2) -> str:
    return "nan" if value is None or (isinstance(value, float) and math.isnan(value)) else f"{value:.{digits}f}"


def trend_table(trades: pd.DataFrame) -> list[str]:
    lines = []

    for span in SPANS:
        lines.append(f"k={span} | 추세 | n | 평균 %(MID) | 승률 % | 총 손익 | 2010–17 평균 | 2018–26 평균")
        early = pd.to_datetime(trades["entry_date"]) < PERIOD_SPLIT

        for label in (swing.TREND_UP, swing.TREND_SIDEWAYS, swing.TREND_DOWN, swing.TREND_UNKNOWN):
            part = trades[trades[f"trend_k{span}"] == label]

            if part.empty:
                continue

            lines.append(f"      | {TREND_NAMES[label]} | {len(part)} | {part['return_percent_mid'].mean():+.3f} | "
                         f"{(part['net_won_mid'] > 0).mean() * 100:.1f} | {won_to_ten_thousand(part['net_won_mid'].sum())} | "
                         f"{part[early[part.index]]['return_percent_mid'].mean():+.3f} | "
                         f"{part[~early[part.index]]['return_percent_mid'].mean():+.3f}")

    return lines


def cell_table(trades, curve, judge: bool) -> tuple[list[str], dict, dict]:
    masks = {name: keep_mask(trades, span, rule) for name, _, span, rule in CELLS}
    base_total = float(trades["net_won_mid"].sum())
    base_drawdown = curve.drawdown(masks["C0"])
    lines = ["칸 | 내용 | 남는 n | 평균 % | 승률 % | 총 손익(MID) | 총 손익(LOW) | 최대 낙폭 | 덜 사기 | 거름−덜사기 | "
             "걸러낸 n | 걸러낸 평균 % | 차이(걸러낸−남긴) %p | 묶음 t"]
    summaries = {}

    for name, description, _, _ in CELLS:
        summary = summarize_cell(trades, curve, masks[name], base_total, base_drawdown)
        summaries[name] = summary
        lines.append(f"{name} | {description} | {summary['n']} | {summary['mean']:+.3f} | {summary['win']:.1f} | "
                     f"{won_to_ten_thousand(summary['total'])} | {won_to_ten_thousand(summary['total_low'])} | "
                     f"{won_to_ten_thousand(summary['drawdown'])} | {won_to_ten_thousand(summary['less_buying'])} | "
                     f"{won_to_ten_thousand(summary['total'] - summary['less_buying'])} | {summary['removed_n']} | "
                     f"{format_number(summary['removed_mean'], 3)} | {format_number(summary['difference'], 3)} | "
                     f"{format_number(summary['tstat'])}")

    lines.append("")
    lines.append("두 기간(매수일) | 칸 | 남긴 n | 남긴 평균 % | 걸러낸 n | 걸러낸 평균 % | 걸러낸 쪽이 더 나쁨")

    for name, _, _, _ in CELLS[1:]:
        for label, (kept_n, kept_mean, removed_n, removed_mean) in summaries[name]["periods"].items():
            worse = "예" if removed_n and removed_mean < kept_mean else "아니오"
            lines.append(f"{label} | {name} | {kept_n} | {kept_mean:+.3f} | {removed_n} | {format_number(removed_mean, 3)} | {worse}")

    return lines, summaries, masks


def yearly_table(trades, masks) -> list[str]:
    year = pd.to_datetime(trades["entry_date"]).dt.year
    lines = ["매수 연도 | n | " + " | ".join(f"{name} 총 손익" for name, _, _, _ in CELLS) + " | 거름 없음 평균 %"]

    for value in sorted(year.unique()):
        in_year = (year == value).to_numpy()
        totals = " | ".join(won_to_ten_thousand(trades.loc[masks[name] & in_year, "net_won_mid"].sum()) for name, _, _, _ in CELLS)
        lines.append(f"{value} | {int(in_year.sum())} | {totals} | {trades.loc[in_year, 'return_percent_mid'].mean():+.3f}")

    return lines


def judgement_lines(summaries, walk) -> list[str]:
    walk_pass = walk["total"] > walk["less_buying"]
    lines = [f"전체 기준 4) 걸어가며 고르기 {won_to_ten_thousand(walk['total'])} vs 덜 사기 {won_to_ten_thousand(walk['less_buying'])} → "
             f"{'통과' if walk_pass else '실패'}"]

    for name, description, _, _ in CELLS[1:]:
        summary = summaries[name]
        worse = summary["removed_mean"] < summary["mean"]
        strong = abs(summary["tstat"]) >= TSTAT_PASS
        both = all(removed_n and removed_mean < kept_mean for _, (_, kept_mean, removed_n, removed_mean) in summary["periods"].items())
        verdict = "통과" if worse and strong and both and walk_pass else "기각"
        lines.append(f"{name} {description}: 1) 걸러낸 쪽 더 나쁨 {'예' if worse else '아니오'} · 2) |t| {abs(summary['tstat']):.2f} "
                     f"{'≥' if strong else '<'} {TSTAT_PASS} · 3) 두 기간 같은 방향 {'예' if both else '아니오'} · 4) {'예' if walk_pass else '아니오'} "
                     f"→ {verdict}  (참고: 거름 > 덜 사기 {'예' if summary['total'] > summary['less_buying'] else '아니오'})")

    return lines


def reproduction_lines(bars: pd.DataFrame) -> list[str]:
    def file_hash(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()[:16]

    try:
        commit = subprocess.run(["git", "-C", str(REPOSITORY_ROOT), "rev-parse", "--short", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        commit = "unknown"

    stat = BARS_PATH.stat()
    return [f"git HEAD {commit} · 무작위 없음(seed 불필요)",
            f"일봉 {BARS_PATH.relative_to(REPOSITORY_ROOT).as_posix()} 크기 {stat.st_size} 바이트, 수정 {pd.Timestamp(stat.st_mtime, unit='s'):%Y-%m-%d %H:%M} UTC, "
            f"쓴 행 {len(bars)}(거래량 0·ETF·리츠 뺀 뒤), 종목 {bars['code'].nunique()}, 마지막 날 {bars['Date'].max():%Y-%m-%d}",
            f"swing_definitions.py sha256 앞 16자 {file_hash(STUDY_DIR / 'swing_definitions.py')} · 이 스크립트 {file_hash(Path(__file__))}",
            "재실행: py -X utf8 research/studies/37_swing_trend/devscale_trend_filter.py   (저장소 루트에서)"]


def main() -> int:
    bars = load_bars()
    trades, marks, flag_frames = run_all(bars, TAKE_PROFIT_PERCENT, collect_flags=True)
    curve = Curve(marks, len(trades))
    trades.to_parquet(TRADES_PATH, index=False)
    lines = ["", "[재현 정보]", *reproduction_lines(bars), ""]
    exit_mix = trades["exit_reason"].value_counts(normalize=True).mul(100).round(1).to_dict()
    lines += ["[재현 매매 개요 — 익절 3.0%·손절 6.5%, 비용 MID]",
              f"매매 {len(trades)}건, 매수 {pd.to_datetime(trades['entry_date']).min():%Y-%m-%d} – {pd.to_datetime(trades['entry_date']).max():%Y-%m-%d}, "
              f"시가 매수 {(trades['entry_kind'] == 'open').mean() * 100:.1f}%·종가 매수 {(trades['entry_kind'] == 'close').mean() * 100:.1f}%",
              f"보유 행 중앙값 {trades['hold_rows'].median():.0f}일, 청산 사유 비중 % {exit_mix}",
              f"이긴 매매 MAE(보유 중 최저) 중앙값 {trades.loc[trades['net_won_mid'] > 0, 'maximum_adverse_percent'].median():+.2f}% · "
              f"진 매매 MFE(보유 중 최고) 중앙값 {trades.loc[trades['net_won_mid'] <= 0, 'maximum_favorable_percent'].median():+.2f}%",
              ""]
    lines += ["[실매매 대조 — 진입 규칙이 맞는지]", *live_cross_check(flag_frames, trades), ""]
    lines += ["[추세별 DevScale 매매]", *trend_table(trades), ""]
    table, summaries, masks = cell_table(trades, curve, judge=True)
    lines += ["[시험 칸 — 금액은 매매 1건 50만원 기준 합, 낙폭은 매일 종가 평가]", *table, ""]
    walk = walk_forward(trades, curve, masks)
    lines += ["[걸어가며 고르기 — 해마다 그 해 전에 끝난 매매로 총 손익÷최대 낙폭 최대 후보]",
              "연도별 고른 칸: " + ", ".join(f"{year} {name}" for year, name, _ in walk["choices"]),
              f"이어붙임 n {walk['n']} · 총 손익 {won_to_ten_thousand(walk['total'])} · 최대 낙폭 {won_to_ten_thousand(walk['drawdown'])}",
              f"같은 기간(2013 – 매수) 거름 없음 n {walk['base_n']} · 총 손익 {won_to_ten_thousand(walk['base_total'])} · "
              f"최대 낙폭 {won_to_ten_thousand(walk['base_drawdown'])} → 덜 사기 {won_to_ten_thousand(walk['less_buying'])}", ""]
    lines += ["[연도별 안정성 — 판정에 안 씀]", *yearly_table(trades, masks), ""]
    lines += ["[판정]", *judgement_lines(summaries, walk), ""]

    reference_trades, reference_marks, _ = run_all(bars, TAKE_PROFIT_REFERENCE_PERCENT, collect_flags=False,
                                                   price_max=PRICE_MAX_LIVE_ACCOUNT)
    reference_trades.to_parquet(TRADES_TAKE_PROFIT_5_PATH, index=False)
    reference_table, _, _ = cell_table(reference_trades, Curve(reference_marks, len(reference_trades)), judge=False)
    lines += ["[참고 1 — 실계좌 설정: 익절 5.0%·가격 상한 10만원(판정 안 함)]", *reference_table, ""]
    wide_trades, wide_marks, _ = run_all(bars, TAKE_PROFIT_PERCENT, collect_flags=False, universe_column="in_universe_wide")
    wide_trades.to_parquet(TRADES_WIDE_PATH, index=False)
    wide_table, _, _ = cell_table(wide_trades, Curve(wide_marks, len(wide_trades)), judge=False)
    lines += [f"[참고 2 — 후보를 전날 거래대금 상위 {UNIVERSE_TOP_WIDE}로 넓힘, 익절 3.0%(판정 안 함)]", *wide_table, ""]
    window = (pd.to_datetime(reference_trades["entry_date"]) >= "2025-09-19") & (pd.to_datetime(reference_trades["entry_date"]) <= "2026-09-18")
    lines += ["[참고 3 — 3분봉 리플레이 매매에 같은 추세 꼬리표(판정 안 함)]", *minute_replay_check(bars), ""]
    lines += ["[분봉 리플레이와 같은 1년 대조 — 실계좌 설정, 비용 LOW]",
              f"이 일봉 근사 2025-09-19 – 2026-09-18 매수 {int(window.sum())}건, 건당 평균 "
              f"{(reference_trades.loc[window, 'net_won_low'] / NOTIONAL_WON * 100).mean():+.3f}% "
              "(스터디 31 3분봉 리플레이 무제한 칸: 172건, 건당 +0.72%)", ""]

    header = REPORT_PATH.read_bytes().replace(b"\r\n", b"\n")
    marker = "\n결과\n".encode("utf-8")
    head = header.split(marker)[0] + marker
    REPORT_PATH.write_bytes(head + "\n".join(lines).encode("utf-8") + b"\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
