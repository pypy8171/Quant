"""스터디 20 · 코스피 매수 후 보유에 국면 배수를 곱한 오버레이 — walk-forward 27창·격자 27셀 → metrics.json.

실행(저장소 루트에서, build_axes.py 를 먼저 돌린다):
  py research/studies/20_macro_overlay/overlay_backtest.py --cell market_only
  py research/studies/20_macro_overlay/overlay_backtest.py --cell four_axis
  py research/studies/20_macro_overlay/overlay_backtest.py --cell four_axis --skip-grid   # 격자 없이 중심 셀만(빠른 확인)

체결·비용(PREREG §2): e_D = macro_scale(D−1 08:50), D 시가 체결, 수익 = e_D × (open_{D+1}/open_D − 1),
노출 변경분 |e_D − e_{D−1}| 에 0.23%. 매수 후 보유는 비용 0.
판정(PREREG §3) ①~⑦ 전부 통과해야 macro_apply=true. 난수 없음 — 같은 입력이면 같은 metrics.json 바이트.

교체 예정: cagr·mdd·Newey-West t·Deflated Sharpe 는 PYQuant/backtest/stats.py 가 생기면 그 함수로 바꾼다(지금은 이 파일 안).
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))

from PYQuant.features import regime_axes  # noqa: E402

STUDY_DIRECTORY = Path(__file__).resolve().parent   # --study-dir 로 바꾼다(스터디 21 은 이 러너를 그대로 쓴다)
OUTPUT_DIRECTORY = STUDY_DIRECTORY / "out"
METRICS_PATH = STUDY_DIRECTORY / "metrics.json"
SCALE_UPDATE = "daily"                             # --scale-update. 격자·축소판의 Parameters 에도 같이 들어간다
KOSPI_PATH = regime_axes.INDEX_CACHE_DIRECTORY / "idx__KS11_1996-01-01_2026-08-15_adj.parquet"

PERIOD_START = pd.Timestamp("1999-01-01")
PERIOD_END = pd.Timestamp("2025-12-31")
WINDOW_YEARS = list(range(1999, 2026))
COST_RATE = 0.0023
TRIALS_PRIOR = 27
GRID_DEADBAND = (0.15, 0.25, 0.35)
GRID_WINDOW = (96, 120, 144)
GRID_CONTRACTION = (0.4, 0.5, 0.6)
CENTER = (0.25, 120, 0.5)
NEWEY_WEST_LAGS = 3
MINIMUM_REGIME_MONTHS = 6

PASS_RULES = {
    "exp_minus_con_t": 2.0,          # ①
    "windows_pos_of_27": 16,         # ②
    "mdd_rel_reduction": 0.20,       # ④
    "cagr_giveback_pp": 1.0,         # ⑤ 이하
    "transitions_per_year": 6.0,     # ⑥ 이하
    "excess_month_t": 1.5,           # ⑦
    "dsr_p": 0.05,                   # ⑦ 미만
}


# ----------------------------------------------------------------------------- 통계(교체 예정: PYQuant/backtest/stats.py)

def cagr_of(daily_returns: pd.Series) -> float:
    if daily_returns.empty:
        return float("nan")

    growth = float(np.prod(1.0 + daily_returns.values))
    years = (daily_returns.index[-1] - daily_returns.index[0]).days / 365.25

    if years <= 0 or growth <= 0:
        return float("nan")

    return growth ** (1.0 / years) - 1.0


def max_drawdown_of(daily_returns: pd.Series) -> float:
    equity = np.cumprod(1.0 + daily_returns.values)
    peak = np.maximum.accumulate(equity)
    return float(np.min(equity / peak - 1.0)) if len(equity) else float("nan")


def sharpe_of(daily_returns: pd.Series) -> float:
    deviation = float(daily_returns.std(ddof=1))
    return float(daily_returns.mean() / deviation * math.sqrt(252.0)) if deviation > 0 else float("nan")


def calmar_of(daily_returns: pd.Series) -> float:
    drawdown = max_drawdown_of(daily_returns)
    growth = cagr_of(daily_returns)
    return growth / abs(drawdown) if drawdown < 0 and not math.isnan(growth) else float("nan")


def newey_west_t(target: np.ndarray, regressor: np.ndarray | None, lags: int = NEWEY_WEST_LAGS) -> float:
    """OLS 계수의 Newey-West t. regressor 가 None 이면 평균의 t, 아니면 더미 계수의 t."""
    target = np.asarray(target, dtype=float)
    count = len(target)

    if count < 8:
        return float("nan")

    design = np.ones((count, 1)) if regressor is None else np.column_stack([np.ones(count), regressor])
    gram_inverse = np.linalg.pinv(design.T @ design)
    beta = gram_inverse @ design.T @ target
    residual = target - design @ beta
    scores = design * residual[:, None]
    meat = scores.T @ scores

    for lag in range(1, lags + 1):
        weight = 1.0 - lag / (lags + 1.0)
        cross = scores[lag:].T @ scores[:-lag]
        meat += weight * (cross + cross.T)

    covariance = gram_inverse @ meat @ gram_inverse
    standard_error = math.sqrt(max(covariance[-1, -1], 0.0))
    return float(beta[-1] / standard_error) if standard_error > 0 else float("nan")


def normal_cumulative(value: float) -> float:
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))


def normal_quantile(probability: float) -> float:
    """역정규분포(Acklam 근사, 오차 1e-9). scipy 가 없어 직접 둔다."""
    low = 0.02425
    high = 1.0 - low
    coefficients_a = (-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                      1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00)
    coefficients_b = (-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                      6.680131188771972e+01, -1.328068155288572e+01)
    coefficients_c = (-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                      -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00)
    coefficients_d = (7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00)

    if probability < low:
        root = math.sqrt(-2.0 * math.log(probability))
        return (((((coefficients_c[0] * root + coefficients_c[1]) * root + coefficients_c[2]) * root
                  + coefficients_c[3]) * root + coefficients_c[4]) * root + coefficients_c[5]) / \
               ((((coefficients_d[0] * root + coefficients_d[1]) * root + coefficients_d[2]) * root
                 + coefficients_d[3]) * root + 1.0)

    if probability > high:
        return -normal_quantile(1.0 - probability)

    centered = probability - 0.5
    square = centered * centered
    return (((((coefficients_a[0] * square + coefficients_a[1]) * square + coefficients_a[2]) * square
              + coefficients_a[3]) * square + coefficients_a[4]) * square + coefficients_a[5]) * centered / \
           (((((coefficients_b[0] * square + coefficients_b[1]) * square + coefficients_b[2]) * square
              + coefficients_b[3]) * square + coefficients_b[4]) * square + 1.0)


def deflated_sharpe_p(monthly_excess: np.ndarray, trial_sharpes: list[float], trials: int) -> float:
    """Bailey·López de Prado Deflated Sharpe. 돌려주는 값은 1 − PSR(SR*), 작을수록 좋다."""
    values = np.asarray(monthly_excess, dtype=float)
    count = len(values)
    deviation = values.std(ddof=1)

    if count < 12 or deviation <= 0:
        return float("nan")

    sharpe = values.mean() / deviation
    skewness = float(np.mean(((values - values.mean()) / deviation) ** 3))
    kurtosis = float(np.mean(((values - values.mean()) / deviation) ** 4))
    trial_values = np.asarray([one for one in trial_sharpes if not math.isnan(one)], dtype=float)
    trial_variance = float(trial_values.var(ddof=1)) if len(trial_values) > 1 else 0.0
    euler = 0.5772156649
    expected_max = math.sqrt(trial_variance) * ((1.0 - euler) * normal_quantile(1.0 - 1.0 / trials)
                                                + euler * normal_quantile(1.0 - 1.0 / (trials * math.e)))
    denominator = math.sqrt(max(1.0 - skewness * sharpe + (kurtosis - 1.0) / 4.0 * sharpe ** 2, 1e-12))
    statistic = (sharpe - expected_max) * math.sqrt(count - 1.0) / denominator
    return float(1.0 - normal_cumulative(statistic))


# ----------------------------------------------------------------------------- 오버레이 시뮬레이션

def load_kospi() -> pd.DataFrame:
    bars = pd.read_parquet(KOSPI_PATH, columns=["date", "open", "close"])
    bars["date"] = pd.to_datetime(bars["date"])
    return bars.sort_values("date").reset_index(drop=True)


def simulate(axes: pd.DataFrame, bars: pd.DataFrame, cost_rate: float = COST_RATE) -> pd.DataFrame:
    """일별 표: date, exposure, regime, buy_hold_return, overlay_return. 수익은 D 시가 → D+1 시가."""
    decided = axes.set_index("decision_date")[["macro_scale", "regime"]].sort_index()
    # e_D 는 D−1 결정(전 평일 08:50)의 배수. 라벨도 같은 시점의 것으로 붙인다.
    lagged = decided.shift(1)
    frame = bars.copy()
    frame["next_open"] = frame["open"].shift(-1)
    frame = frame.dropna(subset=["next_open"])
    frame["buy_hold_return"] = frame["next_open"] / frame["open"] - 1.0
    aligned = lagged.reindex(frame["date"].values, method="ffill")
    frame["exposure"] = aligned["macro_scale"].fillna(1.0).values
    frame["regime"] = aligned["regime"].fillna("expansion").values
    frame = frame[(frame["date"] >= PERIOD_START) & (frame["date"] <= PERIOD_END)].reset_index(drop=True)
    previous_exposure = frame["exposure"].shift(1).fillna(1.0)
    frame["cost"] = cost_rate * (frame["exposure"] - previous_exposure).abs()
    frame["overlay_return"] = frame["exposure"] * frame["buy_hold_return"] - frame["cost"]
    return frame[["date", "exposure", "regime", "buy_hold_return", "overlay_return", "cost"]]


def monthly_table(curve: pd.DataFrame, axes: pd.DataFrame) -> pd.DataFrame:
    indexed = curve.set_index("date")
    monthly_buy_hold = (1.0 + indexed["buy_hold_return"]).groupby(indexed.index.to_period("M")).prod() - 1.0
    monthly_overlay = (1.0 + indexed["overlay_return"]).groupby(indexed.index.to_period("M")).prod() - 1.0
    # 월초 국면 = 그 달 첫 거래일 전 마지막 결정일의 라벨(월이 시작되기 전에 알던 값)
    decided = axes.set_index("decision_date")["regime"].sort_index()
    month_starts = monthly_buy_hold.index.to_timestamp()
    labels = decided.reindex(month_starts - pd.Timedelta(days=1), method="ffill").fillna("expansion").values
    return pd.DataFrame({"buy_hold": monthly_buy_hold.values, "overlay": monthly_overlay.values, "regime": labels},
                        index=monthly_buy_hold.index)


def window_table(curve: pd.DataFrame) -> pd.DataFrame:
    rows = []
    indexed = curve.set_index("date")

    for year in WINDOW_YEARS:
        part = indexed[indexed.index.year == year]

        if part.empty:
            continue

        calmar_buy_hold = calmar_of(part["buy_hold_return"])
        calmar_overlay = calmar_of(part["overlay_return"])
        rows.append({"year": year, "cagr_buy_hold": cagr_of(part["buy_hold_return"]),
                     "cagr_overlay": cagr_of(part["overlay_return"]),
                     "mdd_buy_hold": max_drawdown_of(part["buy_hold_return"]),
                     "mdd_overlay": max_drawdown_of(part["overlay_return"]),
                     "calmar_buy_hold": calmar_buy_hold, "calmar_overlay": calmar_overlay,
                     "overlay_better": bool(calmar_overlay > calmar_buy_hold) if not (
                         math.isnan(calmar_overlay) or math.isnan(calmar_buy_hold)) else False,
                     "exposure_mean": float(part["exposure"].mean())})

    return pd.DataFrame(rows)


def transitions_per_year(axes: pd.DataFrame) -> float:
    inside = axes[(axes["decision_date"] >= PERIOD_START) & (axes["decision_date"] <= PERIOD_END)]
    active = inside[inside["growth"].notna() | inside["inflation"].notna()]

    if active.empty:
        return 0.0

    changed = active["regime"] != active["regime"].shift(1)
    changed.iloc[0] = False
    per_year = changed.groupby(active["decision_date"].dt.year).sum()
    return float(per_year.mean())


def since_axes_valid(axes: pd.DataFrame, curve: pd.DataFrame) -> dict | None:
    """진단용(판정 아님): 성장·물가 축이 둘 다 유효해진 첫 결정일부터의 매수 후 보유 대비 수치."""
    valid = axes[axes["growth"].notna() & axes["inflation"].notna()]

    if valid.empty:
        return None

    start = valid["decision_date"].iloc[0]
    part = curve[curve["date"] >= start].set_index("date")

    if len(part) < 250:
        return None

    return {"from": start.strftime("%Y-%m-%d"), "cagr_bh": cagr_of(part["buy_hold_return"]),
            "cagr_ov": cagr_of(part["overlay_return"]), "mdd_bh": max_drawdown_of(part["buy_hold_return"]),
            "mdd_ov": max_drawdown_of(part["overlay_return"]), "calmar_bh": calmar_of(part["buy_hold_return"]),
            "calmar_ov": calmar_of(part["overlay_return"])}


def evaluate(axes: pd.DataFrame, bars: pd.DataFrame) -> dict:
    curve = simulate(axes, bars)
    months = monthly_table(curve, axes)
    windows = window_table(curve)
    buy_hold = curve.set_index("date")["buy_hold_return"]
    overlay = curve.set_index("date")["overlay_return"]
    cagr_buy_hold = cagr_of(buy_hold)
    cagr_overlay = cagr_of(overlay)
    mdd_buy_hold = max_drawdown_of(buy_hold)
    mdd_overlay = max_drawdown_of(overlay)
    two_regimes = months[months["regime"].isin(["expansion", "contraction"])]
    expansion_months = int((two_regimes["regime"] == "expansion").sum())
    contraction_months = int((two_regimes["regime"] == "contraction").sum())

    if expansion_months >= MINIMUM_REGIME_MONTHS and contraction_months >= MINIMUM_REGIME_MONTHS:
        exp_minus_con_t = newey_west_t(two_regimes["buy_hold"].values,
                                       (two_regimes["regime"] == "expansion").astype(float).values)
    else:
        exp_minus_con_t = float("nan")

    excess = (months["overlay"] - months["buy_hold"]).values
    excess_deviation = float(np.std(excess, ddof=1))
    return {
        "diagnostic_since_axes_valid": since_axes_valid(axes, curve),
        "curve": curve, "months": months, "windows": windows,
        "cagr_bh": cagr_buy_hold, "cagr_ov": cagr_overlay, "mdd_bh": mdd_buy_hold, "mdd_ov": mdd_overlay,
        "mdd_rel_reduction": (1.0 - mdd_overlay / mdd_buy_hold) if mdd_buy_hold < 0 else float("nan"),
        "sharpe_bh": sharpe_of(buy_hold), "sharpe_ov": sharpe_of(overlay),
        "calmar_bh": calmar_of(buy_hold), "calmar_ov": calmar_of(overlay),
        "exp_minus_con_t": exp_minus_con_t,
        "excess_month_t": newey_west_t(excess, None) if excess_deviation > 0 else float("nan"),
        "excess_month_sharpe": float(excess.mean() / excess_deviation) if excess_deviation > 0 else float("nan"),
        "windows_pos_of_27": int(windows["overlay_better"].sum()),
        "windows_total": int(len(windows)),
        "transitions_per_year": transitions_per_year(axes),
        "cagr_giveback_pp": (cagr_buy_hold - cagr_overlay) * 100.0,
        "regime_months": months["regime"].value_counts().to_dict(),
        "regime_month_mean_pct": {name: round(float(value) * 100.0, 3)
                                  for name, value in months.groupby("regime")["buy_hold"].mean().items()},
        "exposure_mean": float(curve["exposure"].mean()),
        "days_exposure_below_1": int((curve["exposure"] < 1.0).sum()),
        "total_cost_pct": float(curve["cost"].sum() * 100.0),
        "sample_n": int(len(curve)),
        "period": f"{curve['date'].iloc[0].strftime('%Y-%m-%d')}~{curve['date'].iloc[-1].strftime('%Y-%m-%d')}",
    }


# ----------------------------------------------------------------------------- 판정·격자

def judge(result: dict, neighbors_same_sign: bool | None, dsr_p: float) -> dict:
    checks = {
        "1_exp_minus_con_t": _at_least(result["exp_minus_con_t"], PASS_RULES["exp_minus_con_t"]),
        "2_windows_pos_of_27": result["windows_pos_of_27"] >= PASS_RULES["windows_pos_of_27"],
        "3_neighbors_same_sign": bool(neighbors_same_sign) if neighbors_same_sign is not None else False,
        "4_mdd_rel_reduction": _at_least(result["mdd_rel_reduction"], PASS_RULES["mdd_rel_reduction"]),
        "5_cagr_giveback_pp": _at_most(result["cagr_giveback_pp"], PASS_RULES["cagr_giveback_pp"]),
        "6_transitions_per_year": _at_most(result["transitions_per_year"], PASS_RULES["transitions_per_year"]),
        "7_excess_month_t": _at_least(result["excess_month_t"], PASS_RULES["excess_month_t"])
                            and _at_most(dsr_p, PASS_RULES["dsr_p"], strict=True),
    }
    checks["all_pass"] = all(checks.values())
    checks["only_5_fails"] = (not checks["5_cagr_giveback_pp"]) and all(
        value for key, value in checks.items() if key not in ("5_cagr_giveback_pp", "all_pass"))
    return checks


def _at_least(value: float, threshold: float) -> bool:
    return (not math.isnan(value)) and value >= threshold


def _at_most(value: float, threshold: float, strict: bool = False) -> bool:
    return (not math.isnan(value)) and ((value < threshold) if strict else (value <= threshold))


def grid_cells() -> list[tuple[float, int, float]]:
    return list(itertools.product(GRID_DEADBAND, GRID_WINDOW, GRID_CONTRACTION))


def is_neighbor(cell: tuple[float, int, float]) -> bool:
    differences = sum(1 for index in range(3) if cell[index] != CENTER[index])
    return differences == 1


def run_grid(cell_name: str, bars: pd.DataFrame, as_of: str, raw_by_rule: dict) -> tuple[list[dict], bool | None]:
    calendar = regime_axes.decision_calendar(as_of)
    rows = []
    center_sign = None
    neighbor_signs = []

    for deadband, window_months, base_contraction in grid_cells():
        parameters = regime_axes.Parameters(deadband=deadband, window_months=window_months,
                                            base_contraction=base_contraction, scale_update=SCALE_UPDATE)
        prepared_list = [regime_axes.prepare_series(rule, parameters, raw_by_rule[rule.name])
                         for rule in regime_axes.rules_for_cell(cell_name)]
        axes = regime_axes.build_axes_table(calendar, cell_name, parameters, prepared_list)
        result = evaluate(axes, bars)
        improvement = result["calmar_ov"] - result["calmar_bh"]
        sign = 0 if abs(improvement) < 1e-12 else (1 if improvement > 0 else -1)
        row = {"delta": deadband, "W": window_months, "base_con": base_contraction,
               "calmar_ov": result["calmar_ov"], "calmar_bh": result["calmar_bh"], "calmar_improvement": improvement,
               "cagr_ov": result["cagr_ov"], "mdd_ov": result["mdd_ov"], "windows_pos_of_27": result["windows_pos_of_27"],
               "transitions_per_year": result["transitions_per_year"],
               "excess_month_sharpe": result["excess_month_sharpe"], "sign": sign}
        rows.append(row)
        this_cell = (deadband, window_months, base_contraction)

        if this_cell == CENTER:
            center_sign = sign
        elif is_neighbor(this_cell):
            neighbor_signs.append(sign)

        print(f"  격자 δ={deadband} W={window_months} 수축={base_contraction}: Calmar {result['calmar_ov']:.3f} "
              f"(매수 후 보유 {result['calmar_bh']:.3f}) 창 {result['windows_pos_of_27']}/27", flush=True)

    neighbors_same_sign = None if center_sign is None else all(sign == center_sign for sign in neighbor_signs)
    return rows, neighbors_same_sign


# ----------------------------------------------------------------------------- 입력 해시·저장

def input_sha256(cell_name: str) -> str:
    digest = hashlib.sha256()
    paths = [KOSPI_PATH]

    for rule in regime_axes.rules_for_cell(cell_name):
        for piece in rule.loader.split("|"):
            kind, _, target = piece.partition(":")

            if kind == "macro":
                paths.append(regime_axes.MACRO_DIRECTORY / f"{target}.parquet")
            elif kind == "index":
                paths.append(regime_axes.INDEX_CACHE_DIRECTORY / f"{target}.parquet")
            elif kind == "derived":
                paths.append(regime_axes.DERIVED_FLOW_PATH)

    for path in sorted(set(paths)):
        if path.exists():
            digest.update(path.name.encode("utf-8"))
            digest.update(path.read_bytes())

    return digest.hexdigest()


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT, capture_output=True,
                              text=True, check=False).stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def clean(value):
    if isinstance(value, float):
        return None if math.isnan(value) else round(value, 6)

    if isinstance(value, dict):
        return {key: clean(inner) for key, inner in value.items()}

    if isinstance(value, list):
        return [clean(inner) for inner in value]

    if isinstance(value, (np.integer,)):
        return int(value)

    if isinstance(value, (np.floating,)):
        return clean(float(value))

    if isinstance(value, (np.bool_,)):
        return bool(value)

    return value


def save_outputs(cell_name: str, result: dict, suffix: str = "") -> None:
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
    curve = result["curve"].copy()
    curve["equity_buy_hold"] = np.cumprod(1.0 + curve["buy_hold_return"].values)
    curve["equity_overlay"] = np.cumprod(1.0 + curve["overlay_return"].values)
    curve.to_csv(OUTPUT_DIRECTORY / f"curve_{cell_name}{suffix}.csv", index=False, float_format="%.8f")
    result["windows"].to_csv(OUTPUT_DIRECTORY / f"windows_{cell_name}{suffix}.csv", index=False, float_format="%.6f")
    months = result["months"].copy()
    months.index = months.index.astype(str)
    months.to_csv(OUTPUT_DIRECTORY / f"months_{cell_name}{suffix}.csv", index_label="month", float_format="%.6f")


def cell_metrics(cell_name: str, result: dict, checks: dict, grid_rows: list[dict], neighbors_same_sign,
                 dsr_p: float, data_grades: dict, reduced: dict | None) -> dict:
    keys = ["cagr_bh", "cagr_ov", "mdd_bh", "mdd_ov", "mdd_rel_reduction", "sharpe_bh", "sharpe_ov", "calmar_bh",
            "calmar_ov", "excess_month_t", "excess_month_sharpe", "exp_minus_con_t", "windows_pos_of_27",
            "windows_total", "transitions_per_year", "cagr_giveback_pp", "regime_months", "regime_month_mean_pct",
            "exposure_mean", "days_exposure_below_1", "total_cost_pct", "sample_n", "period",
            "diagnostic_since_axes_valid"]
    metrics = {"basket": "kospi", "cell": cell_name}
    metrics.update({key: result[key] for key in keys})
    metrics["grid"] = grid_rows
    metrics["windows"] = result["windows"].to_dict("records")   # out/ 은 gitignore 라 창 표는 여기에도 남긴다
    metrics["neighbors_same_sign"] = neighbors_same_sign
    metrics["dsr_p"] = dsr_p
    metrics["cost_bp"] = COST_RATE * 10000.0
    metrics["scale_update"] = SCALE_UPDATE
    metrics["trials_prior"] = TRIALS_PRIOR
    metrics["checks"] = checks
    metrics["macro_apply"] = bool(checks["all_pass"]) or bool(reduced and reduced["checks"]["all_pass"])
    metrics["reduced_variant"] = reduced
    metrics["data_grades"] = data_grades
    metrics["missing_series"] = [name for name, _, _ in regime_axes.MISSING_SERIES]
    metrics["input_sha256"] = input_sha256(cell_name)
    metrics["lag_peak_days_median"] = None    # 다음 실행(스터디 07 이벤트 표와 조인)
    metrics["lag_trough_days_median"] = None
    return clean(metrics)


def write_metrics(cell_name: str, metrics: dict) -> dict:
    existing = json.loads(METRICS_PATH.read_text(encoding="utf-8")) if METRICS_PATH.exists() else {}
    cells = existing.get("cells", {})
    cells[cell_name] = metrics
    primary = "four_axis" if "four_axis" in cells else cell_name
    mirror = cells[primary]
    document = {
        "schema": "quant.metrics/macro_overlay.v1",
        "study_id": "BT-" + STUDY_DIRECTORY.name.split("_")[0],
        "primary_cell": primary,
        "sample_n": mirror["sample_n"],
        "period": mirror["period"],
        "cells": cells,
        "mdd_base": mirror["mdd_bh"],
        "mdd_overlay": mirror["mdd_ov"],
        "cagr_base": mirror["cagr_bh"],
        "cagr_overlay": mirror["cagr_ov"],
        "t_expansion_minus_contraction": mirror["exp_minus_con_t"],
        "walk_forward_windows_positive": mirror["windows_pos_of_27"],
        "switches_per_year": mirror["transitions_per_year"],
        "trials_prior": TRIALS_PRIOR,
        "macro_apply": mirror["macro_apply"],
        "git_commit": git_commit(),
        "prereg": (STUDY_DIRECTORY / "PREREG.md").relative_to(REPO_ROOT).as_posix(),
    }
    METRICS_PATH.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return document


# ----------------------------------------------------------------------------- 진입점

def main() -> int:
    global STUDY_DIRECTORY, OUTPUT_DIRECTORY, METRICS_PATH, SCALE_UPDATE
    parser = argparse.ArgumentParser(description="거시 국면 오버레이 백테스트")
    parser.add_argument("--cell", choices=["market_only", "four_axis"], default="four_axis")
    parser.add_argument("--as-of", default="2026-08-14")
    parser.add_argument("--skip-grid", action="store_true", help="격자 27셀을 건너뛴다(③·⑦ dsr 은 미판정)")
    parser.add_argument("--scale-update", choices=["daily", "on_state_change"], default="daily",
                        help="배수 갱신 규칙. build_axes.py 에 준 값과 같아야 한다(axes 표는 그 값으로 만들어졌다)")
    parser.add_argument("--study-dir", default=str(STUDY_DIRECTORY), help="axes 표를 읽고 산출물을 남길 스터디 폴더")
    arguments = parser.parse_args()
    STUDY_DIRECTORY = Path(arguments.study_dir).resolve()
    OUTPUT_DIRECTORY = STUDY_DIRECTORY / "out"
    METRICS_PATH = STUDY_DIRECTORY / "metrics.json"
    SCALE_UPDATE = arguments.scale_update
    cell_name = arguments.cell
    axes_path = OUTPUT_DIRECTORY / f"axes_{cell_name}.parquet"

    if not axes_path.exists():
        print(f"먼저 build_axes.py --cell {cell_name} 을 돌린다: {axes_path} 없음")
        return 1

    np.random.seed(0)   # 난수는 쓰지 않지만 재현 정보로 고정해 둔다
    bars = load_kospi()
    axes = pd.read_parquet(axes_path)
    axes["decision_date"] = pd.to_datetime(axes["decision_date"])
    print(f"[{cell_name}] 중심 셀 평가", flush=True)
    result = evaluate(axes, bars)
    save_outputs(cell_name, result)

    rules = regime_axes.rules_for_cell(cell_name)
    raw_by_rule = {rule.name: regime_axes.load_series(rule) for rule in rules}
    data_grades = {name: ("missing" if raw.empty else ("B" if (raw["grade"] == "B").any() else "A"))
                   for name, raw in raw_by_rule.items()}

    if arguments.skip_grid:
        grid_rows, neighbors_same_sign, dsr_p = [], None, float("nan")
    else:
        print(f"[{cell_name}] 격자 27셀", flush=True)
        grid_rows, neighbors_same_sign = run_grid(cell_name, bars, arguments.as_of, raw_by_rule)
        pd.DataFrame(grid_rows).to_csv(OUTPUT_DIRECTORY / f"grid_{cell_name}.csv", index=False, float_format="%.6f")
        excess = (result["months"]["overlay"] - result["months"]["buy_hold"]).values
        dsr_p = deflated_sharpe_p(excess, [row["excess_month_sharpe"] for row in grid_rows], TRIALS_PRIOR)

    checks = judge(result, neighbors_same_sign, dsr_p)
    reduced = None

    if checks["only_5_fails"]:
        print(f"[{cell_name}] ⑤만 미달 → 축소판(수축 배수만) 한 번 더", flush=True)
        parameters = regime_axes.Parameters(risk_only_base=True, scale_update=SCALE_UPDATE)
        prepared_list = [regime_axes.prepare_series(rule, parameters, raw_by_rule[rule.name]) for rule in rules]
        reduced_axes = regime_axes.build_axes_table(regime_axes.decision_calendar(arguments.as_of), cell_name,
                                                    parameters, prepared_list)
        reduced_result = evaluate(reduced_axes, bars)
        save_outputs(cell_name, reduced_result, "_reduced")
        reduced_excess = (reduced_result["months"]["overlay"] - reduced_result["months"]["buy_hold"]).values
        reduced_dsr = deflated_sharpe_p(reduced_excess, [row["excess_month_sharpe"] for row in grid_rows],
                                        TRIALS_PRIOR) if grid_rows else float("nan")
        reduced_checks = judge(reduced_result, neighbors_same_sign, reduced_dsr)
        reduced = clean({key: reduced_result[key] for key in
                         ["cagr_bh", "cagr_ov", "mdd_bh", "mdd_ov", "mdd_rel_reduction", "calmar_bh", "calmar_ov",
                          "excess_month_t", "exp_minus_con_t", "windows_pos_of_27", "transitions_per_year",
                          "cagr_giveback_pp"]} | {"dsr_p": reduced_dsr, "checks": reduced_checks})

    metrics = cell_metrics(cell_name, result, checks, grid_rows, neighbors_same_sign, dsr_p, data_grades, reduced)
    document = write_metrics(cell_name, metrics)
    summary = {key: metrics[key] for key in ["period", "sample_n", "cagr_bh", "cagr_ov", "mdd_bh", "mdd_ov",
                                             "mdd_rel_reduction", "exp_minus_con_t", "excess_month_t",
                                             "windows_pos_of_27", "transitions_per_year", "cagr_giveback_pp",
                                             "neighbors_same_sign", "dsr_p", "macro_apply"]}
    summary["checks"] = metrics["checks"]
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"저장: {METRICS_PATH.relative_to(REPO_ROOT)} (primary_cell={document['primary_cell']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
