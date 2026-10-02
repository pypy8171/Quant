"""스터디 29 — 국면 점수 다시 맞추기: 현행 표 점수 vs 연속 특징 선형 점수.

목표 변수: 결정 시각 t → 당일 종가 수익률(코스피·코스닥 평균, %). 보조: t → 다음날 종가, 눌림 후보 바스켓 t → 종가(왕복 0.215% 차감).
현행 점수: Quant/src/regime/RegimeFeed.cpp 표 규칙. 복원은 스터디 26 replay_regime_score.py의 함수(vote_for·price_at 등)를
  그대로 불러 쓰고, 시세 캐시도 스터디 26 raw/를 경로로 읽는다(복사하지 않는다).

갈래
  daily   2015-01~2026-09, 결정 시각 = 개장 직후(네이버 시가). 현행 점수 = 스터디 26 daily_scores.tsv score_open.
          기간 분리: 학습 2015-2022 / 검증 2023-2026-09, 그리고 연 단위 walk-forward(학습 3년 → 검증 1년, 2018~2026).
  hourly  2024-05~2026-09(581일), 결정 시각 09:00·10:00·11:00·12:00·13:00·14:00. 현행 점수는 그 시각 값을 다시 계산.
          기간 분리: 날짜 앞 2/3 학습 / 뒤 1/3 검증, 그리고 확장 창 walk-forward(250일 뒤부터 60일씩).
  breadth hourly 중 1분봉 백필이 있는 2025-09~(cache/minute_panel.parquet, build_minute_panel.py) — 시장 폭·눌림 바스켓.

특징은 각 시각까지 알 수 있던 값만 쓴다. z = 등락률 ÷ 그 지표 일간 등락률의 직전 60거래일 표준편차(당일 제외).
모형은 학습 구간에서만 고른다: 단일 특징 IC가 |IC| ≥ 0.02이고 학습 구간 앞뒤 절반에서 부호가 같은 것 중 |IC| 상위 5개 →
  표준화 → 리지(α = 0.1 × 학습 행 수). 비교용으로 같은 특징의 IC 부호 동일 가중 합도 낸다.

사용: py -X utf8 research/studies/29_regime_score_refit/build_minute_panel.py   (분봉 캐시, 처음 한 번)
      py -X utf8 research/studies/29_regime_score_refit/refit_regime_score.py
산출: 같은 폴더 *.tsv, metrics.json, cache/*.parquet
"""
import importlib.util
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY_DIR = Path(__file__).resolve().parent
REPO_ROOT = STUDY_DIR.parents[2]
STUDY26 = REPO_ROOT / "research" / "studies" / "26_regime_threshold_history"
MINUTE_PANEL = STUDY_DIR / "cache" / "minute_panel.parquet"
CACHE_DIR = STUDY_DIR / "cache"
KST = "Asia/Seoul"
ROUND_TRIP_COST_PCT = 0.215
SEED = 29
MAX_FEATURES = 5
MIN_ABS_IC = 0.02
RIDGE_ALPHA_PER_ROW = 0.1
HALT_SCORE = -7
ON_SCORE = 3
DECISION_HOURS = [(9, 0), (10, 0), (11, 0), (12, 0), (13, 0), (14, 0)]
SIGMA_WINDOW = 60

module_specification = importlib.util.spec_from_file_location("study26", STUDY26 / "replay_regime_score.py")
study26 = importlib.util.module_from_spec(module_specification)
module_specification.loader.exec_module(study26)

DAILY_FEATURES = ["z_NQ_F", "z_ES_F", "z_TNX10", "z_VIX", "z_WTI", "z_USDKRW_prev", "z_KOSPI_gap", "z_KOSDAQ_gap",
                  "z_KOSPI_prevday"]
HOURLY_FEATURES = ["z_NQ_F", "z_ES_F", "z_TNX10", "z_VIX", "z_WTI", "z_USDKRW", "z_NQ_intra", "z_KOSPI_ret",
                   "z_KOSDAQ_ret", "z_gap", "z_since_open", "z_dd_high", "z_twap_dist"]
BREADTH_FEATURES = ["breadth_up", "breadth_below_vwap", "breadth_median_ret"]


# ---------------------------------------------------------------- 공통


def entry_scale(score: float) -> float:
    """RegimeFeed.cpp entry_scale(정지선 −7, on +3): (−7,0)·(−3.5,0.4)·(0,0.7)·(3,1.0) 직선, 0.1 단위."""
    points = [(HALT_SCORE, 0.0), (HALT_SCORE / 2.0, 0.4), (0.0, 0.7), (ON_SCORE, 1.0)]

    if score <= points[0][0]:
        return 0.0

    if score >= points[-1][0]:
        return 1.0

    for (left_x, left_y), (right_x, right_y) in zip(points, points[1:]):
        if left_x <= score <= right_x:
            return round(left_y + (right_y - left_y) * (score - left_x) / (right_x - left_x), 1)

    return 1.0


def spearman(left, right) -> float:
    frame = pd.DataFrame({"x": np.asarray(left, dtype=float), "y": np.asarray(right, dtype=float)}).dropna()

    if len(frame) < 10:
        return np.nan

    return float(frame["x"].rank().corr(frame["y"].rank()))


def rolling_sigma(close: pd.Series) -> pd.Series:
    """일간 등락률(%)의 직전 60거래일 표준편차. 값은 그 날짜 종가까지 포함 — 조회는 '결정일 전' 마지막 값으로 한다."""
    return (close.pct_change() * 100.0).rolling(SIGMA_WINDOW, min_periods=20).std()


def value_before(series: pd.Series, day: pd.Timestamp) -> float:
    position = series.index.searchsorted(day, side="left") - 1
    return float(series.iloc[position]) if position >= 0 else np.nan


def load_inputs():
    kospi = study26.load_naver_index("KOSPI", False)
    kosdaq = study26.load_naver_index("KOSDAQ", False)
    us = {name: study26.load_fdr(name, False) for name in study26.FDR_DAILY}
    krw_daily = study26.load_yahoo("KRW=X", "1d", "max", "KRW", False)
    hourly = {name: study26.load_yahoo(symbol, "60m", "730d", name, False)
              for name, symbol in study26.YAHOO_HOURLY.items()}
    krw = krw_daily.copy()
    krw.index = (krw.index + pd.Timedelta(hours=1)).tz_convert(None).normalize()
    krw = krw[~krw.index.duplicated(keep="last")]
    sigma = {
        "KOSPI": rolling_sigma(kospi["close"]), "KOSDAQ": rolling_sigma(kosdaq["close"]),
        "NQ_F": rolling_sigma(us["IXIC"]["close"]), "ES_F": rolling_sigma(us["GSPC"]["close"]),
        "TNX10": rolling_sigma(us["TNX"]["close"]), "VIX": rolling_sigma(us["VIX"]["close"]),
        "WTI": rolling_sigma(us["CLF"]["close"]), "USDKRW": rolling_sigma(krw["close"]),
    }
    return kospi, kosdaq, us, krw, hourly, sigma


# ---------------------------------------------------------------- 갈래별 패널


def build_daily_panel(kospi, kosdaq, us, krw, sigma) -> pd.DataFrame:
    scores = pd.read_csv(STUDY26 / "daily_scores.tsv", sep="\t", parse_dates=["date"], index_col="date")
    records = []
    kospi_close = kospi["close"]
    kosdaq_close = kosdaq["close"]

    for day in scores.index:
        position = kospi_close.index.get_loc(day)
        previous_kospi = kospi_close.iloc[position - 1]
        previous_kosdaq = kosdaq_close.loc[:day].iloc[-2]
        before_previous_kospi = kospi_close.iloc[position - 2]
        raw = {
            "NQ_F": study26.last_session_percent(us["IXIC"], day)[0] + study26.futures_since_settle_at_open(us["NQF"], day),
            "ES_F": study26.last_session_percent(us["GSPC"], day)[0] + study26.futures_since_settle_at_open(us["ESF"], day),
            "TNX10": study26.last_session_percent(us["TNX"], day)[0],
            "VIX": study26.last_session_percent(us["VIX"], day)[0],
            "WTI": study26.futures_since_settle_at_open(us["CLF"], day),
            "USDKRW_prev": study26.last_session_percent(krw, day)[0],
            "KOSPI_gap": (kospi.loc[day, "open"] / previous_kospi - 1.0) * 100.0,
            "KOSDAQ_gap": (kosdaq.loc[day, "open"] / previous_kosdaq - 1.0) * 100.0,
            "KOSPI_prevday": (previous_kospi / before_previous_kospi - 1.0) * 100.0,
        }
        sigma_of = {"NQ_F": "NQ_F", "ES_F": "ES_F", "TNX10": "TNX10", "VIX": "VIX", "WTI": "WTI", "USDKRW_prev": "USDKRW",
                    "KOSPI_gap": "KOSPI", "KOSDAQ_gap": "KOSDAQ", "KOSPI_prevday": "KOSPI"}
        record = {"date": day, "slot": "09:00", "score_now": scores.loc[day, "score_open"]}
        record.update(raw)

        for name, value in raw.items():
            record["z_" + name] = value / value_before(sigma[sigma_of[name]], day)

        next_position = position + 1
        kospi_next = kospi_close.iloc[next_position] if next_position < len(kospi_close) else np.nan
        kosdaq_next = kosdaq_close.loc[day:].iloc[1] if len(kosdaq_close.loc[day:]) > 1 else np.nan
        record["kospi_to_close"] = (kospi.loc[day, "close"] / kospi.loc[day, "open"] - 1.0) * 100.0
        record["kosdaq_to_close"] = (kosdaq.loc[day, "close"] / kosdaq.loc[day, "open"] - 1.0) * 100.0
        record["kospi_to_next_close"] = (kospi_next / kospi.loc[day, "open"] - 1.0) * 100.0
        record["kosdaq_to_next_close"] = (kosdaq_next / kosdaq.loc[day, "open"] - 1.0) * 100.0
        records.append(record)

    frame = pd.DataFrame(records)
    frame["target"] = (frame["kospi_to_close"] + frame["kosdaq_to_close"]) / 2.0
    frame["target_next"] = (frame["kospi_to_next_close"] + frame["kosdaq_to_next_close"]) / 2.0
    return frame


def build_hourly_panel(kospi, kosdaq, us, krw, hourly, sigma) -> pd.DataFrame:
    days = pd.read_csv(STUDY26 / "hourly_scores.tsv", sep="\t", parse_dates=["date"], index_col="date").index
    korean = {}

    for name in ("KS11", "KQ11"):
        frame = hourly[name].copy()
        local = frame.index.tz_convert(KST)
        frame["day"] = local.tz_localize(None).normalize()
        frame["end"] = [min(stamp + pd.Timedelta(hours=1), stamp.normalize() + pd.Timedelta(hours=15, minutes=30))
                        for stamp in local]
        korean[name] = {day: group for day, group in frame.groupby("day")}

    records = []
    sigma_of = {"NQ_F": "NQ_F", "ES_F": "ES_F", "TNX10": "TNX10", "VIX": "VIX", "WTI": "WTI", "USDKRW": "USDKRW",
                "NQ_intra": "NQ_F", "KOSPI_ret": "KOSPI", "KOSDAQ_ret": "KOSDAQ"}

    for day in days:
        previous_kospi = kospi["close"].loc[:day].iloc[-2]
        previous_kosdaq = kosdaq["close"].loc[:day].iloc[-2]
        next_kospi = kospi["close"].loc[day:]
        next_kosdaq = kosdaq["close"].loc[day:]
        next_kospi = next_kospi.iloc[1] if len(next_kospi) > 1 else np.nan
        next_kosdaq = next_kosdaq.iloc[1] if len(next_kosdaq) > 1 else np.nan
        nasdaq_percent = study26.last_session_percent(us["IXIC"], day)[0]
        sp_percent = study26.last_session_percent(us["GSPC"], day)[0]
        tnx_percent = study26.last_session_percent(us["TNX"], day)[0]
        vix_percent = study26.last_session_percent(us["VIX"], day)[0]
        settle = {name: study26.last_close_before(us[name], day) for name in ("NQF", "ESF", "CLF")}
        day_local = pd.Timestamp(day).tz_localize(KST)
        fx_base_frame = hourly["KRW"].loc[hourly["KRW"].index <= (day_local - pd.Timedelta(hours=1)).tz_convert("UTC")]
        fx_base_frame = fx_base_frame[fx_base_frame.index.hour == 23]
        fx_base = float(fx_base_frame["open"].iloc[-1]) if len(fx_base_frame) else np.nan
        nq_reference = study26.price_at(hourly["NQF"], (day_local + pd.Timedelta(hours=9)).tz_convert("UTC"))
        kospi_bars = korean["KS11"].get(day)
        kosdaq_bars = korean["KQ11"].get(day)
        open_kospi = kospi.loc[day, "open"]
        open_kosdaq = kosdaq.loc[day, "open"]
        sigma_day = {key: value_before(series, day) for key, series in sigma.items()}

        for hour, minute in DECISION_HOURS:
            moment_local = day_local + pd.Timedelta(hours=hour, minutes=minute)
            moment = moment_local.tz_convert("UTC")

            if hour == 9:
                kospi_now, kosdaq_now = open_kospi, open_kosdaq
                kospi_high, kosdaq_high = open_kospi, open_kosdaq
                kospi_twap, kosdaq_twap = open_kospi, open_kosdaq
            else:
                if kospi_bars is None or kosdaq_bars is None:
                    continue

                ended_kospi = kospi_bars[kospi_bars["end"] <= moment_local]
                ended_kosdaq = kosdaq_bars[kosdaq_bars["end"] <= moment_local]

                if ended_kospi.empty or ended_kosdaq.empty or ended_kospi["end"].iloc[-1] != moment_local:
                    continue

                kospi_now, kosdaq_now = float(ended_kospi["close"].iloc[-1]), float(ended_kosdaq["close"].iloc[-1])
                kospi_high = max(open_kospi, float(ended_kospi["high"].max()))
                kosdaq_high = max(open_kosdaq, float(ended_kosdaq["high"].max()))
                kospi_twap = float(np.mean([open_kospi, *ended_kospi["close"]]))
                kosdaq_twap = float(np.mean([open_kosdaq, *ended_kosdaq["close"]]))

            nq_price = study26.price_at(hourly["NQF"], moment)
            raw = {
                "NQ_F": nasdaq_percent + (nq_price / settle["NQF"] - 1.0) * 100.0,
                "ES_F": sp_percent + (study26.price_at(hourly["ESF"], moment) / settle["ESF"] - 1.0) * 100.0,
                "TNX10": tnx_percent,
                "VIX": vix_percent,
                "WTI": (study26.price_at(hourly["CLF"], moment) / settle["CLF"] - 1.0) * 100.0,
                "USDKRW": (study26.price_at(hourly["KRW"], moment) / fx_base - 1.0) * 100.0,
                "NQ_intra": (nq_price / nq_reference - 1.0) * 100.0 if np.isfinite(nq_reference) else 0.0,
                "KOSPI_ret": (kospi_now / previous_kospi - 1.0) * 100.0,
                "KOSDAQ_ret": (kosdaq_now / previous_kosdaq - 1.0) * 100.0,
            }
            score = sum(study26.vote_for(key, raw[key])
                        for key in ("NQ_F", "ES_F", "TNX10", "VIX", "WTI", "USDKRW"))
            score += study26.vote_for("KOSPI", raw["KOSPI_ret"]) + study26.vote_for("KOSDAQ", raw["KOSDAQ_ret"])
            intra = raw["NQ_intra"]
            score += (1 if intra > 0 else -1) if np.isfinite(intra) and abs(intra) >= study26.INTRA_NQ_WARN else 0
            record = {"date": day, "slot": f"{hour:02d}:{minute:02d}", "minute": hour * 60 + minute, "score_now": score}
            record.update(raw)

            for name, value in raw.items():
                record["z_" + name] = value / sigma_day[sigma_of[name]]

            index_sigma = (sigma_day["KOSPI"] + sigma_day["KOSDAQ"]) / 2.0
            gap = ((open_kospi / previous_kospi - 1.0) + (open_kosdaq / previous_kosdaq - 1.0)) * 50.0
            since_open = ((kospi_now / open_kospi - 1.0) + (kosdaq_now / open_kosdaq - 1.0)) * 50.0
            drawdown = ((kospi_now / kospi_high - 1.0) + (kosdaq_now / kosdaq_high - 1.0)) * 50.0
            twap = ((kospi_now / kospi_twap - 1.0) + (kosdaq_now / kosdaq_twap - 1.0)) * 50.0
            record.update({"gap": gap, "since_open": since_open, "dd_high": drawdown, "twap_dist": twap,
                           "z_gap": gap / index_sigma, "z_since_open": since_open / index_sigma,
                           "z_dd_high": drawdown / index_sigma, "z_twap_dist": twap / index_sigma})
            record["kospi_to_close"] = (kospi.loc[day, "close"] / kospi_now - 1.0) * 100.0
            record["kosdaq_to_close"] = (kosdaq.loc[day, "close"] / kosdaq_now - 1.0) * 100.0
            record["kospi_to_next_close"] = (next_kospi / kospi_now - 1.0) * 100.0
            record["kosdaq_to_next_close"] = (next_kosdaq / kosdaq_now - 1.0) * 100.0
            records.append(record)

    frame = pd.DataFrame(records)
    frame["target"] = (frame["kospi_to_close"] + frame["kosdaq_to_close"]) / 2.0
    frame["target_next"] = (frame["kospi_to_next_close"] + frame["kosdaq_to_next_close"]) / 2.0
    return frame


def attach_breadth(frame: pd.DataFrame) -> pd.DataFrame:
    """1분봉 패널에서 (날짜, 시각)별 시장 폭과 눌림 후보 바스켓 t → 종가 수익률."""
    panel = pd.read_parquet(MINUTE_PANEL)
    panel = panel[panel["prev_close"].notna() & (panel["price"] > 0)]
    panel["ret_prev"] = panel["price"] / panel["prev_close"] - 1.0
    panel["to_close"] = (panel["close"] / panel["price"] - 1.0) * 100.0
    aligned = (panel["sma5"] > panel["sma10"]) & (panel["sma10"] > panel["sma20"]) & (panel["sma20"] > panel["sma60"])
    deviation = (panel["price"] / panel["sma20"] - 1.0) * 100.0
    panel["devscale_candidate"] = aligned & (deviation >= -8.0) & (deviation <= 5.0)
    panel["below_vwap"] = np.where(panel["vwap"].notna(), (panel["price"] < panel["vwap"]).astype(float), np.nan)
    grouped = panel.groupby(["day", "minute"])
    summary = pd.DataFrame({
        "universe_n": grouped.size(),
        "breadth_up": grouped["ret_prev"].apply(lambda series: float((series > 0).mean())),
        "breadth_below_vwap": grouped["below_vwap"].mean(),
        "breadth_median_ret": grouped["ret_prev"].median() * 100.0,
        "ew_to_close": grouped["to_close"].mean(),
    })
    candidates = panel[panel["devscale_candidate"]].groupby(["day", "minute"])["to_close"]
    summary["devscale_n"] = candidates.size()
    summary["devscale_to_close_net"] = candidates.mean() - ROUND_TRIP_COST_PCT
    summary = summary.reset_index().rename(columns={"day": "date"})
    summary["devscale_n"] = summary["devscale_n"].fillna(0).astype(int)
    return frame.merge(summary, on=["date", "minute"], how="left")


# ---------------------------------------------------------------- 모형


def select_features(train: pd.DataFrame, candidates: list[str], target: str = "target") -> list[tuple[str, float]]:
    dates = np.sort(train["date"].unique())
    middle = dates[len(dates) // 2]
    chosen = []

    for name in candidates:
        whole = spearman(train[name], train[target])
        first = spearman(train.loc[train["date"] < middle, name], train.loc[train["date"] < middle, target])
        second = spearman(train.loc[train["date"] >= middle, name], train.loc[train["date"] >= middle, target])

        if np.isfinite(whole) and abs(whole) >= MIN_ABS_IC and np.sign(first) == np.sign(second) == np.sign(whole):
            chosen.append((name, whole))

    chosen.sort(key=lambda pair: -abs(pair[1]))
    return chosen[:MAX_FEATURES]


class LinearScore:
    """표준화(학습 평균·표준편차, ±3 자름) → 리지 또는 IC 부호 동일 가중. 출력은 학습 분포 기준 현행 점수 단위로 맞춘다."""

    def __init__(self, train: pd.DataFrame, candidates: list[str], kind: str, target: str = "target"):
        self.kind = kind
        selected = select_features(train, candidates, target)
        self.features = [name for name, _ in selected]
        self.ic = dict(selected)
        clean = train.dropna(subset=self.features + [target])
        self.mean = clean[self.features].mean()
        self.std = clean[self.features].std().replace(0.0, 1.0)

        if not self.features:
            self.weights = pd.Series(dtype=float)
        elif kind == "ridge":
            matrix = self._standardize(clean)
            response = clean[target].to_numpy() - clean[target].mean()
            alpha = RIDGE_ALPHA_PER_ROW * len(clean)
            beta = np.linalg.solve(matrix.T @ matrix + alpha * np.eye(len(self.features)), matrix.T @ response)
            self.weights = pd.Series(beta, index=self.features)
        else:
            self.weights = pd.Series({name: np.sign(self.ic[name]) / len(self.features) for name in self.features})

        raw = self.raw(clean)
        self.train_raw_sorted = np.sort(raw[np.isfinite(raw)])
        self.train_now = clean["score_now"].to_numpy()
        self.raw_mean, self.raw_std = float(np.nanmean(raw)), float(np.nanstd(raw)) or 1.0
        self.now_mean, self.now_std = float(np.mean(self.train_now)), float(np.std(self.train_now)) or 1.0

    def _standardize(self, frame: pd.DataFrame) -> np.ndarray:
        return ((frame[self.features] - self.mean) / self.std).clip(-3.0, 3.0).fillna(0.0).to_numpy()

    def raw(self, frame: pd.DataFrame) -> np.ndarray:
        if not self.features:
            return np.zeros(len(frame))

        return self._standardize(frame) @ self.weights.to_numpy()

    def points(self, frame: pd.DataFrame) -> np.ndarray:
        """현행 점수와 평균·표준편차를 맞춘 점수(읽기 편하게)."""
        return (self.raw(frame) - self.raw_mean) / self.raw_std * self.now_std + self.now_mean

    def equivalent_now(self, frame: pd.DataFrame) -> np.ndarray:
        """분위수 맞춤: 새 점수의 학습 분위 → 같은 분위의 현행 점수. 정지 빈도·scale 분포를 현행과 같게 해 비교한다."""
        position = np.searchsorted(self.train_raw_sorted, self.raw(frame), side="right") / len(self.train_raw_sorted)
        return np.quantile(self.train_now, np.clip(position, 0.0, 1.0), method="nearest")


# ---------------------------------------------------------------- 평가


def compare(test: pd.DataFrame, score_columns: dict, target: str, label: str) -> list[dict]:
    rows = []
    clean = test.dropna(subset=[target])

    for name, (score_column, equivalent_column) in score_columns.items():
        frame = clean.dropna(subset=[score_column])
        scores = frame[score_column]
        values = frame[target]
        low10 = values[scores <= scores.quantile(0.10)]
        low20 = values[scores <= scores.quantile(0.20)]
        top20 = values[scores >= scores.quantile(0.80)]
        scale = frame[equivalent_column].map(entry_scale)
        halted = frame[equivalent_column] <= HALT_SCORE
        cost = ROUND_TRIP_COST_PCT if not target.startswith("devscale") else 0.0
        rows.append({
            "set": label, "target": target, "score": name, "rows": len(frame), "days": frame["date"].nunique(),
            "ic": round(spearman(scores, values), 4),
            "mean_all_pct": round(values.mean(), 4),
            "mean_low10_pct": round(low10.mean(), 4), "mean_low20_pct": round(low20.mean(), 4),
            "mean_top20_pct": round(top20.mean(), 4),
            "halt_share_pct": round(100.0 * halted.mean(), 2),
            "mean_when_halted_pct": round(values[halted].mean(), 4) if halted.any() else np.nan,
            "mean_scale": round(scale.mean(), 3),
            "scaled_net_pct_per_row": round((scale * (values - cost)).mean(), 4),
            "scaled_net_per_exposure_pct": round((scale * (values - cost)).sum() / scale.sum(), 4) if scale.sum() else np.nan,
        })

    return rows


def bootstrap_ic_gap(test: pd.DataFrame, new_column: str, target: str, repeats: int = 1000) -> dict:
    """날짜 단위 재표집으로 IC(새) − IC(현행)의 5~95% 구간."""
    generator = np.random.default_rng(SEED)
    clean = test.dropna(subset=[target, new_column])
    by_day = {day: group for day, group in clean.groupby("date")}
    days = np.array(list(by_day))
    gaps = []

    for _ in range(repeats):
        sample = pd.concat([by_day[day] for day in generator.choice(days, len(days), replace=True)])
        gaps.append(spearman(sample[new_column], sample[target]) - spearman(sample["score_now"], sample[target]))

    gaps = np.array(gaps)
    return {"gap_mean": round(float(np.nanmean(gaps)), 4), "gap_p05": round(float(np.nanpercentile(gaps, 5)), 4),
            "gap_p95": round(float(np.nanpercentile(gaps, 95)), 4), "share_positive": round(float(np.mean(gaps > 0)), 3)}


def single_feature_ic(frame: pd.DataFrame, features: list[str], branch: str, targets: list[str]) -> list[dict]:
    frame = frame.copy()
    frame["session"] = np.where(frame["minute"] < 600, "open", np.where(frame["minute"] < 720, "morning", "afternoon")) \
        if "minute" in frame else "open"
    rows = []

    for target in targets:
        for name in features + ["score_now"]:
            for session, group in [("all", frame)] + list(frame.groupby("session")):
                rows.append({"branch": branch, "target": target, "feature": name, "session": session,
                             "rows": int(group[[name, target]].dropna().shape[0]),
                             "ic": round(spearman(group[name], group[target]), 4)})

    return rows


def tnx_sign_table(frame: pd.DataFrame, branch: str) -> list[dict]:
    rows = []
    nasdaq = frame["NQ_F"]
    groups = {
        "all": frame.index,
        "nq_down(<=-0.5%)": frame.index[nasdaq <= -0.5],
        "nq_flat(-0.5~0.5)": frame.index[(nasdaq > -0.5) & (nasdaq < 0.5)],
        "nq_up(>=0.5%)": frame.index[nasdaq >= 0.5],
        "growth_scare(tnx<0 & nq<0)": frame.index[(frame["TNX10"] < 0) & (nasdaq < 0)],
        "rate_shock(tnx>0 & nq<0)": frame.index[(frame["TNX10"] > 0) & (nasdaq < 0)],
    }

    for name, index in groups.items():
        group = frame.loc[index]
        big_up = group[group["TNX10"] >= 1.5]["target"]
        big_down = group[group["TNX10"] <= -1.5]["target"]
        rows.append({"branch": branch, "group": name, "rows": len(group), "days": group["date"].nunique(),
                     "ic_tnx_vs_target": round(spearman(group["z_TNX10"], group["target"]), 4),
                     "mean_target_tnx_ge_+1.5": round(big_up.mean(), 3) if len(big_up) else np.nan, "n_ge": len(big_up),
                     "mean_target_tnx_le_-1.5": round(big_down.mean(), 3) if len(big_down) else np.nan,
                     "n_le": len(big_down)})

    return rows


def walk_forward(frame: pd.DataFrame, candidates: list[str], windows: list[tuple], kind: str = "ridge") -> pd.DataFrame:
    """windows = [(학습 시작, 학습 끝, 검증 시작, 검증 끝)] — 검증 구간 새 점수를 이어 붙인다."""
    pieces = []

    for train_start, train_end, test_start, test_end in windows:
        train = frame[(frame["date"] >= train_start) & (frame["date"] <= train_end)]
        test = frame[(frame["date"] >= test_start) & (frame["date"] <= test_end)].copy()

        if train.empty or test.empty:
            continue

        model = LinearScore(train, candidates, kind)
        test["oos_points"] = model.points(test)
        test["oos_equivalent"] = model.equivalent_now(test)
        test["oos_features"] = ",".join(model.features)
        test["window"] = f"{pd.Timestamp(test_start).date()}~{pd.Timestamp(test_end).date()}"
        pieces.append(test)

    return pd.concat(pieces) if pieces else pd.DataFrame()


def run_split(frame, candidates, train_mask, label, targets) -> tuple[list[dict], dict, dict]:
    train = frame[train_mask]
    test = frame[~train_mask].copy()
    models = {kind: LinearScore(train, candidates, kind) for kind in ("ridge", "sign")}

    for kind, model in models.items():
        test[f"new_{kind}"] = model.points(test)
        test[f"new_{kind}_eq"] = model.equivalent_now(test)

    test["now_eq"] = test["score_now"]
    columns = {"now": ("score_now", "now_eq"), "new_ridge": ("new_ridge", "new_ridge_eq"),
               "new_sign": ("new_sign", "new_sign_eq")}
    rows = []

    for target in targets:
        rows += compare(test, columns, target, label)

    weights = {kind: {"features": model.features, "train_ic": {name: round(value, 4) for name, value in model.ic.items()},
                      "weights_per_train_std": {name: round(float(value), 4) for name, value in model.weights.items()},
                      "train_rows": int(len(train)), "train_days": int(train["date"].nunique()),
                      "test_days": int(test["date"].nunique()),
                      "feature_mean": {name: round(float(value), 4) for name, value in model.mean.items()},
                      "feature_std": {name: round(float(value), 4) for name, value in model.std.items()}}
               for kind, model in models.items()}
    bootstrap = {target: bootstrap_ic_gap(test, "new_ridge", target) for target in targets[:1]}
    return rows, weights, {"test": test, "bootstrap": bootstrap}


def decile_table(frame: pd.DataFrame, score_column: str, label: str, targets: list[str]) -> pd.DataFrame:
    frame = frame.dropna(subset=[score_column]).copy()
    frame["decile"] = pd.qcut(frame[score_column].rank(method="first"), 10, labels=False) + 1
    rows = []

    for decile, group in frame.groupby("decile"):
        row = {"set": label, "score": score_column, "decile": int(decile), "rows": len(group),
               "score_lo": round(group[score_column].min(), 2), "score_hi": round(group[score_column].max(), 2),
               "score_now_mean": round(group["score_now"].mean(), 2)}

        for target in targets:
            if target in group:
                row[target + "_mean"] = round(group[target].mean(), 4)
                row[target + "_hit"] = round(float((group[target].dropna() > 0).mean()), 3)

        rows.append(row)

    return pd.DataFrame(rows)


# ---------------------------------------------------------------- 실행


def main() -> int:
    kospi, kosdaq, us, krw, hourly, sigma = load_inputs()
    CACHE_DIR.mkdir(exist_ok=True)
    daily = build_daily_panel(kospi, kosdaq, us, krw, sigma)
    daily["minute"] = 540
    hourly_frame = build_hourly_panel(kospi, kosdaq, us, krw, hourly, sigma)
    hourly_frame = attach_breadth(hourly_frame) if MINUTE_PANEL.exists() else hourly_frame
    daily.to_parquet(CACHE_DIR / "daily_features.parquet")
    hourly_frame.to_parquet(CACHE_DIR / "hourly_features.parquet")
    metrics = {"seed": SEED, "cost_round_trip_pct": ROUND_TRIP_COST_PCT, "max_features": MAX_FEATURES,
               "min_abs_ic": MIN_ABS_IC, "ridge_alpha_per_row": RIDGE_ALPHA_PER_ROW,
               "daily_period": [str(daily["date"].min().date()), str(daily["date"].max().date())],
               "daily_days": int(daily["date"].nunique()),
               "hourly_period": [str(hourly_frame["date"].min().date()), str(hourly_frame["date"].max().date())],
               "hourly_days": int(hourly_frame["date"].nunique()), "hourly_rows": int(len(hourly_frame))}

    # 1) 단일 특징 IC
    ic_rows = single_feature_ic(daily, DAILY_FEATURES, "daily", ["target", "target_next"])
    ic_rows += single_feature_ic(hourly_frame, HOURLY_FEATURES, "hourly", ["target", "target_next"])
    breadth_frame = hourly_frame.dropna(subset=["breadth_up"]) if "breadth_up" in hourly_frame else pd.DataFrame()

    if not breadth_frame.empty:
        ic_rows += single_feature_ic(breadth_frame, HOURLY_FEATURES + BREADTH_FEATURES, "breadth",
                                     ["target", "devscale_to_close_net", "ew_to_close"])

    pd.DataFrame(ic_rows).to_csv(STUDY_DIR / "ic_single.tsv", sep="\t", index=False)

    # 2) 금리 부호
    pd.DataFrame(tnx_sign_table(daily, "daily") + tnx_sign_table(hourly_frame, "hourly")).to_csv(
        STUDY_DIR / "tnx_sign.tsv", sep="\t", index=False)

    # 3) 고정 분리
    compare_rows = []
    weights = {}
    daily_rows, weights["daily"], daily_result = run_split(
        daily, DAILY_FEATURES, daily["date"] <= "2022-12-31", "daily_test_2023-2026", ["target", "target_next"])
    compare_rows += daily_rows
    hourly_dates = np.sort(hourly_frame["date"].unique())
    hourly_cut = hourly_dates[int(len(hourly_dates) * 2 / 3)]
    hourly_rows, weights["hourly"], hourly_result = run_split(
        hourly_frame, HOURLY_FEATURES, hourly_frame["date"] < hourly_cut, "hourly_test_last_third",
        ["target", "target_next"] + (["devscale_to_close_net", "ew_to_close"] if not breadth_frame.empty else []))
    compare_rows += hourly_rows
    metrics["hourly_test_start"] = str(pd.Timestamp(hourly_cut).date())
    metrics["bootstrap_ic_gap_ridge"] = {"daily": daily_result["bootstrap"], "hourly": hourly_result["bootstrap"]}

    if not breadth_frame.empty:
        breadth_dates = np.sort(breadth_frame["date"].unique())
        breadth_cut = breadth_dates[int(len(breadth_dates) * 2 / 3)]
        mask = breadth_frame["date"] < breadth_cut

        for label, candidates in [("breadth_window_no_breadth", HOURLY_FEATURES),
                                  ("breadth_window_with_breadth", HOURLY_FEATURES + BREADTH_FEATURES)]:
            rows, weights[label], result = run_split(breadth_frame, candidates, mask, label,
                                                     ["target", "devscale_to_close_net", "ew_to_close"])
            compare_rows += rows
            metrics.setdefault("bootstrap_ic_gap_ridge", {})[label] = result["bootstrap"]

        metrics["breadth_period"] = [str(pd.Timestamp(breadth_dates[0]).date()), str(pd.Timestamp(breadth_dates[-1]).date())]
        metrics["breadth_days"] = int(len(breadth_dates))
        metrics["breadth_test_start"] = str(pd.Timestamp(breadth_cut).date())
        metrics["breadth_universe_mean"] = round(float(breadth_frame["universe_n"].mean()), 1)
        metrics["devscale_candidates_mean"] = round(float(breadth_frame["devscale_n"].mean()), 1)

    pd.DataFrame(compare_rows).to_csv(STUDY_DIR / "compare.tsv", sep="\t", index=False)
    (STUDY_DIR / "model_weights.json").write_text(json.dumps(weights, ensure_ascii=False, indent=2), encoding="utf-8")

    # 4) walk-forward
    daily_windows = [(pd.Timestamp(f"{year - 3}-01-01"), pd.Timestamp(f"{year - 1}-12-31"),
                      pd.Timestamp(f"{year}-01-01"), pd.Timestamp(f"{year}-12-31")) for year in range(2018, 2027)]
    daily_oos = walk_forward(daily, DAILY_FEATURES, daily_windows)
    hourly_windows = []
    start = 250

    while start < len(hourly_dates):
        end = min(start + 60, len(hourly_dates)) - 1
        hourly_windows.append((hourly_dates[0], hourly_dates[start - 1], hourly_dates[start], hourly_dates[end]))
        start += 60

    hourly_oos = walk_forward(hourly_frame, HOURLY_FEATURES, hourly_windows)
    wf_rows = []

    for branch, oos in (("daily", daily_oos), ("hourly", hourly_oos)):
        for window, group in list(oos.groupby("window")) + [("ALL", oos)]:
            group = group.copy()
            row = {"branch": branch, "window": window, "rows": len(group), "days": group["date"].nunique(),
                   "features": group["oos_features"].iloc[0] if window != "ALL" else "",
                   "ic_now": round(spearman(group["score_now"], group["target"]), 4),
                   "ic_new": round(spearman(group["oos_points"], group["target"]), 4),
                   "ic_now_next": round(spearman(group["score_now"], group["target_next"]), 4),
                   "ic_new_next": round(spearman(group["oos_points"], group["target_next"]), 4)}

            for name, column in (("now", "score_now"), ("new", "oos_equivalent")):
                scale = group[column].map(entry_scale)
                row[f"scaled_net_{name}"] = round((scale * (group["target"] - ROUND_TRIP_COST_PCT)).mean(), 4)
                row[f"halt_share_{name}"] = round(100.0 * (group[column] <= HALT_SCORE).mean(), 1)
                row[f"mean_when_halted_{name}"] = round(group.loc[group[column] <= HALT_SCORE, "target"].mean(), 3)

            wf_rows.append(row)

    pd.DataFrame(wf_rows).to_csv(STUDY_DIR / "walkforward.tsv", sep="\t", index=False)
    daily_oos.to_parquet(CACHE_DIR / "daily_oos.parquet")
    hourly_oos.to_parquet(CACHE_DIR / "hourly_oos.parquet")
    metrics["walkforward_bootstrap_ic_gap"] = {
        "daily": bootstrap_ic_gap(daily_oos, "oos_points", "target"),
        "hourly": bootstrap_ic_gap(hourly_oos, "oos_points", "target"),
    }

    # 5) 오늘 같은 날: 코스피 −0.9~−0.3%, 나스닥 선물 ≥ +0.3% (hourly OOS 새 점수)
    like = hourly_oos[(hourly_oos["KOSPI_ret"] <= -0.3) & (hourly_oos["KOSPI_ret"] >= -0.9) & (hourly_oos["NQ_F"] >= 0.3)]
    like = like.assign(scale_now=like["score_now"].map(entry_scale), scale_new=like["oos_equivalent"].map(entry_scale))
    like_columns = ["date", "slot", "KOSPI_ret", "KOSDAQ_ret", "NQ_F", "TNX10", "score_now", "oos_points",
                    "oos_equivalent", "scale_now", "scale_new", "target", "target_next"]
    like[like_columns].round(3).to_csv(STUDY_DIR / "today_like.tsv", sep="\t", index=False)
    metrics["today_like"] = {
        "rows": int(len(like)), "days": int(like["date"].nunique()),
        "score_now_mean": round(float(like["score_now"].mean()), 2),
        "new_points_mean": round(float(like["oos_points"].mean()), 2),
        "scale_now_mean": round(float(like["scale_now"].mean()), 3),
        "scale_new_mean": round(float(like["scale_new"].mean()), 3),
        "target_mean": round(float(like["target"].mean()), 3),
        "target_hit": round(float((like["target"] > 0).mean()), 3),
        "all_rows_target_mean": round(float(hourly_oos["target"].mean()), 3),
    }

    # 6) 분위 표 — 정지선·scale 제안 입력
    targets = ["target", "target_next", "devscale_to_close_net", "ew_to_close"]
    deciles = pd.concat([decile_table(daily_oos, "oos_points", "daily_oos_2018-2026", targets),
                         decile_table(daily_oos, "score_now", "daily_oos_2018-2026", targets),
                         decile_table(hourly_oos, "oos_points", "hourly_oos", targets),
                         decile_table(hourly_oos, "score_now", "hourly_oos", targets)])
    deciles.to_csv(STUDY_DIR / "deciles.tsv", sep="\t", index=False)

    # 7) 제안 곡선 — 정지(하위 10%) / 절반(10~20%) / 전량(20% 위). 분위 경계는 OOS 분포에서 고른 것이라 이 표는 표본 안 평가다.
    q10, q20 = hourly_oos["oos_points"].quantile(0.10), hourly_oos["oos_points"].quantile(0.20)

    def proposed_scale(points: float) -> float:
        return 0.0 if points <= q10 else (0.5 if points <= q20 else 1.0)

    curves = {"flat_1.0": lambda row: 1.0, "now_entry_scale": lambda row: entry_scale(row["score_now"]),
              "now_halt_only": lambda row: 0.0 if row["score_now"] <= HALT_SCORE else 1.0,
              "new_equivalent_entry_scale": lambda row: entry_scale(row["oos_equivalent"]),
              "new_proposed_step": lambda row: proposed_scale(row["oos_points"])}
    proposal_rows = []

    for target, cost in (("target", ROUND_TRIP_COST_PCT), ("devscale_to_close_net", 0.0), ("target_next", ROUND_TRIP_COST_PCT)):
        frame = hourly_oos.dropna(subset=[target])

        for name, curve in curves.items():
            scale = frame.apply(curve, axis=1)
            net = scale * (frame[target] - cost)
            proposal_rows.append({"target": target, "curve": name, "rows": len(frame), "mean_scale": round(scale.mean(), 3),
                                  "zero_share_pct": round(100.0 * (scale == 0).mean(), 1),
                                  "net_pct_per_row": round(net.mean(), 4),
                                  "net_per_exposure_pct": round(net.sum() / scale.sum(), 4) if scale.sum() else np.nan,
                                  "avoided_mean_pct": round(frame.loc[scale == 0, target].mean(), 4) if (scale == 0).any() else np.nan})

    pd.DataFrame(proposal_rows).to_csv(STUDY_DIR / "proposal.tsv", sep="\t", index=False)

    # 8) 엔진 이관 참고용 전체 표본 적합(표본 안 — 검증 수치 아님)
    full = LinearScore(hourly_frame, HOURLY_FEATURES, "ridge")
    full_raw = full.raw(hourly_frame)
    metrics["full_sample_hourly_ridge"] = {
        "features": full.features, "train_ic": {name: round(value, 4) for name, value in full.ic.items()},
        "weights_per_std": {name: round(float(value), 4) for name, value in full.weights.items()},
        "feature_mean": {name: round(float(value), 4) for name, value in full.mean.items()},
        "feature_std": {name: round(float(value), 4) for name, value in full.std.items()},
        "raw_quantiles": {str(q): round(float(np.quantile(full_raw, quantile)), 4) for quantile in (0.05, 0.1, 0.2, 0.5, 0.8)},
        "points_mean": round(full.now_mean, 3), "points_std": round(full.now_std, 3),
        "raw_mean": round(full.raw_mean, 5), "raw_std": round(full.raw_std, 5)}
    metrics["proposal_cut_points"] = {"q10": round(float(q10), 2), "q20": round(float(q20), 2)}
    (STUDY_DIR / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2, default=str),
                                            encoding="utf-8")
    print(json.dumps(metrics, ensure_ascii=False, indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
