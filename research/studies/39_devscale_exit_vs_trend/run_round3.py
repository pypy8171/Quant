"""스터디 39 round3 — 2회차 편향 감사(ⓐ–ⓔ)를 반영해 다시 판정한다.

고친 것:
  ⓓ 진입 당일: 다가온 쪽 극값(위에서 내려와 샀으면 고가, 아래에서 올라와 샀으면 저가)은 종가로 본다. 진입 당일 시가 매도 없음.
  ⓒ 칸마다 자기 청산으로 같은 종목 재진입을 막는다(run_exit_test.own_dedupe).
  ⓐ 판정 값 = 날짜별 포트폴리오 수익 − 같은 가중 코스피 수익(베타 조정 판 같이). t = min(달 묶음 t, 해 묶음 t).
  ⓑ 같은 청산을 붙인 무작위 진입(같은 날·같은 거래대금·가격 조건) 대비 초과를 요구한다.
  ⓔ 슬롯 판은 자본 200만·현금 한도 안에서만 산다. 코스피 보유와 비교하고 슬롯 매매 200 건 미만은 판정 불가.
  미청산 평가 민감도(종가·−10%·−20%).
사전 등록: results/round3/choices_log.csv(실행 전 26 행).
실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round3.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))

import run_round2 as round2     # noqa: E402  스터디 38 경로·numba 설정을 먼저 넣는다
import common     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402

import round3_test     # noqa: E402
import round11_test     # noqa: E402

exit_test = round2.exit_test
OUTPUT = STUDY / "results" / "round3"
CAPITAL = 2_000_000.0
SLOTS = 4
MIN_ORDER = 100_000.0
MIN_SLOT_TRADES = 200
VALUATIONS = (("종가", 1.0), ("종가−10%", 0.9), ("종가−20%", 0.8))
RANDOM_SEED = common.SEED + 2
CHUNK = 150_000
YEAR_DAYS = 250.0
PULLBACK, ENTRY_UPPER, HOLD_WIDEN = exit_test.PULLBACK, exit_test.ENTRY_UPPER, exit_test.HOLD_WIDEN
BOARDS = (("U2", ("previous_turnover_ge_10억", "order_le_0.5pct_of_20d")), ("U0", ("order_le_0.5pct_of_20d",)))


# ── 청산 커널(진입 당일 처리만 run_exit_test.exit_rule 과 다르다) ─────────────

@njit(cache=True)
def exit_rule_fixed(kind, tp, sl, parameter, entry, end, approach, fill, open_price, high, low, close, before19, aligned_close,
                    aligned_previous, folded_open, sma20, states, position, listed, delist_multiplier):
    """approach: 0 시가가 구간 안, 1 아래에서 올라와 삼(그날 저가는 진입 전), 2 위에서 내려와 삼(그날 고가는 진입 전)."""
    tp_level = fill * (1.0 + tp / 100.0) if tp == tp else np.inf
    sl_level = fill * (1.0 - sl / 100.0) if sl == sl else -np.inf
    lower_band = (1.0 - PULLBACK - HOLD_WIDEN)
    upper_band = (1.0 + ENTRY_UPPER + HOLD_WIDEN)
    run = 0

    for row in range(entry, end):
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
            else:
                low_after = close[row] if approach == 1 else low[row]
                high_after = close[row] if approach == 2 else high[row]

                if low_after <= lower:
                    return row, min(lower, fill), lower_reason     # 산 값이 이미 존 밖이면 산 값에 판다(무작위 대조군)

                if high_after >= upper:
                    return row, max(upper, fill), upper_reason

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
def run_exits_fixed(entries, approach, fills, ends, listed, delist_multiplier, kinds, tps, sls, parameters, open_price, high, low, close,
                    before19, aligned_close, aligned_previous, folded_open, sma20, states, position):
    count = entries.shape[0]
    rules = kinds.shape[0]
    exit_row = np.empty((count, rules), dtype=np.int64)
    exit_price = np.empty((count, rules))
    reason = np.empty((count, rules), dtype=np.int8)

    for index in prange(count):
        for rule in range(rules):
            row, price, code = exit_rule_fixed(kinds[rule], tps[rule], sls[rule], parameters[rule], entries[index], ends[index],
                                               approach[index], fills[index], open_price, high, low, close, before19, aligned_close,
                                               aligned_previous, folded_open, sma20, states, position, listed[index],
                                               delist_multiplier[index])
            exit_row[index, rule] = row
            exit_price[index, rule] = price
            reason[index, rule] = code

    return exit_row, exit_price, reason


class OpenEntry:
    """무작위 대조군: 시가 + 1호가에 산다. 보유 판정 배열은 엔진 정의 그대로."""

    def __init__(self, data: exit_test.Data, engine: round2.Engine):
        self.entry_mid = data.open
        self.entry_raw = data.raw_open
        self.entry_tick = data.entry_tick
        self.aligned_close = engine.aligned_close
        self.aligned_previous = engine.aligned_previous
        self.folded_open_aligned = engine.folded_open_aligned


def evaluate(data: exit_test.Data, view, rules: pd.DataFrame, entries: np.ndarray, approach: np.ndarray, runner=None) -> dict:
    """r2.evaluate 와 같은 비용 식. 진입 당일 처리만 고친 커널. 메모리 때문에 CHUNK 건씩 끊어 돈다."""
    panel = data.panel
    dates = panel.dates
    count, cells = len(entries), len(rules)
    exit_row = np.empty((count, cells), dtype=np.int64)
    reason = np.empty((count, cells), dtype=np.int8)
    net = np.empty((count, cells))
    entry_mid = view.entry_mid[entries]
    entry_raw = view.entry_raw[entries]
    entry_tick = view.entry_tick[entries]
    fills = entry_mid * (entry_raw + entry_tick) / entry_raw
    buy_ratio = exit_test.AMOUNT / data.previous_20d[entries]
    buy_cost = (entry_raw / (entry_raw + entry_tick) / (1.0 + round11_test.COMMISSION + round11_test.FLAT_IMPACT)
                / (1.0 + round11_test.LINEAR * np.nan_to_num(buy_ratio)))
    kinds, tps = rules["kind"].to_numpy(np.int64), rules["tp"].to_numpy(np.float64)
    sls, parameters = rules["sl"].to_numpy(np.float64), rules["param"].to_numpy(np.int64)
    position = panel.market_position.astype(np.int64)

    for begin in range(0, count, CHUNK):
        part = slice(begin, min(begin + CHUNK, count))
        rows = entries[part]
        rows_exit, price, code = (runner or run_exits_fixed)(
            rows, approach[part], fills[part], data.stock_end[rows], data.listed_row[rows], data.delist_multiplier_row[rows], kinds, tps,
            sls, parameters, data.open, data.high, data.low, data.close, data.before[20], view.aligned_close, view.aligned_previous,
            view.folded_open_aligned, data.sma[20], data.states, position)
        gross = price / entry_mid[part, None]
        sell_raw = price * data.unadjust_clean[rows_exit]
        sell_dates = dates[rows_exit]
        sell_tick = exit_test.ticks(sell_raw.ravel(), sell_dates.ravel()).reshape(sell_raw.shape)
        sell_tax = round11_test.sell_tax(sell_dates.ravel()).reshape(sell_raw.shape)
        sell_base = data.previous_20d[rows_exit]
        sell_ratio = np.where(np.isfinite(sell_base) & (np.nan_to_num(sell_base) > 0), exit_test.AMOUNT / np.where(sell_base > 0, sell_base, 1.0),
                              np.nan_to_num(buy_ratio[part])[:, None])
        sell_cost = (np.maximum(sell_raw - sell_tick, 0.0) / np.where(sell_raw > 0, sell_raw, 1.0)
                     * (1.0 - round11_test.COMMISSION - sell_tax - round11_test.FLAT_IMPACT)
                     * np.maximum(0.0, 1.0 - round11_test.LINEAR * sell_ratio))
        exit_row[part] = rows_exit
        reason[part] = code
        net[part] = (gross * buy_cost[part, None] * sell_cost - 1.0) * 100.0

    keep = exit_test.own_dedupe(entries, panel.stock_of_row.astype(np.int64), exit_row, reason, data.stock_end[entries])
    day = panel.market_position[entries].astype(np.int64)
    return {"entries": entries, "approach": approach, "entry_mid": entry_mid, "fill": fills, "buy_cost": buy_cost, "exit_row": exit_row,
            "reason": reason, "net": net, "keep": keep, "day": day, "stock": panel.stock_of_row[entries].astype(np.int64),
            "year": panel.years[entries]}


def valued(result: dict, multiplier: float) -> np.ndarray:
    """미청산(사유 8)을 2026-10-08 종가 × multiplier 로 평가한 순수익."""
    if multiplier == 1.0:
        return result["net"]

    return np.where(result["reason"] == 8, ((1.0 + result["net"] / 100.0) * multiplier - 1.0) * 100.0, result["net"])


# ── 무작위 진입 ─────────────────────────────────────────────────────────────

def random_entries(data: exit_test.Data, flags: dict, extra: tuple, candidates: np.ndarray) -> np.ndarray:
    """신호 하나마다 같은 시장일의 같은 조건 행 하나(복원 추출). 행 오름차순."""
    panel = data.panel
    pool = flags["signal_period"] & flags["kospi_gap_gt_-1"] & np.isfinite(data.open)

    with np.errstate(invalid="ignore"):
        pool &= (data.raw_open >= exit_test.MIN_PRICE) & (data.raw_open <= exit_test.MAX_PRICE)

    for name in extra:
        pool &= flags[name]

    rows = np.flatnonzero(pool)
    pool_day = panel.market_position[rows].astype(np.int64)
    order = np.argsort(pool_day, kind="stable")
    rows, pool_day = rows[order], pool_day[order]
    candidate_positions = panel.market_position[candidates].astype(np.int64)
    start = np.searchsorted(pool_day, candidate_positions, side="left")
    size = np.searchsorted(pool_day, candidate_positions, side="right") - start
    generator = np.random.default_rng(RANDOM_SEED)
    draw = generator.random(len(candidates))
    has = size > 0
    picked = rows[start[has] + np.minimum((draw[has] * size[has]).astype(np.int64), size[has] - 1)]
    return np.sort(picked).astype(np.int64), int((~has).sum())


# ── 날짜별 포트폴리오 ───────────────────────────────────────────────────────

@njit(parallel=True, cache=True)
def daily_book(keep, entries, index_id, exit_row, net, entry_mid, buy_cost, close, position, index_open, index_close, market_count):
    """칸 × 시장일 (평가 손익 합, 같은 가중 코스피 손익 합, 전날 평가액 합). 진입 때 1 씩 넣고 들고 간다."""
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
                    index_return = index_close[index_id[index], day] / index_open[index_id[index], day] - 1.0
                else:
                    index_return = index_close[index_id[index], day] / index_close[index_id[index], position[row - 1]] - 1.0

                pnl[cell, day] += value - previous
                bench[cell, day] += previous * index_return
                base[cell, day] += previous
                previous = value

    return pnl, bench, base


def book_returns(data: exit_test.Data, result: dict, net: np.ndarray, kospi_open: np.ndarray, kospi_close: np.ndarray):
    panel = data.panel
    index_id = result.get("index_id", np.zeros(len(result["entries"]), dtype=np.int64))     # 비교 지수 행(round3 은 코스피 하나)
    index_open, index_close = np.atleast_2d(kospi_open), np.atleast_2d(kospi_close)
    pnl, bench, base = daily_book(result["keep"], result["entries"], index_id, result["exit_row"], net, result["entry_mid"],
                                  result["buy_cost"], data.close, panel.market_position.astype(np.int64), index_open, index_close,
                                  panel.market_count)
    held = base > 0

    with np.errstate(invalid="ignore", divide="ignore"):
        portfolio = np.where(held, pnl / base * 100.0, np.nan)
        index = np.where(held, bench / base * 100.0, np.nan)

    return portfolio, index


def cluster_sums(values: np.ndarray, groups: np.ndarray, size: int) -> tuple[np.ndarray, np.ndarray]:
    cells = values.shape[0]
    sums = np.zeros((cells, size))
    counts = np.zeros((cells, size))

    for cell in range(cells):
        valid = np.isfinite(values[cell])
        np.add.at(sums[cell], groups[valid], values[cell, valid])
        np.add.at(counts[cell], groups[valid], 1.0)

    return sums, counts


def family_t(values: np.ndarray, month_of_day: np.ndarray, year_of_day: np.ndarray, month_count: int, year_count: int,
             signs_month: np.ndarray, signs_year: np.ndarray, judged: np.ndarray) -> tuple[np.ndarray, np.ndarray, dict]:
    """값(칸 × 시장일)의 달 t·해 t 와, judged 칸으로 낸 뒤집기 기준선."""
    month_sums, month_counts = cluster_sums(values, month_of_day, month_count)
    year_sums, year_counts = cluster_sums(values, year_of_day, year_count)
    t_month, flip_month = round2.flipped_t(month_sums, month_counts, signs_month)
    t_year, flip_year = round2.flipped_t(year_sums, year_counts, signs_year)
    bars = {"month_up": round2.quantile_of_max(flip_month[judged]), "month_down": round2.quantile_of_max(-flip_month[judged]),
            "year_up": round2.quantile_of_max(flip_year[judged]), "year_down": round2.quantile_of_max(-flip_year[judged])}
    bars["main_bar"] = max(bars.values())
    return t_month, t_year, bars


# ── 슬롯 ────────────────────────────────────────────────────────────────────

@njit(parallel=True, cache=True)
def slot_cash(order, day, exit_day, stock, stock_count, net, slots, capital, amount, min_order):
    """칸마다 시간순으로 넣는다. 주문 = min(amount, 현금), 현금 < min_order 면 건너뜀. 매도 대금은 매도 다음 시장일부터."""
    count, cells = exit_day.shape
    notional = np.zeros((count, cells))

    for cell in prange(cells):
        held_until = np.full(stock_count, -1, dtype=np.int64)
        slot_end = np.full(slots, -1, dtype=np.int64)
        slot_back = np.zeros(slots)
        busy = np.zeros(slots, dtype=np.bool_)
        cash = capital

        for step in range(count):
            index = order[step]
            today = day[index]

            for slot in range(slots):
                if busy[slot] and slot_end[slot] < today:
                    cash += slot_back[slot]
                    busy[slot] = False

            if held_until[stock[index]] >= today:
                continue

            free = -1

            for slot in range(slots):
                if not busy[slot]:
                    free = slot
                    break

            if free < 0:
                continue

            size = min(amount, cash)

            if size < min_order:
                continue

            cash -= size
            busy[free] = True
            slot_end[free] = exit_day[index, cell]
            slot_back[free] = size * (1.0 + net[index, cell] / 100.0)
            held_until[stock[index]] = exit_day[index, cell]
            notional[index, cell] = size

    return notional


@njit(parallel=True, cache=True)
def slot_value(notional, entries, exit_row, net, entry_mid, buy_cost, close, position, market_count):
    """칸 × 시장일 평가 손익 변화량(종가 평가, 매도일엔 순수익)."""
    count, cells = notional.shape
    delta = np.zeros((cells, market_count))

    for cell in prange(cells):
        for index in range(count):
            amount = notional[index, cell]

            if amount <= 0.0:
                continue

            previous = 0.0
            last = exit_row[index, cell]

            for row in range(entries[index], last + 1):
                if row == last:
                    value = amount * net[index, cell] / 100.0
                else:
                    value = amount * (close[row] / entry_mid[index] * buy_cost[index] - 1.0)

                delta[cell, position[row]] += value - previous
                previous = value

    return delta


def curve_statistics(curve: np.ndarray, years: float) -> tuple[float, float]:
    peak = np.maximum.accumulate(curve)
    return float(((curve[-1] / curve[0]) ** (1.0 / years) - 1.0) * 100.0), float(((curve - peak) / peak).min() * 100.0)


def slot_book(data: exit_test.Data, result: dict, net: np.ndarray, first_day: int, last_day: int):
    panel = data.panel
    exit_day = panel.market_position[result["exit_row"]].astype(np.int64)
    previous_turnover = np.nan_to_num(data.previous_day_turnover[result["entries"]], nan=-1.0)
    order = np.lexsort((result["stock"], -previous_turnover, result["day"])).astype(np.int64)
    notional = slot_cash(order, result["day"], exit_day, result["stock"], panel.stock_count, net, SLOTS, CAPITAL, exit_test.AMOUNT, MIN_ORDER)
    delta = slot_value(notional, result["entries"], result["exit_row"], net, result["entry_mid"], result["buy_cost"], data.close,
                       panel.market_position.astype(np.int64), panel.market_count)
    equity = CAPITAL + np.cumsum(delta[:, first_day:last_day + 1], axis=1)
    return notional, equity


# ── 표 ──────────────────────────────────────────────────────────────────────

def masked(values: np.ndarray, valid: np.ndarray) -> np.ndarray:
    return np.where(valid, values, np.nan)


def board_tables(data, rules, strategy, control, valuation_net, kospi_open, kospi_close, groups, signs, first_day, last_day, years,
                 kospi_curve):
    """한 판·한 평가의 칸 표·해 표·슬롯 곡선·날짜별 값."""
    panel = data.panel
    month_of_day, year_of_day, month_count, year_count = groups
    signs_month, signs_year = signs
    live_cell = int(rules.loc[rules["rule"] == exit_test.LIVE_RULE, "cell"].iloc[0])
    multiplier = valuation_net
    net_s, net_c = valued(strategy, multiplier), valued(control, multiplier)
    ret_s, index_s = book_returns(data, strategy, net_s, kospi_open, kospi_close)
    ret_c, index_c = book_returns(data, control, net_c, kospi_open, kospi_close)
    window = np.zeros(panel.market_count, dtype=bool)
    window[first_day:last_day + 1] = True
    ret_s[:, ~window], index_s[:, ~window], ret_c[:, ~window], index_c[:, ~window] = np.nan, np.nan, np.nan, np.nan
    excess = ret_s - index_s
    versus_random = ret_s - ret_c
    versus_live = ret_s - ret_s[[live_cell]]
    betas = []

    for cell in range(len(rules)):
        valid = np.isfinite(ret_s[cell]) & np.isfinite(index_s[cell])
        index_values, strategy_values = index_s[cell, valid], ret_s[cell, valid]
        betas.append(float(np.cov(index_values, strategy_values)[0, 1] / np.var(index_values, ddof=1)) if valid.sum() > 2 else np.nan)

    betas = np.array(betas)
    beta_excess = ret_s - betas[:, None] * index_s
    every = np.ones(len(rules), dtype=bool)
    not_live = np.arange(len(rules)) != live_cell
    t_values, bars = {}, {}

    for name, values, judged in (("kospi", excess, every), ("beta", beta_excess, every), ("random", versus_random, every),
                                 ("live", versus_live, not_live), ("control_kospi", ret_c - index_c, every)):
        t_month, t_year, bar = family_t(values, month_of_day, year_of_day, month_count, year_count, signs_month, signs_year, judged)
        t_values[name] = (t_month, t_year)
        bars[name] = bar

    notional_s, equity_s = slot_book(data, strategy, net_s, first_day, last_day)
    notional_c, equity_c = slot_book(data, control, net_c, first_day, last_day)
    kospi_annual, kospi_mdd = curve_statistics(kospi_curve, years)
    near = exit_test.neighbours(rules)
    rows = []

    for cell in rules["cell"]:
        keep_s, keep_c = strategy["keep"][:, cell], control["keep"][:, cell]
        hold = panel.market_position[strategy["exit_row"][keep_s, cell]] - strategy["day"][keep_s]
        reason = strategy["reason"][keep_s, cell]
        slot_trades = int((notional_s[:, cell] > 0).sum())
        slot_annual, slot_mdd = curve_statistics(equity_s[cell], years)
        control_annual, control_mdd = curve_statistics(equity_c[cell], years)
        year_means = pd.Series(excess[cell]).groupby(year_of_day).mean().dropna()
        row = {"cell": cell, "rule": rules.at[cell, "rule"], "group": rules.at[cell, "group"],
               "trades": int(keep_s.sum()), "net_mean": float(net_s[keep_s, cell].mean()), "net_median": float(np.median(net_s[keep_s, cell])),
               "hold_median": float(np.median(hold)), "hold_p90": float(np.percentile(hold, 90)),
               "unclosed": int((reason == 8).sum()), "delisted": int((reason == 9).sum()),
               "control_trades": int(keep_c.sum()), "control_net_mean": float(net_c[keep_c, cell].mean()),
               "held_days": int(np.isfinite(ret_s[cell]).sum()),
               "annual_pct": float(np.nanmean(ret_s[cell]) * YEAR_DAYS), "kospi_same_weight_annual_pct": float(np.nanmean(index_s[cell]) * YEAR_DAYS),
               "control_annual_pct": float(np.nanmean(ret_c[cell]) * YEAR_DAYS),
               "beta": float(betas[cell])}

        for name, values in (("kospi", excess), ("beta", beta_excess), ("random", versus_random), ("live", versus_live),
                             ("control_kospi", ret_c - index_c)):
            t_month, t_year = t_values[name]
            row[f"{name}_annual_pct"] = float(np.nanmean(values[cell]) * YEAR_DAYS)
            row[f"{name}_t_month"] = float(t_month[cell])
            row[f"{name}_t_year"] = float(t_year[cell])
            row[f"{name}_t_min"] = float(np.fmin(t_month[cell], t_year[cell]))

        row.update({"share_positive_years": float((year_means > 0).mean()), "years": int(len(year_means)),
                    "slot_trades": slot_trades, "slot_annual_pct": slot_annual, "slot_mdd_pct": slot_mdd,
                    "slot_end_krw": float(equity_s[cell, -1]),
                    "slot_below_min_order_date": int(panel.market_dates[first_day + np.argmax(equity_s[cell] < MIN_ORDER)])
                    if (equity_s[cell] < MIN_ORDER).any() else 0, "kospi_hold_annual_pct": kospi_annual, "kospi_hold_mdd_pct": kospi_mdd,
                    "control_slot_trades": int((notional_c[:, cell] > 0).sum()), "control_slot_annual_pct": control_annual,
                    "control_slot_mdd_pct": control_mdd})
        rows.append(row)

    table = pd.DataFrame(rows).set_index("cell")
    table["neighbours"] = ["·".join(str(other) for other in near[cell]) for cell in table.index]
    table["neighbours_same_sign"] = [bool(near[cell]) and all(
        np.sign(table.at[other, "kospi_annual_pct"]) == np.sign(table.at[cell, "kospi_annual_pct"])
        and np.sign(table.at[other, "random_annual_pct"]) == np.sign(table.at[cell, "random_annual_pct"]) for other in near[cell])
        for cell in table.index]
    table["bar_kospi"] = bars["kospi"]["main_bar"]
    table["bar_random"] = bars["random"]["main_bar"]
    table["bar_live"] = bars["live"]["main_bar"]
    table["over_kospi_bar"] = table["kospi_t_min"] >= table["bar_kospi"]
    table["over_random_bar"] = table["random_t_min"] >= table["bar_random"]
    table["better_than_a"] = table["live_t_min"] >= table["bar_live"]
    table["worse_than_a"] = np.fmax(table["live_t_month"], table["live_t_year"]) <= -table["bar_live"]
    table["slot_judgeable"] = table["slot_trades"] >= MIN_SLOT_TRADES
    table["passes"] = (table["over_kospi_bar"] & table["over_random_bar"] & table["neighbours_same_sign"]
                       & (table["share_positive_years"] >= exit_test.JUDGE_POSITIVE_YEARS) & table["slot_judgeable"]
                       & (table["slot_annual_pct"] > table["kospi_hold_annual_pct"]))
    table["status"] = np.where(~table["slot_judgeable"], "판정 불가", np.where(table["passes"], "통과", "불통과"))
    year_rows = []

    for cell in rules["cell"]:
        for year in np.unique(year_of_day[first_day:last_day + 1]):
            part = year_of_day == year
            year_rows.append({"cell": cell, "rule": rules.at[cell, "rule"], "year": int(year + panel.first_year),
                              "held_days": int(np.isfinite(ret_s[cell, part]).sum()),
                              "kospi_excess_annual_pct": float(np.nanmean(excess[cell, part]) * YEAR_DAYS) if np.isfinite(excess[cell, part]).any() else np.nan,
                              "random_excess_annual_pct": float(np.nanmean(versus_random[cell, part]) * YEAR_DAYS) if np.isfinite(versus_random[cell, part]).any() else np.nan,
                              "versus_a_annual_pct": float(np.nanmean(versus_live[cell, part]) * YEAR_DAYS) if np.isfinite(versus_live[cell, part]).any() else np.nan})

    daily = {"portfolio": ret_s, "kospi": index_s, "control": ret_c}
    return table.reset_index(), pd.DataFrame(year_rows), bars, equity_s, daily


def entry_day_table(data: exit_test.Data, rules: pd.DataFrame, strategy: dict, control: dict) -> pd.DataFrame:
    """진입 당일 매도 비율을 진입 방식별로. 올라와 산 날(1)은 그날 저가가 진입 전이라 손절 판정에서 뺐다 —
    그날 저가가 손절선 아래였던 건수(놓쳤을 수 있는 손절)를 같이 센다."""
    panel = data.panel
    rows = []

    for label, result, approaches in (("신호", strategy, (0, 1, 2)), ("무작위", control, (0,))):
        for approach in approaches:
            for cell in rules["cell"]:
                keep = result["keep"][:, cell] & (result["approach"] == approach)
                hold = panel.market_position[result["exit_row"][keep, cell]] - result["day"][keep]
                reason = result["reason"][keep, cell]
                day0 = hold == 0
                row = {"set": label, "approach": approach, "cell": cell, "rule": rules.at[cell, "rule"], "trades": int(keep.sum()),
                       "day0_share": float(day0.mean()) if keep.any() else np.nan,
                       "day0_upper_share": float((day0 & np.isin(reason, [1, 5])).mean()) if keep.any() else np.nan,
                       "day0_lower_share": float((day0 & np.isin(reason, [2, 3])).mean()) if keep.any() else np.nan,
                       "net_mean": float(result["net"][keep, cell].mean()) if keep.any() else np.nan}
                stop = rules.at[cell, "sl"]

                if approach == 1 and rules.at[cell, "kind"] == exit_test.KIND_PRICE and np.isfinite(stop):
                    entries = result["entries"][keep]
                    below = data.low[entries] <= result["fill"][keep] * (1.0 - stop / 100.0)
                    row["low_below_stop_not_sold_day0"] = int((below & ~(day0 & np.isin(reason, [2, 3]))).sum())

                rows.append(row)

    return pd.DataFrame(rows)


def first_row(board: str, valuation: str, table: pd.DataFrame, live_cell: int) -> dict:
    row = table.set_index("cell").loc[live_cell]
    return {"board": board, "valuation": valuation, "rule": exit_test.LIVE_RULE, "trades": int(row["trades"]), "net_mean_pct": row["net_mean"],
            "control_net_mean_pct": row["control_net_mean"], "annual_pct": row["annual_pct"],
            "kospi_same_weight_annual_pct": row["kospi_same_weight_annual_pct"], "control_annual_pct": row["control_annual_pct"],
            "excess_vs_kospi_annual_pct": row["kospi_annual_pct"], "t_vs_kospi": row["kospi_t_min"],
            "beta": row["beta"], "beta_excess_annual_pct": row["beta_annual_pct"], "t_beta": row["beta_t_min"],
            "excess_vs_random_annual_pct": row["random_annual_pct"], "t_vs_random": row["random_t_min"],
            "control_excess_vs_kospi_annual_pct": row["control_kospi_annual_pct"], "t_control_vs_kospi": row["control_kospi_t_min"],
            "slot_trades": int(row["slot_trades"]), "slot_annual_pct": row["slot_annual_pct"], "slot_mdd_pct": row["slot_mdd_pct"],
            "control_slot_annual_pct": row["control_slot_annual_pct"], "kospi_hold_annual_pct": row["kospi_hold_annual_pct"],
            "kospi_hold_mdd_pct": row["kospi_hold_mdd_pct"]}


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    rules = exit_test.build_rules()
    data = exit_test.Data()
    panel = data.panel
    exit_test.MONTHS = panel.month_count
    engine = round2.Engine(data)
    flags = round2.flags_of(data, engine)
    live_cell = int(rules.loc[rules["rule"] == exit_test.LIVE_RULE, "cell"].iloc[0])
    kospi = pd.read_parquet(exit_test.KOSPI).set_index("date").reindex(panel.market_dates)
    kospi_close = kospi["close"].ffill().to_numpy()
    kospi_open = kospi["open"].fillna(pd.Series(kospi_close, index=kospi.index).shift(1)).to_numpy()
    kospi_missing = int(kospi["close"].isna().sum())
    first_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_FIRST))
    last_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_LAST, side="right")) - 1
    years = ((pd.Timestamp(str(exit_test.SIGNAL_LAST)) - pd.Timestamp(str(exit_test.SIGNAL_FIRST))).days) / 365.25
    kospi_curve = CAPITAL * kospi_close[first_day:last_day + 1] / kospi_open[first_day]
    market_years = (panel.market_dates // 10000).astype(np.int64)
    year_of_day = np.clip(market_years - panel.first_year, 0, None)
    month_of_day = np.clip(year_of_day * 12 + (panel.market_dates // 100) % 100 - 1, 0, None).astype(np.int64)
    month_count, year_count = int(month_of_day.max()) + 1, int(year_of_day.max()) + 1
    generator = np.random.default_rng(common.SEED)
    signs_month = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    signs_year = np.where(generator.random((round3_test.FLIPS, year_count)) < 0.5, -1.0, 1.0)
    groups = (month_of_day, year_of_day, month_count, year_count)
    print(f"준비 {time.time() - started:.0f}초 · 코스피 빠진 시장일 {kospi_missing}", flush=True)
    view = OpenEntry(data, engine)
    first_rows, all_bars, sizes, hashed = [], {}, {}, {}

    for board, extra in BOARDS:
        mask = np.ones(panel.row_count, dtype=bool)

        for name in round2.BASE_FLAGS + extra:
            mask &= flags[name]

        candidates = np.flatnonzero(mask).astype(np.int64)
        open_price = data.open[candidates]
        approach = np.where(open_price > engine.upper[candidates], 2, np.where(open_price < engine.lower[candidates], 1, 0)).astype(np.int64)
        strategy = evaluate(data, engine, rules, candidates, approach)
        control_rows, no_pool = random_entries(data, flags, extra, candidates)
        control = evaluate(data, view, rules, control_rows, np.zeros(len(control_rows), dtype=np.int64))
        sizes[board] = {"signals": int(len(candidates)), "control_rows": int(len(control_rows)), "signals_without_pool": no_pool,
                        "approach_share_inside_below_above": (np.bincount(approach, minlength=3) / len(approach)).tolist(),
                        "a_trades_after_own_block": int(strategy["keep"][:, live_cell].sum())}
        print(f"{board}: 신호 {len(candidates)} · 대조 {len(control_rows)} ({time.time() - started:.0f}초)", flush=True)
        suffix = "" if board == "U2" else "_u0"
        slot_frames = []

        for label, multiplier in VALUATIONS:
            table, year_table, bars, equity, daily = board_tables(data, rules, strategy, control, multiplier, kospi_open, kospi_close, groups,
                                                                  (signs_month, signs_year), first_day, last_day, years, kospi_curve)
            all_bars[f"{board}·{label}"] = bars
            first_rows.append(first_row(board, label, table, live_cell))
            slot_frames.append(table[["cell", "rule", "slot_trades", "slot_judgeable", "slot_annual_pct", "slot_mdd_pct", "slot_end_krw",
                                      "slot_below_min_order_date", "control_slot_trades", "control_slot_annual_pct", "control_slot_mdd_pct", "kospi_hold_annual_pct",
                                      "kospi_hold_mdd_pct"]].assign(board=board, valuation=label))

            if label == "종가":
                main_table = table
                common.write_csv(OUTPUT / f"year_table{suffix}.csv", year_table)

                if board == "U2":
                    curves = pd.DataFrame(equity.T, columns=[f"cell{cell}" for cell in rules["cell"]])
                    curves.insert(0, "date", panel.market_dates[first_day:last_day + 1])
                    curves["kospi_hold"] = kospi_curve
                    curves.to_parquet(OUTPUT / "slot_equity.parquet", index=False)
                    days = np.arange(first_day, last_day + 1)
                    long = pd.DataFrame({"date": np.tile(panel.market_dates[days], len(rules)),
                                         "cell": np.repeat(rules["cell"].to_numpy(), len(days)).astype(np.int16),
                                         "portfolio_pct": daily["portfolio"][:, days].ravel().astype(np.float32),
                                         "kospi_same_weight_pct": daily["kospi"][:, days].ravel().astype(np.float32),
                                         "control_pct": daily["control"][:, days].ravel().astype(np.float32)})
                    long.to_parquet(OUTPUT / "daily_returns.parquet", index=False)
            else:
                short = "m10" if multiplier == 0.9 else "m20"
                main_table = main_table.merge(table[["cell", "kospi_t_min", "random_t_min", "live_t_min", "slot_annual_pct", "passes", "status"]]
                                              .rename(columns=lambda name: name if name == "cell" else f"{name}_{short}"), on="cell")

        if board == "U2":
            safe = np.where(approach == 1, 0, approach).astype(np.int64)
            conservative = evaluate(data, engine, rules, candidates, safe)
            table, _, bars, _, _ = board_tables(data, rules, conservative, control, 1.0, kospi_open, kospi_close, groups,
                                                (signs_month, signs_year), first_day, last_day, years, kospi_curve)
            all_bars["U2·종가·올라와 산 날 저가 씀(보수)"] = bars
            first_rows.append(first_row("U2 올라와 산 날 저가 씀(보수)", "종가", table, live_cell))
            main_table = main_table.merge(table[["cell", "net_mean", "kospi_t_min", "random_t_min", "live_t_min", "slot_annual_pct", "status"]]
                                          .rename(columns=lambda name: name if name == "cell" else f"{name}_low_kept"), on="cell")
            common.write_csv(OUTPUT / "entry_day_table.csv", entry_day_table(data, rules, strategy, control))
            del conservative

        main_table["passes_all_valuations"] = main_table["passes"] & main_table["passes_m10"] & main_table["passes_m20"]
        common.write_csv(OUTPUT / f"cell_table{suffix}.csv", main_table)
        common.write_csv(OUTPUT / f"slot_table{suffix}.csv", pd.concat(slot_frames, ignore_index=True))
        print(main_table[["cell", "rule", "trades", "net_mean", "kospi_annual_pct", "kospi_t_min", "random_annual_pct", "random_t_min",
                          "live_annual_pct", "live_t_min", "slot_trades", "slot_annual_pct", "status", "status_m20"]].to_string(index=False), flush=True)

        if board == "U2":
            keep = strategy["keep"]
            index, cell = np.nonzero(keep)
            exit_rows = strategy["exit_row"][index, cell]
            trades = pd.DataFrame({
                "code": np.array(panel.codes)[strategy["stock"][index]], "entry_date": panel.dates[strategy["entries"][index]],
                "entry_price_raw": engine.entry_raw[strategy["entries"][index]].astype(np.float32),
                "approach": strategy["approach"][index].astype(np.int8), "cell": cell.astype(np.int16),
                "exit_date": panel.dates[exit_rows], "reason": strategy["reason"][index, cell],
                "hold_days": (panel.market_position[exit_rows] - strategy["day"][index]).astype(np.int32),
                "net_pct": strategy["net"][index, cell].astype(np.float32)})
            trades.to_parquet(OUTPUT / "trades.parquet", index=False)
            pd.DataFrame({"code": np.array(panel.codes)[panel.stock_of_row[control_rows]],
                          "date": panel.dates[control_rows]}).to_parquet(OUTPUT / "control_entries.parquet", index=False)

        del strategy, control

    first = pd.DataFrame(first_rows)
    common.write_csv(OUTPUT / "first_table_a.csv", first)
    command = "py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round3.py"
    common.write_text(OUTPUT / "run_command.txt", command + "\n")
    for name in ("first_table_a.csv", "cell_table.csv", "cell_table_u0.csv", "year_table.csv", "year_table_u0.csv", "slot_table.csv",
                 "slot_table_u0.csv"):
        hashed[name] = exit_test.file_hash(OUTPUT / name)
    metrics = {"reproduction": panel.reproduction(), "command": command, "seconds": round(time.time() - started, 1), "years": years,
               "boards": sizes, "bars": all_bars, "kospi_missing_market_days": kospi_missing, "flips": round3_test.FLIPS,
               "seed": common.SEED, "random_seed": RANDOM_SEED, "capital": CAPITAL, "slots": SLOTS, "min_order": MIN_ORDER,
               "min_slot_trades": MIN_SLOT_TRADES, "output_sha256_16": hashed}
    common.write_json(OUTPUT / "metrics.json", metrics)
    print(first.T.to_string(), flush=True)
    print({key: value["kospi"]["main_bar"] for key, value in all_bars.items()}, flush=True)
    print(f"끝 {time.time() - started:.0f}초", flush=True)


if __name__ == "__main__":
    main()
