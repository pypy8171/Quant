"""스터디 35 — "신호일 코스닥 종가 < 120일 평균이면 신규 매수 안 함" 거름을 믿을 수 있는지 네 가지로 확인한다(사용자 2026-10-06).

원칙: 그날 장 마감까지 알 수 있는 값만 쓰고, 규칙 고르기도 그 시점까지의 자료로만 한다(사후에 나쁜 해를 고르지 않는다).
대상  : halt_trigger.py 와 같은 실매매 칸 263건(load_trades·attach_index 를 그대로 불러 쓴다). 기간 자료 전체(매수일 2009–2026).
신호일: 매수일 직전 지수 거래일 Dk. 지수 조건은 Dk 종가까지만 본다. 평균을 낼 만큼 자료가 없으면(2009 초) 거르지 않는다.
돈    : 종목당 500만, 자본 2,000만 환산, 낙폭 = 매수일 순서로 손익을 쌓은 곡선의 최대 낙폭(halt_trigger 와 같다).
V1 걸어가며 고르기 / V2 표본 넓히기 / V3 이평선 길이 훑기 / V4 연도 묶음 재표집 — 자세한 목록과 통과 기준은 PLAN.
산출  : halt_validate.txt(요약), halt_validate_walk.parquet(V1 해마다 고른 조건), halt_validate_sweep.parquet(V3 길이별),
        halt_validate_wide.parquet(V2 넓힌 표본 한 줄 = 사건 하나, 신호일 지표 포함).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/halt_validate.py
"""
from __future__ import annotations

import importlib.util
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
EVENTS_PATH = HERE / "close_depth_events.parquet"
INDEX_PATH = HERE / "index_snapshot.parquet"
BARS_PATH = REPOSITORY / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
TEXT_PATH = HERE / "halt_validate.txt"
WALK_PATH = HERE / "halt_validate_walk.parquet"
SWEEP_PATH = HERE / "halt_validate_sweep.parquet"
WIDE_PATH = HERE / "halt_validate_wide.parquet"
SEED = 20261006
BOOTSTRAP_DRAWS = 1000
CAPITAL_WON = 2000.0          # 만 원
HIGH_WINDOW = 250             # 지수 고점 창(halt_trigger 와 같다)
MINIMUM_TRAINING_YEARS = 3    # 걸어가며 고르기 시작 전 쌓을 해 수(사용자)
DRAWDOWN_FLOOR = 100.0        # 목표 '총 손익 / 낙폭'에서 낙폭이 이보다 작으면 이 값으로 나눈다(만 원, 내가 정함)
AVERAGE_LENGTHS = (20, 40, 60, 90, 120, 150, 200)
DEPTH_LEVELS = (10, 15, 20)
SWEEP_LENGTHS = tuple(range(10, 251, 10))
GENERIC_HOLDS = tuple(range(1, 21))
EXIT_RULES = ("low3_t15", "low3_t20", "low3_t25", "low3_t30", "close3_t15", "entry8_t15", "entry10_t15")

PLAN = """시험 목록과 통과 기준(돌리기 전에 고정, 2026-10-06)
대상 거름: "신호일(매수 전날) 코스닥 종가 < 120일 평균이면 신규 매수 안 함". 기준 263건: 거름 없음 총 +4,095만·낙폭 −803만, 거름 뒤 +3,247만·−416만.
고정: 실매매 칸(저가 되돌림 < 15%, 대기 4일, 상승폭 ≥ 30%, 다음 날 시가 매수, 손절 관찰 최저 저가 −3%, 익절 +15%, 120거래일),
      비용 포함 수익 ret_low3_t15, 종목당 500만·자본 2,000만, 낙폭은 매수일 순서 곡선, 기간 자료 전체(매수일 2009-06 – 2026-03).
      지수 평균을 낼 만큼 자료가 없는 날(2009 초, 긴 평균)은 거르지 않는다(내가 정함).

V1 걸어가며 고르기: 해 Y 마다 'Y-01-01 전에 청산까지 끝난 매매'만 보고 후보 21개 중 목표 최고를 골라 그 해 매수에 적용.
    후보: 거름 없음 / 코스닥·코스피 × 종가 < 20·40·60·90·120·150·200일 평균 / 코스닥·코스피 250일 고점 대비 −10·−15·−20% 이하.
    목표 = 학습 구간 총 손익 / max(|낙폭|, 100만)(내가 정함 — 손익을 지키면서 낙폭을 줄이는 쪽을 고르려는 뜻, 낙폭이 아주 작을 때
    나눗셈이 터지지 않게 바닥 100만). 같은 값이면 목록 앞쪽(거름 없음이 맨 앞). 자료 3년(2009–2011) 쌓인 뒤 2012 부터, 그 전은 거름 없음.
    참고로 목표를 '총 손익 − |낙폭|'로 바꾼 결과도 적는다(판정에는 안 쓴다).
V2 표본 넓히기: close_depth_events 중 다음 날 시가 매수, 저가 되돌림 < 25%, 상승폭 ≥ 20%(자료 최소 21%), 대기 3–5일(자료에 있는 전부;
    요청한 2·6일은 사건 파일에 없다), 같은 종목·같은 매수일은 하나로(대기일 짧은 것). 청산 ret_low3_t15 가 본 시험, 다른 청산 6개는 참고.
    코스닥 종가 < N일 평균(N 20·40·60·90·120·150·200)인 쪽 − 아닌 쪽 평균 차이, 표준오차는 매수 월 묶음(클러스터).
    엄격판: 한 급등 사건(종목·급등일)에 한 건만. 되돌림 칸(<10·<15·<20·<25)별 120일도 적는다.
    시장 전체 효과 대조: 같은 기간 전 종목(코스피·코스닥, 상폐 포함) 매일 '다음 날 시가 매수 → H일째 종가' 평균(H 1–20)으로 같은 차이를 낸다.
    사건 수익 − 같은 날 전 종목 평균(H = 사건 보유일, 20 초과는 20)으로 초과 수익 차이도 낸다 — 이 차이가 0 근처면 '전략 고유 아님, 시장 효과'.
V3 이평선 길이 훑기: 코스닥 10–250일 10일 간격 25칸, 263건 기준 남긴 n·총 손익·낙폭·낙폭 개선·걸러낸 쪽 평균·남긴 쪽 평균.
    인접 길이 사이 부호 바뀜(낙폭 개선 > 0, 걸러낸 평균 < 남긴 평균) 횟수.
V4 연도 묶음 재표집: 매수 연도 18개를 복원 추출로 18개 뽑아 뽑힌 순서로 이어붙이기 1,000번(seed 20261006). 120일 거름의 낙폭 개선
    (거름 낙폭 − 거름 없음 낙폭, 양수가 개선)이 0 이하일 확률. 총 손익 감소가 20% 넘는 확률도 같이.

통과 기준(내가 정함):
  V1 이어붙인 결과가 거름 없음 대비 낙폭 30% 이상 감소, 총 손익 감소 20% 이내.
  V2 본 시험(ret_low3_t15, 중복 제거)에서 아래쪽 평균이 낮고 클러스터 t ≤ −3, 120일 포함 7개 길이 중 4개 이상.
  V3 60–200일 15칸 모두 낙폭 개선 > 0.
  V4 개선 ≤ 0 확률 < 5%.
  넷 다 통과 = "모의 적용 근거 충분", 일부 = 빠진 것을 적는다, 다 실패 = "거름 효과 없음".
"""


def load_module(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


HALT = load_module("halt_trigger_module", HERE / "halt_trigger.py")


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=HERE, capture_output=True, text=True).stdout.strip()
    except OSError:
        return "unknown"


def money(value: float) -> str:
    return f"{value:+,.0f}만"


# ---------------------------------------------------------------- 지수 지표(신호일 종가까지)


def index_features() -> pd.DataFrame:
    """날짜별 '종가 < N일 평균'(1·0, 평균 없으면 NaN)과 250일 고점 대비 %. 모두 그날 종가로 끝나는 창이다."""
    index = pd.read_parquet(INDEX_PATH).sort_index()
    features = pd.DataFrame(index=index.index)

    for symbol, label in (("KQ11", "kosdaq"), ("KS11", "kospi")):
        close = index[symbol]

        for days in sorted(set(AVERAGE_LENGTHS) | set(SWEEP_LENGTHS)):
            average = close.rolling(days).mean()
            features[f"{label}_below{days}"] = (close < average).astype(float).where(average.notna())

        features[f"{label}_drawdown"] = (close / close.rolling(HIGH_WINDOW, min_periods=20).max() - 1.0) * 100.0

    return features


def signal_dates(entry_dates: np.ndarray, calendar: np.ndarray) -> np.ndarray:
    """매수일보다 앞선 마지막 지수 거래일 위치."""
    return np.searchsorted(calendar, entry_dates, side="left") - 1


def attach_features(frame: pd.DataFrame, features: pd.DataFrame) -> pd.DataFrame:
    calendar = features.index.to_numpy()
    positions = signal_dates(frame["entry_date"].to_numpy(), calendar)
    valid = positions >= 0
    frame = frame.copy()
    frame["signal_date"] = np.where(valid, calendar[np.clip(positions, 0, None)], 0)

    for column in features.columns:
        values = features[column].to_numpy()[np.clip(positions, 0, None)]
        frame[column] = np.where(valid, values, np.nan)

    return frame


# ---------------------------------------------------------------- 공통 계산


def max_drawdown(profits: np.ndarray) -> float:
    return HALT.max_drawdown(np.asarray(profits, dtype=float))


def keep_mask(frame: pd.DataFrame, condition: str) -> np.ndarray:
    """condition: 'none' · 'kosdaq_below120' · 'kospi_drawdown15' 꼴. NaN(지표 없음)은 남긴다."""
    if condition == "none":
        return np.ones(len(frame), bool)

    if "_drawdown" in condition:
        label, depth = condition.split("_drawdown")
        values = frame[f"{label}_drawdown"].to_numpy(float)
        return ~(values <= -float(depth))

    return ~(frame[condition].to_numpy(float) == 1.0)


def condition_label(condition: str) -> str:
    if condition == "none":
        return "거름 없음"

    korean = "코스닥" if condition.startswith("kosdaq") else "코스피"

    if "_drawdown" in condition:
        return f"{korean} 고점 −{condition.split('_drawdown')[1]}%"

    return f"{korean} {condition.split('_below')[1]}일선"


def candidate_conditions() -> list[str]:
    conditions = ["none"]

    for label in ("kosdaq", "kospi"):
        conditions += [f"{label}_below{days}" for days in AVERAGE_LENGTHS]

    for label in ("kosdaq", "kospi"):
        conditions += [f"{label}_drawdown{depth}" for depth in DEPTH_LEVELS]

    return conditions


def summary(frame: pd.DataFrame, keep: np.ndarray) -> dict:
    kept = frame[keep]
    dropped = frame[~keep]
    return {"n": int(keep.sum()), "total": float(kept["profit"].sum()), "drawdown": max_drawdown(kept["profit"].to_numpy()),
            "kept_mean": float(kept["ret"].mean()) if len(kept) else float("nan"),
            "dropped_n": int((~keep).sum()), "dropped_mean": float(dropped["ret"].mean()) if len(dropped) else float("nan")}


def cluster_difference(values: np.ndarray, below: np.ndarray, clusters: np.ndarray) -> dict:
    """values = 상수 + 차이 × below 회귀. 차이 = 아래쪽 평균 − 위쪽 평균, 표준오차는 묶음(클러스터) 강건 CR1."""
    values = np.asarray(values, float)
    below = np.asarray(below, float)
    design = np.column_stack([np.ones(len(values)), below])
    inverse = np.linalg.inv(design.T @ design)
    coefficients = inverse @ design.T @ values
    residuals = values - design @ coefficients
    scores = pd.DataFrame(design * residuals[:, None]).groupby(clusters).sum().to_numpy()
    groups = len(scores)
    count = len(values)
    middle = scores.T @ scores
    correction = groups / (groups - 1) * (count - 1) / (count - 2)
    covariance = correction * inverse @ middle @ inverse
    error = math.sqrt(covariance[1, 1])
    return {"difference": float(coefficients[1]), "error": error, "t": float(coefficients[1] / error) if error > 0 else float("nan"),
            "clusters": groups, "below_n": int(below.sum()), "above_n": int(count - below.sum()),
            "below_mean": float(values[below == 1].mean()), "above_mean": float(values[below == 0].mean()),
            "plain_t": HALT.welch_t(values[below == 1], values[below == 0])}


# ---------------------------------------------------------------- V1 걸어가며 고르기


def objective_ratio(total: float, drawdown: float) -> float:
    return total / max(abs(drawdown), DRAWDOWN_FLOOR)


def objective_difference(total: float, drawdown: float) -> float:
    return total - abs(drawdown)


def walk_forward(trades: pd.DataFrame, objective) -> tuple[np.ndarray, list[dict]]:
    conditions = candidate_conditions()
    masks = {condition: keep_mask(trades, condition) for condition in conditions}
    years = sorted(trades["entry_year"].unique())
    first_test_year = years[0] + MINIMUM_TRAINING_YEARS
    keep = np.ones(len(trades), bool)
    choices = []

    for year in years:
        in_year = (trades["entry_year"] == year).to_numpy()

        if year < first_test_year:
            choices.append({"year": year, "chosen": "none", "label": "거름 없음(자료 쌓는 중)", "training_n": 0, "objective": float("nan"),
                            "runner_up": "", "year_n": int(in_year.sum())})
            continue

        training = (trades["exit_date"] < year * 10000 + 101).to_numpy()
        scored = []

        for order, condition in enumerate(conditions):
            subset = training & masks[condition]
            profits = trades.loc[subset, "profit"].to_numpy()
            scored.append((objective(float(profits.sum()), max_drawdown(profits)), -order, condition))

        scored.sort(reverse=True)
        best, runner = scored[0], scored[1]
        keep[in_year] = masks[best[2]][in_year]
        choices.append({"year": year, "chosen": best[2], "label": condition_label(best[2]), "training_n": int(training.sum()),
                        "objective": best[0], "runner_up": f"{condition_label(runner[2])}({runner[0]:.2f})",
                        "none_objective": next(score for score, _, condition in scored if condition == "none"),
                        "year_n": int(in_year.sum()), "year_kept": int(masks[best[2]][in_year].sum())})

    return keep, choices


def yearly_percent(trades: pd.DataFrame, keep: np.ndarray, years: list[int]) -> pd.Series:
    return trades[keep].groupby("entry_year")["profit"].sum().reindex(years, fill_value=0.0) / CAPITAL_WON * 100.0


# ---------------------------------------------------------------- V2 넓힌 표본


def wide_events(features: pd.DataFrame, strict: bool) -> pd.DataFrame:
    events = pd.read_parquet(EVENTS_PATH)
    events = events[(events["entry"] == "open") & (events["depth_low"] < 25) & (events["move_pct"] >= 20)]
    events = events.sort_values(["code", "entry_date", "wait"], kind="mergesort")
    events = events.drop_duplicates(["code", "entry_date"], keep="first")

    if strict:
        events = events.sort_values(["code", "surge", "wait"], kind="mergesort").drop_duplicates(["code", "surge"], keep="first")

    events = events.sort_values(["entry_date", "code"], kind="mergesort").reset_index(drop=True)
    events["month"] = events["entry_date"] // 100
    return attach_features(events, features)


def generic_daily_returns() -> pd.DataFrame:
    """전 종목 매일: 그날 신호 → 다음 날 시가 매수 → 매수일 포함 H 거래일째 종가 매도 수익(%) 평균. 행 = 신호일, 열 = H."""
    bars = pd.read_parquet(BARS_PATH, columns=["Date", "code", "Open", "Close", "Volume"])
    bars["code"] = bars["code"].astype("category")
    bars = bars.sort_values(["code", "Date"], kind="mergesort").reset_index(drop=True)
    grouped = bars.groupby("code", observed=True, sort=False)
    next_open = grouped["Open"].shift(-1).to_numpy(float)
    next_volume = grouped["Volume"].shift(-1).to_numpy(float)
    valid = (next_open > 0) & (next_volume > 0)
    signal_day = bars["Date"].dt.strftime("%Y%m%d").astype(np.int64).to_numpy()
    columns = {}

    for hold in GENERIC_HOLDS:
        future_close = grouped["Close"].shift(-hold).to_numpy(float)
        returns = np.where(valid & (future_close > 0), (future_close / np.where(valid, next_open, 1.0) - 1.0) * 100.0, np.nan)
        # 자료 오류(권리락 미수정 등)가 평균을 끌지 않게 날마다가 아니라 전체에서 ±60% 밖은 버린다(내가 정함)
        returns = np.where(np.abs(returns) <= 60.0, returns, np.nan)
        columns[hold] = pd.Series(returns).groupby(signal_day).mean()

    return pd.DataFrame(columns)


# ---------------------------------------------------------------- V4 연도 묶음 재표집


def block_bootstrap(trades: pd.DataFrame, keep: np.ndarray) -> dict:
    generator = np.random.default_rng(SEED)
    years = sorted(trades["entry_year"].unique())
    blocks = {year: ((trades["entry_year"] == year).to_numpy()) for year in years}
    profits = trades["profit"].to_numpy()
    improvements, total_ratios = [], []

    for _ in range(BOOTSTRAP_DRAWS):
        drawn = generator.choice(years, size=len(years), replace=True)
        base_profits = np.concatenate([profits[blocks[year]] for year in drawn])
        filtered_profits = np.concatenate([profits[blocks[year] & keep] for year in drawn])
        improvements.append(max_drawdown(filtered_profits) - max_drawdown(base_profits))
        base_total = base_profits.sum()
        total_ratios.append(filtered_profits.sum() / base_total if base_total > 0 else float("nan"))

    improvements = np.array(improvements)
    total_ratios = np.array(total_ratios)
    return {"improvements": improvements, "probability_not_better": float((improvements <= 0).mean()),
            "quantiles": np.quantile(improvements, [0.05, 0.25, 0.5, 0.75, 0.95]),
            "probability_total_cut": float(np.nanmean(total_ratios < 0.8)), "base_negative": int(np.isnan(total_ratios).sum())}


# ---------------------------------------------------------------- 본문


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    TEXT_PATH.write_bytes((PLAN + "\n(실행 중)\n").encode("utf-8"))
    features = index_features()
    trades = HALT.load_trades()
    trades = attach_features(trades, features)
    years = sorted(trades["entry_year"].unique())
    all_keep = np.ones(len(trades), bool)
    base = summary(trades, all_keep)
    fixed_keep = keep_mask(trades, "kosdaq_below120")
    fixed = summary(trades, fixed_keep)
    lines = [PLAN, f"halt_validate — 커밋 {git_commit()}, 매매 {len(trades)}건, 매수일 {trades['entry_date'].min()}–{trades['entry_date'].max()}, "
             f"지수 {features.index.min()}–{features.index.max()}, seed {SEED}", ""]
    lines.append(f"재현 점검: 거름 없음 총 {money(base['total'])}·낙폭 {money(base['drawdown'])} (기대 +4,095만·−803만), "
                 f"120일 거름 총 {money(fixed['total'])}·낙폭 {money(fixed['drawdown'])} n{fixed['n']} (기대 +3,247만·−416만·175)")
    verdicts = {}

    # V1
    lines += ["", "V1 걸어가며 고르기 — 목표 총 손익 / max(|낙폭|, 100만), 학습 = 그 해 1월 1일 전에 청산 끝난 매매"]
    walk_keep, choices = walk_forward(trades, objective_ratio)
    walk = summary(trades, walk_keep)
    lines.append("  해   학습n  고른 조건          목표   (거름 없음 목표)  2위                     그해 매수 n → 남김")

    for choice in choices:
        if choice["training_n"] == 0:
            lines.append(f"  {choice['year']}  {choice['training_n']:4d}  {choice['label']}  (그해 매수 {choice['year_n']})")
        else:
            lines.append(f"  {choice['year']}  {choice['training_n']:4d}  {choice['label']:14s} {choice['objective']:6.2f} ({choice['none_objective']:6.2f})  "
                         f"{choice['runner_up']:22s} {choice['year_n']:3d} → {choice['year_kept']:3d}")

    base_years = yearly_percent(trades, all_keep, years)
    walk_years = yearly_percent(trades, walk_keep, years)
    fixed_years = yearly_percent(trades, fixed_keep, years)
    lines.append("  연도별(2,000만 대비 %)  거름 없음 / 걸어가며 고르기 / 120일 고정(사후 선택이라 참고)")

    for year in years:
        lines.append(f"    {year}  {base_years[year]:+6.1f}  {walk_years[year]:+6.1f}  {fixed_years[year]:+6.1f}")

    drawdown_cut = 1.0 - walk["drawdown"] / base["drawdown"]
    total_cut = 1.0 - walk["total"] / base["total"]
    test_rows = (trades["entry_year"] >= years[0] + MINIMUM_TRAINING_YEARS).to_numpy()
    base_test = summary(trades[test_rows], all_keep[test_rows])
    walk_test = summary(trades[test_rows], walk_keep[test_rows])
    lines.append(f"  이어붙인 결과: n{walk['n']} 총 {money(walk['total'])} 낙폭 {money(walk['drawdown'])} | 거름 없음 총 {money(base['total'])} 낙폭 {money(base['drawdown'])}"
                 f" → 낙폭 {drawdown_cut * 100:+.0f}% 감소, 총 손익 {total_cut * 100:+.0f}% 감소 | 걸러낸 n{walk['dropped_n']} 평균 {walk['dropped_mean']:+.2f}% vs 남긴 {walk['kept_mean']:+.2f}%")
    lines.append(f"  시험 해(2012–)만: 걸어가며 총 {money(walk_test['total'])} 낙폭 {money(walk_test['drawdown'])} vs 거름 없음 총 {money(base_test['total'])} 낙폭 {money(base_test['drawdown'])}")
    verdicts["V1"] = drawdown_cut >= 0.30 and total_cut <= 0.20
    lines.append(f"  판정 V1: {'통과' if verdicts['V1'] else '실패'} (낙폭 30%+ 감소 {'O' if drawdown_cut >= 0.30 else 'x'}, 손익 감소 20% 이내 {'O' if total_cut <= 0.20 else 'x'})")
    alternative_keep, alternative_choices = walk_forward(trades, objective_difference)
    alternative = summary(trades, alternative_keep)
    picked = ", ".join(f"{choice['year'] % 100:02d}:{choice['label']}" for choice in alternative_choices if choice["training_n"])
    lines.append(f"  참고 — 목표를 '총 손익 − |낙폭|'로: 총 {money(alternative['total'])} 낙폭 {money(alternative['drawdown'])} | 고른 것 {picked}")
    pd.DataFrame(choices).to_parquet(WALK_PATH, index=False)

    # V2
    lines += ["", "V2 표본 넓히기 — 다음 날 시가 매수, 되돌림 < 25%, 상승폭 ≥ 20%, 대기 3–5, 같은 종목·매수일 하나로. 차이 = 코스닥 N일선 아래 − 위, t 는 매수 월 묶음"]
    wide = wide_events(features, strict=False)
    strict = wide_events(features, strict=True)
    wide.to_parquet(WIDE_PATH, index=False)
    lines.append(f"  사건 {len(wide):,}건(엄격판 급등 사건당 1건 {len(strict):,}건), 매수 월 {wide['month'].nunique()}개, 매수일 {wide['entry_date'].min()}–{wide['entry_date'].max()}")
    lines.append("  길이   아래 n  아래 평균  위 n  위 평균   차이    묶음 t  (단순 t) | 엄격판 차이 묶음 t")
    passing_lengths = 0
    main_results = {}

    for days in AVERAGE_LENGTHS:
        column = f"kosdaq_below{days}"
        usable = wide[wide[column].notna()]
        result = cluster_difference(usable["ret_low3_t15"].to_numpy(), usable[column].to_numpy(), usable["month"].to_numpy())
        strict_usable = strict[strict[column].notna()]
        strict_result = cluster_difference(strict_usable["ret_low3_t15"].to_numpy(), strict_usable[column].to_numpy(), strict_usable["month"].to_numpy())
        main_results[days] = result
        passed = result["difference"] < 0 and result["t"] <= -3.0
        passing_lengths += passed
        lines.append(f"  {days:3d}일 {result['below_n']:6d} {result['below_mean']:+8.2f}% {result['above_n']:6d} {result['above_mean']:+7.2f}% "
                     f"{result['difference']:+6.2f}%p {result['t']:+6.2f} ({result['plain_t']:+6.2f}) | {strict_result['difference']:+6.2f}%p {strict_result['t']:+6.2f}"
                     f"{'  ← 기준 충족' if passed else ''}")

    verdicts["V2"] = passing_lengths >= 4 and main_results[120]["t"] <= -3.0 and main_results[120]["difference"] < 0
    lines.append(f"  판정 V2: {'통과' if verdicts['V2'] else '실패'} (t ≤ −3 길이 {passing_lengths}/7, 120일 t {main_results[120]['t']:+.2f})")
    lines.append("  참고 — 120일, 되돌림 칸별(ret_low3_t15)")

    for depth in (10, 15, 20, 25):
        part = wide[(wide["depth_low"] < depth) & wide["kosdaq_below120"].notna()]
        result = cluster_difference(part["ret_low3_t15"].to_numpy(), part["kosdaq_below120"].to_numpy(), part["month"].to_numpy())
        lines.append(f"    되돌림 < {depth}%: n{len(part):5d} 차이 {result['difference']:+6.2f}%p 묶음 t {result['t']:+6.2f}")

    lines.append("  참고 — 120일, 다른 청산 규칙")

    for rule in EXIT_RULES:
        part = wide[wide["kosdaq_below120"].notna()]
        result = cluster_difference(part[f"ret_{rule}"].to_numpy(), part["kosdaq_below120"].to_numpy(), part["month"].to_numpy())
        lines.append(f"    {rule:12s} 아래 {result['below_mean']:+6.2f}% 위 {result['above_mean']:+6.2f}% 차이 {result['difference']:+6.2f}%p 묶음 t {result['t']:+6.2f}")

    lines.append("  시장 전체 대조 — 전 종목 매일 평균(다음 날 시가 → H일째 종가), 표본 = 날, 묶음 = 월")
    generic = generic_daily_returns()
    # 일반 표본은 신호일 자체가 행이라 신호일 지표 = 그날 지표(매수일 직전 거래일 규칙이 아니다)
    day_features = features.reindex(generic.index)

    for hold in (1, 3, 5, 10, 20):
        column = "kosdaq_below120"
        usable = day_features[column].notna() & generic[hold].notna()
        values = generic.loc[usable, hold].to_numpy()
        below = day_features.loc[usable, column].to_numpy()
        months = (generic.index[usable].to_numpy() // 100)
        result = cluster_difference(values, below, months)
        lines.append(f"    H{hold:2d}: 날 {len(values):,} 아래 {result['below_mean']:+6.2f}% 위 {result['above_mean']:+6.2f}% 차이 {result['difference']:+6.2f}%p 묶음 t {result['t']:+6.2f}")

    holds = np.clip(wide["hold_low3_t15"].to_numpy(), 1, max(GENERIC_HOLDS))
    market_part = np.array([generic[hold].get(signal, np.nan) for hold, signal in zip(holds, wide["signal_date"].to_numpy())])
    wide["generic_same_hold"] = market_part
    wide["excess"] = wide["ret_low3_t15"] - market_part
    usable = wide[wide["excess"].notna() & wide["kosdaq_below120"].notna()]
    excess_result = cluster_difference(usable["excess"].to_numpy(), usable["kosdaq_below120"].to_numpy(), usable["month"].to_numpy())
    market_result = cluster_difference(usable["generic_same_hold"].to_numpy(), usable["kosdaq_below120"].to_numpy(), usable["month"].to_numpy())
    lines.append(f"    사건과 같은 날·같은 보유일의 전 종목 평균: 아래 {market_result['below_mean']:+.2f}% 위 {market_result['above_mean']:+.2f}% 차이 {market_result['difference']:+.2f}%p 묶음 t {market_result['t']:+.2f}")
    lines.append(f"    사건 초과 수익(사건 − 전 종목): 아래 {excess_result['below_mean']:+.2f}% 위 {excess_result['above_mean']:+.2f}% 차이 {excess_result['difference']:+.2f}%p 묶음 t {excess_result['t']:+.2f} (n{len(usable):,})")
    wide.to_parquet(WIDE_PATH, index=False)

    # V3
    lines += ["", "V3 코스닥 이평선 길이 훑기(263건) — 낙폭 개선 = 거름 낙폭 − 거름 없음 낙폭(양수가 개선)"]
    lines.append("  길이  남긴n  총 손익     낙폭     개선    걸러낸n 걸러낸 평균 남긴 평균")
    sweep_rows = []

    for days in SWEEP_LENGTHS:
        keep = keep_mask(trades, f"kosdaq_below{days}")
        row = summary(trades, keep)
        row.update({"days": days, "improvement": row["drawdown"] - base["drawdown"], "missing": int(trades[f"kosdaq_below{days}"].isna().sum())})
        sweep_rows.append(row)
        lines.append(f"  {days:3d}  {row['n']:4d}  {money(row['total']):>9s} {money(row['drawdown']):>8s} {money(row['improvement']):>7s}  {row['dropped_n']:4d}  "
                     f"{row['dropped_mean']:+7.2f}%  {row['kept_mean']:+7.2f}%{'  (평균 없는 매매 ' + str(row['missing']) + '건은 남김)' if row['missing'] else ''}")

    sweep = pd.DataFrame(sweep_rows)
    sweep.to_parquet(SWEEP_PATH, index=False)
    improved = (sweep["improvement"] > 0).to_numpy()
    ordered = (sweep["dropped_mean"] < sweep["kept_mean"]).to_numpy()
    band = sweep[(sweep["days"] >= 60) & (sweep["days"] <= 200)]
    flips_improvement = int((improved[1:] != improved[:-1]).sum())
    flips_order = int((ordered[1:] != ordered[:-1]).sum())
    thirty = int((band["drawdown"] >= 0.7 * base["drawdown"]).sum())
    verdicts["V3"] = bool((band["improvement"] > 0).all())
    lines.append(f"  인접 길이 부호 바뀜: 낙폭 개선 {flips_improvement}회, 걸러낸 평균 < 남긴 평균 {flips_order}회 (24쌍 중)")
    lines.append(f"  60–200일 15칸: 낙폭 개선 {int((band['improvement'] > 0).sum())}/15, 낙폭 30%+ 감소 {thirty}/15, 걸러낸 평균 < 남긴 평균 {int((band['dropped_mean'] < band['kept_mean']).sum())}/15, "
                 f"총 손익 감소 20% 이내 {int((band['total'] >= 0.8 * base['total']).sum())}/15")
    lines.append(f"  판정 V3: {'통과' if verdicts['V3'] else '실패'}")

    # V4
    bootstrap = block_bootstrap(trades, fixed_keep)
    quantiles = bootstrap["quantiles"]
    verdicts["V4"] = bootstrap["probability_not_better"] < 0.05
    lines += ["", f"V4 연도 묶음 재표집 {BOOTSTRAP_DRAWS}번(seed {SEED}) — 120일 거름의 낙폭 개선"]
    lines.append(f"  개선 분위 5%·25%·50%·75%·95%: " + " ".join(money(value) for value in quantiles))
    lines.append(f"  개선 ≤ 0 확률 {bootstrap['probability_not_better'] * 100:.1f}%, 총 손익 20% 넘게 감소 확률 {bootstrap['probability_total_cut'] * 100:.1f}%"
                 f"(거름 없음 총 손익이 0 이하인 뽑기 {bootstrap['base_negative']}번은 뺌)")
    lines.append(f"  판정 V4: {'통과' if verdicts['V4'] else '실패'}")

    passed = [name for name, value in verdicts.items() if value]
    failed = [name for name, value in verdicts.items() if not value]

    if not failed:
        conclusion = "넷 다 통과 — 모의 적용 근거 충분"
    elif not passed:
        conclusion = "넷 다 실패 — 거름 효과 없음"
    else:
        conclusion = f"일부 통과 — 통과 {', '.join(passed)} / 실패 {', '.join(failed)}"

    lines += ["", f"종합: {conclusion}"]
    text = "\n".join(lines) + "\n"
    TEXT_PATH.write_bytes(text.encode("utf-8"))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
