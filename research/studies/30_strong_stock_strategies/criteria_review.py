"""
스터디 30 합격선 점검 — CRITERIA_REVIEW.md의 숫자를 만드는 스크립트.

SPEC.md·README.md·b_*.tsv는 읽기만 하고 고치지 않는다. data/ 아래는 열지 않는다(적재 중).
산출:
  research/studies/30_strong_stock_strategies/criteria_review.json  B 검출력·지수 대비·비용 손익분기·다중검정 문턱·A 틱 비용
  research/studies/30_strong_stock_strategies/a_power.tsv           A 합격선 통과 확률 모의실험(★가정 분포)

재실행(저장소 루트, 약 1분): py -X utf8 research/studies/30_strong_stock_strategies/criteria_review.py
"""
from __future__ import annotations

import json
import math
from pathlib import Path
from statistics import NormalDist

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[3]
STUDY = Path(__file__).resolve().parent
PERIODS = STUDY / "b_periods.tsv"
WALKFORWARD = STUDY / "b_walkforward.tsv"
KOSPI_CACHE = REPO / "PYQuant" / ".index_cache" / "idx__KS11_1985-01-01_2026-08-15_adj.parquet"
REGIME_SCORES = REPO / "research" / "studies" / "26_regime_threshold_history" / "daily_scores.tsv"
BARS = REPO / "PYQuant" / "data" / "bars_all_pit_v2.parquet"

NORMAL = NormalDist()
SEED = 0


# ---------------------------------------------------------------- 공통 통계

def newey_west_t(series: np.ndarray, lags: int = 3) -> float:
    """평균의 Newey-West t(Bartlett 가중). backtest_b.py와 같은 정의."""
    values = np.asarray(series, dtype=float)
    count = len(values)
    demeaned = values - values.mean()
    long_run_variance = demeaned @ demeaned / count
    for lag in range(1, lags + 1):
        weight = 1.0 - lag / (lags + 1)
        long_run_variance += 2.0 * weight * (demeaned[lag:] @ demeaned[:-lag]) / count

    return float(values.mean() / math.sqrt(long_run_variance / count))


def binomial_tail(trials: int, at_least: int) -> float:
    """동전 던지기(p=0.5)로 trials번 중 at_least번 이상 나올 확률."""
    return sum(math.comb(trials, wins) for wins in range(at_least, trials + 1)) / 2 ** trials


def sidak_one_sided_t(alpha: float, tests: int) -> float:
    """한쪽 alpha를 tests번 독립 검정으로 나눴을 때의 z 문턱(시닥)."""
    per_test = 1.0 - (1.0 - alpha) ** (1.0 / tests)
    return NORMAL.inv_cdf(1.0 - per_test)


# ---------------------------------------------------------------- B: 이어 붙인 월 계열

def stitched_walkforward(periods: pd.DataFrame, walkforward: pd.DataFrame) -> pd.DataFrame:
    """b_walkforward.tsv가 연도마다 고른 칸의 월 행을 이어 붙인다(README 3절과 같은 계열)."""
    pieces = []
    for row in walkforward.itertuples():
        run_name = "PTH|" + row.cell.replace("/", "|")
        year_rows = periods[(periods["run"] == run_name) & (periods["period"].str.startswith(str(row.year)))]
        pieces.append(year_rows)

    stitched = pd.concat(pieces).sort_values("period").reset_index(drop=True)
    return stitched


def index_closes() -> tuple[pd.Series, pd.Series]:
    """코스피·코스닥 종가. 코스피는 캐시(~2026-08-14) + 스터디 26 일별 종가, 코스닥은 FinanceDataReader + 스터디 26."""
    kospi_cache = pd.read_parquet(KOSPI_CACHE)
    kospi = pd.Series(kospi_cache["close"].values, index=pd.to_datetime(kospi_cache["date"]))
    regime = pd.read_csv(REGIME_SCORES, sep="\t", parse_dates=["date"]).set_index("date")
    kospi = pd.concat([kospi, regime["kospi_close"][regime.index > kospi.index.max()]]).sort_index()

    try:
        import FinanceDataReader as finance_data_reader
        kosdaq_frame = finance_data_reader.DataReader("KQ11", "2009-12-01", "2026-09-30")
        kosdaq = kosdaq_frame["Close"].astype(float)
        kosdaq.index = pd.to_datetime(kosdaq.index)
    except Exception:  # 네트워크가 없으면 스터디 26(2015~)만 쓴다
        kosdaq = pd.Series(dtype=float)

    kosdaq = pd.concat([kosdaq, regime["kosdaq_close"][regime.index > (kosdaq.index.max() if len(kosdaq) else pd.Timestamp("1900-01-01"))]]).sort_index()
    return kospi.dropna(), kosdaq.dropna()


def period_return(closes: pd.Series, start: str, end: str) -> float:
    """start 종가 → end 종가 수익률. 그날 값이 없으면 직전 값(asof)."""
    start_close = closes.asof(pd.Timestamp(start))
    end_close = closes.asof(pd.Timestamp(end))
    return float(end_close / start_close - 1.0)


def annual_summary(net: np.ndarray, reference: np.ndarray, months_per_year: float = 12.0) -> dict:
    excess = net - reference
    years = len(net) / months_per_year
    return {
        "months": int(len(net)),
        "cagr_strategy_pct": round((np.prod(1 + net) ** (1 / years) - 1) * 100, 2),
        "cagr_reference_pct": round((np.prod(1 + reference) ** (1 / years) - 1) * 100, 2),
        "ann_excess_arith_pct": round(excess.mean() * 12 * 100, 2),
        "tracking_error_ann_pct": round(excess.std(ddof=1) * math.sqrt(12) * 100, 2),
        "nw3_t_excess": round(newey_west_t(excess), 2),
    }


def capm_alpha(net: np.ndarray, market: np.ndarray) -> dict:
    """net = a + b × market. a의 NW(3) t는 잔차 + a로 다시 잰다(근사)."""
    design = np.column_stack([np.ones_like(market), market])
    coefficients, *_ = np.linalg.lstsq(design, net, rcond=None)
    residual = net - design @ coefficients
    alpha_series = residual + coefficients[0]
    return {
        "alpha_ann_pct": round(coefficients[0] * 12 * 100, 2),
        "beta": round(float(coefficients[1]), 2),
        "alpha_nw3_t": round(newey_west_t(alpha_series), 2),
    }


def power_numbers(excess_monthly: np.ndarray) -> dict:
    """관측된 월 초과수익의 실질 흔들림(NW 표준오차로 역산)으로 검출력을 잰다."""
    months = len(excess_monthly)
    years = months / 12.0
    mean_monthly = excess_monthly.mean()
    t_value = newey_west_t(excess_monthly)
    effective_monthly_standard_deviation = mean_monthly / t_value * math.sqrt(months)
    tracking_error = effective_monthly_standard_deviation * math.sqrt(12)
    information_ratio = mean_monthly * 12 / tracking_error
    z_alpha = 1.96
    z_power = NORMAL.inv_cdf(0.80)
    return {
        "months": months,
        "years": round(years, 2),
        "ann_excess_pct": round(mean_monthly * 12 * 100, 2),
        "nw3_t": round(t_value, 2),
        "effective_tracking_error_ann_pct": round(tracking_error * 100, 2),
        "information_ratio": round(information_ratio, 3),
        "years_needed_for_t2_at_this_ir": round((2.0 / information_ratio) ** 2, 1),
        "years_needed_for_80pct_power_at_this_ir": round(((z_alpha + z_power) / information_ratio) ** 2, 1),
        "min_ann_excess_for_t2_pct": round(2.0 * tracking_error / math.sqrt(years) * 100, 2),
        "min_ann_excess_80pct_power_pct": round((z_alpha + z_power) * tracking_error / math.sqrt(years) * 100, 2),
        "t_expected_if_true_excess_3pp": round(0.03 / (tracking_error / math.sqrt(years)), 2),
    }


def b_review() -> dict:
    periods = pd.read_csv(PERIODS, sep="\t", dtype={"period": str})
    walkforward = pd.read_csv(WALKFORWARD, sep="\t")
    stitched = stitched_walkforward(periods, walkforward)
    kospi, kosdaq = index_closes()

    stitched["kospi"] = [period_return(kospi, row.start, row.end) for row in stitched.itertuples()]
    stitched["kosdaq"] = [period_return(kosdaq, row.start, row.end) for row in stitched.itertuples()]
    net = stitched["net"].to_numpy()
    bench = stitched["bench"].to_numpy()
    gross = stitched["gross"].to_numpy()

    cost_monthly = gross - net
    gross_excess = gross - bench
    result = {
        "stitched_check": {"months": len(stitched), "ann_excess_pct": round((net - bench).mean() * 1200, 2)},
        "power_walkforward": power_numbers(net - bench),
        "power_by_cell": {},
        "vs_equal_weight_universe": annual_summary(net, bench),
        "vs_kospi": annual_summary(net, stitched["kospi"].to_numpy()),
        "vs_kosdaq": annual_summary(net, stitched["kosdaq"].to_numpy()),
        "equal_weight_universe_vs_kospi": annual_summary(bench, stitched["kospi"].to_numpy()),
        "capm_vs_kospi": capm_alpha(net, stitched["kospi"].to_numpy()),
        "capm_equal_weight_universe_vs_kospi": capm_alpha(bench, stitched["kospi"].to_numpy()),
        "cost_breakeven": {
            "gross_excess_ann_pct": round(gross_excess.mean() * 1200, 2),
            "cost_ann_pct": round(cost_monthly.mean() * 1200, 2),
            "breakeven_cost_multiple": round(gross_excess.mean() / cost_monthly.mean(), 2),
            "net_excess_at_2x_cost_ann_pct": round((gross_excess - 2 * cost_monthly).mean() * 1200, 2),
            "nw3_t_gross_excess": round(newey_west_t(gross_excess), 2),
        },
        "window_majority_coin_flip": {
            f">={wins}/16": round(binomial_tail(16, wins), 4) for wins in (9, 10, 11, 12, 13)
        },
    }

    for cell in ("N20/F10", "N20/F30", "N30/F10", "N30/F30", "N50/F10", "N50/F30"):
        rows = periods[(periods["run"] == "PTH|" + cell.replace("/", "|")) & (periods["period"] >= "2010-01")]
        result["power_by_cell"][cell] = power_numbers((rows["net"] - rows["bench"]).to_numpy())

    stitched[["period", "start", "end", "net", "bench", "kospi", "kosdaq"]].to_csv(
        STUDY / "b_stitched_vs_index.tsv", sep="\t", index=False, float_format="%.6f")
    return result


# ---------------------------------------------------------------- 다중검정 문턱

def multiple_testing() -> dict:
    alpha = 0.025   # 한쪽 2.5% ≈ t 1.96
    return {
        "one_sided_alpha": alpha,
        "single_prespecified_cell": round(NORMAL.inv_cdf(1 - alpha), 2),
        "pick_best_of_6": round(sidak_one_sided_t(alpha, 6), 2),
        "pick_best_of_27": round(sidak_one_sided_t(alpha, 27), 2),
        "pick_best_of_27_plus_ablation": round(sidak_one_sided_t(alpha, 28), 2),
        "false_pass_t2_if_best_of_27_independent": round(1 - (1 - (1 - NORMAL.cdf(2.0))) ** 27, 3),
        "false_pass_t2_if_best_of_6_independent": round(1 - (1 - (1 - NORMAL.cdf(2.0))) ** 6, 3),
    }


# ---------------------------------------------------------------- A: 틱 비용(후보 집합 근사)

def tick_size(price: float) -> int:
    for upper_bound, tick in ((2_000, 1), (5_000, 5), (20_000, 10), (50_000, 50), (200_000, 100), (500_000, 500)):
        if price < upper_bound:
            return tick

    return 1_000


def a_tick_cost() -> dict:
    """2025-09-01~2026-09-29, 전일 종가 ≥ 2,000원, 고가 ≥ 전일 종가 × 1.06 종목일에서 1틱이 가격의 몇 %인가.

    가격은 패널 수정 종가 기준 전일 종가 × 1.08(09:30 후보의 대표 가격)로 잡는다. 이 구간은 분할 보정 영향이 작다.
    """
    bars = pd.read_parquet(BARS, columns=["Date", "High", "Close", "code"])
    bars = bars[bars["Date"] >= "2025-08-01"].sort_values(["code", "Date"])
    bars["prev_close"] = bars.groupby("code")["Close"].shift(1)
    window = bars[(bars["Date"] >= "2025-09-01") & (bars["Date"] <= "2026-09-29")]
    candidates = window[(window["prev_close"] >= 2000) & (window["High"] >= window["prev_close"] * 1.06)]
    reference_price = candidates["prev_close"] * 1.08
    tick_percent = np.array([tick_size(price) for price in reference_price]) / reference_price.to_numpy() * 100
    explicit = 0.015 * 2 + 0.20
    quantiles = np.percentile(tick_percent, [25, 50, 75])
    return {
        "candidate_stock_days": int(len(candidates)),
        "tick_pct_p25_p50_p75": [round(value, 3) for value in quantiles],
        "explicit_roundtrip_pct_2026": explicit,
        "roundtrip_pct_explicit_plus_1tick_each_side_median": round(explicit + 2 * quantiles[1], 3),
        "roundtrip_pct_explicit_plus_stop_2tick_median": round(explicit + 3 * quantiles[1], 3),
        "roundtrip_pct_spec_0.31_plus_1tick_each_side_median": round(0.31 + 2 * quantiles[1], 3),
    }


# ---------------------------------------------------------------- A: 합격선 통과 확률 모의실험

def simulate_r(random_engine: np.random.Generator, count: int, mean_r: float) -> np.ndarray:
    """거래 한 건의 순 R(★가정). 손절 45%(−1.05R), 15:10 손실 청산 15%(−1~0R 균등), 이익 40%(지수분포).

    이익 평균은 전체 평균이 mean_r이 되도록 맞춘다. 측정값이 아니라 SPEC 2.9의 ★예상(승률 35~45%, 손익비 1.8~2.5)을 흉내 낸 것.
    """
    win_mean = (mean_r + 0.45 * 1.05 + 0.15 * 0.5) / 0.40
    kind = random_engine.random(count)
    values = np.where(kind < 0.45, -1.05, 0.0)
    time_loss = (kind >= 0.45) & (kind < 0.60)
    values[time_loss] = -random_engine.random(time_loss.sum())
    wins = kind >= 0.60
    values[wins] = random_engine.exponential(win_mean, wins.sum())
    return values


def profit_factor(values: np.ndarray) -> float:
    losses = -values[values < 0].sum()
    return float(values[values > 0].sum() / losses) if losses > 0 else float("inf")


def max_drawdown_r(values: np.ndarray) -> float:
    curve = np.cumsum(values)
    return float((np.maximum.accumulate(np.concatenate([[0.0], curve]))[1:] - curve).max())


def concentration_top5(values: np.ndarray) -> float:
    """상위 5건 이익 / 순이익. 순이익 ≤ 0이면 무한대(통과 불가)."""
    total = values.sum()
    return float(np.sort(values)[-5:].sum() / total) if total > 0 else float("inf")


def concentration_month(values: np.ndarray, day_index: np.ndarray) -> float:
    """가장 좋은 달(21거래일 묶음) 순이익 / 전체 순이익."""
    total = values.sum()
    if total <= 0:
        return float("inf")

    month_totals = np.bincount(day_index // 21, weights=values)
    return float(month_totals.max() / total)


def one_backtest(random_engine: np.random.Generator, mean_r: float, trades_per_day: float, days: int, validation_days: int) -> dict:
    counts = np.minimum(random_engine.poisson(trades_per_day, days), 4)
    values = simulate_r(random_engine, int(counts.sum()), mean_r)
    day_index = np.repeat(np.arange(days), counts)
    traded_days = np.unique(day_index)
    daily_mean = np.array([values[day_index == day].mean() for day in traded_days]) if len(values) else np.array([])
    day_t = daily_mean.mean() / (daily_mean.std(ddof=1) / math.sqrt(len(daily_mean))) if len(daily_mean) > 2 else 0.0
    validation = values[day_index >= days - validation_days]
    return {
        "n": len(values),
        "n_validation": len(validation),
        "mean_r": values.mean() if len(values) else 0.0,
        "pf": profit_factor(values) if len(values) else 0.0,
        "day_t": day_t,
        "validation_mean": validation.mean() if len(validation) else 0.0,
        "validation_pf": profit_factor(validation) if len(validation) else 0.0,
        "mdd_r": max_drawdown_r(values) if len(values) else 0.0,
        "top5_share": concentration_top5(values),
        "best_month_share": concentration_month(values, day_index),
        "net_ex_top5": float(values.sum() - np.sort(values)[-5:].sum()) if len(values) >= 5 else 0.0,
        "net_ex_best_month": float(values.sum() - np.bincount(day_index // 21, weights=values).max()) if len(values) else 0.0,
    }


def a_power(simulations: int = 2000) -> pd.DataFrame:
    random_engine = np.random.default_rng(SEED)
    rows = []
    for trades_per_day in (0.75, 1.0, 1.5):
        for mean_r in (0.0, 0.1, 0.2, 0.3, 0.4):
            outcomes = pd.DataFrame([one_backtest(random_engine, mean_r, trades_per_day, 262, 125) for _ in range(simulations)])
            first_draft_sample = (outcomes["n"] >= 200) & (outcomes["n_validation"] >= 80)
            first_draft_profit = (outcomes["pf"] >= 1.3) & (outcomes["mean_r"] >= 0.15)
            first_draft_t = outcomes["day_t"] >= 2.0
            first_draft_validation = (outcomes["validation_mean"] > 0) & (outcomes["validation_pf"] >= 1.15)
            first_draft_drawdown = outcomes["mdd_r"] <= 15
            first_draft_concentration = (outcomes["top5_share"] <= 0.40) & (outcomes["best_month_share"] <= 0.50)
            first_draft_all = first_draft_sample & first_draft_profit & first_draft_t & first_draft_validation & first_draft_drawdown
            first_draft_all_with_concentration = first_draft_all & first_draft_concentration
            # 수정안: 이익 층을 t와 겹치지 않게 경제적 하한만 두고(+0.10R), 낙폭은 20R
            revised_profit = outcomes["mean_r"] >= 0.10
            revised_mdd = outcomes["mdd_r"] <= 20
            revised_all = first_draft_sample & revised_profit & first_draft_t & first_draft_validation & revised_mdd
            # 수정안 집중 층: 상위 5건·가장 좋은 달을 빼고도 순손익 > 0
            revised_concentration = (outcomes["net_ex_top5"] > 0) & (outcomes["net_ex_best_month"] > 0)
            revised_all_with_concentration = revised_all & revised_concentration
            rows.append({
                "trades_per_day": trades_per_day,
                "true_mean_r": mean_r,
                "median_n": int(outcomes["n"].median()),
                "pass_sample": round(first_draft_sample.mean(), 3),
                "pass_pf1.3_and_0.15R": round(first_draft_profit.mean(), 3),
                "pass_day_t2": round(first_draft_t.mean(), 3),
                "pass_validation": round(first_draft_validation.mean(), 3),
                "pass_mdd15R": round(first_draft_drawdown.mean(), 3),
                "pass_spec_all_core": round(first_draft_all.mean(), 3),
                "pass_revised_core": round(revised_all.mean(), 3),
                "median_day_t": round(outcomes["day_t"].median(), 2),
                "median_mdd_r": round(outcomes["mdd_r"].median(), 1),
                "pass_top5_le_40pct": round((outcomes["top5_share"] <= 0.40).mean(), 3),
                "pass_best_month_le_50pct": round((outcomes["best_month_share"] <= 0.50).mean(), 3),
                "median_top5_share": round(outcomes["top5_share"].replace(np.inf, np.nan).median(), 2),
                "pass_spec_all_with_concentration": round(first_draft_all_with_concentration.mean(), 3),
                "pass_revised_concentration": round(revised_concentration.mean(), 3),
                "pass_revised_all_with_concentration": round(revised_all_with_concentration.mean(), 3),
            })

    return pd.DataFrame(rows)


def a_analytic() -> dict:
    """거래 단위 근사: R 표준편차 σ(★ 1.8R)에서 n건으로 t 2와 검출력 80%에 필요한 평균."""
    sigma = 1.8
    result = {"sigma_r_assumed": sigma}
    for count in (200, 300, 500):
        result[f"n{count}_mean_r_for_t2"] = round(2.0 * sigma / math.sqrt(count), 3)
        result[f"n{count}_mean_r_for_80pct_power"] = round((1.96 + NORMAL.inv_cdf(0.8)) * sigma / math.sqrt(count), 3)

    for mean_r in (0.15, 0.2, 0.3):
        result[f"n_needed_t2_at_{mean_r}R"] = int(math.ceil((2.0 * sigma / mean_r) ** 2))
        result[f"n_needed_80pct_power_at_{mean_r}R"] = int(math.ceil(((1.96 + NORMAL.inv_cdf(0.8)) * sigma / mean_r) ** 2))

    return result


def main() -> None:
    review = {
        "b": b_review(),
        "multiple_testing": multiple_testing(),
        "a_tick_cost": a_tick_cost(),
        "a_analytic": a_analytic(),
        "seed": SEED,
        "rerun": "py -X utf8 research/studies/30_strong_stock_strategies/criteria_review.py",
    }
    power = a_power()
    power.to_csv(STUDY / "a_power.tsv", sep="\t", index=False)
    (STUDY / "criteria_review.json").write_text(json.dumps(review, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps(review, ensure_ascii=False, indent=1))
    print(power.to_string(index=False))


if __name__ == "__main__":
    main()
