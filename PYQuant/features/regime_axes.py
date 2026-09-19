"""네 축 거시 국면 점수 — 성장·물가·유동성·위험선호 → 국면 라벨 → 노출 배수.

스펙 정본: research/RESET_2026-09-19_R2/macro-quant.md §2 (사전등록 research/studies/20_macro_overlay/PREREG.md).
백테스트(research/studies/20_macro_overlay/build_axes.py)와 라이브(regime.json 발행)가 이 파일의 같은 함수를 부른다.

시점 규칙
  결정 시각은 한국 거래일 D 08:50 KST. 시리즈 행은 `published_at`(날짜, 00:00) 이 D 보다 앞선 것만 보인다
  (`visible_from = published_at + 1일 ≤ D`). `observation_date` 로는 절대 정렬하지 않는다.
  값은 관측치마다 **첫 발표치**(가장 이른 `published_at`) 하나만 쓴다 — 나중 수정본은 그 시점에 몰랐던 값이므로
  과거 재구성에는 넣지 않는다. 판본이 없는 시리즈(ECOS 월간)는 `fallback_lag_days` 로 발표일을 근사하고 등급 B 로 적는다.
  FRED 시리즈의 ALFRED 판본 시작일 이전 관측(첫 판본 날짜에 한꺼번에 실린 행)도 같은 근사·등급 B 다.

점수
  변환 → robust z(중앙값·MAD, 창 W·최소 관측 수, ±clip) → 축 = Σ 가중·부호·z / Σ|가중| (유효 시리즈 2개 미만이면 NaN)
  → 성장·물가 부호는 데드밴드 이력 규칙으로만 바뀐다(월간 발표가 새로 보인 날에만 재판정)
  → 국면 4개 → 기본 배수 → 위험선호·유동성 일간 배수 → macro_scale (0.05 단위, 0~1).

공개 함수
  score(as_of, cell="four_axis", parameters=Parameters()) -> dict   라이브·검증 공용. as_of 뒤에 발표된 값은 결과를 못 바꾼다.
  build_axes_table(decision_dates, cell, parameters) -> DataFrame     백테스트용 전 기간 표(한 행 = 결정일 하나).

교체 예정: 시점 고정 조인은 PYQuant/data/point_in_time.py::as_of_join 이 생기면 그 함수로 바꾼다(지금은 이 파일 안의
`visible_position` 이 같은 일을 한다).
"""

from __future__ import annotations

import dataclasses
import datetime as dt
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[2]
MACRO_DIRECTORY = REPO_ROOT / "PYQuant" / "data" / "macro"
INDEX_CACHE_DIRECTORY = REPO_ROOT / "PYQuant" / ".index_cache"
FLOW_PATH = REPO_ROOT / "PYQuant" / "data" / "investor_flow_pit.parquet"
BARS_PATH = REPO_ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
DERIVED_FLOW_PATH = MACRO_DIRECTORY / "derived_foreign_flow_kospi.parquet"

CALENDAR_START = pd.Timestamp("1997-01-01")
AXES = ("growth", "inflation", "liquidity", "risk")
REGIME_LABELS = {("+", "-"): "expansion", ("+", "+"): "overheat", ("-", "+"): "contraction", ("-", "-"): "recovery"}
REGIME_LABELS_KOREAN = {"expansion": "확장", "overheat": "과열", "contraction": "수축", "recovery": "회복"}
DAYS_PER_MONTH = 21
WEEKS_PER_MONTH = 52.0 / 12.0


@dataclasses.dataclass(frozen=True)
class Parameters:
    """격자에서 움직이는 값 셋과 고정값. 기본값이 사전등록 중심 셀이다."""

    deadband: float = 0.25
    window_months: int = 120
    base_contraction: float = 0.5
    base_expansion: float = 1.0
    base_overheat: float = 0.75
    base_recovery: float = 0.75
    minimum_months: int = 36
    minimum_days: int = 250
    clip: float = 3.0
    risk_only_base: bool = False   # 축소판: 수축 배수만 적용하고 위험선호·유동성 일간 배수는 끈다

    @property
    def window_days(self) -> int:
        return self.window_months * DAYS_PER_MONTH

    @property
    def window_weeks(self) -> int:
        return int(round(self.window_months * WEEKS_PER_MONTH))

    @property
    def minimum_weeks(self) -> int:
        return int(round(self.minimum_months * WEEKS_PER_MONTH))


@dataclasses.dataclass(frozen=True)
class SeriesRule:
    """지표 표(스펙 §2-2)의 한 행."""

    name: str
    axis: str
    sign: float
    weight: float
    transform: str
    frequency: str          # "D" 일간 | "W" 주간 | "M" 월간
    loader: str             # "macro:<파일 stem>" | "index:<캐시 stem>" | "derived:foreign_flow" | "chain:<a>|<b>"
    fallback_lag_days: int  # published_at 이 없거나 판본 시작 이전인 행에 쓰는 발표 지연(달력일)
    cells: tuple[str, ...]
    note: str = ""
    grade_cap: str = "A"    # "B" 면 원자료 등급과 무관하게 B 로 적는다(스펙이 B 로 판정한 캐시 시리즈)


# 지표 표. 스펙 §2-2 에서 지금 parquet·캐시가 있는 행만 옮겼다. 빠진 행은 README "빠진 시리즈" 표에 적는다.
SERIES_TABLE: tuple[SeriesRule, ...] = (
    # 성장
    SeriesRule("us_unemployment", "growth", -1.0, 1.0, "sahm", "M", "macro:fred_UNRATE", 35, ("four_axis",)),
    SeriesRule("us_initial_claims", "growth", -1.0, 1.0, "average4_yoy", "W", "macro:fred_ICSA", 5, ("four_axis",)),
    SeriesRule("us_curve_10y_2y", "growth", 1.0, 0.5, "level", "D", "macro:fred_T10Y2Y", 1, ("four_axis",)),
    SeriesRule("korea_exports", "growth", 1.0, 2.0, "yoy", "M", "macro:ecos_exports", 46, ("four_axis",),
               "월간 확정치(통관). 10·20일 잠정치 parquet 이 생기면 같은 열의 앞 행으로 붙는다"),
    SeriesRule("sox", "growth", 1.0, 1.0, "return_60", "D", "index:idx__SOX_1985-01-01_2026-08-15_adj", 0,
               ("market_only", "four_axis")),
    # 물가
    SeriesRule("us_cpi", "inflation", 1.0, 1.5, "acceleration", "M", "macro:fred_CPIAUCSL", 45, ("four_axis",)),
    SeriesRule("us_ppi", "inflation", 1.0, 1.0, "yoy", "M", "macro:fred_PPIACO", 45, ("four_axis",)),
    SeriesRule("wti", "inflation", 1.0, 0.5, "return_60", "D", "index:idx_CL=F_1985-01-01_2026-08-15_adj", 0,
               ("four_axis",), "FRED DCOILWTICO 미적재 — 선물 캐시(등급 B)로 대신", "B"),
    SeriesRule("korea_cpi", "inflation", 1.0, 1.0, "yoy_minus_2pct", "M", "macro:ecos_cpi", 36, ("four_axis",)),
    # 유동성
    SeriesRule("fed_funds", "liquidity", -1.0, 1.0, "change_6m", "M", "macro:fred_FEDFUNDS", 38, ("four_axis",)),
    SeriesRule("us_2y", "liquidity", -1.0, 1.0, "change_60", "D", "macro:fred_DGS2", 1, ("four_axis",)),
    SeriesRule("dollar_index", "liquidity", -1.0, 1.0, "return_60", "D",
               "chain:index:idx_DX-Y.NYB_1985-01-01_2026-08-15_adj|macro:fred_DTWEXBGS", 7, ("four_axis",),
               "2006 이전은 DX-Y.NYB 캐시(등급 B)를 비율로 이어 붙인다"),
    SeriesRule("dollar_index_cache", "liquidity", -1.0, 1.0, "return_60", "D",
               "index:idx_DX-Y.NYB_1985-01-01_2026-08-15_adj", 0, ("market_only",), "", "B"),
    SeriesRule("bok_base_rate", "liquidity", -1.0, 1.0, "change_6m_daily", "D", "macro:ecos_base_rate", 0,
               ("four_axis",)),
    SeriesRule("ktb_3y", "liquidity", -1.0, 1.0, "change_60", "D", "macro:ecos_ktb_3y", 0, ("four_axis",)),
    SeriesRule("usdkrw", "liquidity", -1.0, 1.0, "return_60", "D", "macro:ecos_usdkrw", 0, ("four_axis",)),
    SeriesRule("usdkrw_cache", "liquidity", -1.0, 1.0, "return_60", "D",
               "index:idx_KRW=X_1985-01-01_2026-08-15_adj", 0, ("market_only",), "", "B"),
    SeriesRule("korea_m2", "liquidity", 1.0, 0.5, "yoy", "M", "macro:ecos_m2", 76, ("four_axis",)),
    # 위험선호
    SeriesRule("vix", "risk", -1.0, 1.0, "level", "D", "index:idx__VIX_2000-01-01_2026-08-15_adj", 0,
               ("market_only", "four_axis")),
    SeriesRule("hy_spread", "risk", -1.0, 1.0, "level", "D", "macro:fred_BAMLH0A0HYM2", 1,
               ("market_only", "four_axis"), "2023-09 이후만 있어 2024-09 전에는 NaN"),
    SeriesRule("kospi_gap_200", "risk", 1.0, 1.0, "gap_to_average_200", "D",
               "index:idx__KS11_1996-01-01_2026-08-15_adj", 0, ("market_only", "four_axis")),
    SeriesRule("foreign_flow_20", "risk", 1.0, 1.0, "level", "D", "derived:foreign_flow", 0,
               ("market_only", "four_axis"), "2019 이후만"),
)

MISSING_SERIES = (
    ("INDPRO", "성장", "ALFRED 미적재"),
    ("한국 수출 1·10·20일 잠정치", "성장", "관세청 data.go.kr 활용신청 대기"),
    ("T10YIE", "물가", "FRED 미적재"),
    ("DCOILWTICO", "물가", "FRED 미적재 — CL=F 캐시로 대신(등급 B)"),
    ("한국 PPI 404Y014", "물가", "ECOS 미적재"),
    ("M2SL", "유동성", "ALFRED 미적재"),
    ("기존 8지표 등락 점수", "위험선호", "등급 C — 백테스트 미투입(스펙대로)"),
)


def rules_for_cell(cell: str) -> tuple[SeriesRule, ...]:
    return tuple(rule for rule in SERIES_TABLE if cell in rule.cells)


# ----------------------------------------------------------------------------- 적재

def _first_release(frame: pd.DataFrame, fallback_lag_days: int) -> pd.DataFrame:
    """(observation_date, published_at, value, grade) → 관측치마다 첫 발표치 한 행. published_at 근사는 등급 B."""
    frame = frame.dropna(subset=["value"]).copy()
    frame["published_at"] = pd.to_datetime(frame["published_at"]).dt.normalize()
    frame["observation_date"] = pd.to_datetime(frame["observation_date"]).dt.normalize()
    fallback = pd.Timedelta(days=fallback_lag_days)
    missing = frame["published_at"].isna()

    if (~missing).any():
        first_vintage = frame.loc[~missing, "published_at"].min()
        artifact = (frame["published_at"] == first_vintage) & (
            (frame["published_at"] - frame["observation_date"]) > fallback + pd.Timedelta(days=30))
        missing = missing | artifact

    frame.loc[missing, "published_at"] = frame.loc[missing, "observation_date"] + fallback
    frame.loc[missing, "grade"] = "B"
    frame = frame.sort_values(["observation_date", "published_at"]).drop_duplicates("observation_date", keep="first")
    return frame[["observation_date", "published_at", "value", "grade"]].reset_index(drop=True)


def _load_macro(stem: str, fallback_lag_days: int) -> pd.DataFrame:
    path = MACRO_DIRECTORY / f"{stem}.parquet"

    if not path.exists():
        return pd.DataFrame(columns=["observation_date", "published_at", "value", "grade"])

    return _first_release(pd.read_parquet(path, columns=["observation_date", "published_at", "value", "grade"]),
                          fallback_lag_days)


def _load_index(stem: str) -> pd.DataFrame:
    path = INDEX_CACHE_DIRECTORY / f"{stem}.parquet"

    if not path.exists():
        return pd.DataFrame(columns=["observation_date", "published_at", "value", "grade"])

    bars = pd.read_parquet(path, columns=["date", "close"])
    frame = pd.DataFrame({"observation_date": pd.to_datetime(bars["date"]), "value": bars["close"].astype(float)})
    frame["published_at"] = frame["observation_date"]
    frame["grade"] = "A"
    return _first_release(frame, 0)


def build_foreign_flow_kospi(output_path: Path = DERIVED_FLOW_PATH) -> pd.DataFrame:
    """외국인 순매수 20일 합 / 코스피 20일 거래대금 합. 결과를 parquet 로 남기고 돌려준다."""
    flow = pd.read_parquet(FLOW_PATH, columns=["date", "ticker", "foreign_net_quantity", "close"])
    bars = pd.read_parquet(BARS_PATH, columns=["Date", "Close", "Volume", "code", "market"])
    kospi = bars[bars["market"] == "KOSPI"]
    kospi_codes = set(kospi["code"].unique())
    flow = flow[flow["ticker"].isin(kospi_codes)]
    net_value = (flow["foreign_net_quantity"] * flow["close"]).groupby(flow["date"]).sum()
    trading_value = (kospi["Close"] * kospi["Volume"]).groupby(kospi["Date"]).sum()
    joined = pd.concat({"net": net_value, "trading": trading_value}, axis=1).dropna().sort_index()
    ratio = joined["net"].rolling(20).sum() / joined["trading"].rolling(20).sum()
    frame = pd.DataFrame({"observation_date": ratio.index, "value": ratio.values})
    frame = frame.dropna()
    frame["published_at"] = frame["observation_date"]
    frame["source"] = "derived"
    frame["series_id"] = "foreign_flow_kospi_20d"
    frame["fetched_at"] = pd.Timestamp.now(tz="UTC").tz_localize(None)
    frame["grade"] = "A"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    frame.to_parquet(output_path, index=False)
    return frame


def _load_derived_flow() -> pd.DataFrame:
    if not DERIVED_FLOW_PATH.exists():
        if not (FLOW_PATH.exists() and BARS_PATH.exists()):
            return pd.DataFrame(columns=["observation_date", "published_at", "value", "grade"])

        build_foreign_flow_kospi()

    return _load_macro(DERIVED_FLOW_PATH.stem, 0)


def _chain(first: pd.DataFrame, second: pd.DataFrame) -> pd.DataFrame:
    """두 수준 시리즈를 이어 붙인다. 앞 시리즈는 겹치는 첫 날의 비율로 뒤 시리즈 수준에 맞추고 등급 B 로 적는다."""
    if second.empty:
        return first

    if first.empty:
        return second

    seam = second["observation_date"].min()
    overlap = first[first["observation_date"] >= seam]
    head = first[first["observation_date"] < seam].copy()

    if overlap.empty or head.empty:
        return second

    ratio = float(second["value"].iloc[0]) / float(overlap["value"].iloc[0])
    head["value"] = head["value"] * ratio
    head["grade"] = "B"
    return pd.concat([head, second], ignore_index=True)


def load_series(rule: SeriesRule) -> pd.DataFrame:
    """규칙 한 줄의 원자료 → (observation_date, published_at, value, grade), 관측일 오름차순."""
    frame = _load_by_loader(rule)

    if rule.grade_cap == "B" and not frame.empty:
        frame = frame.copy()
        frame["grade"] = "B"

    return frame


def _load_by_loader(rule: SeriesRule) -> pd.DataFrame:
    kind, _, target = rule.loader.partition(":")

    if kind == "macro":
        return _load_macro(target, rule.fallback_lag_days)

    if kind == "index":
        return _load_index(target)

    if kind == "derived":
        return _load_derived_flow()

    if kind == "chain":
        first_loader, second_loader = target.split("|")
        first = load_series(dataclasses.replace(rule, loader=first_loader))
        second = load_series(dataclasses.replace(rule, loader=second_loader))
        return _chain(first, second)

    raise ValueError(f"모르는 loader: {rule.loader}")


# ----------------------------------------------------------------------------- 변환·표준화

def transform(values: pd.Series, transform_name: str, frequency: str) -> pd.Series:
    """관측일 인덱스 시리즈 → 특징. 월간은 달력 달 기준으로 시차를 잡는다(행 수가 아니라 관측월 12개월 앞)."""
    if frequency == "M":
        monthly = values.copy()
        monthly.index = pd.DatetimeIndex(monthly.index).to_period("M")
        monthly = monthly[~monthly.index.duplicated(keep="last")]
        monthly = monthly.reindex(pd.period_range(monthly.index.min(), monthly.index.max(), freq="M"))
        yoy = monthly / monthly.shift(12) - 1.0

        if transform_name == "yoy" or transform_name == "yoy_minus_2pct":
            feature = yoy - (0.02 if transform_name == "yoy_minus_2pct" else 0.0)
        elif transform_name == "acceleration":
            annualized_3m = (monthly / monthly.shift(3)) ** 4 - 1.0
            feature = annualized_3m - yoy
        elif transform_name == "change_6m":
            feature = monthly - monthly.shift(6)
        elif transform_name == "sahm":
            average_3m = monthly.rolling(3).mean()
            feature = average_3m - average_3m.rolling(12).min()
        else:
            raise ValueError(f"월간 변환 아님: {transform_name}")

        feature = feature.reindex(values.index.to_period("M"))
        feature.index = values.index
        return feature

    if frequency == "W":
        if transform_name == "average4_yoy":
            average_4w = values.rolling(4).mean()
            return average_4w / average_4w.shift(52) - 1.0

        raise ValueError(f"주간 변환 아님: {transform_name}")

    if transform_name == "level":
        return values.astype(float)

    if transform_name == "return_60":
        return values / values.shift(60) - 1.0

    if transform_name == "change_60":
        return values - values.shift(60)

    if transform_name == "change_6m_daily":
        six_months_ago = values.reindex(values.index - pd.DateOffset(months=6), method="ffill")
        return values - pd.Series(six_months_ago.values, index=values.index)

    if transform_name == "gap_to_average_200":
        return values / values.rolling(200).mean() - 1.0

    raise ValueError(f"일간 변환 아님: {transform_name}")


def robust_z(feature: np.ndarray, window: int, minimum: int, clip: float) -> np.ndarray:
    """z = clip((f − median_W) / (1.4826·MAD_W), ±clip). 창은 그 시점까지의 값만(확장 → 고정 창)."""
    values = np.asarray(feature, dtype=float)
    count = len(values)
    result = np.full(count, np.nan)

    if count == 0:
        return result

    for position in range(count):
        start = max(0, position - window + 1)
        segment = values[start:position + 1]
        segment = segment[~np.isnan(segment)]

        if len(segment) < minimum or np.isnan(values[position]):
            continue

        median = np.median(segment)
        mad = np.median(np.abs(segment - median))

        if mad <= 0.0:
            continue

        result[position] = np.clip((values[position] - median) / (1.4826 * mad), -clip, clip)

    return result


def window_for(rule: SeriesRule, parameters: Parameters) -> tuple[int, int]:
    if rule.frequency == "M":
        return parameters.window_months, parameters.minimum_months

    if rule.frequency == "W":
        return parameters.window_weeks, parameters.minimum_weeks

    return parameters.window_days, parameters.minimum_days


@dataclasses.dataclass
class PreparedSeries:
    rule: SeriesRule
    visible_from: np.ndarray      # datetime64[D], 단조 증가
    observation_date: np.ndarray
    standardized: np.ndarray
    grade: str


def prepare_series(rule: SeriesRule, parameters: Parameters, raw: pd.DataFrame | None = None) -> PreparedSeries:
    raw = load_series(rule) if raw is None else raw

    if raw.empty:
        empty = np.array([], dtype="datetime64[D]")
        return PreparedSeries(rule, empty, empty, np.array([]), "missing")

    raw = raw.sort_values("observation_date").reset_index(drop=True)
    values = pd.Series(raw["value"].astype(float).values, index=pd.DatetimeIndex(raw["observation_date"]))
    feature = transform(values, rule.transform, rule.frequency)
    window, minimum = window_for(rule, parameters)
    standardized = robust_z(feature.values, window, minimum, parameters.clip)
    visible_from = (raw["published_at"] + pd.Timedelta(days=1)).values.astype("datetime64[D]")
    visible_from = np.maximum.accumulate(visible_from)
    grade = "B" if (raw["grade"] == "B").any() else "A"
    return PreparedSeries(rule, visible_from, raw["observation_date"].values.astype("datetime64[D]"), standardized, grade)


def visible_position(prepared: PreparedSeries, decision_dates: np.ndarray) -> np.ndarray:
    """결정일마다 보이는 마지막 행 위치(없으면 −1). 교체 예정: point_in_time.as_of_join."""
    return np.searchsorted(prepared.visible_from, decision_dates, side="right") - 1


# ----------------------------------------------------------------------------- 축·국면·배수

def axis_scores(prepared_list: list[PreparedSeries], decision_dates: np.ndarray) -> tuple[pd.DataFrame, pd.DataFrame]:
    """(축 점수 표, 시리즈별 z 표). 축은 유효 시리즈 2개 미만이면 NaN."""
    z_table = {}
    new_monthly = np.zeros(len(decision_dates), dtype=bool)

    for prepared in prepared_list:
        if len(prepared.standardized) == 0:
            z_table[prepared.rule.name] = np.full(len(decision_dates), np.nan)
            continue

        position = visible_position(prepared, decision_dates)
        picked = np.where(position >= 0, prepared.standardized[np.clip(position, 0, None)], np.nan)
        z_table[prepared.rule.name] = picked

        if prepared.rule.frequency == "M" and prepared.rule.axis in ("growth", "inflation"):
            changed = np.diff(position, prepend=position[0] - 1) != 0
            new_monthly |= changed & (position >= 0)

    z_frame = pd.DataFrame(z_table, index=decision_dates)
    axis_frame = pd.DataFrame(index=decision_dates)

    for axis in AXES:
        numerator = np.zeros(len(decision_dates))
        denominator = np.zeros(len(decision_dates))
        valid_count = np.zeros(len(decision_dates), dtype=int)

        for prepared in prepared_list:
            if prepared.rule.axis != axis:
                continue

            column = z_frame[prepared.rule.name].values
            valid = ~np.isnan(column)
            numerator += np.where(valid, prepared.rule.weight * prepared.rule.sign * column, 0.0)
            denominator += np.where(valid, abs(prepared.rule.weight), 0.0)
            valid_count += valid

        with np.errstate(invalid="ignore", divide="ignore"):
            score_values = np.where(valid_count >= 2, numerator / denominator, np.nan)

        axis_frame[axis] = score_values
        axis_frame[f"n_series_{axis}"] = valid_count

    axis_frame["new_monthly_release"] = new_monthly
    return axis_frame, z_frame


def next_sign(previous: str, value: float, deadband: float) -> str:
    """데드밴드 이력 규칙. NaN 이면 그대로."""
    if np.isnan(value):
        return previous

    if previous == "+" and value < -deadband:
        return "-"

    if previous == "-" and value > deadband:
        return "+"

    return previous


def base_multiplier(regime: str, parameters: Parameters) -> float:
    return {"expansion": parameters.base_expansion, "overheat": parameters.base_overheat,
            "contraction": parameters.base_contraction, "recovery": parameters.base_recovery}[regime]


def risk_multiplier(risk: float) -> float:
    if np.isnan(risk) or risk >= -1.0:
        return 1.0

    if risk >= -2.0:
        return 0.75

    return 0.5


def liquidity_multiplier(liquidity: float) -> float:
    if np.isnan(liquidity):
        return 1.0

    return float(np.clip(1.0 + 0.1 * min(liquidity, 0.0), 0.8, 1.0))


def macro_scale_of(base: float, multiplier_risk: float, multiplier_liquidity: float) -> float:
    return float(np.clip(round(base * multiplier_risk * multiplier_liquidity * 20.0) / 20.0, 0.0, 1.0))


def build_axes_table(decision_dates: pd.DatetimeIndex, cell: str = "four_axis",
                     parameters: Parameters = Parameters(),
                     prepared_list: list[PreparedSeries] | None = None) -> pd.DataFrame:
    """결정일 하나가 한 행. 열: decision_date, growth..risk, n_series_*, sign_growth, sign_inflation, regime, base,
    multiplier_risk, multiplier_liquidity, macro_scale, entry_halt, axis_missing, new_monthly_release."""
    if prepared_list is None:
        prepared_list = [prepare_series(rule, parameters) for rule in rules_for_cell(cell)]

    dates = np.asarray(decision_dates.values, dtype="datetime64[D]")
    axis_frame, z_frame = axis_scores(prepared_list, dates)

    growth = axis_frame["growth"].values
    inflation = axis_frame["inflation"].values
    liquidity = axis_frame["liquidity"].values
    risk = axis_frame["risk"].values
    new_release = axis_frame["new_monthly_release"].values

    sign_growth = "+"
    sign_inflation = "-"
    growth_initialized = False
    inflation_initialized = False
    rows = []

    for position in range(len(dates)):
        if new_release[position]:
            if not growth_initialized and not np.isnan(growth[position]):
                sign_growth = "-" if growth[position] < 0.0 else "+"
                growth_initialized = True
            else:
                sign_growth = next_sign(sign_growth, growth[position], parameters.deadband)

            if not inflation_initialized and not np.isnan(inflation[position]):
                sign_inflation = "+" if inflation[position] > 0.0 else "-"
                inflation_initialized = True
            else:
                sign_inflation = next_sign(sign_inflation, inflation[position], parameters.deadband)

        regime = REGIME_LABELS[(sign_growth, sign_inflation)]
        base = base_multiplier(regime, parameters)

        if parameters.risk_only_base:
            multiplier_risk = 1.0
            multiplier_liquidity = 1.0
        else:
            multiplier_risk = risk_multiplier(risk[position])
            multiplier_liquidity = liquidity_multiplier(liquidity[position])

        rows.append((sign_growth, sign_inflation, regime, base, multiplier_risk, multiplier_liquidity,
                     macro_scale_of(base, multiplier_risk, multiplier_liquidity),
                     bool(not np.isnan(risk[position]) and risk[position] < -2.5 and not parameters.risk_only_base)))

    state = pd.DataFrame(rows, columns=["sign_growth", "sign_inflation", "regime", "base", "multiplier_risk",
                                        "multiplier_liquidity", "macro_scale", "entry_halt"], index=dates)
    table = pd.concat([axis_frame, state], axis=1)
    table["axis_missing"] = table[list(AXES)].isna().any(axis=1)
    table.index.name = "decision_date"
    table = table.reset_index()
    table["decision_date"] = pd.to_datetime(table["decision_date"])
    table.attrs["z_frame"] = z_frame
    return table


def decision_calendar(as_of: dt.date | pd.Timestamp, start: pd.Timestamp = CALENDAR_START) -> pd.DatetimeIndex:
    """결정일 달력 = 평일. 한국 휴장일의 결정은 다음 거래일 시가에 그대로 쓰인다."""
    return pd.bdate_range(start, pd.Timestamp(as_of).normalize())


def score(as_of: dt.date | pd.Timestamp, cell: str = "four_axis", parameters: Parameters = Parameters(),
          prepared_list: list[PreparedSeries] | None = None) -> dict:
    """as_of 08:50 KST 의 국면 점수. 이력 규칙 때문에 1997 부터 재생하고 마지막 행을 돌려준다(라이브 하루 한 번)."""
    table = build_axes_table(decision_calendar(as_of), cell, parameters, prepared_list)
    last = table.iloc[-1]
    return {
        "as_of": pd.Timestamp(as_of).strftime("%Y-%m-%d"),
        "cell": cell,
        "growth": None if np.isnan(last["growth"]) else round(float(last["growth"]), 4),
        "inflation": None if np.isnan(last["inflation"]) else round(float(last["inflation"]), 4),
        "liquidity": None if np.isnan(last["liquidity"]) else round(float(last["liquidity"]), 4),
        "risk": None if np.isnan(last["risk"]) else round(float(last["risk"]), 4),
        "label": REGIME_LABELS_KOREAN[last["regime"]],
        "regime": last["regime"],
        "multiplier": float(last["macro_scale"]),
        "base": float(last["base"]),
        "multiplier_risk": float(last["multiplier_risk"]),
        "multiplier_liquidity": float(last["multiplier_liquidity"]),
        "entry_halt": bool(last["entry_halt"]),
        "axis_missing": bool(last["axis_missing"]),
    }
