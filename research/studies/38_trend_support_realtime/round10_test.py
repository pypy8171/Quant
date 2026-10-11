"""스터디 38 10회차 — 2–5억·5–10억 균등 바구니가 소형주 효과인지, 겹침 유지로 비용을 줄이면 어떤지.

사용자 원래 질문(추세가 꺾인 자리에서 파는 손절 자리)과 다른 갈래다. 9회차 통과 2 칸(100만·2–5억·보유 20/60)을 이어 본다(지시 2026-10-11).

시작 전에 고정한 칸: 보유 {20, 60} 시장일 × 구간 {2–5억, 5–10억} × 금액 100만 × 판정 값 3 = 12 칸(한 판으로 기준선).
  판정 값 ①가 시가총액 대조: 바구니 순수익 − 같은 날 같은 시가총액 10분위(그날 거래된 보통주 전부 균등, COST, 충격 없음)
      평균을 바구니 종목마다 맞춰 뺀 값. 시가총액 = 그날 무수정 종가 × 상장주식수(shares_point_in_time, 그날 이전 최근값).
      상장주식수는 data.go.kr 2020-01-02 부터 등급 A, DART 사업보고서 2016–2019 등급 B, 2015 이전 없음
      → 판정은 매수일 2020-01-02 이후만(지시). 2016–2019 는 견줌으로만 낸다.
  판정 값 ①나 가격·변동성 대조(전 기간): 같은 날 무수정 종가 5분위 × 전날까지 60일 일간 로그수익 표준편차 5분위
      (25 묶음, 40일 이상 있어야 함 — 내가 정함) 평균을 종목마다 맞춰 뺀 값.
  판정 값 ② 겹침 유지: 직전 리밸런스에도 있던 종목은 사지 않고, 다음 리밸런스에도 남는 종목은 팔지 않는다. 사는 편 수수료·충격,
      파는 편 수수료·세금·충격을 그 편이 실제로 일어날 때만 매긴다. 보유 비중은 리밸런스마다 균등으로 되돌린다고 보고 그 조정 매매
      비용은 세지 않는다(내가 정함 — 조금 낙관). 값 = 바구니 순수익 − 시장 B(9회차와 같음).
  나머지 9회차와 같음: 리밸런스일 종가 매수, 충격 계수 0.3, 상한 0.5%, 2010-01-04 부터 보유 일수 간격, 끝 넘는 보유 뺌.
  판정: 월 묶음 t 가 12 칸 기준선((가)·(나) 둘 다) 이상, 옆 구간(같은 보유·같은 판정 값의 2–5억 ↔ 5–10억) 부호 같음,
  양수 연도 ≥ 0.7, 평균 > 0. 뒤집기 1,000 회, seed 20261010.

진단(판정에 안 씀): 9회차 값(전부 다시 삼) 재현 대조, 겹침 유지 회전율·주문 수, 시가총액 자료 해마다 덮는 비율,
  2025–2026 약세 표(구간별 평균·t, 바구니와 시장 B 나눠 보기, 같은 길이 이동 창 안에서 최근 창 순위).

산출(results/round10/): periods.csv, cell_table.csv, multiple_comparison.json, turnover.csv, coverage.csv, recent_table.csv,
  choices_log.csv(1–10회차), diagnostics.csv, round10_metrics.json
재실행: py research/studies/38_trend_support_realtime/round10_test.py
"""
from __future__ import annotations

import time

import common     # numba 보다 먼저: common 이 NUMBA_NUM_THREADS(12)를 넣는다
import numpy as np
import pandas as pd
from numba import njit

import round3_test
import round4_test
import round5_test
import round6_test

OUTPUT = common.STUDY / "results" / "round10"
AMOUNT = 1_000_000.0
HOLDS = (20, 60)
BINS = (2, 3)
BIN_LOWS = {2: 2e8, 3: 5e8}
BIN_HIGHS = {2: 5e8, 3: 1e9}
BIN_LABELS = round6_test.ABSOLUTE_LABELS
CAP_PERCENT = 0.5
COEFFICIENT = 0.3
COMMISSION = 0.00015      # PYQuant/backtest/costs.py LIVE 와 같음
SELL_TAX = 0.0020
BUY_FACTOR = 1.0 / (1.0 + COMMISSION)
SELL_FACTOR = common.COST / BUY_FACTOR      # 곱하면 왕복 COST 와 같다
CONTROLS = ("①가 시가총액 대조(2020–)", "①나 가격·변동성 대조", "② 겹침 유지 − B")
CAP_FROM = 20200102
VOLATILITY_DAYS = 60
VOLATILITY_MIN = 40
RECENT_FROM = 20250101
SPANS = ((2010, 2019), (2020, 2022), (2023, 2024), (2025, 2026))


@njit(cache=True)
def past_volatility(close, starts, window, minimum):
    """행마다 전날까지 window 개 일간 로그수익 표준편차(같은 종목 안). 모자라면 NaN."""
    result = np.full(close.shape[0], np.nan)

    for stock in range(starts.shape[0] - 1):
        first = starts[stock]
        last = starts[stock + 1]

        for row in range(first + 2, last):
            begin = max(first + 1, row - window)
            total = 0.0
            total2 = 0.0
            count = 0

            for inner in range(begin, row):
                if close[inner] > 0 and close[inner - 1] > 0:
                    value = np.log(close[inner] / close[inner - 1])
                    total += value
                    total2 += value * value
                    count += 1

            if count >= minimum:
                mean = total / count
                result[row] = np.sqrt(max(total2 / count - mean * mean, 0.0) * count / (count - 1))

    return result


def quantile_of(values: np.ndarray, valid: np.ndarray, buckets: int) -> np.ndarray:
    """그날 유효 종목 안 순위 분위(0 = 가장 작음), 없으면 −1."""
    result = np.full(values.shape[0], -1, dtype=np.int64)
    index = np.flatnonzero(valid)

    if len(index) == 0:
        return result

    rank = pd.Series(values[index]).rank(method="first").to_numpy()
    result[index] = np.minimum((rank - 1) * buckets // len(index), buckets - 1).astype(np.int64)
    return result


def matched_excess(member: np.ndarray, member_net: np.ndarray, plain_net: np.ndarray, alive: np.ndarray, group: np.ndarray):
    """바구니 종목(그룹 있는 것만) 순수익 평균 − 같은 그룹 균등 순수익 평균을 종목마다 맞춘 평균(%)."""
    known = member & (group >= 0)

    if known.sum() == 0:
        return np.nan, 0

    pool = alive & (group >= 0)
    group_count = np.bincount(group[pool], minlength=group.max() + 1)
    group_sum = np.bincount(group[pool], weights=plain_net[pool], minlength=group.max() + 1)
    group_mean = group_sum / np.maximum(group_count, 1)
    return float(member_net[known].mean() - group_mean[group[known]].mean()), int(known.sum())


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    panel = common.Panel()
    round3_test.apply_corrections(panel)
    panel.build_benchmark()
    panel.build_deciles()      # panel.market_cap 을 채운다
    previous_20d = round4_test.shift_within_stock(panel.turnover20, panel.starts)
    volatility = past_volatility(panel.close, panel.starts, VOLATILITY_DAYS, VOLATILITY_MIN)
    stock_count = panel.stock_count
    stock_of_row = np.repeat(np.arange(stock_count), np.diff(panel.starts))

    def matrix(values: np.ndarray) -> np.ndarray:
        result = np.full((panel.market_count, stock_count), np.nan)
        result[panel.market_position, stock_of_row] = values
        return result

    close_raw = matrix(panel.close)
    alive_matrix = np.isfinite(close_raw)
    close_matrix = pd.DataFrame(close_raw).ffill().to_numpy()
    turnover_matrix = matrix(previous_20d)
    cap_matrix = matrix(panel.market_cap)
    price_matrix = matrix(panel.raw_close)
    volatility_matrix = matrix(volatility)
    print(f"준비 끝 {time.time() - started:.0f}초", flush=True)

    month_count = panel.month_count
    years = list(range(panel.first_year, panel.first_year + panel.year_count))
    period_rows, coverage_rows = [], []
    minimum = AMOUNT / (CAP_PERCENT / 100.0)

    for hold in HOLDS:
        days = np.arange(panel.signal_start, panel.market_count - hold, hold)
        exits = days + hold
        market = panel.bench[days - panel.signal_start, exits - panel.signal_start]
        dates = panel.market_dates[days]

        for absolute_bin in BINS:
            low = max(BIN_LOWS[absolute_bin], minimum)
            high = BIN_HIGHS[absolute_bin]
            membership = []

            for day, exit_day in zip(days, exits):
                turnover = turnover_matrix[day]
                buy = close_matrix[day]
                sell = close_matrix[exit_day]
                known = np.nan_to_num(turnover, nan=-1.0)
                membership.append((known >= low) & (known < high) & (np.nan_to_num(buy) > 0) & (np.nan_to_num(sell) > 0))

            for position, (day, exit_day) in enumerate(zip(days, exits)):
                member = membership[position]
                alive = alive_matrix[day] & (np.nan_to_num(close_matrix[exit_day]) > 0)
                buy = close_matrix[day]
                sell = close_matrix[exit_day]
                ratio = np.where(alive, sell / np.where(alive, buy, 1.0), 0.0)
                plain_net = (ratio * common.COST - 1.0) * 100.0
                turnover = turnover_matrix[day]
                buy_ratio = np.where(member, AMOUNT / np.where(member, turnover, 1.0), 0.0)
                sell_base = turnover_matrix[exit_day]
                sell_ok = np.isfinite(sell_base) & (np.nan_to_num(sell_base) > 0)
                sell_ratio = np.where(sell_ok, AMOUNT / np.where(sell_ok, sell_base, 1.0), buy_ratio)
                buy_impact = 1.0 / (1.0 + COEFFICIENT * buy_ratio)
                sell_impact = np.maximum(0.0, 1.0 - COEFFICIENT * sell_ratio)
                member_net = (ratio * common.COST * buy_impact * sell_impact - 1.0) * 100.0
                count = int(member.sum())

                if count == 0:
                    continue

                rebuy_net = float(member_net[member].mean())
                # ② 겹침 유지
                held_before = membership[position - 1] if position > 0 else np.zeros(stock_count, dtype=bool)
                held_after = membership[position + 1] if position + 1 < len(days) else np.zeros(stock_count, dtype=bool)
                entering = member & ~held_before
                leaving = member & ~held_after
                factor = (np.where(entering, BUY_FACTOR * buy_impact, 1.0) * np.where(leaving, SELL_FACTOR * sell_impact, 1.0))
                keep_net = float(((ratio * factor)[member].mean() - 1.0) * 100.0)
                # ①가 시가총액 10분위, ①나 가격 5분위 × 변동성 5분위
                market_capitalization = cap_matrix[day]
                cap_valid = alive & np.isfinite(market_capitalization) & (np.nan_to_num(market_capitalization) > 0)
                cap_group = quantile_of(market_capitalization, cap_valid, 10)
                cap_value, cap_known = matched_excess(member, member_net, plain_net, alive, cap_group)
                price = price_matrix[day]
                day_volatility = volatility_matrix[day]
                style_valid = alive & np.isfinite(price) & np.isfinite(day_volatility)
                price_group = quantile_of(price, style_valid, 5)
                volatility_group = quantile_of(day_volatility, style_valid, 5)
                style_group = np.where(style_valid, price_group * 5 + volatility_group, -1)
                style_value, style_known = matched_excess(member, member_net, plain_net, alive, style_group)
                common_row = {"hold_days": hold, "order_amount_krw": AMOUNT, "absolute_bin": absolute_bin,
                              "absolute_label": BIN_LABELS[absolute_bin], "buy_date": int(dates[position]),
                              "sell_date": int(panel.market_dates[exit_day]), "year": int(dates[position] // 10000),
                              "month": int((dates[position] // 10000 - panel.first_year) * 12 + (dates[position] // 100) % 100 - 1),
                              "stocks": count, "market_b_percent": float(market[position])}
                period_rows.append({**common_row, "control": "9회차 재현(전부 다시 삼) − B", "basket_net_percent": rebuy_net,
                                    "value": rebuy_net - market[position], "stocks_used": count, "entering": count, "leaving": count})
                period_rows.append({**common_row, "control": CONTROLS[2], "basket_net_percent": keep_net,
                                    "value": keep_net - market[position], "stocks_used": count,
                                    "entering": int(entering.sum()), "leaving": int(leaving.sum())})
                period_rows.append({**common_row, "control": CONTROLS[0], "basket_net_percent": rebuy_net, "value": cap_value,
                                    "stocks_used": cap_known, "entering": count, "leaving": count})
                period_rows.append({**common_row, "control": CONTROLS[1], "basket_net_percent": rebuy_net, "value": style_value,
                                    "stocks_used": style_known, "entering": count, "leaving": count})
                coverage_rows.append({"hold_days": hold, "absolute_label": BIN_LABELS[absolute_bin], "year": int(dates[position] // 10000),
                                      "members": count, "members_with_cap": cap_known, "members_with_style": style_known,
                                      "market_alive": int(alive.sum()), "market_with_cap": int(cap_valid.sum())})

        print(f"보유 {hold} 끝 {time.time() - started:.0f}초", flush=True)

    periods = pd.DataFrame(period_rows)
    periods = periods[np.isfinite(periods["value"])]
    common.write_csv(OUTPUT / "periods.csv", periods)

    # 판정: 12 칸 한 판
    keys = [(hold, control, absolute_bin) for hold in HOLDS for control in CONTROLS for absolute_bin in BINS]
    cell_of = {key: index for index, key in enumerate(keys)}
    judged = periods[periods["control"].isin(CONTROLS)]
    judged = judged[(judged["control"] != CONTROLS[0]) | (judged["buy_date"] >= CAP_FROM)]
    cell = np.array([cell_of[key] for key in zip(judged["hold_days"], judged["control"], judged["absolute_bin"])])
    month_sum = np.zeros((len(keys), month_count))
    month_n = np.zeros((len(keys), month_count))
    year_sum = np.zeros((len(keys), panel.year_count, 2))
    np.add.at(month_sum, (cell, judged["month"].to_numpy()), judged["value"].to_numpy())
    np.add.at(month_n, (cell, judged["month"].to_numpy()), 1.0)
    year_index = judged["year"].to_numpy() - panel.first_year
    np.add.at(year_sum, (cell, year_index, 0), 1.0)
    np.add.at(year_sum, (cell, year_index, 1), judged["value"].to_numpy())
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    base_frame = pd.DataFrame(keys, columns=["hold_days", "control", "turnover_decile"])
    base_frame["order_amount_krw"] = AMOUNT
    base_frame["absolute_label"] = np.array(BIN_LABELS)[base_frame["turnover_decile"].to_numpy()]
    table, summary = round5_test.family_table("10회차 12 칸", month_sum, month_n, year_sum, base_frame, years, signs,
                                              ["family", "hold_days", "control"])
    table = table.rename(columns={"turnover_decile": "absolute_bin", "n": "rebalances"})
    common.write_csv(OUTPUT / "cell_table.csv", table)
    common.write_json(OUTPUT / "multiple_comparison.json", {"cells_fixed": len(keys), "family": summary})

    # 진단: 9회차 재현·2016–2019 시가총액(등급 B)
    diagnostics = []

    for (hold, control, absolute_bin), part in periods.groupby(["hold_days", "control", "absolute_bin"]):
        for name, piece in (("전 기간", part), ("2016–2019(등급 B)", part[(part["year"] >= 2016) & (part["year"] <= 2019)]),
                            ("2020–2026", part[part["buy_date"] >= CAP_FROM])):
            if len(piece) < 3:
                continue

            diagnostics.append({"hold_days": hold, "control": control, "absolute_label": BIN_LABELS[absolute_bin], "span": name,
                                "rebalances": len(piece), "mean_value": float(piece["value"].mean()),
                                "t_value": round6_test.month_cluster_t(piece["value"].to_numpy(), piece["month"].to_numpy()),
                                "mean_stocks_used": float(piece["stocks_used"].mean())})

    diagnostic_frame = pd.DataFrame(diagnostics)

    # 회전율·주문 수(② 와 전부 다시 삼)
    turnover_rows = []

    for (hold, control, absolute_bin), part in periods[periods["control"].isin(["9회차 재현(전부 다시 삼) − B", CONTROLS[2]])].groupby(
            ["hold_days", "control", "absolute_bin"]):
        turnover_rows.append({"hold_days": hold, "control": control, "absolute_label": BIN_LABELS[absolute_bin], "rebalances": len(part),
                              "stocks_median": float(part["stocks"].median()),
                              "entering_share_median": float((part["entering"] / part["stocks"]).median()),
                              "leaving_share_median": float((part["leaving"] / part["stocks"]).median()),
                              "orders_per_rebalance_median": float((part["entering"] + part["leaving"]).median()),
                              "total_orders": int((part["entering"] + part["leaving"]).sum()),
                              "mean_basket_net_percent": float(part["basket_net_percent"].mean()),
                              "mean_value": float(part["value"].mean()),
                              "t_value": round6_test.month_cluster_t(part["value"].to_numpy(), part["month"].to_numpy())})

    common.write_csv(OUTPUT / "turnover.csv", pd.DataFrame(turnover_rows))
    common.write_csv(OUTPUT / "coverage.csv", pd.DataFrame(coverage_rows).groupby(["hold_days", "absolute_label", "year"]).agg(
        rebalances=("members", "size"), members_mean=("members", "mean"), members_with_cap_mean=("members_with_cap", "mean"),
        members_with_style_mean=("members_with_style", "mean"), market_alive_mean=("market_alive", "mean"),
        market_with_cap_mean=("market_with_cap", "mean")).reset_index())

    # 2025–2026 약세 표
    recent_rows = []

    for (hold, control, absolute_bin), part in periods.groupby(["hold_days", "control", "absolute_bin"]):
        row = {"hold_days": hold, "control": control, "absolute_label": BIN_LABELS[absolute_bin]}

        for first, last in SPANS:
            piece = part[(part["year"] >= first) & (part["year"] <= last)]
            label = f"{first}–{last}"
            row[f"n_{label}"] = len(piece)
            row[f"mean_{label}"] = float(piece["value"].mean()) if len(piece) else None
            row[f"t_{label}"] = round6_test.month_cluster_t(piece["value"].to_numpy(), piece["month"].to_numpy()) if len(piece) else None

        recent = part[part["buy_date"] >= RECENT_FROM]
        before = part[part["buy_date"] < RECENT_FROM]
        row["recent_basket_net_mean"] = float(recent["basket_net_percent"].mean())
        row["recent_market_b_mean"] = float(recent["market_b_percent"].mean())
        row["before_basket_net_mean"] = float(before["basket_net_percent"].mean())
        row["before_market_b_mean"] = float(before["market_b_percent"].mean())
        # 같은 길이(리밸런스 수) 이동 창 평균 가운데 최근 창의 순위(0 = 가장 나쁨)
        values = part.sort_values("buy_date")["value"].to_numpy()
        width = len(recent)

        if width >= 2 and len(values) > width:
            windows = np.convolve(values, np.ones(width) / width, mode="valid")
            row["recent_window_mean"] = float(windows[-1])
            row["recent_window_percentile"] = float((windows < windows[-1]).mean())
            row["windows"] = len(windows)

        recent_rows.append(row)

    common.write_csv(OUTPUT / "recent_table.csv", pd.DataFrame(recent_rows))
    common.write_csv(OUTPUT / "diagnostics.csv", diagnostic_frame)

    history = pd.read_csv(common.STUDY / "results" / "round9" / "choices_log.csv")
    new_rows = pd.DataFrame([
        (10, "보유 {20,60} × 구간 {2–5억,5–10억} × 100만 × 판정 값 3(시가총액 대조·가격 변동성 대조·겹침 유지)", "지시(9회차 내 설계, ① 두 판은 지시)",
         "예", "칸 12", "9회차 통과 칸을 본 뒤 — 원래 질문과 다른 갈래"),
        (10, "시가총액 대조는 2020-01-02 이후만 판정(상장주식수 등급 A 시작), 2016–2019 등급 B 는 견줌", "지시", "예", "–", ""),
        (10, "가격 5분위 × 60일 변동성 5분위(40일 이상), 시가총액 10분위", "내가 정함", "아니오", "–", ""),
        (10, "겹침 유지에서 균등 비중 되돌리는 매매 비용 안 셈", "내가 정함", "아니오", "–", "조금 낙관"),
    ], columns=history.columns)
    common.write_csv(OUTPUT / "choices_log.csv", pd.concat([history, new_rows], ignore_index=True))
    common.write_json(OUTPUT / "round10_metrics.json", {
        "reproduction": panel.reproduction(), "seconds": time.time() - started, "cells_fixed": len(keys),
        "cost_split": {"buy_factor": BUY_FACTOR, "sell_factor": SELL_FACTOR, "roundtrip": common.COST},
        "passing_cells": table[table["passes"]][["hold_days", "control", "absolute_label", "rebalances", "mean_excess", "t_excess",
                                                 "share_positive_excess_years"]].to_dict("records"),
        "hand_set": {"style_groups": "무수정 종가 5분위 × 전날까지 60일 변동성 5분위(40일 이상) — 내가 정함",
                     "cap_groups": "시가총액 10분위 — 내가 정함",
                     "keep_weights": "겹침 유지에서 균등 비중 되돌리는 매매 비용 안 셈 — 내가 정함(조금 낙관)"}})
    print(f"전체 {time.time() - started:.0f}초, 통과 {int(table['passes'].sum())} 칸", flush=True)


if __name__ == "__main__":
    main()
