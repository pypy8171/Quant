"""스터디 22 · 저PBR × 고ROE — 가치 비중 0.7·분기 리밸 중심 셀, 시총가중·크기 분해·용량·상폐 보유 기록.

사전등록 `research/studies/22_pbr_roe_value_tilt/PREREG.md`를 그대로 옮긴 하네스다. 달력·신호·수익·통계는 스터디 19의
`research/studies/19_fundamental_factors/run_pbr_roe.py`를 그대로 가져다 쓰고(신호는 `PYQuant/features/fundamental.py`, 비용은
`PYQuant/backtest/costs.py`, 통계는 `PYQuant/backtest/stats.py`), 여기서 바꾸는 것은 중심 셀·격자 27칸·walk-forward 후보(리밸 축 포함)와
추가 표 네 가지(시총가중·크기 3분위·용량·상폐 보유 기록)뿐이다.

산출물(같은 폴더): metrics.json, curves.csv, walkforward.csv, robustness.csv, quintiles.csv, holdings.csv,
                 size_buckets.csv(분위별 월 곡선), capacity.csv(리밸 달별 용량), delisted_holdings.csv, README.md.

재현: py research/studies/22_pbr_roe_value_tilt/run_value_tilt.py --cost mid   (seed 20260920, 난수는 쓰지 않는다)
"""
from __future__ import annotations

import argparse
import datetime
import importlib.util
import json
import math
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "PYQuant"))

from PYQuant.features import fundamental  # noqa: E402
from backtest import stats  # noqa: E402
from backtest.costs import RESEARCH_BY_LEVEL, CostSpec, side_cost_percent_at_price  # noqa: E402

STUDY_DIR = Path(__file__).resolve().parent
BASE_PATH = REPO_ROOT / "research" / "studies" / "19_fundamental_factors" / "run_pbr_roe.py"
MEMBERSHIP_PATH = REPO_ROOT / "PYQuant" / "data" / "universe" / "membership.parquet"
SEED = 20260920
TOP_N_GRID = (20, 30, 40)
VALUE_WEIGHT_GRID = (0.5, 0.7, 0.9)
REBALANCE_GRID = (1, 3, 6)
CENTER = (30, 0.7, 3)
BUCKET_TOP_N = 10
BUCKET_LABELS = ("소형", "중형", "대형")
CAPACITY_LIMITS = (0.10, 0.05)
BILLION_KRW = 100_000_000


def load_base():
    """스터디 19 하네스를 모듈로 읽어 달력·신호·통계 함수를 빌려 쓴다. 중심 셀만 이 스터디 것으로 바꾼다(factor_diagnostics가 CENTER[1]을 본다)."""
    specification = importlib.util.spec_from_file_location("study19", BASE_PATH)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    module.CENTER = CENTER
    return module


base = load_base()
JUDGMENT_START, JUDGMENT_END = base.JUDGMENT_START, base.JUDGMENT_END


# ── 포트폴리오 ─────────────────────────────────────────────────────────────────

def simulate(signals: dict, all_returns: dict, months: list, top_n: int, value_weight: float, rebalance_every: int,
             cost: CostSpec, weighting: str = "equal", bucket: str | None = None, capacity_rows: list | None = None,
             dropped_rows: list | None = None) -> pd.DataFrame:
    """스터디 19 simulate에 세 가지를 더한 것 — weighting("equal"|"cap": 상위 N 안에서 기준일 시총 비례), bucket(유니버스 시총 3분위 안에서만,
    벤치마크도 그 분위 동일가중), capacity_rows(리밸 달마다 종목별 주문 금액이 20일 평균 거래대금의 한도 비율을 넘지 않는 최대 자본),
    dropped_rows(다음 달 체결일 봉이 없어 마지막 종가로 청산된 보유 종목 기록)."""
    holdings: dict = {}
    records = []

    for position, month in enumerate(months):
        frame = signals[month]
        tradable = all_returns[month]

        if bucket is not None:
            tercile = pd.qcut(frame["market_cap"].rank(method="first"), 3, labels=BUCKET_LABELS)
            frame = frame[tercile == bucket]

        score = value_weight * frame["z_value"] + (1.0 - value_weight) * frame["z_quality"]
        ranked = score.sort_values(ascending=False)
        rank_of = pd.Series(np.arange(1, len(ranked) + 1), index=ranked.index)
        benchmark = float(frame["forward_return"].mean())

        if dropped_rows is not None:
            for ticker, weight in holdings.items():
                if ticker not in tradable.index:
                    dropped_rows.append({"month": month, "ticker": ticker, "weight_before": weight})

        holdings = {ticker: weight for ticker, weight in holdings.items() if ticker in tradable.index}

        if position % rebalance_every == 0 or not holdings:
            kept = [ticker for ticker in holdings if rank_of.get(ticker, np.inf) <= base.BUFFER_MULTIPLE * top_n]
            fill = [ticker for ticker in ranked.index if ticker not in kept][: max(top_n - len(kept), 0)]
            members = kept + fill

            if weighting == "cap":
                caps = frame["market_cap"].reindex(members).fillna(0.0)
                target = {ticker: float(caps[ticker] / caps.sum()) for ticker in members} if caps.sum() > 0 else {}
            else:
                target = {ticker: 1.0 / len(members) for ticker in members}
        else:
            target = dict(holdings)

        buy_cost = 0.0
        sell_cost = 0.0
        turnover_oneway = 0.0
        capacity = {limit: np.inf for limit in CAPACITY_LIMITS}

        for ticker in set(holdings) | set(target):
            change = target.get(ticker, 0.0) - holdings.get(ticker, 0.0)
            price = float(tradable.loc[ticker, "entry_close"])

            if change > 0:
                buy_cost += change * side_cost_percent_at_price(cost, "BUY", price) / 100.0
                turnover_oneway += change
            elif change < 0:
                sell_cost += -change * side_cost_percent_at_price(cost, "SELL", price) / 100.0

            if change != 0.0 and ticker in frame.index:
                turnover_value = float(frame.loc[ticker, "turnover_20d"])

                for limit in CAPACITY_LIMITS:
                    capacity[limit] = min(capacity[limit], limit * turnover_value / abs(change))

        if capacity_rows is not None and turnover_oneway > 0:
            capacity_rows.append({"month": month, "holdings": len(target), "turnover_oneway": turnover_oneway,
                                  **{f"capacity_{int(limit * 100)}pct_krw": capacity[limit] for limit in CAPACITY_LIMITS}})

        gross = sum(weight * float(tradable.loc[ticker, "forward_return"]) for ticker, weight in target.items())
        cost_total = buy_cost + sell_cost
        net = gross - cost_total

        grown = {ticker: weight * (1.0 + float(tradable.loc[ticker, "forward_return"])) for ticker, weight in target.items()}
        total = sum(grown.values())
        holdings = {ticker: value / total for ticker, value in grown.items()} if total > 0 else {}

        records.append({"month": month, "gross": gross, "cost": cost_total, "net": net, "benchmark": benchmark,
                        "excess": net - benchmark, "turnover_oneway": turnover_oneway, "holdings": len(target),
                        "universe": len(frame), "members": ";".join(f"{ticker}:{weight:.4f}" for ticker, weight in sorted(target.items()))})

    return pd.DataFrame(records)


# ── 판정 층 ───────────────────────────────────────────────────────────────────

def grid_cells(signals: dict, all_returns: dict, months: list, cost: CostSpec) -> dict:
    return {(top_n, weight, rebalance_every): simulate(signals, all_returns, months, top_n, weight, rebalance_every, cost)
            for top_n in TOP_N_GRID for weight in VALUE_WEIGHT_GRID for rebalance_every in REBALANCE_GRID}


def walk_forward(cells: dict) -> list:
    """검증 연도마다 직전 3년 학습창에서 27칸(리밸 축 포함) 중 순초과수익 샤프 최대 칸을 골라 검증 연도에 적용."""
    rows = []

    for year in base.VALIDATION_YEARS:
        train_start, train_end = pd.Timestamp(year=year - base.TRAIN_YEARS, month=1, day=1), pd.Timestamp(year=year - 1, month=12, day=31)
        test_start, test_end = pd.Timestamp(year=year, month=1, day=1), pd.Timestamp(year=year, month=12, day=31)
        best_key, best_sharpe = None, -np.inf

        for key, curve in cells.items():
            train = base.month_in(curve, train_start, train_end)
            train_sharpe = stats.performance_summary(train["excess"], periods_per_year=12)["sharpe"] if len(train) >= 12 else np.nan

            if np.isfinite(train_sharpe) and train_sharpe > best_sharpe:
                best_key, best_sharpe = key, train_sharpe

        test = base.month_in(cells[best_key], test_start, test_end) if best_key else pd.DataFrame()
        center_test = base.month_in(cells[CENTER], test_start, test_end)
        test_mean = float(test["excess"].mean()) if len(test) else np.nan
        rows.append({"validation_year": year, "train_years": f"{year - base.TRAIN_YEARS}-{year - 1}",
                     "train_months": int(len(base.month_in(cells[best_key], train_start, train_end))) if best_key else 0,
                     "chosen_top_n": best_key[0] if best_key else None, "chosen_value_weight": best_key[1] if best_key else None,
                     "chosen_rebalance_months": best_key[2] if best_key else None,
                     "train_sharpe": float(best_sharpe) if np.isfinite(best_sharpe) else np.nan,
                     "test_months": int(len(test)), "test_excess_annual": test_mean * 12 if np.isfinite(test_mean) else np.nan,
                     "test_excess_t": stats.one_sample_t(test["excess"]).t_statistic if len(test) >= 2 else np.nan,
                     "center_test_excess_annual": float(center_test["excess"].mean() * 12) if len(center_test) else np.nan,
                     "sign": int(np.sign(test_mean)) if np.isfinite(test_mean) else 0})

    return rows


def robustness(cells: dict) -> tuple:
    """27칸 격자(판정 구간): 칸마다 순초과수익 연율·샤프. 이웃 판정은 stats.grid_robustness."""
    rows = []
    grid_excess = {}
    grid_sharpe = {}

    for key, full_curve in cells.items():
        summary = base.excess_summary(base.month_in(full_curve, JUDGMENT_START, JUDGMENT_END))
        grid_excess[key] = summary["excess_annual"]
        grid_sharpe[key] = summary["sharpe"]
        rows.append({"top_n": key[0], "value_weight": key[1], "rebalance_months": key[2],
                     "excess_annual": summary["excess_annual"], "excess_t": summary["excess_t"],
                     "sharpe": summary["sharpe"], "max_drawdown_percent": summary["max_drawdown_percent"],
                     "turnover_oneway_monthly": summary["turnover_oneway_monthly"]})

    positive, neighbor_count, _ = stats.grid_robustness(grid_excess, CENTER)
    _, _, sharpe_ratio = stats.grid_robustness(grid_sharpe, CENTER)
    return rows, positive, neighbor_count, sharpe_ratio


# ── 추가 표 ───────────────────────────────────────────────────────────────────

def cap_weighted_benchmark(signals: dict, months: list) -> pd.Series:
    """참고 열 — 같은 달 유니버스 시총가중 수익(비용 없음)."""
    values = {}

    for month in months:
        frame = signals[month]
        caps = frame["market_cap"].astype(float)
        values[month] = float((frame["forward_return"] * caps).sum() / caps.sum()) if caps.sum() > 0 else np.nan

    return pd.Series(values)


def size_buckets(signals: dict, all_returns: dict, months: list, cost: CostSpec) -> tuple:
    """시총 3분위마다 상위 10 동일가중·중심 w·분기 리밸, 벤치마크는 그 분위 동일가중."""
    curves = []
    summaries = {}

    for label in BUCKET_LABELS:
        curve = simulate(signals, all_returns, months, BUCKET_TOP_N, CENTER[1], CENTER[2], cost, bucket=label)
        curve.insert(0, "bucket", label)
        curves.append(curve)
        summaries[label] = base.excess_summary(base.month_in(curve, JUDGMENT_START, JUDGMENT_END))

    return pd.concat(curves, ignore_index=True), summaries


def capacity_summary(capacity_rows: list) -> dict:
    frame = pd.DataFrame(capacity_rows)
    judgment = base.month_in(frame, JUDGMENT_START, JUDGMENT_END)
    summary = {"rebalance_months": int(len(judgment))}

    for limit in CAPACITY_LIMITS:
        column = f"capacity_{int(limit * 100)}pct_krw"
        values = judgment[column].replace(np.inf, np.nan).dropna() / BILLION_KRW
        summary[f"limit_{int(limit * 100)}pct"] = {"median_billion_krw": float(values.median()) if len(values) else np.nan,
                                                   "p10_billion_krw": float(values.quantile(0.10)) if len(values) else np.nan,
                                                   "min_billion_krw": float(values.min()) if len(values) else np.nan}

    return summary


def delisted_summary(dropped_rows: list, all_returns: dict, months: list) -> tuple:
    """마지막 종가로 청산된 보유 종목 — 전달 수익률과 회원 기간표 end_reason."""
    frame = pd.DataFrame(dropped_rows)

    if frame.empty:
        return frame, {"count": 0}

    previous_of = {months[index]: months[index - 1] for index in range(1, len(months))}
    frame["liquidation_month"] = frame["month"].map(previous_of)
    frame["liquidation_return"] = [float(all_returns[row.liquidation_month]["forward_return"].get(row.ticker, np.nan))
                                   if isinstance(row.liquidation_month, str) else np.nan for row in frame.itertuples(index=False)]
    membership = pd.read_parquet(MEMBERSHIP_PATH, columns=["code", "name", "end_reason", "delist_date", "delist_reason"])
    frame = frame.merge(membership, left_on="ticker", right_on="code", how="left").drop(columns="code")
    returns = frame["liquidation_return"].dropna()
    summary = {"count": int(len(frame)), "end_reason": {str(key): int(value) for key, value in frame["end_reason"].fillna("(없음)").value_counts().items()},
               "liquidation_return_mean": float(returns.mean()) if len(returns) else np.nan,
               "liquidation_return_min": float(returns.min()) if len(returns) else np.nan,
               "liquidation_return_below_minus_50pct": int((returns < -0.5).sum()),
               "weight_before_mean": float(frame["weight_before"].mean())}
    return frame, summary


# ── 출력 ─────────────────────────────────────────────────────────────────────

def write_readme(metrics: dict, walk_rows: list, robustness_rows: list) -> None:
    judgment = metrics["center_judgment"]
    standard_error = judgment["excess_monthly_std"] / math.sqrt(judgment["months"])
    verdict = metrics["verdict"]
    diagnostics = metrics["factor_diagnostics"]["judgment_period"]
    cost_rows = "\n".join(f"| {level} | {values['excess_annual'] * 100:.2f}% | {values['excess_t']:.2f} | {values['sharpe']:.2f} | "
                          f"{values['max_drawdown_percent']:.1f}% | {values['cost_drag_annual'] * 100:.2f}% |"
                          for level, values in metrics["cost_sensitivity"].items())
    walk_lines = "\n".join(f"| {row['validation_year']} | {row['train_years']} | N={row['chosen_top_n']}, w={row['chosen_value_weight']}, "
                           f"리밸 {row['chosen_rebalance_months']}개월 | {row['test_excess_annual'] * 100:+.2f}% | {row['test_excess_t']:.2f} | "
                           f"{row['center_test_excess_annual'] * 100:+.2f}% | {'+' if row['sign'] > 0 else '−'} |" for row in walk_rows)
    grid_lines = "\n".join(f"| {row['top_n']} | {row['value_weight']} | {row['rebalance_months']} | {row['excess_annual'] * 100:+.2f}% | "
                           f"{row['excess_t']:.2f} | {row['sharpe']:.2f} | {row['turnover_oneway_monthly'] * 100:.1f}% |" for row in robustness_rows)
    cap_weighted = metrics["cap_weighted"]
    bucket_lines = "\n".join(f"| {label} | {values['excess_annual'] * 100:+.2f}% | {values['excess_t']:.2f} | {values['sharpe']:.2f} | "
                             f"{values['max_drawdown_percent']:.1f}% | {values['turnover_oneway_monthly'] * 100:.1f}% | {values['cost_drag_annual'] * 100:.2f}% |"
                             for label, values in metrics["size_buckets"].items())
    capacity = metrics["capacity"]
    delisted = metrics["delisted_holdings"]
    observation = metrics["observation_window"]
    text = f"""# 22. 저PBR × 고ROE — 가치 비중 0.7·분기 리밸 (연구군: 종목레벨 · 재무 팩터)

> 한 줄 요약: {verdict['one_line']} 상태: **{verdict['overall']}**.
> 사전등록 `research/studies/22_pbr_roe_value_tilt/PREREG.md`(결과 전 확정). 스터디 19의 격자에서 눈에 띈 칸을 미리 중심으로 못 박고 다시 잰 것.
> 숫자 읽는 법(t·창·문턱의 출처): [../READING_NUMBERS.md](../READING_NUMBERS.md). 이 스터디의 t {judgment['excess_t']:.2f} = 판정 구간 {judgment['months']}개월 월 초과수익 평균 {judgment['excess_monthly_mean'] * 100:+.3f}% ÷ 표준오차 {standard_error * 100:.3f}%(표준편차 {judgment['excess_monthly_std'] * 100:.3f}% ÷ √{judgment['months']}). 2.0은 통계학 관행(우연 5%)이고, 5창 중 3창은 동전 던지기도 50% 통과하는 약한 기준이다.
> 데이터 등급 {metrics['grade']} — 일봉은 KIND 상폐사 보강 뒤 A(3,680종목), 재무는 2016년 이후 끝난 회사의 81%만 붙어 B. 배당 제외(보수적).

## 1. 판정 (비용 {metrics['cost_level'].upper()}, 판정 구간 {metrics['holdout']})

| 층 | 기준 | 값 | 판정 |
|---|---|---|---|
| 1 초과수익 t | ≥ {base.LAYER1_T} | {judgment['excess_t']:.2f} (뉴이-웨스트 {judgment['excess_t_newey_west']:.2f}) | {'통과' if verdict['layer1'] else '미달'} |
| 2 walk-forward | 5창 중 ≥ {base.LAYER2_MIN_POSITIVE} 양수 | {metrics['walk_forward_windows_positive']} / {len(walk_rows)} | {'통과' if verdict['layer2'] else '미달'} |
| 3 ±1 이웃 | 이웃 전부 > 0, 중심 샤프 ≥ 격자 최대의 70% | 양수 {metrics['robustness_neighbors_positive']} / {metrics['robustness_neighbors']}, 샤프 비 {metrics['robustness_center_sharpe_ratio']:.2f} | {'통과' if verdict['layer3'] else '미달'} |

중심 셀(N=30, w=0.7, 분기 리밸) 판정 구간: 순초과수익 연 {judgment['excess_annual'] * 100:+.2f}%, 전략 연복리 {judgment['net_annual_return'] * 100:.2f}% 대 유니버스 동일가중 {judgment['benchmark_annual_return'] * 100:.2f}%,
샤프 {judgment['sharpe']:.2f}, 최대 낙폭 {judgment['max_drawdown_percent']:.1f}%, 편도 회전 월 {judgment['turnover_oneway_monthly'] * 100:.1f}%, 비용 연 {judgment['cost_drag_annual'] * 100:.2f}%.
전 구간({metrics['period']}, {metrics['center_full']['months']}개월): 순초과수익 연 {metrics['center_full']['excess_annual'] * 100:+.2f}%, t {metrics['center_full']['excess_t']:.2f}, 샤프 {metrics['center_full']['sharpe']:.2f}.
스터디 19 중심(N=30, w=0.5, 월 리밸)은 같은 구간 순초과수익 연 +6.99%, t 1.14였다.

**읽을 때 주의**: 이 중심 셀은 스터디 19의 격자(같은 판정 구간)를 보고 고른 것이다. 사전등록으로 격자를 더 넓히지 못하게는 했지만
판정 구간 자체가 이미 한 번 본 구간이라, 진짜로 안 본 구간은 관찰창 {observation['months']}개월(2026-01~08: 순초과수익 연 {observation['excess_annual'] * 100:+.2f}%,
t {observation['excess_t']:.2f})과 앞으로 쌓이는 forward뿐이다. "채택 후보"는 라이브 소액 forward 검증으로 넘긴다는 뜻이지 확정이 아니다.

보조(판정 구간, w=0.7 점수): 순위 IC 평균 {diagnostics['ic_mean']:.4f}, IC t {diagnostics['ic_t']:.2f}, ICIR {diagnostics['icir']:.2f}, 5분위 단조 {diagnostics['quintile_monotonic_spearman']:.2f},
Q5−Q1 동일가중 연 {diagnostics['q5_q1_annual_ew'] * 100:+.2f}% (t {diagnostics['q5_q1_t_ew']:.2f}).

## 2. 비용 감도 (중심 셀, 판정 구간)

| 비용 | 초과수익 연 | t | 샤프 | 최대 낙폭 | 비용 연 |
|---|---|---|---|---|---|
{cost_rows}

## 3. walk-forward (학습 3년 → 검증 1년, 27칸 중 학습창이 고른 칸)

| 검증 | 학습 | 고른 칸 | 검증 초과수익 연 | t | 중심 셀 검증 | 부호 |
|---|---|---|---|---|---|---|
{walk_lines}

## 4. 격자 {len(robustness_rows)}칸 (판정 구간, 사전등록 trials_prior = {metrics['trials_prior']})

| N | w | 리밸(월) | 초과수익 연 | t | 샤프 | 편도 회전/월 |
|---|---|---|---|---|---|---|
{grid_lines}

## 5. 추가 표 (판정 층 아님)

### 5-1. 시총가중 (같은 상위 30, 가중만 시총 비례)

| 가중 | 초과수익 연(대 유니버스 동일가중) | t | 샤프 | 최대 낙폭 | 참고: 대 유니버스 시총가중 |
|---|---|---|---|---|---|
| 동일가중(중심) | {judgment['excess_annual'] * 100:+.2f}% | {judgment['excess_t']:.2f} | {judgment['sharpe']:.2f} | {judgment['max_drawdown_percent']:.1f}% | {metrics['center_vs_cap_benchmark_annual'] * 100:+.2f}% |
| 시총가중 | {cap_weighted['excess_annual'] * 100:+.2f}% | {cap_weighted['excess_t']:.2f} | {cap_weighted['sharpe']:.2f} | {cap_weighted['max_drawdown_percent']:.1f}% | {metrics['cap_weighted_vs_cap_benchmark_annual'] * 100:+.2f}% |

시총가중/동일가중 초과수익 비 {metrics['cap_over_equal_ratio']:.2f} (0.5 아래면 "소형주 전용"으로 적기로 사전등록).

### 5-2. 크기 3분위 분해 (분위 안 상위 10 동일가중, 벤치마크 = 그 분위 동일가중)

| 분위 | 초과수익 연 | t | 샤프 | 최대 낙폭 | 편도 회전/월 | 비용 연 |
|---|---|---|---|---|---|---|
{bucket_lines}

### 5-3. 용량 (리밸 달 {capacity['rebalance_months']}개, 종목별 주문 ≤ 20일 평균 거래대금 × 한도)

| 한도 | 중앙값 | 하위 10% | 최소 |
|---|---|---|---|
| 10% | {capacity['limit_10pct']['median_billion_krw']:.1f}억 | {capacity['limit_10pct']['p10_billion_krw']:.1f}억 | {capacity['limit_10pct']['min_billion_krw']:.1f}억 |
| 5% | {capacity['limit_5pct']['median_billion_krw']:.1f}억 | {capacity['limit_5pct']['p10_billion_krw']:.1f}억 | {capacity['limit_5pct']['min_billion_krw']:.1f}억 |

용량이 달마다 거의 같은 것은 유니버스 하한(20일 평균 거래대금 10억)에 걸린 종목이 리밸 때마다 상위 30에 한둘은 들어오기 때문이다
(0.10 × 10억 ÷ (1/30) = 30억). 즉 한도는 특정 달이 아니라 "가장 안 거래되는 편입 종목"이 정한다 — 거래대금 하한을 올린 유니버스는 다음 사전등록 후보.

### 5-4. 상폐 종목 보유 기록 (전 구간, 마지막 종가로 청산된 보유)

건수 {delisted['count']}, end_reason {delisted.get('end_reason', {})}, 청산 달 수익률 평균 {delisted.get('liquidation_return_mean', float('nan')) * 100:+.1f}%
(최소 {delisted.get('liquidation_return_min', float('nan')) * 100:+.1f}%, −50% 아래 {delisted.get('liquidation_return_below_minus_50pct', 0)}건), 청산 직전 평균 가중 {delisted.get('weight_before_mean', float('nan')) * 100:.1f}%.
목록은 delisted_holdings.csv.

## 6. 표본 · 데이터

- 종목-월 {metrics['sample_n']:,}, 월 유니버스 중앙값 {metrics['universe_size_median']}, 첫 신호 {metrics['first_signal_month']}, 재무 폴백 비율 {metrics['ttm_fallback_share'] * 100:.1f}%.
- 일봉 패널 3,680종목(상폐 939, `PYQuant/tools/kind_delisted_fill.py`), 회원 기간표 `PYQuant/data/universe/membership.parquet`.
- 주식수 출처: 2020~ data.go.kr 월말(A), 2015~2019 DART 사업보고서(B). 주식수 없는 달은 유니버스에서 빠진다.

## 7. 재현

`{metrics['rerun_command']}` · 커밋 {metrics['git_commit']} · seed {SEED}(난수 없음) · 데이터 지문은 metrics.json `data_fingerprint`.
산출물: metrics.json · curves.csv · walkforward.csv · robustness.csv · quintiles.csv · holdings.csv · size_buckets.csv · capacity.csv · delisted_holdings.csv.
"""
    (STUDY_DIR / "README.md").write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cost", choices=tuple(RESEARCH_BY_LEVEL), default="mid")
    arguments = parser.parse_args()
    np.random.seed(SEED)
    started = time.time()

    data = fundamental.FundamentalData.load()
    calendar = base.month_calendar(data.bars.trading_days)
    signals, all_returns = base.build_signals(data, calendar)
    months = sorted(signals)
    print(f"신호 {len(months)}개월 {months[0]}~{months[-1]}, 적재 {time.time() - started:.0f}초", flush=True)

    cost = RESEARCH_BY_LEVEL[arguments.cost]
    capacity_rows: list = []
    dropped_rows: list = []
    center_curve = simulate(signals, all_returns, months, *CENTER, cost, capacity_rows=capacity_rows, dropped_rows=dropped_rows)
    center_judgment = base.excess_summary(base.month_in(center_curve, JUDGMENT_START, JUDGMENT_END))
    center_full = base.excess_summary(center_curve)
    observation = base.excess_summary(base.month_in(center_curve, JUDGMENT_END + pd.Timedelta(days=1), base.OBSERVATION_END))
    cost_sensitivity = {level: base.excess_summary(base.month_in(simulate(signals, all_returns, months, *CENTER, specification), JUDGMENT_START, JUDGMENT_END))
                        for level, specification in RESEARCH_BY_LEVEL.items()}
    cells = grid_cells(signals, all_returns, months, cost)
    walk_rows = walk_forward(cells)
    robustness_rows, neighbors_positive, neighbor_count, sharpe_ratio = robustness(cells)
    quintiles, diagnostics = base.factor_diagnostics(signals, months)

    cap_curve = simulate(signals, all_returns, months, *CENTER, cost, weighting="cap")
    cap_benchmark = cap_weighted_benchmark(signals, months)
    center_curve["benchmark_cap_weighted"] = center_curve["month"].map(cap_benchmark)
    cap_curve["benchmark_cap_weighted"] = cap_curve["month"].map(cap_benchmark)
    cap_judgment = base.excess_summary(base.month_in(cap_curve, JUDGMENT_START, JUDGMENT_END))
    center_judgment_curve = base.month_in(center_curve, JUDGMENT_START, JUDGMENT_END)
    cap_judgment_curve = base.month_in(cap_curve, JUDGMENT_START, JUDGMENT_END)
    bucket_curves, bucket_summaries = size_buckets(signals, all_returns, months, cost)
    capacity = capacity_summary(capacity_rows)
    delisted_frame, delisted = delisted_summary(dropped_rows, all_returns, months)

    positive_windows = sum(1 for row in walk_rows if row["sign"] > 0)
    layer1 = bool(np.isfinite(center_judgment["excess_t"]) and center_judgment["excess_t"] >= base.LAYER1_T and center_judgment["excess_annual"] > 0)
    layer2 = positive_windows >= base.LAYER2_MIN_POSITIVE
    layer3 = bool(neighbor_count > 0 and neighbors_positive == neighbor_count and np.isfinite(sharpe_ratio) and sharpe_ratio >= base.LAYER3_SHARPE_RATIO)
    overall = "채택 후보" if (layer1 and layer2 and layer3) else ("조건부" if layer1 and (layer2 or layer3) else "미달")
    cap_over_equal = cap_judgment["excess_annual"] / center_judgment["excess_annual"] if center_judgment["excess_annual"] > 0 else np.nan
    small_only = (np.isfinite(cap_over_equal) and cap_over_equal < 0.5) or (np.isfinite(capacity["limit_10pct"]["median_billion_krw"])
                                                                            and capacity["limit_10pct"]["median_billion_krw"] < 10)
    one_line = (f"저PBR×고ROE 상위 30 동일가중(w=0.7, 분기 리밸)은 비용 {arguments.cost.upper()} 뒤 판정 구간 초과수익 연 "
                f"{center_judgment['excess_annual'] * 100:+.2f}%(t {center_judgment['excess_t']:.2f}), walk-forward {positive_windows}/{len(walk_rows)}창 양수, "
                f"이웃 {neighbors_positive}/{neighbor_count} 양수. 시총가중은 연 {cap_judgment['excess_annual'] * 100:+.2f}%, 용량(10% 한도) 중앙값 "
                f"{capacity['limit_10pct']['median_billion_krw']:.0f}억{' — 소형주·소액 전용' if small_only else ''}.")

    sample_n = int(sum(len(frame) for frame in signals.values()))
    fallback_share = float(np.mean([frame["ttm_is_fallback"].eq(True).mean() for frame in signals.values()]))
    metrics = {
        "study": "22_pbr_roe_value_tilt/pbr_roe_top30_w07_quarterly",
        "sample_n": sample_n,
        "period": f"{months[0]}~{months[-1]}",
        "holdout": f"{JUDGMENT_START:%Y-%m}~{JUDGMENT_END:%Y-%m} (walk-forward 검증 5창 합)",
        "sharpe": center_judgment["sharpe"],
        "mdd": center_judgment["max_drawdown_percent"],
        "t_stat": center_judgment["excess_t"],
        "walk_forward_windows_positive": positive_windows,
        "walk_forward_windows": len(walk_rows),
        "trials_prior": len(robustness_rows),
        "cost_level": arguments.cost,
        "cost_spec": {"commission_percent": cost.commission_percent, "sell_tax_percent": cost.sell_tax_percent,
                      "slippage_ticks": cost.slippage_ticks, "impact_percent": cost.impact_percent},
        "grade": "B",
        "verdict": {"layer1": layer1, "layer2": layer2, "layer3": layer3, "overall": overall, "small_only": bool(small_only), "one_line": one_line},
        "center_cell": {"top_n": CENTER[0], "value_weight": CENTER[1], "rebalance_months": CENTER[2], "buffer_rank": base.BUFFER_MULTIPLE * CENTER[0]},
        "center_judgment": center_judgment,
        "center_full": center_full,
        "observation_window": observation,
        "cost_sensitivity": cost_sensitivity,
        "walkforward": walk_rows,
        "robustness_neighbors_positive": neighbors_positive,
        "robustness_neighbors": neighbor_count,
        "robustness_center_sharpe_ratio": sharpe_ratio,
        "robustness": robustness_rows,
        "factor_diagnostics": diagnostics,
        "cap_weighted": cap_judgment,
        "cap_over_equal_ratio": cap_over_equal,
        "center_vs_cap_benchmark_annual": float((center_judgment_curve["net"] - center_judgment_curve["benchmark_cap_weighted"]).mean() * 12),
        "cap_weighted_vs_cap_benchmark_annual": float((cap_judgment_curve["net"] - cap_judgment_curve["benchmark_cap_weighted"]).mean() * 12),
        "size_buckets": bucket_summaries,
        "capacity": capacity,
        "delisted_holdings": delisted,
        "universe_size_median": int(np.median([len(frame) for frame in signals.values()])),
        "first_signal_month": months[0],
        "ttm_fallback_share": fallback_share,
        "benchmark": "같은 달 유니버스 동일가중, 비용 없음",
        "dividends": "제외(일봉에 배당 없음, 보수적)",
        "seed": SEED,
        "git_commit": base.git_commit(),
        "data_fingerprint": {**base.data_fingerprint(), "membership": {"size": MEMBERSHIP_PATH.stat().st_size}},
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "rerun_command": f"py research/studies/22_pbr_roe_value_tilt/run_value_tilt.py --cost {arguments.cost}",
        "elapsed_seconds": round(time.time() - started, 1),
    }
    (STUDY_DIR / "metrics.json").write_text(json.dumps(base.clean(metrics), ensure_ascii=False, indent=2), encoding="utf-8")

    center_curve.drop(columns="members").to_csv(STUDY_DIR / "curves.csv", index=False)
    pd.DataFrame(walk_rows).to_csv(STUDY_DIR / "walkforward.csv", index=False)
    pd.DataFrame(robustness_rows).to_csv(STUDY_DIR / "robustness.csv", index=False)
    quintiles.to_csv(STUDY_DIR / "quintiles.csv")
    holdings = pd.DataFrame([{"month": row.month, "ticker": member.split(":")[0], "weight": float(member.split(":")[1])}
                             for row in center_curve.itertuples(index=False) for member in row.members.split(";") if member])
    holdings.to_csv(STUDY_DIR / "holdings.csv", index=False)
    bucket_curves.drop(columns="members").to_csv(STUDY_DIR / "size_buckets.csv", index=False)
    pd.DataFrame(capacity_rows).to_csv(STUDY_DIR / "capacity.csv", index=False)
    delisted_frame.to_csv(STUDY_DIR / "delisted_holdings.csv", index=False)
    write_readme(metrics, walk_rows, robustness_rows)

    print(json.dumps({key: base.clean(metrics[key]) for key in ("sample_n", "period", "holdout", "sharpe", "mdd", "t_stat",
                                                                 "walk_forward_windows_positive", "trials_prior", "cost_level", "grade")},
                     ensure_ascii=False, indent=2))
    print(one_line, f"→ {overall}. {time.time() - started:.0f}초")
    return 0


if __name__ == "__main__":
    sys.exit(main())
