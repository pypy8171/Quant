"""스터디 38 8회차 — 판정 값을 '같은 날·같은 보유 기간·같은 절대 구간 전 종목 균등 매수' 대조를 뺀 초과수익으로 바꾸고,
1분위로 좁혀 온 경로를 풀어 전 칸을 다시 본다.

7회차까지 판정 값은 시장 전체(보통주 균등) 대비 초과수익이었다. 7회차에서 1분위 효과가 같은 달·같은 절대 구간 대조를 빼면
사라졌으므로(t 0.49 / −0.29), 8회차는 그 대조를 뺀 값을 판정 값으로 쓴다(지시 2026-10-11).

대조(내가 정함 — 지시 '같은 달·같은 절대 구간'을 더 좁힌 판):
  매매마다, 매수일(시장일 d)에 같은 절대 구간(전날까지 20일 평균 거래대금, 하한 = 주문 금액 ÷ 0.5% 를 같이 적용)에 든 보통주
  전부를 d 종가에 똑같이 사서 그 매매의 매도일 e 종가에 판 순수익 평균. 거래 없는 날·상장폐지 뒤는 마지막 종가(시장 비교 평균 B 와
  같은 처리). 왕복 비용 COST 는 대조에도 B 와 같이 곱하고, 충격 비용은 신호에만 붙인다(보수적).
  → 판정 값 = 신호 순수익 − 대조 순수익 = (신호 총수익 × 충격 배수 × COST − 대조 총수익 × COST) × 100.
  같은 달이 아니라 같은 날·같은 보유 기간이라 보유 기간 차이와 달 안 시기 차이까지 지운다. 신호 종목 자신도 대조에 들어 있다.

시작 전에 고정한 칸(지시):
  주문 금액 {100만, 500만, 2,000만, 1억} × 규칙 13(E1, E2, E3, E4j1–E4j10) × 절대 구간 7 × 그날 순위 10분위 = 3,640 칸.
  상한 0.5%(하한 = 금액 ÷ 0.5% = 2억·10억·40억·200억)는 7회차와 같게 고정(내가 정함). 그래서 금액이 클수록 아래 구간 칸은
  구조상 빈다. 기준선은 3,640 칸 중 매매 3 건·거래 든 달 2 개 이상인 칸 수(M)로 다시 구한다.
  칸 안 보유 중 무시 = 칸(금액·규칙·절대 구간·순위) 단위. 판 = Q1(같은 종목·같은 매수일 1 건, 이평 N 7 × ATR 5 = 35 묶음 평균).
  판정(5–7회차와 같은 방식): (가) 거래 든 달 24 개월 이상 칸 최대 t 95% 분위, (나) 자유도 보정 — 둘 다, 옆 순위 분위 부호 같음
  (같은 금액·규칙·절대 구간), 대조 뺀 값 양수 연도 ≥ 0.7, 대조 뺀 평균 > 0. 뒤집기 1,000 회, seed 20261010.
  참고 열: 대조 안 뺀 순 초과수익(시장 B 대비) n·평균·t, 순 초과수익 > 0 까지 붙인 통과.

시기 노출(지시): 확장창 국면(7회차 X_exp)별로
  (가) 2–5억 전 종목 균등 매수, 매 시장일 종가에 사서 20 시장일 뒤 종가에 판다(20 일은 내가 정함 — E3·E4j5 1분위 보유 중앙값 11–17일 근처),
  (나) 신호 100만 원 E3·E4j5 2–5억 전 분위 Q1 — 순 초과수익, 대조, 대조 뺀 값.

청산 격자(지시 격자): 6회차 남는 칸 매수 집합(100만 원 E3·E4j5 1분위) 고정, 매도 E3 확인 1–3, E4j 1–10 → 대조 뺀 값.
  26 검정 통과선 t 3.0(헌장 20 칸 기준 — 내가 정함). 트레일링 10/15/20% 는 비교만.

원인 진단(통과 0 칸이어도 멈추지 않도록 늘 낸다): 100만 원 E3·E4j5 Q1 매매의 보유 일수 구간별 대조 뺀 값, 신호 종목이 같은 날·같은
  절대 구간 안에서 차지하는 백분위(지난 20일·5일 수익률, ATR20/종가, 종가/60일 이평).

산출(results/round8/): cell_table.csv, judgement.csv, multiple_comparison.json, timing_exposure.csv, exit_grid.csv, diagnosis_hold.csv,
  diagnosis_features.csv, choices_log.csv(1–8회차), survivors_trades.csv(6회차와 같은 열), round8_metrics.json
재실행: py research/studies/38_trend_support_realtime/round8_test.py
"""
from __future__ import annotations

import time

import common     # numba 보다 먼저: common 이 NUMBA_NUM_THREADS(12)를 넣는다
import numpy as np
import pandas as pd
from numba import njit, prange

import round3_test
import round4_test
import round5_test
import round6_test
import round7_test

OUTPUT = common.STUDY / "results" / "round8"
AMOUNTS = (1_000_000.0, 5_000_000.0, 20_000_000.0, 100_000_000.0)   # 지시
CAP_PERCENT = 0.5                                                   # 내가 정함(7회차와 같게)
COEFFICIENT = 0.3
RULE_NAMES = ("E1", "E2", "E3") + tuple(f"E4j{days}" for days in range(1, 11))   # 지시
ABSOLUTE_COUNT = 7
DECILE_COUNT = 10
SLOTS = ABSOLUTE_COUNT * DECILE_COUNT
BIN_LOWS = np.array([0.0, 1e8, 2e8, 5e8, 1e9, 3e9, 1e10])
BIN_HIGHS = np.array([1e8, 2e8, 5e8, 1e9, 3e9, 1e10, np.inf])
PASSIVE_DAYS = 20               # 내가 정함
GRID_BAR = 3.0                  # 내가 정함(헌장 20 칸 기준)
HOLD_EDGES = (1, 6, 21, 61)     # 보유 일수 구간 1–5, 6–20, 21–60, 61+ (내가 정함)
GRID_RULES = (2, 7)             # E3, E4j5 — 6회차 남는 칸


@njit(cache=True)
def emit_window(rule, window, slot, minimum, averages, atrs, open_price, low, close, starts, signal_ok, base_turnover, chosen, states,
                latest_lows, distance_edges, capacity, out_stock, out_row, out_exit_row, out_reason, out_period, out_span, out_price):
    """7회차 emit_slots 를 이평 N 하나로 좁힌 것(메모리). 보유 중 무시는 (N, ATR 기간, 슬롯)마다. capacity 0 이면 수만 센다."""
    spans = states.shape[0]
    inner_last = distance_edges.shape[0] - 1
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit_span = np.empty((spans, longest + 1), dtype=np.int64)
    next_exit_window = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    allowed = np.zeros(SLOTS, dtype=np.int64)
    average = averages[window]
    written = 0

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first

        if rule <= 2:
            for span in range(spans):
                common.fill_next_exit(rule, first, length, averages[0], close, low, states[span], latest_lows[span], run_length,
                                      next_exit_span[span])
        else:
            common.fill_next_exit(rule, first, length, average, close, low, states[0], latest_lows[0], run_length, next_exit_window)

        for period in range(atrs.shape[0]):
            atr = atrs[period]
            allowed[:] = 0

            for offset in range(length):
                row = first + offset
                span = chosen[row]
                row_slot = slot[row]

                if row_slot < 0 or not signal_ok[row] or span < 0 or offset < allowed[row_slot]:
                    continue

                if not (np.isfinite(average[row]) and np.isfinite(atr[row]) and atr[row] > 0):
                    continue

                distance_index = common.distance_bin((low[row] - average[row]) / atr[row], distance_edges)

                if distance_index < 1 or distance_index > inner_last or not (base_turnover[row] >= minimum):
                    continue

                exit_offset, exit_price = round6_test.ensemble_exit(rule, offset, length, first, span, next_exit_span,
                                                                    next_exit_window, open_price, close, latest_lows)
                reason = common.REASON_RULE

                if exit_offset < length:
                    exit_row = first + exit_offset
                    allowed[row_slot] = exit_offset + 1
                else:
                    exit_row = last - 1
                    exit_price = close[exit_row]
                    reason = 0
                    allowed[row_slot] = length + 1

                if written < capacity:
                    out_stock[written] = stock
                    out_row[written] = row
                    out_exit_row[written] = exit_row
                    out_reason[written] = reason
                    out_period[written] = period
                    out_span[written] = span
                    out_price[written] = exit_price

                written += 1

    return written


@njit(cache=True, parallel=True)
def pool_returns(days_in, days_out, lows, highs, close_matrix, turnover_matrix):
    """d 날 전날까지 20일 평균 거래대금이 [low, high) 인 보통주 전부를 d 종가에 사서 e 종가에 판 총수익 평균(배). 종목 수도 돌려준다."""
    count = days_in.shape[0]
    stocks = close_matrix.shape[1]
    result = np.full(count, np.nan)
    members = np.zeros(count, dtype=np.int64)

    for index in prange(count):
        day = days_in[index]
        exit_day = days_out[index]
        total = 0.0
        number = 0

        for stock in range(stocks):
            turnover = turnover_matrix[day, stock]

            if turnover >= lows[index] and turnover < highs[index]:
                buy = close_matrix[day, stock]
                sell = close_matrix[exit_day, stock]

                if buy > 0 and sell > 0:
                    total += sell / buy
                    number += 1

        if number > 0:
            result[index] = total / number

        members[index] = number

    return result, members


class Context:
    """한 번 만들어 두고 계속 쓰는 값."""

    def __init__(self):
        panel = common.Panel()
        round3_test.apply_corrections(panel)
        panel.build_benchmark()
        previous_20d = round4_test.shift_within_stock(panel.turnover20, panel.starts)
        panel.turnover20 = previous_20d
        deciles = panel.build_deciles()
        self.panel = panel
        self.previous_20d = previous_20d
        self.decile = deciles[:, 1].astype(np.int64)
        self.absolute = np.where(np.isfinite(previous_20d),
                                 np.searchsorted(round6_test.ABSOLUTE_EDGES, np.nan_to_num(previous_20d), side="right"), -1).astype(np.int64)
        self.slot = np.where((self.absolute >= 0) & (self.decile >= 0), self.absolute * DECILE_COUNT + self.decile, -1).astype(np.int64)
        self.span_values = list(common.K_VALUES)
        states, latest_lows, pivots = [], [], []

        for span in self.span_values:
            state, latest_low, pivots_by_stock = panel.trend(span)
            states.append(state)
            latest_lows.append(latest_low)
            pivots.append(pivots_by_stock)

        latest_lows = np.stack(latest_lows)
        self.states = round6_test.fixed_states(np.stack(states), latest_lows, panel.close)
        self.latest_lows = latest_lows
        self.pivots = pivots
        self.chosen, self.mask = round6_test.choose_support(self.states, latest_lows)
        # 시장일 × 종목 가격·거래대금 표(대조용). 가격은 상장 뒤 마지막 값 이어 쓰기.
        stock_count = len(panel.starts) - 1
        self.close_matrix = np.full((panel.market_count, stock_count), np.nan)
        self.turnover_matrix = np.full((panel.market_count, stock_count), np.nan)
        stock_of_row = np.repeat(np.arange(stock_count), np.diff(panel.starts))
        self.close_matrix[panel.market_position, stock_of_row] = panel.close
        self.turnover_matrix[panel.market_position, stock_of_row] = previous_20d
        self.close_matrix = pd.DataFrame(self.close_matrix).ffill().to_numpy()
        self.stock_of_row = stock_of_row

    def pool(self, rows: np.ndarray, exit_rows: np.ndarray, minimum: float, bins: np.ndarray | None = None):
        """매매(행, 매도 행)마다 같은 날·같은 절대 구간 대조의 순 초과수익(%, 시장 B 대비)과 대조 종목 수."""
        panel = self.panel
        days_in = panel.market_position[rows].astype(np.int64)
        days_out = panel.market_position[exit_rows].astype(np.int64)
        bins = self.absolute[rows] if bins is None else bins
        lows = np.maximum(BIN_LOWS[bins], minimum)
        highs = BIN_HIGHS[bins]
        key = pd.DataFrame({"day": days_in, "exit": days_out, "low": lows, "high": highs})
        unique = key.drop_duplicates().reset_index(drop=True)
        gross, members = pool_returns(unique["day"].to_numpy(), unique["exit"].to_numpy(), unique["low"].to_numpy(),
                                      unique["high"].to_numpy(), self.close_matrix, self.turnover_matrix)
        unique["gross"] = gross
        unique["members"] = members
        joined = key.merge(unique, on=["day", "exit", "low", "high"], how="left")
        start = panel.signal_start
        market = panel.bench[days_in - start, days_out - start]
        excess = (joined["gross"].to_numpy() * common.COST - 1.0) * 100.0 - market
        return excess, joined["members"].to_numpy()


def net_excess(panel, previous_20d, amount, rows, exit_rows, exit_prices) -> np.ndarray:
    """6·7회차와 같은 식(계수 0.3), 금액만 받는다."""
    buy_ratio = amount / previous_20d[rows]
    sell_base = previous_20d[exit_rows]
    sell_ratio = np.where(np.isfinite(sell_base) & (sell_base > 0), amount / np.where(sell_base > 0, sell_base, 1.0), buy_ratio)
    multiplier = np.maximum(0.0, 1.0 - COEFFICIENT * sell_ratio) / (1.0 + COEFFICIENT * buy_ratio)
    gross = exit_prices / panel.close[rows]
    market = panel.bench[panel.market_position[rows] - panel.signal_start, panel.market_position[exit_rows] - panel.signal_start]
    return (gross * multiplier * common.COST - 1.0) * 100.0 - market


def emit_all(context: Context, rule: int, minimum: float, keep_slot=None, slot=None):
    """이평 N 7 개를 차례로 뽑아 (행, 매도 행)으로 줄인 표와, keep_slot 조건에 맞는 묶음 원본을 돌려준다."""
    panel = context.panel
    reduced, kept = [], []
    slot = context.slot if slot is None else slot

    for window in range(len(common.WINDOWS)):
        arguments = (rule, window, slot, minimum, panel.averages, panel.atrs, panel.open_price, panel.low, panel.close,
                     panel.starts, panel.signal_ok, context.previous_20d, context.chosen, context.states, context.latest_lows,
                     common.DISTANCE_EDGES)
        empty = np.zeros(0, np.int64)
        count = emit_window(*arguments, 0, empty, empty, empty, empty, empty, empty, np.zeros(0))
        columns = {name: np.empty(count, np.int64) for name in ("stock", "row", "exit_row", "reason", "period_index", "span_index")}
        price = np.empty(count)
        emit_window(*arguments, count, columns["stock"], columns["row"], columns["exit_row"], columns["reason"], columns["period_index"],
                    columns["span_index"], price)
        frame = pd.DataFrame(columns)
        frame["window_index"] = window
        frame["exit_price"] = price

        if keep_slot is not None:
            kept.append(frame[keep_slot(slot[frame["row"].to_numpy()])].copy())

        reduced.append(frame[["row", "exit_row", "exit_price"]])

    slices = pd.concat(reduced, ignore_index=True)
    return slices, (pd.concat(kept, ignore_index=True) if kept else None)


def q1_values(context: Context, amount: float, slices: pd.DataFrame):
    """묶음마다 순 초과수익·대조·대조 뺀 값 → 같은 종목·같은 매수일 1 건 평균."""
    panel = context.panel
    minimum = amount / (CAP_PERCENT / 100.0)
    rows = slices["row"].to_numpy()
    exits = slices["exit_row"].to_numpy()
    net = net_excess(panel, context.previous_20d, amount, rows, exits, slices["exit_price"].to_numpy())
    frame = pd.DataFrame({"row": rows, "exit_row": exits, "net": net})
    by_exit = frame.groupby(["row", "exit_row"], sort=False)["net"].agg(["sum", "size"]).reset_index()
    control, members = context.pool(by_exit["row"].to_numpy(), by_exit["exit_row"].to_numpy(), minimum)
    by_exit["control_sum"] = control * by_exit["size"].to_numpy()
    by_exit["hold_sum"] = (by_exit["exit_row"] - by_exit["row"]) * by_exit["size"]
    by_exit["members_sum"] = members * by_exit["size"].to_numpy()
    single = by_exit.groupby("row", sort=True)[["sum", "size", "control_sum", "hold_sum", "members_sum"]].sum().reset_index()
    count = single["size"].to_numpy()
    result = pd.DataFrame({"row": single["row"].to_numpy(), "net": single["sum"].to_numpy() / count,
                           "control": single["control_sum"].to_numpy() / count, "hold": single["hold_sum"].to_numpy() / count,
                           "members": single["members_sum"].to_numpy() / count, "slices": count})
    result["value"] = result["net"] - result["control"]
    row_values = result["row"].to_numpy()
    result["slot"] = context.slot[row_values]
    result["month"] = panel.month_index[row_values]
    result["year"] = panel.year_index[row_values]
    return result


def describe(values: np.ndarray, months: np.ndarray) -> dict:
    return {"n": int(len(values)), "mean": float(values.mean()) if len(values) else None,
            "t": round6_test.month_cluster_t(values, months) if len(values) else None}


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    timings = {}
    context = Context()
    panel = context.panel
    month_count = panel.month_count
    year_count = panel.year_count
    years = list(range(panel.first_year, panel.first_year + year_count))
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    decile_slot = np.where(context.decile >= 0, context.decile, 10).astype(np.int64)
    timings["prepare"] = time.time() - started
    print(f"준비 끝 {timings['prepare']:.0f}초", flush=True)

    cell_count = len(AMOUNTS) * len(RULE_NAMES) * SLOTS
    month_value = np.zeros((cell_count, month_count))
    month_net = np.zeros((cell_count, month_count))
    month_n = np.zeros((cell_count, month_count))
    year_sum = np.zeros((cell_count, year_count, 2))
    control_sum = np.zeros(cell_count)
    kept_tables = {}
    kept_slices = {}

    for amount_index, amount in enumerate(AMOUNTS):
        minimum = amount / (CAP_PERCENT / 100.0)

        for rule in range(len(RULE_NAMES)):
            step = time.time()
            slices, _ = emit_all(context, rule, minimum)

            if len(slices) == 0:
                print(f"{amount:,.0f} {RULE_NAMES[rule]} 매매 없음", flush=True)
                continue

            single = q1_values(context, amount, slices)
            del slices
            cell = (amount_index * len(RULE_NAMES) + rule) * SLOTS + single["slot"].to_numpy()
            months = single["month"].to_numpy()
            np.add.at(month_value, (cell, months), single["value"].to_numpy())
            np.add.at(month_net, (cell, months), single["net"].to_numpy())
            np.add.at(month_n, (cell, months), 1.0)
            np.add.at(year_sum, (cell, single["year"].to_numpy(), 0), 1.0)
            np.add.at(year_sum, (cell, single["year"].to_numpy(), 1), single["value"].to_numpy())
            np.add.at(control_sum, cell, single["control"].to_numpy())

            if amount_index == 0 and rule in GRID_RULES:
                kept_tables[rule] = single
                # 6회차 10분위 판과 같은 매수 집합(보유 중 무시 = 순위 분위 단위)
                _, kept_slices[rule] = emit_all(context, rule, minimum, lambda slot: slot == 1, decile_slot)

            timings[f"{int(amount)}_{RULE_NAMES[rule]}"] = time.time() - step
            print(f"{amount:,.0f} {RULE_NAMES[rule]} 1 건 {len(single):,} ({time.time() - step:.0f}초)", flush=True)

    # 판정
    step = time.time()
    keys = [(amount_index, rule, absolute_bin, decile) for amount_index in range(len(AMOUNTS)) for rule in range(len(RULE_NAMES))
            for absolute_bin in range(ABSOLUTE_COUNT) for decile in range(DECILE_COUNT)]
    frame = pd.DataFrame(keys, columns=["amount_index", "rule_index", "absolute_bin", "turnover_decile"])
    frame["order_amount_krw"] = np.array(AMOUNTS)[frame["amount_index"].to_numpy()]
    frame["min_turnover_krw"] = frame["order_amount_krw"] / (CAP_PERCENT / 100.0)
    frame["rule"] = np.array(RULE_NAMES)[frame["rule_index"].to_numpy()]
    frame["absolute_label"] = np.array(round6_test.ABSOLUTE_LABELS)[frame["absolute_bin"].to_numpy()]
    table, summary = round5_test.family_table("R8", month_value, month_n, year_sum, frame, years, signs,
                                              ["family", "order_amount_krw", "rule", "absolute_bin"])
    count = month_n.sum(axis=1)

    with np.errstate(invalid="ignore", divide="ignore"):
        table["mean_net_excess_vs_market"] = month_net.sum(axis=1) / count
        table["mean_control_excess_vs_market"] = control_sum / count

    table["t_net_excess_vs_market"] = round5_test.cluster_t_matrix(month_net, month_n)
    table = table.rename(columns={"mean_excess": "mean_value_minus_control", "t_excess": "t_value_minus_control",
                                  "share_positive_excess_years": "share_positive_years_minus_control"})
    table["passes_and_net_positive"] = table["passes"] & (table["mean_net_excess_vs_market"] > 0)
    common.write_csv(OUTPUT / "cell_table.csv", table)
    judgement = table.groupby(["order_amount_krw", "rule"]).agg(
        cells=("n", "size"), cells_with_trades=("n", lambda values: int((values > 0).sum())), over_bar_a=("over_bar_a", "sum"),
        over_bar_b=("over_bar_b", "sum"), passes=("passes", "sum"), passes_and_net_positive=("passes_and_net_positive", "sum"),
        best_t=("t_value_minus_control", "max"), worst_t=("t_value_minus_control", "min")).reset_index()
    common.write_csv(OUTPUT / "judgement.csv", judgement)
    summary["cells_fixed"] = cell_count
    common.write_json(OUTPUT / "multiple_comparison.json", summary)
    timings["judge"] = time.time() - step
    print(f"통과 {int(table['passes'].sum())} 칸, M {summary['cells_M_all']}, 기준선 (가) {summary['bar_a_24_months']:.2f}"
          f" (나) {summary['bar_b_normal_equivalent_z']:.2f}", flush=True)

    # 시기 노출
    step = time.time()
    market_total = np.zeros(panel.market_count)
    np.add.at(market_total, panel.market_position, np.nan_to_num(context.previous_20d))
    regime_day, _ = round7_test.expanding_regime(market_total)
    exposure_rows = []
    days = np.arange(panel.signal_start, panel.market_count - PASSIVE_DAYS)
    gross, members = pool_returns(days, days + PASSIVE_DAYS, np.full(len(days), 2e8), np.full(len(days), 5e8), context.close_matrix,
                                  context.turnover_matrix)
    passive = (gross * common.COST - 1.0) * 100.0 - panel.bench[days - panel.signal_start, days + PASSIVE_DAYS - panel.signal_start]
    day_dates = panel.market_dates[days]
    day_months = ((day_dates // 10000 - panel.first_year) * 12 + (day_dates // 100) % 100 - 1).astype(np.int64)
    first_signal_date = int(panel.market_dates[panel.signal_start])

    for regime in range(-1, 3):
        name = "모름" if regime < 0 else round7_test.REGIME_NAMES[regime]
        chosen_days = (regime_day[days] == regime) & np.isfinite(passive)
        result = describe(passive[chosen_days], day_months[chosen_days])
        exposure_rows.append({"regime_expanding": name, "what": f"2–5억 전 종목 균등 매수 {PASSIVE_DAYS}일 보유(매일)", "value": "순 초과수익(시장 B 대비)",
                              **result, "mean_members": float(members[chosen_days].mean()) if chosen_days.any() else None})


    for rule in GRID_RULES:
        single = kept_tables[rule]
        single = single[single["slot"] // DECILE_COUNT == 2]
        single_regime = regime_day[panel.market_position[single["row"].to_numpy()]]

        for regime in range(-1, 3):
            name = "모름" if regime < 0 else round7_test.REGIME_NAMES[regime]
            part = single[single_regime == regime]

            for value_name, column in (("순 초과수익(시장 B 대비)", "net"), ("대조 순 초과수익", "control"), ("신호 − 대조", "value")):
                result = describe(part[column].to_numpy(), part["month"].to_numpy()) if len(part) else {"n": 0, "mean": None, "t": None}
                exposure_rows.append({"regime_expanding": name, "what": f"신호 {RULE_NAMES[rule]} 100만 원 2–5억 전 분위 Q1", "value": value_name,
                                      **result, "mean_members": float(part["members"].mean()) if len(part) else None})

    exposure = pd.DataFrame(exposure_rows)
    exposure.insert(0, "first_signal_date", first_signal_date)
    common.write_csv(OUTPUT / "timing_exposure.csv", exposure)
    timings["exposure"] = time.time() - step

    # 청산 격자(대조 뺀 값)
    step = time.time()
    grid_rows = []
    variant_names = ([f"E3 확인 {days}일" for days in round7_test.CONFIRM_DAYS] + [f"E4j{days}" for days in round7_test.BELOW_DAYS]
                     + [f"기본 + 트레일링 {percent:g}% (비교만)" for percent in round7_test.TRAILING_PERCENTS])
    judged_variants = len(round7_test.CONFIRM_DAYS) + len(round7_test.BELOW_DAYS)

    grid_reference = {}

    for rule in GRID_RULES:
        slices = kept_slices[rule]
        reference = q1_values(context, AMOUNTS[0], slices[["row", "exit_row", "exit_price"]])
        grid_reference[rule] = {"n": int(len(reference)), "mean_net": float(reference["net"].mean())}
        stocks = slices["stock"].to_numpy()
        rows = slices["row"].to_numpy().astype(np.int64)
        exit_rows, exit_prices, triggered = round7_test.exit_variants(
            rows, panel.starts[stocks].astype(np.int64), panel.starts[stocks + 1].astype(np.int64),
            slices["exit_row"].to_numpy().astype(np.int64), slices["exit_price"].to_numpy(),
            slices["window_index"].to_numpy().astype(np.int64), slices["span_index"].to_numpy().astype(np.int64), panel.averages,
            panel.open_price, panel.high, panel.low, panel.close, context.states, np.array(round7_test.CONFIRM_DAYS, np.int64),
            np.array(round7_test.BELOW_DAYS, np.int64), np.array(round7_test.TRAILING_PERCENTS))

        for column, name in enumerate(variant_names):
            variant = pd.DataFrame({"row": rows, "exit_row": exit_rows[:, column], "exit_price": exit_prices[:, column]})
            single = q1_values(context, AMOUNTS[0], variant)
            judged = column < judged_variants
            t_value = round6_test.month_cluster_t(single["value"].to_numpy(), single["month"].to_numpy())
            by_year = single.groupby(panel.years[single["row"].to_numpy()])["value"].mean()
            grid_rows.append({"entry_set": f"100만 원 {RULE_NAMES[rule]} 1분위 매수(6회차 남는 칸)", "variant": name, "judged": judged,
                              "n": int(len(single)), "mean_net_excess_vs_market": float(single["net"].mean()),
                              "t_net_excess_vs_market": round6_test.month_cluster_t(single["net"].to_numpy(), single["month"].to_numpy()),
                              "mean_control": float(single["control"].mean()), "mean_value_minus_control": float(single["value"].mean()),
                              "t_value_minus_control": t_value, "share_positive_years_minus_control": float((by_year > 0).mean()),
                              "win_rate_vs_control": float((single["value"] > 0).mean()), "mean_hold_days": float(single["hold"].mean()),
                              "passes_bar_3_0": bool(judged and np.nan_to_num(t_value, nan=-np.inf) > GRID_BAR)})

    common.write_csv(OUTPUT / "exit_grid.csv", pd.DataFrame(grid_rows))
    timings["exit_grid"] = time.time() - step

    # 원인 진단
    step = time.time()
    hold_rows, feature_rows = [], []
    close = panel.close
    previous_row = np.arange(len(close)) - 20
    previous_row_5 = np.arange(len(close)) - 5
    valid_20 = previous_row >= panel.starts[context.stock_of_row]
    valid_5 = previous_row_5 >= panel.starts[context.stock_of_row]
    features = pd.DataFrame({
        "day": panel.market_position, "bin": context.absolute,
        "return_20d": np.where(valid_20, close / close[np.maximum(previous_row, 0)] - 1.0, np.nan),
        "return_5d": np.where(valid_5, close / close[np.maximum(previous_row_5, 0)] - 1.0, np.nan),
        "atr20_over_close": panel.atrs[2] / close, "close_over_ma60": close / panel.averages[4] - 1.0})
    in_scope = (np.arange(len(close)) >= 0) & (context.absolute >= 0) & (panel.market_position >= panel.signal_start)
    feature_names = ["return_20d", "return_5d", "atr20_over_close", "close_over_ma60"]
    ranks = features[in_scope].groupby(["day", "bin"])[feature_names].rank(pct=True)
    percentile = np.full((len(close), len(feature_names)), np.nan)
    percentile[np.flatnonzero(in_scope)] = ranks.to_numpy()

    for rule in GRID_RULES:
        single = kept_tables[rule]
        groups = {"전 구간·전 분위": np.ones(len(single), bool), "1분위": (single["slot"] % DECILE_COUNT == 1).to_numpy(),
                  "2–5억 전 분위": (single["slot"] // DECILE_COUNT == 2).to_numpy()}

        for group_name, chosen_rows in groups.items():
            part = single[chosen_rows]
            hold_bucket = np.searchsorted(np.array(HOLD_EDGES), part["hold"].to_numpy(), side="right") - 1

            for bucket in range(len(HOLD_EDGES)):
                bucket_part = part[hold_bucket == bucket]
                label = f"{HOLD_EDGES[bucket]}–{HOLD_EDGES[bucket + 1] - 1}일" if bucket + 1 < len(HOLD_EDGES) else f"{HOLD_EDGES[bucket]}일 이상"
                hold_rows.append({"rule": RULE_NAMES[rule], "group": group_name, "hold_bucket": label, "share": float(len(bucket_part) / max(len(part), 1)),
                                  "mean_net": float(bucket_part["net"].mean()) if len(bucket_part) else None,
                                  "mean_control": float(bucket_part["control"].mean()) if len(bucket_part) else None,
                                  **{f"{key}_minus_control": value for key, value in
                                     describe(bucket_part["value"].to_numpy(), bucket_part["month"].to_numpy()).items()}})

            feature_values = percentile[part["row"].to_numpy()]
            feature_rows.append({"rule": RULE_NAMES[rule], "group": group_name, "n": int(len(part)),
                                 **{f"mean_percentile_{name}": float(np.nanmean(feature_values[:, index])) for index, name in enumerate(feature_names)},
                                 "mean_value_minus_control": float(part["value"].mean()),
                                 "t_value_minus_control": round6_test.month_cluster_t(part["value"].to_numpy(), part["month"].to_numpy())})

    common.write_csv(OUTPUT / "diagnosis_hold.csv", pd.DataFrame(hold_rows))
    common.write_csv(OUTPUT / "diagnosis_features.csv", pd.DataFrame(feature_rows))
    timings["diagnosis"] = time.time() - step

    # 선택 기록 1–8회차
    history = pd.read_csv(common.STUDY / "results" / "round7" / "choices_log.csv")
    new_rows = pd.DataFrame([
        (8, "판정 값을 같은 날·같은 보유 기간·같은 절대 구간 전 종목 균등 매수 대조를 뺀 값으로", "지시(같은 날·같은 보유 기간으로 좁힌 것은 내가 정함)", "예",
         "–", "7회차 같은 달 대조 결과를 본 뒤"),
        (8, "전 절대 구간 × 전 분위 × 금액 4 × 규칙 13 다시 보기", "지시", "예", "칸 3,640", "1분위로 좁혀 온 경로를 풀기 위함"),
        (8, "상한 0.5% 고정", "내가 정함", "예", "–", "7회차와 같게"),
        (8, "시기 노출(확장창 국면 × 2–5억 균등 매수 20일)", "지시(20일은 내가 정함)", "예", "국면 4 × 판 7", ""),
        (8, "청산 격자를 대조 뺀 값으로", "지시", "예", "26 검정(+트레일링 6 비교)", ""),
    ], columns=history.columns)
    common.write_csv(OUTPUT / "choices_log.csv", pd.concat([history, new_rows], ignore_index=True))

    # 매매 목록(6회차와 같은 열): 통과 칸 + 6회차 1분위 남는 칸(8회차 재계산)
    step = time.time()
    listings = []
    passing = table[table["passes"]]

    for (amount_index, rule), cells in passing.groupby(["amount_index", "rule_index"]):
        amount = AMOUNTS[amount_index]
        wanted = set((cells["absolute_bin"] * DECILE_COUNT + cells["turnover_decile"]).tolist())
        _, slices = emit_all(context, int(rule), amount / (CAP_PERCENT / 100.0), lambda slot: np.isin(slot, list(wanted)))
        slices["excess_c0_3"] = net_excess(panel, context.previous_20d, amount, slices["row"].to_numpy(), slices["exit_row"].to_numpy(),
                                           slices["exit_price"].to_numpy())
        slot_of = context.slot[slices["row"].to_numpy()]

        for slot_value in sorted(wanted):
            label = {"cell_role": "8회차 대조 뺀 값 통과 칸", "scheme": "absolute_x_decile", "rule": RULE_NAMES[rule],
                     "turnover_bin": f"{round6_test.ABSOLUTE_LABELS[slot_value // DECILE_COUNT]}|{slot_value % DECILE_COUNT}분위"}
            listing = round7_test.build_listing(panel, context.previous_20d, slices[slot_of == slot_value].copy(), context.pivots, context.mask,
                                                context.span_values, label)
            listing["order_amount_krw"] = amount
            listings.append(listing)

    for rule in GRID_RULES:
        slices = kept_slices[rule].copy()
        slices["excess_c0_3"] = net_excess(panel, context.previous_20d, AMOUNTS[0], slices["row"].to_numpy(), slices["exit_row"].to_numpy(),
                                           slices["exit_price"].to_numpy())
        label = {"cell_role": "6회차 10분위 판 남는 칸(8회차 재계산, 대조 뺀 값은 cell_table)", "scheme": "decile", "rule": RULE_NAMES[rule],
                 "turnover_bin": "1"}
        listings.append(round7_test.build_listing(panel, context.previous_20d, slices, context.pivots, context.mask, context.span_values, label))

    common.write_csv(OUTPUT / "survivors_trades.csv", pd.concat(listings, ignore_index=True))
    timings["listing"] = time.time() - step
    timings["total"] = time.time() - started
    reproduction = {RULE_NAMES[rule]: grid_reference[rule] for rule in GRID_RULES}
    common.write_json(OUTPUT / "round8_metrics.json", {
        "reproduction": panel.reproduction(), "timings_seconds": timings, "cells_fixed": cell_count,
        "round6_decile1_reproduction": reproduction, "round6_expected": {"E3": [30597, 1.3013], "E4j5": [43746, 1.0364]},
        "passing_cells": passing[["order_amount_krw", "rule", "absolute_label", "turnover_decile", "n", "mean_value_minus_control",
                                  "t_value_minus_control", "mean_net_excess_vs_market", "months_with_trades",
                                  "share_positive_years_minus_control"]].to_dict("records"),
        "hand_set": {"control": "같은 날·같은 보유 기간·같은 절대 구간(하한 같이) 보통주 균등, 비용 COST 는 대조에도, 충격 비용은 신호만 — 내가 정함",
                     "cap_percent": "0.5 고정 — 내가 정함", "passive_days": f"{PASSIVE_DAYS} — 내가 정함",
                     "grid_bar": "26 검정 t 3.0 — 내가 정함", "hold_edges": "1–5·6–20·21–60·61+ — 내가 정함"},
        "confirmation_only": "8회차 칸 구성은 7회차 결과를 보고 정했다"})
    print(f"전체 {timings['total']:.0f}초", flush=True)


if __name__ == "__main__":
    main()
