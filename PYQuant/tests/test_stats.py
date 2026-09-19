"""`backtest.stats` 검산 — 리셋 2라운드 판정 양식(`research/RESET_2026-09-19_R2/quant-analyst.md` §5)의 테스트 3개와 골든 재현.

  1) 뉴이-웨스트 lag=0은 1표본 t와 같고(1e-9), AR(1) φ=0.5에서는 lag=6의 |t|가 더 작다.
  2) 스터디 17 `research/studies/17_exit_ev/exit_ev_all.tsv` TRENDX 묶음(수익률 0.07%, CI [−0.19, 0.46])을
     같은 seed·2,000회로 다시 계산해 소수 2자리 일치. 원장(`Quant/build_win/logs/trades_*.csv`, gitignore)이 없으면 건너뛴다.
     17번은 `metrics.json`·자산곡선이 없어 `performance_summary` 골든은 스터디 11
     `research/studies/11_signal_axes/metrics.json`(자산곡선 CSV 있음)으로 대신 잰다 — 엔진 `_curve_stats`와 같은 정의라 1e-6 안에서 같다.
  3) walk-forward 창: 2015~2026-09-19는 9창, 2019~는 5창, 검증 창은 겹치지 않고 마지막 창은 end로 잘린다.
     덤: 무작위 점수 1,000회 rank_ic 평균 0±0.01, BH q<0.1 비율 5% 이하.
  골든: 스터디 13 `monthly_excess.csv` 월 시계열 → `results.tsv`의 t·p(소수 4자리), `q_bh` 열.

실행: py -m pytest PYQuant/tests/test_stats.py -q
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

PYQUANT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PYQUANT_ROOT.parent
sys.path.insert(0, str(PYQUANT_ROOT))

from backtest import stats  # noqa: E402

STUDY_11 = REPO_ROOT / "research" / "studies" / "11_signal_axes"
STUDY_13 = REPO_ROOT / "research" / "studies" / "13_trendx_gate"
STUDY_17 = REPO_ROOT / "research" / "studies" / "17_exit_ev"
LEDGER_DIR = REPO_ROOT / "Quant" / "build_win" / "logs"


# ── 1. 뉴이-웨스트 ─────────────────────────────────────────────────────────

def test_newey_west_matches_one_sample_t_when_lag_zero_and_shrinks_under_autocorrelation():
    generator = np.random.default_rng(20260919)
    white_noise = pd.Series(generator.standard_normal(1000) + 0.05)
    plain = stats.one_sample_t(white_noise)
    newey_zero = stats.newey_west_t(white_noise, lag=0)
    assert abs(plain.t_statistic - newey_zero.t_statistic) < 1e-9
    assert abs(plain.p_value - newey_zero.p_value) < 1e-9
    assert abs(plain.standard_error - newey_zero.standard_error) < 1e-12
    assert plain.sample_count == newey_zero.sample_count == 1000

    phi = 0.5
    shocks = generator.standard_normal(1000)
    autocorrelated = np.empty(1000)
    autocorrelated[0] = shocks[0]
    for index in range(1, 1000):
        autocorrelated[index] = phi * autocorrelated[index - 1] + shocks[index]
    autocorrelated += 0.1
    plain_ar = stats.one_sample_t(autocorrelated)
    newey_ar = stats.newey_west_t(autocorrelated, lag=6)
    assert abs(newey_ar.t_statistic) < abs(plain_ar.t_statistic)
    assert newey_ar.standard_error > plain_ar.standard_error * 1.3


def test_one_sample_t_edge_cases():
    assert math.isnan(stats.one_sample_t([1.0]).t_statistic)
    assert stats.one_sample_t([]).sample_count == 0
    constant = stats.one_sample_t([2.0, 2.0, 2.0])
    assert constant.mean == 2.0 and math.isnan(constant.t_statistic)
    with_nan = stats.one_sample_t([1.0, float("nan"), 3.0, 2.0])
    assert with_nan.sample_count == 3


# ── 2. 스터디 17 골든 (일 블록 부트스트랩) · 스터디 11 골든 (성과 요약) ────

def _study_17_trendx_legs() -> pd.DataFrame:
    """`scripts/exit_ev.py`의 레그 집계를 그대로 빌려 TRENDX 묶음 청산 레그를 만든다."""
    sys.path.insert(0, str(REPO_ROOT / "scripts"))
    import exit_ev  # noqa: E402

    loaded = exit_ev.load_legs(LEDGER_DIR, exit_ev.FIRST_DAY, exit_ev.LAST_DAY)
    rows = [{"day": leg.day, "pnl": leg.pnl, "cost_basis": leg.cost_basis}
            for leg in loaded.legs if leg.family == "TRENDX"]
    return pd.DataFrame(rows)


def test_day_block_bootstrap_reproduces_study_17_cell():
    ledger_files = sorted(LEDGER_DIR.glob("trades_202609*.csv")) if LEDGER_DIR.exists() else []
    if not ledger_files:
        pytest.skip("원장 CSV가 없다(gitignore) — 골든 재현은 원장이 있는 머신에서만")

    result_lines = (STUDY_17 / "RESULT.md").read_text(encoding="utf-8").splitlines()
    family_row = next(line for line in result_lines if line.startswith("| TRENDX |") and "| 301 |" in line)
    cells = [cell.strip() for cell in family_row.strip("|").split("|")]
    golden_return = float(cells[9])
    golden_low, golden_high = (float(value) for value in cells[10].strip("[]").split(","))

    legs = _study_17_trendx_legs()
    assert len(legs) == 301 and legs["day"].nunique() == 8
    observed_return = legs["pnl"].sum() / legs["cost_basis"].sum() * 100.0
    assert round(observed_return, 2) == golden_return

    low, high = stats.day_block_bootstrap_ci(legs, "day", "pnl", "cost_basis", draws=2000, seed=20260919)
    assert round(low * 100.0, 2) == golden_low
    assert round(high * 100.0, 2) == golden_high

    # 같은 seed면 같은 숫자
    assert stats.day_block_bootstrap_ci(legs, "day", "pnl", "cost_basis") == (low, high)


def test_performance_summary_reproduces_study_11_metrics():
    metrics = json.loads((STUDY_11 / "metrics.json").read_text(encoding="utf-8"))
    golden = next(row for row in metrics if row["strategy"] == "횡단면 모멘텀(동일가중)")
    equity = pd.read_csv(REPO_ROOT / golden["equity_csv_path"], encoding="utf-8-sig")["equity"].astype(float)
    returns = equity.pct_change().dropna()

    summary = stats.performance_summary(returns, periods_per_year=252)
    assert summary["sample_count"] == len(returns)
    assert abs(summary["sharpe"] - golden["sharpe"]) < 1e-6
    assert abs(summary["max_drawdown_percent"] - golden["mdd"]) < 1e-6
    assert abs(summary["total_return_percent"] - golden["total_return"]) < 1e-6
    assert summary["drawdown_recovery_days"] > 0
    assert summary["calmar"] < 0


def test_performance_summary_hand_computed():
    returns = pd.Series([0.10, -0.05, 0.02, -0.10, 0.05])
    summary = stats.performance_summary(returns, periods_per_year=252)
    # 자산곡선 1.0 → 1.10 → 1.045 → 1.0659 → 0.95931 → 1.007276, 고점 1.10 대비 최저 0.95931 → 낙폭 12.79%
    assert abs(summary["max_drawdown_percent"] - (1.0 - 0.95931 / 1.10) * 100.0) < 1e-3
    assert summary["drawdown_recovery_days"] == 4  # 1.10 고점 뒤 끝까지 못 넘김 → 4개
    mean = returns.mean()
    assert abs(summary["sharpe"] - mean / returns.std(ddof=1) * math.sqrt(252)) < 1e-12
    downside = np.sqrt(np.mean(np.minimum(returns.to_numpy(), 0.0) ** 2))
    assert abs(summary["sortino"] - mean / downside * math.sqrt(252)) < 1e-12
    assert stats.performance_summary([])["sample_count"] == 0


def test_block_bootstrap_ci_is_deterministic_and_covers_mean():
    generator = np.random.default_rng(7)
    values = pd.Series(generator.standard_normal(500) * 0.01 + 0.001)
    first = stats.block_bootstrap_ci(values, block_length=20, draws=500, seed=20260919)
    second = stats.block_bootstrap_ci(values, block_length=20, draws=500, seed=20260919)
    assert first == second
    assert first[0] < values.mean() < first[1]
    assert stats.block_bootstrap_ci([1.0], block_length=20) == (pytest.approx(float("nan"), nan_ok=True),) * 2


# ── 3. walk-forward 창 · 무작위 점수 ───────────────────────────────────────

def _assert_windows_well_formed(windows: list[tuple[str, str, str, str]], end: str) -> None:
    one_day = pd.Timedelta(days=1)
    for train_start, train_end, test_start, test_end in windows:
        assert pd.Timestamp(test_start) == pd.Timestamp(train_end) + one_day
        assert pd.Timestamp(train_start) < pd.Timestamp(train_end) < pd.Timestamp(test_start) <= pd.Timestamp(test_end)
    for earlier, later in zip(windows, windows[1:]):
        assert pd.Timestamp(later[2]) == pd.Timestamp(earlier[3]) + one_day  # 검증 창은 붙어 있고 겹치지 않는다
    assert windows[-1][3] == end


def test_walk_forward_windows_count_and_no_overlap():
    long_windows = stats.walk_forward_windows("2015-01-01", "2026-09-19")
    assert len(long_windows) == 9
    _assert_windows_well_formed(long_windows, "2026-09-19")
    assert long_windows[0] == ("2015-01-01", "2017-12-31", "2018-01-01", "2018-12-31")
    assert long_windows[-1] == ("2023-01-01", "2025-12-31", "2026-01-01", "2026-09-19")

    short_windows = stats.walk_forward_windows("2019-01-01", "2026-09-19")
    assert len(short_windows) == 5
    _assert_windows_well_formed(short_windows, "2026-09-19")
    assert [window[2][:4] for window in short_windows] == ["2022", "2023", "2024", "2025", "2026"]

    assert stats.walk_forward_windows("2024-01-01", "2026-09-19") == []
    assert stats.walk_forward_verdict([0.1, -0.2, 0.3, 0.05, float("nan")]) == (3, 5, True)
    assert stats.walk_forward_verdict([0.1, -0.2, 0.3, -0.05]) == (2, 4, False)


def test_random_scores_have_zero_rank_ic_and_controlled_false_discoveries():
    generator = np.random.default_rng(20260919)
    dates = pd.date_range("2020-01-01", periods=1000, freq="D")
    codes = [f"{index:06d}" for index in range(50)]
    scores = pd.DataFrame(generator.standard_normal((1000, 50)), index=dates, columns=codes)
    forward_returns = pd.DataFrame(generator.standard_normal((1000, 50)) * 0.02, index=dates, columns=codes)

    ic_series = stats.rank_ic(scores, forward_returns)
    assert len(ic_series) == 1000 and ic_series.notna().all()
    assert abs(ic_series.mean()) < 0.01
    assert abs(stats.ic_information_ratio(ic_series)) < 0.1

    # 날짜별 IC를 각각 하나의 가설로 보고 p값을 만든 뒤 BH — 무작위라면 q<0.1 발견 비율은 5% 이하
    sample_count = 50
    t_values = ic_series * np.sqrt((sample_count - 2) / (1.0 - ic_series ** 2))
    p_values = [stats.t_sf2(float(value), sample_count - 2) for value in t_values]
    adjusted_p_values = stats.benjamini_hochberg_qvalues(p_values)
    discoveries = sum(1 for value in adjusted_p_values if value < 0.1)
    assert discoveries / len(adjusted_p_values) <= 0.05

    spread = stats.quintile_spread(scores, forward_returns, quantiles=5)
    assert list(spread.columns) == ["q1", "q2", "q3", "q4", "q5", "spread", "monotonic_spearman"]
    assert abs(spread["spread"].mean()) < 0.002
    summary = stats.quintile_summary(spread)
    assert summary["sample_count"] == 1000 and abs(summary["quintile_monotonic_spearman"]) <= 1.0


def test_rank_ic_matches_pandas_spearman_and_handles_missing():
    dates = pd.date_range("2024-01-01", periods=3, freq="D")
    scores = pd.DataFrame([[1.0, 2.0, 3.0, 4.0], [4.0, 3.0, 2.0, 1.0], [1.0, np.nan, 3.0, 2.0]],
                          index=dates, columns=list("abcd"))
    forward_returns = pd.DataFrame([[0.1, 0.2, 0.3, 0.4], [0.1, 0.2, 0.3, 0.4], [0.1, 0.2, 0.3, np.nan]],
                                   index=dates, columns=list("abcd"))
    ic_series = stats.rank_ic(scores, forward_returns)
    assert ic_series.iloc[0] == pytest.approx(1.0)
    assert ic_series.iloc[1] == pytest.approx(-1.0)
    assert math.isnan(ic_series.iloc[2])  # 겹치는 종목이 2개뿐


# ── 스터디 13 골든 ─────────────────────────────────────────────────────────

def test_one_sample_t_reproduces_study_13_results():
    results = pd.read_csv(STUDY_13 / "results.tsv", sep="\t")
    monthly = pd.read_csv(STUDY_13 / "monthly_excess.csv")
    main_cells = results[(~results["tp_close"]) & (results["main"]) & (results["period"] != "all4")]
    assert len(main_cells) >= 3
    for _, row in main_cells.iterrows():
        series = monthly[(~monthly["tp_close"]) & (monthly["band"] == row["band"]) & (monthly["res"] == row["res"])
                         & (monthly["stop"] == row["stop"]) & (monthly["period"] == row["period"])]["excess_r"]
        result = stats.one_sample_t(series)
        assert result.sample_count == row["n_months"]
        # 월 시계열 CSV가 소수 5자리라 t·p는 4자리 근처에서 견준다
        assert abs(result.mean - row["excess_r"]) < 1e-4
        assert abs(result.t_statistic - row["t"]) < 2e-3
        assert abs(result.p_value - row["p"]) < 1e-3

    exploration = results[(~results["tp_close"]) & (results["period"] == "all4") & (~results["main"])]
    adjusted_p_values = stats.benjamini_hochberg_qvalues(exploration["p"].tolist())
    assert len(adjusted_p_values) == 11
    for computed, golden in zip(adjusted_p_values, exploration["q_bh"].tolist()):
        # results.tsv의 p가 소수 4자리라 q = p·11/순위는 최대 5.5e-4까지 벌어질 수 있다
        assert abs(computed - golden) < 6e-4


def test_study_13_shim_keeps_tuple_interface():
    sys.path.insert(0, str(STUDY_13))
    import stats_util  # noqa: E402

    mean, t_statistic, p_value, count = stats_util.one_sample_t([0.1, 0.3, -0.2, 0.4])
    reference = stats.one_sample_t([0.1, 0.3, -0.2, 0.4])
    assert (mean, t_statistic, p_value, count) == reference.as_tuple()
    assert stats_util.benjamini_hochberg_qvalues([0.01, 0.04, 0.5]) == stats.benjamini_hochberg_qvalues([0.01, 0.04, 0.5])


# ── 나머지 판정 도구 ───────────────────────────────────────────────────────

def test_deflated_sharpe_p_single_trial_equals_probabilistic_sharpe():
    # trials=1: p = 1 − Φ(SR·sqrt(T−1) / sqrt(1 + (γ4−1)/4·SR²)). SR=0.1, T=101, 정규(γ3=0, γ4=3) → z = 1/sqrt(1.005)
    p_value = stats.deflated_sharpe_p(0.1, trials=1, sample_count=101, skewness=0.0, kurtosis=3.0,
                                      sharpe_variance_across_trials=0.0)
    assert abs(p_value - (1.0 - stats.normal_cumulative_distribution(1.0 / math.sqrt(1.005)))) < 1e-12
    # 시도가 많으면 같은 샤프의 p가 커진다
    p_many = stats.deflated_sharpe_p(0.1, trials=50, sample_count=101, skewness=0.0, kurtosis=3.0,
                                     sharpe_variance_across_trials=0.01)
    assert p_many > p_value
    assert abs(stats.normal_ppf(0.975) - 1.959963984540054) < 1e-9
    assert abs(stats.normal_ppf(0.001) + 3.090232306167813) < 1e-9


def test_grid_robustness_neighbors_and_ratio():
    grid = {(10, 1.0): 0.5, (20, 1.0): 1.0, (30, 1.0): 0.8, (20, 0.5): -0.1, (20, 1.5): 0.3, (10, 0.5): 0.2}
    positive, total, ratio = stats.grid_robustness(grid, center=(20, 1.0))
    assert (positive, total) == (3, 4)  # 이웃 (10,1)(30,1)(20,0.5)(20,1.5) 중 (20,0.5)만 음수
    assert ratio == pytest.approx(1.0)
    positive, total, ratio = stats.grid_robustness(grid, center=(10, 1.0))
    assert (positive, total) == (2, 2)  # 이웃 (20,1.0)(10,0.5) — 왼쪽·아래 칸은 없다
    assert ratio == pytest.approx(0.5)
    assert stats.grid_robustness(grid, center=(99, 9.0)) == (0, 0, pytest.approx(float("nan"), nan_ok=True))


def test_required_sample_count():
    assert stats.required_sample_count(176.0, 567.0) == math.ceil((2.5 * 567.0 / 176.0) ** 2)  # ≈ 65거래일(quant-analyst §1-2)
    assert stats.required_sample_count(0.0, 1.0) == -1
    assert stats.required_sample_count(1.0, 1.0, target_t=2.0) == 4
