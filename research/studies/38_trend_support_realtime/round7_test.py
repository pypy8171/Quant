"""스터디 38 7회차 — 1분위 효과가 순위에서 오는지 시기에서 오는지 교차표로 가르고, 같은 달·같은 절대 구간 대조, 추세 청산 격자.

**확인용 회차다.** 6회차는 5회차 결과(1분위·100만 원·상한 0.5%)를 보고 설계했고, 7회차 칸(E3·E4j5, 100만 원·상한 0.5%,
1분위 대 나머지)은 6회차 결과를 보고 골랐다. 그래서 여기 판정은 새 발견이 아니라 앞 회차 결과가 버티는지 보는 확인이다.
1–7회차에서 사전 정의 없이 바꾼 조건은 results/round7/choices_log.csv 에 남긴다.

1–6회차 산출은 건드리지 않고 results/round7/ 에만 쓴다. 매수는 신호일 종가(동시호가), 다음 날 아님.
신호는 6회차 단일 규칙 그대로: 결함 고친 추세(종가 > 마지막 확정 저점), k 8 개 중 하나라도 상승이면 1 주문, 지지 = 신호 난 k 중
가장 높은 확정 저점(같으면 작은 k), 이평 N 7 × ATR 5 = 35 묶음, 거리 안쪽 12 구간. 비용·충격 비용 식·비교 평균(B)은 6회차와 같다.
주문 100만 원·상한 0.5%(하한 = 전날까지 20일 평균 거래대금 2억 원 이상)·계수 0.3 하나만 쓴다(지시).

시작 전에 고정한 칸(지시 2026-10-11 7회차):
  1) 교차표 = 규칙 {E3, E4j5} × 절대 구간 × 그날 순위 10분위 × 시장 거래대금 국면 3.
     절대 구간 7 개 중 <1억·1–2억은 하한 2억 때문에 구조상 비어 있어 칸에서 뺀다 → 5 개(2–5억, 5–10억, 10–30억, 30–100억, ≥100억).
     칸 수 = 2 × 5 × 10 × 3 = 300(판마다). 기준선은 이 300 칸 중 매매 3 건·거래 든 달 2 개 이상인 칸(M)으로 다시 구한다.
     국면 두 판:
       X_full  전 기간 3분위(6회차와 같음 — 경계에 미래 정보가 들어 있다)
       X_exp   확장창: 그날 값을 그 전날까지 모든 시장일(자료 첫날부터) 분포의 3분위 경계와 비교(미래 정보 없음).
               앞선 시장일이 250 일 미만이면 국면 모름(내가 정함 — 신호 기간 2010 이후는 해당 없음).
     칸 안 보유 중 무시 = 칸(규칙·절대 구간·순위·국면) 단위. 판 = Q1(같은 종목·같은 매수일 1 건, 35 묶음 평균).
     판정(5·6회차와 같은 방식, 확인용): (가) 거래 든 달 24 개월 이상 칸 최대 t 95% 분위, (나) 자유도 보정 — 둘 다,
     옆 순위 분위 부호 같음(같은 규칙·절대 구간·국면), 양수 연도 ≥ 0.7, 평균 > 0. 뒤집기 1,000 회, seed 20261010.
  1') 같은 절대 구간 안 1분위 대 나머지 순위: 1분위 매매 값 − 같은 달·같은 규칙·같은 절대 구간(·같은 국면) 2–10분위 매매 평균,
     월 묶음 t. 국면 합친 판 = 규칙 2 × 절대 구간 5 = 10 검정 → 통과선 t 3.0(헌장 20 칸 기준을 그대로 씀 — 내가 정함).
     국면별 판(30 검정)은 표로만.
  2) 같은 달 대조(6회차 10분위 남는 칸 E3·E4j5 1분위): 대조군 = 같은 달·같은 절대 구간·같은 규칙의 10분위 판 Q1 매매.
     (A) 1분위 포함, (B) 1분위 제외 두 판.
  3) 추세 청산 격자(판정에 안 씀, 격자는 지시자가 정함): 같은 매수 집합(6회차 남는 칸 두 개)에 매도만 바꿔 다시 계산.
     E3 확인 일수 c ∈ {1,2,3}(추세 −1 이 c 일 이어진 날 종가), E4j n ∈ {1..10}(종가 < 이평 N 이 n 일 이어진 날 종가),
     기본 규칙(E3·E4j5) + 트레일링 {10,15,20}%: 매수 종가부터 전날까지 최고 고가 × (1 − x) 를 장중 저가가 찍으면 그 값,
     시가가 이미 아래면 시가(먼저 걸리는 쪽, 같은 날이면 트레일링 우선). 매수 집합은 고정 — 매도가 바뀌어도 보유 중 무시를
     다시 하지 않는다(내가 정함, 매수가 같아야 매도만 비교된다). 최고 고가 시작값 = 매수 종가(내가 정함).

산출(results/round7/):
  cross_table.csv          X_full·X_exp × 규칙 × 절대 구간 × 순위 × 국면(300 칸씩): n·평균·t·기준선 넘음·통과
  cross_judgement.csv      판별 통과 수
  multiple_comparison.json 판별 M·기준선
  decile1_difference.csv   1분위 대 나머지(같은 달·같은 절대 구간) — 국면 합침·국면별
  regime_days.csv          두 판의 국면별 시장일 수·연도 범위
  same_month_absolute_control.csv  2) 결과
  exit_grid.csv            3) 결과
  choices_log.csv          1–7회차 사전 정의 없이 바꾼 조건
  survivors_trades.csv     6회차와 같은 열. 교차표 통과 칸 + 6회차 10분위 남는 칸(7회차 재계산)
  round7_metrics.json      재현 정보·시간·6회차 재현 대조
재실행: py research/studies/38_trend_support_realtime/round7_test.py
"""
from __future__ import annotations

import bisect
import os
import time

import common     # numba 보다 먼저: common 이 NUMBA_NUM_THREADS(12)를 넣는다
import numpy as np
import pandas as pd
from numba import njit

import round3_test
import round4_test
import round5_test
import round6_test

OUTPUT = common.STUDY / "results" / "round7"
AMOUNT = 1_000_000.0            # 지시
CAP_PERCENT = 0.5               # 지시
MINIMUM = AMOUNT / (CAP_PERCENT / 100.0)
COEFFICIENT = 0.3               # 지시
RULES = (2, 7)                  # E3, E4j5 (지시)
ABSOLUTE_USED = (2, 3, 4, 5, 6) # 2–5억 … ≥100억 (하한 2억 때문에 0·1 은 구조상 빈다)
REGIME_NAMES = ("낮음", "중간", "높음")
EXPANDING_MINIMUM_DAYS = 250    # 내가 정함
DIFFERENCE_BAR = 3.0            # 국면 합친 10 검정 통과선(헌장 20 칸 기준 — 내가 정함)
CONFIRM_DAYS = (1, 2, 3)        # 지시자가 정함
BELOW_DAYS = tuple(range(1, 11))  # 지시자가 정함
TRAILING_PERCENTS = (10.0, 15.0, 20.0)  # 지시자가 정함
SLOT_PER_ABSOLUTE = 30          # 순위 10 × 국면 3


# ── 매매 뽑기(칸 = 슬롯, 보유 중 무시는 슬롯마다) ─────────────────────────

@njit(cache=True)
def emit_slots(rule, slot, slot_count, minimum, averages, atrs, open_price, low, close, starts, signal_ok, base_turnover, chosen,
               states, latest_lows, distance_edges, capacity, out_stock, out_row, out_exit_row, out_reason, out_window, out_period,
               out_span, out_price):
    """6회차 emit_ensemble 과 같은 매매를 슬롯 전부에 대해 한 번에. capacity 0 이면 수만 센다."""
    spans = states.shape[0]
    inner_last = distance_edges.shape[0] - 1
    longest = 0

    for stock in range(starts.shape[0] - 1):
        longest = max(longest, starts[stock + 1] - starts[stock])

    next_exit_span = np.empty((spans, longest + 1), dtype=np.int64)
    next_exit_window = np.empty(longest + 1, dtype=np.int64)
    run_length = np.zeros(longest + 1, dtype=np.int64)
    allowed = np.zeros(slot_count, dtype=np.int64)
    written = 0

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]
        length = last - first

        if rule <= 2:
            for span in range(spans):
                common.fill_next_exit(rule, first, length, averages[0], close, low, states[span], latest_lows[span], run_length,
                                      next_exit_span[span])

        for window in range(averages.shape[0]):
            average = averages[window]

            if rule >= 3:
                common.fill_next_exit(rule, first, length, average, close, low, states[0], latest_lows[0], run_length,
                                      next_exit_window)

            for period in range(atrs.shape[0]):
                atr = atrs[period]
                allowed[:] = 0

                for offset in range(length):
                    row = first + offset
                    span = chosen[row]
                    row_slot = slot[row]

                    if row_slot < 0 or row_slot >= slot_count or not signal_ok[row] or span < 0 or offset < allowed[row_slot]:
                        continue

                    if not (np.isfinite(average[row]) and np.isfinite(atr[row]) and atr[row] > 0):
                        continue

                    distance_index = common.distance_bin((low[row] - average[row]) / atr[row], distance_edges)

                    if distance_index < 1 or distance_index > inner_last:
                        continue

                    if not (base_turnover[row] >= minimum):
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
                        out_window[written] = window
                        out_period[written] = period
                        out_span[written] = span
                        out_price[written] = exit_price

                    written += 1

    return written


def emit_frame(panel, rule, slot, slot_count, previous_20d, chosen, states, latest_lows) -> pd.DataFrame:
    arguments = (rule, slot, slot_count, MINIMUM, panel.averages, panel.atrs, panel.open_price, panel.low, panel.close,
                 panel.starts, panel.signal_ok, previous_20d, chosen, states, latest_lows, common.DISTANCE_EDGES)
    empty_int = np.zeros(0, np.int64)
    count = emit_slots(*arguments, 0, empty_int, empty_int, empty_int, empty_int, empty_int, empty_int, empty_int,
                       np.zeros(0))
    columns = {name: np.empty(count, np.int64) for name in ("stock", "row", "exit_row", "reason", "window_index", "period_index",
                                                             "span_index")}
    price = np.empty(count)
    emit_slots(*arguments, count, columns["stock"], columns["row"], columns["exit_row"], columns["reason"],
               columns["window_index"], columns["period_index"], columns["span_index"], price)
    frame = pd.DataFrame({name: values.astype(np.int32) for name, values in columns.items()})
    frame["exit_price"] = price
    frame["slot"] = slot[frame["row"].to_numpy()].astype(np.int32)
    return frame


def excess_of(panel, previous_20d, rows, exit_rows, exit_prices) -> np.ndarray:
    """6회차 trade_values 와 같은 식(계수 0.3, 100만 원)."""
    buy_ratio = AMOUNT / previous_20d[rows]
    sell_base = previous_20d[exit_rows]
    sell_ratio = np.where(np.isfinite(sell_base) & (sell_base > 0), AMOUNT / np.where(sell_base > 0, sell_base, 1.0), buy_ratio)
    multiplier = np.maximum(0.0, 1.0 - COEFFICIENT * sell_ratio) / (1.0 + COEFFICIENT * buy_ratio)
    gross = exit_prices / panel.close[rows]
    market = panel.bench[panel.market_position[rows] - panel.signal_start, panel.market_position[exit_rows] - panel.signal_start]
    return (gross * multiplier * common.COST - 1.0) * 100.0 - market


def single_trades(slices: pd.DataFrame, value_column: str) -> pd.DataFrame:
    """같은 슬롯·종목·매수일 1 건(35 묶음 평균)."""
    grouped = slices.groupby(["slot", "stock", "row"], sort=True)[value_column]
    single = grouped.mean().rename("value").reset_index()
    single["slices"] = grouped.size().to_numpy()
    return single


# ── 매도 격자 ───────────────────────────────────────────────────────────────

@njit(cache=True)
def exit_variants(rows, firsts, lasts, base_exit_rows, base_exit_prices, windows, spans, averages, open_price, high, low, close,
                  states, confirm_days, below_days, trailing):
    """매수 고정, 매도만 바꾼 결과. 열 순서: E3 확인 c, E4j n, 기본 + 트레일링 x. 반환 (매도 행, 가격, 규칙/트레일링 걸림)."""
    count = rows.shape[0]
    variants = confirm_days.shape[0] + below_days.shape[0] + trailing.shape[0]
    exit_rows = np.empty((count, variants), dtype=np.int64)
    exit_prices = np.empty((count, variants))
    triggered = np.zeros((count, variants), dtype=np.bool_)

    for index in range(count):
        row = rows[index]
        first = firsts[index]
        last = lasts[index]
        state = states[spans[index]]
        average = averages[windows[index]]
        column = 0

        for confirm in confirm_days:
            run = 0
            exit_row = -1

            for day in range(row + 1, last):
                run = run + 1 if state[day] == -1 else 0

                if run >= confirm:
                    exit_row = day
                    break

            if exit_row >= 0:
                exit_rows[index, column] = exit_row
                exit_prices[index, column] = close[exit_row]
                triggered[index, column] = True
            else:
                exit_rows[index, column] = last - 1
                exit_prices[index, column] = close[last - 1]

            column += 1

        for days in below_days:
            run = 0
            exit_row = -1

            for day in range(max(first, row - 10), last):
                run = run + 1 if (np.isfinite(average[day]) and close[day] < average[day]) else 0

                if day >= row + days and run >= days:
                    exit_row = day
                    break

            if exit_row >= 0:
                exit_rows[index, column] = exit_row
                exit_prices[index, column] = close[exit_row]
                triggered[index, column] = True
            else:
                exit_rows[index, column] = last - 1
                exit_prices[index, column] = close[last - 1]

            column += 1

        for percent in trailing:
            peak = close[row]
            exit_rows[index, column] = base_exit_rows[index]
            exit_prices[index, column] = base_exit_prices[index]

            for day in range(row + 1, base_exit_rows[index] + 1):
                level = peak * (1.0 - percent / 100.0)

                if low[day] <= level:
                    exit_rows[index, column] = day
                    exit_prices[index, column] = open_price[day] if open_price[day] < level else level
                    triggered[index, column] = True
                    break

                peak = max(peak, high[day])

            column += 1

    return exit_rows, exit_prices, triggered


# ── 국면 ────────────────────────────────────────────────────────────────────

def expanding_regime(market_total: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """그날 값을 그 전날까지 분포의 3분위 경계와 비교. 반환 (국면 −1 = 모름, 경계 두 개)."""
    regime = np.full(len(market_total), -1, dtype=np.int64)
    edges = np.full((len(market_total), 2), np.nan)
    history: list[float] = []

    for day, value in enumerate(market_total):
        if len(history) >= EXPANDING_MINIMUM_DAYS:
            size = len(history)
            low_edge = history[int((size - 1) / 3.0)]
            high_edge = history[int(2.0 * (size - 1) / 3.0)]
            edges[day] = (low_edge, high_edge)
            regime[day] = 0 if value < low_edge else (1 if value < high_edge else 2)

        bisect.insort(history, float(value))

    return regime, edges


# ── 매매 목록(6회차와 같은 열) ──────────────────────────────────────────────

def build_listing(panel, previous_20d, slices: pd.DataFrame, pivots, mask, span_values, label: dict) -> pd.DataFrame:
    k_array = np.array(span_values)
    rows_all = slices["row"].to_numpy()
    mae, mfe, _, _, _, _, _ = round6_test.trade_paths(rows_all.astype(np.int64), slices["exit_row"].to_numpy().astype(np.int64),
                                                      slices["exit_price"].to_numpy(), False, panel.open_price, panel.high,
                                                      panel.low, panel.close, np.zeros(0))
    slices = slices.assign(mae_percent=mae, mfe_percent=mfe,
                           realized_percent=(slices["exit_price"].to_numpy() / panel.close[rows_all] - 1.0) * 100.0,
                           hold_days=slices["exit_row"].to_numpy() - rows_all)
    slices = slices.sort_values(["stock", "row", "window_index", "period_index"], kind="mergesort")
    grouped = slices.groupby(["stock", "row"], sort=True)
    single = grouped[["excess_c0_3", "mae_percent", "mfe_percent", "realized_percent", "hold_days"]].mean()
    single["slices"] = grouped.size()
    first_slice = grouped.first()
    for column in ("exit_row", "exit_price", "reason", "span_index"):
        single[column] = first_slice[column]
    single["distinct_exits"] = grouped["exit_row"].nunique()
    single["windows"] = grouped["window_index"].agg(lambda values: "|".join(str(common.WINDOWS[value]) for value in sorted(set(values))))
    single["atr_periods"] = grouped["period_index"].agg(lambda values: "|".join(str(common.ATR_PERIODS[value]) for value in sorted(set(values))))
    single = single.reset_index()
    stock_array = single["stock"].to_numpy()
    rows = single["row"].to_numpy()
    exits = single["exit_row"].to_numpy()
    span_index = single["span_index"].to_numpy()
    support_dates, support_values = [], []

    for stock, row, span in zip(stock_array, rows, span_index):
        pivot = pivots[span][stock]
        offset = row - panel.starts[stock]
        position = int(np.searchsorted(pivot["low_confirm_rows"], offset, side="right")) - 1
        support_dates.append(int(panel.dates[panel.starts[stock] + pivot["low_rows"][position]]) if position >= 0 else -1)
        support_values.append(float(pivot["low_values"][position]) if position >= 0 else np.nan)

    reason_text = np.where(single["reason"].to_numpy() == common.REASON_RULE, common.REASON_TEXT[common.REASON_RULE],
                           np.where(panel.listed_at_end[stock_array], common.REASON_TEXT[common.REASON_OPEN],
                                    common.REASON_TEXT[common.REASON_END]))
    signal_k = ["|".join(str(k_array[bit]) for bit in range(len(span_values)) if value >> bit & 1) for value in mask[rows]]
    return pd.DataFrame({
        "cell_role": label["cell_role"], "scheme": label["scheme"], "rule": label["rule"], "turnover_bin": label["turnover_bin"],
        "order_amount_krw": AMOUNT, "ratio_cap_percent": str(CAP_PERCENT),
        "code": np.array(panel.codes)[stock_array], "name": np.array(panel.names)[stock_array], "entry_date": panel.dates[rows],
        "entry_price_adjusted": panel.close[rows], "entry_price_raw": panel.raw_close[rows], "exit_date": panel.dates[exits],
        "exit_price_adjusted": single["exit_price"].to_numpy(),
        "exit_price_raw": single["exit_price"].to_numpy() * panel.raw_close[exits] / panel.close[exits],
        "exit_reason": reason_text, "signal_k_list": signal_k, "chosen_k": k_array[span_index],
        "support_low_date": support_dates, "support_low_value_adjusted": support_values,
        "moving_average_windows": single["windows"].to_numpy(), "atr_periods": single["atr_periods"].to_numpy(),
        "slices": single["slices"].to_numpy(), "distinct_exits_across_windows": single["distinct_exits"].to_numpy(),
        "previous_20d_turnover_krw": previous_20d[rows], "excess_c0_3": single["excess_c0_3"].to_numpy(),
        "mae_percent": single["mae_percent"].to_numpy(), "mfe_percent": single["mfe_percent"].to_numpy(),
        "realized_percent": single["realized_percent"].to_numpy(), "hold_days": single["hold_days"].to_numpy()})


# ── 사전 정의 없이 바꾼 조건(1–7회차) ───────────────────────────────────────

CHOICES = [
    # 회차, 바꾼 것, 정한 쪽, 결과를 보고 골랐나, 늘어난 비교(대략), 설명
    (1, "모든 조합 한 번에(k·N·ATR·거리·반등·규칙)", "사용자 승인안", "아니오", "상승 칸 313,600",
     "자동 선택 없음, 통과선 없음"),
    (2, "반등 구간 축을 하나로 합침", "지시", "예", "–", "1회차 칸 표를 본 뒤"),
    (2, "거래대금·시가총액 10분위·코스피 국면 축 추가", "지시", "예", "칸 × 11 × 11 × 4", "1회차 결과를 본 뒤 종목 구간 축으로 바꿈"),
    (3, "시가총액·코스피 국면·거리 구간 축을 칸에서 뺌", "지시(2회차 차이가 작음)", "예", "–", "2회차 결과로 축을 고름"),
    (3, "거래대금 보정표로 분위 다시 매김", "지시(자료 오류 점검 뒤)", "예", "–", "2회차 상위 값 점검을 보고 정함"),
    (3, "주문 금액 4 × 상한 6 축 추가", "지시(격자는 내가 정함)", "아니오", "칸 × 24", "체결 제약"),
    (4, "문턱·분위 기준을 전날까지 값으로(6 판)", "지시", "예", "판 × 6", "3회차의 미래 정보 지적 뒤"),
    (4, "충격 비용 계수 4 개 축", "지시(격자는 내가 정함)", "아니오", "칸 × 4", ""),
    (4, "판정 판 4·주문 1억 원·계수 0.3·양수 연도 0.7", "지시", "예", "–", "판정 숫자"),
    (5, "판정을 주문 금액 4 개 모두로", "지시", "예", "판정 칸 × 4", "4회차 1억 원 판정이 0 칸인 것을 본 뒤"),
    (5, "같은 종목·같은 매수일 1 건 판(P1)을 판정 판으로", "내가 정함", "예", "–", "중복 계산 지적 뒤"),
    (5, "기준선 (가)를 24 개월 이상 칸으로", "지시", "예", "–", "짧은 칸의 큰 t 를 본 뒤"),
    (5, "거래대금 급증 거름 {2,3,5,10} 추가", "지시", "아니오", "칸 × 5", ""),
    (6, "매수 조건 결함 수정(종가 > 마지막 확정 저점)", "지시(조건은 내가 정함)", "예", "–", "경방 2022-07-01 예를 본 뒤"),
    (6, "k 8 개를 단일 규칙으로 묶음", "지시", "예", "k 축 제거", "5회차 k 별 결과를 본 뒤"),
    (6, "절대 거래대금 7 구간", "지시", "예", "구간 축 교체", "5회차 1분위가 2–7억으로 작동한 것을 본 뒤"),
    (6, "10분위 판 남는 칸을 진단 대상으로 추가", "내가 정함", "예", "–", "절대 구간 남는 칸 0 을 본 뒤"),
    (7, "E3·E4j5, 100만 원·상한 0.5% 만", "지시", "예", "칸 2 × 5 × 10 × 3 = 300", "6회차 남는 칸에서 고름"),
    (7, "1분위 대 나머지 차이 검정", "지시", "예", "10 검정(+국면별 30)", "6회차 결과에서 고름"),
    (7, "국면 확장창 판", "지시", "아니오", "판 × 2", "미래 정보 제거"),
    (7, "추세 청산 격자(E3 확인 1–3, E4j 1–10, 트레일링 10/15/20%)", "지시", "예", "16 판(판정 안 씀)",
     "6회차 손절 비교 뒤"),
]


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    timings = {}
    panel = common.Panel()
    round3_test.apply_corrections(panel)
    panel.build_benchmark()
    previous_20d = round4_test.shift_within_stock(panel.turnover20, panel.starts)
    panel.turnover20 = previous_20d
    deciles = panel.build_deciles()
    decile = deciles[:, 1].astype(np.int64)
    absolute = np.where(np.isfinite(previous_20d), np.searchsorted(round6_test.ABSOLUTE_EDGES, np.nan_to_num(previous_20d), side="right"),
                        -1).astype(np.int64)
    span_values = list(common.K_VALUES)
    states, latest_lows, pivots = [], [], []

    for span in span_values:
        state, latest_low, pivots_by_stock = panel.trend(span)
        states.append(state)
        latest_lows.append(latest_low)
        pivots.append(pivots_by_stock)

    states = np.stack(states)
    latest_lows = np.stack(latest_lows)
    states_fixed = round6_test.fixed_states(states, latest_lows, panel.close)
    del states
    chosen, mask = round6_test.choose_support(states_fixed, latest_lows)
    month_count = panel.month_count
    year_count = panel.year_count
    years = list(range(panel.first_year, panel.first_year + year_count))
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)

    # 국면: 전 종목 전날까지 20일 평균 거래대금 합
    market_total_all = np.zeros(panel.market_count)
    np.add.at(market_total_all, panel.market_position, np.nan_to_num(previous_20d))
    signal_total = market_total_all[panel.signal_start:]
    full_edges = np.quantile(signal_total, [1.0 / 3.0, 2.0 / 3.0])
    regime_full_day = np.full(panel.market_count, -1, dtype=np.int64)
    regime_full_day[panel.signal_start:] = np.searchsorted(full_edges, signal_total, side="right")
    regime_expanding_day, expanding_edges = expanding_regime(market_total_all)
    regime_rows = []

    for name, day_regime in (("X_full", regime_full_day), ("X_exp", regime_expanding_day)):
        part = day_regime[panel.signal_start:]
        dates = panel.market_dates[panel.signal_start:]

        for regime in range(-1, 3):
            days = part == regime

            if days.any():
                regime_rows.append({"family": name, "regime": "모름" if regime < 0 else REGIME_NAMES[regime], "market_days": int(days.sum()),
                                    "first_date": int(dates[days].min()), "last_date": int(dates[days].max()),
                                    **{f"days_{year}": int((days & (dates // 10000 == year)).sum()) for year in years}})

    common.write_csv(OUTPUT / "regime_days.csv", pd.DataFrame(regime_rows))
    timings["prepare"] = time.time() - started
    print(f"준비 끝 {timings['prepare']:.0f}초, 자료 첫 시장일 {int(panel.market_dates[0])}", flush=True)

    # 슬롯: 10분위 판(6회차 재현·대조·청산 격자용), 교차표 두 판
    decile_slot = np.where(decile >= 0, decile, 10).astype(np.int64)
    slot_schemes = {"decile": (decile_slot, 11)}

    for name, day_regime in (("X_full", regime_full_day), ("X_exp", regime_expanding_day)):
        row_regime = day_regime[panel.market_position]
        valid = (absolute >= 0) & (decile >= 0) & (row_regime >= 0)
        slot = np.where(valid, absolute * SLOT_PER_ABSOLUTE + decile * 3 + row_regime, -1).astype(np.int64)
        slot_schemes[name] = (slot, 7 * SLOT_PER_ABSOLUTE)

    singles = {}
    decile_slices = {}

    for scheme, (slot, slot_count) in slot_schemes.items():
        for rule in RULES:
            step = time.time()
            slices = emit_frame(panel, rule, slot, slot_count, previous_20d, chosen, states_fixed, latest_lows)
            rows = slices["row"].to_numpy()
            slices["excess_c0_3"] = excess_of(panel, previous_20d, rows, slices["exit_row"].to_numpy(), slices["exit_price"].to_numpy())
            single = single_trades(slices, "excess_c0_3")
            single["rule"] = common.RULE_NAMES[rule]
            single["month"] = panel.month_index[single["row"].to_numpy()]
            single["year"] = panel.year_index[single["row"].to_numpy()]
            single["absolute"] = absolute[single["row"].to_numpy()]
            singles[(scheme, rule)] = single

            if scheme == "decile":
                decile_slices[rule] = slices[slices["slot"] == 1].copy()

            del slices
            timings[f"emit_{scheme}_{common.RULE_NAMES[rule]}"] = time.time() - step
            print(f"{scheme} {common.RULE_NAMES[rule]} 1 건 {len(single):,} ({time.time() - step:.0f}초)", flush=True)

    # 6회차 재현 대조(10분위 1분위)
    round6_table = pd.read_csv(common.STUDY / "results" / "round6" / "decision_table.csv", low_memory=False)
    round6_cells = round6_table[(round6_table["scheme"] == "decile") & (round6_table["family"] == "Q1")
                                & (round6_table["turnover_bin"] == 1) & (round6_table["order_amount_krw"] == AMOUNT)
                                & (round6_table["ratio_cap_percent"].astype(str) == "0.5") & (round6_table["impact_coefficient"] == COEFFICIENT)]
    reproduction = []

    for rule in RULES:
        single = singles[("decile", rule)]
        part = single[single["slot"] == 1]
        reference = round6_cells[round6_cells["rule"] == common.RULE_NAMES[rule]].iloc[0]
        reproduction.append({"rule": common.RULE_NAMES[rule], "n_round7": int(len(part)), "n_round6": float(reference["n"]),
                             "mean_round7": float(part["value"].mean()), "mean_round6": float(reference["mean_excess"])})

    # 1) 교차표
    step = time.time()
    tables, summaries = [], {}
    absolute_labels = np.array(round6_test.ABSOLUTE_LABELS)

    for family in ("X_full", "X_exp"):
        keys = []

        for rule in RULES:
            for absolute_bin in ABSOLUTE_USED:
                for decile_index in range(10):
                    for regime in range(3):
                        keys.append((rule, absolute_bin, decile_index, regime))

        frame = pd.DataFrame(keys, columns=["rule_index", "absolute_bin", "turnover_decile", "regime_index"])
        cell_count = len(frame)
        month_sum = np.zeros((cell_count, month_count))
        month_n = np.zeros((cell_count, month_count))
        year_sum = np.zeros((cell_count, year_count, 2))
        position_of = {key: index for index, key in enumerate(keys)}

        for rule in RULES:
            single = singles[(family, rule)]
            slot = single["slot"].to_numpy()
            cell_key = [(rule, value // SLOT_PER_ABSOLUTE, (value % SLOT_PER_ABSOLUTE) // 3, value % 3) for value in np.unique(slot)]
            lookup = {value: position_of.get(key, -1) for value, key in zip(np.unique(slot), cell_key)}
            cell = np.array([lookup[value] for value in slot])
            assert (cell >= 0).all(), "칸 밖 매매"
            np.add.at(month_sum, (cell, single["month"].to_numpy()), single["value"].to_numpy())
            np.add.at(month_n, (cell, single["month"].to_numpy()), 1.0)
            np.add.at(year_sum, (cell, single["year"].to_numpy(), 0), 1.0)
            np.add.at(year_sum, (cell, single["year"].to_numpy(), 1), single["value"].to_numpy())

        frame["rule"] = np.array(common.RULE_NAMES)[frame["rule_index"].to_numpy()]
        frame["absolute_label"] = absolute_labels[frame["absolute_bin"].to_numpy()]
        frame["regime"] = np.array(REGIME_NAMES)[frame["regime_index"].to_numpy()]
        table, summary = round5_test.family_table(family, month_sum, month_n, year_sum, frame, years, signs,
                                                  ["family", "rule", "absolute_bin", "regime_index"])
        tables.append(table)
        summaries[family] = summary

    cross = pd.concat(tables, ignore_index=True)
    common.write_csv(OUTPUT / "cross_table.csv", cross)
    judgement = cross.groupby("family").agg(cells_fixed=("n", "size"), cells_with_trades=("n", lambda values: int((values > 0).sum())),
                                            over_bar_a=("over_bar_a", "sum"), over_bar_b=("over_bar_b", "sum"),
                                            passes=("passes", "sum"), passes_one_bar_only=("passes_one_bar_only", "sum"),
                                            best_t=("t_excess", "max")).reset_index()
    common.write_csv(OUTPUT / "cross_judgement.csv", judgement)
    common.write_json(OUTPUT / "multiple_comparison.json", {"families": summaries, "cells_fixed_per_family": 300,
                                                            "difference_bar_pooled_10_tests": DIFFERENCE_BAR})
    timings["cross"] = time.time() - step

    # 1') 1분위 대 나머지, 같은 달·같은 절대 구간(·같은 국면)
    difference_rows = []

    def demeaned_test(target: pd.DataFrame, control: pd.DataFrame, by: list[str]) -> dict:
        control_mean = control.groupby(by)["value"].mean().rename("control")
        joined = target.join(control_mean, on=by, how="left")
        kept = joined[np.isfinite(joined["control"])]
        difference = kept["value"].to_numpy() - kept["control"].to_numpy()
        return {"n_target": int(len(target)), "n_target_with_control": int(len(kept)), "n_control": int(len(control)),
                "mean_target": float(target["value"].mean()) if len(target) else None,
                "mean_control": float(control["value"].mean()) if len(control) else None,
                "mean_difference_same_month": float(difference.mean()) if len(difference) else None,
                "t_difference_same_month": round6_test.month_cluster_t(difference, kept["month"].to_numpy())}

    for family in ("X_full", "X_exp"):
        for rule in RULES:
            single = singles[(family, rule)].copy()
            single["decile"] = (single["slot"] % SLOT_PER_ABSOLUTE) // 3
            single["regime_index"] = single["slot"] % 3

            for absolute_bin in ABSOLUTE_USED:
                part = single[single["absolute"] == absolute_bin]
                target = part[part["decile"] == 1]
                control = part[part["decile"] != 1]
                result = demeaned_test(target, control, ["month"])
                difference_rows.append({"family": family, "rule": common.RULE_NAMES[rule], "absolute_bin": absolute_labels[absolute_bin],
                                        "regime": "합침", "pooled_test": True,
                                        "passes_bar_3_0": bool(np.nan_to_num(result["t_difference_same_month"], nan=-np.inf) > DIFFERENCE_BAR),
                                        **result})

                for regime in range(3):
                    regime_part = part[part["regime_index"] == regime]
                    result = demeaned_test(regime_part[regime_part["decile"] == 1], regime_part[regime_part["decile"] != 1], ["month"])
                    difference_rows.append({"family": family, "rule": common.RULE_NAMES[rule], "absolute_bin": absolute_labels[absolute_bin],
                                            "regime": REGIME_NAMES[regime], "pooled_test": False, "passes_bar_3_0": None, **result})

    common.write_csv(OUTPUT / "decile1_difference.csv", pd.DataFrame(difference_rows))

    # 2) 같은 달·같은 절대 구간 대조(10분위 판)
    control_rows = []

    for rule in RULES:
        single = singles[("decile", rule)]
        target = single[single["slot"] == 1]
        for version, control in (("A 1분위 포함", single), ("B 1분위 제외", single[single["slot"] != 1])):
            result = demeaned_test(target, control, ["month", "absolute"])
            t_plain = round6_test.month_cluster_t(target["value"].to_numpy(), target["month"].to_numpy())
            control_rows.append({"cell": f"10분위 판 {common.RULE_NAMES[rule]} 1분위 100만 원·상한 0.5%", "control_version": version,
                                 "control_definition": "같은 달·같은 절대 구간·같은 규칙, 10분위 판 Q1 매매(하한 2억 같음)",
                                 "t_target_alone": t_plain, **result})
            by_absolute = target.join(control.groupby(["month", "absolute"])["value"].mean().rename("control"), on=["month", "absolute"])

            for absolute_bin, group in by_absolute.groupby("absolute"):
                group = group[np.isfinite(group["control"])]
                difference = group["value"].to_numpy() - group["control"].to_numpy()
                control_rows.append({"cell": f"  └ 절대 구간 {absolute_labels[absolute_bin]}", "control_version": version,
                                     "n_target_with_control": int(len(group)), "mean_target": float(group["value"].mean()),
                                     "mean_difference_same_month": float(difference.mean()) if len(difference) else None,
                                     "t_difference_same_month": round6_test.month_cluster_t(difference, group["month"].to_numpy())})

    common.write_csv(OUTPUT / "same_month_absolute_control.csv", pd.DataFrame(control_rows))

    # 3) 청산 격자
    step = time.time()
    grid_rows, grid_checks = [], []
    variant_names = ([f"E3 확인 {days}일" for days in CONFIRM_DAYS] + [f"E4j{days}" for days in BELOW_DAYS]
                     + [f"기본 + 트레일링 {percent:g}%" for percent in TRAILING_PERCENTS])

    for rule in RULES:
        slices = decile_slices[rule]
        stocks = slices["stock"].to_numpy()
        rows = slices["row"].to_numpy().astype(np.int64)
        exit_rows, exit_prices, triggered = exit_variants(
            rows, panel.starts[stocks].astype(np.int64), panel.starts[stocks + 1].astype(np.int64),
            slices["exit_row"].to_numpy().astype(np.int64), slices["exit_price"].to_numpy(),
            slices["window_index"].to_numpy().astype(np.int64), slices["span_index"].to_numpy().astype(np.int64), panel.averages,
            panel.open_price, panel.high, panel.low, panel.close, states_fixed, np.array(CONFIRM_DAYS, np.int64),
            np.array(BELOW_DAYS, np.int64), np.array(TRAILING_PERCENTS))
        base_name = "E3 확인 1일" if rule == 2 else "E4j5"
        base_column = variant_names.index(base_name)
        grid_checks.append({"rule": common.RULE_NAMES[rule], "base_variant": base_name,
                            "exit_row_mismatch": int((exit_rows[:, base_column] != slices["exit_row"].to_numpy()).sum()),
                            "exit_price_max_abs_difference": float(np.max(np.abs(exit_prices[:, base_column] - slices["exit_price"].to_numpy())))})

        for column, name in enumerate(variant_names):
            values = excess_of(panel, previous_20d, rows, exit_rows[:, column], exit_prices[:, column])
            frame = pd.DataFrame({"stock": stocks, "row": rows, "value": values, "hold": exit_rows[:, column] - rows,
                                  "trailing": triggered[:, column] if name.startswith("기본") else False})
            single = frame.groupby(["stock", "row"], sort=True)[["value", "hold", "trailing"]].mean().reset_index()
            months = panel.month_index[single["row"].to_numpy()]
            by_year = single.groupby(panel.years[single["row"].to_numpy()])["value"].mean()
            grid_rows.append({"entry_set": f"10분위 판 {common.RULE_NAMES[rule]} 1분위 매수", "variant": name,
                              "is_base": column == base_column, "n": int(len(single)), "mean_excess": float(single["value"].mean()),
                              "t_excess": round6_test.month_cluster_t(single["value"].to_numpy(), months),
                              "median_excess": float(single["value"].median()), "win_rate": float((single["value"] > 0).mean()),
                              "mean_hold_days": float(single["hold"].mean()), "median_hold_days": float(single["hold"].median()),
                              "share_trailing_exit": float(single["trailing"].mean()) if name.startswith("기본") else None,
                              "share_positive_excess_years": float((by_year > 0).mean())})

    common.write_csv(OUTPUT / "exit_grid.csv", pd.DataFrame(grid_rows))
    timings["exit_grid"] = time.time() - step

    # 4) 선택 기록
    common.write_csv(OUTPUT / "choices_log.csv", pd.DataFrame(CHOICES, columns=["round", "change", "decided_by", "chosen_after_seeing_results",
                                                                              "comparisons_added", "note"]))

    # 남는 칸 매매 목록(6회차 열 그대로)
    step = time.time()
    listings = []
    passing = cross[cross["passes"]]

    for _, cell in passing.iterrows():
        scheme = cell["family"]
        rule = int(cell["rule_index"])
        slot_value = int(cell["absolute_bin"]) * SLOT_PER_ABSOLUTE + int(cell["turnover_decile"]) * 3 + int(cell["regime_index"])
        slot, slot_count = slot_schemes[scheme]
        slices = emit_frame(panel, rule, slot, slot_count, previous_20d, chosen, states_fixed, latest_lows)
        slices = slices[slices["slot"] == slot_value].copy()
        slices["excess_c0_3"] = excess_of(panel, previous_20d, slices["row"].to_numpy(), slices["exit_row"].to_numpy(),
                                          slices["exit_price"].to_numpy())
        label = {"cell_role": "7회차 교차표 통과 칸(확인용)", "scheme": scheme, "rule": cell["rule"],
                 "turnover_bin": f"{cell['absolute_label']}|{int(cell['turnover_decile'])}분위|{cell['regime']}"}
        listings.append(build_listing(panel, previous_20d, slices, pivots, mask, span_values, label))

    for rule in RULES:
        slices = decile_slices[rule]
        label = {"cell_role": "6회차 10분위 판 남는 칸(7회차 재계산)", "scheme": "decile", "rule": common.RULE_NAMES[rule],
                 "turnover_bin": "1"}
        listings.append(build_listing(panel, previous_20d, slices, pivots, mask, span_values, label))

    common.write_csv(OUTPUT / "survivors_trades.csv", pd.concat(listings, ignore_index=True))
    timings["listing"] = time.time() - step
    timings["total"] = time.time() - started
    common.write_json(OUTPUT / "round7_metrics.json", {
        "reproduction": panel.reproduction(), "timings_seconds": timings, "first_market_date": int(panel.market_dates[0]),
        "round6_reproduction": reproduction, "exit_grid_base_checks": grid_checks,
        "regime_full_edges_krw": full_edges.tolist(),
        "regime_expanding_edges_krw_at": {str(int(panel.market_dates[index])): expanding_edges[index].tolist()
                                          for index in (panel.signal_start, panel.signal_start + (panel.market_count - panel.signal_start) // 2,
                                                        panel.market_count - 1)},
        "passing_cells": passing[["family", "rule", "absolute_label", "turnover_decile", "regime", "n", "mean_excess", "t_excess",
                                  "months_with_trades", "share_positive_excess_years"]].to_dict("records"),
        "confirmation_only": "6회차는 5회차 결과를 보고, 7회차 칸은 6회차 결과를 보고 골랐다 — 판정은 확인용",
        "hand_set": {"absolute_bins_used": "하한 2억 때문에 <1억·1–2억은 구조상 빈다 → 5 구간(내가 정함 — 구조상)",
                     "expanding_minimum_days": f"{EXPANDING_MINIMUM_DAYS}(내가 정함)",
                     "expanding_quantile": "전날까지 정렬 값의 (n−1)/3·2(n−1)/3 번째(아래쪽 값) — 내가 정함",
                     "difference_bar": "국면 합친 10 검정 t 3.0(헌장 20 칸 기준) — 내가 정함",
                     "difference_control": "같은 달·같은 규칙·같은 절대 구간(·같은 국면)의 2–10분위 매매 평균, 대조 없는 달의 1분위 매매는 뺌 — 내가 정함",
                     "exit_grid_entries_fixed": "매수 집합 고정, 보유 중 무시 다시 안 함 — 내가 정함",
                     "trailing_peak_start": "매수 종가 — 내가 정함", "grids": "E3 확인 1–3, E4j 1–10, 트레일링 10/15/20% — 지시자가 정함"}})
    print(f"전체 {timings['total']:.0f}초", flush=True)
    print(judgement.to_string(), flush=True)


if __name__ == "__main__":
    main()
