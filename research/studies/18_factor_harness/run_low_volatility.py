"""스터디 18 — 저변동성 바스켓(실현변동성 하위 상위 30 동일가중·분기 리밸)을 거래대금 하한 30억·50억 유니버스에서 7창(2019~2025)으로 잰다.

스터디 22 하네스(`research/studies/22_pbr_roe_value_tilt/run_value_tilt.py`)를 모듈로 읽어 그대로 돌리되, 실행 전에 바꾸는 것은 셋뿐이다:
신호 함수(스터디 19의 `build_signals`를 저변동성 판으로 바꿔 끼움 — 250일·20일 실현변동성의 −z 점수), 유니버스 거래대금 하한
(`PYQuant/features/fundamental.py` `MIN_TURNOVER_KRW`), walk-forward 창·판정 구간·층 ② 문턱(23과 같은 값). 하한마다 `floor_30/`·`floor_50/`에 22와 같은
파일을 남기고, 이 폴더의 `metrics.json`·`README.md`에 합본을 쓴다.

실행: py research/studies/18_factor_harness/run_low_volatility.py --cost mid   (저장소 루트에서)
사전등록: research/studies/18_factor_harness/PREREG.md (결과 보기 전 확정)
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

STUDY_DIR = Path(__file__).resolve().parent
STUDY22_PATH = REPO_ROOT / "research" / "studies" / "22_pbr_roe_value_tilt" / "run_value_tilt.py"
STUDY23_METRICS = REPO_ROOT / "research" / "studies" / "23_value_tilt_liquidity_floor" / "metrics.json"
SEED = 20260920
BILLION_KRW = 100_000_000
TURNOVER_FLOORS_BILLION = (30, 50)          # 주 판정 30억, 부 판정 50억
VALIDATION_YEARS = tuple(range(2019, 2026))  # 7창
JUDGMENT_START = pd.Timestamp("2019-01-01")
LAYER2_MIN_POSITIVE = 6
LONG_WINDOW_DAYS, LONG_MIN_DAYS = 250, 200    # z_value 자리 — 긴 창 실현변동성
SHORT_WINDOW_DAYS, SHORT_MIN_DAYS = 20, 15    # z_quality 자리 — 짧은 창(H10 원문의 20일)


def load_study22():
    """스터디 22 하네스를 모듈로 읽는다(22는 안에서 19를 읽는다). 상수·신호 함수는 run_floor()가 실행 직전에 바꾼다."""
    specification = importlib.util.spec_from_file_location("study22", STUDY22_PATH)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class LowVolatilityPanels:
    """일봉 종가 패널에서 긴 창·짧은 창 실현변동성을 한 번 계산해 두고 기준일마다 한 줄씩 꺼낸다."""

    def __init__(self, close: pd.DataFrame):
        log_returns = np.log(close).diff()
        self.long_volatility = log_returns.rolling(LONG_WINDOW_DAYS, min_periods=LONG_MIN_DAYS).std()
        self.short_volatility = log_returns.rolling(SHORT_WINDOW_DAYS, min_periods=SHORT_MIN_DAYS).std()

    def at(self, as_of: pd.Timestamp) -> pd.DataFrame:
        return pd.DataFrame({"volatility_long": self.long_volatility.loc[as_of], "volatility_short": self.short_volatility.loc[as_of]})


def make_build_signals(base, panels: LowVolatilityPanels):
    """스터디 19 `build_signals`와 같은 반환 꼴이되 신호만 저변동성. 유니버스는 `candidates()`(재무 필터 없음)."""

    def build_signals(data: fundamental.FundamentalData, calendar: pd.DataFrame) -> tuple:
        signals = {}
        all_returns = {}

        for row in calendar.itertuples(index=False):
            frame = data.candidates(row.as_of).set_index("ticker")
            frame = frame.join(panels.at(row.as_of))
            frame = frame[(frame["volatility_long"] > 0) & (frame["volatility_short"] > 0)]
            frame = frame[np.isfinite(frame["volatility_long"]) & np.isfinite(frame["volatility_short"])]

            if len(frame) < base.MIN_UNIVERSE:
                continue

            # [formula] 변동성이 낮을수록 점수가 높도록 −z. 로그를 씌워 오른쪽 꼬리(급등락 종목)를 눌러 둔 뒤 윈저화 z.
            frame["z_value"] = -fundamental.winsorized_z(np.log(frame["volatility_long"]))
            frame["z_quality"] = -fundamental.winsorized_z(np.log(frame["volatility_short"]))
            frame["ttm_is_fallback"] = False      # 22 하네스가 재무 대체율을 세는 열 — 재무를 쓰지 않으니 0
            forward = base.month_returns(data.bars, row.entry, row.exit)
            entry_close = data.bars.close.loc[row.entry]
            frame["forward_return"] = forward.reindex(frame.index)
            frame["entry_close"] = entry_close.reindex(frame.index)
            frame = frame[frame["forward_return"].notna()]
            signals[row.month] = frame
            all_returns[row.month] = pd.DataFrame({"forward_return": forward, "entry_close": entry_close.reindex(forward.index)})

        return signals, all_returns

    return build_signals


def run_floor(study22, panels: LowVolatilityPanels, floor_billion: int, cost_level: str) -> dict:
    """하한 하나에 대해 22의 main()을 그대로 돌리고 그 폴더의 metrics.json을 돌려준다."""
    base = study22.base
    floor_directory = STUDY_DIR / f"floor_{floor_billion}"
    floor_directory.mkdir(exist_ok=True)

    # [why PREREG 18] 신호·하한·창·판정 구간·층 ② 문턱만 바꾼다. 19·22 모듈은 이 값을 실행 시점에 읽는다.
    fundamental.MIN_TURNOVER_KRW = floor_billion * BILLION_KRW
    base.fundamental.MIN_TURNOVER_KRW = fundamental.MIN_TURNOVER_KRW
    base.build_signals = make_build_signals(base, panels)
    base.VALIDATION_YEARS = VALIDATION_YEARS
    base.JUDGMENT_START = JUDGMENT_START
    base.LAYER2_MIN_POSITIVE = LAYER2_MIN_POSITIVE
    study22.JUDGMENT_START = JUDGMENT_START
    study22.STUDY_DIR = floor_directory
    study22.write_readme = lambda *_arguments, **_keywords: None   # 합본 README는 이 스크립트가 쓴다

    saved_argv = sys.argv
    sys.argv = [str(STUDY22_PATH), "--cost", cost_level]

    try:
        print(f"\n=== 저변동성 · 거래대금 하한 {floor_billion}억 ===", flush=True)
        study22.main()

    finally:
        sys.argv = saved_argv

    metrics = json.loads((floor_directory / "metrics.json").read_text(encoding="utf-8"))
    curve = pd.read_csv(floor_directory / "curves.csv")
    judgment_curve = base.month_in(curve, JUDGMENT_START, base.JUDGMENT_END)
    benchmark = stats.performance_summary(judgment_curve["benchmark"], periods_per_year=12)
    metrics["study"] = f"18_factor_harness/floor_{floor_billion}"
    metrics["holdout"] = f"{JUDGMENT_START:%Y-%m}~{base.JUDGMENT_END:%Y-%m} (walk-forward 검증 {len(VALIDATION_YEARS)}창 합)"
    metrics["turnover_floor_billion_krw"] = floor_billion
    metrics["layer2_min_positive"] = LAYER2_MIN_POSITIVE
    metrics["signal"] = {"z_value": f"-z(log σ_{LONG_WINDOW_DAYS}d)", "z_quality": f"-z(log σ_{SHORT_WINDOW_DAYS}d)"}
    metrics["benchmark_judgment"] = {"annual_return": benchmark["annual_return"], "sharpe": benchmark["sharpe"],
                                     "max_drawdown_percent": benchmark["max_drawdown_percent"]}
    metrics["rerun_command"] = f"py research/studies/18_factor_harness/run_low_volatility.py --cost {cost_level}"
    (floor_directory / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
    return metrics


def layer_row(label: str, metrics: dict) -> str:
    judgment = metrics["center_judgment"]
    verdict = metrics["verdict"]
    walk = f"{metrics['walk_forward_windows_positive']}/{metrics['walk_forward_windows']}"
    neighbors = f"{metrics['robustness_neighbors_positive']}/{metrics['robustness_neighbors']}"
    capacity = metrics["capacity"]["limit_10pct"]["median_billion_krw"]
    return (f"| {label} | {judgment['excess_annual'] * 100:+.2f}% | {judgment['excess_t']:.2f} ({judgment['excess_t_newey_west']:.2f}) | "
            f"{walk} | {neighbors} · {metrics['robustness_center_sharpe_ratio']:.2f} | "
            f"{judgment['max_drawdown_percent']:.1f}% / {metrics['benchmark_judgment']['max_drawdown_percent']:.1f}% | "
            f"{judgment['sharpe']:.2f} / {metrics['benchmark_judgment']['sharpe']:.2f} | "
            f"{capacity:.0f}억 | {metrics['universe_size_median']} | "
            f"{'통과' if verdict['layer1'] else '미달'}·{'통과' if verdict['layer2'] else '미달'}·{'통과' if verdict['layer3'] else '미달'} → **{verdict['overall']}** |")


def write_readme(combined: dict, by_floor: dict, study23_metrics: dict) -> None:
    primary = by_floor[TURNOVER_FLOORS_BILLION[0]]
    judgment = primary["center_judgment"]
    standard_error = judgment["excess_monthly_std"] / math.sqrt(judgment["months"])
    walk_years = [row["validation_year"] for row in primary["walkforward"]]
    walk_lines = []

    for year in walk_years:
        cells = []

        for floor in TURNOVER_FLOORS_BILLION:
            row = next(item for item in by_floor[floor]["walkforward"] if item["validation_year"] == year)
            cells.append(f"{row['test_excess_annual'] * 100:+.2f}% ({row['chosen_top_n']}·{row['chosen_value_weight']}·{row['chosen_rebalance_months']})")

        walk_lines.append(f"| {year} | " + " | ".join(cells) + " |")

    bucket_lines = []

    for label in ("소형", "중형", "대형"):
        cells = [f"{by_floor[floor]['size_buckets'][label]['excess_annual'] * 100:+.2f}% (t {by_floor[floor]['size_buckets'][label]['excess_t']:.2f})"
                 for floor in TURNOVER_FLOORS_BILLION]
        bucket_lines.append(f"| {label} | " + " | ".join(cells) + " |")

    observation_lines = [f"| {floor}억 | {by_floor[floor]['observation_window']['excess_annual'] * 100:+.2f}% | "
                         f"{by_floor[floor]['observation_window']['excess_t']:.2f} | {by_floor[floor]['observation_window']['months']} |"
                         for floor in TURNOVER_FLOORS_BILLION]
    diagnostics = primary["factor_diagnostics"]["judgment_period"]
    value_30 = study23_metrics["by_floor"]["30"]
    text = f"""# 18. 저변동성 바스켓 — 실현변동성 하위 상위 30 동일가중·분기 리밸, 거래대금 하한 30억·50억, 7창 (연구군: 종목레벨 · 팩터)

> 한 줄 요약: {combined['verdict']['one_line']} 상태: **{combined['verdict']['overall']}**.
> 사전등록 `research/studies/18_factor_harness/PREREG.md`(결과 전 확정). 스터디 23의 자(세 층·7창·6/7·하한 30억·50억)를 그대로 쓰고 신호만
> 저변동성(250일 −z 0.7 + 20일 −z 0.3)으로 바꿨다. 유니버스는 재무 필터 없이 가격·유동성·시총 조건만(그래서 23보다 크다).
> 숫자 읽는 법(t·창·문턱의 출처): [../READING_NUMBERS.md](../READING_NUMBERS.md). 30억 유니버스의 t {judgment['excess_t']:.2f} = 판정 구간 {judgment['months']}개월 월 초과수익 평균 {judgment['excess_monthly_mean'] * 100:+.3f}% ÷ 표준오차 {standard_error * 100:.3f}%.
> 데이터 등급 {combined['grade']} — 일봉 A(상폐 포함), 상장주식수 시점 고정 B.

## 1. 판정 (비용 {combined['cost_level'].upper()}, 판정 구간 {combined['holdout']})

| 유니버스 | 초과수익 연 | t (뉴이-웨스트) | 창 + | 이웃 + · 샤프 비 | MDD 전략 / 유니버스 | 샤프 전략 / 유니버스 | 용량 10% 중앙값 | 유니버스 중앙값 | 층 ①·②·③ |
|---|---|---|---|---|---|---|---|---|---|
{layer_row('하한 30억(주 판정)', by_floor[30])}
{layer_row('하한 50억(부 판정)', by_floor[50])}
| (참고) 23 가치 기울임 30억 | {value_30['center_judgment']['excess_annual'] * 100:+.2f}% | {value_30['center_judgment']['excess_t']:.2f} ({value_30['center_judgment']['excess_t_newey_west']:.2f}) | {value_30['walk_forward_windows_positive']}/7 | {value_30['robustness_neighbors_positive']}/6 · {value_30['robustness_center_sharpe_ratio']:.2f} | {value_30['center_judgment']['max_drawdown_percent']:.1f}% / — | {value_30['center_judgment']['sharpe']:.2f} / — | {value_30['capacity']['limit_10pct']['median_billion_krw']:.0f}억 | {value_30['universe_size_median']} | {value_30['verdict']['overall']} |

합격선: ① t ≥ 2.0 ② 7창 중 ≥ 6 ③ 이웃 6/6 > 0 이고 중심 샤프 ≥ 격자 최댓값의 70%. 채택 후보는 30억·50억 둘 다 세 층 통과일 때(PREREG).
MDD·샤프 비교 열은 판정 층이 아니라 저변동 가설(위험 조정)의 참고다. 보조 지표(30억): 순위 IC 평균 {diagnostics['ic_mean']:+.3f}(t {diagnostics['ic_t']:.2f}), ICIR {diagnostics['icir']:.2f}, Q5−Q1 연 {diagnostics['q5_q1_annual_ew'] * 100:+.2f}%(t {diagnostics['q5_q1_t_ew']:.2f}).

## 2. walk-forward 창별 검증 초과수익 (학습창이 고른 칸: 종목 수·긴 창 비중·리밸 개월)

| 검증 연도 | 하한 30억 | 하한 50억 |
|---|---|---|
{chr(10).join(walk_lines)}

## 3. 크기 3분위(분위 안 상위 10, 그 분위 동일가중 대비)

| 분위 | 하한 30억 | 하한 50억 |
|---|---|---|
{chr(10).join(bucket_lines)}

## 4. 관찰창 2026-01~08 (판정 밖)

| 유니버스 | 초과수익 연율 | t | 개월 |
|---|---|---|---|
{chr(10).join(observation_lines)}

## 5. 읽을 때 주의

- 중심 칸(30·0.7·3)과 격자는 23에서 그대로 가져온 것이라 저변동 신호에 맞춰 고른 값이 아니다. 창 길이(250·20일)도 결과를 보기 전에 정했다.
- 유니버스 동일가중이 벤치마크다. 저변동 바스켓은 상승장에서 벤치마크에 뒤지는 것이 보통이라 초과수익 t가 낮게 나오기 쉽고, 그때는 MDD·샤프 열이 가설의 본래 주장을 보여 준다.
- 하한마다 세부 표(비용 감도·격자 27칸·시총가중·용량·상폐 보유)는 `floor_30/`·`floor_50/`의 `metrics.json`과 csv에 있다.

## 6. 재현

`{combined['rerun_command']}` · 커밋 {combined['git_commit']} · seed {SEED}(난수 없음) · {combined['elapsed_seconds']:.0f}초.
"""
    (STUDY_DIR / "README.md").write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cost", default="mid")
    arguments = parser.parse_args()
    started = time.time()
    study22 = load_study22()
    panels = LowVolatilityPanels(fundamental.BarsPanel.load().close)
    print(f"변동성 패널 {time.time() - started:.0f}초", flush=True)
    by_floor = {floor: run_floor(study22, panels, floor, arguments.cost) for floor in TURNOVER_FLOORS_BILLION}
    study23_metrics = json.loads(STUDY23_METRICS.read_text(encoding="utf-8"))

    primary, secondary = by_floor[TURNOVER_FLOORS_BILLION[0]], by_floor[TURNOVER_FLOORS_BILLION[1]]
    primary_pass = primary["verdict"]["overall"] == "채택 후보"
    secondary_pass = secondary["verdict"]["overall"] == "채택 후보"
    overall = "채택 후보(forward 검증 대기)" if primary_pass and secondary_pass else ("조건부(하한 30억 전용)" if primary_pass else "미달")
    one_line = (f"하한 30억: 초과수익 연 {primary['center_judgment']['excess_annual'] * 100:+.2f}%(t {primary['center_judgment']['excess_t']:.2f}), "
                f"창 {primary['walk_forward_windows_positive']}/{primary['walk_forward_windows']}, 이웃 {primary['robustness_neighbors_positive']}/{primary['robustness_neighbors']}, "
                f"MDD {primary['center_judgment']['max_drawdown_percent']:.1f}%(유니버스 {primary['benchmark_judgment']['max_drawdown_percent']:.1f}%) → {primary['verdict']['overall']}. "
                f"하한 50억: 연 {secondary['center_judgment']['excess_annual'] * 100:+.2f}%(t {secondary['center_judgment']['excess_t']:.2f}), "
                f"창 {secondary['walk_forward_windows_positive']}/{secondary['walk_forward_windows']}, 이웃 {secondary['robustness_neighbors_positive']}/{secondary['robustness_neighbors']}, "
                f"MDD {secondary['center_judgment']['max_drawdown_percent']:.1f}%(유니버스 {secondary['benchmark_judgment']['max_drawdown_percent']:.1f}%) → {secondary['verdict']['overall']}.")
    combined = {
        "study": "18_factor_harness/low_volatility_top30_w07_quarterly_floor30_50",
        "sample_n": primary["sample_n"],
        "period": primary["period"],
        "holdout": primary["holdout"],
        "sharpe": primary["sharpe"],
        "mdd": primary["mdd"],
        "t_stat": primary["t_stat"],
        "walk_forward_windows_positive": primary["walk_forward_windows_positive"],
        "walk_forward_windows": primary["walk_forward_windows"],
        "trials_prior": primary["trials_prior"],
        "cost_level": arguments.cost,
        "cost_spec": primary["cost_spec"],
        "grade": "B",
        "signal": primary["signal"],
        "verdict": {"overall": overall, "one_line": one_line, "floor_30": primary["verdict"], "floor_50": secondary["verdict"]},
        "layer2_min_positive": LAYER2_MIN_POSITIVE,
        "turnover_floors_billion_krw": list(TURNOVER_FLOORS_BILLION),
        "by_floor": {str(floor): {key: metrics[key] for key in ("center_judgment", "benchmark_judgment", "observation_window",
                                                                 "walk_forward_windows_positive", "robustness_neighbors_positive",
                                                                 "robustness_center_sharpe_ratio", "cap_weighted", "size_buckets", "capacity",
                                                                 "universe_size_median", "factor_diagnostics", "verdict")}
                     for floor, metrics in by_floor.items()},
        "seed": SEED,
        "git_commit": primary["git_commit"],
        "data_fingerprint": primary["data_fingerprint"],
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "rerun_command": f"py research/studies/18_factor_harness/run_low_volatility.py --cost {arguments.cost}",
        "elapsed_seconds": round(time.time() - started, 1),
    }
    (STUDY_DIR / "metrics.json").write_text(json.dumps(combined, ensure_ascii=False, indent=2), encoding="utf-8")
    write_readme(combined, by_floor, study23_metrics)
    print("\n" + one_line, f"→ {overall}. {time.time() - started:.0f}초")
    return 0


if __name__ == "__main__":
    sys.exit(main())
