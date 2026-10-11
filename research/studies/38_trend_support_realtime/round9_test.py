"""스터디 38 9회차 — 거래대금 구간 균등 바구니(신호 없음) 시험.

사용자 원래 질문(추세가 꺾인 자리에서 파는 손절 자리)과 다른 갈래다. 8회차에서 지지선 추세 신호가 같은 날·같은 구간 균등 매수를
못 이겼고, 신호의 시장 대비 수익은 소형 거래대금 구간 노출이었다. 그 노출을 독립 후보로 따로 검정한다(지시 2026-10-11).

시작 전에 고정한 칸(지시): 절대 구간 7(전날까지 20일 평균 거래대금) × 보유 {5, 20, 60} 시장일 × 주문 금액 {100만, 500만, 2,000만} = 63 칸.
  리밸런스일 = 신호 기간 첫 시장일(2010-01-04)부터 보유 일수 간격(내가 정함 — 시작일). 마지막 보유가 자료 끝을 넘으면 뺀다(내가 정함).
  리밸런스일 종가에 구간 전 종목(하한 = 금액 ÷ 0.5% 같이 적용)을 종목마다 같은 금액으로 사서 보유 일수 뒤 종가에 판다.
  리밸런스마다 전부 팔고 다시 산다고 본다 — 겹치는 종목의 비용 절약을 세지 않는다(보수적, 내가 정함).
  충격 비용 계수 0.3(사는 날·파는 날 전날까지 20일 평균 거래대금 대비 금액 비율), 상한 0.5%, 왕복 비용 COST.
  파는 날 거래가 없으면(정지·상장폐지) 마지막 종가, 충격 비율은 사는 날 값(6–8회차 신호 매매와 같은 처리).
  판정 값 = 바구니 순수익 − 시장 비교 평균 B(보통주 전부 균등, COST 포함). 한 리밸런스 = 한 값, 매수일의 달로 묶는다.
  판정: 월 묶음 t 가 63 칸 기준선(5–8회차와 같은 (가) 24 개월 이상 칸 최대 t 95% 분위·(나) 자유도 보정, 둘 다) 이상,
  옆 구간(같은 보유·금액, 구간 ±1) 부호 같음, 양수 연도 ≥ 0.7, 평균 > 0. 뒤집기 1,000 회, seed 20261010. 2010–2026 한 번에.

진단(판정에 안 씀): 확장창 국면별, 연도별(2020–2022 몫 포함), 리밸런스마다 체결 가능 종목 수·총 주문 수.
생존 편향 점검(지시): 보정표에서 대용값(수정종가 × 거래량) 행 667,918 개가 섞여 있다(대부분 2020 이전 상장폐지 종목).
  판 A 전부 / 판 B 대용값 행은 그날 바구니에 안 넣음 / 판 C 대용값 행이 하나라도 있는 종목은 아예 뺌. B 는 세 판 모두 같다.
  판정은 판 A 로만, B·C 는 견줌 표.

산출(results/round9/): cell_table.csv, multiple_comparison.json, survivorship.csv, regime_table.csv, year_table.csv, capacity.csv,
  periods.csv(리밸런스마다 값), choices_log.csv(1–9회차), round9_metrics.json
재실행: py research/studies/38_trend_support_realtime/round9_test.py
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

OUTPUT = common.STUDY / "results" / "round9"
AMOUNTS = (1_000_000.0, 5_000_000.0, 20_000_000.0)   # 지시
HOLDS = (5, 20, 60)                                   # 지시
CAP_PERCENT = 0.5
COEFFICIENT = 0.3
BIN_LOWS = np.array([0.0, 1e8, 2e8, 5e8, 1e9, 3e9, 1e10])
BIN_HIGHS = np.array([1e8, 2e8, 5e8, 1e9, 3e9, 1e10, np.inf])
BIN_LABELS = round6_test.ABSOLUTE_LABELS
VARIANTS = ("A 전부", "B 대용값 행 뺌", "C 대용값 종목 뺌")


@njit(cache=True, parallel=True)
def basket_returns(days_in, days_out, lows, highs, amount, coefficient, close_matrix, member_matrix, turnover_matrix):
    """리밸런스마다 바구니 총수익(충격 배수 포함, 배)·종목 수."""
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
            turnover = member_matrix[day, stock]

            if turnover >= lows[index] and turnover < highs[index]:
                buy = close_matrix[day, stock]
                sell = close_matrix[exit_day, stock]

                if buy > 0 and sell > 0:
                    buy_ratio = amount / turnover
                    sell_base = turnover_matrix[exit_day, stock]
                    sell_ratio = amount / sell_base if (np.isfinite(sell_base) and sell_base > 0) else buy_ratio
                    multiplier = max(0.0, 1.0 - coefficient * sell_ratio) / (1.0 + coefficient * buy_ratio)
                    total += sell / buy * multiplier
                    number += 1

        if number > 0:
            result[index] = total / number

        members[index] = number

    return result, members


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    panel = common.Panel()
    round3_test.apply_corrections(panel)
    panel.build_benchmark()
    previous_20d = round4_test.shift_within_stock(panel.turnover20, panel.starts)
    status = pd.read_parquet(round3_test.CORRECTIONS, columns=["status"])["status"].astype(str).to_numpy()
    proxy_row = status == "proxy_adjusted_close_times_volume"
    stock_count = len(panel.starts) - 1
    stock_of_row = np.repeat(np.arange(stock_count), np.diff(panel.starts))
    close_matrix = np.full((panel.market_count, stock_count), np.nan)
    turnover_matrix = np.full((panel.market_count, stock_count), np.nan)
    close_matrix[panel.market_position, stock_of_row] = panel.close
    turnover_matrix[panel.market_position, stock_of_row] = previous_20d
    close_matrix = pd.DataFrame(close_matrix).ffill().to_numpy()
    member_b = turnover_matrix.copy()
    member_b[panel.market_position[proxy_row], stock_of_row[proxy_row]] = np.nan
    proxy_stocks = np.zeros(stock_count, dtype=bool)
    proxy_stocks[np.unique(stock_of_row[proxy_row])] = True
    member_c = turnover_matrix.copy()
    member_c[:, proxy_stocks] = np.nan
    members_by_variant = (turnover_matrix, member_b, member_c)
    market_total = np.zeros(panel.market_count)
    np.add.at(market_total, panel.market_position, np.nan_to_num(previous_20d))
    regime_day, _ = round7_test.expanding_regime(market_total)
    proxy_years = pd.Series((panel.dates[proxy_row] // 10000)).value_counts().sort_index()
    delisted = ~panel.listed_at_end
    print(f"준비 끝 {time.time() - started:.0f}초, 대용값 행 {int(proxy_row.sum()):,}, 대용값 종목 {int(proxy_stocks.sum())}"
          f"(그중 상장폐지 {int((proxy_stocks & delisted).sum())})", flush=True)

    month_count = panel.month_count
    year_count = panel.year_count
    years = list(range(panel.first_year, panel.first_year + year_count))
    generator = np.random.default_rng(common.SEED)
    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    keys = [(hold, amount, absolute_bin) for hold in HOLDS for amount in AMOUNTS for absolute_bin in range(7)]
    period_rows = []

    for variant_index, member_matrix in enumerate(members_by_variant):
        for hold in HOLDS:
            days = np.arange(panel.signal_start, panel.market_count - hold, hold)
            exits = days + hold
            market = panel.bench[days - panel.signal_start, exits - panel.signal_start]
            dates = panel.market_dates[days]
            months = ((dates // 10000 - panel.first_year) * 12 + (dates // 100) % 100 - 1).astype(np.int64)

            for amount in AMOUNTS:
                minimum = amount / (CAP_PERCENT / 100.0)

                for absolute_bin in range(7):
                    lows = np.full(len(days), max(BIN_LOWS[absolute_bin], minimum))
                    highs = np.full(len(days), BIN_HIGHS[absolute_bin])

                    if lows[0] >= highs[0]:
                        continue

                    gross, members = basket_returns(days, exits, lows, highs, amount, COEFFICIENT, close_matrix, member_matrix,
                                                    turnover_matrix)
                    value = (gross * common.COST - 1.0) * 100.0 - market
                    period_rows.append(pd.DataFrame({
                        "variant": VARIANTS[variant_index], "hold_days": hold, "order_amount_krw": amount, "absolute_bin": absolute_bin,
                        "absolute_label": BIN_LABELS[absolute_bin], "buy_date": dates, "sell_date": panel.market_dates[exits],
                        "month": months, "year": dates // 10000, "regime_expanding": regime_day[days], "stocks": members,
                        "basket_net_percent": (gross * common.COST - 1.0) * 100.0, "market_b_percent": market, "value": value}))

        print(f"판 {VARIANTS[variant_index]} 끝 {time.time() - started:.0f}초", flush=True)

    periods = pd.concat(period_rows, ignore_index=True)
    periods = periods[np.isfinite(periods["value"])]
    common.write_csv(OUTPUT / "periods.csv", periods)

    # 판정(판 A)
    def cell_arrays(frame: pd.DataFrame):
        cell_of = {key: index for index, key in enumerate(keys)}
        cell = np.array([cell_of[key] for key in zip(frame["hold_days"], frame["order_amount_krw"], frame["absolute_bin"])])
        month_sum = np.zeros((len(keys), month_count))
        month_n = np.zeros((len(keys), month_count))
        year_sum = np.zeros((len(keys), year_count, 2))
        np.add.at(month_sum, (cell, frame["month"].to_numpy()), frame["value"].to_numpy())
        np.add.at(month_n, (cell, frame["month"].to_numpy()), 1.0)
        year_index = frame["year"].to_numpy() - panel.first_year
        np.add.at(year_sum, (cell, year_index, 0), 1.0)
        np.add.at(year_sum, (cell, year_index, 1), frame["value"].to_numpy())
        return month_sum, month_n, year_sum

    base_frame = pd.DataFrame(keys, columns=["hold_days", "order_amount_krw", "turnover_decile"])
    base_frame["absolute_label"] = np.array(BIN_LABELS)[base_frame["turnover_decile"].to_numpy()]
    base_frame["min_turnover_krw"] = base_frame["order_amount_krw"] / (CAP_PERCENT / 100.0)
    tables, summaries = [], {}

    for variant in VARIANTS:
        month_sum, month_n, year_sum = cell_arrays(periods[periods["variant"] == variant])
        table, summary = round5_test.family_table(variant, month_sum, month_n, year_sum, base_frame, years, signs,
                                                  ["family", "hold_days", "order_amount_krw"])
        tables.append(table.rename(columns={"turnover_decile": "absolute_bin", "n": "rebalances"}))
        summaries[variant] = summary

    cells = pd.concat(tables, ignore_index=True)
    cells["judged"] = cells["family"] == VARIANTS[0]
    common.write_csv(OUTPUT / "cell_table.csv", cells)
    common.write_json(OUTPUT / "multiple_comparison.json", {"cells_fixed": len(keys), "judged_variant": VARIANTS[0], "families": summaries})

    # 생존 편향 견줌
    keys_columns = ["hold_days", "order_amount_krw", "absolute_bin", "absolute_label"]
    wide = cells.pivot_table(index=keys_columns, columns="family", values=["mean_excess", "t_excess", "rebalances"]).reset_index()
    wide.columns = [column if not isinstance(column, tuple) or column[1] == "" else f"{column[0]}|{column[1]}" for column in wide.columns]
    wide.columns = [column[0] if isinstance(column, tuple) else column for column in wide.columns]
    mean_stocks = periods.groupby(["variant"] + keys_columns)["stocks"].mean().unstack("variant").add_prefix("mean_stocks|").reset_index()
    survivorship = wide.merge(mean_stocks, on=keys_columns, how="left")
    survivorship["mean_change_C_minus_A"] = survivorship[f"mean_excess|{VARIANTS[2]}"] - survivorship[f"mean_excess|{VARIANTS[0]}"]
    survivorship["mean_change_B_minus_A"] = survivorship[f"mean_excess|{VARIANTS[1]}"] - survivorship[f"mean_excess|{VARIANTS[0]}"]
    common.write_csv(OUTPUT / "survivorship.csv", survivorship)

    # 진단: 국면·연도·체결 가능 종목 수
    judged = periods[periods["variant"] == VARIANTS[0]]
    regime_rows, year_rows, capacity_rows = [], [], []

    for key, part in judged.groupby(keys_columns):
        for regime in range(-1, 3):
            regime_part = part[part["regime_expanding"] == regime]
            regime_rows.append({**dict(zip(keys_columns, key)), "regime": "모름" if regime < 0 else round7_test.REGIME_NAMES[regime],
                                "rebalances": int(len(regime_part)),
                                "mean_value": float(regime_part["value"].mean()) if len(regime_part) else None,
                                "t_value": round6_test.month_cluster_t(regime_part["value"].to_numpy(), regime_part["month"].to_numpy())
                                if len(regime_part) else None})

        by_year = part.groupby("year")["value"].agg(["sum", "mean", "size"])
        total = by_year["sum"].sum()
        crisis = by_year.loc[by_year.index.isin([2020, 2021, 2022]), "sum"].sum()
        outside = part[~part["year"].isin([2020, 2021, 2022])]
        year_rows.append({**dict(zip(keys_columns, key)), "total_value_sum": float(total),
                          "share_of_value_sum_2020_2022": float(crisis / total) if total != 0 else None,
                          "share_of_rebalances_2020_2022": float(part["year"].isin([2020, 2021, 2022]).mean()),
                          "mean_value_outside_2020_2022": float(outside["value"].mean()),
                          "t_value_outside_2020_2022": round6_test.month_cluster_t(outside["value"].to_numpy(), outside["month"].to_numpy()),
                          **{f"mean_{year}": float(by_year.loc[year, "mean"]) if year in by_year.index else None for year in years}})
        capacity_rows.append({**dict(zip(keys_columns, key)), "rebalances": int(len(part)), "stocks_min": int(part["stocks"].min()),
                              "stocks_median": float(part["stocks"].median()), "stocks_max": int(part["stocks"].max()),
                              "orders_per_rebalance_median": float(part["stocks"].median() * 2),
                              "total_orders": int(part["stocks"].sum() * 2),
                              "capital_per_rebalance_median_krw": float(part["stocks"].median() * key[1])})

    common.write_csv(OUTPUT / "regime_table.csv", pd.DataFrame(regime_rows))
    common.write_csv(OUTPUT / "year_table.csv", pd.DataFrame(year_rows))
    common.write_csv(OUTPUT / "capacity.csv", pd.DataFrame(capacity_rows))

    history = pd.read_csv(common.STUDY / "results" / "round8" / "choices_log.csv")
    new_rows = pd.DataFrame([
        (9, "신호 없는 거래대금 구간 균등 바구니(구간 7 × 보유 3 × 금액 3)", "지시(8회차 내 설계)", "예", "칸 63", "8회차 시기 노출 결과를 본 뒤 — 원래 질문과 다른 갈래"),
        (9, "리밸런스 시작일 2010-01-04·전부 팔고 다시 삼·끝 넘는 보유 뺌", "내가 정함", "아니오", "–", ""),
        (9, "대용값 행·종목 뺀 판 B·C(생존 편향 견줌)", "지시", "아니오", "판 × 3(판정은 A)", ""),
    ], columns=history.columns)
    common.write_csv(OUTPUT / "choices_log.csv", pd.concat([history, new_rows], ignore_index=True))
    judged_cells = cells[cells["judged"]]
    common.write_json(OUTPUT / "round9_metrics.json", {
        "reproduction": panel.reproduction(), "seconds": time.time() - started, "cells_fixed": len(keys),
        "proxy_rows": int(proxy_row.sum()), "proxy_rows_by_year": {str(key): int(value) for key, value in proxy_years.items()},
        "proxy_stocks": int(proxy_stocks.sum()), "proxy_stocks_delisted": int((proxy_stocks & delisted).sum()),
        "passing_cells": judged_cells[judged_cells["passes"]][["hold_days", "order_amount_krw", "absolute_label", "rebalances", "mean_excess",
                                                               "t_excess", "share_positive_excess_years"]].to_dict("records"),
        "hand_set": {"start": "2010-01-04 부터 보유 일수 간격 — 내가 정함", "rebuy": "리밸런스마다 전부 팔고 다시 삼(겹침 비용 절약 안 셈) — 내가 정함",
                     "tail": "자료 끝을 넘는 보유 뺌 — 내가 정함"}})
    print(f"전체 {time.time() - started:.0f}초, 통과 {int(judged_cells['passes'].sum())} 칸", flush=True)


if __name__ == "__main__":
    main()
