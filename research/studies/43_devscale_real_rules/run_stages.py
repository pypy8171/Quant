"""스터디 43 실행 — 1단계(DevScale 실제 매수 × D158·OLD 매도)와 2단계(VALUE·QUAL·HIGH250 신호 결합)를 한 번에 돌린다.

시험 목록·판정 기준은 README.md "시험 목록"에 실행 전에 고정했다. 기간은 2010-01-04 – 2026-10-08 한 번에 돌린다.
실행: py -I -X utf8 research/studies/43_devscale_real_rules/run_stages.py
산출: results/stage1_table.csv · stage2_table.csv · order_random.csv · controls.csv · family_bars.csv · judge.csv ·
      years.csv · exit_reasons.csv · costs.csv · curves.parquet · trades/*.parquet · run_info.json · run_command.txt
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))

import harness     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from harness import study41     # noqa: E402

RESULTS = harness.RESULTS
SLOT_CHOICES = (4, 10, 20)
AMOUNT_CHOICES = (0, 1)
SLIPPAGE_CHOICES = (0, 1, 2)
RULE_CHOICES = (harness.RULE_TREND, harness.RULE_OLD)
QUANTILE_CHOICES = (0.2, 0.5)
CONTROL_SEEDS = tuple(study41.SEED + number for number in range(101, 121))
CONTROL_WIN_NEED = 19
BATCH_LOGGED = 24
BATCH_PLAIN = 600
FIELD = {name: number for number, name in enumerate(harness.PARAMETER_FIELDS)}
SCALAR = {name: number for number, name in enumerate(harness.SCALAR_FIELDS)}


def combination_table() -> pd.DataFrame:
    """순서 (i) 조합 전부(1단계 36 + 2단계 378). 무작위 순서·대조는 여기서 파생한다."""
    rows = []

    for rule in RULE_CHOICES:
        for slots in SLOT_CHOICES:
            for amount_mode in AMOUNT_CHOICES:
                for slippage in SLIPPAGE_CHOICES:
                    base = {"rule": rule, "slots": slots, "amount_mode": amount_mode, "slippage": slippage}
                    rows.append({**base, "stage": 1, "mode": harness.MODE_TURNOVER, "signal": -1, "quantile": 0.0})

                    for signal in range(len(harness.SIGNALS)):
                        rows.append({**base, "stage": 2, "mode": harness.MODE_SIGNAL_ORDER, "signal": signal,
                                     "quantile": 0.0})

                        for quantile in QUANTILE_CHOICES:
                            rows.append({**base, "stage": 2, "mode": harness.MODE_SIGNAL_FILTER, "signal": signal,
                                         "quantile": quantile})

                        if rule == harness.RULE_TREND:
                            rows.append({**base, "stage": 2, "mode": harness.MODE_SIGNAL_ONLY, "signal": signal,
                                         "quantile": 0.0})

    table = pd.DataFrame(rows).sort_values(["stage", "mode", "signal", "quantile", "rule", "slots", "amount_mode",
                                             "slippage"], kind="stable").reset_index(drop=True)
    table["seed"] = 0
    table["control"] = 0
    table["pair_row"] = 0
    table["log"] = 1

    for name in ("take_profit", "stop_loss", "capital", "gross_cap", "fixed_amount"):
        table[name] = 0.0     # 43-D 에서 더한 열. 0 이면 1·2단계 동작 그대로

    return table


def describe(table: pd.DataFrame) -> pd.DataFrame:
    named = pd.DataFrame({
        "단계": table["stage"].to_numpy(),
        "방식": table["mode"].map(harness.MODE_TEXT).to_numpy(),
        "신호": [harness.SIGNALS[signal] if signal >= 0 else "" for signal in table["signal"]],
        "신호 상위 q": [f"{quantile:.0%}" if quantile > 0 else "" for quantile in table["quantile"]],
        "매도": table["rule"].map({harness.RULE_TREND: "D158", harness.RULE_OLD: "OLD"}).to_numpy(),
        "슬롯": table["slots"].to_numpy(),
        "금액": table["amount_mode"].map(harness.AMOUNT_TEXT).to_numpy(),
        "미끄러짐 호가": table["slippage"].to_numpy()})
    return named


def parameter_matrix(table: pd.DataFrame) -> np.ndarray:
    return table[list(harness.PARAMETER_FIELDS)].to_numpy(np.float64)


def daily_returns(equity: np.ndarray, capital: np.ndarray) -> np.ndarray:
    previous = np.concatenate([capital[:, None], equity[:, :-1]], axis=1)
    return equity / previous - 1.0


class Runner:
    def __init__(self, market: harness.Market):
        self.market = market
        span = market.day_count - market.first_day
        self.span = span
        dates = market.input_dates
        years = dates.year.to_numpy()
        year_index = years - years[0]
        self.calendar = SimpleNamespace(year_index=year_index, year_count=int(year_index.max()) + 1,
                                        month_index=(year_index * 12 + dates.month.to_numpy() - 1).astype(np.int64))
        self.calendar.month_count = int(self.calendar.month_index.max()) + 1
        self.years = np.unique(years)
        self.index_returns = {}

        for name, close in (("코스피", market.kospi_close), ("코스닥", market.kosdaq_close)):
            returns = np.zeros(span)
            returns[1:] = close[1:] / close[:-1] - 1.0
            self.index_returns[name] = np.nan_to_num(returns)

    def run(self, parameters: np.ndarray, target_buys: np.ndarray, logged: bool):
        capacity = int(parameters[:, FIELD["slots"]].max()) * self.span if logged else 0
        return self.market.run(parameters, target_buys, capacity)

    def yearly_compound(self, daily: np.ndarray) -> np.ndarray:
        """행 × 해 복리 수익(%)."""
        out = np.zeros((daily.shape[0], len(self.years)))

        for number, year in enumerate(self.years):
            part = self.calendar.year_index == number
            out[:, number] = (np.prod(1.0 + np.nan_to_num(daily[:, part]), axis=1) - 1.0) * 100.0

        return out


def summary_rows(runner: Runner, table: pd.DataFrame, equity: np.ndarray, buys: np.ndarray, scalars: np.ndarray,
                 medians: np.ndarray) -> pd.DataFrame:
    capital = table["slots"].to_numpy(np.float64) * harness.SLOT_AMOUNT
    daily = daily_returns(equity, capital)
    span_years = runner.span / study41.YEAR_DAYS
    rows = []
    index_yearly = {name: runner.yearly_compound(returns[None, :])[0] for name, returns in runner.index_returns.items()}
    yearly = runner.yearly_compound(daily)

    for row in range(len(table)):
        annual, drawdown, sharpe = study41.curve_statistics(daily[row])
        closed = scalars[row, SCALAR["closed_trades"]]
        entry = {"연 수익 %": annual, "MDD %": drawdown, "샤프": sharpe}

        for name, returns in runner.index_returns.items():
            entry[f"{name} 대비 연 초과 %p"] = float(study41.annual_mean((daily[row] - returns)[None, :])[0] * 100.0)
            entry[f"{name} 대비 플러스 해"] = int((yearly[row] > index_yearly[name]).sum())

        entry.update({
            "전체 해": len(runner.years),
            "매수 수": int(scalars[row, SCALAR["buys"]]),
            "건당 순수익 평균 %": scalars[row, SCALAR["net_sum"]] / closed * 100.0 if closed else np.nan,
            "건당 순수익 중앙값 %": medians[row],
            "연 회전(매수액÷평균 평가금)": scalars[row, SCALAR["spend_sum"]] / equity[row].mean() / span_years,
            "평균 보유일": scalars[row, SCALAR["hold_day_sum"]] / closed if closed else np.nan,
            "슬롯 평균 사용률 %": scalars[row, SCALAR["held_day_sum"]] / (table["slots"].iloc[row] * runner.span) * 100.0,
            "현금 부족 건너뜀": int(scalars[row, SCALAR["no_cash_skips"]]),
            "수수료 합(원)": scalars[row, SCALAR["commission"]],
            "매도세 합(원)": scalars[row, SCALAR["tax"]],
            "미끄러짐 합(원)": scalars[row, SCALAR["slippage"]],
            "끝 평가금(원)": equity[row, -1],
            "기록 넘침": int(scalars[row, SCALAR["log_overflow"]])})
        rows.append(entry)

    return pd.DataFrame(rows), daily, yearly


def main() -> None:
    started = time.time()
    RESULTS.mkdir(exist_ok=True)
    (RESULTS / "trades").mkdir(exist_ok=True)
    market = harness.Market()
    market.candidates()
    runner = Runner(market)
    built = time.time() - started
    print(f"시장 자료·후보 행렬 {built:.0f}초, 후보 {len(market.candidate_stock):,}건")
    table = combination_table()
    description = describe(table)
    combination_count = len(table)
    equity_all = np.zeros((combination_count, runner.span))
    buys_all = np.zeros((combination_count, runner.span), dtype=np.int32)
    scalars_all = np.zeros((combination_count, len(harness.SCALAR_FIELDS)))
    medians = np.full(combination_count, np.nan)
    reason_rows = []
    dummy_target = np.zeros((1, runner.span), dtype=np.int32)
    parameters = parameter_matrix(table)

    for begin in range(0, combination_count, BATCH_LOGGED):
        end = min(begin + BATCH_LOGGED, combination_count)
        equity, buys, scalars, logs, log_counts = runner.run(parameters[begin:end], dummy_target, True)
        equity_all[begin:end], buys_all[begin:end], scalars_all[begin:end] = equity, buys, scalars

        for offset in range(end - begin):
            row = begin + offset
            log = pd.DataFrame(logs[offset, :log_counts[offset]], columns=harness.LOG_FIELDS)
            log["net_return"] = log["proceeds"] / log["spend"] - 1.0
            log["gross_return"] = log["exit_price"] / log["entry_price"] - 1.0
            medians[row] = log["net_return"].median() * 100.0 if len(log) else np.nan

            if table["slippage"].iloc[row] != 1:
                continue

            for reason, group in log.groupby("reason"):
                reason_rows.append({**description.iloc[row].to_dict(), "조합 번호": row,
                                    "매도 이유": harness.REASON_TEXT[int(reason)], "건수": len(group),
                                    "비중 %": len(group) / len(log) * 100.0,
                                    "가격 수익 평균 %": group["gross_return"].mean() * 100.0,
                                    "순수익 평균 %": group["net_return"].mean() * 100.0,
                                    "순수익 합(원)": float((group["proceeds"] - group["spend"]).sum()),
                                    "평균 보유일": float((group["exit_day"] - group["entry_day"] + 1).mean())})

            log["stock"] = log["stock"].astype(np.int64)
            log["code"] = market.codes[log["stock"].to_numpy()]
            log["entry_date"] = market.dates[log["entry_day"].astype(np.int64).to_numpy()]
            log["exit_date"] = market.dates[log["exit_day"].astype(np.int64).to_numpy()]
            log["reason_text"] = log["reason"].astype(int).map(harness.REASON_TEXT)
            log.to_parquet(RESULTS / "trades" / f"combination_{row:03d}.parquet", index=False)

        print(f"  순서 (i) {end}/{combination_count} — {time.time() - started:.0f}초")

    strategy, daily_all, yearly_all = summary_rows(runner, table, equity_all, buys_all, scalars_all, medians)
    strategy_time = time.time() - started

    # 순서 (ii): 1단계 36 조합 × 무작위 순서 20회
    stage1 = np.flatnonzero(table["stage"].to_numpy() == 1)
    random_table = pd.concat([table.iloc[stage1].assign(mode=harness.MODE_RANDOM_ORDER, seed=seed, log=0,
                                                        base_row=stage1) for seed in study41.RANDOM_SEEDS],
                             ignore_index=True)
    random_equity = np.zeros((len(random_table), runner.span))
    random_scalars = np.zeros((len(random_table), len(harness.SCALAR_FIELDS)))
    random_parameters = parameter_matrix(random_table)

    for begin in range(0, len(random_table), BATCH_PLAIN):
        end = min(begin + BATCH_PLAIN, len(random_table))
        equity, _, scalars, _, _ = runner.run(random_parameters[begin:end], dummy_target, False)
        random_equity[begin:end], random_scalars[begin:end] = equity, scalars

    random_summary, random_daily, _ = summary_rows(runner, random_table, random_equity, None, random_scalars,
                                                   np.full(len(random_table), np.nan))
    random_time = time.time() - started

    # 무작위 종목 대조: 순서 (i) 조합마다 20회, 같은 날 같은 건수를 시가에
    control_table = pd.concat([table.assign(control=1, seed=seed, log=0, pair_row=np.arange(combination_count))
                               for seed in CONTROL_SEEDS], ignore_index=True)
    control_parameters = parameter_matrix(control_table)
    control_annual = np.zeros(len(control_table))
    control_excess = {name: np.zeros(len(control_table)) for name in runner.index_returns}
    control_buys = np.zeros(len(control_table))
    capital_control = control_table["slots"].to_numpy(np.float64) * harness.SLOT_AMOUNT

    for begin in range(0, len(control_table), BATCH_PLAIN):
        end = min(begin + BATCH_PLAIN, len(control_table))
        equity, _, scalars, _, _ = runner.run(control_parameters[begin:end], buys_all, False)
        daily = daily_returns(equity, capital_control[begin:end])
        control_annual[begin:end] = [study41.curve_statistics(row)[0] for row in daily]
        control_buys[begin:end] = scalars[:, SCALAR["buys"]]

        for name, returns in runner.index_returns.items():
            control_excess[name][begin:end] = study41.annual_mean(daily - returns[None, :]) * 100.0

        print(f"  대조 {end}/{len(control_table)} — {time.time() - started:.0f}초")

    control_time = time.time() - started
    control_frame = pd.concat([description.iloc[control_table["pair_row"]].reset_index(drop=True),
                               pd.DataFrame({"조합 번호": control_table["pair_row"], "대조 시드": control_table["seed"],
                                             "대조 연 수익 %": control_annual, "대조 매수 수": control_buys,
                                             **{f"대조 {name} 대비 연 초과 %p": values
                                                for name, values in control_excess.items()}})], axis=1)
    control_frame.to_csv(RESULTS / "controls.csv", index=False, encoding="utf-8-sig", lineterminator="\n")
    pair_annual = control_annual.reshape(len(CONTROL_SEEDS), combination_count)
    strategy["무작위 대조 승수"] = (strategy["연 수익 %"].to_numpy()[None, :] > pair_annual).sum(axis=0)
    strategy["무작위 대조 연 수익 중앙값 %"] = np.median(pair_annual, axis=0)

    # t 값과 묶음 통과선: 순서 (i) 조합 × 지수 2
    sign_pair = study41.signs(runner.calendar)
    names = list(runner.index_returns)
    excess = np.concatenate([daily_all - runner.index_returns[name][None, :] for name in names])
    t_month, t_year, flip_month, flip_year = study41.t_values(excess, runner.calendar, sign_pair)

    for number, name in enumerate(names):
        part = slice(number * combination_count, (number + 1) * combination_count)
        strategy[f"{name} 월 t"] = t_month[part]
        strategy[f"{name} 연 t"] = t_year[part]

    stage1_mask = np.tile(table["stage"].to_numpy() == 1, len(names))
    bars = {"1단계만(72계열)": study41.family_bar(flip_month[stage1_mask], flip_year[stage1_mask]),
            "1·2단계 전체(828계열)": study41.family_bar(flip_month, flip_year)}
    pd.DataFrame([{"묶음": label, **values} for label, values in bars.items()]).to_csv(
        RESULTS / "family_bars.csv", index=False, encoding="utf-8-sig", lineterminator="\n")
    bar_all = bars["1·2단계 전체(828계열)"]["family_bar"]
    bar_stage1 = bars["1단계만(72계열)"]["family_bar"]
    t_columns = [f"{name} {kind} t" for name in names for kind in ("월", "연")]
    lowest_t = strategy[t_columns].min(axis=1)
    strategy["가장 낮은 t"] = lowest_t
    strategy["통과(전체 통과선)"] = (lowest_t >= bar_all) & (strategy["무작위 대조 승수"] >= CONTROL_WIN_NEED)
    strategy["통과(1단계 통과선)"] = (lowest_t >= bar_stage1) & (strategy["무작위 대조 승수"] >= CONTROL_WIN_NEED)
    full = pd.concat([description, strategy], axis=1)
    full.insert(0, "조합 번호", np.arange(combination_count))
    full[table["stage"].to_numpy() == 1].to_csv(RESULTS / "stage1_table.csv", index=False, encoding="utf-8-sig",
                                                 lineterminator="\n")
    full[table["stage"].to_numpy() == 2].to_csv(RESULTS / "stage2_table.csv", index=False, encoding="utf-8-sig",
                                                 lineterminator="\n")

    # 무작위 순서(ii): 시드마다 같은 통과선으로
    random_months, random_years, _, _ = study41.t_values(
        np.concatenate([random_daily - runner.index_returns[name][None, :] for name in names]), runner.calendar)
    random_out = pd.concat([describe(random_table).assign(방식="DevScale·무작위 순"),
                            random_table[["seed"]].rename(columns={"seed": "순서 시드"}).reset_index(drop=True),
                            random_summary.reset_index(drop=True)], axis=1)
    random_out.insert(0, "조합 번호", random_table["base_row"].to_numpy())

    for number, name in enumerate(names):
        part = slice(number * len(random_table), (number + 1) * len(random_table))
        random_out[f"{name} 월 t"] = random_months[part]
        random_out[f"{name} 연 t"] = random_years[part]

    random_lowest = random_out[t_columns].min(axis=1)
    random_out["t 통과(1단계 통과선)"] = random_lowest >= bar_stage1
    random_out.to_csv(RESULTS / "order_random.csv", index=False, encoding="utf-8-sig", lineterminator="\n")

    # 해마다 표: 조합 × 해(전략·코스피·코스닥 복리 %)
    index_yearly = {name: runner.yearly_compound(returns[None, :])[0] for name, returns in runner.index_returns.items()}
    year_rows = []

    for row in range(combination_count):
        for number, year in enumerate(runner.years):
            year_rows.append({"조합 번호": row, "해": int(year), "전략 %": yearly_all[row, number],
                              **{f"{name} %": index_yearly[name][number] for name in names},
                              **{f"{name} 대비 초과 %p": yearly_all[row, number] - index_yearly[name][number]
                                 for name in names}})

    pd.DataFrame(year_rows).to_csv(RESULTS / "years.csv", index=False, encoding="utf-8-sig", lineterminator="\n")
    pd.DataFrame(reason_rows).to_csv(RESULTS / "exit_reasons.csv", index=False, encoding="utf-8-sig",
                                     lineterminator="\n")

    # 판정 요약
    judge = []

    for (stage, mode), group in full.groupby(["단계", "방식"], sort=False):
        judge.append({"단계": stage, "방식": mode, "조합 수": len(group),
                      "통과(전체 통과선)": int(group["통과(전체 통과선)"].sum()),
                      "통과(1단계 통과선)": int(group["통과(1단계 통과선)"].sum()),
                      "t만 통과(전체)": int((group["가장 낮은 t"] >= bar_all).sum()),
                      "대조 19승 이상": int((group["무작위 대조 승수"] >= CONTROL_WIN_NEED).sum()),
                      "가장 높은 '가장 낮은 t'": group["가장 낮은 t"].max(),
                      "가장 높은 코스피 대비 연 초과 %p": group["코스피 대비 연 초과 %p"].max(),
                      "통과선 전체": bar_all, "통과선 1단계": bar_stage1})

    pd.DataFrame(judge).to_csv(RESULTS / "judge.csv", index=False, encoding="utf-8-sig", lineterminator="\n")
    curve_rows = np.flatnonzero(table["slippage"].to_numpy() == 1)
    curves = pd.DataFrame(equity_all[curve_rows].T, columns=[f"combination_{row:03d}" for row in curve_rows])
    curves.insert(0, "date", market.input_dates)
    curves.to_parquet(RESULTS / "curves.parquet", index=False)

    # 지수 자체 성과(비교용 한 줄)
    index_rows = []

    for name, returns in runner.index_returns.items():
        annual, drawdown, sharpe = study41.curve_statistics(returns)
        index_rows.append({"지수": name, "연 수익 %": annual, "MDD %": drawdown, "샤프": sharpe})

    pd.DataFrame(index_rows).to_csv(RESULTS / "index_reference.csv", index=False, encoding="utf-8-sig",
                                    lineterminator="\n")
    info_path = study41.CACHE / "inputs_info.json"
    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                            cwd=harness.REPOSITORY).stdout.strip()
    run_info = {"commit": commit, "inputs_info": json.loads(info_path.read_text(encoding="utf-8")),
                "panel_sha256": hashlib.sha256(harness.grid.PANEL_PATH.read_bytes()).hexdigest(),
                "period": [str(market.input_dates[0].date()), str(market.input_dates[-1].date())],
                "stock_count": market.stock_count, "excluded_38_only_codes": len(market.excluded_codes),
                "ratio_change_cells": market.ratio_change_days, "candidate_count": int(len(market.candidate_stock)),
                "order_seeds": list(study41.RANDOM_SEEDS), "control_seeds": list(CONTROL_SEEDS),
                "flip_seed": study41.SEED, "flips": study41.FLIPS, "family_bar_all": bar_all,
                "family_bar_stage1": bar_stage1, "control_win_need": CONTROL_WIN_NEED,
                "seconds": {"build": built, "strategy": strategy_time - built, "order_random": random_time - strategy_time,
                            "controls": control_time - random_time, "total": time.time() - started}}
    (RESULTS / "run_info.json").write_bytes(json.dumps(run_info, ensure_ascii=False, indent=1, default=str)
                                            .encode("utf-8"))
    (RESULTS / "run_command.txt").write_bytes(
        b"py -I -X utf8 research/studies/43_devscale_real_rules/reproduce.py\n"
        b"py -I -X utf8 research/studies/43_devscale_real_rules/run_stages.py\n")
    print(pd.DataFrame(judge).to_string())
    print(f"전체 {time.time() - started:.0f}초")


if __name__ == "__main__":
    main()
