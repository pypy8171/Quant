"""스터디 39 round2 — 엔진 정배열 정의로 진입 근사를 고치고, 판정 값을 (a) 대비 짝 차이로 바꾸고, 슬롯 4 개 자본 곡선을 낸다.

엔진 정의(코드에서 읽음, 고치지 않음):
  - 정배열 = 접은 SMA5 > 접은 SMA10 > 접은 SMA20 × (1 − tol), tol 기본 0. SMA60 없음.
    Quant/src/universe/MaAlign.cpp aligned·fold_today, Quant/src/strategy/DeviationScaleZone.cpp judge_zone
  - 진입 = 접은 정배열 AND 접은 SMA20 대비 이격 −8 – +5%. 전날 확정 정배열은 보유 유지에만.
  - 일봉은 KIS 일봉(오늘 제외)에 현재가를 접어 넣는다. 하루 필터는 09:03 뒤 첫 평가 가격과 전날 SMA20·ATR14.
근사 E: 그날 [저가·고가] 안에 위 두 조건을 함께 만족하는 가격이 있으면 진입. 그 가격 구간의 양 끝은 전날까지 종가로만 정해진다.
사전 등록: results/round2/choices_log.csv(실행 전 33 행).
실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round2.py
"""
from __future__ import annotations

import re
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))

import run_exit_test as exit_test     # noqa: E402  스터디 38 경로·numba 설정을 먼저 넣는다
import common     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402

import round3_test     # noqa: E402
import round6_test     # noqa: E402
import round11_test     # noqa: E402

OUTPUT = STUDY / "results" / "round2"
SLOTS = 4
CAPITAL = SLOTS * exit_test.AMOUNT
COVERAGE_GOAL = 0.5
REFERENCE_LINE = 3.5          # 내가 정한 참고선(판정에 안 씀)
EPSILON = 1e-9
SAME_CONFIG_FROM = 20260923   # 모의 로그에 ATR≤4.5·이격≥−1 필터가 처음 보인 날(09-21–22 는 ATR≤4.0, 그 전은 필터 없음)
PAPER_MIN_PRICE = 2000.0       # 모의 config(config_dev_paper.json) min_price 2000·max_price 0(상한 없음)


# ── 엔진 정의로 바꾼 지표 ────────────────────────────────────────────────────

class Engine:
    """엔진 정배열·존을 일봉으로 옮긴 값."""

    def __init__(self, data: exit_test.Data):
        panel = data.panel
        starts = panel.starts
        s4, s9, s19 = data.before[5], data.before[10], data.before[20]
        moving_averages = data.sma

        with np.errstate(invalid="ignore"):
            self.aligned_close = (moving_averages[5] > moving_averages[10]) & (moving_averages[10] > moving_averages[20])

        self.aligned_previous = np.r_[False, self.aligned_close[:-1]]
        self.aligned_previous[starts[:-1]] = False
        # 접은 정배열 ⇔ p > max(S9 − 2·S4, S19 − 2·S9). 존 ⇔ 0.92·S19/19.08 ≤ p ≤ 1.05·S19/18.95
        self.align_floor = np.maximum(s9 - 2.0 * s4, s19 - 2.0 * s9)
        low_ratio, high_ratio = 1.0 - exit_test.PULLBACK, 1.0 + exit_test.ENTRY_UPPER
        self.band_low = low_ratio * s19 / (20.0 - low_ratio)
        self.band_high = high_ratio * s19 / (20.0 - high_ratio)
        floor = np.where(self.align_floor > 0, self.align_floor * (1.0 + EPSILON), self.align_floor + EPSILON)
        self.lower = np.maximum(floor, self.band_low)
        self.upper = self.band_high

        with np.errstate(invalid="ignore"):
            self.interval = np.isfinite(s19) & (self.lower <= self.upper) & (self.upper > 0)
            self.touched = self.interval & (data.low <= self.upper) & (data.high >= self.lower)
            self.folded_open_aligned = data.open > self.align_floor

        inside = (data.open >= self.lower) & (data.open <= self.upper)
        self.entry_mid = np.where(inside, data.open, np.where(data.open > self.upper, self.upper, self.lower))
        self.entry_raw = self.entry_mid * data.unadjust_clean
        self.entry_tick = exit_test.ticks(self.entry_raw, panel.dates)


def flags_of(data: exit_test.Data, engine: Engine) -> dict:
    """근사 E 의 조건(행마다). 순서가 보고 순서."""
    panel = data.panel

    with np.errstate(invalid="ignore", divide="ignore"):
        open_deviation = (data.open - data.sma20_previous) / data.sma20_previous
        atr_ratio = data.atr14 / data.sma20_previous
        liquidity = exit_test.AMOUNT / data.previous_20d
        result = {
            "signal_period": (panel.dates >= exit_test.SIGNAL_FIRST) & (panel.dates <= exit_test.SIGNAL_LAST),
            "zone_interval_exists": engine.interval,
            "touched_in_day": engine.touched,
            "atr14_le_4.5": atr_ratio <= exit_test.ATR_MAX,
            "open_dev_ge_-1": open_deviation >= exit_test.OPEN_DEV_MIN,
            "price_2000_100000": (engine.entry_raw >= exit_test.MIN_PRICE) & (engine.entry_raw <= exit_test.MAX_PRICE),
            "kospi_gap_gt_-1": ~(data.index_gap <= exit_test.RISK_OFF),
            "previous_turnover_ge_10억": data.previous_day_turnover >= exit_test.MIN_TURNOVER,
            "previous_rank_le_200": data.previous_rank <= exit_test.TOP_N,
            "order_le_0.5pct_of_20d": liquidity <= exit_test.RATIO_CAP,
        }

    data.open_deviation = open_deviation
    data.atr_ratio = atr_ratio
    return result


BASE_FLAGS = ("signal_period", "zone_interval_exists", "touched_in_day", "atr14_le_4.5", "open_dev_ge_-1", "price_2000_100000",
              "kospi_gap_gt_-1")
LADDER = (
    ("E∧U1∧상한", ("previous_turnover_ge_10억", "previous_rank_le_200", "order_le_0.5pct_of_20d")),
    ("E∧U2∧상한", ("previous_turnover_ge_10억", "order_le_0.5pct_of_20d")),
    ("E∧U0∧상한", ("order_le_0.5pct_of_20d",)),
)
EXTRA_OPTIONS = (
    ("E∧U1(상한 없음)", ("previous_turnover_ge_10억", "previous_rank_le_200")),
    ("E∧U2(상한 없음)", ("previous_turnover_ge_10억",)),
    ("E 만(후보 조건 없음)", ()),
)


def same_day_universe(data: exit_test.Data) -> np.ndarray:
    """진단 S(미래 정보): 그날 최종 거래대금 ≥ 10억 이고 (그날 거래대금 순위 ≤ 200 또는 그날 등락률 ≥ 1% 중 상위 100)."""
    panel = data.panel
    rank = exit_test.turnover_rank(panel, panel.day_turnover)
    previous_close = np.r_[np.nan, data.close[:-1]]
    previous_close[panel.starts[:-1]] = np.nan

    with np.errstate(invalid="ignore", divide="ignore"):
        change = data.close / previous_close - 1.0

    frame = pd.DataFrame({"position": panel.market_position, "change": np.where(change >= 0.01, change, -np.inf)})
    change_rank = frame.groupby("position")["change"].rank(ascending=False, method="first").to_numpy()
    turnover_ok = np.nan_to_num(panel.day_turnover) >= exit_test.MIN_TURNOVER
    return turnover_ok & ((rank <= exit_test.TOP_N) | ((change >= 0.01) & (change_rank <= 100)))


# ── 라이브 겹침 ─────────────────────────────────────────────────────────────

def live_buys() -> pd.DataFrame:
    frames = []

    for account, folder in (("모의", "logs"), ("실계좌", "logs_live")):
        for path in sorted((exit_test.REPOSITORY / "Quant" / "build_win" / folder).glob("trades_*.csv")):
            table = pd.read_csv(path, dtype=str)
            text = table.get("entry_reason", pd.Series("", index=table.index)).fillna("") + table.get("reason", pd.Series("", index=table.index)).fillna("")
            table["text"] = text
            part = table[(table["event"] == "FILL") & (table["side"] == "BUY") & text.str.contains("정배열눌림")].copy()

            if len(part):
                part["account"] = account
                part["source"] = str(path.relative_to(exit_test.REPOSITORY)).replace("\\", "/")
                frames.append(part)

    buys = pd.concat(frames, ignore_index=True)
    buys["date"] = buys["ts_kst"].str.slice(0, 10).str.replace("-", "").astype(np.int64)
    buys["time"] = buys["ts_kst"].str.slice(11, 16)
    buys["fill_price"] = buys["fill_price"].astype(float)
    buys["ticker"] = buys["ticker"].str.zfill(6)
    first = buys.sort_values("ts_kst").groupby(["account", "date", "ticker"], as_index=False).agg(
        time=("time", "first"), fill_price=("fill_price", "first"), text=("text", "first"), source=("source", "first"))
    sma20 = first["text"].str.extract(r"일봉SMA20=([\d.]+)")[0].astype(float)
    price = first["text"].str.extract(r"현재가=([\d.]+)")[0].astype(float)
    first["engine_sma20"] = sma20
    first["engine_price"] = price
    return first.drop(columns="text")


def live_coverage(data: exit_test.Data, engine: Engine, flags: dict, options: dict, options_paper: dict, same_day: np.ndarray):
    panel = data.panel
    buys = live_buys()
    row_of = {(panel.codes[panel.stock_of_row[row]], int(panel.dates[row])): row for row in
              np.flatnonzero(panel.dates >= int(buys["date"].min()))}
    names = list(flags)
    records = []

    for buy in buys.itertuples():
        row = row_of.get((buy.ticker, int(buy.date)))
        record = {"account": buy.account, "date": int(buy.date), "ticker": buy.ticker, "time": buy.time, "fill_price": buy.fill_price,
                  "source": buy.source, "in_panel": row is not None}

        if row is not None:
            to_clean = 1.0 / data.unadjust_clean[row]
            fill_clean = buy.fill_price * to_clean
            raw = data.unadjust_clean[row]
            record["name"] = panel.names[panel.stock_of_row[row]]
            record["after_1530"] = buy.time >= "15:30"
            record["fill_inside_day_range"] = bool(data.low[row] * raw - 1e-6 <= buy.fill_price <= data.high[row] * raw + 1e-6)
            record["fill_aligned_engine"] = bool(fill_clean > engine.align_floor[row])
            record["fill_in_band"] = bool(engine.band_low[row] <= fill_clean <= engine.band_high[row])
            record["fill_in_interval"] = bool(engine.lower[row] <= fill_clean <= engine.upper[row])
            record["approx_entry_raw"] = float(engine.entry_raw[row])
            record["fill_vs_approx_entry_pct"] = (buy.fill_price / engine.entry_raw[row] - 1.0) * 100.0 if engine.interval[row] else np.nan

            if np.isfinite(buy.engine_sma20) and np.isfinite(buy.engine_price):
                panel_sma20 = (data.before[20][row] + buy.engine_price * to_clean) / 20.0 * raw
                record["engine_sma20"] = buy.engine_sma20
                record["panel_folded_sma20_at_engine_price"] = panel_sma20
                record["sma20_engine_vs_panel_pct"] = (buy.engine_sma20 / panel_sma20 - 1.0) * 100.0

            record["open_dev_pct"] = float(data.open_deviation[row] * 100.0)
            record["atr_ratio_pct"] = float(data.atr_ratio[row] * 100.0)
            record["previous_rank"] = float(data.previous_rank[row])
            record["previous_day_turnover_억"] = float(data.previous_day_turnover[row] / 1e8)
            passed = {name: bool(flags[name][row]) for name in names}

            if buy.account == "모의":
                passed["price_2000_100000"] = bool(engine.entry_raw[row] >= PAPER_MIN_PRICE)   # 모의는 상한 없음

            failed = [name for name in names if not passed[name]]
            record["failed_flags"] = "·".join(failed)
            record["first_failed_base"] = next((name for name in BASE_FLAGS if name in failed), "")
            record["in_S_same_day_lookahead"] = bool(same_day[row])

            for label, mask in (options_paper if buy.account == "모의" else options).items():
                record[f"in[{label}]"] = bool(mask[row])

        records.append(record)

    detail = pd.DataFrame(records)
    rows = []

    detail["same_config"] = (detail["account"] == "실계좌") | (detail["date"] >= SAME_CONFIG_FROM)

    for account in ("모의 전체", "모의", "실계좌", "합"):
        if account == "모의 전체":
            part = detail[detail["account"] == "모의"]
        else:
            part = detail[detail["same_config"]] if account == "합" else detail[(detail["account"] == account) & detail["same_config"]]

        inside = part[part["in_panel"]].infer_objects()
        record = {"account": account, "live_buys": len(part), "in_panel": len(inside),
                  "fill_aligned_engine": float(inside["fill_aligned_engine"].mean()),
                  "fill_in_band": float(inside["fill_in_band"].mean()),
                  "fill_in_interval": float(inside["fill_in_interval"].mean()),
                  "after_1530_share": float(inside["after_1530"].mean()),
                  "fill_outside_day_range": int((~inside["fill_inside_day_range"]).sum()),
                  "sma20_engine_vs_panel_median_pct": float(inside["sma20_engine_vs_panel_pct"].median()),
                  "sma20_abs_gap_le_0.5pct_share": float((inside["sma20_engine_vs_panel_pct"].abs() <= 0.5).mean())}

        for label in options:
            record[f"cover[{label}]"] = float(inside[f"in[{label}]"].mean())

        record["cover[진단 E∧S 그날 최종값, 미래 정보]"] = float((inside["in[E 만(후보 조건 없음)]"] & inside["in_S_same_day_lookahead"]).mean())
        rows.append(record)

    return detail, pd.DataFrame(rows)


def gap_table(detail: pd.DataFrame, chosen: str) -> pd.DataFrame:
    """고른 판에 안 든 실매수의 첫 탈락 조건(계좌별)."""
    inside = detail[detail["in_panel"] & detail["same_config"]].infer_objects()
    rows = []
    order = list(BASE_FLAGS) + ["previous_turnover_ge_10억", "previous_rank_le_200", "order_le_0.5pct_of_20d"]
    needed = set(BASE_FLAGS) | set(dict(LADDER + EXTRA_OPTIONS)[chosen])

    for account, part in inside.groupby("account"):
        missed = part[~part[f"in[{chosen}]"]]

        for name in order:
            if name not in needed:
                continue

            first = missed["failed_flags"].fillna("").apply(lambda text: next((item for item in order if item in needed and item in text.split("·")), "") == name)
            rows.append({"account": account, "option": chosen, "first_failed_flag": name, "missed_buys": int(first.sum()),
                         "share_of_buys": float(first.sum() / len(part))})

    return pd.DataFrame(rows)


# ── 계산 ────────────────────────────────────────────────────────────────────

def evaluate(data: exit_test.Data, engine: Engine, rules: pd.DataFrame, entries: np.ndarray) -> dict:
    panel = data.panel
    dates = panel.dates
    entry_mid = engine.entry_mid[entries]
    entry_raw = engine.entry_raw[entries]
    entry_tick = engine.entry_tick[entries]
    fills = entry_mid * (entry_raw + entry_tick) / entry_raw
    ends = data.stock_end[entries]
    exit_row, exit_price, reason = exit_test.run_exits(
        entries, False, fills, ends, data.listed_row[entries], data.delist_multiplier_row[entries], rules["kind"].to_numpy(np.int64),
        rules["tp"].to_numpy(np.float64), rules["sl"].to_numpy(np.float64), rules["param"].to_numpy(np.int64), data.open, data.high,
        data.low, data.close, data.before[20], engine.aligned_close, engine.aligned_previous, engine.folded_open_aligned, data.sma[20],
        data.states, panel.market_position.astype(np.int64))
    gross = exit_price / entry_mid[:, None]
    buy_ratio = exit_test.AMOUNT / data.previous_20d[entries]
    buy_cost = (entry_raw / (entry_raw + entry_tick) / (1.0 + round11_test.COMMISSION + round11_test.FLAT_IMPACT)
                / (1.0 + round11_test.LINEAR * np.nan_to_num(buy_ratio)))
    sell_raw = exit_price * data.unadjust_clean[exit_row]
    sell_dates = dates[exit_row]
    sell_tick = exit_test.ticks(sell_raw.ravel(), sell_dates.ravel()).reshape(sell_raw.shape)
    sell_tax = round11_test.sell_tax(sell_dates.ravel()).reshape(sell_raw.shape)
    sell_base = data.previous_20d[exit_row]
    sell_ratio = np.where(np.isfinite(sell_base) & (np.nan_to_num(sell_base) > 0), exit_test.AMOUNT / np.where(sell_base > 0, sell_base, 1.0),
                          np.nan_to_num(buy_ratio)[:, None])
    sell_cost = (np.maximum(sell_raw - sell_tick, 0.0) / np.where(sell_raw > 0, sell_raw, 1.0)
                 * (1.0 - round11_test.COMMISSION - sell_tax - round11_test.FLAT_IMPACT)
                 * np.maximum(0.0, 1.0 - round11_test.LINEAR * sell_ratio))
    net = (gross * buy_cost[:, None] * sell_cost - 1.0) * 100.0
    day = panel.market_position[entries].astype(np.int64)
    return {"entries": entries, "entry_mid": entry_mid, "buy_cost": buy_cost, "exit_row": exit_row, "reason": reason, "net": net,
            "hold": panel.market_position[exit_row].astype(np.int64) - day[:, None], "day": day,
            "month": panel.month_index[entries], "year": panel.years[entries], "stock": panel.stock_of_row[entries].astype(np.int64)}


def flipped_t(sums: np.ndarray, counts: np.ndarray, signs: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """칸 × 묶음 합·건수에서 관측 t(칸) 와 뒤집은 t(칸 × 뒤집기). round3_test.flip_null 과 같은 식."""
    count = counts.sum(axis=1)
    groups = (counts > 0).sum(axis=1)
    scale = np.where(groups > 1, groups / np.maximum(groups - 1.0, 1.0), np.nan)
    squared = (sums ** 2).sum(axis=1)
    count_squared = (counts ** 2).sum(axis=1)
    weighted = sums * counts

    def t_of(total, cross):
        mean = total / count[:, None]
        residual = np.maximum(squared[:, None] - 2.0 * mean * cross + mean ** 2 * count_squared[:, None], 0.0)
        error = np.sqrt(scale[:, None] * residual) / count[:, None]

        with np.errstate(invalid="ignore", divide="ignore"):
            return np.where(residual > 1e-18, mean / error, np.nan)

    observed = t_of(sums.sum(axis=1)[:, None], weighted.sum(axis=1)[:, None])[:, 0]
    flipped = np.empty((sums.shape[0], signs.shape[0]))

    for begin in range(0, signs.shape[0], 100):
        part = signs[begin:begin + 100].T
        flipped[:, begin:begin + 100] = t_of(sums @ part, weighted @ part)

    return observed, flipped


def group_sums(values: np.ndarray, groups: np.ndarray, size: int) -> tuple[np.ndarray, np.ndarray]:
    cells = values.shape[1]
    sums = np.zeros((cells, size))
    counts = np.zeros((cells, size))

    for cell in range(cells):
        valid = np.isfinite(values[:, cell])
        np.add.at(sums[cell], groups[valid], values[valid, cell])
        np.add.at(counts[cell], groups[valid], 1.0)

    return sums, counts


def quantile_of_max(matrix: np.ndarray) -> float:
    with np.errstate(invalid="ignore"):
        return float(np.quantile(np.nanmax(matrix, axis=0), 0.95))


# ── 슬롯 ────────────────────────────────────────────────────────────────────

@njit(parallel=True, cache=True)
def slot_accept(order, day, exit_day, stock, stock_count, slots):
    """칸마다 시간순으로 슬롯 slots 개에 넣는다. 그 칸에서 같은 종목을 보유 중이면 건너뛴다. 매도 다음 날부터 슬롯이 빈다."""
    count, cells = exit_day.shape
    accept = np.zeros((count, cells), dtype=np.bool_)

    for cell in prange(cells):
        held_until = np.full(stock_count, -1, dtype=np.int64)
        slot_end = np.full(slots, -1, dtype=np.int64)

        for position in range(count):
            index = order[position]
            today = day[index]

            if held_until[stock[index]] >= today:
                continue

            free = -1

            for slot in range(slots):
                if slot_end[slot] < today:
                    free = slot
                    break

            if free < 0:
                continue

            slot_end[free] = exit_day[index, cell]
            held_until[stock[index]] = exit_day[index, cell]
            accept[index, cell] = True

    return accept


@njit(parallel=True, cache=True)
def slot_pnl(accept, entries, exit_row, net, entry_mid, buy_cost, close, position, market_count, amount):
    """칸 × 시장일 평가 손익 변화량. 보유 중엔 종가 평가(매수 비용 반영), 매도일엔 순수익."""
    count, cells = accept.shape
    delta = np.zeros((cells, market_count))

    for cell in prange(cells):
        for index in range(count):
            if not accept[index, cell]:
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


def slot_tables(data: exit_test.Data, rules: pd.DataFrame, result: dict, years: float) -> tuple[pd.DataFrame, pd.DataFrame]:
    panel = data.panel
    entries = result["entries"]
    exit_day = panel.market_position[result["exit_row"]].astype(np.int64)
    previous_turnover = np.nan_to_num(data.previous_day_turnover[entries], nan=-1.0)
    order = np.lexsort((result["stock"], -previous_turnover, result["day"])).astype(np.int64)
    accept = slot_accept(order, result["day"], exit_day, result["stock"], panel.stock_count, SLOTS)
    delta = slot_pnl(accept, entries, result["exit_row"], result["net"], result["entry_mid"], result["buy_cost"], data.close,
                     panel.market_position.astype(np.int64), panel.market_count, exit_test.AMOUNT)
    first_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_FIRST))
    last_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_LAST, side="right")) - 1
    equity = CAPITAL + np.cumsum(delta[:, first_day:last_day + 1], axis=1)
    market_days = last_day - first_day + 1
    rows = []

    for cell in rules["cell"]:
        taken = accept[:, cell]
        hold = result["hold"][taken, cell]
        net = result["net"][taken, cell]
        reason = result["reason"][taken, cell]
        curve = equity[cell]
        peak = np.maximum.accumulate(curve)
        end = curve[-1]
        rows.append({"cell": cell, "rule": rules.at[cell, "rule"], "trades": int(taken.sum()),
                     "unclosed_valued_at_20261008_close": int((reason == 8).sum()), "delisted": int((reason == 9).sum()),
                     "end_equity_krw": float(end), "pnl_krw": float(end - CAPITAL),
                     "capital_exhausted": bool(curve.min() <= 0),
                     "annualized_pct": float(((end / CAPITAL) ** (1.0 / years) - 1.0) * 100.0) if curve.min() > 0 else np.nan,
                     "simple_annual_pct": float((end - CAPITAL) / CAPITAL / years * 100.0),
                     "mdd_krw": float((curve - peak).min()),
                     "mdd_pct_of_capital": float((curve - peak).min() / CAPITAL * 100.0),
                     "mdd_pct_of_peak": float(((curve - peak) / peak).min() * 100.0) if curve.min() > 0 else np.nan,
                     "net_mean_pct": float(net.mean()) if len(net) else np.nan,
                     "net_per_hold_day_bp": float(net.sum() / np.maximum(hold, 1).sum() * 100.0) if len(net) else np.nan,
                     "slot_utilization": float(np.maximum(hold, 1).sum() / (SLOTS * market_days)),
                     "hold_median": float(np.median(hold)) if len(hold) else np.nan,
                     "hold_p90": float(np.percentile(hold, 90)) if len(hold) else np.nan,
                     "hold_max": int(hold.max()) if len(hold) else 0,
                     "longest_hold_share_of_period": float(hold.max() / market_days) if len(hold) else np.nan})

    curves = pd.DataFrame(equity.T, columns=[f"cell{cell}" for cell in rules["cell"]])
    curves.insert(0, "date", panel.market_dates[first_day:last_day + 1])
    return pd.DataFrame(rows), curves


ORDER_DRAWS = 20


def slot_order_spread(data: exit_test.Data, rules: pd.DataFrame, result: dict, cells: list[int], years: float) -> pd.DataFrame:
    """사후 진단: 같은 날 고르는 순서를 무작위로 20 번 바꿔 슬롯 판이 순서 운에 얼마나 흔들리는지 본다(seed 20261010 + 1)."""
    panel = data.panel
    entries = result["entries"]
    exit_row = np.ascontiguousarray(result["exit_row"][:, cells])
    exit_day = panel.market_position[exit_row].astype(np.int64)
    net = np.ascontiguousarray(result["net"][:, cells])
    generator = np.random.default_rng(common.SEED + 1)
    first_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_FIRST))
    last_day = int(np.searchsorted(panel.market_dates, exit_test.SIGNAL_LAST, side="right")) - 1
    rows = []

    for draw in range(ORDER_DRAWS):
        order = np.lexsort((generator.random(len(entries)), result["day"])).astype(np.int64)
        accept = slot_accept(order, result["day"], exit_day, result["stock"], panel.stock_count, SLOTS)
        delta = slot_pnl(accept, entries, exit_row, net, result["entry_mid"], result["buy_cost"], data.close,
                         panel.market_position.astype(np.int64), panel.market_count, exit_test.AMOUNT)
        equity = CAPITAL + np.cumsum(delta[:, first_day:last_day + 1], axis=1)

        for position, cell in enumerate(cells):
            curve = equity[position]
            rows.append({"cell": cell, "draw": draw, "trades": int(accept[:, position].sum()),
                         "simple_annual_pct": float((curve[-1] - CAPITAL) / CAPITAL / years * 100.0),
                         "mdd_pct_of_capital": float((curve - np.maximum.accumulate(curve)).min() / CAPITAL * 100.0)})

    frame = pd.DataFrame(rows)
    summary = frame.groupby("cell").agg(trades_median=("trades", "median"), annual_min=("simple_annual_pct", "min"),
                                        annual_median=("simple_annual_pct", "median"), annual_max=("simple_annual_pct", "max"),
                                        share_positive=("simple_annual_pct", lambda values: float(np.mean(values > 0))),
                                        mdd_median=("mdd_pct_of_capital", "median")).reset_index()
    summary.insert(1, "rule", [rules.at[cell, "rule"] for cell in summary["cell"]])
    return summary


# ── 판정 ────────────────────────────────────────────────────────────────────

def judge(result: dict, rules: pd.DataFrame, slots: pd.DataFrame, panel, signs_month, signs_stock):
    live_cell = int(rules.loc[rules["rule"] == exit_test.LIVE_RULE, "cell"].iloc[0])
    pair = result["net"] - result["net"][:, [live_cell]]
    others = np.flatnonzero(rules["cell"].to_numpy() != live_cell)
    month_sums, month_counts = group_sums(pair[:, others], result["month"], panel.month_count)
    stock_sums, stock_counts = group_sums(pair[:, others], result["stock"], panel.stock_count)
    used_stocks = stock_counts.sum(axis=0) > 0
    stock_sums, stock_counts = stock_sums[:, used_stocks], stock_counts[:, used_stocks]
    signs_stock = signs_stock[:, used_stocks]
    t_month, flip_month = flipped_t(month_sums, month_counts, signs_month)
    t_stock, flip_stock = flipped_t(stock_sums, stock_counts, signs_stock)
    bars = {"month_up": quantile_of_max(flip_month), "month_down": quantile_of_max(-flip_month),
            "stock_up": quantile_of_max(flip_stock), "stock_down": quantile_of_max(-flip_stock),
            "min_stat_up_reference": quantile_of_max(np.fmin(flip_month, flip_stock)),
            "min_stat_down_reference": quantile_of_max(np.fmin(-flip_month, -flip_stock))}
    main_bar = max(bars["month_up"], bars["month_down"], bars["stock_up"], bars["stock_down"])
    near = exit_test.neighbours(rules)
    rows = []
    years = sorted(np.unique(result["year"]))

    for position, cell in enumerate(others):
        values = pair[:, cell]
        valid = np.isfinite(values)
        year_means = [values[valid & (result["year"] == year)].mean() for year in years if (valid & (result["year"] == year)).any()]
        rows.append({"cell": cell, "rule": rules.at[cell, "rule"], "group": rules.at[cell, "group"], "n": int(valid.sum()),
                     "net_mean": float(np.nanmean(result["net"][:, cell])), "hold_mean": float(result["hold"][:, cell].mean()),
                     "hold_median": float(np.median(result["hold"][:, cell])), "unclosed": int((result["reason"][:, cell] == 8).sum()),
                     "delisted": int((result["reason"][:, cell] == 9).sum()),
                     "pair_mean": float(values[valid].mean()), "pair_median": float(np.median(values[valid])),
                     "pair_share_cell_better": float(np.mean(values[valid] > 0)),
                     "t_month": float(t_month[position]), "t_stock": float(t_stock[position]),
                     "t_min": float(np.fmin(t_month[position], t_stock[position])),
                     "t_max": float(np.fmax(t_month[position], t_stock[position])),
                     "share_positive_years": float(np.mean(np.array(year_means) > 0)), "years_with_trades": len(year_means)})

    table = pd.DataFrame(rows).set_index("cell")
    sign = np.sign(table["pair_mean"])
    table["neighbours"] = ["·".join(str(other) for other in near[cell] if other != live_cell) for cell in table.index]
    table["neighbours_same_sign"] = [bool(any(other != live_cell for other in near[cell])
                                          and all(np.sign(table.at[other, "pair_mean"]) == sign[cell] for other in near[cell] if other != live_cell))
                                     for cell in table.index]
    table = table.join(slots.set_index("cell")[["simple_annual_pct", "annualized_pct", "capital_exhausted", "mdd_pct_of_capital", "trades"]].rename(columns={"trades": "slot_trades"}))
    table["main_bar"] = main_bar
    table["over_main_bar"] = table["t_min"] >= main_bar
    table["over_reference_3.5_mine"] = table["t_min"] >= REFERENCE_LINE
    table["worse_than_a"] = table["t_max"] <= -main_bar
    table["passes"] = (table["over_main_bar"] & table["neighbours_same_sign"] & (table["share_positive_years"] >= exit_test.JUDGE_POSITIVE_YEARS)
                       & (table["pair_median"] > 0) & (table["simple_annual_pct"] > 0))
    year_rows = []

    for cell in rules["cell"]:
        for year in years:
            part = result["year"] == year
            year_rows.append({"cell": cell, "rule": rules.at[cell, "rule"], "year": int(year), "n": int(part.sum()),
                              "net_mean": float(result["net"][part, cell].mean()) if part.any() else np.nan,
                              "pair_mean": float(pair[part, cell].mean()) if part.any() else np.nan})

    return table.reset_index(), {"main_bar": main_bar, **bars, "reference_line_mine": REFERENCE_LINE, "cells": len(others),
                                 "flips": int(signs_month.shape[0]), "seed": common.SEED,
                                 "rule": "주 기준선 = max(달 뒤집기 최대 t 95%, 종목 뒤집기 최대 t 95%) 위·아래 중 큰 값. 판정 t = min(달 t, 종목 t)"}, pd.DataFrame(year_rows)


ENGINE_TABLE = [
    ("정배열 정의", "접은 SMA5 > 접은 SMA10 > 접은 SMA20×(1−tol), tol=0(config 키 없음 → 기본 0). SMA60 없음",
     "Quant/src/universe/MaAlign.cpp aligned", "SMA5>10>20>60(SMA60 더 요구)", "SMA5>10>20(엔진과 같음)"),
    ("진입 정배열 축", "접은 정배열만(zone = aligned && band)", "Quant/src/strategy/DeviationScaleZone.cpp judge_zone",
     "전날 확정 AND 시가 접은 값", "그날 가격 구간에서 접은 값"),
    ("보유 유지 축", "(전날 확정 OR 접은 정배열) AND 넓힌 존(−12 – +9%)", "DeviationScaleZone.cpp judge_zone hold_zone",
     "같음(SMA60 포함)", "같음(SMA60 없음)"),
    ("SMA 원천", "KIS 일봉 종가(오늘 제외, daily_lookback 250, 20 봉 미만이면 판정 없음)에 현재가를 접음. 장중 3분봉은 매수 지정가 기준선에만",
     "DeviationScaleZone.cpp daily_simple_moving_averages·fold_today", "패널 정제 수정 종가에 시가를 접음", "패널 정제 수정 종가에 그날 가격 구간을 접음"),
    ("존", "접은 SMA20 대비 −8 – +5%(존이 켜진 뒤·보유 중엔 −12 – +9%)", "DeviationScaleZone.cpp zone_band",
     "시가 기준 −8 – +5%", "그날 [저가·고가] 와 겹치는 구간"),
    ("하루 진입 필터", "09:03 뒤 첫 평가 가격의 전날 SMA20 대비 이격 ≥ −1% · ATR14/전날 SMA20 ≤ 4.5%", "DeviationScaleZone.cpp day_entry_allowed",
     "시가 기준", "시가 기준(같음)"),
    ("후보 목록", "당일 누적 거래대금 상위 200 ∪ 시세판 100 ∪ 거래량 급증 40 ∪ 등락률 상위 100(≥1%) → 접은 정배열·이격 ≤ +5% → 점수 상위 100",
     "Quant/src/universe/UniverseFeatures.cpp·UniverseScoring.cpp", "전날 거래대금 ≥ 10억·전날 순위 ≤ 200", "사다리 U1·U2·U0(choices 11·13)"),
    ("지수 게이트", "코스피·코스닥 장중 등락률 ≤ −1% 이면 막고 재개선까지 걸쇠(코스닥 종목은 두 지수 다)", "Quant/src/universe/UniverseRiskGate.cpp build_market_gate",
     "코스피 시가 갭", "코스피 시가 갭(같음)"),
]


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    rules = exit_test.build_rules()
    data = exit_test.Data()
    panel = data.panel
    exit_test.MONTHS = panel.month_count
    engine = Engine(data)
    flags = flags_of(data, engine)
    base = np.ones(panel.row_count, dtype=bool)

    for name in BASE_FLAGS:
        base &= flags[name]

    options = {}

    for label, extra in LADDER + EXTRA_OPTIONS:
        mask = base.copy()

        for name in extra:
            mask &= flags[name]

        options[label] = mask

    paper_price = engine.entry_raw >= PAPER_MIN_PRICE
    options_paper = {}

    for label, extra in LADDER + EXTRA_OPTIONS:
        mask = paper_price.copy()

        for name in BASE_FLAGS + tuple(extra):
            if name != "price_2000_100000":
                mask &= flags[name]

        options_paper[label] = mask

    for label, extra in LADDER:
        for target, price in ((options, flags["price_2000_100000"]), (options_paper, paper_price)):
            mask = price.copy()

            for name in BASE_FLAGS + tuple(extra):
                if name not in ("price_2000_100000", "kospi_gap_gt_-1"):
                    mask &= flags[name]

            target[f"{label} 지수 게이트 뺌(진단)"] = mask

    same_day = same_day_universe(data)
    print(f"준비 {time.time() - started:.0f}초 · 신호 " + ", ".join(f"{label} {int(options[label].sum())}" for label, _ in LADDER + EXTRA_OPTIONS), flush=True)
    detail, coverage = live_coverage(data, engine, flags, options, options_paper, same_day)
    common.write_csv(OUTPUT / "live_buys_detail.csv", detail)
    common.write_csv(OUTPUT / "live_coverage.csv", coverage)

    if "--coverage-only" in sys.argv:
        print(coverage.T.to_string(), flush=True)
        return

    chosen, reason_text = None, ""

    for label, _ in LADDER:
        by_account = coverage.set_index("account")[f"cover[{label}]"]

        if by_account["모의"] >= COVERAGE_GOAL and by_account["실계좌"] >= COVERAGE_GOAL:
            chosen, reason_text = label, "두 계좌 모두 50% 이상인 첫 판"
            break

    if chosen is None:
        total = {label: float(coverage.set_index("account").at["합", f"cover[{label}]"]) for label, _ in LADDER}
        chosen = max(total, key=total.get)
        reason_text = "50% 에 미치는 판이 없어 두 계좌 합 겹침이 가장 큰 판"

    print(coverage.T.to_string(), flush=True)
    print(f"고른 판: {chosen} ({reason_text})", flush=True)
    candidates = np.flatnonzero(options[chosen]).astype(np.int64)
    fills = engine.entry_mid[candidates] * (engine.entry_raw[candidates] + engine.entry_tick[candidates]) / engine.entry_raw[candidates]
    keep = exit_test.dedupe(candidates, panel.stock_of_row.astype(np.int64), fills, data.stock_end[candidates], data.listed_row[candidates],
                     data.delist_multiplier_row[candidates], data.open, data.high, data.low, data.close, data.before[20],
                     engine.aligned_close, engine.aligned_previous, engine.folded_open_aligned, data.sma[20], data.states,
                     panel.market_position.astype(np.int64))
    p_entries = candidates[keep]
    print(f"신호 {len(candidates)} → 진입 묶음 {len(p_entries)} ({time.time() - started:.0f}초)", flush=True)
    result_all = evaluate(data, engine, rules, candidates)
    index_of = {row: position for position, row in enumerate(candidates)}
    picked = np.array([index_of[row] for row in p_entries], dtype=np.int64)
    result = {key: (value[picked] if isinstance(value, np.ndarray) and value.shape[0] == len(candidates) else value)
              for key, value in result_all.items()}
    years = ((pd.Timestamp(str(exit_test.SIGNAL_LAST)) - pd.Timestamp(str(exit_test.SIGNAL_FIRST))).days) / 365.25
    slots, curves = slot_tables(data, rules, result_all, years)
    generator = np.random.default_rng(common.SEED)
    signs_month = np.where(generator.random((round3_test.FLIPS, panel.month_count)) < 0.5, -1.0, 1.0)
    signs_stock = np.where(generator.random((signs_month.shape[0], panel.stock_count)) < 0.5, -1.0, 1.0)
    table, bars, year_table = judge(result, rules, slots, panel, signs_month, signs_stock)
    live_cell = int(rules.loc[rules["rule"] == exit_test.LIVE_RULE, "cell"].iloc[0])
    live_row = {"cell": live_cell, "rule": exit_test.LIVE_RULE, "n": len(p_entries), "net_mean": float(result["net"][:, live_cell].mean()),
                "net_t_month": round6_test.month_cluster_t(result["net"][:, live_cell], result["month"]),
                "net_t_stock": round6_test.month_cluster_t(result["net"][:, live_cell], result["stock"]),
                "net_median": float(np.median(result["net"][:, live_cell])), "hold_mean": float(result["hold"][:, live_cell].mean())}
    common.write_csv(OUTPUT / "engine_definition.csv", pd.DataFrame(ENGINE_TABLE, columns=["item", "engine", "code", "round1_approx", "round2_approx"]))
    common.write_csv(OUTPUT / "live_coverage.csv", coverage)
    common.write_csv(OUTPUT / "live_buys_detail.csv", detail)
    common.write_csv(OUTPUT / "coverage_gaps.csv", gap_table(detail, chosen))
    common.write_csv(OUTPUT / "cell_table.csv", table)
    common.write_csv(OUTPUT / "year_table.csv", year_table)
    common.write_csv(OUTPUT / "slot_table.csv", slots)
    spread_cells = sorted(set([live_cell, 9, 37] + table.loc[table["passes"], "cell"].tolist()
                              + table.loc[table["over_main_bar"], "cell"].tolist()))
    common.write_csv(OUTPUT / "slot_order_spread.csv", slot_order_spread(data, rules, result_all, spread_cells, years))
    curves.to_parquet(OUTPUT / "slot_equity.parquet", index=False)
    count, cells = result["net"].shape
    trades = pd.DataFrame({
        "code": np.repeat(np.array(panel.codes)[result["stock"]], cells),
        "entry_date": np.repeat(panel.dates[p_entries], cells),
        "entry_price_raw": np.repeat(engine.entry_raw[p_entries], cells).astype(np.float32),
        "cell": np.tile(np.arange(cells), count).astype(np.int16),
        "exit_date": panel.dates[result["exit_row"]].ravel(),
        "reason": result["reason"].ravel(),
        "hold_days": result["hold"].ravel().astype(np.int32),
        "net_pct": result["net"].ravel().astype(np.float32),
        "pair_vs_a_pct": (result["net"] - result["net"][:, [live_cell]]).ravel().astype(np.float32)})
    trades.to_parquet(OUTPUT / "trades.parquet", index=False)
    command = "py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round2.py"
    common.write_text(OUTPUT / "run_command.txt", command + "\n")
    hashed = {name: exit_test.file_hash(OUTPUT / name) for name in ("live_coverage.csv", "live_buys_detail.csv", "cell_table.csv", "year_table.csv",
                                                             "slot_table.csv", "coverage_gaps.csv", "slot_order_spread.csv")}
    metrics = {"reproduction": panel.reproduction(), "command": command, "seconds": round(time.time() - started, 1),
               "signals": {label: int(mask.sum()) for label, mask in options.items()}, "chosen": chosen, "chosen_reason": reason_text,
               "candidates": int(len(candidates)), "entries": int(len(p_entries)), "years": years, "bars": bars, "live_a": live_row,
               "slots": SLOTS, "capital": CAPITAL, "output_sha256_16": hashed}
    common.write_json(OUTPUT / "metrics.json", metrics)
    print(f"끝 {time.time() - started:.0f}초", flush=True)
    print(bars, flush=True)
    print(live_row, flush=True)
    print(table[["rule", "n", "net_mean", "pair_mean", "pair_median", "t_month", "t_stock", "t_min", "share_positive_years",
                 "neighbours_same_sign", "hold_median", "unclosed", "simple_annual_pct", "mdd_pct_of_capital", "passes"]].to_string(index=False), flush=True)


if __name__ == "__main__":
    main()
