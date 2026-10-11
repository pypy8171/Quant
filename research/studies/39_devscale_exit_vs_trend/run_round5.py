"""스터디 39 round5 — 진입 후보 C1–C5(격자 19칸)를 4회차 하네스로 시험한다. 명세: ENTRY_CANDIDATES.md.

바꾼 것(그 밖은 run_round3·run_round4 그대로 부른다):
  1. 진입: entry_candidates.VARIANTS 19칸. C1 은 장중 존 가격, C2–C5 는 t 시가 + 1호가.
  2. 체결 시점 축: C2–C5 는 신호를 계산한 날(t−1) 종가 + 1호가에도 산다(approach 3). 이 판은 t 날짜 정보(시가·코스피 갭)를 쓰지 않는다.
  3. 판정: 본 청산 칸 32개(시가 19 + 종가 13)를 따로 판정. 통과선 = max(실행 기준선, 32칸 가족 기준선, 3.0).
사전 등록: results/round5/choices_log.csv(실행 전 24 행).
실행: py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round5.py   (약 25분)
"""
from __future__ import annotations

import gc
import sys
import time
from pathlib import Path

STUDY = Path(__file__).resolve().parent
sys.path.insert(0, str(STUDY))

import run_round4 as round4     # noqa: E402  스터디 38 경로·numba 설정을 먼저 넣는다
import common     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from numba import njit, prange     # noqa: E402

import round3_test     # noqa: E402
import entry_candidates as entry_candidates     # noqa: E402

round3 = round4.round3
round2, exit_test = round3.round2, round3.exit_test
OUTPUT = STUDY / "results" / "round5"
ROUND4 = STUDY / "results" / "round4"
PASS_FLOOR = 3.0                 # strategist 가 정함(헌장 20칸 t 3.0)
EXCURSION_WINDOW = 60
PULLBACK, ENTRY_UPPER, HOLD_WIDEN = exit_test.PULLBACK, exit_test.ENTRY_UPPER, exit_test.HOLD_WIDEN
HOLD_BUCKETS = ((0, 0, "0"), (1, 1, "1"), (2, 5, "2–5"), (6, 20, "6–20"), (21, 60, "21–60"), (61, 1 << 30, "61+"))
GRID = ((0, 0), (0, 1), (1, 0), (1, 1), (2, 0), (2, 1),      # C1 존 상단(+5·0·−3) × 시가 이격(켬·끔)
        (0, 0), (0, 1), (1, 0), (1, 1),                      # C2 하위(10·20) × 추세(60·120)
        (0, 0), (0, 1), (1, 0), (1, 1),                      # C3 상위(10·20) × 수익 창(5·20)
        (0,), (1,), (2,),                                    # C4 서로 모두 옆 칸
        (0,), (1,))                                          # C5 서로 옆 칸


# ── 청산 커널: run_round4 커널 + approach 3(전날 종가 매수, 진입 당일은 종가만 봄) ──

@njit(cache=True)
def exit_rule_close_entry(kind, tp, sl, parameter, entry, end, approach, fill, open_price, high, low, close, before19, aligned_close,
                          aligned_previous, folded_open, sma20, states, position, listed, delist_multiplier):
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
                low_after = close[row] if (approach == 1 or approach == 3) else low[row]
                high_after = close[row] if (approach == 2 or approach == 3) else high[row]

                if low_after <= lower:
                    return row, min(lower, fill), lower_reason

                if high_after >= upper:
                    return row, max(upper, fill), upper_reason

            if kind == 1 and not aligned_previous[row] and not aligned_close[row]:
                return row, close[row], 4
        elif kind == 2:
            if row > entry and states[parameter, row] == -1:
                return row, close[row], 0
        elif kind == 3:
            if row > entry:
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
def run_exits_close_entry(entries, approach, fills, ends, listed, delist_multiplier, kinds, tps, sls, parameters, open_price, high, low,
                          close, before19, aligned_close, aligned_previous, folded_open, sma20, states, position):
    count = entries.shape[0]
    rules = kinds.shape[0]
    exit_row = np.empty((count, rules), dtype=np.int64)
    exit_price = np.empty((count, rules))
    reason = np.empty((count, rules), dtype=np.int8)

    for index in prange(count):
        for rule in range(rules):
            row, price, code = exit_rule_close_entry(kinds[rule], tps[rule], sls[rule], parameters[rule], entries[index], ends[index],
                                                     approach[index], fills[index], open_price, high, low, close, before19, aligned_close,
                                                     aligned_previous, folded_open, sma20, states, position, listed[index],
                                                     delist_multiplier[index])
            exit_row[index, rule] = row
            exit_price[index, rule] = price
            reason[index, rule] = code

    return exit_row, exit_price, reason


# ── 날짜별 장부: 종가에 산 매매는 진입일 지수 수익 0 ─────────────────────────

@njit(parallel=True, cache=True)
def daily_book_close_aware(keep, entries, index_id, entry_close, exit_row, net, entry_mid, buy_cost, close, position, index_open,
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
                else:
                    index_return = index_close[index_id[index], day] / index_close[index_id[index], position[row - 1]] - 1.0

                pnl[cell, day] += value - previous
                bench[cell, day] += previous * index_return
                base[cell, day] += previous
                previous = value

    return pnl, bench, base


def book_returns_close_aware(data, result: dict, net: np.ndarray, index_open: np.ndarray, index_close: np.ndarray):
    """r3.book_returns 와 같고, result["entry_close"] 가 참인 매매는 진입일 지수 수익을 0 으로 둔다."""
    panel = data.panel
    count = len(result["entries"])
    index_id = result.get("index_id", np.zeros(count, dtype=np.int64))
    entry_close = result.get("entry_close", np.zeros(count, dtype=np.bool_))
    pnl, bench, base = daily_book_close_aware(result["keep"], result["entries"], index_id, entry_close, result["exit_row"], net,
                                              result["entry_mid"], result["buy_cost"], data.close, panel.market_position.astype(np.int64),
                                              np.atleast_2d(index_open), np.atleast_2d(index_close), panel.market_count)
    held = base > 0

    with np.errstate(invalid="ignore", divide="ignore"):
        portfolio = np.where(held, pnl / base * 100.0, np.nan)
        index = np.where(held, bench / base * 100.0, np.nan)

    return portfolio, index


round3.book_returns = book_returns_close_aware     # board_tables 가 이 이름을 부른다(시가 매매는 값이 같다 — 라이브 재현 점검으로 확인)


# ── 전날 종가 매수 ──────────────────────────────────────────────────────────

class CloseView:
    """t−1 종가 + 1호가에 산다. 보유 판정 배열은 엔진 정의 그대로."""

    def __init__(self, data, engine):
        self.entry_mid = data.close
        self.entry_raw = data.raw_close
        self.entry_tick = data.close_tick
        self.aligned_close = engine.aligned_close
        self.aligned_previous = engine.aligned_previous
        self.folded_open_aligned = engine.folded_open_aligned


def close_pool(data, flags: dict, extra: tuple, features) -> np.ndarray:
    """t 행에 둔 풀. t 날짜의 시가·코스피 갭은 쓰지 않고 가격 범위는 t−1 무수정 종가로 본다."""
    raw_close_previous = entry_candidates.previous_known(data.raw_close, data)
    pool = flags["signal_period"] & np.isfinite(features.close_prev)

    with np.errstate(invalid="ignore"):
        pool &= (raw_close_previous >= exit_test.MIN_PRICE) & (raw_close_previous <= exit_test.MAX_PRICE)

    for name in extra:
        pool &= flags[name]

    return pool


def random_from_pool(data, pool: np.ndarray, candidates: np.ndarray) -> tuple[np.ndarray, int]:
    """r3.random_entries 와 같은 뽑기(같은 seed), 풀만 바꾼다."""
    panel = data.panel
    rows = np.flatnonzero(pool)
    pool_day = panel.market_position[rows].astype(np.int64)
    order = np.argsort(pool_day, kind="stable")
    rows, pool_day = rows[order], pool_day[order]
    candidate_positions = panel.market_position[candidates].astype(np.int64)
    start = np.searchsorted(pool_day, candidate_positions, side="left")
    size = np.searchsorted(pool_day, candidate_positions, side="right") - start
    generator = np.random.default_rng(round3.RANDOM_SEED)
    draw = generator.random(len(candidates))
    has = size > 0
    picked = rows[start[has] + np.minimum((draw[has] * size[has]).astype(np.int64), size[has] - 1)]
    return np.sort(picked).astype(np.int64), int((~has).sum())


# ── 진단 ────────────────────────────────────────────────────────────────────

class Pieces:
    """행마다 밤(전날 종가→시가)·장중(시가→종가) 로그 수익의 누적합. 같은 종목 안 구간 합에 쓴다."""

    def __init__(self, data):
        panel = data.panel
        previous_close = np.full(panel.row_count, np.nan)
        previous_close[1:] = data.close[:-1]
        offset = np.arange(panel.row_count) - panel.starts[panel.stock_of_row]
        previous_close[offset < 1] = np.nan

        with np.errstate(invalid="ignore", divide="ignore"):
            overnight = np.log(data.open / previous_close)
            intraday = np.log(data.close / data.open)

        self.missing = np.cumsum(np.concatenate([[0], ~np.isfinite(overnight) | ~np.isfinite(intraday)]))
        self.overnight = np.concatenate([[0.0], np.cumsum(np.where(np.isfinite(overnight), overnight, 0.0))])
        self.intraday = np.concatenate([[0.0], np.cumsum(np.where(np.isfinite(intraday), intraday, 0.0))])


def exit_prices(data, engine, rules, result, cell, runner) -> np.ndarray:
    """본 청산 칸의 매도가(evaluate 는 순수익만 남겨서 한 칸만 다시 돈다)."""
    keep = result["keep"][:, cell]
    entries = result["entries"][keep]
    rule = rules.iloc[[cell]]
    rows, price, _ = runner(entries, result["approach"][keep], result["fill"][keep], data.stock_end[entries], data.listed_row[entries],
                            data.delist_multiplier_row[entries], rule["kind"].to_numpy(np.int64), rule["tp"].to_numpy(np.float64),
                            rule["sl"].to_numpy(np.float64), rule["param"].to_numpy(np.int64), data.open, data.high, data.low, data.close,
                            data.before[20], engine.aligned_close, engine.aligned_previous, engine.folded_open_aligned, data.sma[20],
                            data.states, data.panel.market_position.astype(np.int64))

    if not np.array_equal(rows[:, 0], result["exit_row"][keep, cell]):
        raise SystemExit("매도가 다시 계산: 매도 행이 evaluate 와 다르다")

    return price[:, 0]


def decompose(data, engine, pieces, rules, result, cell, runner, net) -> pd.DataFrame:
    """매매마다 로그 수익을 진입일·밤·장중·청산일·비용으로 나눈다(합 = log(1 + 순수익))."""
    keep = result["keep"][:, cell]
    entry = result["entries"][keep]
    exit_row = result["exit_row"][keep, cell]
    price = exit_prices(data, engine, rules, result, cell, runner)
    entry_mid = result["entry_mid"][keep]
    same = exit_row == entry

    with np.errstate(invalid="ignore", divide="ignore"):
        gross = np.log(price / entry_mid)
        entry_part = np.where(same, gross, np.log(data.close[entry] / entry_mid))
        overnight = np.where(same, 0.0, pieces.overnight[exit_row + 1] - pieces.overnight[entry + 1])
        intraday = np.where(exit_row - 1 >= entry + 1, pieces.intraday[exit_row] - pieces.intraday[entry + 1], 0.0)
        exit_part = np.where(same, 0.0, np.log(price / data.open[exit_row]))
        cost = np.log1p(net[keep, cell] / 100.0) - gross

    missing = np.where(same, 0, pieces.missing[exit_row + 1] - pieces.missing[entry + 1]) > 0
    hold = data.panel.market_position[exit_row] - result["day"][keep]
    return pd.DataFrame({"hold": hold, "net_pct": net[keep, cell], "entry_day": entry_part * 100.0, "overnight": overnight * 100.0,
                         "intraday": intraday * 100.0, "exit_day": exit_part * 100.0, "cost": cost * 100.0, "gross": gross * 100.0,
                         "nights": np.where(same, 0, hold), "missing_piece": missing})


def decomposition_rows(frame: pd.DataFrame, label: dict) -> tuple[dict, list[dict]]:
    good = frame[~frame["missing_piece"]]
    summary = dict(label, trades=len(frame), trades_used=len(good), net_mean_pct=frame["net_pct"].mean(),
                   log_total_mean=float((good["gross"] + good["cost"]).mean()),
                   entry_day_mean=good["entry_day"].mean(), overnight_mean=good["overnight"].mean(), intraday_mean=good["intraday"].mean(),
                   exit_day_mean=good["exit_day"].mean(), cost_mean=good["cost"].mean(), nights_mean=good["nights"].mean(),
                   overnight_per_night=float(good["overnight"].sum() / max(good["nights"].sum(), 1)))
    buckets = []

    for low, high, name in HOLD_BUCKETS:
        part = frame[(frame["hold"] >= low) & (frame["hold"] <= high)]
        used = part[~part["missing_piece"]]
        buckets.append(dict(label, hold_bucket=name, trades=len(part), share=len(part) / max(len(frame), 1),
                            net_mean_pct=part["net_pct"].mean() if len(part) else np.nan,
                            win_rate=float((part["net_pct"] > 0).mean()) if len(part) else np.nan,
                            overnight_mean=used["overnight"].mean() if len(used) else np.nan,
                            intraday_mean=(used["intraday"] + used["entry_day"] + used["exit_day"]).mean() if len(used) else np.nan,
                            cost_mean=used["cost"].mean() if len(used) else np.nan))

    return summary, buckets


def excursion_row(data, result, cell, close_entry: bool, net, label: dict) -> dict:
    keep = result["keep"][:, cell] & (net[:, cell] > 0)
    entries = result["entries"][keep] + (1 if close_entry else 0)
    ends = data.stock_end[result["entries"][keep]]
    usable = entries < ends
    output = exit_test.excursions(entries[usable], result["fill"][keep][usable], ends[usable], EXCURSION_WINDOW, data.high, data.low, data.close,
                           data.panel.market_position.astype(np.int64))
    row = dict(label, winners=int(usable.sum()))

    for column, name in ((0, "mfe"), (1, "mae"), (3, "peak_day")):
        for percentile in (10, 25, 50, 75, 90):
            row[f"{name}_p{percentile}"] = float(np.nanpercentile(output[:, column], percentile)) if len(output) else np.nan

    return row


# ── 판정 ────────────────────────────────────────────────────────────────────

def grid_neighbours(variant: int) -> list[int]:
    name = entry_candidates.VARIANTS[variant][0]
    same = [other for other in range(len(entry_candidates.VARIANTS)) if entry_candidates.VARIANTS[other][0] == name and other != variant]

    if name in ("C4", "C5"):
        return same

    return [other for other in same if sum(abs(left - right) for left, right in zip(GRID[variant], GRID[other])) == 1]


def judge(records: pd.DataFrame, family: dict) -> pd.DataFrame:
    frame = records.copy()
    frame["family_bar_index"] = [family[(row.board, row.valuation)]["index"] for row in frame.itertuples()]
    frame["family_bar_random"] = [family[(row.board, row.valuation)]["random"] for row in frame.itertuples()]
    frame["line_index"] = np.maximum(np.maximum(frame["bar_index"], frame["family_bar_index"]), PASS_FLOOR)
    frame["line_random"] = np.maximum(np.maximum(frame["bar_random"], frame["family_bar_random"]), PASS_FLOOR)
    frame["c1_index"] = frame["index_t_min"] >= frame["line_index"]
    frame["c2_random"] = frame["random_t_min"] >= frame["line_random"]
    sign = frame.set_index(["board", "timing", "valuation", "variant"])["index_annual_pct"].apply(np.sign)
    grid_ok = []

    for row in frame.itertuples():
        mine = np.sign(row.index_annual_pct)
        others = [sign.get((row.board, row.timing, row.valuation, other)) for other in grid_neighbours(row.variant)]
        others = [value for value in others if value is not None]
        grid_ok.append(bool(others) and all(value == mine for value in others))

    frame["c3_grid_same_sign"] = grid_ok
    frame["c3_neighbours"] = frame["neighbours_same_sign"] & frame["c3_grid_same_sign"]
    frame["c4_years"] = frame["share_positive_years"] >= exit_test.JUDGE_POSITIVE_YEARS
    frame["slot_judgeable"] = frame["slot_trades"] >= round3.MIN_SLOT_TRADES
    frame["c5_slot"] = frame["slot_judgeable"] & (frame["slot_annual_pct"] > frame["kospi_hold_annual_pct"]) & (
        frame["slot_annual_pct"] > frame["kosdaq_hold_annual_pct"])
    frame["pass_1_5"] = frame["c1_index"] & frame["c2_random"] & frame["c3_neighbours"] & frame["c4_years"] & frame["c5_slot"]
    return frame


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    rules = exit_test.build_rules()
    data = exit_test.Data()
    panel = data.panel
    exit_test.MONTHS = panel.month_count
    engine = round2.Engine(data)
    flags = round2.flags_of(data, engine)
    features = entry_candidates.Features(data)
    pieces = Pieces(data)
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
    days = np.arange(first_day, last_day + 1)
    round4_daily = pd.read_parquet(ROUND4 / "daily_returns.parquet")
    round4_cells = pd.read_csv(ROUND4 / "cell_table.csv")
    close_view = CloseView(data, engine)
    print(f"준비 {time.time() - started:.0f}초", flush=True)
    records, cell_frames, year_frames, daily_frames, equity_columns, excursions, summaries, buckets = [], [], [], [], {}, [], [], []
    series, sizes, replication = {}, {}, {}
    trade_frames = []

    for board, extra in round3.BOARDS:
        context = entry_candidates.Context(data, engine, flags, features, extra)
        close_context = entry_candidates.Context(data, engine, flags, features, extra)
        close_context.pool = close_pool(data, flags, extra, features)

        for variant, (name, label, build) in enumerate(entry_candidates.VARIANTS):
            primary = entry_candidates.PRIMARY_CELL[name]
            timings = ("open",) if name == "C1" else ("open", "close")

            for timing in timings:
                run = f"{board}|{variant + 1:02d}|{timing}"

                if timing == "open":
                    entries, approach, view = build(context)
                    control_rows, no_pool = round3.random_entries(data, flags, extra, entries)
                    control_approach = np.zeros(len(control_rows), dtype=np.int64)
                    control_view = context.open_view
                    runner = round4.run_exits_next_day
                else:
                    signal_rows, _, _ = build(close_context)
                    entries = signal_rows - 1
                    approach = np.full(len(entries), 3, dtype=np.int64)
                    view = close_view
                    control_signal, no_pool = random_from_pool(data, close_context.pool, signal_rows)
                    control_rows = control_signal - 1
                    control_approach = np.full(len(control_rows), 3, dtype=np.int64)
                    control_view = close_view
                    runner = run_exits_close_entry

                strategy = round3.evaluate(data, view, rules, entries, approach, runner=runner)
                control = round3.evaluate(data, control_view, rules, control_rows, control_approach, runner=runner)
                strategy["index_id"] = market_of_stock[strategy["stock"]]
                control["index_id"] = market_of_stock[control["stock"]]
                strategy["entry_close"] = np.full(len(entries), timing == "close")
                control["entry_close"] = np.full(len(control_rows), timing == "close")
                sizes[run] = {"signals": int(len(entries)), "control_rows": int(len(control_rows)), "signals_without_pool": no_pool,
                              "primary_trades": int(strategy["keep"][:, primary].sum()),
                              "kosdaq_share": float(strategy["index_id"].mean()) if len(entries) else np.nan}
                label_row = {"run": run, "board": board, "variant": variant, "variant_no": variant + 1, "candidate": name,
                             "variant_label": label, "timing": timing, "primary_cell": primary,
                             "primary_rule": rules.at[primary, "rule"]}

                for valuation, multiplier in round3.VALUATIONS:
                    table, year_table, bars, equity, daily = round3.board_tables(data, rules, strategy, control, multiplier, index_open,
                                                                             index_close, groups, (signs_month, signs_year), first_day,
                                                                             last_day, years, kospi_curve)
                    table = round4.renamed(round4.with_kosdaq_hold(table, kosdaq_annual, kosdaq_mdd))
                    row = table.set_index("cell").loc[primary]
                    record = dict(label_row, valuation=valuation, bar_index=bars["kospi"]["main_bar"], bar_random=bars["random"]["main_bar"])

                    for column in ("trades", "net_mean", "net_median", "hold_median", "control_trades", "control_net_mean", "annual_pct",
                                   "index_same_weight_annual_pct", "control_annual_pct", "index_annual_pct", "index_t_month",
                                   "index_t_year", "index_t_min", "beta", "beta_annual_pct", "beta_t_min", "random_annual_pct",
                                   "random_t_min", "control_index_annual_pct", "control_index_t_min", "share_positive_years",
                                   "neighbours_same_sign", "slot_trades", "slot_annual_pct", "slot_mdd_pct", "control_slot_annual_pct",
                                   "kospi_hold_annual_pct", "kosdaq_hold_annual_pct", "unclosed", "delisted"):
                        record[column] = row[column]

                    portfolio = daily["portfolio"][primary]
                    series[(board, valuation, run)] = (portfolio - daily["kospi"][primary], portfolio - daily["control"][primary])

                    if valuation == "종가":
                        cell_frames.append(table.assign(**label_row))
                        year_part = year_table[year_table["cell"] == primary]
                        year_frames.append(round4.renamed(year_part).assign(**label_row))
                        daily_frames.append(pd.DataFrame({"run": run, "date": panel.market_dates[days],
                                                          "portfolio_pct": portfolio[days].astype(np.float32),
                                                          "index_same_weight_pct": daily["kospi"][primary, days].astype(np.float32),
                                                          "control_pct": daily["control"][primary, days].astype(np.float32)}))

                        if board == "U2":
                            before = round4_daily[round4_daily["cell"] == primary].set_index("date")["portfolio_pct"]
                            mine = pd.Series(portfolio, index=panel.market_dates)
                            difference = (mine - before.reindex(panel.market_dates).to_numpy()).to_numpy()
                            t_month, t_year, _ = round3.family_t(difference[None, :], month_of_day, year_of_day, month_count, year_count,
                                                             signs_month, signs_year, np.array([True]))
                            record["versus_round4_annual_pct"] = float(np.nanmean(difference) * round3.YEAR_DAYS)
                            record["versus_round4_days"] = int(np.isfinite(difference).sum())
                            record["versus_round4_t_min"] = float(np.fmin(t_month[0], t_year[0]))
                            equity_columns[run] = equity[primary]
                            net = round3.valued(strategy, 1.0)
                            excursions.append(excursion_row(data, strategy, primary, timing == "close", net, label_row))

                            for set_name, result, set_runner in (("신호", strategy, runner), ("무작위", control, runner)):
                                parts = decompose(data, engine, pieces, rules, result, primary, set_runner, round3.valued(result, 1.0))
                                summary, bucket = decomposition_rows(parts, dict(label_row, set=set_name))
                                summaries.append(summary)
                                buckets.extend(bucket)

                            keep = strategy["keep"][:, primary]
                            exit_rows = strategy["exit_row"][keep, primary]
                            trade_frames.append(pd.DataFrame({
                                "run": run, "code": np.array(panel.codes)[strategy["stock"][keep]],
                                "entry_date": panel.dates[strategy["entries"][keep]], "market": strategy["index_id"][keep].astype(np.int8),
                                "exit_date": panel.dates[exit_rows], "reason": strategy["reason"][keep, primary],
                                "hold_days": (panel.market_position[exit_rows] - strategy["day"][keep]).astype(np.int32),
                                "net_pct": strategy["net"][keep, primary].astype(np.float32)}))

                            if variant == 0:
                                columns = ["trades", "net_mean", "index_t_min", "random_t_min", "live_t_min", "slot_trades", "slot_annual_pct"]
                                mine_table = table.set_index("cell")[columns]
                                old = round4_cells.set_index("cell")[columns]
                                gap = float(np.nanmax(np.abs(mine_table.to_numpy(np.float64) - old.to_numpy(np.float64)) / np.maximum(1.0, np.abs(old.to_numpy(np.float64)))))     # 4회차 csv 는 유효숫자 10자리
                                replication = {"max_abs_difference": gap, "columns": columns}
                                print(f"라이브 재현 점검: 4회차 cell_table 과 최대 상대 차이 {gap:.3e}", flush=True)

                                if gap > 1e-8:
                                    raise SystemExit("C1 (+5·−1) U2 가 4회차와 다르다 — 나머지를 돌리지 않는다")

                    records.append(record)

                print(f"{run} {label}: 신호 {len(entries)} · 본 칸 {sizes[run]['primary_trades']} · "
                      f"지수 t {records[-3]['index_t_min']:.2f} · 무작위 t {records[-3]['random_t_min']:.2f} · "
                      f"슬롯 {records[-3]['slot_trades']}건 {records[-3]['slot_annual_pct']:.1f}% ({time.time() - started:.0f}초)", flush=True)
                del strategy, control
                gc.collect()

    family = {}

    for board, _ in round3.BOARDS:
        for valuation, _ in round3.VALUATIONS:
            keys = [key for key in series if key[0] == board and key[1] == valuation]
            judged = np.ones(len(keys), dtype=bool)
            family[(board, valuation)] = {}

            for slot, name in ((0, "index"), (1, "random")):
                values = np.vstack([series[key][slot] for key in keys])
                _, _, bars = round3.family_t(values, month_of_day, year_of_day, month_count, year_count, signs_month, signs_year, judged)
                family[(board, valuation)][name] = bars["main_bar"]
                family[(board, valuation)][f"{name}_bars"] = bars

    frame = judge(pd.DataFrame(records), family)
    main = frame[frame["valuation"] == "종가"].set_index("run")

    for short, valuation in (("m10", "종가−10%"), ("m20", "종가−20%")):
        other = frame[frame["valuation"] == valuation].set_index("run")
        main[f"index_t_min_{short}"] = other["index_t_min"]
        main[f"pass_1_5_{short}"] = other["pass_1_5"]

    main["c6_all_valuations"] = main["pass_1_5"] & main["pass_1_5_m10"] & main["pass_1_5_m20"]
    u0 = main[main["board"] == "U0"].reset_index().set_index(["variant", "timing"])["index_annual_pct"]
    main["u0_index_annual_pct"] = [u0.get((row.variant, row.timing), np.nan) for row in main.itertuples()]
    main["c7_u0_same_sign"] = np.sign(main["u0_index_annual_pct"]) == np.sign(main["index_annual_pct"])
    main["c8_versus_round4"] = main.get("versus_round4_annual_pct", pd.Series(np.nan, index=main.index)) > 0
    main["passes"] = main["c6_all_valuations"] & main["c7_u0_same_sign"] & main["c8_versus_round4"]
    main["status"] = np.where(~main["slot_judgeable"], "판정 불가", np.where(main["passes"], "통과", "불통과"))
    main.loc[main["board"] == "U0", "status"] = "참고(U0)"
    main = main.reset_index()
    common.write_csv(OUTPUT / "judge_table.csv", main)
    common.write_csv(OUTPUT / "judge_table_all_valuations.csv", frame)
    common.write_csv(OUTPUT / "cell_table_all.csv", pd.concat(cell_frames, ignore_index=True))
    common.write_csv(OUTPUT / "year_table_primary.csv", pd.concat(year_frames, ignore_index=True))
    common.write_csv(OUTPUT / "excursions_primary.csv", pd.DataFrame(excursions))
    common.write_csv(OUTPUT / "diagnosis_decomposition.csv", pd.DataFrame(summaries))
    common.write_csv(OUTPUT / "diagnosis_holding.csv", pd.DataFrame(buckets))
    pd.concat(daily_frames, ignore_index=True).to_parquet(OUTPUT / "daily_primary.parquet", index=False)
    curves = pd.DataFrame(equity_columns)
    curves.insert(0, "date", panel.market_dates[days])
    curves["kospi_hold"] = kospi_curve
    curves["kosdaq_hold"] = kosdaq_curve
    curves.to_parquet(OUTPUT / "slot_equity_primary.parquet", index=False)
    pd.concat(trade_frames, ignore_index=True).to_parquet(OUTPUT / "trades_primary.parquet", index=False)
    command = "py -I -X utf8 research/studies/39_devscale_exit_vs_trend/run_round5.py"
    common.write_text(OUTPUT / "run_command.txt", command + "\n")
    hashed = {name: exit_test.file_hash(OUTPUT / name) for name in ("judge_table.csv", "judge_table_all_valuations.csv", "cell_table_all.csv",
                                                             "year_table_primary.csv", "excursions_primary.csv",
                                                             "diagnosis_decomposition.csv", "diagnosis_holding.csv")}
    metrics = {"reproduction": panel.reproduction(), "command": command, "seconds": round(time.time() - started, 1), "years": years,
               "runs": sizes, "family_bars": {f"{board}·{valuation}": value for (board, valuation), value in family.items()},
               "pass_floor": PASS_FLOOR, "live_replication": replication, "flow_coverage": features.flow_coverage,
               "kosdaq_hold_annual_pct": kosdaq_annual, "kosdaq_hold_mdd_pct": kosdaq_mdd, "flips": round3_test.FLIPS,
               "seed": common.SEED, "random_seed": round3.RANDOM_SEED, "status_counts_u2": main[main["board"] == "U2"]["status"].value_counts().to_dict(),
               "output_sha256_16": hashed}
    common.write_json(OUTPUT / "metrics.json", metrics)
    pd.set_option("display.width", 250)
    print(main[main["board"] == "U2"][["run", "variant_label", "primary_rule", "trades", "net_mean", "index_annual_pct", "index_t_min",
                                       "line_index", "random_t_min", "line_random", "share_positive_years", "slot_trades",
                                       "slot_annual_pct", "versus_round4_annual_pct", "status"]].to_string(index=False), flush=True)
    print({key: {name: round(value[name], 3) for name in ("index", "random")} for key, value in family.items()}, flush=True)
    print(f"끝 {time.time() - started:.0f}초", flush=True)


if __name__ == "__main__":
    main()
