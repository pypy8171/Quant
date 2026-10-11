"""스터디 38 분봉 시험 — 이미 받아 둔 분봉만 쓴다(PYQuant/data/minute/<종목>/<YYYYMMDD>.parquet, 2025-09-01 이후). 새로 받지 않는다.

분봉 파일: 09:00–15:19 1분봉 + 15:30 종가 동시호가 봉 하나. 장중 봉은 15:19 까지만 쓰고, 15:30 은 일봉 종가로 본다.
가격 맞추기: 분봉은 무수정 가격이다. 그날 계수 = 중앙값(첫 분 시가/일봉 시가, 분 고가 최고/일봉 고가, 분 저가 최저/일봉 저가).
  예전의 "셋이 2% 넘게 어긋나면 뺀다"는 없앴다. 대신 셋의 어긋남(최대/최소 − 1)과 일봉 계수와의 차이 분포를 표로 낸다.

3a 15:19 판단 대 종가 판단: 분봉 있는 모든 종목일에서, 15:19 값(종가 = 15:19 분 종가, 고가·저가 = 15:19 까지 최고·최저,
   이평 = 전날까지 N−1 개 종가 + 15:19 가격, 확정 저점·고점·추세도 오늘 고가·저가를 15:19 값으로 바꿔 다시 계산)으로 낸 칸과
   일봉 종가로 낸 칸이 같은지. (k, N, ATR 기간)마다 센다. ATR 은 전날까지라 둘이 같다.
3b 장중 즉시 매수·장중 매도(1·5·10분봉, 5·10분봉은 1분봉을 09:00 기준으로 묶음). 되돌림 폭 r ∈ {0, 0.1, 0.25, 0.5} × ATR.
   매수 후보: 전날 종가까지의 추세가 상승(오늘 고가·저가는 장 마감 전 모르므로 — 내가 정함). 봉 b 의 이평 = (전날까지 N−1 개 종가 + 봉 종가)/N.
   그때까지 최저가의 거리 = (그때까지 최저 − 이평)/ATR 이 격자 위 끝(+1.0) 이하이고, 봉 종가 ≥ 이평 + r×ATR 인 첫 봉 → 다음 봉 시가 매수
   (마지막 봉이면 종가 동시호가 = 일봉 종가). 칸 = 그 봉의 거리 구간(반등 구간은 종가가 있어야 해서 장중에는 없다).
   매도(분봉 있는 날은 봉으로, 없는 날은 일봉 규칙으로):
     E1 봉 종가 < 마지막 확정 저점(전날까지 확정) → 다음 봉 시가(마지막 봉이면 종가). 분봉 없는 날은 종가 < 수준 → 종가.
     E2 봉 저가 < 수준 → min(봉 시가, 수준). 매수 봉 안에서 닿으면 순서를 모르므로 나쁜 쪽(그 봉에서 매도)으로 보고 센다.
     E3 추세 판정은 종가에서만 나므로 일봉 규칙 그대로(매수일 종가부터 본다).
     E4 j 앞 j−1 일 종가가 이평 아래(매수일 이후)이고 그날 봉 종가 < 봉 이평 → 다음 봉 시가. 분봉 없는 날은 일봉 규칙.
   매도 검사는 매수일(매수 봉부터)부터 한다. 끝까지 안 서면 2026-10-08 종가(미청산).
   같은 조합(k, N, ATR 기간, r, 봉 길이, 규칙, 거리 구간)에서 보유 중 신호는 무시, 청산 다음 행부터 다시 받는다.
   짝 비교: 같은 종목·같은 날 일봉 종가에 사서 일봉 규칙으로 판 순수익(daily_same_day_net)을 같이 낸다.
산출: results/minute_factor.csv, results/close_check.csv, results/close_check_price.csv, results/intraday_combinations.csv,
      results/intraday_summary.csv, results/intraday_metrics.json
재실행: py research/studies/38_trend_support_realtime/intraday_test.py
"""
from __future__ import annotations

import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import pandas as pd
import pyarrow.parquet as parquet
from numba import njit, prange

import common
from common import swing

RESULTS = common.STUDY / "results"
MINUTE_DIRECTORY = common.REPOSITORY / "PYQuant" / "data" / "minute"
WINDOW_FROM = 20250901
RECLAIM_GRID = (0.0, 0.1, 0.25, 0.5)
BAR_MINUTES = (1, 5, 10)
LAST_MINUTE = 6 * 60 + 19          # 15:19 = 09:00 기준 379분


def read_day(path) -> tuple[np.ndarray, np.ndarray] | None:
    table = parquet.read_table(path, columns=["hms", "open", "high", "low", "close"]).to_pandas()

    if table.empty:
        return None

    time_of_day = table["hms"].astype(str).str.zfill(6)
    minute = ((time_of_day.str[:2].astype(int) - 9) * 60 + time_of_day.str[2:4].astype(int)).to_numpy()
    order = np.argsort(minute, kind="stable")
    values = table[["open", "high", "low", "close"]].to_numpy(np.float64)[order]
    return minute[order], values


def load_minutes(panel: common.Panel) -> dict:
    """분봉이 있는 종목일 → (행, 분, 값). 일봉에 없는 날(거래량 0 등)은 센다."""
    code_index = {code: stock for stock, code in enumerate(panel.codes)}
    jobs = []

    for folder in sorted(MINUTE_DIRECTORY.iterdir()):
        stock = code_index.get(folder.name)

        if stock is None or not folder.is_dir():
            continue

        first, last = panel.starts[stock], panel.starts[stock + 1]
        dates = panel.dates[first:last]

        for path in folder.glob("*.parquet"):
            date = int(path.stem)

            if date < WINDOW_FROM or date > common.LAST_DATE:
                continue

            position = int(np.searchsorted(dates, date))

            if position < len(dates) and dates[position] == date:
                jobs.append((first + position, path))
            else:
                jobs.append((-1, path))

    with ThreadPoolExecutor(max_workers=12) as pool:
        loaded = list(pool.map(lambda job: (job[0], read_day(job[1])) if job[0] >= 0 else (-1, None), jobs))

    days = {row: data for row, data in loaded if row >= 0 and data is not None}
    return {"days": days, "files": len(jobs), "not_in_daily": sum(1 for row, _ in loaded if row < 0),
            "empty": sum(1 for row, data in loaded if row >= 0 and data is None),
            "folders_not_common": sum(1 for folder in MINUTE_DIRECTORY.iterdir() if folder.is_dir() and folder.name not in code_index)}


def factor_table(panel: common.Panel, days: dict) -> tuple[pd.DataFrame, dict]:
    rows = []
    factors = {}

    for row, (minute, values) in days.items():
        ratios = np.array([values[0, 0] / panel.open_price[row], values[:, 1].max() / panel.high[row], values[:, 2].min() / panel.low[row]])
        factor = float(np.median(ratios))
        factors[row] = factor
        rows.append({"row": row, "spread_percent": (ratios.max() / ratios.min() - 1.0) * 100.0,
                     "versus_daily_factor_percent": (factor / panel.factor[row] - 1.0) * 100.0,
                     "close_ratio_percent": (values[-1, 3] / factor / panel.close[row] - 1.0) * 100.0 if minute[-1] > LAST_MINUTE else np.nan,
                     "has_1519": bool((minute == LAST_MINUTE).any()), "has_auction": bool((minute > LAST_MINUTE).any())})

    return pd.DataFrame(rows), factors


def distribution(values: np.ndarray, name: str) -> dict:
    values = values[np.isfinite(values)]
    absolute = np.abs(values)
    result = {"measure": name, "n": len(values)}

    for label, quantile in (("p01", 1), ("p10", 10), ("median", 50), ("p90", 90), ("p99", 99)):
        result[label] = float(np.percentile(values, quantile)) if len(values) else None

    result.update({"max_absolute": float(absolute.max()) if len(values) else None, "n_exact_zero": int((values == 0).sum()),
                   "n_over_2_percent": int((absolute > 2.0).sum())})
    return result


def build_bars(panel: common.Panel, days: dict, factors: dict, size: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """장중(09:00–15:19) 봉을 수정주가로 바꿔 이어 붙인다. 반환: 봉 값(봉, 4), 행마다 첫 봉 번호·봉 수(없으면 −1·0)."""
    start = np.full(panel.row_count, -1, dtype=np.int64)
    count = np.zeros(panel.row_count, dtype=np.int64)
    pieces = []
    used = 0

    for row in sorted(days):
        minute, values = days[row]
        keep = minute <= LAST_MINUTE
        minute, values = minute[keep], values[keep] / factors[row]

        if len(minute) == 0:
            continue

        if size > 1:
            bucket = minute // size
            change = np.r_[True, bucket[1:] != bucket[:-1]]
            begins = np.flatnonzero(change)
            ends = np.r_[begins[1:], len(minute)]
            values = np.column_stack([values[begins, 0], np.maximum.reduceat(values[:, 1], begins),
                                      np.minimum.reduceat(values[:, 2], begins), values[ends - 1, 3]])

        start[row] = used
        count[row] = len(values)
        used += len(values)
        pieces.append(values)

    return np.vstack(pieces), start, count


# ── 3a ─────────────────────────────────────────────────────────────────────

def close_check(panel: common.Panel, days: dict, factors: dict) -> tuple[pd.DataFrame, pd.DataFrame]:
    """종목일마다 15:19 값과 종가 값으로 낸 신호 칸 비교."""
    targets = sorted(row for row, (minute, _) in days.items() if (minute <= LAST_MINUTE).any())
    snapshot = {}

    for row in targets:
        minute, values = days[row]
        keep = minute <= LAST_MINUTE
        part = values[keep] / factors[row]
        snapshot[row] = (part[-1, 3], part[:, 1].max(), part[:, 2].min(), int(minute[keep][-1]))

    rows_array = np.array(targets, dtype=np.int64)
    price = np.array([snapshot[row][0] for row in targets])
    high = np.array([snapshot[row][1] for row in targets])
    low = np.array([snapshot[row][2] for row in targets])
    last_minute = np.array([snapshot[row][3] for row in targets])
    price_frame = pd.DataFrame({"row": rows_array, "code": np.array(panel.codes)[panel.stock_of_row[rows_array]],
                                "date": panel.dates[rows_array], "price_1519": price, "close": panel.close[rows_array],
                                "close_versus_1519_percent": (panel.close[rows_array] / price - 1.0) * 100.0,
                                "last_bar_minute": last_minute})
    records = []
    stocks = panel.stock_of_row[rows_array]

    for span in common.K_VALUES:
        state_close, _, _ = panel.trend(span)
        state_minute = np.empty(len(targets), dtype=np.int8)

        for index, row in enumerate(targets):
            stock = stocks[index]
            first = panel.starts[stock]
            low_part = panel.low[first:row + 1].copy()
            high_part = panel.high[first:row + 1].copy()
            low_part[-1] = low[index]
            high_part[-1] = high[index]
            pivots = swing.confirmed_pivots(low_part, high_part, span)
            state_minute[index] = swing.trend_state(pivots, len(low_part))["state"][-1]

        for window_index, window in enumerate(common.WINDOWS):
            previous_sum = panel.averages[window_index][rows_array] * window - panel.close[rows_array]
            average_minute = (previous_sum + price) / window
            average_close = panel.averages[window_index][rows_array]

            for period_index, period in enumerate(common.ATR_PERIODS):
                atr = panel.atrs[period_index][rows_array]
                cell_close = cells(panel.low[rows_array], panel.high[rows_array], panel.close[rows_array], average_close, atr)
                cell_minute = cells(low, high, price, average_minute, atr)
                signal_close = (state_close[rows_array] == 1) & (cell_close >= 0)
                signal_minute = (state_minute == 1) & (cell_minute >= 0)
                both = signal_close & signal_minute
                records.append({"k": span, "window": window, "atr_period": period, "stock_days": len(targets),
                                "n_close_signal": int(signal_close.sum()), "n_1519_signal": int(signal_minute.sum()),
                                "n_same_cell": int((both & (cell_close == cell_minute)).sum()),
                                "n_both_other_cell": int((both & (cell_close != cell_minute)).sum()),
                                "n_close_only": int((signal_close & ~signal_minute).sum()),
                                "n_1519_only": int((signal_minute & ~signal_close).sum()),
                                "n_trend_differs": int((state_close[rows_array] != state_minute).sum())})

    frame = pd.DataFrame(records)
    union = frame["n_close_signal"] + frame["n_1519_only"]
    frame["agreement_percent"] = frame["n_same_cell"] / union * 100.0
    frame["agreement_of_close_percent"] = frame["n_same_cell"] / frame["n_close_signal"] * 100.0
    return frame, price_frame


def cells(low, high, close, average, atr) -> np.ndarray:
    with np.errstate(invalid="ignore", divide="ignore"):
        distance = (low - average) / atr
        bounce = (close - low) / (high - low)

    distance_index = np.searchsorted(common.DISTANCE_EDGES, distance, side="right")
    bounce_index = np.clip(np.searchsorted(common.BOUNCE_EDGES, bounce, side="right") - 1, 0, common.BOUNCE_BINS - 1)
    valid = np.isfinite(distance) & (atr > 0) & (high > low)
    return np.where(valid, distance_index * common.BOUNCE_BINS + bounce_index, -1)


# ── 3b ─────────────────────────────────────────────────────────────────────

@njit(cache=True)
def run_intraday(state_previous, state, latest_low, average_values, atr, close, open_price, low, starts, stock_of_row, bars,
                 bar_start, bar_count, candidates, reclaim, rule, window, distance_edges, cost, listed_at_end, results):
    """한 (k, N, ATR 기간, r, 봉 길이, 규칙)의 장중 매매.
    results[매매] = (행, 칸, 매수가, 매도 행, 매도가, 순수익, 비움, 애매한 봉, 분봉 없는 날 수, 사유)."""
    bins = distance_edges.shape[0] + 1
    next_allowed = np.zeros(bins, dtype=np.int64)
    current_stock = -1
    used = 0
    days = rule - 2 if rule >= 3 else 0
    top = distance_edges[distance_edges.shape[0] - 1]

    for index in range(candidates.shape[0]):
        row = candidates[index]
        stock = stock_of_row[row]

        if stock != current_stock:
            current_stock = stock
            next_allowed[:] = 0

        if state_previous[row] != 1 or not (atr[row] > 0) or not np.isfinite(average_values[row]):
            continue

        previous_sum = average_values[row] * window - close[row]
        first_bar = bar_start[row]
        bar_total = bar_count[row]
        running = np.inf
        signal = -1
        distance = np.nan

        for bar in range(bar_total):
            values = bars[first_bar + bar]

            if values[2] < running:
                running = values[2]

            average_bar = (previous_sum + values[3]) / window
            distance_bar = (running - average_bar) / atr[row]

            if distance_bar <= top and values[3] >= average_bar + reclaim * atr[row]:
                signal = bar
                distance = distance_bar
                break

        if signal < 0:
            continue

        cell = 0

        while cell < distance_edges.shape[0] and distance_edges[cell] <= distance:
            cell += 1

        if row < next_allowed[cell]:
            continue

        entry_bar = signal + 1
        same_day = entry_bar < bar_total
        entry = bars[first_bar + entry_bar, 0] if same_day else close[row]
        first = starts[stock]
        last = starts[stock + 1]
        exit_row = -1
        exit_price = np.nan
        ambiguous = 0
        no_minute = 0
        reason = 1
        run_since = 0

        for day in range(row, last):
            if day == row and not same_day:
                continue

            level = latest_low[day - 1] if day > first else np.nan
            below_close = np.isfinite(average_values[day]) and close[day] < average_values[day]

            if rule == 2:
                if state[day] == -1:
                    exit_row = day
                    exit_price = close[day]
                    break
            elif bar_start[day] >= 0:
                begin = entry_bar if day == row else 0
                previous_close_sum = average_values[day] * window - close[day]

                for bar in range(begin, bar_count[day]):
                    values = bars[bar_start[day] + bar]
                    found = False

                    if rule == 0:
                        found = np.isfinite(level) and values[3] < level
                    elif rule == 1:
                        if np.isfinite(level) and values[2] < level:
                            exit_row = day
                            exit_price = values[0] if values[0] < level else level

                            if day == row and bar == entry_bar:
                                ambiguous = 1
                                exit_price = entry if entry < level else level

                            break
                    elif run_since >= days - 1:
                        found = values[3] < (previous_close_sum + values[3]) / window

                    if found:
                        exit_row = day
                        exit_price = bars[bar_start[day] + bar + 1, 0] if bar + 1 < bar_count[day] else close[day]
                        break

                if exit_row >= 0:
                    break

                # 장중 봉으로는 안 섰어도 종가 동시호가에서 서면 종가에 판다
                if rule <= 1 and np.isfinite(level) and close[day] < level:
                    exit_row = day
                    exit_price = close[day]
                    break

                if rule >= 3 and run_since >= days - 1 and below_close:
                    exit_row = day
                    exit_price = close[day]
                    break
            else:
                no_minute += 1

                if rule == 0 and np.isfinite(level) and close[day] < level:
                    exit_row = day
                    exit_price = close[day]
                    break

                if rule == 1 and np.isfinite(level) and low[day] < level:
                    exit_row = day
                    exit_price = open_price[day] if open_price[day] < level else level
                    break

                if rule >= 3 and run_since >= days - 1 and below_close:
                    exit_row = day
                    exit_price = close[day]
                    break

            run_since = run_since + 1 if below_close else 0

        if exit_row < 0:
            exit_row = last - 1
            exit_price = close[exit_row]
            reason = 2 if listed_at_end[stock] else 3
            next_allowed[cell] = last + 1
        else:
            next_allowed[cell] = exit_row + 1

        results[used, 0] = row
        results[used, 1] = cell
        results[used, 2] = entry
        results[used, 3] = exit_row
        results[used, 4] = exit_price
        results[used, 5] = (exit_price / entry * cost - 1.0) * 100.0
        results[used, 7] = ambiguous
        results[used, 8] = no_minute
        results[used, 9] = reason
        used += 1

    return used


@njit(cache=True)
def daily_same_day(rows, rule, close, open_price, low, state, latest_low, average_values, starts, stock_of_row, cost):
    """같은 종목·같은 날 일봉 종가 매수 → 일봉 규칙 매도 순수익(짝 비교)."""
    result = np.empty(rows.shape[0])
    days = rule - 2 if rule >= 3 else 1

    for index in range(rows.shape[0]):
        row = rows[index]
        stock = stock_of_row[row]
        last = starts[stock + 1]
        price = np.nan
        run = 0

        for day in range(row + 1, last):
            level = latest_low[day - 1]

            if rule == 0 and np.isfinite(level) and close[day] < level:
                price = close[day]
            elif rule == 1 and np.isfinite(level) and low[day] < level:
                price = open_price[day] if open_price[day] < level else level
            elif rule == 2 and state[day] == -1:
                price = close[day]
            elif rule >= 3:
                run = run + 1 if (np.isfinite(average_values[day]) and close[day] < average_values[day]) else 0

                if run >= days:
                    price = close[day]

            if np.isfinite(price):
                break

        if not np.isfinite(price):
            price = close[last - 1]

        result[index] = (price / close[row] * cost - 1.0) * 100.0

    return result


def main() -> None:
    started = time.time()
    RESULTS.mkdir(exist_ok=True)
    panel = common.Panel()
    loaded = load_minutes(panel)
    days = loaded["days"]
    factor_frame, factors = factor_table(panel, days)
    factor_summary = pd.DataFrame([distribution(factor_frame["spread_percent"].to_numpy(), "세 비율 어긋남(최대/최소−1, %)"),
                                   distribution(factor_frame["versus_daily_factor_percent"].to_numpy(), "분봉 계수 대 일봉 계수(%)"),
                                   distribution(factor_frame["close_ratio_percent"].to_numpy(), "15:30 봉 ÷ 계수 대 일봉 종가(%)")])
    common.write_csv(RESULTS / "minute_factor.csv", factor_summary)
    factor_frame.to_parquet(RESULTS / "minute_factor_days.parquet", index=False)
    print(f"분봉 {len(days):,} 종목일 읽음 {time.time() - started:.0f}초", flush=True)
    check, price_frame = close_check(panel, days, factors)
    common.write_csv(RESULTS / "close_check.csv", check)
    price_summary = pd.DataFrame([distribution(price_frame["close_versus_1519_percent"].to_numpy(), "종가 대 15:19 가격(%)")])
    common.write_csv(RESULTS / "close_check_price.csv", price_summary)
    print(f"3a 끝 {time.time() - started:.0f}초", flush=True)
    candidates = np.array(sorted(days), dtype=np.int64)
    bar_sets = {size: build_bars(panel, days, factors, size) for size in BAR_MINUTES}
    records = []

    for span in common.K_VALUES:
        state, latest_low, _ = panel.trend(span)
        state_previous = np.r_[np.int8(-9), state[:-1]]
        state_previous[panel.starts[:-1]] = -9

        for window_index, window in enumerate(common.WINDOWS):
            average_values = panel.averages[window_index]

            for rule_index, rule in enumerate(common.RULE_NAMES):
                for period_index, period in enumerate(common.ATR_PERIODS):
                    for size, (bars, bar_start, bar_count) in bar_sets.items():
                        for reclaim in RECLAIM_GRID:
                            results = np.full((len(candidates), 10), np.nan)
                            used = run_intraday(state_previous, state, latest_low, average_values, panel.atrs[period_index],
                                                panel.close, panel.open_price, panel.low, panel.starts, panel.stock_of_row, bars,
                                                bar_start, bar_count, candidates, reclaim, rule_index, window, common.DISTANCE_EDGES,
                                                common.COST, panel.listed_at_end, results)
                            part = results[:used]
                            paired = daily_same_day(part[:, 0].astype(np.int64), rule_index, panel.close, panel.open_price, panel.low,
                                                    state, latest_low, average_values, panel.starts, panel.stock_of_row, common.COST)

                            for cell in np.unique(part[:, 1]).astype(int):
                                chosen = part[:, 1] == cell
                                net = part[chosen, 5]
                                pair = paired[chosen]
                                records.append({"k": span, "window": window, "atr_period": period, "bar_minutes": size,
                                                "reclaim_atr": reclaim, "rule": rule, "distance_bin": cell, "n": int(chosen.sum()),
                                                "sum_net": float(net.sum()), "wins": int((net > 0).sum()),
                                                "sum_daily_same_day_net": float(pair.sum()),
                                                "n_ambiguous_bar": int(part[chosen, 7].sum()),
                                                "n_days_without_minute": int(part[chosen, 8].sum()),
                                                "n_open": int((part[chosen, 9] == 2).sum()), "n_data_end": int((part[chosen, 9] == 3).sum()),
                                                "sum_hold": float((part[chosen, 3] - part[chosen, 0]).sum())})

        print(f"3b k={span} 끝 {time.time() - started:.0f}초", flush=True)

    combos = pd.DataFrame(records)

    with np.errstate(invalid="ignore", divide="ignore"):
        combos["mean_net"] = combos["sum_net"] / combos["n"]
        combos["win_rate"] = combos["wins"] / combos["n"] * 100.0
        combos["mean_daily_same_day_net"] = combos["sum_daily_same_day_net"] / combos["n"]
        combos["mean_difference"] = combos["mean_net"] - combos["mean_daily_same_day_net"]
        combos["mean_hold"] = combos["sum_hold"] / combos["n"]

    common.write_csv(RESULTS / "intraday_combinations.csv", combos)
    summary = combos.groupby(["bar_minutes", "reclaim_atr", "rule"], as_index=False)[
        ["n", "sum_net", "wins", "sum_daily_same_day_net", "n_ambiguous_bar", "n_days_without_minute", "n_open", "sum_hold"]].sum()
    summary["mean_net"] = summary["sum_net"] / summary["n"]
    summary["win_rate"] = summary["wins"] / summary["n"] * 100.0
    summary["mean_daily_same_day_net"] = summary["sum_daily_same_day_net"] / summary["n"]
    summary["mean_difference"] = summary["mean_net"] - summary["mean_daily_same_day_net"]
    summary["mean_hold"] = summary["sum_hold"] / summary["n"]
    common.write_csv(RESULTS / "intraday_summary.csv", summary)
    common.write_json(RESULTS / "intraday_metrics.json", {
        "minute_files": loaded["files"], "minute_days_used": len(days), "files_not_in_daily": loaded["not_in_daily"],
        "files_empty": loaded["empty"], "folders_not_common_stock": loaded["folders_not_common"],
        "stock_days_with_1519_bar": int(factor_frame["has_1519"].sum()), "stock_days_with_auction_bar": int(factor_frame["has_auction"].sum()),
        "seconds": time.time() - started, "reproduction": panel.reproduction()})
    print(f"끝 {time.time() - started:.0f}초", flush=True)


if __name__ == "__main__":
    main()
