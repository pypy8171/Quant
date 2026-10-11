"""스터디 39 round6 — C5(250일 고가 근접) 재판정(2016칸 가족)과 밤 보유(종가 매수 → 다음 날 시가 매도).

A. C5 h90·95 × 시가·전날 종가 × 청산 63칸 = 252칸을 5회차 U2 32 실행 × 63칸 = 2016칸 가족 기준선으로 다시 판정한다.
   통과선 = max(2016칸 가족 기준선, 3.5).
A2. 분봉이 있는 기간(2025-09–2026-10)에서 15:20 가격으로 낸 C5 신호가 종가 신호와 얼마나 겹치는지, 15:20 신호로 산 매매의
   첫 밤 갭·순수익이 종가 신호와 같은지 본다(서술, t 판정 없음).
B. 청산 종류 5(다음 시장일 시가 − 1호가 매도)를 U2 전체·C5 90·C5 95 풀에 태운다. 손익분기 표(비용 시나리오 S0–S4)를 같이 낸다.
사전 등록: results/round6/choices_log.csv(실행 전 20 행).
실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round6.py
"""
from __future__ import annotations

import gc
import os
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))

import run_round5 as round5     # noqa: E402  round3.book_returns 를 종가 진입용으로 바꿔 끼운다(아래에서 한 번 더 바꾼다)
import common     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402

import round3_test     # noqa: E402
import round4_test     # noqa: E402
import round11_test     # noqa: E402

round4, round3, entry_candidates = round5.round4, round5.round3, round5.entry_candidates
round2, exit_test = round3.round2, round3.exit_test
OUTPUT = STUDY / "results" / "round6"
ROUND4 = STUDY / "results" / "round4"
ROUND5 = STUDY / "results" / "round5"
MINUTE = exit_test.REPOSITORY / "PYQuant" / "data" / "minute"
PASS_FLOOR = 3.5                 # 지시('내가 정함'으로 기록)
KIND_NEXT_OPEN = 5
REASON_NEXT_OPEN = 20
C5_VARIANTS = (17, 18)           # entry_candidates.VARIANTS 안 C5 90%·95%
MINUTE_FIRST, MINUTE_LAST = 20250901, 20261008
PREFILTER = 0.80                 # 15:20 점검에서 분봉을 읽는 범위: 종가 비율 ≥ 0.80(종가와 15:20 차이가 10%p 넘는 날은 빠진다)
SCENARIOS = (("S0 현행", 1, round11_test.COMMISSION, True, True),
             ("S1 호가 0틱", 0, round11_test.COMMISSION, True, True),
             ("S2 S1+충격 0", 0, round11_test.COMMISSION, False, True),
             ("S3 S2+수수료 0.004%", 0, 0.004 / 100, False, True),
             ("S4 S3+매도세 0", 0, 0.004 / 100, False, False))
SHUFFLES = 20


# ── 청산 종류 5: 다음 시장일 시가에 판다 ─────────────────────────────────────

@njit(cache=True)
def exit_rule_r6(kind, tp, sl, parameter, entry, end, approach, fill, open_price, high, low, close, before19, aligned_close,
                 aligned_previous, folded_open, sma20, states, position, listed, delist_multiplier):
    if kind == 5:
        if entry + 1 < end:
            return entry + 1, open_price[entry + 1], 20

        last = end - 1

        if listed:
            return last, close[last], 8

        return last, close[last] * delist_multiplier, 9

    return round5.exit_rule_close_entry(kind, tp, sl, parameter, entry, end, approach, fill, open_price, high, low, close, before19,
                                    aligned_close, aligned_previous, folded_open, sma20, states, position, listed, delist_multiplier)


@njit(parallel=True, cache=True)
def run_exits_r6(entries, approach, fills, ends, listed, delist_multiplier, kinds, tps, sls, parameters, open_price, high, low,
                 close, before19, aligned_close, aligned_previous, folded_open, sma20, states, position):
    count = entries.shape[0]
    rules = kinds.shape[0]
    exit_row = np.empty((count, rules), dtype=np.int64)
    exit_price = np.empty((count, rules))
    reason = np.empty((count, rules), dtype=np.int8)

    for index in prange(count):
        for rule in range(rules):
            row, price, code = exit_rule_r6(kinds[rule], tps[rule], sls[rule], parameters[rule], entries[index], ends[index],
                                            approach[index], fills[index], open_price, high, low, close, before19, aligned_close,
                                            aligned_previous, folded_open, sma20, states, position, listed[index],
                                            delist_multiplier[index])
            exit_row[index, rule] = row
            exit_price[index, rule] = price
            reason[index, rule] = code

    return exit_row, exit_price, reason


@njit(cache=True)
def keep_overnight(candidates, stock_of_row, exit_rows, reasons, ends, cell):
    """own_dedupe 와 같고, 시가에 판 매매(사유 20)는 그날 종가 매수를 막지 않는다(매도 시각이 매수 시각보다 앞)."""
    count = candidates.shape[0]
    keep = np.zeros(count, dtype=np.bool_)
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

        keep[index] = True
        code = reasons[index, cell]

        if code == 8 or code == 9:
            blocked_until = ends[index]
        elif code == 20:
            blocked_until = exit_rows[index, cell] - 1
        else:
            blocked_until = exit_rows[index, cell]

    return keep


# ── 날짜별 장부: 시가에 판 날은 지수도 시가까지만 ───────────────────────────

@njit(parallel=True, cache=True)
def daily_book_r6(keep, entries, index_id, entry_close, exit_row, reason, net, entry_mid, buy_cost, close, position, index_open,
                  index_close, market_count):
    count, cells = keep.shape
    pnl = np.zeros((cells, market_count))
    bench = np.zeros((cells, market_count))
    base = np.zeros((cells, market_count))

    for cell in prange(cells):
        for index in range(count):
            if not keep[index, cell]:
                continue

            first = entries[index]
            last = exit_row[index, cell]
            previous = 1.0

            for row in range(first, last + 1):
                day = position[row]

                if row == last:
                    value = 1.0 + net[index, cell] / 100.0
                else:
                    value = close[row] / entry_mid[index] * buy_cost[index]

                if row == first:
                    if entry_close[index]:
                        index_return = 0.0
                    else:
                        index_return = index_close[index_id[index], day] / index_open[index_id[index], day] - 1.0
                elif row == last and reason[index, cell] == 20:
                    index_return = index_open[index_id[index], day] / index_close[index_id[index], position[row - 1]] - 1.0
                else:
                    index_return = index_close[index_id[index], day] / index_close[index_id[index], position[row - 1]] - 1.0

                pnl[cell, day] += value - previous
                bench[cell, day] += previous * index_return
                base[cell, day] += previous
                previous = value

    return pnl, bench, base


def book_returns_r6(data, result: dict, net: np.ndarray, index_open: np.ndarray, index_close: np.ndarray):
    """run_round5.book_returns_close_aware 와 같고, 사유 20(다음 날 시가 매도) 매도일 지수 수익은 시가 ÷ 전날 종가."""
    panel = data.panel
    count = len(result["entries"])
    index_id = result.get("index_id", np.zeros(count, dtype=np.int64))
    entry_close = result.get("entry_close", np.zeros(count, dtype=np.bool_))
    pnl, bench, base = daily_book_r6(result["keep"], result["entries"], index_id, entry_close, result["exit_row"], result["reason"], net,
                                     result["entry_mid"], result["buy_cost"], data.close, panel.market_position.astype(np.int64),
                                     np.atleast_2d(index_open), np.atleast_2d(index_close), panel.market_count)
    held = base > 0

    with np.errstate(invalid="ignore", divide="ignore"):
        portfolio = np.where(held, pnl / base * 100.0, np.nan)
        index = np.where(held, bench / base * 100.0, np.nan)

    return portfolio, index


round3.book_returns = book_returns_r6     # 사유 20 이 없는 매매는 run_round5 와 값이 같다(C5 본 칸 재현 점검으로 확인)


# ── 보조 ────────────────────────────────────────────────────────────────────

def overnight_rules(rules: pd.DataFrame) -> pd.DataFrame:
    """B 의 청산 표: board_tables 가 라이브 칸을 찾으므로 라이브 칸을 0 번에 두고 1 번에 밤 보유를 둔다."""
    live = rules[rules["rule"] == exit_test.LIVE_RULE].iloc[[0]].copy()
    row = pd.DataFrame([{"rule": "(f) 종가 매수 → 다음 날 시가 매도", "group": "밤 보유", "kind": KIND_NEXT_OPEN, "tp": np.nan, "sl": np.nan,
                         "param": 0}])
    frame = pd.concat([live, row], ignore_index=True)
    frame["cell"] = np.arange(len(frame))
    return frame


def apply_overnight_keep(data, result: dict, cell: int) -> None:
    keep = keep_overnight(result["entries"], data.panel.stock_of_row.astype(np.int64), result["exit_row"], result["reason"],
                          data.stock_end[result["entries"]], cell)
    result["keep"][:, cell] = keep


def cost_scenarios(data, result: dict, cell: int, label: dict) -> tuple[list[dict], pd.DataFrame]:
    """B 매매마다 중간가 밤 수익과 시나리오별 왕복 비용. S0 은 evaluate 순수익과 같아야 한다."""
    keep = result["keep"][:, cell] & (result["reason"][:, cell] == REASON_NEXT_OPEN)
    entry = result["entries"][keep]
    exit_row = result["exit_row"][keep, cell]
    dates = data.panel.dates
    gross = data.open[exit_row] / data.close[entry]
    buy_raw, buy_tick = data.raw_close[entry], data.close_tick[entry]
    sell_raw = data.open[exit_row] * data.unadjust_clean[exit_row]
    sell_tick = exit_test.ticks(sell_raw, dates[exit_row])
    tax = round11_test.sell_tax(dates[exit_row])
    buy_ratio = np.nan_to_num(exit_test.AMOUNT / data.previous_20d[entry])
    sell_base = data.previous_20d[exit_row]
    sell_ratio = np.where(np.isfinite(sell_base) & (np.nan_to_num(sell_base) > 0), exit_test.AMOUNT / np.where(sell_base > 0, sell_base, 1.0), buy_ratio)
    rows, per_trade = [], {"gross_pct": (gross - 1.0) * 100.0}

    for name, ticks_each, commission, impact, taxed in SCENARIOS:
        flat = round11_test.FLAT_IMPACT if impact else 0.0
        linear = round11_test.LINEAR if impact else 0.0
        buy_factor = buy_raw / (buy_raw + ticks_each * buy_tick) / (1.0 + commission + flat) / (1.0 + linear * buy_ratio)
        sell_factor = (np.maximum(sell_raw - ticks_each * sell_tick, 0.0) / sell_raw * (1.0 - commission - (tax if taxed else 0.0) - flat)
                       * np.maximum(0.0, 1.0 - linear * sell_ratio))
        net = (gross * buy_factor * sell_factor - 1.0) * 100.0
        cost = (gross - 1.0) * 100.0 - net

        if name.startswith("S0"):
            gap = float(np.nanmax(np.abs(net - result["net"][keep, cell]))) if keep.any() else 0.0

            if gap > 1e-9:
                raise SystemExit(f"비용 분해 S0 이 evaluate 순수익과 다르다({gap:.3e})")

        per_trade[name] = net
        rows.append(dict(label, scenario=name, trades=int(keep.sum()), overnight_gross_mean_pct=float(np.nanmean(gross - 1.0) * 100.0),
                         cost_mean_pct=float(np.nanmean(cost)), net_mean_pct=float(np.nanmean(net)),
                         net_se_pct=float(np.nanstd(net, ddof=1) / np.sqrt(max(keep.sum(), 1))),
                         win_rate=float(np.nanmean(net > 0)), breakeven_gross_pct=float(np.nanmean(cost)),
                         possible=bool(np.nanmean(gross - 1.0) * 100.0 > np.nanmean(cost))))

    return rows, pd.DataFrame(per_trade)


def series_of(daily: dict, cells) -> tuple[np.ndarray, np.ndarray]:
    portfolio = daily["portfolio"][cells]
    return (portfolio - daily["kospi"][cells]).astype(np.float64), (portfolio - daily["control"][cells]).astype(np.float64)


def versus_round4(portfolio: np.ndarray, before: np.ndarray, groups, signs) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    month_of_day, year_of_day, month_count, year_count = groups
    difference = portfolio - before
    t_month, t_year, _ = round3.family_t(difference, month_of_day, year_of_day, month_count, year_count, signs[0], signs[1],
                                     np.ones(len(difference), dtype=bool))
    return np.nanmean(difference, axis=1) * round3.YEAR_DAYS, np.fmin(t_month, t_year), np.isfinite(difference).sum(axis=1)


def shuffled_slots(data, result: dict, net: np.ndarray, cell: int, first_day: int, last_day: int, years: float) -> list[float]:
    """같은 날 안 순서를 seed 20261010+k 로 섞은 슬롯 연수익(통과 칸에만)."""
    panel = data.panel
    exit_day = panel.market_position[result["exit_row"][:, [cell]]].astype(np.int64)
    values = []

    for shuffle_number in range(1, SHUFFLES + 1):
        key = np.random.default_rng(common.SEED + shuffle_number).random(len(result["entries"]))
        keep = result["keep"][:, cell]
        net_cell = np.where(keep, net[:, cell], np.nan)[:, None]
        order = np.lexsort((key, result["day"]))
        order = order[keep[order]].astype(np.int64)
        notional = round3.slot_cash(order, result["day"], exit_day, result["stock"], panel.stock_count, np.nan_to_num(net_cell), round3.SLOTS,
                                round3.CAPITAL, exit_test.AMOUNT, round3.MIN_ORDER)
        delta = round3.slot_value(notional, result["entries"], result["exit_row"][:, [cell]], np.nan_to_num(net_cell), result["entry_mid"],
                              result["buy_cost"], data.close, panel.market_position.astype(np.int64), panel.market_count)
        equity = round3.CAPITAL + np.cumsum(delta[:, first_day:last_day + 1], axis=1)
        values.append(round3.curve_statistics(equity[0], years)[0])

    return values


# ── A2: 15:20 신호 ──────────────────────────────────────────────────────────

def minute_1520(code: str, date: int):
    path = MINUTE / code / f"{date}.parquet"

    try:
        frame = pd.read_parquet(path, columns=["time", "high", "close"])
    except (FileNotFoundError, OSError):
        return None

    part = frame[frame["time"].astype(str) < "1520"]

    if part.empty or part["time"].astype(str).max() < "1500":
        return None

    return float(part["close"].iloc[-1]), float(part["high"].max())


def close_vs_1520(data, features, pool_u2: np.ndarray, rules: pd.DataFrame, close_view) -> tuple[pd.DataFrame, pd.DataFrame, dict]:
    panel = data.panel
    codes = np.array(panel.codes)
    stock_of = {code: index for index, code in enumerate(panel.codes)}
    starts = panel.starts
    bounds = np.append(starts, panel.row_count)
    high249 = (pd.Series(data.high).groupby(panel.stock_of_row).rolling(249, min_periods=249).max()
               .reset_index(level=0, drop=True).sort_index().to_numpy())
    high249_before = round4_test.shift_within_stock(high249, starts)           # 행 d 에 d−249..d−1 최고가
    files = 0
    rows = []

    for entry in os.scandir(MINUTE):
        if not entry.is_dir() or entry.name not in stock_of:
            continue

        stock = stock_of[entry.name]
        dates = panel.dates[bounds[stock]:bounds[stock + 1]]

        for item in os.scandir(entry.path):
            if not item.name.endswith(".parquet"):
                continue

            date = int(item.name[:8])

            if date < MINUTE_FIRST or date > MINUTE_LAST:
                continue

            files += 1
            where = int(np.searchsorted(dates, date))

            if where < len(dates) and dates[where] == date:
                rows.append(bounds[stock] + where)

    rows = np.array(sorted(rows), dtype=np.int64)
    nxt = rows + 1
    same = (nxt < panel.row_count)
    same[same] = panel.stock_of_row[nxt[same]] == panel.stock_of_row[rows[same]]
    rows, nxt = rows[same], nxt[same]
    in_pool = pool_u2[nxt]
    rows, nxt = rows[in_pool], nxt[in_pool]

    with np.errstate(invalid="ignore", divide="ignore"):
        ratio_close = features.close_prev[nxt] / features.high250_prev[nxt]

    read = np.isfinite(ratio_close) & (ratio_close >= PREFILTER) & np.isfinite(high249_before[rows])
    ratio_1520 = np.full(len(rows), np.nan)
    missing = 0

    for position in np.flatnonzero(read):
        row = rows[position]
        minute_prices = minute_1520(codes[panel.stock_of_row[row]], int(panel.dates[row]))

        if minute_prices is None:
            missing += 1
            continue

        price, high_so_far = minute_prices[0] / data.unadjust_clean[row], minute_prices[1] / data.unadjust_clean[row]
        ratio_1520[position] = price / max(high249_before[row], high_so_far)

    pick = rules.set_index("cell").loc[[60, 32]].reset_index()          # 0 = 칸 60 (d) E4j10, 1 = 칸 32 익절20·손절10
    extra = overnight_rules(rules).iloc[[1]]
    small = pd.concat([pick, extra], ignore_index=True)
    small["cell"] = np.arange(len(small))
    names = {0: small.at[0, "rule"], 1: small.at[1, "rule"], 2: small.at[2, "rule"]}
    overlap, outcome = [], []
    usable = np.isfinite(ratio_1520) | (~read)

    for near in (0.90, 0.95):
        close_signal = np.isfinite(ratio_close) & (ratio_close >= near) & usable
        signal_1520 = np.isfinite(ratio_1520) & (ratio_1520 >= near)
        overlap.append({"near": near, "pool_pairs": int(len(rows)), "read_pairs": int(read.sum()), "unreadable": missing,
                        "close_signal_A": int(close_signal.sum()), "signal_1520_B": int(signal_1520.sum()), "both": int((close_signal & signal_1520).sum()),
                        "A_only": int((close_signal & ~signal_1520).sum()), "B_only": int((signal_1520 & ~close_signal).sum()),
                        "share_of_A_in_B": float((close_signal & signal_1520).sum() / max(close_signal.sum(), 1)), "share_of_B_in_A": float((close_signal & signal_1520).sum() / max(signal_1520.sum(), 1))})
        sets = (("A 종가 신호", close_signal), ("B 15:20 신호", signal_1520), ("A∩B", close_signal & signal_1520), ("A만", close_signal & ~signal_1520), ("B만", signal_1520 & ~close_signal), ("덮는 풀 전체", usable))

        for name, mask in sets:
            entries = rows[mask]

            if len(entries) == 0:
                outcome.append({"near": near, "set": name, "trades": 0})
                continue

            result = round3.evaluate(data, close_view, small, entries, np.full(len(entries), 3, dtype=np.int64), runner=run_exits_r6)
            gap = (data.open[entries + 1] / data.close[entries] - 1.0) * 100.0
            record = {"near": near, "set": name, "trades": int(len(entries)), "first_gap_mean_pct": float(np.nanmean(gap)),
                      "first_gap_se_pct": float(np.nanstd(gap, ddof=1) / np.sqrt(len(entries))) if len(entries) > 1 else np.nan}

            for cell, short in ((2, "overnight"), (0, "cell60"), (1, "cell32")):
                values = result["net"][:, cell]
                record[f"{short}_net_mean_pct"] = float(np.nanmean(values))
                record[f"{short}_net_se_pct"] = float(np.nanstd(values, ddof=1) / np.sqrt(len(values))) if len(values) > 1 else np.nan
                record[f"{short}_unclosed"] = int((result["reason"][:, cell] == 8).sum())

            outcome.append(record)

    info = {"minute_files": files, "pairs_with_panel_row": int(len(rows)), "cells": names, "prefilter": PREFILTER}
    return pd.DataFrame(overlap), pd.DataFrame(outcome), info


# ── 본 실행 ─────────────────────────────────────────────────────────────────

def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    rules = exit_test.build_rules()
    rules_b = overnight_rules(rules)
    data = exit_test.Data()
    panel = data.panel
    exit_test.MONTHS = panel.month_count
    engine = round2.Engine(data)
    flags = round2.flags_of(data, engine)
    features = entry_candidates.Features(data)
    kospi_open, kospi_close, _ = round4.index_daily(exit_test.KOSPI, panel.market_dates)
    kosdaq_open, kosdaq_close, _ = round4.index_daily(round4.KOSDAQ, panel.market_dates)
    index_open, index_close = np.vstack([kospi_open, kosdaq_open]), np.vstack([kospi_close, kosdaq_close])
    market_of_stock = round4.stock_markets(panel.codes)
    first_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_FIRST))
    last_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_LAST, side="right")) - 1
    years = ((pd.Timestamp(str(exit_test.SIGNAL_LAST)) - pd.Timestamp(str(exit_test.SIGNAL_FIRST))).days) / 365.25
    kospi_curve = round3.CAPITAL * kospi_close[first_day:last_day + 1] / kospi_open[first_day]
    kosdaq_curve = round3.CAPITAL * kosdaq_close[first_day:last_day + 1] / kosdaq_open[first_day]
    kosdaq_annual, kosdaq_mdd = round3.curve_statistics(kosdaq_curve, years)
    market_years = (panel.market_dates // 10000).astype(np.int64)
    year_of_day = np.clip(market_years - panel.first_year, 0, None)
    month_of_day = np.clip(year_of_day * 12 + (panel.market_dates // 100) % 100 - 1, 0, None).astype(np.int64)
    month_count, year_count = int(month_of_day.max()) + 1, int(year_of_day.max()) + 1
    generator = np.random.default_rng(common.SEED)
    signs_month = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    signs_year = np.where(generator.random((round3_test.FLIPS, year_count)) < 0.5, -1.0, 1.0)
    groups = (month_of_day, year_of_day, month_count, year_count)
    signs = (signs_month, signs_year)
    days = np.arange(first_day, last_day + 1)
    round4_daily = pd.read_parquet(ROUND4 / "daily_returns.parquet")
    round4_wide = round4_daily.pivot(index="cell", columns="date", values="portfolio_pct").reindex(columns=panel.market_dates).to_numpy()
    round5_judge = pd.read_csv(ROUND5 / "judge_table.csv")
    close_view = round5.CloseView(data, engine)
    print(f"준비 {time.time() - started:.0f}초", flush=True)

    family_series = {}                 # (valuation) → [index 값 목록, random 값 목록]
    a_tables, a_years, a_equity, a_daily, a_round4 = [], [], {}, {}, []
    b_records, b_series, b_tables, cost_rows, cost_frames, b_years, b_equity = [], {}, [], [], [], [], {}
    u0_tables = []
    replication = {}
    shuffle_inputs = {}
    a2 = None

    for board, extra in round3.BOARDS:
        context = entry_candidates.Context(data, engine, flags, features, extra)
        close_context = entry_candidates.Context(data, engine, flags, features, extra)
        close_context.pool = round5.close_pool(data, flags, extra, features)

        if board == "U2":
            a2 = close_vs_1520(data, features, close_context.pool, rules, close_view)
            print(f"15:20 점검 끝 ({time.time() - started:.0f}초)", flush=True)

        # A: 5회차 32 실행(U2) — 63칸 전부 모은다. U0 는 C5 만.
        for variant, (name, label, build) in enumerate(entry_candidates.VARIANTS):
            if board == "U0" and variant not in C5_VARIANTS:
                continue

            timings = ("open",) if name == "C1" else ("open", "close")

            for timing in timings:
                run = f"{board}|{variant + 1:02d}|{timing}"

                if timing == "open":
                    entries, approach, view = build(context)
                    control_rows, _ = round3.random_entries(data, flags, extra, entries)
                    control_approach = np.zeros(len(control_rows), dtype=np.int64)
                    control_view = context.open_view
                    runner = round4.run_exits_next_day
                else:
                    signal_rows, _, _ = build(close_context)
                    entries = signal_rows - 1
                    approach = np.full(len(entries), 3, dtype=np.int64)
                    view = close_view
                    control_signal, _ = round5.random_from_pool(data, close_context.pool, signal_rows)
                    control_rows = control_signal - 1
                    control_approach = np.full(len(control_rows), 3, dtype=np.int64)
                    control_view = close_view
                    runner = round5.run_exits_close_entry

                strategy = round3.evaluate(data, view, rules, entries, approach, runner=runner)
                control = round3.evaluate(data, control_view, rules, control_rows, control_approach, runner=runner)
                strategy["index_id"] = market_of_stock[strategy["stock"]]
                control["index_id"] = market_of_stock[control["stock"]]
                strategy["entry_close"] = np.full(len(entries), timing == "close")
                control["entry_close"] = np.full(len(control_rows), timing == "close")
                label_row = {"run": run, "board": board, "variant": variant, "variant_no": variant + 1, "candidate": name,
                             "variant_label": label, "timing": timing}
                is_c5 = variant in C5_VARIANTS

                for valuation, multiplier in round3.VALUATIONS:
                    if board == "U0" and valuation != "종가":
                        continue

                    table, year_table, bars, equity, daily = round3.board_tables(data, rules, strategy, control, multiplier, index_open,
                                                                             index_close, groups, signs, first_day, last_day, years,
                                                                             kospi_curve)
                    table = round4.renamed(round4.with_kosdaq_hold(table, kosdaq_annual, kosdaq_mdd)).assign(valuation=valuation, **label_row)

                    if board == "U0":
                        u0_tables.append(table)
                        continue

                    index_values, random_values = series_of(daily, slice(None))
                    family_series.setdefault(valuation, ([], []))
                    family_series[valuation][0].append(index_values.astype(np.float32))
                    family_series[valuation][1].append(random_values.astype(np.float32))

                    if is_c5:
                        table["bar_run_index"] = bars["kospi"]["main_bar"]
                        table["bar_run_random"] = bars["random"]["main_bar"]
                        a_tables.append(table)

                        if valuation == "종가":
                            a_years.append(round4.renamed(year_table).assign(**label_row))
                            a_equity[run] = equity
                            a_daily[run] = daily["portfolio"][:, days].astype(np.float32)
                            annual, t_min, count = versus_round4(daily["portfolio"], round4_wide, groups, signs)
                            a_round4.append(pd.DataFrame({"run": run, "cell": rules["cell"], "versus_round4_annual_pct": annual,
                                                          "versus_round4_t_min": t_min, "versus_round4_days": count}))
                            shuffle_inputs[run] = (strategy, round3.valued(strategy, 1.0))
                            primary = entry_candidates.PRIMARY_CELL["C5"]
                            mine = table.set_index("cell").loc[primary]
                            old = round5_judge[(round5_judge["run"] == run)].iloc[0]
                            gap = max(abs(float(mine[c]) - float(old[c])) / max(1.0, abs(float(old[c])))
                                      for column in ("trades", "index_t_min", "random_t_min", "slot_annual_pct", "index_annual_pct"))
                            replication[run] = gap
                            print(f"재현 점검 {run}: 5회차 judge_table 과 최대 상대 차이 {gap:.3e}", flush=True)

                            if gap > 1e-8:
                                raise SystemExit("C5 본 칸이 5회차와 다르다 — 나머지를 돌리지 않는다")

                print(f"A {run} {label}: 신호 {len(entries)} ({time.time() - started:.0f}초)", flush=True)

                if not (is_c5 and board == "U2"):
                    del strategy
                del control
                gc.collect()

        # B: 밤 보유
        for pool_name in ("U2 전체" if board == "U2" else "U0 전체", "C5 90%", "C5 95%"):
            run = f"{board}|B|{pool_name}"

            if pool_name.endswith("전체"):
                signal_rows = np.flatnonzero(close_context.pool).astype(np.int64)
            else:
                variant = C5_VARIANTS[0] if "90" in pool_name else C5_VARIANTS[1]
                signal_rows, _, _ = entry_candidates.VARIANTS[variant][2](close_context)

            entries = signal_rows - 1
            control_signal, _ = round5.random_from_pool(data, close_context.pool, signal_rows)
            control_rows = control_signal - 1
            approach = np.full(len(entries), 3, dtype=np.int64)
            strategy = round3.evaluate(data, close_view, rules_b, entries, approach, runner=run_exits_r6)
            control = round3.evaluate(data, close_view, rules_b, control_rows, np.full(len(control_rows), 3, dtype=np.int64), runner=run_exits_r6)
            apply_overnight_keep(data, strategy, 1)
            apply_overnight_keep(data, control, 1)

            for result in (strategy, control):
                result["index_id"] = market_of_stock[result["stock"]]
                result["entry_close"] = np.ones(len(result["entries"]), dtype=bool)

            label_row = {"run": run, "board": board, "pool": pool_name}

            if board == "U2":
                rows_cost, per_trade = cost_scenarios(data, strategy, 1, dict(label_row, set="신호"))
                cost_rows.extend(rows_cost)
                control_cost, _ = cost_scenarios(data, control, 1, dict(label_row, set="무작위"))
                cost_rows.extend(control_cost)
                keep = strategy["keep"][:, 1] & (strategy["reason"][:, 1] == REASON_NEXT_OPEN)
                per_trade.insert(0, "year", (panel.dates[strategy["entries"][keep]] // 10000).astype(np.int32))
                per_trade.insert(0, "run", run)
                cost_frames.append(per_trade)

            for valuation, multiplier in round3.VALUATIONS:
                if board == "U0" and valuation != "종가":
                    continue

                table, year_table, bars, equity, daily = round3.board_tables(data, rules_b, strategy, control, multiplier, index_open, index_close,
                                                                         groups, signs, first_day, last_day, years, kospi_curve)
                table = round4.renamed(round4.with_kosdaq_hold(table, kosdaq_annual, kosdaq_mdd)).assign(valuation=valuation, **label_row)
                b_tables.append(table)

                if board == "U2":
                    b_series[(valuation, run)] = series_of(daily, [1])

                    if valuation == "종가":
                        b_years.append(round4.renamed(year_table[year_table["cell"] == 1]).assign(**label_row))
                        b_equity[run] = equity[1]
                        annual, t_min, count = versus_round4(daily["portfolio"][[1]], round4_wide[[55]], groups, signs)
                        b_records.append({"run": run, "versus_live_round4_annual_pct": float(annual[0]),
                                          "versus_live_round4_t_min": float(t_min[0]), "versus_live_round4_days": int(count[0])})
                        shuffle_inputs[run] = (strategy, round3.valued(strategy, 1.0))

            print(f"B {run}: 신호 {len(entries)} · 남은 매매 {int(strategy['keep'][:, 1].sum())} ({time.time() - started:.0f}초)", flush=True)

            if board != "U2":
                del strategy
            del control
            gc.collect()

    # ── 가족 기준선 ──
    family = {}

    for valuation, (index_list, random_list) in family_series.items():
        family[valuation] = {}

        for name, values_list in (("index", index_list), ("random", random_list)):
            values = np.vstack(values_list).astype(np.float64)
            _, _, bars = round3.family_t(values, month_of_day, year_of_day, month_count, year_count, signs_month, signs_year,
                                     np.ones(len(values), dtype=bool))
            family[valuation][name] = bars["main_bar"]
            family[valuation][f"{name}_bars"] = bars
            family[valuation]["cells"] = int(len(values))

    family_b = {}

    for valuation in [valuation_key for valuation_key, _ in round3.VALUATIONS]:
        keys = [key for key in b_series if key[0] == valuation]
        family_b[valuation] = {}

        for slot, name in ((0, "index"), (1, "random")):
            values = np.vstack([b_series[key][slot] for key in keys])
            _, _, bars = round3.family_t(values, month_of_day, year_of_day, month_count, year_count, signs_month, signs_year,
                                     np.ones(len(values), dtype=bool))
            family_b[valuation][name] = bars["main_bar"]

    # ── A 판정 ──
    table_a = pd.concat(a_tables, ignore_index=True)
    table_a["family_bar_index"] = table_a["valuation"].map(lambda v: family[v]["index"])
    table_a["family_bar_random"] = table_a["valuation"].map(lambda v: family[v]["random"])
    table_a["line_index"] = np.maximum(table_a["family_bar_index"], PASS_FLOOR)
    table_a["line_random"] = np.maximum(table_a["family_bar_random"], PASS_FLOOR)
    table_a["c1_index"] = table_a["index_t_min"] >= table_a["line_index"]
    table_a["c2_random"] = table_a["random_t_min"] >= table_a["line_random"]
    sign = table_a.set_index(["timing", "valuation", "variant", "cell"])["index_annual_pct"].apply(np.sign)
    table_a["c3_h_same_sign"] = [sign.get((row.timing, row.valuation, C5_VARIANTS[1] if row.variant == C5_VARIANTS[0] else C5_VARIANTS[0],
                                     row.cell)) == np.sign(row.index_annual_pct) for row in table_a.itertuples()]
    table_a["c3_neighbours"] = table_a["neighbours_same_sign"] & table_a["c3_h_same_sign"]
    table_a["c4_years"] = table_a["share_positive_years"] >= exit_test.JUDGE_POSITIVE_YEARS
    table_a["slot_judgeable"] = table_a["slot_trades"] >= round3.MIN_SLOT_TRADES
    table_a["c5_slot"] = table_a["slot_judgeable"] & (table_a["slot_annual_pct"] > table_a["kospi_hold_annual_pct"]) & (table_a["slot_annual_pct"] > table_a["kosdaq_hold_annual_pct"])
    table_a["pass_1_5"] = table_a["c1_index"] & table_a["c2_random"] & table_a["c3_neighbours"] & table_a["c4_years"] & table_a["c5_slot"]
    main = table_a[table_a["valuation"] == "종가"].copy()
    key = ["run", "cell"]

    for short, valuation in (("m10", "종가−10%"), ("m20", "종가−20%")):
        other = table_a[table_a["valuation"] == valuation].set_index(key)
        main = main.join(other[["index_t_min", "pass_1_5"]].rename(columns=lambda c: f"{c}_{short}"), on=key)

    main["c6_all_valuations"] = main["pass_1_5"] & main["pass_1_5_m10"] & main["pass_1_5_m20"]
    u0 = pd.concat(u0_tables, ignore_index=True)
    u0 = u0[u0["run"].str.startswith("U0|")].set_index(["variant", "timing", "cell"])["index_annual_pct"]
    main["u0_index_annual_pct"] = [u0.get((row.variant, row.timing, row.cell), np.nan) for row in main.itertuples()]
    main["c7_u0_same_sign"] = np.sign(main["u0_index_annual_pct"]) == np.sign(main["index_annual_pct"])
    main = main.merge(pd.concat(a_round4, ignore_index=True), on=["run", "cell"], how="left")
    main["c8_versus_round4"] = main["versus_round4_annual_pct"] > 0
    main["passes"] = main["c6_all_valuations"] & main["c7_u0_same_sign"] & main["c8_versus_round4"]
    main["status"] = np.where(~main["slot_judgeable"], "판정 불가", np.where(main["passes"], "통과", "불통과"))

    # ── B 판정 ──
    table_b = pd.concat(b_tables, ignore_index=True)
    table_b = table_b[table_b["cell"] == 1].copy()
    bu2 = table_b[table_b["board"] == "U2"].copy()
    bu2["family_bar_index"] = bu2["valuation"].map(lambda v: family_b[v]["index"])
    bu2["family_bar_random"] = bu2["valuation"].map(lambda v: family_b[v]["random"])
    bu2["line_index"] = np.maximum(bu2["family_bar_index"], PASS_FLOOR)
    bu2["line_random"] = np.maximum(bu2["family_bar_random"], PASS_FLOOR)
    bu2["c1_index"] = bu2["index_t_min"] >= bu2["line_index"]
    bu2["c2_random"] = bu2["random_t_min"] >= bu2["line_random"]
    b_neighbours = {"U2 전체": ["C5 90%"], "C5 90%": ["C5 95%"], "C5 95%": ["C5 90%"]}
    bsign = bu2.set_index(["valuation", "pool"])["index_annual_pct"].apply(np.sign)
    bu2["c3_neighbours"] = [all(bsign.get((row.valuation, other)) == np.sign(row.index_annual_pct) for other in b_neighbours[row.pool])
                            for row in bu2.itertuples()]
    bu2["c4_years"] = bu2["share_positive_years"] >= exit_test.JUDGE_POSITIVE_YEARS
    bu2["slot_judgeable"] = bu2["slot_trades"] >= round3.MIN_SLOT_TRADES
    bu2["c5_slot"] = bu2["slot_judgeable"] & (bu2["slot_annual_pct"] > bu2["kospi_hold_annual_pct"]) & (
        bu2["slot_annual_pct"] > bu2["kosdaq_hold_annual_pct"])
    bu2["pass_1_5"] = bu2["c1_index"] & bu2["c2_random"] & bu2["c3_neighbours"] & bu2["c4_years"] & bu2["c5_slot"]
    bmain = bu2[bu2["valuation"] == "종가"].copy().set_index("pool")

    for short, valuation in (("m10", "종가−10%"), ("m20", "종가−20%")):
        other = bu2[bu2["valuation"] == valuation].set_index("pool")
        bmain[f"pass_1_5_{short}"] = other["pass_1_5"]
        bmain[f"index_t_min_{short}"] = other["index_t_min"]

    bmain["c6_all_valuations"] = bmain["pass_1_5"] & bmain["pass_1_5_m10"] & bmain["pass_1_5_m20"]
    bu0 = table_b[table_b["board"] == "U0"].copy()
    bu0["pool"] = bu0["pool"].replace({"U0 전체": "U2 전체"})
    bu0 = bu0.set_index("pool")["index_annual_pct"]
    bmain["u0_index_annual_pct"] = [bu0.get(pool, np.nan) for pool in bmain.index]
    bmain["c7_u0_same_sign"] = np.sign(bmain["u0_index_annual_pct"]) == np.sign(bmain["index_annual_pct"])
    bmain = bmain.reset_index().merge(pd.DataFrame(b_records), on="run", how="left")
    bmain["c8_versus_round4"] = bmain["versus_live_round4_annual_pct"] > 0
    bmain["passes"] = bmain["c6_all_valuations"] & bmain["c7_u0_same_sign"] & bmain["c8_versus_round4"]
    bmain["status"] = np.where(~bmain["slot_judgeable"], "판정 불가", np.where(bmain["passes"], "통과", "불통과"))

    # ── 통과 칸 추가 산출 ──
    shuffles = []

    for row in main[main["status"] == "통과"].itertuples():
        result, net = shuffle_inputs[row.run]
        values = shuffled_slots(data, result, net, int(row.cell), first_day, last_day, years)
        shuffles.append({"part": "A", "run": row.run, "cell": int(row.cell), "slot_annual_pct": row.slot_annual_pct,
                         **{f"shuffle_{shuffle_index + 1}": value for shuffle_index, value in enumerate(values)}})

    for row in bmain[bmain["status"] == "통과"].itertuples():
        result, net = shuffle_inputs[row.run]
        values = shuffled_slots(data, result, net, 1, first_day, last_day, years)
        shuffles.append({"part": "B", "run": row.run, "cell": 1, "slot_annual_pct": row.slot_annual_pct,
                         **{f"shuffle_{shuffle_index + 1}": value for shuffle_index, value in enumerate(values)}})

    # ── 저장 ──
    overlap, outcome, a2_info = a2
    common.write_csv(OUTPUT / "a_judge_c5_252.csv", main)
    common.write_csv(OUTPUT / "a_judge_all_valuations.csv", table_a)
    common.write_csv(OUTPUT / "a_year_table_c5.csv", pd.concat(a_years, ignore_index=True))
    common.write_csv(OUTPUT / "a2_overlap_1520.csv", overlap)
    common.write_csv(OUTPUT / "a2_outcome_1520.csv", outcome)
    common.write_csv(OUTPUT / "b_judge.csv", bmain)
    common.write_csv(OUTPUT / "b_table_all.csv", table_b)
    common.write_csv(OUTPUT / "b_breakeven.csv", pd.DataFrame(cost_rows))
    common.write_csv(OUTPUT / "b_year_table.csv", pd.concat(b_years, ignore_index=True))
    per_trade = pd.concat(cost_frames, ignore_index=True)
    by_year = per_trade.groupby(["run", "year"]).agg(trades=("gross_pct", "size"), overnight_gross_mean_pct=("gross_pct", "mean"),
                                                     net_s0_mean_pct=("S0 현행", "mean"), net_s1_mean_pct=("S1 호가 0틱", "mean"),
                                                     net_s4_mean_pct=("S4 S3+매도세 0", "mean")).reset_index()
    common.write_csv(OUTPUT / "b_breakeven_by_year.csv", by_year)
    per_trade.to_parquet(OUTPUT / "b_trades_costs.parquet", index=False)

    if shuffles:
        common.write_csv(OUTPUT / "pass_shuffles.csv", pd.DataFrame(shuffles))

    curves = pd.DataFrame({f"B|{run}": values for run, values in b_equity.items()})
    curves.insert(0, "date", panel.market_dates[days])
    curves["kospi_hold"] = kospi_curve
    curves["kosdaq_hold"] = kosdaq_curve
    curves.to_parquet(OUTPUT / "slot_equity_b.parquet", index=False)
    command = "py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round6.py"
    common.write_text(OUTPUT / "run_command.txt", command + "\n")
    hashed = {name: exit_test.file_hash(OUTPUT / name) for name in ("a_judge_c5_252.csv", "a_judge_all_valuations.csv", "a2_overlap_1520.csv",
                                                             "a2_outcome_1520.csv", "b_judge.csv", "b_breakeven.csv",
                                                             "b_breakeven_by_year.csv")}
    metrics = {"reproduction": panel.reproduction(), "command": command, "seconds": round(time.time() - started, 1), "years": years,
               "pass_floor": PASS_FLOOR, "family_a": {valuation_name: {key: entry for key, entry in value.items() if not key.endswith("_bars")} for valuation_name, value in family.items()},
               "family_b": family_b, "replication_vs_round5": replication, "a2": a2_info, "flips": round3_test.FLIPS, "seed": common.SEED,
               "random_seed": round3.RANDOM_SEED, "status_a": main["status"].value_counts().to_dict(),
               "status_b": bmain["status"].value_counts().to_dict(), "output_sha256_16": hashed}
    common.write_json(OUTPUT / "metrics.json", metrics)
    pd.set_option("display.width", 250)
    pd.set_option("display.max_columns", 30)
    print({valuation_name: {key: round(value[key], 3) for key in ("index", "random")} | {"cells": value["cells"]} for valuation_name, value in family.items()}, flush=True)
    print(family_b, flush=True)
    print(main["status"].value_counts().to_dict(), "A 지수 t 최댓값", round(float(main["index_t_min"].max()), 3), flush=True)
    print(bmain[["pool", "trades", "net_mean", "index_annual_pct", "index_t_min", "line_index", "random_t_min", "share_positive_years",
                 "slot_trades", "slot_annual_pct", "versus_live_round4_annual_pct", "status"]].to_string(index=False), flush=True)
    print(pd.DataFrame(cost_rows)[["run", "set", "scenario", "trades", "overnight_gross_mean_pct", "cost_mean_pct", "net_mean_pct",
                                   "possible"]].to_string(index=False), flush=True)
    print(overlap.to_string(index=False), flush=True)
    print(outcome.to_string(index=False), flush=True)
    print(f"끝 {time.time() - started:.0f}초", flush=True)


if __name__ == "__main__":
    main()
