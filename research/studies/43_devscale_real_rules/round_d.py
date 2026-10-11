"""스터디 43-D 실행 — 엔진 실제 설정 재현, D158 매매의 MAE/MFE, 익절% × 손절% 격자, 무작위 대조, 묶음 통과선.

시험 목록·판정 기준은 README.md "43-D" 절에 실행 전에 고정했다. 기간은 2010-01-04 – 2026-10-08 한 번에 돌린다.
실행: py -I -X utf8 research/studies/43_devscale_real_rules/round_d.py [--output <폴더>]
산출: results/round_d/*.csv · run_info.json · run_command.txt, 큰 parquet 은 results/round_d/trades/
재현 확인: 같은 입력으로 --output 을 바꿔 한 번 더 돌린 뒤 csv·parquet 을 바이트 단위로 비교한다(run_info.json 은 시각이 달라 뺀다).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import harness     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
import run_stages     # noqa: E402
from harness import study41     # noqa: E402

TAKE_PROFITS = (0.0, 3.0, 5.0, 8.0, 10.0, 15.0, 20.0, 30.0, 50.0)
STOP_LOSSES = (0.0, 3.0, 5.0, 6.5, 8.0, 10.0, 15.0, 20.0)
ENGINE_ACCOUNT = "실제 설정"
ACCOUNTS = (
    (ENGINE_ACCOUNT, {"mode": harness.MODE_ENGINE, "slots": harness.ENGINE_MAX_POSITIONS,
                      "amount_mode": harness.AMOUNT_ENGINE, "capital": harness.ENGINE_CAPITAL,
                      "gross_cap": harness.ENGINE_GROSS_CAP}),
    *((f"S{slots} {'고정' if amount_mode == harness.AMOUNT_FIXED else '복리'}",
       {"mode": harness.MODE_TURNOVER, "slots": slots, "amount_mode": amount_mode, "capital": 0.0, "gross_cap": 0.0})
      for slots in run_stages.SLOT_CHOICES for amount_mode in run_stages.AMOUNT_CHOICES))
ACCOUNT_NAMES = [name for name, _ in ACCOUNTS]
MAE_TARGETS_SLIPPAGE = 1
QUANTILES = (5, 10, 25, 50, 75, 90, 95)
MFE_LEVELS = (3, 5, 8, 10, 15, 20, 30, 50)
SENSITIVITY_TOP = 10
LOG_CAPACITY = 200_000
BATCH_LOGGED = 16
BATCH_PLAIN = 600
FIELD = run_stages.FIELD
SCALAR = run_stages.SCALAR
INDEX_NAMES = ("코스피", "코스닥")


def text(value: float) -> str:
    return "없음" if value == 0 else f"{value:g}%"


def grid_table() -> pd.DataFrame:
    """계좌 7 × 익절 9 × 손절 8 (D158, 1호가) + 실제 설정 OLD 1."""
    rows = []

    for account, setting in ACCOUNTS:
        for take_profit in TAKE_PROFITS:
            for stop_loss in STOP_LOSSES:
                rows.append({"account": account, **setting, "rule": harness.RULE_TREND, "take_profit": take_profit,
                             "stop_loss": stop_loss})

    rows.append({"account": ENGINE_ACCOUNT, **ACCOUNTS[0][1], "rule": harness.RULE_OLD, "take_profit": 0.0,
                 "stop_loss": 0.0})
    table = pd.DataFrame(rows)
    table["slippage"] = MAE_TARGETS_SLIPPAGE
    table["signal"] = -1
    table["quantile"] = 0.0
    table["seed"] = 0
    table["control"] = 0
    table["pair_row"] = 0
    table["log"] = 1
    table["fixed_amount"] = 0.0
    return table


def effective_capital(table: pd.DataFrame) -> np.ndarray:
    capital = table["capital"].to_numpy(np.float64)
    return np.where(capital > 0, capital, table["slots"].to_numpy(np.float64) * harness.SLOT_AMOUNT)


def run_plain(runner: run_stages.Runner, table: pd.DataFrame, target_buys: np.ndarray):
    parameters = run_stages.parameter_matrix(table)
    equity_all = np.zeros((len(table), runner.span))
    buys_all = np.zeros((len(table), runner.span), dtype=np.int32)
    scalars_all = np.zeros((len(table), len(harness.SCALAR_FIELDS)))

    for begin in range(0, len(table), BATCH_PLAIN):
        end = min(begin + BATCH_PLAIN, len(table))
        equity, buys, scalars, _, _ = runner.market.run(parameters[begin:end], target_buys, 0)
        equity_all[begin:end], buys_all[begin:end], scalars_all[begin:end] = equity, buys, scalars

    return equity_all, buys_all, scalars_all


def summarize(runner: run_stages.Runner, table: pd.DataFrame, equity: np.ndarray, scalars: np.ndarray,
              medians: np.ndarray) -> tuple[pd.DataFrame, np.ndarray, np.ndarray]:
    """run_stages.summary_rows 와 같은 열을 자본이 조합마다 다른 경우로 낸다."""
    capital = effective_capital(table)
    daily = run_stages.daily_returns(equity, capital)
    span_years = runner.span / study41.YEAR_DAYS
    yearly = runner.yearly_compound(daily)
    index_yearly = {name: runner.yearly_compound(returns[None, :])[0] for name, returns in runner.index_returns.items()}
    rows = []

    for row in range(len(table)):
        annual, drawdown, sharpe = study41.curve_statistics(daily[row])
        closed = scalars[row, SCALAR["closed_trades"]]
        buys = scalars[row, SCALAR["buys"]]
        entry = {"연 수익 %": annual, "MDD %": drawdown, "샤프": sharpe}

        for name, returns in runner.index_returns.items():
            entry[f"{name} 대비 연 초과 %p"] = float(study41.annual_mean((daily[row] - returns)[None, :])[0] * 100.0)
            entry[f"{name} 대비 플러스 해"] = int((yearly[row] > index_yearly[name]).sum())

        entry.update({
            "전체 해": len(runner.years),
            "매수 수": int(buys),
            "매수당 평균 금액(원)": scalars[row, SCALAR["spend_sum"]] / buys if buys else np.nan,
            "건당 순수익 평균 %": scalars[row, SCALAR["net_sum"]] / closed * 100.0 if closed else np.nan,
            "건당 순수익 중앙값 %": medians[row],
            "연 회전(매수액÷평균 평가금)": scalars[row, SCALAR["spend_sum"]] / equity[row].mean() / span_years,
            "평균 보유일": scalars[row, SCALAR["hold_day_sum"]] / closed if closed else np.nan,
            "평균 보유 종목 수": scalars[row, SCALAR["held_day_sum"]] / runner.span,
            "현금 부족 건너뜀": int(scalars[row, SCALAR["no_cash_skips"]]),
            "총노출 상한 거부": int(scalars[row, SCALAR["gross_skips"]]),
            "수수료 합(원)": scalars[row, SCALAR["commission"]],
            "매도세 합(원)": scalars[row, SCALAR["tax"]],
            "미끄러짐 합(원)": scalars[row, SCALAR["slippage"]],
            "시작 자본(원)": capital[row],
            "끝 평가금(원)": equity[row, -1],
            "기록 넘침": int(scalars[row, SCALAR["log_overflow"]])})
        rows.append(entry)

    return pd.DataFrame(rows), daily, yearly


def excess_of(runner: run_stages.Runner, daily: np.ndarray) -> np.ndarray:
    return np.concatenate([daily - runner.index_returns[name][None, :] for name in INDEX_NAMES])


def attach_t(frame: pd.DataFrame, t_month: np.ndarray, t_year: np.ndarray, count: int) -> list[str]:
    columns = []

    for number, name in enumerate(INDEX_NAMES):
        part = slice(number * count, (number + 1) * count)
        frame[f"{name} 월 t"] = t_month[part]
        frame[f"{name} 연 t"] = t_year[part]
        columns += [f"{name} 월 t", f"{name} 연 t"]

    frame["가장 낮은 t"] = frame[columns].min(axis=1)
    return columns


def path_statistics(market: harness.Market, log: pd.DataFrame) -> pd.DataFrame:
    """건마다 MAE·MFE·고점 전 MAE(매수가 대비 %).

    경로 = 매수일 종가, 매수 다음 날부터 매도 전날까지 고가·저가(시험 끝 보유는 마지막 날 포함), 매도가.
    매수 당일 장중과 매도일 장중은 체결 전후를 일봉으로 가를 수 없어 뺀다(README 43-D).
    """
    high, low, close = market.high, market.low, market.close
    end_index = market.day_count - 1
    mae = np.empty(len(log))
    mfe = np.empty(len(log))
    before_peak = np.empty(len(log))
    peak_day = np.empty(len(log))
    stocks = log["stock"].to_numpy(np.int64)
    entries = log["entry_day"].to_numpy(np.int64)
    exits = log["exit_day"].to_numpy(np.int64)
    reasons = log["reason"].to_numpy(np.int64)
    entry_prices = log["entry_price"].to_numpy(np.float64)
    exit_prices = log["exit_price"].to_numpy(np.float64)

    for trade in range(len(log)):
        stock, entry, exit_day = stocks[trade], entries[trade], exits[trade]
        last = exit_day + 1 if reasons[trade] == harness.REASON_END else exit_day
        last = min(last, end_index + 1)
        highs = np.concatenate([[close[stock, entry]], high[stock, entry + 1:last], [exit_prices[trade]]])
        lows = np.concatenate([[close[stock, entry]], low[stock, entry + 1:last], [exit_prices[trade]]])
        highs = np.where(np.isfinite(highs), highs, np.nan)
        lows = np.where(np.isfinite(lows), lows, np.nan)
        peak = int(np.nanargmax(highs))
        mfe[trade] = np.nanmax(highs) / entry_prices[trade] - 1.0
        mae[trade] = np.nanmin(lows) / entry_prices[trade] - 1.0
        before_peak[trade] = np.nanmin(lows[:peak + 1]) / entry_prices[trade] - 1.0
        peak_day[trade] = peak

    return pd.DataFrame({"MAE %": mae * 100.0, "MFE %": mfe * 100.0, "고점 전 MAE %": before_peak * 100.0,
                         "고점까지 일수": peak_day,
                         "가격 수익 %": (exit_prices / entry_prices - 1.0) * 100.0,
                         "순수익 %": (log["proceeds"] / log["spend"] - 1.0).to_numpy() * 100.0})


def mae_tables(label: str, statistics: pd.DataFrame) -> tuple[list[dict], list[dict]]:
    quantile_rows = []
    groups = (("전체", statistics), ("이긴 매매(순수익 > 0)", statistics[statistics["순수익 %"] > 0]),
              ("진 매매(순수익 ≤ 0)", statistics[statistics["순수익 %"] <= 0]))

    for group_name, group in groups:
        for measure in ("MAE %", "MFE %", "고점 전 MAE %", "가격 수익 %", "순수익 %", "고점까지 일수"):
            values = group[measure].to_numpy()
            row = {"대상": label, "묶음": group_name, "건수": len(values), "값": measure,
                   "평균": float(values.mean()) if len(values) else np.nan}

            for quantile in QUANTILES:
                row[f"{quantile}%"] = float(np.percentile(values, quantile)) if len(values) else np.nan

            quantile_rows.append(row)

    level_rows = []

    for level in MFE_LEVELS:
        reached = statistics[statistics["MFE %"] >= level]
        level_rows.append({"대상": label, "MFE 이상 %": level, "전체 건수": len(statistics), "닿은 건수": len(reached),
                           "닿은 비율 %": len(reached) / len(statistics) * 100.0 if len(statistics) else np.nan,
                           "닿고 최종 가격 손실 비율 %": float((reached["가격 수익 %"] < 0).mean() * 100.0) if len(reached) else np.nan,
                           "닿고 최종 순손실 비율 %": float((reached["순수익 %"] <= 0).mean() * 100.0) if len(reached) else np.nan,
                           "닿은 건 순수익 평균 %": float(reached["순수익 %"].mean()) if len(reached) else np.nan,
                           "닿은 건 순수익 중앙값 %": float(reached["순수익 %"].median()) if len(reached) else np.nan})

    return quantile_rows, level_rows


def write_csv(frame: pd.DataFrame, path: Path) -> None:
    frame.to_csv(path, index=False, encoding="utf-8-sig", lineterminator="\n", float_format="%.10g")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default=str(harness.RESULTS / "round_d"))
    output = Path(parser.parse_args().output)
    trades_folder = output / "trades"
    trades_folder.mkdir(parents=True, exist_ok=True)
    started = time.time()
    market = harness.Market()
    market.candidates()
    scan = market.engine_scan()
    runner = run_stages.Runner(market)
    built = time.time() - started
    print(f"시장 자료·후보·스캔 {built:.0f}초, 후보 {len(market.candidate_stock):,}건 중 스캔 안 {scan['candidates_in_scan']:,}건")
    empty_target = np.zeros((1, runner.span), dtype=np.int32)

    # ── 1·2단계 414조합 다시 돌리기: 하네스 변경 회귀 확인 + 합친 묶음 통과선용 ──
    table12 = run_stages.combination_table()
    table12["account"] = ""
    equity12, _, scalars12 = run_plain(runner, table12, empty_target)
    daily12 = run_stages.daily_returns(equity12, table12["slots"].to_numpy(np.float64) * harness.SLOT_AMOUNT)
    previous = pd.concat([pd.read_csv(harness.RESULTS / "stage1_table.csv"), pd.read_csv(harness.RESULTS / "stage2_table.csv")])
    previous = previous.sort_values("조합 번호").reset_index(drop=True)
    regression = pd.DataFrame({"조합 번호": previous["조합 번호"],
                               "이전 끝 평가금(원)": previous["끝 평가금(원)"], "다시 돈 끝 평가금(원)": equity12[:, -1],
                               "이전 매수 수": previous["매수 수"], "다시 돈 매수 수": scalars12[:, SCALAR["buys"]].astype(int)})
    regression["끝 평가금 차이(원)"] = regression["다시 돈 끝 평가금(원)"] - regression["이전 끝 평가금(원)"]
    write_csv(regression, output / "stage12_regression.csv")
    regression_gap = float(regression["끝 평가금 차이(원)"].abs().max())
    buys_gap = int((regression["다시 돈 매수 수"] - regression["이전 매수 수"]).abs().max())
    print(f"1·2단계 회귀: 끝 평가금 최대 차이 {regression_gap:.3g}원, 매수 수 최대 차이 {buys_gap}")
    stage12_time = time.time() - started

    # ── 격자 505조합(기록 켬) ──
    table = grid_table()
    count = len(table)
    parameters = run_stages.parameter_matrix(table)
    equity_all = np.zeros((count, runner.span))
    buys_all = np.zeros((count, runner.span), dtype=np.int32)
    scalars_all = np.zeros((count, len(harness.SCALAR_FIELDS)))
    medians = np.full(count, np.nan)
    reason_rows = []
    logs_kept = {}
    keep_rows = set(np.flatnonzero((table["take_profit"] == 0) & (table["stop_loss"] == 0)).tolist())

    for begin in range(0, count, BATCH_LOGGED):
        end = min(begin + BATCH_LOGGED, count)
        equity, buys, scalars, logs, log_counts = runner.market.run(parameters[begin:end], empty_target, LOG_CAPACITY)
        equity_all[begin:end], buys_all[begin:end], scalars_all[begin:end] = equity, buys, scalars

        for offset in range(end - begin):
            row = begin + offset
            log = pd.DataFrame(logs[offset, :log_counts[offset]], columns=harness.LOG_FIELDS)
            log["net_return"] = log["proceeds"] / log["spend"] - 1.0
            log["gross_return"] = log["exit_price"] / log["entry_price"] - 1.0
            medians[row] = log["net_return"].median() * 100.0 if len(log) else np.nan

            for reason, group in log.groupby("reason"):
                reason_rows.append({"조합 번호": row, "계좌": table["account"].iloc[row],
                                    "매도": "D158" if table["rule"].iloc[row] == harness.RULE_TREND else "OLD",
                                    "익절": text(table["take_profit"].iloc[row]), "손절": text(table["stop_loss"].iloc[row]),
                                    "매도 이유": harness.REASON_TEXT[int(reason)], "건수": len(group),
                                    "비중 %": len(group) / len(log) * 100.0,
                                    "가격 수익 평균 %": group["gross_return"].mean() * 100.0,
                                    "순수익 평균 %": group["net_return"].mean() * 100.0,
                                    "순수익 합(원)": float((group["proceeds"] - group["spend"]).sum()),
                                    "평균 보유일": float((group["exit_day"] - group["entry_day"] + 1).mean())})

            if row in keep_rows:
                logs_kept[row] = log

        print(f"  격자 {end}/{count} — {time.time() - started:.0f}초")

    if scalars_all[:, SCALAR["log_overflow"]].any():
        raise SystemExit("매매 기록 칸이 모자란다 — LOG_CAPACITY 를 늘린다")

    strategy, daily_all, yearly_all = summarize(runner, table, equity_all, scalars_all, medians)
    grid_time = time.time() - started

    # 실제 설정 S 계좌 없음·없음 6칸은 1단계 D158 1호가 조합과 같은 계열이어야 한다
    duplicate_rows = np.flatnonzero((table["account"] != ENGINE_ACCOUNT) & (table["take_profit"] == 0)
                                    & (table["stop_loss"] == 0) & (table["rule"] == harness.RULE_TREND))
    stage1_match = []

    for row in duplicate_rows:
        match = np.flatnonzero((table12["stage"] == 1) & (table12["mode"] == harness.MODE_TURNOVER)
                               & (table12["rule"] == harness.RULE_TREND) & (table12["slippage"] == 1)
                               & (table12["slots"] == table["slots"].iloc[row])
                               & (table12["amount_mode"] == table["amount_mode"].iloc[row]))[0]
        stage1_match.append(float(np.abs(equity_all[row] - equity12[match]).max()))

    # ── 무작위 대조: 조합마다 20회, 같은 날 같은 건수를 시가에, 같은 익절·손절·매도 규칙 ──
    mean_spend = scalars_all[:, SCALAR["spend_sum"]] / np.maximum(scalars_all[:, SCALAR["buys"]], 1.0)
    control_parts = []

    for seed in run_stages.CONTROL_SEEDS:
        part = table.copy()
        part["control"] = 1
        part["seed"] = seed
        part["log"] = 0
        part["pair_row"] = np.arange(count)
        engine = part["mode"].to_numpy() == harness.MODE_ENGINE
        # 실제 설정 계좌의 대조는 점수 금액 대신 그 조합의 매수당 평균 금액으로 고정해 산다(자본·100종목·총노출은 같다)
        part.loc[engine, "mode"] = harness.MODE_TURNOVER
        part.loc[engine, "amount_mode"] = harness.AMOUNT_FIXED
        part.loc[engine, "fixed_amount"] = mean_spend[engine]
        control_parts.append(part)

    control_table = pd.concat(control_parts, ignore_index=True)
    control_equity, _, control_scalars = run_plain(runner, control_table, buys_all)
    control_daily = run_stages.daily_returns(control_equity, effective_capital(control_table))
    control_annual = np.array([study41.curve_statistics(row)[0] for row in control_daily])
    control_excess = {name: study41.annual_mean(control_daily - runner.index_returns[name][None, :]) * 100.0
                      for name in INDEX_NAMES}
    control_frame = pd.DataFrame({"조합 번호": control_table["pair_row"], "계좌": control_table["account"],
                                  "익절": control_table["take_profit"].map(text), "손절": control_table["stop_loss"].map(text),
                                  "대조 시드": control_table["seed"], "대조 연 수익 %": control_annual,
                                  "대조 매수 수": control_scalars[:, SCALAR["buys"]].astype(int),
                                  "대조 매수당 금액(원)": control_table["fixed_amount"],
                                  **{f"대조 {name} 대비 연 초과 %p": values for name, values in control_excess.items()}})
    write_csv(control_frame, output / "controls.csv")
    pair_annual = control_annual.reshape(len(run_stages.CONTROL_SEEDS), count)
    strategy["무작위 대조 승수"] = (strategy["연 수익 %"].to_numpy()[None, :] > pair_annual).sum(axis=0)
    strategy["무작위 대조 연 수익 중앙값 %"] = np.median(pair_annual, axis=0)
    strategy["대조 매수 수 중앙값"] = np.median(control_scalars[:, SCALAR["buys"]].reshape(len(run_stages.CONTROL_SEEDS),
                                                                                        count), axis=0)
    control_time = time.time() - started

    # ── t 와 묶음 통과선 ──
    sign_pair = study41.signs(runner.calendar)
    excess12 = excess_of(runner, daily12)
    excess_d = excess_of(runner, daily_all)
    t_month, t_year, flip_month, flip_year = study41.t_values(np.concatenate([excess12, excess_d]), runner.calendar,
                                                              sign_pair)
    series12 = excess12.shape[0]
    keep_d = np.ones(count, dtype=bool)
    keep_d[duplicate_rows] = False
    keep_d_series = np.tile(keep_d, len(INDEX_NAMES))
    only_d = np.r_[np.zeros(series12, dtype=bool), np.ones(excess_d.shape[0], dtype=bool)]
    merged = np.r_[np.ones(series12, dtype=bool), keep_d_series]
    bars = {"43-D만(1,010계열)": study41.family_bar(flip_month[only_d], flip_year[only_d]),
            f"1·2단계 + 43-D(겹치는 12계열 뺀 {int(merged.sum()):,}계열)": study41.family_bar(flip_month[merged],
                                                                                          flip_year[merged])}
    write_csv(pd.DataFrame([{"묶음": label, **values} for label, values in bars.items()]), output / "family_bars.csv")
    bar_d = list(bars.values())[0]["family_bar"]
    bar_merged = list(bars.values())[1]["family_bar"]
    t_columns = attach_t(strategy, t_month[series12:], t_year[series12:], count)
    frame12 = pd.DataFrame(index=range(len(table12)))
    attach_t(frame12, t_month[:series12], t_year[:series12], len(table12))
    t_check = float(np.abs(frame12["가장 낮은 t"].to_numpy() - previous["가장 낮은 t"].to_numpy()).max())
    wins = strategy["무작위 대조 승수"] >= run_stages.CONTROL_WIN_NEED
    strategy["통과(합친 통과선)"] = (strategy["가장 낮은 t"] >= bar_merged) & wins
    strategy["통과(43-D 통과선)"] = (strategy["가장 낮은 t"] >= bar_d) & wins
    description = pd.DataFrame({"조합 번호": np.arange(count), "계좌": table["account"],
                                "매도": np.where(table["rule"] == harness.RULE_TREND, "D158", "OLD"),
                                "익절": table["take_profit"].map(text), "손절": table["stop_loss"].map(text),
                                "익절 %": table["take_profit"], "손절 %": table["stop_loss"],
                                "슬롯": table["slots"], "금액": table["amount_mode"].map(harness.AMOUNT_TEXT),
                                "미끄러짐 호가": table["slippage"]})

    # 옆 칸(같은 계좌, 익절·손절 한 칸 앞뒤) 모두 같은 부호인지 — 가장 낮은 t 기준
    lowest = strategy["가장 낮은 t"].to_numpy()
    neighbor_same = np.zeros(count, dtype=bool)
    neighbor_min = np.full(count, np.nan)

    for row in range(count):
        if table["rule"].iloc[row] != harness.RULE_TREND:
            continue

        account = table["account"].iloc[row]
        take_place = TAKE_PROFITS.index(table["take_profit"].iloc[row])
        stop_place = STOP_LOSSES.index(table["stop_loss"].iloc[row])
        others = []

        for take_step, stop_step in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            take_next, stop_next = take_place + take_step, stop_place + stop_step

            if 0 <= take_next < len(TAKE_PROFITS) and 0 <= stop_next < len(STOP_LOSSES):
                other = np.flatnonzero((table["account"] == account) & (table["rule"] == harness.RULE_TREND)
                                       & (table["take_profit"] == TAKE_PROFITS[take_next])
                                       & (table["stop_loss"] == STOP_LOSSES[stop_next]))[0]
                others.append(lowest[other])

        neighbor_min[row] = min(others)
        neighbor_same[row] = bool(np.all(np.sign(others) == np.sign(lowest[row])))

    strategy["옆 칸 가장 낮은 t 최소"] = neighbor_min
    strategy["옆 칸 모두 같은 부호"] = neighbor_same
    full = pd.concat([description, strategy], axis=1)
    write_csv(full, output / "grid_table.csv")

    # 계좌마다 익절 × 손절 표(가장 낮은 t, 코스피 대비 연 초과)
    heat_rows = []

    for account in ACCOUNT_NAMES:
        part = full[(full["계좌"] == account) & (full["매도"] == "D158")]

        for value_name in ("가장 낮은 t", "코스피 대비 연 초과 %p", "연 수익 %", "무작위 대조 승수"):
            pivot = part.pivot(index="익절 %", columns="손절 %", values=value_name)

            for take_profit, values in pivot.iterrows():
                heat_rows.append({"계좌": account, "값": value_name, "익절": text(take_profit),
                                  **{f"손절 {text(stop_loss)}": values[stop_loss] for stop_loss in pivot.columns}})

    write_csv(pd.DataFrame(heat_rows), output / "grid_heat.csv")

    # 계좌마다 가장 좋은 칸이 격자 끝에 걸렸는지
    edge_rows = []

    for account in ACCOUNT_NAMES:
        part = full[(full["계좌"] == account) & (full["매도"] == "D158")]

        for value_name in ("가장 낮은 t", "코스피 대비 연 초과 %p"):
            best = part.loc[part[value_name].idxmax()]
            take_edge = best["익절 %"] in (TAKE_PROFITS[1], TAKE_PROFITS[-1])
            stop_edge = best["손절 %"] in (STOP_LOSSES[1], STOP_LOSSES[-1])
            edge_rows.append({"계좌": account, "기준": value_name, "조합 번호": int(best["조합 번호"]),
                              "익절": best["익절"], "손절": best["손절"], "값": best[value_name],
                              "가장 낮은 t": best["가장 낮은 t"], "익절 끝 값": take_edge, "손절 끝 값": stop_edge,
                              "없음·없음 값": float(part[(part["익절 %"] == 0) & (part["손절 %"] == 0)][value_name].iloc[0])})

    write_csv(pd.DataFrame(edge_rows), output / "edge.csv")

    # 해마다 표
    index_yearly = {name: runner.yearly_compound(returns[None, :])[0] for name, returns in runner.index_returns.items()}
    year_rows = []

    for row in range(count):
        for number, year in enumerate(runner.years):
            year_rows.append({"조합 번호": row, "해": int(year), "전략 %": yearly_all[row, number],
                              **{f"{name} %": index_yearly[name][number] for name in INDEX_NAMES},
                              **{f"{name} 대비 초과 %p": yearly_all[row, number] - index_yearly[name][number]
                                 for name in INDEX_NAMES}})

    write_csv(pd.DataFrame(year_rows), output / "years.csv")
    write_csv(pd.DataFrame(reason_rows), output / "exit_reasons.csv")

    # ── 상위 10칸 미끄러짐 0·2호가 민감도 ──
    top_rows = full[full["매도"] == "D158"].sort_values(["가장 낮은 t", "조합 번호"], ascending=[False, True]).head(
        SENSITIVITY_TOP)["조합 번호"].to_numpy()
    sensitivity_table = pd.concat([table.iloc[top_rows].assign(slippage=slippage, log=0, base_row=top_rows)
                                   for slippage in (0, 2)], ignore_index=True)
    sensitivity_equity, _, sensitivity_scalars = run_plain(runner, sensitivity_table, empty_target)
    sensitivity_summary, sensitivity_daily, _ = summarize(runner, sensitivity_table, sensitivity_equity,
                                                          sensitivity_scalars, np.full(len(sensitivity_table), np.nan))
    sensitivity_months, sensitivity_years, _, _ = study41.t_values(excess_of(runner, sensitivity_daily), runner.calendar)
    attach_t(sensitivity_summary, sensitivity_months, sensitivity_years, len(sensitivity_table))
    sensitivity = pd.concat([pd.DataFrame({"조합 번호": sensitivity_table["base_row"],
                                           "계좌": sensitivity_table["account"],
                                           "익절": sensitivity_table["take_profit"].map(text),
                                           "손절": sensitivity_table["stop_loss"].map(text),
                                           "미끄러짐 호가": sensitivity_table["slippage"]}),
                             sensitivity_summary], axis=1)
    base = full.iloc[top_rows][["조합 번호", "계좌", "익절", "손절", "미끄러짐 호가"] + list(strategy.columns)]
    sensitivity = pd.concat([base[sensitivity.columns.intersection(base.columns)], sensitivity], ignore_index=True)
    sensitivity = sensitivity.sort_values(["조합 번호", "미끄러짐 호가"], kind="stable")
    write_csv(sensitivity, output / "slippage_sensitivity.csv")

    # ── MAE / MFE: 1단계 D158 1호가 6조합(= S 계좌 없음·없음) + 실제 설정 D158 없음·없음 ──
    quantile_rows, level_rows = [], []
    engine_plain = int(np.flatnonzero((table["account"] == ENGINE_ACCOUNT) & (table["rule"] == harness.RULE_TREND)
                                      & (table["take_profit"] == 0) & (table["stop_loss"] == 0))[0])
    engine_old = count - 1
    statistics_all = []

    for row in [engine_plain, *duplicate_rows.tolist()]:
        label = f"{table['account'].iloc[row]} D158"
        log = logs_kept[row]
        statistics = path_statistics(market, log)
        quantile_part, level_part = mae_tables(label, statistics)
        quantile_rows += quantile_part
        level_rows += level_part
        statistics_all.append(pd.concat([log.assign(target=label, code=market.codes[log["stock"].astype(np.int64)],
                                                    entry_date=market.dates[log["entry_day"].astype(np.int64)],
                                                    exit_date=market.dates[np.minimum(log["exit_day"].astype(np.int64),
                                                                                      market.day_count - 1)]),
                                         statistics], axis=1))

    write_csv(pd.DataFrame(quantile_rows), output / "mae_mfe_quantiles.csv")
    write_csv(pd.DataFrame(level_rows), output / "mfe_then_loss.csv")
    pd.concat(statistics_all, ignore_index=True).to_parquet(trades_folder / "mae_mfe_trades.parquet", index=False)

    for row in (engine_plain, engine_old):
        log = logs_kept[row].copy()
        log["code"] = market.codes[log["stock"].astype(np.int64).to_numpy()]
        log["entry_date"] = market.dates[log["entry_day"].astype(np.int64).to_numpy()]
        log["reason_text"] = log["reason"].astype(int).map(harness.REASON_TEXT)
        log.to_parquet(trades_folder / f"engine_{'d158' if row == engine_plain else 'old'}.parquet", index=False)

    curves = pd.DataFrame(equity_all.T, columns=[f"combination_{row:03d}" for row in range(count)])
    curves.insert(0, "date", market.input_dates)
    curves.to_parquet(trades_folder / "curves.parquet", index=False)

    # ── 판정 요약 ──
    judge = []

    for account in ACCOUNT_NAMES:
        for rule in ("D158", "OLD"):
            group = full[(full["계좌"] == account) & (full["매도"] == rule)]

            if group.empty:
                continue

            judge.append({"계좌": account, "매도": rule, "조합 수": len(group),
                          "통과(합친 통과선)": int(group["통과(합친 통과선)"].sum()),
                          "통과(43-D 통과선)": int(group["통과(43-D 통과선)"].sum()),
                          "t만 통과(합친)": int((group["가장 낮은 t"] >= bar_merged).sum()),
                          "대조 19승 이상": int((group["무작위 대조 승수"] >= run_stages.CONTROL_WIN_NEED).sum()),
                          "가장 높은 '가장 낮은 t'": group["가장 낮은 t"].max(),
                          "가장 높은 코스피 대비 연 초과 %p": group["코스피 대비 연 초과 %p"].max(),
                          "가장 높은 코스닥 대비 연 초과 %p": group["코스닥 대비 연 초과 %p"].max(),
                          "통과선 합친": bar_merged, "통과선 43-D": bar_d})

    judge_frame = pd.DataFrame(judge)
    write_csv(judge_frame, output / "judge.csv")
    scan_frame = pd.DataFrame([{"값": key, "수": value} for key, value in scan.items()])
    write_csv(scan_frame, output / "engine_scan.csv")
    inputs_path = study41.CACHE / "inputs.npz"
    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                            cwd=harness.REPOSITORY).stdout.strip()
    run_info = {"commit": commit, "inputs_sha256": hashlib.sha256(inputs_path.read_bytes()).hexdigest(),
                "panel_sha256": hashlib.sha256(harness.grid.PANEL_PATH.read_bytes()).hexdigest(),
                "harness_sha256": hashlib.sha256((harness.STUDY / "harness.py").read_bytes()).hexdigest(),
                "period": [str(market.input_dates[0].date()), str(market.input_dates[-1].date())],
                "stock_count": market.stock_count, "candidate_count": int(len(market.candidate_stock)),
                "engine_scan": scan, "combinations": count, "series_43d": int(excess_d.shape[0]),
                "series_merged": int(merged.sum()), "control_seeds": list(run_stages.CONTROL_SEEDS),
                "flip_seed": study41.SEED, "flips": study41.FLIPS, "family_bar_43d": bar_d,
                "family_bar_merged": bar_merged, "control_win_need": run_stages.CONTROL_WIN_NEED,
                "stage12_regression_max_gap_krw": regression_gap, "stage12_regression_buys_gap": buys_gap,
                "stage12_lowest_t_gap": t_check, "stage1_duplicate_max_gap_krw": max(stage1_match),
                "seconds": {"build": built, "stage12": stage12_time - built, "grid": grid_time - stage12_time,
                            "controls": control_time - grid_time, "total": time.time() - started}}
    (output / "run_info.json").write_bytes(json.dumps(run_info, ensure_ascii=False, indent=1, default=str)
                                           .encode("utf-8"))
    (output / "run_command.txt").write_bytes(b"py -I -X utf8 research/studies/43_devscale_real_rules/round_d.py\n")
    print(judge_frame.to_string())
    print(f"통과선 43-D {bar_d:.3f}, 합친 {bar_merged:.3f}; 1단계 겹침 최대 차이 {max(stage1_match):.3g}원, "
          f"1·2단계 가장 낮은 t 차이 {t_check:.3g}")
    print(f"전체 {time.time() - started:.0f}초")


if __name__ == "__main__":
    main()
