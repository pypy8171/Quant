"""스터디 19 · 저PBR × 고ROE 복합 — 월 리밸 상위 N 동일가중, walk-forward 5창.

사전등록 `research/studies/19_fundamental_factors/PREREG.md`를 그대로 옮긴 하네스다. 신호는
`PYQuant/features/fundamental.py::FundamentalData.compute(as_of)`(라이브와 같은 함수)에서만 받고,
비용은 `PYQuant/backtest/costs.py`의 RESEARCH_LOW/MID/HIGH, 통계는 `PYQuant/backtest/stats.py`를 쓴다.

흐름: 달력(기준일=전월 말 거래일, 체결=당월 첫 거래일 종가) → 기준일마다 compute → 다음 달 첫 거래일 종가까지 수익 →
      포트폴리오(버퍼 2N, 가중 드리프트 추적, 매수·매도 금액에 틱 환산 비용) → 벤치마크(유니버스 동일가중) → 판정 세 층.

산출물(같은 폴더): metrics.json, curves.csv(월별 전략·벤치·비용·회전), walkforward.csv, robustness.csv,
                 quintiles.csv(월별 5분위 수익·IC), holdings.csv(월·종목·가중), README.md.

재현: py research/studies/19_fundamental_factors/run_pbr_roe.py --cost mid   (seed 20260919, 난수는 쓰지 않는다)
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import subprocess
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
SEED = 20260919
SIGNAL_START = pd.Timestamp("2016-01-01")
OBSERVATION_END = pd.Timestamp("2026-08-31")
VALIDATION_YEARS = (2021, 2022, 2023, 2024, 2025)
TRAIN_YEARS = 3
JUDGMENT_START, JUDGMENT_END = pd.Timestamp("2021-01-01"), pd.Timestamp("2025-12-31")
TOP_N_GRID = (20, 30, 40)
VALUE_WEIGHT_GRID = (0.3, 0.5, 0.7)
REBALANCE_GRID = (1, 3)
CENTER = (30, 0.5, 1)
BUFFER_MULTIPLE = 2
MIN_UNIVERSE = 100
LAYER1_T = 2.0
LAYER2_MIN_POSITIVE = 3
LAYER3_SHARPE_RATIO = 0.7
NEWEY_WEST_LAG = 3


# ── 달력 · 신호 · 수익 ─────────────────────────────────────────────────────────

def month_calendar(trading_days: pd.DatetimeIndex) -> pd.DataFrame:
    """월마다 (기준일 = 전월 마지막 거래일, 체결일 = 당월 첫 거래일, 청산일 = 다음 달 첫 거래일)."""
    days = pd.Series(trading_days)
    by_month = days.groupby(days.dt.to_period("M"))
    first = by_month.min()
    last = by_month.max()
    rows = []

    for position in range(1, len(first) - 1):
        rows.append({"month": str(first.index[position]), "as_of": last.iloc[position - 1],
                     "entry": first.iloc[position], "exit": first.iloc[position + 1]})

    calendar = pd.DataFrame(rows)
    return calendar[(calendar["entry"] >= SIGNAL_START) & (calendar["entry"] <= OBSERVATION_END)].reset_index(drop=True)


def month_returns(bars: fundamental.BarsPanel, entry: pd.Timestamp, exit_day: pd.Timestamp) -> pd.Series:
    """체결일 종가 → 청산일 종가. 청산일 봉이 없으면 그 전 마지막 종가(상폐·정지 청산). 체결일 봉이 없으면 결측(못 산다)."""
    entry_close = bars.close.loc[entry]
    exit_close = bars.close.loc[entry:exit_day].ffill().loc[exit_day]
    returns = exit_close / entry_close - 1.0
    return returns[entry_close.notna() & (entry_close > 0)]


def build_signals(data: fundamental.FundamentalData, calendar: pd.DataFrame) -> tuple:
    """기준일마다 compute 한 번. 반환 ({month: 유니버스 DataFrame[ticker, z_value, z_quality, forward_return, entry_close, …]},
    {month: 전 종목 DataFrame[forward_return, entry_close]}) — 뒤의 것은 유니버스에서 빠진 보유 종목의 수익·청산가에 쓴다."""
    signals = {}
    all_returns = {}

    for row in calendar.itertuples(index=False):
        features = data.compute(row.as_of)

        if len(features) < MIN_UNIVERSE:
            continue

        forward = month_returns(data.bars, row.entry, row.exit)
        entry_close = data.bars.close.loc[row.entry]
        features = features.set_index("ticker")
        features["forward_return"] = forward.reindex(features.index)
        features["entry_close"] = entry_close.reindex(features.index)
        features = features[features["forward_return"].notna()]
        signals[row.month] = features
        all_returns[row.month] = pd.DataFrame({"forward_return": forward, "entry_close": entry_close.reindex(forward.index)})

    return signals, all_returns


# ── 포트폴리오 ─────────────────────────────────────────────────────────────────

def simulate(signals: dict, all_returns: dict, months: list, top_n: int, value_weight: float, rebalance_every: int,
             cost: CostSpec) -> pd.DataFrame:
    """상위 top_n 동일가중, rebalance_every개월마다 교체(버퍼 2N), 사이 달은 가중 드리프트. 월별 gross·cost·net·benchmark·turnover.

    유니버스에서 빠진 보유 종목은 리밸 달에 판다(비용 있음). 다음 달 첫 거래일 봉이 없는 종목은 전달 마지막 종가로 이미 청산된 것이라
    가중을 현금(수익 0)으로 돌린다. 비용은 체결가(당월 첫 거래일 종가)의 틱으로 환산해 매수·매도 금액에 곱한다.
    """
    holdings: dict = {}          # ticker → 가중(달 시작, 비용 차감 전)
    records = []

    for position, month in enumerate(months):
        frame = signals[month]
        tradable = all_returns[month]
        score = value_weight * frame["z_value"] + (1.0 - value_weight) * frame["z_quality"]
        ranked = score.sort_values(ascending=False)
        rank_of = pd.Series(np.arange(1, len(ranked) + 1), index=ranked.index)
        benchmark = float(frame["forward_return"].mean())

        holdings = {ticker: weight for ticker, weight in holdings.items() if ticker in tradable.index}

        if position % rebalance_every == 0 or not holdings:
            kept = [ticker for ticker in holdings if rank_of.get(ticker, np.inf) <= BUFFER_MULTIPLE * top_n]
            fill = [ticker for ticker in ranked.index if ticker not in kept][: max(top_n - len(kept), 0)]
            members = kept + fill
            target = {ticker: 1.0 / len(members) for ticker in members}
        else:
            target = dict(holdings)

        buy_cost = 0.0
        sell_cost = 0.0
        turnover_oneway = 0.0

        for ticker in set(holdings) | set(target):
            change = target.get(ticker, 0.0) - holdings.get(ticker, 0.0)
            price = float(tradable.loc[ticker, "entry_close"])

            if change > 0:
                buy_cost += change * side_cost_percent_at_price(cost, "BUY", price) / 100.0
                turnover_oneway += change
            elif change < 0:
                sell_cost += -change * side_cost_percent_at_price(cost, "SELL", price) / 100.0

        gross = sum(weight * float(tradable.loc[ticker, "forward_return"]) for ticker, weight in target.items())
        cost_total = buy_cost + sell_cost
        net = gross - cost_total

        grown = {ticker: weight * (1.0 + float(tradable.loc[ticker, "forward_return"])) for ticker, weight in target.items()}
        total = sum(grown.values())
        holdings = {ticker: value / total for ticker, value in grown.items()} if total > 0 else {}

        records.append({"month": month, "gross": gross, "cost": cost_total, "net": net, "benchmark": benchmark,
                        "excess": net - benchmark, "turnover_oneway": turnover_oneway, "holdings": len(target),
                        "universe": len(frame), "members": ";".join(sorted(target))})

    return pd.DataFrame(records)


def excess_summary(curve: pd.DataFrame) -> dict:
    excess = curve["excess"]
    test = stats.one_sample_t(excess)
    newey = stats.newey_west_t(excess, NEWEY_WEST_LAG)
    performance = stats.performance_summary(curve["net"], periods_per_year=12)
    return {"months": int(len(curve)), "excess_monthly_mean": test.mean, "excess_t": test.t_statistic,
            "excess_t_newey_west": newey.t_statistic, "excess_annual": float(excess.mean() * 12),
            "net_annual_return": performance["annual_return"], "sharpe": performance["sharpe"],
            "max_drawdown_percent": performance["max_drawdown_percent"],
            "benchmark_annual_return": stats.performance_summary(curve["benchmark"], periods_per_year=12)["annual_return"],
            "turnover_oneway_monthly": float(curve["turnover_oneway"].mean()),
            "cost_drag_annual": float(curve["cost"].mean() * 12)}


def month_in(curve: pd.DataFrame, start: pd.Timestamp, end: pd.Timestamp) -> pd.DataFrame:
    stamps = pd.PeriodIndex(curve["month"], freq="M").to_timestamp()
    return curve[(stamps >= start) & (stamps <= end)].reset_index(drop=True)


# ── 판정 층 ───────────────────────────────────────────────────────────────────

def walk_forward(signals: dict, all_returns: dict, months: list, cost: CostSpec) -> list:
    """검증 연도마다 직전 3년 학습창에서 (N, w) 9칸 중 순초과수익 샤프 최대 칸을 골라 검증 연도에 적용."""
    cells = {(top_n, weight): simulate(signals, all_returns, months, top_n, weight, 1, cost) for top_n in TOP_N_GRID for weight in VALUE_WEIGHT_GRID}
    rows = []

    for year in VALIDATION_YEARS:
        train_start, train_end = pd.Timestamp(year=year - TRAIN_YEARS, month=1, day=1), pd.Timestamp(year=year - 1, month=12, day=31)
        test_start, test_end = pd.Timestamp(year=year, month=1, day=1), pd.Timestamp(year=year, month=12, day=31)
        best_key, best_sharpe = None, -np.inf

        for key, curve in cells.items():
            train = month_in(curve, train_start, train_end)
            train_sharpe = stats.performance_summary(train["excess"], periods_per_year=12)["sharpe"] if len(train) >= 12 else np.nan

            if np.isfinite(train_sharpe) and train_sharpe > best_sharpe:
                best_key, best_sharpe = key, train_sharpe

        test = month_in(cells[best_key], test_start, test_end) if best_key else pd.DataFrame()
        center_test = month_in(cells[(CENTER[0], CENTER[1])], test_start, test_end)
        test_mean = float(test["excess"].mean()) if len(test) else np.nan
        rows.append({"validation_year": year, "train_years": f"{year - TRAIN_YEARS}-{year - 1}",
                     "train_months": int(len(month_in(cells[best_key], train_start, train_end))) if best_key else 0,
                     "chosen_top_n": best_key[0] if best_key else None, "chosen_value_weight": best_key[1] if best_key else None,
                     "train_sharpe": float(best_sharpe) if np.isfinite(best_sharpe) else np.nan,
                     "test_months": int(len(test)), "test_excess_annual": test_mean * 12 if np.isfinite(test_mean) else np.nan,
                     "test_excess_t": stats.one_sample_t(test["excess"]).t_statistic if len(test) >= 2 else np.nan,
                     "center_test_excess_annual": float(center_test["excess"].mean() * 12) if len(center_test) else np.nan,
                     "sign": int(np.sign(test_mean)) if np.isfinite(test_mean) else 0})

    return rows


def robustness(signals: dict, all_returns: dict, months: list, cost: CostSpec) -> tuple:
    """18칸 격자(판정 구간): 칸마다 순초과수익 연율·샤프. 이웃 판정은 stats.grid_robustness."""
    rows = []
    grid_excess = {}
    grid_sharpe = {}

    for top_n in TOP_N_GRID:
        for weight in VALUE_WEIGHT_GRID:
            for rebalance_every in REBALANCE_GRID:
                curve = month_in(simulate(signals, all_returns, months, top_n, weight, rebalance_every, cost), JUDGMENT_START, JUDGMENT_END)
                summary = excess_summary(curve)
                key = (top_n, weight, rebalance_every)
                grid_excess[key] = summary["excess_annual"]
                grid_sharpe[key] = summary["sharpe"]
                rows.append({"top_n": top_n, "value_weight": weight, "rebalance_months": rebalance_every,
                             "excess_annual": summary["excess_annual"], "excess_t": summary["excess_t"],
                             "sharpe": summary["sharpe"], "max_drawdown_percent": summary["max_drawdown_percent"],
                             "turnover_oneway_monthly": summary["turnover_oneway_monthly"]})

    positive, neighbor_count, _ = stats.grid_robustness(grid_excess, CENTER)
    _, _, sharpe_ratio = stats.grid_robustness(grid_sharpe, CENTER)
    return rows, positive, neighbor_count, sharpe_ratio


def factor_diagnostics(signals: dict, months: list) -> tuple:
    """순위 IC·5분위(동일가중, 중심 w) — 전 구간과 판정 구간."""
    score_panel = pd.DataFrame({month: CENTER[1] * frame["z_value"] + (1 - CENTER[1]) * frame["z_quality"]
                                for month, frame in signals.items() if month in months}).T
    return_panel = pd.DataFrame({month: frame["forward_return"] for month, frame in signals.items() if month in months}).T
    ic = stats.rank_ic(score_panel, return_panel)
    quintiles = stats.quintile_spread(score_panel, return_panel)
    quintiles["rank_ic"] = ic
    quintiles.index.name = "month"

    def summarize(frame: pd.DataFrame) -> dict:
        ic_test = stats.one_sample_t(frame["rank_ic"])
        spread_test = stats.one_sample_t(frame["spread"])
        summary = stats.quintile_summary(frame)
        return {"months": int(len(frame)), "ic_mean": ic_test.mean, "ic_t": ic_test.t_statistic,
                "icir": stats.ic_information_ratio(frame["rank_ic"]),
                "quintile_monthly_mean_ew": [summary[f"q{index}"] for index in range(1, 6)],
                "quintile_monotonic_spearman": summary["quintile_monotonic_spearman"],
                "q5_q1_annual_ew": spread_test.mean * 12, "q5_q1_t_ew": spread_test.t_statistic}

    stamps = pd.PeriodIndex(quintiles.index, freq="M").to_timestamp()
    judgment = quintiles[(stamps >= JUDGMENT_START) & (stamps <= JUDGMENT_END)]
    return quintiles, {"full_period": summarize(quintiles), "judgment_period": summarize(judgment)}


# ── 재현 정보 · 출력 ───────────────────────────────────────────────────────────

def data_fingerprint() -> dict:
    fingerprint = {}

    for path in (fundamental.FIN_PATH, fundamental.SHARES_PATH, fundamental.BARS_PATH):
        digest = hashlib.sha256()
        digest.update(str(path.stat().st_size).encode())
        digest.update(str(int(path.stat().st_mtime)).encode())
        fingerprint[str(path.relative_to(REPO_ROOT)).replace("\\", "/")] = {"size": path.stat().st_size, "sha256_size_mtime": digest.hexdigest()[:16]}

    return fingerprint


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT, capture_output=True, text=True, check=False).stdout.strip()
    except OSError:
        return ""


def clean(value):
    if isinstance(value, float) and not np.isfinite(value):
        return None

    if isinstance(value, (np.floating, np.integer)):
        return clean(value.item())

    if isinstance(value, dict):
        return {key: clean(item) for key, item in value.items()}

    if isinstance(value, (list, tuple)):
        return [clean(item) for item in value]

    return value


def write_readme(metrics: dict, walk_rows: list, robustness_rows: list) -> None:
    judgment = metrics["center_judgment"]
    cost_rows = "\n".join(f"| {level} | {values['excess_annual'] * 100:.2f}% | {values['excess_t']:.2f} | {values['sharpe']:.2f} | "
                          f"{values['max_drawdown_percent']:.1f}% | {values['cost_drag_annual'] * 100:.2f}% |"
                          for level, values in metrics["cost_sensitivity"].items())
    walk_lines = "\n".join(f"| {row['validation_year']} | {row['train_years']} | N={row['chosen_top_n']}, w={row['chosen_value_weight']} | "
                           f"{row['test_excess_annual'] * 100:+.2f}% | {row['test_excess_t']:.2f} | {row['center_test_excess_annual'] * 100:+.2f}% | "
                           f"{'+' if row['sign'] > 0 else '−'} |" for row in walk_rows)
    grid_lines = "\n".join(f"| {row['top_n']} | {row['value_weight']} | {row['rebalance_months']} | {row['excess_annual'] * 100:+.2f}% | "
                           f"{row['excess_t']:.2f} | {row['sharpe']:.2f} | {row['turnover_oneway_monthly'] * 100:.1f}% |" for row in robustness_rows)
    diagnostics = metrics["factor_diagnostics"]["judgment_period"]
    verdict = metrics["verdict"]
    text = f"""# 19. 저PBR × 고ROE 복합 (연구군: 종목레벨 · 재무 팩터)

> 한 줄 요약: {verdict['one_line']} 상태: **{verdict['overall']}**.
> 사전등록 `research/studies/19_fundamental_factors/PREREG.md`(결과 전 확정), 스펙 `research/RESET_2026-09-19_R2/fundamental-quant.md` 후보 1.
> 데이터 등급 {metrics['grade']} — 재무 표가 corpCode 현재 목록 기반이라 옛 상폐사 일부가 빠진다. 배당 제외(보수적).

## 1. 판정 (비용 {metrics['cost_level'].upper()}, 판정 구간 {metrics['holdout']})

| 층 | 기준 | 값 | 판정 |
|---|---|---|---|
| 1 초과수익 t | ≥ {LAYER1_T} | {judgment['excess_t']:.2f} (뉴이-웨스트 {judgment['excess_t_newey_west']:.2f}) | {'통과' if verdict['layer1'] else '미달'} |
| 2 walk-forward | 5창 중 ≥ {LAYER2_MIN_POSITIVE} 양수 | {metrics['walk_forward_windows_positive']} / {len(walk_rows)} | {'통과' if verdict['layer2'] else '미달'} |
| 3 ±1 이웃 | 이웃 전부 > 0, 중심 샤프 ≥ 격자 최대의 70% | 양수 {metrics['robustness_neighbors_positive']} / {metrics['robustness_neighbors']}, 샤프 비 {metrics['robustness_center_sharpe_ratio']:.2f} | {'통과' if verdict['layer3'] else '미달'} |

중심 셀(N=30, w=0.5, 월 리밸) 판정 구간: 순초과수익 연 {judgment['excess_annual'] * 100:+.2f}%, 전략 연복리 {judgment['net_annual_return'] * 100:.2f}% 대 유니버스 동일가중 {judgment['benchmark_annual_return'] * 100:.2f}%,
샤프 {judgment['sharpe']:.2f}, 최대 낙폭 {judgment['max_drawdown_percent']:.1f}%, 편도 회전 월 {judgment['turnover_oneway_monthly'] * 100:.1f}%, 비용 연 {judgment['cost_drag_annual'] * 100:.2f}%.
전 구간({metrics['period']}, {metrics['center_full']['months']}개월): 순초과수익 연 {metrics['center_full']['excess_annual'] * 100:+.2f}%, t {metrics['center_full']['excess_t']:.2f}, 샤프 {metrics['center_full']['sharpe']:.2f}.

보조(판정 구간): 순위 IC 평균 {diagnostics['ic_mean']:.4f}, IC t {diagnostics['ic_t']:.2f}, ICIR {diagnostics['icir']:.2f}, 5분위 단조 {diagnostics['quintile_monotonic_spearman']:.2f},
Q5−Q1 동일가중 연 {diagnostics['q5_q1_annual_ew'] * 100:+.2f}% (t {diagnostics['q5_q1_t_ew']:.2f}). 시총가중 분위는 이번 실행에 없다(다음 실행).

## 2. 비용 감도 (중심 셀, 판정 구간)

| 비용 | 초과수익 연 | t | 샤프 | 최대 낙폭 | 비용 연 |
|---|---|---|---|---|---|
{cost_rows}

## 3. walk-forward (학습 3년 → 검증 1년, 학습창이 고른 칸)

| 검증 | 학습 | 고른 칸 | 검증 초과수익 연 | t | 중심 셀 검증 | 부호 |
|---|---|---|---|---|---|---|
{walk_lines}

## 4. 격자 {len(robustness_rows)}칸 (판정 구간, 사전등록 trials_prior = {metrics['trials_prior']})

| N | w | 리밸(월) | 초과수익 연 | t | 샤프 | 편도 회전/월 |
|---|---|---|---|---|---|---|
{grid_lines}

## 5. 표본 · 데이터

- 종목-월 {metrics['sample_n']:,}, 월 유니버스 중앙값 {metrics['universe_size_median']}, 첫 신호 {metrics['first_signal_month']}, 재무 폴백(전년 분기 없어 연간 대체) 비율 {metrics['ttm_fallback_share'] * 100:.1f}%.
- 주식수 출처: 2020~ data.go.kr 월말(A), 2015~2019 DART 사업보고서(B). 주식수 없는 달은 유니버스에서 빠진다.
- 미실행 항목: 시총가중 분위, 크기 분해, 용량(20일 거래대금 1%) — 다음 실행.

## 6. 재현

`{metrics['rerun_command']}` · 커밋 {metrics['git_commit']} · seed {SEED}(난수 없음) · 데이터 지문은 metrics.json `data_fingerprint`.
산출물: metrics.json · curves.csv · walkforward.csv · robustness.csv · quintiles.csv · holdings.csv (모두 이 폴더).
"""
    (STUDY_DIR / "README.md").write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cost", choices=tuple(RESEARCH_BY_LEVEL), default="mid")
    arguments = parser.parse_args()
    np.random.seed(SEED)
    started = time.time()

    data = fundamental.FundamentalData.load()
    calendar = month_calendar(data.bars.trading_days)
    signals, all_returns = build_signals(data, calendar)
    months = sorted(signals)
    print(f"신호 {len(months)}개월 {months[0]}~{months[-1]}, 적재 {time.time() - started:.0f}초", flush=True)

    cost = RESEARCH_BY_LEVEL[arguments.cost]
    center_curve = simulate(signals, all_returns, months, CENTER[0], CENTER[1], CENTER[2], cost)
    center_judgment = excess_summary(month_in(center_curve, JUDGMENT_START, JUDGMENT_END))
    center_full = excess_summary(center_curve)
    cost_sensitivity = {level: excess_summary(month_in(simulate(signals, all_returns, months, CENTER[0], CENTER[1], CENTER[2], cost_specification),
                                                       JUDGMENT_START, JUDGMENT_END)) for level, cost_specification in RESEARCH_BY_LEVEL.items()}
    walk_rows = walk_forward(signals, all_returns, months, cost)
    robustness_rows, neighbors_positive, neighbor_count, sharpe_ratio = robustness(signals, all_returns, months, cost)
    quintiles, diagnostics = factor_diagnostics(signals, months)

    positive_windows = sum(1 for row in walk_rows if row["sign"] > 0)
    layer1 = bool(np.isfinite(center_judgment["excess_t"]) and center_judgment["excess_t"] >= LAYER1_T and center_judgment["excess_annual"] > 0)
    layer2 = positive_windows >= LAYER2_MIN_POSITIVE
    layer3 = bool(neighbor_count > 0 and neighbors_positive == neighbor_count and np.isfinite(sharpe_ratio) and sharpe_ratio >= LAYER3_SHARPE_RATIO)
    overall = "채택 후보" if (layer1 and layer2 and layer3) else ("조건부" if layer1 and (layer2 or layer3) else "미달")
    one_line = (f"저PBR×고ROE 상위 30 동일가중은 비용 {arguments.cost.upper()} 뒤 판정 구간 초과수익 연 {center_judgment['excess_annual'] * 100:+.2f}%"
                f"(t {center_judgment['excess_t']:.2f}), walk-forward {positive_windows}/{len(walk_rows)}창 양수, 이웃 {neighbors_positive}/{neighbor_count} 양수.")

    sample_n = int(sum(len(frame) for frame in signals.values()))
    fallback_share = float(np.mean([frame["ttm_is_fallback"].eq(True).mean() for frame in signals.values()]))
    metrics = {
        "study": "19_fundamental_factors/pbr_roe_top30",
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
        "verdict": {"layer1": layer1, "layer2": layer2, "layer3": layer3, "overall": overall, "one_line": one_line},
        "center_cell": {"top_n": CENTER[0], "value_weight": CENTER[1], "rebalance_months": CENTER[2], "buffer_rank": BUFFER_MULTIPLE * CENTER[0]},
        "center_judgment": center_judgment,
        "center_full": center_full,
        "cost_sensitivity": cost_sensitivity,
        "walkforward": walk_rows,
        "robustness_neighbors_positive": neighbors_positive,
        "robustness_neighbors": neighbor_count,
        "robustness_center_sharpe_ratio": sharpe_ratio,
        "robustness": robustness_rows,
        "factor_diagnostics": diagnostics,
        "universe_size_median": int(np.median([len(frame) for frame in signals.values()])),
        "first_signal_month": months[0],
        "ttm_fallback_share": fallback_share,
        "benchmark": "같은 달 유니버스 동일가중, 비용 없음",
        "dividends": "제외(일봉에 배당 없음, 보수적)",
        "seed": SEED,
        "git_commit": git_commit(),
        "data_fingerprint": data_fingerprint(),
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "rerun_command": f"py research/studies/19_fundamental_factors/run_pbr_roe.py --cost {arguments.cost}",
        "elapsed_seconds": round(time.time() - started, 1),
    }
    (STUDY_DIR / "metrics.json").write_text(json.dumps(clean(metrics), ensure_ascii=False, indent=2), encoding="utf-8")

    center_curve.drop(columns="members").to_csv(STUDY_DIR / "curves.csv", index=False)
    pd.DataFrame(walk_rows).to_csv(STUDY_DIR / "walkforward.csv", index=False)
    pd.DataFrame(robustness_rows).to_csv(STUDY_DIR / "robustness.csv", index=False)
    quintiles.to_csv(STUDY_DIR / "quintiles.csv")
    holdings = pd.DataFrame([{"month": row.month, "ticker": ticker, "weight": 1.0 / row.holdings}
                             for row in center_curve.itertuples(index=False) for ticker in row.members.split(";")])
    holdings.to_csv(STUDY_DIR / "holdings.csv", index=False)
    write_readme(metrics, walk_rows, robustness_rows)

    print(json.dumps({key: clean(metrics[key]) for key in ("sample_n", "period", "holdout", "sharpe", "mdd", "t_stat",
                                                            "walk_forward_windows_positive", "trials_prior", "cost_level", "grade")},
                     ensure_ascii=False, indent=2))
    print(one_line, f"→ {overall}. {time.time() - started:.0f}초")
    return 0


if __name__ == "__main__":
    sys.exit(main())
