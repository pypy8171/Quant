"""백테스트 판정 통계 한 벌 — scipy 없이 numpy·pandas만 쓴다.

스터디 13(`research/studies/13_trendx_gate/stats_util.py`)에 있던 t 분포 p값·Benjamini-Hochberg·Spearman을
여기로 옮기고, 리셋 2라운드 판정 양식(`research/RESET_2026-09-19_R2/quant-analyst.md` §2·§5)이 요구하는
뉴이-웨스트 t·블록 부트스트랩·Deflated Sharpe·순위 IC·분위 스프레드·walk-forward 창·격자 강건성·
필요 표본 수·성과 요약을 더했다. 스터디 13·14·16은 `stats_util.py`를 거쳐 이 파일을 쓴다.

규약
  - 수익률은 소수(0.01 = 1%)로 받는다. 퍼센트 단위 열은 이름에 `_percent`를 붙여 돌려준다.
  - 난수는 seed 인자로만 정한다(기본 20260919). 같은 입력·같은 seed면 같은 숫자가 나온다.
  - 표본이 모자라면 예외 대신 nan을 돌려준다. 호출한 쪽이 "표본 부족 — 결론 보류"를 적는다.
  - 열 이름은 `metrics.json` v2(quant-analyst.md §2-5)와 같게 둔다.

재현: py -m pytest PYQuant/tests/test_stats.py -q
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass
from typing import Callable, Sequence

import numpy as np
import pandas as pd

DEFAULT_SEED = 20260919
DEFAULT_DRAWS = 2000
EULER_MASCHERONI = 0.5772156649015329


# ── t 분포 (스터디 13 이관) ─────────────────────────────────────────────────

def _beta_continued_fraction(alpha: float, beta: float, point: float) -> float:
    """정규화 불완전 베타의 연분수 부분. Numerical Recipes 6.4 betacf를 그대로 옮겼다."""
    max_iterations, epsilon, floor = 300, 3e-14, 1e-300
    alpha_plus_beta, alpha_plus_one, alpha_minus_one = alpha + beta, alpha + 1.0, alpha - 1.0
    numerator_c, denominator_d = 1.0, 1.0 - alpha_plus_beta * point / alpha_plus_one
    if abs(denominator_d) < floor:
        denominator_d = floor
    denominator_d = 1.0 / denominator_d
    result = denominator_d
    for iteration in range(1, max_iterations + 1):
        twice = 2 * iteration
        term = iteration * (beta - iteration) * point / ((alpha_minus_one + twice) * (alpha + twice))
        denominator_d = 1.0 + term * denominator_d
        if abs(denominator_d) < floor:
            denominator_d = floor
        numerator_c = 1.0 + term / numerator_c
        if abs(numerator_c) < floor:
            numerator_c = floor
        denominator_d = 1.0 / denominator_d
        result *= denominator_d * numerator_c
        term = -(alpha + iteration) * (alpha_plus_beta + iteration) * point / ((alpha + twice) * (alpha_plus_one + twice))
        denominator_d = 1.0 + term * denominator_d
        if abs(denominator_d) < floor:
            denominator_d = floor
        numerator_c = 1.0 + term / numerator_c
        if abs(numerator_c) < floor:
            numerator_c = floor
        denominator_d = 1.0 / denominator_d
        delta = denominator_d * numerator_c
        result *= delta
        if abs(delta - 1.0) < epsilon:
            break
    return result


def betainc(alpha: float, beta: float, point: float) -> float:
    """정규화 불완전 베타 I_x(a, b)."""
    if point <= 0.0:
        return 0.0
    if point >= 1.0:
        return 1.0
    log_beta = math.lgamma(alpha + beta) - math.lgamma(alpha) - math.lgamma(beta)
    front = math.exp(log_beta + alpha * math.log(point) + beta * math.log(1.0 - point))
    if point < (alpha + 1.0) / (alpha + beta + 2.0):
        return front * _beta_continued_fraction(alpha, beta, point) / alpha
    return 1.0 - front * _beta_continued_fraction(beta, alpha, 1.0 - point) / beta


def t_sf2(t_statistic: float, degrees_of_freedom: float) -> float:
    """Student t 양측 p값 P(|T| ≥ |t|)."""
    if not np.isfinite(t_statistic) or degrees_of_freedom <= 0:
        return float("nan")
    point = degrees_of_freedom / (degrees_of_freedom + t_statistic * t_statistic)
    return betainc(degrees_of_freedom / 2.0, 0.5, point)


# ── 정규분포 (Deflated Sharpe용) ────────────────────────────────────────────

def normal_cumulative_distribution(point: float) -> float:
    """표준정규 누적분포 Φ(x)."""
    return 0.5 * math.erfc(-point / math.sqrt(2.0))


def normal_ppf(probability: float) -> float:
    """표준정규 분위수 Φ^{-1}(p). Acklam 근사 뒤 뉴턴 한 걸음으로 다듬는다(오차 1e-12 이하)."""
    if not (0.0 < probability < 1.0):
        return float("nan")
    coefficients_a = (-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                      1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00)
    coefficients_b = (-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                      6.680131188771972e+01, -1.328068155288572e+01)
    coefficients_c = (-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                      -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00)
    coefficients_d = (7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                      3.754408661907416e+00)
    lower_break = 0.02425
    if probability < lower_break:
        tail = math.sqrt(-2.0 * math.log(probability))
        estimate = (((((coefficients_c[0] * tail + coefficients_c[1]) * tail + coefficients_c[2]) * tail
                      + coefficients_c[3]) * tail + coefficients_c[4]) * tail + coefficients_c[5]) / \
                   ((((coefficients_d[0] * tail + coefficients_d[1]) * tail + coefficients_d[2]) * tail
                     + coefficients_d[3]) * tail + 1.0)
    elif probability > 1.0 - lower_break:
        tail = math.sqrt(-2.0 * math.log(1.0 - probability))
        estimate = -(((((coefficients_c[0] * tail + coefficients_c[1]) * tail + coefficients_c[2]) * tail
                       + coefficients_c[3]) * tail + coefficients_c[4]) * tail + coefficients_c[5]) / \
                    ((((coefficients_d[0] * tail + coefficients_d[1]) * tail + coefficients_d[2]) * tail
                      + coefficients_d[3]) * tail + 1.0)
    else:
        centered = probability - 0.5
        square = centered * centered
        estimate = (((((coefficients_a[0] * square + coefficients_a[1]) * square + coefficients_a[2]) * square
                      + coefficients_a[3]) * square + coefficients_a[4]) * square + coefficients_a[5]) * centered / \
                   (((((coefficients_b[0] * square + coefficients_b[1]) * square + coefficients_b[2]) * square
                      + coefficients_b[3]) * square + coefficients_b[4]) * square + 1.0)
    # 뉴턴 한 걸음: x ← x − (Φ(x) − p) / φ(x)
    density = math.exp(-0.5 * estimate * estimate) / math.sqrt(2.0 * math.pi)
    if density > 0.0:
        estimate -= (normal_cumulative_distribution(estimate) - probability) / density
    return estimate


# ── 1표본 t · 뉴이-웨스트 t ─────────────────────────────────────────────────

@dataclass(frozen=True)
class TestResult:
    """평균 0 검정 결과. sample_count<2면 t·p·standard_error는 nan."""
    mean: float
    t_statistic: float
    p_value: float
    sample_count: int
    standard_error: float

    def as_tuple(self) -> tuple[float, float, float, int]:
        """스터디 13·14·16이 쓰던 (mean, t, p, n) 네 값."""
        return (self.mean, self.t_statistic, self.p_value, self.sample_count)


def _finite_values(values) -> np.ndarray:
    array = np.asarray(values, dtype=float).ravel()
    return array[np.isfinite(array)]


def one_sample_t(values) -> TestResult:
    """평균 0 가설의 1표본 t(스터디 13 이관). 표준편차 0이면 t·p는 nan."""
    finite = _finite_values(values)
    count = int(len(finite))
    if count < 2:
        return TestResult(float(finite.mean()) if count else float("nan"), float("nan"), float("nan"), count, float("nan"))
    mean = float(finite.mean())
    standard_deviation = float(finite.std(ddof=1))
    if standard_deviation == 0.0:
        return TestResult(mean, float("nan"), float("nan"), count, 0.0)
    standard_error = standard_deviation / math.sqrt(count)
    t_statistic = mean / standard_error
    return TestResult(mean, float(t_statistic), t_sf2(t_statistic, count - 1), count, standard_error)


def newey_west_t(series: pd.Series | Sequence[float], lag: int) -> TestResult:
    """자기상관을 보정한 평균 0 검정(뉴이-웨스트, Bartlett 가중).

    장기분산 = gamma_0 + 2 · Σ_{k=1..lag} (1 − k/(lag+1)) · gamma_k,
    gamma_k = Σ_t (x_t − 평균)(x_{t−k} − 평균) / (n − 1).
    분모를 n−1로 두어 lag=0이면 one_sample_t와 같은 값이 나온다. 결측은 빼고 순서는 유지한다.
    """
    finite = _finite_values(series)
    count = int(len(finite))
    if count < 2:
        return TestResult(float(finite.mean()) if count else float("nan"), float("nan"), float("nan"), count, float("nan"))
    if lag < 0:
        raise ValueError("lag는 0 이상")

    mean = float(finite.mean())
    residual = finite - mean
    effective_lag = min(lag, count - 1)
    long_run_variance = float(residual @ residual) / (count - 1)
    for shift in range(1, effective_lag + 1):
        weight = 1.0 - shift / (effective_lag + 1.0)
        autocovariance = float(residual[shift:] @ residual[:-shift]) / (count - 1)
        long_run_variance += 2.0 * weight * autocovariance

    if long_run_variance <= 0.0:
        return TestResult(mean, float("nan"), float("nan"), count, 0.0)
    standard_error = math.sqrt(long_run_variance / count)
    t_statistic = mean / standard_error
    return TestResult(mean, float(t_statistic), t_sf2(t_statistic, count - 1), count, standard_error)


# ── 블록 부트스트랩 ─────────────────────────────────────────────────────────

def _nearest_rank_percentile(values: Sequence[float], fraction: float) -> float:
    """가까운 순위 백분위. 스터디 17(`scripts/exit_ev.py`)과 같은 정의라 그 표를 그대로 재현한다."""
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return float(ordered[index])


def block_bootstrap_ci(values: pd.Series | Sequence[float], block_length: int, draws: int = DEFAULT_DRAWS,
                       seed: int = DEFAULT_SEED, statistic: Callable[[np.ndarray], float] = np.mean,
                       alpha: float = 0.05) -> tuple[float, float]:
    """원형 블록 부트스트랩 신뢰구간(하한, 상한). 월 시계열은 블록 6, 일 시계열은 블록 20.

    블록 시작점을 복원추출하고 끝을 넘으면 앞으로 이어 붙인다(원형). 표본이 2개 미만이면 (nan, nan).
    왕복 원장처럼 같은 날 관측이 묶이는 자료는 day_block_bootstrap_ci를 쓴다.
    """
    finite = _finite_values(values)
    count = int(len(finite))
    if count < 2 or draws <= 0:
        return (float("nan"), float("nan"))
    block_length = max(1, min(int(block_length), count))
    block_count = math.ceil(count / block_length)
    generator = np.random.default_rng(seed)
    offsets = np.arange(block_length)
    statistics_drawn = np.empty(draws)
    for draw_index in range(draws):
        starts = generator.integers(0, count, size=block_count)
        indices = ((starts[:, None] + offsets[None, :]) % count).ravel()[:count]
        statistics_drawn[draw_index] = float(statistic(finite[indices]))
    return (_nearest_rank_percentile(statistics_drawn, alpha / 2.0),
            _nearest_rank_percentile(statistics_drawn, 1.0 - alpha / 2.0))


def day_block_bootstrap_ci(frame: pd.DataFrame, day_column: str, value_column: str, weight_column: str | None,
                           draws: int = DEFAULT_DRAWS, seed: int = DEFAULT_SEED,
                           alpha: float = 0.05) -> tuple[float, float]:
    """거래일을 복원추출하는 부트스트랩(스터디 17 방식) 신뢰구간(하한, 상한).

    weight_column이 있으면 통계량은 Σvalue / Σweight(예: 손익 / 매입원가 = 가중 수익률), 없으면 value 평균.
    같은 날 왕복은 지수 베타를 공유해 독립이 아니므로 날 단위로 뽑는다. 거래일이 2일 미만이면 (nan, nan).
    난수는 `random.Random(seed).choice`를 정렬한 날짜 목록에 날 수만큼 부르는 순서라 `scripts/exit_ev.py`와 같은 표본이 나온다.
    """
    if frame.empty or draws <= 0:
        return (float("nan"), float("nan"))
    grouped = frame.groupby(day_column)
    day_value_sum = grouped[value_column].sum()
    day_weight_sum = grouped[weight_column].sum() if weight_column else grouped[value_column].count().astype(float)
    days = sorted(day_value_sum.index.tolist())
    if len(days) < 2:
        return (float("nan"), float("nan"))

    value_by_day = {day: float(day_value_sum[day]) for day in days}
    weight_by_day = {day: float(day_weight_sum[day]) for day in days}
    generator = random.Random(seed)
    statistics_drawn: list[float] = []
    for _ in range(draws):
        chosen = [generator.choice(days) for _ in days]
        value_total = sum(value_by_day[day] for day in chosen)
        weight_total = sum(weight_by_day[day] for day in chosen)
        statistics_drawn.append(value_total / weight_total if weight_total > 0.0 else 0.0)
    return (_nearest_rank_percentile(statistics_drawn, alpha / 2.0),
            _nearest_rank_percentile(statistics_drawn, 1.0 - alpha / 2.0))


# ── 다중검정 ────────────────────────────────────────────────────────────────

def benjamini_hochberg_qvalues(p_values: Sequence[float]) -> list[float]:
    """Benjamini–Hochberg q값(스터디 13 이관). nan은 그대로 둔다."""
    values = np.asarray(p_values, dtype=float)
    finite_mask = np.isfinite(values)
    result = np.full_like(values, np.nan)
    finite_values = values[finite_mask]
    count = len(finite_values)
    if count == 0:
        return result.tolist()
    order = np.argsort(finite_values)
    ranked = finite_values[order] * count / (np.arange(count) + 1)
    # 뒤에서부터 누적 최소로 단조성을 맞춘다
    ranked = np.minimum.accumulate(ranked[::-1])[::-1]
    ranked_values = np.empty(count)
    ranked_values[order] = np.minimum(ranked, 1.0)
    result[finite_mask] = ranked_values
    return result.tolist()




def deflated_sharpe_p(observed_sharpe: float, trials: int, sample_count: int, skewness: float, kurtosis: float,
                      sharpe_variance_across_trials: float) -> float:
    """Deflated Sharpe(Bailey & López de Prado 2014)의 p값 = 1 − PSR(SR ≥ SR0).

    observed_sharpe는 표본 단위(연율화 전) 샤프, kurtosis는 정규분포가 3인 값(초과첨도 아님).
    SR0 = sqrt(V) · ((1−γ)·Φ^{-1}(1−1/N) + γ·Φ^{-1}(1−1/(N·e))), γ = 오일러 상수. trials=1이면 SR0=0이라
    보통 샤프 검정(PSR)과 같다. sample_count<2면 nan.
    """
    if sample_count < 2 or not np.isfinite(observed_sharpe):
        return float("nan")
    if trials <= 1 or sharpe_variance_across_trials <= 0.0:
        expected_max_sharpe = 0.0
    else:
        expected_max_sharpe = math.sqrt(sharpe_variance_across_trials) * (
            (1.0 - EULER_MASCHERONI) * normal_ppf(1.0 - 1.0 / trials)
            + EULER_MASCHERONI * normal_ppf(1.0 - 1.0 / (trials * math.e)))
    denominator = 1.0 - skewness * observed_sharpe + (kurtosis - 1.0) / 4.0 * observed_sharpe * observed_sharpe
    if denominator <= 0.0:
        return float("nan")
    z_score = (observed_sharpe - expected_max_sharpe) * math.sqrt(sample_count - 1) / math.sqrt(denominator)
    return 1.0 - normal_cumulative_distribution(z_score)


# ── 팩터 단면 ───────────────────────────────────────────────────────────────

def spearman(left: pd.Series, right: pd.Series) -> float:
    """Spearman ρ(스터디 13 이관). 결측 제거 후 3개 미만이면 nan."""
    paired = pd.concat([left, right], axis=1).dropna()
    if len(paired) < 3:
        return float("nan")
    left_rank = paired.iloc[:, 0].rank()
    right_rank = paired.iloc[:, 1].rank()
    if left_rank.std() == 0 or right_rank.std() == 0:
        return float("nan")
    return float(np.corrcoef(left_rank, right_rank)[0, 1])


def _aligned_panels(scores: pd.DataFrame, forward_returns: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    """날짜×종목 두 패널을 같은 축으로 맞추고 한쪽이라도 결측인 칸은 둘 다 비운다."""
    scores_aligned, returns_aligned = scores.align(forward_returns, join="inner")
    mask = scores_aligned.notna() & returns_aligned.notna()
    return scores_aligned.where(mask), returns_aligned.where(mask)


def rank_ic(scores: pd.DataFrame, forward_returns: pd.DataFrame) -> pd.Series:
    """날짜별 순위 IC(Spearman). 행 = 날짜, 열 = 종목. 같은 날 단면 안에서만 계산하고 종목 3개 미만인 날은 nan."""
    scores_aligned, returns_aligned = _aligned_panels(scores, forward_returns)
    score_rank = scores_aligned.rank(axis=1)
    return_rank = returns_aligned.rank(axis=1)
    score_centered = score_rank.sub(score_rank.mean(axis=1), axis=0)
    return_centered = return_rank.sub(return_rank.mean(axis=1), axis=0)
    covariance = (score_centered * return_centered).sum(axis=1)
    scale = np.sqrt((score_centered ** 2).sum(axis=1) * (return_centered ** 2).sum(axis=1))
    count = scores_aligned.notna().sum(axis=1)
    result = covariance / scale.where(scale > 0)
    result[count < 3] = np.nan
    return result.rename("rank_ic")


def ic_information_ratio(ic_series: pd.Series) -> float:
    """IC 평균 / IC 표준편차. 연율화하지 않는다(월 IC면 월 ICIR). 2개 미만이면 nan."""
    finite = _finite_values(ic_series)
    if len(finite) < 2:
        return float("nan")
    standard_deviation = float(finite.std(ddof=1))
    return float(finite.mean() / standard_deviation) if standard_deviation > 0 else float("nan")


def quintile_spread(scores: pd.DataFrame, forward_returns: pd.DataFrame, quantiles: int = 5) -> pd.DataFrame:
    """날짜별 분위 포트폴리오 수익. 열: q1..qK(동일가중 평균 수익), spread(qK − q1), monotonic_spearman(분위 순위 vs 평균 수익).

    q1이 점수 최하, qK가 최상. 분위는 그날 단면의 백분위 순위를 K칸으로 나눈다. 종목이 K개 미만인 날은 nan.
    """
    if quantiles < 2:
        raise ValueError("quantiles는 2 이상")
    scores_aligned, returns_aligned = _aligned_panels(scores, forward_returns)
    percentile_rank = scores_aligned.rank(axis=1, pct=True)
    bucket = np.ceil(percentile_rank * quantiles).clip(lower=1, upper=quantiles)
    count = scores_aligned.notna().sum(axis=1)

    columns: dict[str, pd.Series] = {}
    for quantile_index in range(1, quantiles + 1):
        member = returns_aligned.where(bucket == quantile_index)
        columns[f"q{quantile_index}"] = member.mean(axis=1)
    result = pd.DataFrame(columns, index=scores_aligned.index)
    result[count < quantiles] = np.nan
    result["spread"] = result[f"q{quantiles}"] - result["q1"]
    bucket_positions = pd.Series(range(1, quantiles + 1), index=[f"q{index}" for index in range(1, quantiles + 1)])
    result["monotonic_spearman"] = result[bucket_positions.index].apply(
        lambda row: spearman(bucket_positions, row) if row.notna().all() else float("nan"), axis=1)
    return result


def quintile_summary(spread_frame: pd.DataFrame) -> dict[str, float]:
    """quintile_spread 결과를 기간 전체로 요약 — 분위별 평균, 스프레드 평균, 시간 평균 수익에 대한 단조 Spearman."""
    quantile_columns = [column for column in spread_frame.columns if column.startswith("q") and column[1:].isdigit()]
    averaged = spread_frame[quantile_columns].mean()
    positions = pd.Series(range(1, len(quantile_columns) + 1), index=quantile_columns)
    summary = {column: float(averaged[column]) for column in quantile_columns}
    summary["spread_mean"] = float(spread_frame["spread"].mean())
    summary["quintile_monotonic_spearman"] = spearman(positions, averaged)
    summary["sample_count"] = int(spread_frame["spread"].notna().sum())
    return summary


# ── 분리검증·강건성·표본 ───────────────────────────────────────────────────

def walk_forward_windows(start: str, end: str, train_years: int = 3,
                         test_years: int = 1) -> list[tuple[str, str, str, str]]:
    """학습 train_years → 검증 test_years를 test_years씩 굴린 창 목록 [(train_start, train_end, test_start, test_end)].

    검증 창은 서로 겹치지 않고 마지막 창의 test_end는 end로 잘린다(부분 창). 2015-01-01~2026-09-19면 9창, 2019-01-01~이면 5창.
    """
    if train_years < 1 or test_years < 1:
        raise ValueError("train_years·test_years는 1 이상")
    start_stamp = pd.Timestamp(start)
    end_stamp = pd.Timestamp(end)
    one_day = pd.Timedelta(days=1)
    windows: list[tuple[str, str, str, str]] = []
    train_start = start_stamp
    while True:
        train_end = train_start + pd.DateOffset(years=train_years) - one_day
        test_start = train_end + one_day
        if test_start > end_stamp:
            break
        test_end = min(test_start + pd.DateOffset(years=test_years) - one_day, end_stamp)
        windows.append((train_start.strftime("%Y-%m-%d"), train_end.strftime("%Y-%m-%d"),
                        test_start.strftime("%Y-%m-%d"), test_end.strftime("%Y-%m-%d")))
        train_start = train_start + pd.DateOffset(years=test_years)
    return windows


def walk_forward_verdict(test_excess_by_window: Sequence[float]) -> tuple[int, int, bool]:
    """(양의 창 수, 전체 창 수, 과반 여부). 과반 = positive > total // 2. nan 창은 전체에 세되 양수로 치지 않는다."""
    total = len(test_excess_by_window)
    positive = sum(1 for value in test_excess_by_window if np.isfinite(value) and value > 0.0)
    return positive, total, (total > 0 and positive > total // 2)


def grid_robustness(grid: dict[tuple, float], center: tuple) -> tuple[int, int, float]:
    """±1 격자 강건성 — (이웃 중 >0 개수, 이웃 수, 중심값 / 격자 최댓값).

    이웃은 축마다 정렬된 값에서 한 칸 앞·뒤, 다른 축은 중심 그대로. 격자에 없는 칸은 세지 않는다.
    최댓값이 0 이하이거나 중심이 없으면 비율은 nan.
    """
    if center not in grid:
        return 0, 0, float("nan")
    axis_count = len(center)
    axis_values = [sorted({key[axis] for key in grid}) for axis in range(axis_count)]
    neighbors: list[tuple] = []
    for axis in range(axis_count):
        values = axis_values[axis]
        position = values.index(center[axis])
        for step in (-1, 1):
            neighbor_position = position + step
            if 0 <= neighbor_position < len(values):
                candidate = tuple(values[neighbor_position] if other == axis else center[other] for other in range(axis_count))
                if candidate in grid:
                    neighbors.append(candidate)

    positive = sum(1 for key in neighbors if np.isfinite(grid[key]) and grid[key] > 0.0)
    finite_values = [value for value in grid.values() if np.isfinite(value)]
    best = max(finite_values) if finite_values else float("nan")
    center_value = grid[center]
    ratio = center_value / best if np.isfinite(best) and best > 0.0 and np.isfinite(center_value) else float("nan")
    return positive, len(neighbors), float(ratio)


def required_sample_count(mean: float, standard_deviation: float, target_t: float = 2.5) -> int:
    """t = target_t를 얻는 데 필요한 표본 수 ceil((target_t · sd / mean)²). mean=0이면 −1."""
    if mean == 0.0 or not np.isfinite(mean) or not np.isfinite(standard_deviation):
        return -1
    return int(math.ceil((target_t * standard_deviation / mean) ** 2))


# ── 성과 요약 ───────────────────────────────────────────────────────────────

def performance_summary(daily_excess: pd.Series | Sequence[float], periods_per_year: int = 252) -> dict:
    """수익률 시계열(소수) → metrics.json v2 열 이름의 성과 요약.

    sharpe = 평균 / 표준편차(ddof=1) · sqrt(periods_per_year). sortino는 분모가 하방 편차(0 아래 수익의 제곱평균 제곱근).
    max_drawdown_percent는 누적곱 자산곡선의 고점 대비 최대 낙폭(%, 양수), 시작점 1.0을 고점에 넣는다.
    drawdown_recovery_days는 고점에서 그 고점을 다시 넘기까지 가장 길었던 기간(표본 단위, 끝까지 못 넘으면 끝까지).
    annual_return은 복리 연율(CAGR), mean_return_annual은 평균 × periods_per_year. calmar = annual_return / MDD.
    표본 2개 미만이면 sharpe·sortino·calmar는 nan.
    """
    returns = _finite_values(daily_excess)
    count = int(len(returns))
    nan = float("nan")
    if count == 0:
        return {"sample_count": 0, "total_return_percent": nan, "annual_return": nan, "mean_return_annual": nan,
                "sharpe": nan, "sortino": nan, "max_drawdown_percent": nan, "drawdown_recovery_days": 0, "calmar": nan}

    equity = np.concatenate(([1.0], np.cumprod(1.0 + returns)))
    peak = np.maximum.accumulate(equity)
    drawdown = (peak - equity) / peak
    max_drawdown_percent = float(drawdown.max() * 100.0)

    longest_recovery = 0
    current_length = 0
    for point in drawdown[1:]:
        if point > 0.0:
            current_length += 1
            longest_recovery = max(longest_recovery, current_length)
        else:
            current_length = 0

    total_return = float(equity[-1] - 1.0)
    annual_return = float(equity[-1] ** (periods_per_year / count) - 1.0) if equity[-1] > 0 else -1.0
    mean_return_annual = float(returns.mean() * periods_per_year)

    sharpe = sortino = nan
    if count >= 2:
        standard_deviation = float(returns.std(ddof=1))
        sharpe = float(returns.mean() / standard_deviation * math.sqrt(periods_per_year)) if standard_deviation > 0 else nan
        downside = np.minimum(returns, 0.0)
        downside_deviation = float(np.sqrt(np.mean(downside * downside)))
        sortino = float(returns.mean() / downside_deviation * math.sqrt(periods_per_year)) if downside_deviation > 0 else nan
    calmar = float(annual_return / (max_drawdown_percent / 100.0)) if max_drawdown_percent > 0 and count >= 2 else nan

    return {
        "sample_count": count,
        "total_return_percent": total_return * 100.0,
        "annual_return": annual_return,
        "mean_return_annual": mean_return_annual,
        "sharpe": sharpe,
        "sortino": sortino,
        "max_drawdown_percent": max_drawdown_percent,
        "drawdown_recovery_days": int(longest_recovery),
        "calmar": calmar,
    }
