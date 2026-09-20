"""스터디 23 — 저PBR×고ROE 가치 기울임을 거래대금 하한 30억·50억 유니버스에서 7창(2019~2025)으로 다시 잰다.

스터디 22 하네스(`research/studies/22_pbr_roe_value_tilt/run_value_tilt.py`)를 모듈로 읽어 그대로 돌리되, 실행 전에 네 가지만 바꾼다:
유니버스 거래대금 하한(`PYQuant/features/fundamental.py` `MIN_TURNOVER_KRW`), walk-forward 검증 연도(2019~2025), 판정 구간 시작(2019-01),
층 ② 문턱(7창 중 6). 하한마다 `floor_30/`·`floor_50/`에 22와 같은 파일을 남기고, 이 폴더의 `metrics.json`·`README.md`에 합본을 쓴다.

실행: py research/studies/23_value_tilt_liquidity_floor/run_liquidity_floor.py --cost mid   (저장소 루트에서)
사전등록: research/studies/23_value_tilt_liquidity_floor/PREREG.md (결과 보기 전 확정)
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

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "PYQuant"))

from PYQuant.features import fundamental  # noqa: E402

STUDY_DIR = Path(__file__).resolve().parent
STUDY22_PATH = REPO_ROOT / "research" / "studies" / "22_pbr_roe_value_tilt" / "run_value_tilt.py"
STUDY22_METRICS = STUDY22_PATH.parent / "metrics.json"
SEED = 20260920
BILLION_KRW = 100_000_000
TURNOVER_FLOORS_BILLION = (30, 50)          # 주 판정 30억, 부 판정 50억
VALIDATION_YEARS = tuple(range(2019, 2026))  # 7창
JUDGMENT_START = pd.Timestamp("2019-01-01")
LAYER2_MIN_POSITIVE = 6


def load_study22():
    """스터디 22 하네스를 모듈로 읽는다(22는 안에서 19를 읽는다). 상수는 run_floor()가 실행 직전에 바꾼다."""
    specification = importlib.util.spec_from_file_location("study22", STUDY22_PATH)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def run_floor(study22, floor_billion: int, cost_level: str) -> dict:
    """하한 하나에 대해 22의 main()을 그대로 돌리고 그 폴더의 metrics.json을 돌려준다."""
    base = study22.base
    floor_directory = STUDY_DIR / f"floor_{floor_billion}"
    floor_directory.mkdir(exist_ok=True)

    # [why PREREG 23] 하한·창·판정 구간·층 ② 문턱 네 가지만 바꾼다. 19·22 모듈은 이 값을 실행 시점에 읽는다.
    fundamental.MIN_TURNOVER_KRW = floor_billion * BILLION_KRW
    base.fundamental.MIN_TURNOVER_KRW = fundamental.MIN_TURNOVER_KRW
    base.VALIDATION_YEARS = VALIDATION_YEARS
    base.JUDGMENT_START = JUDGMENT_START
    base.LAYER2_MIN_POSITIVE = LAYER2_MIN_POSITIVE
    study22.JUDGMENT_START = JUDGMENT_START
    study22.STUDY_DIR = floor_directory
    study22.write_readme = lambda *_arguments, **_keywords: None   # 합본 README는 이 스크립트가 쓴다

    saved_argv = sys.argv
    sys.argv = [str(STUDY22_PATH), "--cost", cost_level]

    try:
        print(f"\n=== 거래대금 하한 {floor_billion}억 ===", flush=True)
        study22.main()

    finally:
        sys.argv = saved_argv

    metrics = json.loads((floor_directory / "metrics.json").read_text(encoding="utf-8"))
    metrics["study"] = f"23_value_tilt_liquidity_floor/floor_{floor_billion}"
    metrics["holdout"] = f"{JUDGMENT_START:%Y-%m}~{base.JUDGMENT_END:%Y-%m} (walk-forward 검증 {len(VALIDATION_YEARS)}창 합)"
    metrics["turnover_floor_billion_krw"] = floor_billion
    metrics["layer2_min_positive"] = LAYER2_MIN_POSITIVE
    metrics["rerun_command"] = f"py research/studies/23_value_tilt_liquidity_floor/run_liquidity_floor.py --cost {cost_level}"
    (floor_directory / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
    return metrics


def layer_row(label: str, metrics: dict) -> str:
    judgment = metrics["center_judgment"]
    verdict = metrics["verdict"]
    walk = f"{metrics['walk_forward_windows_positive']}/{metrics['walk_forward_windows']}"
    neighbors = f"{metrics['robustness_neighbors_positive']}/{metrics['robustness_neighbors']}"
    capacity = metrics["capacity"]["limit_10pct"]["median_billion_krw"]
    return (f"| {label} | {judgment['excess_annual'] * 100:+.2f}% | {judgment['excess_t']:.2f} ({judgment['excess_t_newey_west']:.2f}) | "
            f"{walk} | {neighbors} · {metrics['robustness_center_sharpe_ratio']:.2f} | {metrics['cap_weighted']['excess_annual'] * 100:+.2f}% | "
            f"{capacity:.0f}억 | {metrics['universe_size_median']} | "
            f"{'통과' if verdict['layer1'] else '미달'}·{'통과' if verdict['layer2'] else '미달'}·{'통과' if verdict['layer3'] else '미달'} → **{verdict['overall']}** |")


def write_readme(combined: dict, by_floor: dict, study22_metrics: dict) -> None:
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
    text = f"""# 23. 저PBR × 고ROE 가치 기울임 — 거래대금 하한 30억·50억, 7창 (연구군: 종목레벨 · 재무 팩터)

> 한 줄 요약: {combined['verdict']['one_line']} 상태: **{combined['verdict']['overall']}**.
> 사전등록 `research/studies/23_value_tilt_liquidity_floor/PREREG.md`(결과 전 확정). 스터디 22에서 하한 10억→30억·50억, 5창→7창(2019~2025), 층 ② 3/5→6/7만 바꿨다.
> 숫자 읽는 법(t·창·문턱의 출처): [../READING_NUMBERS.md](../READING_NUMBERS.md). 30억 유니버스의 t {judgment['excess_t']:.2f} = 판정 구간 {judgment['months']}개월 월 초과수익 평균 {judgment['excess_monthly_mean'] * 100:+.3f}% ÷ 표준오차 {standard_error * 100:.3f}%(표준편차 {judgment['excess_monthly_std'] * 100:.3f}% ÷ √{judgment['months']}). 2.0은 통계학 관행(우연 5%), 7창 중 6은 동전 던지기 통과 6%로 맞춘 고른 값.
> 데이터 등급 {combined['grade']} — 22와 같다(일봉 A, 재무는 2016년 이후 끝난 회사의 81%만 붙어 B). 배당 제외(보수적).

## 1. 판정 (비용 {combined['cost_level'].upper()}, 판정 구간 {combined['holdout']})

| 유니버스 | 초과수익 연 | t (뉴이-웨스트) | 창 + | 이웃 + · 샤프 비 | 시총가중 연 | 용량 10% 중앙값 | 유니버스 중앙값 | 층 ①·②·③ |
|---|---|---|---|---|---|---|---|---|
{layer_row('하한 30억(주 판정)', by_floor[30])}
{layer_row('하한 50억(부 판정)', by_floor[50])}
| (참고) 22 하한 10억·5창·3/5 | {study22_metrics['center_judgment']['excess_annual'] * 100:+.2f}% | {study22_metrics['center_judgment']['excess_t']:.2f} ({study22_metrics['center_judgment']['excess_t_newey_west']:.2f}) | {study22_metrics['walk_forward_windows_positive']}/{study22_metrics['walk_forward_windows']} | {study22_metrics['robustness_neighbors_positive']}/{study22_metrics['robustness_neighbors']} · {study22_metrics['robustness_center_sharpe_ratio']:.2f} | {study22_metrics['cap_weighted']['excess_annual'] * 100:+.2f}% | {study22_metrics['capacity']['limit_10pct']['median_billion_krw']:.0f}억 | {study22_metrics['universe_size_median']} | {study22_metrics['verdict']['overall']}(2021~2025 기준) |

합격선: ① t ≥ 2.0 ② 7창 중 ≥ 6 ③ 이웃 6/6 > 0 이고 중심 샤프 ≥ 격자 최댓값의 70%. 채택 후보는 30억·50억 둘 다 세 층 통과일 때(PREREG).
22 행은 판정 구간(2021~2025)과 창 수가 다르니 크기 비교만 하고 층 판정은 비교하지 않는다.

## 2. walk-forward 창별 검증 초과수익 (학습창이 고른 칸: 종목 수·가치 비중·리밸 개월)

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

- 중심 칸(30·0.7·3)은 19·22 결과를 보고 고른 것이라 2021~2025는 이번에도 처음 보는 구간이 아니다. 새로 본 것은 2019·2020 두 창과 하한을 올린 유니버스다.
  그래서 통과해도 "채택 후보"이고 확정은 forward 검증에서 한다.
- 2019 창의 학습창은 2016-04~2018-12(33개월)로 다른 창보다 얇다.
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
    by_floor = {floor: run_floor(study22, floor, arguments.cost) for floor in TURNOVER_FLOORS_BILLION}
    study22_metrics = json.loads(STUDY22_METRICS.read_text(encoding="utf-8"))

    primary, secondary = by_floor[TURNOVER_FLOORS_BILLION[0]], by_floor[TURNOVER_FLOORS_BILLION[1]]
    primary_pass = primary["verdict"]["overall"] == "채택 후보"
    secondary_pass = secondary["verdict"]["overall"] == "채택 후보"
    overall = "채택 후보(forward 검증 대기)" if primary_pass and secondary_pass else ("조건부(하한 30억 전용)" if primary_pass else "미달")
    one_line = (f"하한 30억: 초과수익 연 {primary['center_judgment']['excess_annual'] * 100:+.2f}%(t {primary['center_judgment']['excess_t']:.2f}), "
                f"창 {primary['walk_forward_windows_positive']}/{primary['walk_forward_windows']}, 이웃 {primary['robustness_neighbors_positive']}/{primary['robustness_neighbors']} → {primary['verdict']['overall']}. "
                f"하한 50억: 연 {secondary['center_judgment']['excess_annual'] * 100:+.2f}%(t {secondary['center_judgment']['excess_t']:.2f}), "
                f"창 {secondary['walk_forward_windows_positive']}/{secondary['walk_forward_windows']}, 이웃 {secondary['robustness_neighbors_positive']}/{secondary['robustness_neighbors']} → {secondary['verdict']['overall']}. "
                f"용량(10%) 중앙값 {primary['capacity']['limit_10pct']['median_billion_krw']:.0f}억·{secondary['capacity']['limit_10pct']['median_billion_krw']:.0f}억.")
    combined = {
        "study": "23_value_tilt_liquidity_floor/pbr_roe_top30_w07_quarterly_floor30_50",
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
        "verdict": {"overall": overall, "one_line": one_line, "floor_30": primary["verdict"], "floor_50": secondary["verdict"]},
        "layer2_min_positive": LAYER2_MIN_POSITIVE,
        "turnover_floors_billion_krw": list(TURNOVER_FLOORS_BILLION),
        "by_floor": {str(floor): {key: metrics[key] for key in ("center_judgment", "observation_window", "walk_forward_windows_positive",
                                                                 "robustness_neighbors_positive", "robustness_center_sharpe_ratio",
                                                                 "cap_weighted", "size_buckets", "capacity", "universe_size_median", "verdict")}
                     for floor, metrics in by_floor.items()},
        "study22_reference": {key: study22_metrics[key] for key in ("t_stat", "walk_forward_windows_positive", "universe_size_median")},
        "seed": SEED,
        "git_commit": primary["git_commit"],
        "data_fingerprint": primary["data_fingerprint"],
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "rerun_command": f"py research/studies/23_value_tilt_liquidity_floor/run_liquidity_floor.py --cost {arguments.cost}",
        "elapsed_seconds": round(time.time() - started, 1),
    }
    (STUDY_DIR / "metrics.json").write_text(json.dumps(combined, ensure_ascii=False, indent=2), encoding="utf-8")
    write_readme(combined, by_floor, study22_metrics)
    print("\n" + one_line, f"→ {overall}. {time.time() - started:.0f}초")
    return 0


if __name__ == "__main__":
    sys.exit(main())
