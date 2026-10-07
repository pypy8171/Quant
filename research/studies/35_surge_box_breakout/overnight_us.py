"""스터디 35 — 밤사이 미국장 정보로 SURGE 매수를 거르거나 줄이면 나아지나(사용자 2026-10-06).

판단 시각: 매수일 07:50 KST(엔진 기동 직후, NXT 프리마켓 08:00 개장 전 — 메인 세션 지시 2026-10-06). 체결은 그대로 KRX 시가 동시호가.
대상  : halt_trigger.load_trades() 263건(실매매 칸) + halt_validate_wide.parquet 넓은 표본 2,093건. 기간 자료 전체(매수일 2009-06 – 2026-03).
지표  : 직전 미국 정규장 나스닥·S&P500 하루 수익률, 나스닥 5·20일 수익률, 나스닥 20·60일선 아래 여부, VIX 종가·하루 변화 %,
        나스닥 선물 재개장 변화(정산 → 18:00 ET 시가, 07:50 KST 전에 열린 날만 = 미국 서머타임 기간).
        한국 종목 시가 갭(시가 / 전날 종가)은 07:50 에 알 수 없어 '참고'로만 적는다.
O1 3분위·위아래별 평균과 매수 월 묶음 t(넓은 표본·263건, 원 수익과 전 종목 같은 날 시가 매수 대비 초과 수익).
O2 걸어가며 고르기(그해 1월 1일 전 청산 매매만으로 후보 33개 중 선택), O3 연도 재표집 1,000번, O4 '그냥 덜 사기' 대조.
산출  : overnight_us.txt(요약), overnight_us_features.parquet(한국 거래일별 지표·시각), overnight_us_trades.parquet(263건 + 지표 + 걸어가며 비중),
        overnight_us_generic.parquet(전 종목 매일 다음 날 시가 → H일째 종가 평균, halt_validate.generic_daily_returns 결과 캐시).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/overnight_us.py
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
RAW_DIRECTORY = HERE / "halt_trigger_raw"
INDEX_PATH = HERE / "index_snapshot.parquet"
WIDE_PATH = HERE / "halt_validate_wide.parquet"
BARS_PATH = REPOSITORY / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
TEXT_PATH = HERE / "overnight_us.txt"
FEATURES_PATH = HERE / "overnight_us_features.parquet"
TRADES_PATH = HERE / "overnight_us_trades.parquet"
GENERIC_PATH = HERE / "overnight_us_generic.parquet"
SEED = 20261006
BOOTSTRAP_DRAWS = 1000
CAPITAL_WON = 2000.0
DRAWDOWN_FLOOR = 100.0
MINIMUM_TRAINING_YEARS = 3
DECISION_HOUR, DECISION_MINUTE = 7, 50
T_LIMIT = 2.7
BOOTSTRAP_LIMIT = 0.10
REDUCED_WEIGHT = 0.5

# (열 이름, 한국어 이름, 종류) — 종류 tertile 은 3분위, binary 는 1 = 해당(이평선 아래)
INDICATORS = [
    ("nasdaq_day", "나스닥 하루 %", "tertile"),
    ("standard_day", "S&P500 하루 %", "tertile"),
    ("nasdaq_week", "나스닥 5일 %", "tertile"),
    ("nasdaq_month", "나스닥 20일 %", "tertile"),
    ("nasdaq_below20", "나스닥 20일선 아래", "binary"),
    ("nasdaq_below60", "나스닥 60일선 아래", "binary"),
    ("vix_level", "VIX 종가", "tertile"),
    ("vix_change", "VIX 하루 %", "tertile"),
    ("futures_reopen", "나스닥 선물 재개장 %", "tertile"),
]

PLAN = """시험 목록과 통과 기준(돌리기 전에 고정, 2026-10-06)
질문: 매수일 07:50 KST(엔진 기동 직후, NXT 프리마켓 08:00 전)에 알 수 있는 밤사이 미국 정보로 SURGE 매수를 거르거나 줄이면 나아지는가.
      체결은 지금처럼 KRX 시가 동시호가(시가). 판단 시각은 메인 세션 지시로 08:50 → 07:50 으로 바꿨다.
고정: 실매매 칸 263건(저가 되돌림 < 15%, 대기 4일, 상승폭 ≥ 30%, 다음 날 시가 매수, 손절 관찰 최저 저가 −3%, 익절 +15%, 120거래일),
      비용 포함 수익 ret_low3_t15, 종목당 500만·자본 2,000만, 낙폭은 매수일 순서 곡선. 기간 자료 전체(매수일 2009-06 – 2026-03).
      2009 매매(앞선 스터디 기준 숫자 +4,095만·−803만에 들어 있다)도 그대로 둔다(내가 정함 — 기준 숫자와 맞추려고). 2010– 만 본 줄도 적는다.
      넓은 표본 = halt_validate_wide.parquet 2,093건(시가 매수, 되돌림 < 25%, 상승폭 ≥ 20%, 대기 3–5, 같은 종목·매수일 하나).
지표(9개, 모두 07:50 KST 전에 확정된 값 — 시각 검증 표를 결과에 적는다):
      나스닥 하루 % · S&P500 하루 % · 나스닥 5일 % · 나스닥 20일 % (직전 미국 정규장 종가 기준)
      나스닥 20일선 아래 · 60일선 아래(종가 < 평균) · VIX 종가 · VIX 하루 %
      나스닥 선물 재개장 % = 선물 직전 봉 종가(정산) → 18:00 ET 재개장 시가. 18:00 ET 는 서머타임 07:00 KST·표준시 08:00 KST 라
      표준시 기간에는 07:50 전에 열리지 않아 값 없음(그 매매는 이 지표에서 빠짐). 07:50 시점 선물 호가 자체는 일봉 자료로 못 만든다
      (Yahoo 시간봉은 최근 730일뿐, 무료 1분봉 이력 없음) — 그래서 재개장 시가로 대신한다.
      참고(판정 밖): 종목 시가 갭 = 매수일 시가 / 전날 종가. 07:50 엔 모르고 08:59 예상체결가로만 근접 — 실전 사용 불가.
      NXT 프리마켓 가격은 2025-03 개장이라 이력이 없다.
3분위 경계: 2009–2026 한국 거래일 전체의 지표 분포로 정한다(O1, 내가 정함). O2 는 그해 1월 1일 전 거래일만으로 다시 정한다.
O1 지표마다 아래·가운데·위 3분위(이평선은 아래/위) 평균. 차이 = 아래 3분위 − 위 3분위(이평선은 아래 − 위), t 는 매수 월 묶음(클러스터).
   넓은 표본·263건 각각, 원 수익과 초과 수익(사건 수익 − 같은 날 전 종목 '다음 날 시가 매수 → 같은 보유일(최대 20) 종가' 평균).
O2 걸어가며 고르기: 해 Y 마다 Y-01-01 전에 청산 끝난 263건 중 매매만으로 후보 33개 중 목표 최고를 골라 그해 매수에 적용, 이어붙인다.
   후보 = 거름 없음 / 3분위 지표 7개 × (아래 3분위·위 3분위) × (안 삼·절반만 삼) / 이평선 2개 × (안 삼·절반만 삼).
   목표 = 학습 총 손익 / max(|낙폭|, 100만)(halt_validate V1 과 같다). 같은 값이면 목록 앞쪽. 2012 부터, 그 전은 거름 없음.
O3 연도 재표집 1,000번(seed 20261006): 매수 연도를 복원 추출로 이어붙이고 O2 비중을 그대로 적용.
   개선 = 거름 총 손익 − 거름 없음 총 손익 × (거름 낙폭 / 거름 없음 낙폭) = '덜 사기' 대비 초과 손익(내가 정함). 낙폭 개선 확률도 적는다.
O4 '그냥 덜 사기': 낙폭·손익 모두 종목당 금액에 비례하므로 같은 낙폭을 금액 일률 축소로 얻으면 손익 = 거름 없음 손익 × 거름 낙폭 / 거름 없음 낙폭.
   O2 결과, O1 지표별 사후 최악 3분위 거름(참고), halt_validate 120일선(표본 안·걸어가며)에 같이 적는다.
통과 기준(사용자 지정 — 수치는 메인 세션이 정함):
   O1 넓은 표본 묶음 |t| ≥ 2.7(0.05/7 양측 보정. 지표가 9개라 0.05/9 면 2.77 — 같이 표시) 그리고 263건 차이가 같은 방향.
   O2 이어붙인 총 손익 > O4 '덜 사기' 손익.  O3 개선 ≤ 0 확률 < 10%.
   셋 다면 "07:50 거름 후보", 아니면 "효과 없음".
"""


def load_module(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


VALIDATE = load_module("halt_validate_module", HERE / "halt_validate.py")
HALT = VALIDATE.HALT


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=HERE, capture_output=True, text=True).stdout.strip()
    except OSError:
        return "unknown"


def money(value: float) -> str:
    return f"{value:+,.0f}만"


def max_drawdown(profits) -> float:
    return HALT.max_drawdown(np.asarray(profits, dtype=float))


# ---------------------------------------------------------------- 지표(07:50 KST 전 확정)


def read_raw(name: str) -> pd.DataFrame:
    frame = pd.read_parquet(RAW_DIRECTORY / f"fdr_{name}.parquet").sort_index()
    return frame[frame["close"] > 0]


def eastern_to_korea(dates: pd.DatetimeIndex, hour: int, minute: int = 0) -> pd.DatetimeIndex:
    """미국 날짜의 동부 시각을 한국 시각(시간대 없는 값)으로."""
    moments = (dates.normalize() + pd.Timedelta(hours=hour, minutes=minute)).tz_localize("America/New_York")
    return moments.tz_convert("Asia/Seoul").tz_localize(None)


def build_features(korean_days: np.ndarray) -> pd.DataFrame:
    """한국 거래일(정수 YYYYMMDD)마다 그날 07:50 KST 전에 확정된 미국 지표."""
    days = pd.to_datetime(pd.Series(korean_days).astype(str), format="%Y%m%d")
    decision = days + pd.Timedelta(hours=DECISION_HOUR, minutes=DECISION_MINUTE)
    features = pd.DataFrame({"date": korean_days, "decision_kst": decision.to_numpy()})

    nasdaq = read_raw("IXIC")
    standard = read_raw("GSPC")
    volatility = read_raw("VIX")
    futures = read_raw("NQF")

    def last_before(frame: pd.DataFrame) -> np.ndarray:
        # 한국 날짜보다 앞선 미국 날짜의 마지막 봉 = 직전 정규장. 같은 날짜 봉은 한국 다음 날 새벽에 끝나므로 뺀다
        return np.searchsorted(frame.index.to_numpy(), days.to_numpy(), side="left") - 1

    nasdaq_close = nasdaq["close"]
    nasdaq_table = pd.DataFrame({
        "nasdaq_day": nasdaq_close.pct_change() * 100.0,
        "nasdaq_week": (nasdaq_close / nasdaq_close.shift(5) - 1.0) * 100.0,
        "nasdaq_month": (nasdaq_close / nasdaq_close.shift(20) - 1.0) * 100.0,
        "nasdaq_below20": (nasdaq_close < nasdaq_close.rolling(20).mean()).astype(float).where(nasdaq_close.rolling(20).mean().notna()),
        "nasdaq_below60": (nasdaq_close < nasdaq_close.rolling(60).mean()).astype(float).where(nasdaq_close.rolling(60).mean().notna()),
    }, index=nasdaq.index)
    positions = last_before(nasdaq)

    for column in nasdaq_table.columns:
        features[column] = nasdaq_table[column].to_numpy()[positions]

    features["nasdaq_session"] = nasdaq.index.to_numpy()[positions]
    features["nasdaq_close_kst"] = eastern_to_korea(pd.DatetimeIndex(features["nasdaq_session"]), 16).to_numpy()

    positions = last_before(standard)
    features["standard_day"] = (standard["close"].pct_change() * 100.0).to_numpy()[positions]
    features["standard_session"] = standard.index.to_numpy()[positions]
    features["standard_close_kst"] = eastern_to_korea(pd.DatetimeIndex(features["standard_session"]), 16).to_numpy()

    positions = last_before(volatility)
    features["vix_level"] = volatility["close"].to_numpy()[positions]
    features["vix_change"] = (volatility["close"].pct_change() * 100.0).to_numpy()[positions]
    features["vix_session"] = volatility.index.to_numpy()[positions]
    features["vix_close_kst"] = eastern_to_korea(pd.DatetimeIndex(features["vix_session"]), 16, 15).to_numpy()

    # 선물: 한국 날짜와 같은 날짜의 봉이 전날 18:00 ET 에 열린다(Yahoo 선물 일봉 관례, 스터디 26 과 같은 가정)
    futures_dates = futures.index.to_numpy()
    same = np.searchsorted(futures_dates, days.to_numpy(), side="left")
    has_bar = (same < len(futures_dates)) & (same >= 1)
    has_bar &= futures_dates[np.clip(same, 0, len(futures_dates) - 1)] == days.to_numpy()
    clipped = np.clip(same, 1, len(futures_dates) - 1)
    reopen = (futures["open"].to_numpy()[clipped] / futures["close"].to_numpy()[clipped - 1] - 1.0) * 100.0
    reopen_kst = eastern_to_korea(pd.DatetimeIndex(days - pd.Timedelta(days=1)), 18)
    usable = has_bar & (reopen_kst.to_numpy() <= decision.to_numpy())
    features["futures_reopen_kst"] = reopen_kst.to_numpy()
    features["futures_has_bar"] = has_bar
    features["futures_reopen"] = np.where(usable, reopen, np.nan)
    return features.set_index("date")


def timing_table(features: pd.DataFrame, used_dates: np.ndarray) -> list[str]:
    part = features.loc[np.unique(used_dates)]
    decision = pd.to_datetime(part["decision_kst"])
    rows = [("나스닥 종가(하루·5일·20일·이평선)", "nasdaq_close_kst", None, "16:00 ET"),
            ("S&P500 종가", "standard_close_kst", None, "16:00 ET"),
            ("VIX 종가(16:15 ET 마감)", "vix_close_kst", None, "16:15 ET"),
            ("나스닥 선물 재개장 시가", "futures_reopen_kst", "futures_reopen", "18:00 ET 전날")]
    lines = ["  지표                               미국 시각       한국 시각(서머/표준)  쓴 날  07:50 전 최소 여유  최대 여유  07:50 넘은 날"]

    for label, column, value_column, eastern in rows:
        moments = pd.to_datetime(part[column])
        used = part[value_column].notna() if value_column else pd.Series(True, index=part.index)
        margin = (decision - moments)[used].dt.total_seconds() / 60.0
        violations = int((margin < 0).sum())
        lines.append(f"  {label:32s} {eastern:14s} {'07:00/08:00' if value_column else ('05:15/06:15' if 'VIX' in label else '05:00/06:00'):20s} "
                     f"{int(used.sum()):5d}  {margin.min():6.0f}분  {margin.max() / 60.0:7.1f}시간  {violations}")

    skipped = part["futures_has_bar"] & part["futures_reopen"].isna()
    lines.append(f"  선물 재개장이 08:00 KST(표준시)라 07:50 전에 못 쓴 날 {int(skipped.sum())}일 — 이 날은 선물 지표 값 없음으로 둔다")
    lines.append("  최대 여유가 긴 줄은 한국 연휴·미국 휴장으로 직전 정규장이 며칠 전인 날이다(그래도 '직전 정규장' 정의는 같다).")
    lines.append("  참고(판정 밖): 종목 시가 갭 — 시가는 09:00 확정, 07:50 엔 없음. NXT 프리마켓(08:00–08:50)은 2025-03 개장이라 이력 없음.")
    return lines


# ---------------------------------------------------------------- 표본·초과 수익


def generic_returns() -> pd.DataFrame:
    if GENERIC_PATH.exists():
        frame = pd.read_parquet(GENERIC_PATH)
        frame.columns = [int(column) for column in frame.columns]
        return frame

    frame = VALIDATE.generic_daily_returns()
    stored = frame.copy()
    stored.columns = [str(column) for column in stored.columns]
    stored.to_parquet(GENERIC_PATH)
    return frame


def attach(frame: pd.DataFrame, features: pd.DataFrame, generic: pd.DataFrame, return_column: str, hold_column: str,
           calendar: np.ndarray) -> pd.DataFrame:
    frame = frame.copy()
    rows = features.reindex(frame["entry_date"].to_numpy())

    for column, _, _ in INDICATORS:
        frame[column] = rows[column].to_numpy()

    frame["nasdaq_session"] = rows["nasdaq_session"].to_numpy()
    positions = np.searchsorted(calendar, frame["entry_date"].to_numpy(), side="left") - 1
    frame["signal_day"] = calendar[np.clip(positions, 0, None)]
    holds = np.clip(frame[hold_column].to_numpy(), 1, max(generic.columns))
    frame["market_same_hold"] = [generic[int(hold)].get(signal, np.nan) for hold, signal in zip(holds, frame["signal_day"].to_numpy())]
    frame["value"] = frame[return_column]
    frame["excess_value"] = frame["value"] - frame["market_same_hold"]
    frame["month"] = frame["entry_date"] // 100
    return frame


def stock_gap(frame: pd.DataFrame) -> np.ndarray:
    """참고용: 매수일 시가 / 전날 종가 − 1(%). 07:50 엔 알 수 없다."""
    codes = sorted(frame["code"].unique())
    bars = pd.read_parquet(BARS_PATH, columns=["Date", "code", "Open", "Close"], filters=[("code", "in", codes)])
    bars = bars.sort_values(["code", "Date"], kind="mergesort")
    bars["previous_close"] = bars.groupby("code", observed=True)["Close"].shift(1)
    bars["date_int"] = bars["Date"].dt.strftime("%Y%m%d").astype(np.int64)
    bars["gap"] = (bars["Open"] / bars["previous_close"] - 1.0) * 100.0
    table = bars.set_index(["code", "date_int"])["gap"]
    keys = list(zip(frame["code"], frame["entry_date"]))
    return table.reindex(keys).to_numpy()


# ---------------------------------------------------------------- O1


def tertile_cuts(values: pd.Series) -> tuple[float, float]:
    clean = values.dropna()
    return float(clean.quantile(1 / 3)), float(clean.quantile(2 / 3))


def bins_of(values: np.ndarray, kind: str, cuts: tuple[float, float] | None) -> np.ndarray:
    """0 아래 · 1 가운데 · 2 위(3분위) / 1 해당 · 0 아님(이평선). 값 없음은 −1."""
    values = np.asarray(values, float)
    result = np.full(len(values), -1)
    finite = np.isfinite(values)

    if kind == "binary":
        result[finite] = values[finite].astype(int)
        return result

    low, high = cuts
    result[finite & (values <= low)] = 0
    result[finite & (values > low) & (values <= high)] = 1
    result[finite & (values > high)] = 2
    return result


def compare(frame: pd.DataFrame, bins: np.ndarray, kind: str, value_column: str) -> dict:
    values = frame[value_column].to_numpy(float)
    valid = np.isfinite(values) & (bins >= 0)
    means = {}

    for label in ((0, 1, 2) if kind == "tertile" else (1, 0)):
        part = values[valid & (bins == label)]
        means[label] = (float(part.mean()) if len(part) else float("nan"), int(len(part)))

    if kind == "tertile":
        chosen = valid & (bins != 1)
        flag = (bins[chosen] == 0).astype(float)
    else:
        chosen = valid
        flag = (bins[chosen] == 1).astype(float)

    if flag.sum() < 2 or (1 - flag).sum() < 2:
        return {"means": means, "difference": float("nan"), "t": float("nan")}

    result = VALIDATE.cluster_difference(values[chosen], flag, frame["month"].to_numpy()[chosen])
    return {"means": means, "difference": result["difference"], "t": result["t"]}


def format_compare(result: dict, kind: str) -> str:
    means = result["means"]

    if kind == "tertile":
        body = " ".join(f"{name} {means[label][0]:+6.2f}%(n{means[label][1]})" for label, name in ((0, "아래"), (1, "가운데"), (2, "위")))
    else:
        body = f"아래 {means[1][0]:+6.2f}%(n{means[1][1]}) 위 {means[0][0]:+6.2f}%(n{means[0][1]})"

    return f"{body} | 차이 {result['difference']:+6.2f}%p 묶음 t {result['t']:+6.2f}"


# ---------------------------------------------------------------- O2·O3·O4


def candidates() -> list[dict]:
    items = [{"name": "거름 없음", "column": None, "side": None, "weight": 1.0}]

    for column, label, kind in INDICATORS:
        for weight in (0.0, REDUCED_WEIGHT):
            action = "안 삼" if weight == 0.0 else "절반"

            if kind == "binary":
                items.append({"name": f"{label} {action}", "column": column, "side": 1, "weight": weight, "kind": kind})
            else:
                for side, side_label in ((0, "아래 3분위"), (2, "위 3분위")):
                    items.append({"name": f"{label} {side_label} {action}", "column": column, "side": side, "weight": weight, "kind": kind})

    return items


def weights_for(trades: pd.DataFrame, item: dict, cuts: dict) -> np.ndarray:
    if item["column"] is None:
        return np.ones(len(trades))

    bins = bins_of(trades[item["column"]].to_numpy(), item["kind"], cuts.get(item["column"]))
    return np.where(bins == item["side"], item["weight"], 1.0)


def objective(total: float, drawdown: float) -> float:
    return total / max(abs(drawdown), DRAWDOWN_FLOOR)


def walk_forward(trades: pd.DataFrame, features: pd.DataFrame) -> tuple[np.ndarray, list[dict]]:
    items = candidates()
    years = sorted(trades["entry_year"].unique())
    first_test_year = years[0] + MINIMUM_TRAINING_YEARS
    weights = np.ones(len(trades))
    profits = trades["profit"].to_numpy()
    choices = []

    for year in years:
        in_year = (trades["entry_year"] == year).to_numpy()

        if year < first_test_year:
            choices.append({"year": year, "chosen": "거름 없음(자료 쌓는 중)", "training_n": 0, "year_n": int(in_year.sum()), "year_weight": float(in_year.sum())})
            continue

        past_days = features[features.index < year * 10000 + 101]
        cuts = {column: tertile_cuts(past_days[column]) for column, _, kind in INDICATORS if kind == "tertile"}
        training = (trades["exit_date"] < year * 10000 + 101).to_numpy()
        scored = []

        for order, item in enumerate(items):
            item_weights = weights_for(trades, item, cuts)
            weighted = profits[training] * item_weights[training]
            scored.append((objective(float(weighted.sum()), max_drawdown(weighted)), -order, item["name"], item_weights))

        scored.sort(key=lambda entry: (entry[0], entry[1]), reverse=True)
        best = scored[0]
        none_score = next(entry[0] for entry in scored if entry[2] == "거름 없음")
        weights[in_year] = best[3][in_year]
        choices.append({"year": year, "chosen": best[2], "training_n": int(training.sum()), "objective": best[0], "none_objective": none_score,
                        "runner_up": f"{scored[1][2]}({scored[1][0]:.2f})", "year_n": int(in_year.sum()), "year_weight": float(best[3][in_year].sum())})

    return weights, choices


def less_buying(base_total: float, base_drawdown: float, filtered_drawdown: float) -> float:
    if base_drawdown == 0:
        return base_total

    return base_total * filtered_drawdown / base_drawdown


def bootstrap(trades: pd.DataFrame, weights: np.ndarray) -> dict:
    generator = np.random.default_rng(SEED)
    years = sorted(trades["entry_year"].unique())
    blocks = {year: (trades["entry_year"] == year).to_numpy() for year in years}
    profits = trades["profit"].to_numpy()
    improvements, drawdown_improvements = [], []

    for _ in range(BOOTSTRAP_DRAWS):
        drawn = generator.choice(years, size=len(years), replace=True)
        base_profits = np.concatenate([profits[blocks[year]] for year in drawn])
        filtered = np.concatenate([profits[blocks[year]] * weights[blocks[year]] for year in drawn])
        base_drawdown = max_drawdown(base_profits)
        filtered_drawdown = max_drawdown(filtered)
        improvements.append(filtered.sum() - less_buying(base_profits.sum(), base_drawdown, filtered_drawdown))
        drawdown_improvements.append(filtered_drawdown - base_drawdown)

    improvements = np.array(improvements)
    drawdown_improvements = np.array(drawdown_improvements)
    return {"probability": float((improvements <= 0).mean()), "quantiles": np.quantile(improvements, [0.05, 0.25, 0.5, 0.75, 0.95]),
            "drawdown_probability": float((drawdown_improvements <= 0).mean())}


# ---------------------------------------------------------------- 본문


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    TEXT_PATH.write_bytes((PLAN + "\n(실행 중)\n").encode("utf-8"))
    calendar = pd.read_parquet(INDEX_PATH).sort_index().index.to_numpy()
    features = build_features(calendar)
    features.to_parquet(FEATURES_PATH)
    generic = generic_returns()

    trades = HALT.load_trades()
    trades = attach(trades, features, generic, "ret", "hold", calendar)
    wide = pd.read_parquet(WIDE_PATH)
    wide = attach(wide, features, generic, "ret_low3_t15", "hold_low3_t15", calendar)
    trades["stock_gap"] = stock_gap(trades)
    wide["stock_gap"] = stock_gap(wide)

    base_total = float(trades["profit"].sum())
    base_drawdown = max_drawdown(trades["profit"].to_numpy())
    lines = [PLAN, f"overnight_us — 커밋 {git_commit()}, 매매 {len(trades)}건 · 넓은 표본 {len(wide):,}건, 매수일 {trades['entry_date'].min()}–{trades['entry_date'].max()}, "
                   f"미국 자료 halt_trigger_raw/(FDR, 2008-12 – 2026-10 캐시 고정), seed {SEED}", ""]
    lines.append(f"재현 점검: 거름 없음 총 {money(base_total)} 낙폭 {money(base_drawdown)} (기대 +4,095만·−803만)")
    lines.append(f"초과 수익에 쓸 전 종목 평균이 없는 매매: 263건 중 {int(trades['market_same_hold'].isna().sum())}, 넓은 표본 중 {int(wide['market_same_hold'].isna().sum())}")

    lines += ["", "시각 검증 — 판단 시각 매수일 07:50 KST, 값이 확정된 시각과의 여유(263건 + 넓은 표본 매수일 전부)"]
    lines += timing_table(features, np.r_[trades["entry_date"].to_numpy(), wide["entry_date"].to_numpy()])
    lookahead = int((pd.to_datetime(trades["nasdaq_session"]) >= pd.to_datetime(trades["entry_date"].astype(str))).sum())
    lines.append(f"  직전 정규장 날짜 ≥ 매수일인 매매 {lookahead}건(0 이어야 한다)")

    # O1
    cuts = {column: tertile_cuts(features[column]) for column, _, kind in INDICATORS if kind == "tertile"}
    lines += ["", "O1 지표별 — 3분위 경계는 2009–2026 한국 거래일 전체 분포. 차이 = 아래 3분위 − 위 3분위(이평선은 아래 − 위), 묶음 = 매수 월"]
    o1_rows = []

    for column, label, kind in INDICATORS:
        cut = cuts.get(column)
        cut_text = f"경계 {cut[0]:+.2f} / {cut[1]:+.2f}" if cut else "이평선 아래 = 1"
        lines.append(f"  [{label}] {cut_text}")
        results = {}

        for sample_name, sample in (("넓은", wide), ("263", trades)):
            bins = bins_of(sample[column].to_numpy(), kind, cut)

            for value_column, value_name in (("value", "원"), ("excess_value", "초과")):
                result = compare(sample, bins, kind, value_column)
                results[(sample_name, value_name)] = result
                lines.append(f"    {sample_name:3s} {value_name}: {format_compare(result, kind)}")

        wide_result, small_result = results[("넓은", "원")], results[("263", "원")]
        same_direction = np.sign(wide_result["difference"]) == np.sign(small_result["difference"])
        passed = abs(wide_result["t"]) >= T_LIMIT and same_direction
        strict = abs(wide_result["t"]) >= 2.77 and same_direction
        lines.append(f"    판정: {'통과' if passed else '실패'} (넓은 |t| {abs(wide_result['t']):.2f} ≥ 2.7 {'O' if abs(wide_result['t']) >= T_LIMIT else 'x'}, "
                     f"같은 방향 {'O' if same_direction else 'x'}; 9개 보정 2.77 {'O' if strict else 'x'})")
        o1_rows.append({"column": column, "label": label, "kind": kind, "passed": bool(passed), "wide_t": wide_result["t"],
                        "wide_difference": wide_result["difference"], "small_difference": small_result["difference"],
                        "small_t": small_result["t"], "wide_excess_t": results[("넓은", "초과")]["t"]})

    lines.append("  참고(판정 밖, 07:50 엔 모름) — 종목 시가 갭 3분위(넓은 표본 경계)")
    gap_cut = tertile_cuts(wide["stock_gap"])

    for sample_name, sample in (("넓은", wide), ("263", trades)):
        bins = bins_of(sample["stock_gap"].to_numpy(), "tertile", gap_cut)
        lines.append(f"    {sample_name:3s} 원: 경계 {gap_cut[0]:+.2f}/{gap_cut[1]:+.2f} {format_compare(compare(sample, bins, 'tertile', 'value'), 'tertile')}")

    o1_passed = [row for row in o1_rows if row["passed"]]
    lines.append(f"  O1 통과 지표: {', '.join(row['label'] for row in o1_passed) if o1_passed else '없음'}")

    lines.append("  2010– 만(263건 중 2009 매매 제외), 원 수익 차이·묶음 t:")
    recent = trades[trades["entry_year"] >= 2010]

    for column, label, kind in INDICATORS:
        result = compare(recent, bins_of(recent[column].to_numpy(), kind, cuts.get(column)), kind, "value")
        lines.append(f"    {label:18s} 차이 {result['difference']:+6.2f}%p t {result['t']:+6.2f}")

    # O2
    weights, choices = walk_forward(trades, features)
    profits = trades["profit"].to_numpy()
    walk_profits = profits * weights
    walk_total = float(walk_profits.sum())
    walk_drawdown = max_drawdown(walk_profits)
    walk_less = less_buying(base_total, base_drawdown, walk_drawdown)
    trades["walk_weight"] = weights
    lines += ["", "O2 걸어가며 고르기 — 후보 33개, 목표 총 손익 / max(|낙폭|, 100만), 학습 = 그해 1월 1일 전에 청산 끝난 매매"]
    lines.append("  해   학습n  고른 조건                          목표 (거름 없음)  2위                                   그해 매수 n → 실린 비중")

    for choice in choices:
        if choice["training_n"] == 0:
            lines.append(f"  {choice['year']}     0  {choice['chosen']}  (그해 매수 {choice['year_n']})")
        else:
            lines.append(f"  {choice['year']}  {choice['training_n']:4d}  {choice['chosen']:32s} {choice['objective']:6.2f} ({choice['none_objective']:6.2f})  "
                         f"{choice['runner_up']:36s} {choice['year_n']:3d} → {choice['year_weight']:5.1f}")

    years = sorted(trades["entry_year"].unique())
    base_years = trades.groupby("entry_year")["profit"].sum().reindex(years, fill_value=0.0) / CAPITAL_WON * 100.0
    walk_years = pd.Series(walk_profits).groupby(trades["entry_year"].to_numpy()).sum().reindex(years, fill_value=0.0) / CAPITAL_WON * 100.0
    lines.append("  연도별(2,000만 대비 %) 거름 없음 / 걸어가며: " + " ".join(f"{str(year)[2:]}:{base_years[year]:+.0f}/{walk_years[year]:+.0f}" for year in years))
    lines.append(f"  이어붙인 결과: 총 {money(walk_total)} 낙폭 {money(walk_drawdown)} | 거름 없음 총 {money(base_total)} 낙폭 {money(base_drawdown)}")
    o2_passed = walk_total > walk_less
    lines.append(f"  O4 '덜 사기'(같은 낙폭 {money(walk_drawdown)}을 금액 축소로): 총 {money(walk_less)} → 거름 − 덜 사기 = {money(walk_total - walk_less)}")
    lines.append(f"  판정 O2: {'통과' if o2_passed else '실패'}")

    # O3
    result = bootstrap(trades, weights)
    o3_passed = result["probability"] < BOOTSTRAP_LIMIT
    lines += ["", f"O3 연도 재표집 {BOOTSTRAP_DRAWS}번(seed {SEED}) — 개선 = 거름 총 손익 − 덜 사기 손익"]
    lines.append("  개선 분위 5·25·50·75·95%: " + " ".join(money(value) for value in result["quantiles"]))
    lines.append(f"  개선 ≤ 0 확률 {result['probability'] * 100:.1f}% (참고: 낙폭 개선 ≤ 0 확률 {result['drawdown_probability'] * 100:.1f}%)")
    lines.append(f"  판정 O3: {'통과' if o3_passed else '실패'}")

    # O4 표
    lines += ["", "O4 '그냥 덜 사기' 대조표 — 덜 사기 손익 = 거름 없음 총 손익 × 거름 낙폭 / 거름 없음 낙폭"]
    lines.append("  조건                                         총 손익     낙폭     덜 사기 손익  거름 − 덜 사기")

    def o4_line(label: str, total: float, drawdown: float) -> str:
        equal = less_buying(base_total, base_drawdown, drawdown)
        return f"  {label:44s} {money(total):>9s} {money(drawdown):>8s} {money(equal):>10s} {money(total - equal):>10s}"

    lines.append(o4_line("거름 없음", base_total, base_drawdown))
    lines.append(o4_line("O2 걸어가며 고르기(이 시험)", walk_total, walk_drawdown))
    validate_features = VALIDATE.index_features()
    validate_trades = VALIDATE.attach_features(HALT.load_trades(), validate_features)
    fixed = VALIDATE.summary(validate_trades, VALIDATE.keep_mask(validate_trades, "kosdaq_below120"))
    validate_keep, _ = VALIDATE.walk_forward(validate_trades, VALIDATE.objective_ratio)
    validate_walk = VALIDATE.summary(validate_trades, validate_keep)
    lines.append(o4_line("halt_validate 코스닥 120일선 — 표본 안(사후)", fixed["total"], fixed["drawdown"]))
    lines.append(o4_line("halt_validate 걸어가며 고르기(V1)", validate_walk["total"], validate_walk["drawdown"]))

    for row in o1_rows:
        column, kind = row["column"], row["kind"]
        bins = bins_of(trades[column].to_numpy(), kind, cuts.get(column))
        labels = (0, 1, 2) if kind == "tertile" else (0, 1)
        worst = min(labels, key=lambda label: trades.loc[bins == label, "ret"].mean() if (bins == label).any() else np.inf)
        kept = profits[bins != worst]
        name = {0: "아래 3분위", 1: "가운데 3분위", 2: "위 3분위"}[worst] if kind == "tertile" else ("선 아래인 날" if worst == 1 else "선 위인 날")
        lines.append(o4_line(f"(사후) {row['label']} {name} 안 삼", float(kept.sum()), max_drawdown(kept)))

    o1_ok = bool(o1_passed)
    verdict = "07:50 거름 후보" if (o1_ok and o2_passed and o3_passed) else "효과 없음"
    lines += ["", f"종합: {verdict} (O1 {'통과' if o1_ok else '실패'} · O2 {'통과' if o2_passed else '실패'} · O3 {'통과' if o3_passed else '실패'})"]

    keep_columns = ["code", "name", "market", "entry_date", "exit_date", "entry_year", "ret", "kind", "hold", "profit", "signal_day", "nasdaq_session",
                    "market_same_hold", "excess_value", "stock_gap", "walk_weight"] + [column for column, _, _ in INDICATORS]
    trades[keep_columns].to_parquet(TRADES_PATH, index=False)
    text = "\n".join(lines) + "\n"
    TEXT_PATH.write_bytes(text.encode("utf-8"))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
