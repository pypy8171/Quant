"""스터디 45 — 위기 국면 판정 다시 만들기(A)와 같은 위기 상태 이후 결과 차이 검정(B). 사전등록 PREREG.md.

입력: PYQuant/.index_cache 지수 parquet(코스피·VIX·달러지수·원달러·미국10년물) + raw/ks11_fdr_1995-05_1996-12.csv
산출: metrics.json, daily.csv, episode_quality.csv, sensitivity.csv, events.csv, event_paths.csv, rules.csv
실행: 저장소 루트에서 `py research/studies/45_crisis_regime/run_crisis_regime.py`
"""
from __future__ import annotations

import bisect
import json
import math
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
CACHE = REPO / "PYQuant" / ".index_cache"
SEED = 20261016
N_BOOT = 10_000
N_RANK_BOOT = 1_000
MIN_HISTORY = 500           # 분위수에 필요한 과거 값 개수
ENTER, EXIT = 0.90, 0.70    # S 진입·해제 (사전등록)
FAST_REBOUND = 1.10         # 빠른 해제: 20일 최저 대비 +10%
BEAR_LINE = -0.20           # 정렬일·B1 기준
REARM_GAP = 120             # 정렬일 간 최소 간격(거래일)
HORIZON = 120               # 전략 비교 기간
COST_ROUND = 0.0025         # 왕복 비용(고른 값)
RANK_GAP = 0.03             # 확정 1위 차이
RANK_SHARE = 0.80           # 확정 1위 재표집 일치 비율
BLOCK = 20
RESET_LINE = -0.10          # 감사 뒤 추가: 정렬일을 다시 세려면 낙폭이 이 위로 회복해야 한다
N_TAU_NULL = 2_000
N_POWER = 500


# ─── 데이터 ───────────────────────────────────────────────────────────────
def read_index(name: str) -> pd.DataFrame:
    path = next(CACHE.glob(f"idx_{name}_1985-01-01_2026-08-15_adj.parquet"))
    frame = pd.read_parquet(path)
    frame["date"] = pd.to_datetime(frame["date"])
    frame = frame.sort_values("date").drop_duplicates("date")
    return frame[frame["close"] > 0].reset_index(drop=True)


def load_kospi() -> pd.DataFrame:
    recent = read_index("_KS11")
    early = pd.read_csv(HERE / "raw" / "ks11_fdr_1995-05_1996-12.csv", parse_dates=["date"])
    early = early[early["date"] < recent["date"].min()]
    frame = pd.concat([early, recent[["date", "open", "high", "low", "close"]]], ignore_index=True)
    frame = frame[(frame["open"] > 0) & (frame["close"] > 0)].reset_index(drop=True)
    return frame


def expanding_percentile(values: np.ndarray) -> np.ndarray:
    """값 t의 분위수 = t 이전 값들 중 t보다 작은 비율(같은 값은 절반). 과거 값이 MIN_HISTORY 미만이면 NaN."""
    seen: list[float] = []
    out = np.full(len(values), np.nan)
    for index, value in enumerate(values):
        if np.isnan(value):
            continue
        if len(seen) >= MIN_HISTORY:
            low = bisect.bisect_left(seen, value)
            high = bisect.bisect_right(seen, value)
            out[index] = (low + 0.5 * (high - low)) / len(seen)
        bisect.insort(seen, value)
    return out


def asof_before(kr_dates: pd.Series, frame: pd.DataFrame, column: str) -> np.ndarray:
    """한국 날짜 D보다 앞선 날짜의 마지막 값(같은 날짜는 쓰지 않는다)."""
    left = pd.DataFrame({"date": kr_dates.values})
    right = frame[["date", column]].dropna().sort_values("date")
    merged = pd.merge_asof(left, right, on="date", direction="backward", allow_exact_matches=False)
    return merged[column].to_numpy()


def build_daily() -> pd.DataFrame:
    kospi = load_kospi()
    close = kospi["close"]
    daily = kospi.copy()
    daily["high252"] = close.rolling(252, min_periods=1).max()
    daily["dd"] = close / daily["high252"] - 1
    log_ret = np.log(close).diff()
    daily["vol20"] = log_ret.rolling(20).std() * np.sqrt(252)
    for window_length in (20, 60, 120, 200):
        daily[f"ma{window_length}"] = close.rolling(window_length).mean()

    vix = read_index("_VIX")
    vix["vix_pct"] = expanding_percentile(vix["close"].to_numpy())
    dxy = read_index("DX-Y.NYB")
    dxy["chg20"] = dxy["close"].pct_change(20)
    dxy["fx_pct"] = expanding_percentile(dxy["chg20"].to_numpy())
    krw = read_index("KRW=X")
    krw["chg20"] = krw["close"].pct_change(20)
    krw["fx_pct"] = expanding_percentile(krw["chg20"].to_numpy())
    tnx = read_index("_TNX")
    tnx["chg60"] = tnx["close"].diff(60)

    dates = daily["date"]
    daily["vix"] = asof_before(dates, vix, "close")
    daily["vix_pct"] = asof_before(dates, vix, "vix_pct")
    krw_fx_percentile = asof_before(dates, krw, "fx_pct")
    dxy_fx_percentile = asof_before(dates, dxy, "fx_pct")
    daily["fx_pct"] = np.where(np.isnan(krw_fx_percentile), dxy_fx_percentile, krw_fx_percentile)
    daily["krw_chg20"] = asof_before(dates, krw, "chg20")
    daily["us10y_chg60"] = asof_before(dates, tnx, "chg60")
    daily["dd_pct"] = expanding_percentile((-daily["dd"]).to_numpy())
    daily["vol_pct"] = expanding_percentile(daily["vol20"].to_numpy())
    axes = daily[["dd_pct", "vol_pct", "vix_pct", "fx_pct"]].to_numpy()
    count = np.sum(~np.isnan(axes), axis=1)
    with np.errstate(all="ignore"):
        daily["S"] = np.where(count >= 2, np.nanmean(axes, axis=1), np.nan)
    daily["S_axes"] = count
    return daily


# ─── 판정 규칙 ────────────────────────────────────────────────────────────
def rule_stress(daily: pd.DataFrame, enter: float, exit_: float) -> np.ndarray:
    score = daily["S"].to_numpy()
    close = daily["close"].to_numpy()
    volatility = daily["vol20"].to_numpy()
    low20 = daily["close"].rolling(20, min_periods=1).min().to_numpy()
    on = np.zeros(len(daily), dtype=bool)
    state = False
    armed = True              # 빠른 해제 뒤에는 재무장 조건이 필요
    period_low = np.inf       # 직전 켜짐 구간 최저 종가
    for index in range(len(daily)):
        stress_score = score[index]
        if np.isnan(stress_score):
            on[index] = False
            continue
        if state:
            period_low = min(period_low, close[index])
            fast = index >= 5 and close[index] >= low20[index] * FAST_REBOUND and volatility[index] < volatility[index - 5]
            if stress_score < exit_:
                state, armed = False, True
            elif fast:
                state, armed = False, False
        else:
            if not armed and (stress_score < exit_ or close[index] < period_low):
                armed = True
            if armed and stress_score >= enter:
                state = True
                period_low = close[index]
        on[index] = state
    return on


def rule_bear20(daily: pd.DataFrame) -> np.ndarray:
    close = daily["close"].to_numpy()
    high252 = daily["high252"].to_numpy()
    on = np.zeros(len(daily), dtype=bool)
    state = False
    trough = np.inf
    peak_after = None
    for index, price in enumerate(close):
        if state:
            trough = min(trough, price)
            if price >= trough * 1.20:
                state = False
                peak_after = price
        else:
            reference = high252[index] if peak_after is None else max(peak_after, price)
            if peak_after is not None:
                peak_after = reference
            if price <= reference * (1 + BEAR_LINE):
                state = True
                trough = price
                peak_after = None
        on[index] = state
    return on


def rule_v0(daily: pd.DataFrame) -> np.ndarray:
    close = daily["close"]
    ready = daily["ma200"].notna()
    score = np.where(close > daily["ma200"], 1, -1)
    up = (daily["ma20"] > daily["ma60"]) & (daily["ma60"] > daily["ma120"])
    down = (daily["ma20"] < daily["ma60"]) & (daily["ma60"] < daily["ma120"])
    score = score + np.where(up, 1, np.where(down, -1, 0))
    return (ready & (score <= -2)).to_numpy()


# ─── A. 품질 ──────────────────────────────────────────────────────────────
def locate_episodes(daily: pd.DataFrame) -> pd.DataFrame:
    table = pd.read_csv(HERE / "episodes.csv")
    rows = []
    for record in table.itertuples():
        mask = (daily["date"] >= record.search_start) & (daily["date"] <= record.search_end)
        window = daily[mask]
        trough_index = window["close"].idxmin()
        peak_index = window.loc[:trough_index, "close"].idxmax()
        rows.append({
            "key": record.key, "name": record.name, "grade": record.grade,
            "peak_index": int(peak_index), "trough_index": int(trough_index),
            "peak_date": daily.at[peak_index, "date"].strftime("%Y-%m-%d"),
            "trough_date": daily.at[trough_index, "date"].strftime("%Y-%m-%d"),
            "peak_close": round(float(daily.at[peak_index, "close"]), 2),
            "trough_close": round(float(daily.at[trough_index, "close"]), 2),
            "maxdd": round(float(daily.at[trough_index, "close"] / daily.at[peak_index, "close"] - 1) * 100, 1),
        })
    return pd.DataFrame(rows)


def quality(daily: pd.DataFrame, on: np.ndarray, episodes: pd.DataFrame, label: str):
    close = daily["close"].to_numpy()
    day_count = len(daily)
    rows = []
    for record in episodes.itertuples():
        peak, trough = record.peak_index, record.trough_index
        end = min(day_count - 1, trough + 60)
        span = np.flatnonzero(on[peak:end + 1])
        row = {"rule": label, "key": record.key, "grade": record.grade, "carried": bool(peak > 0 and on[peak - 1]),
               "new_entry": bool((on[peak:end + 1] & ~on[peak - 1:end]).any()) if peak > 0 else bool(on[peak:end + 1].any())}
        if len(span) == 0:
            row.update(entry_date=None, entry_lag=None, entry_dd=None, exit_date=None, exit_lag=None, exit_rebound=None,
                       on_at_trough=False, missed=True)
            rows.append(row)
            continue
        first = peak + int(span[0])
        row.update(entry_date=daily.at[first, "date"].strftime("%Y-%m-%d"), entry_lag=first - peak,
                   entry_dd=round((close[first] / close[peak] - 1) * 100, 1), missed=False)
        # 바닥을 덮은 켜짐 구간, 없으면 바닥 전 마지막 켜짐 구간
        anchor = trough if on[trough] else None
        if anchor is None:
            before = np.flatnonzero(on[peak:trough + 1])
            anchor = peak + int(before[-1]) if len(before) else None
        row["on_at_trough"] = bool(on[trough])
        if anchor is None:
            row.update(exit_date=None, exit_lag=None, exit_rebound=None)
        else:
            stop = anchor
            while stop + 1 < day_count and on[stop + 1]:
                stop += 1
            off = stop + 1 if stop + 1 < day_count else None
            if off is None:
                row.update(exit_date=None, exit_lag=None, exit_rebound=None)
            else:
                row.update(exit_date=daily.at[off, "date"].strftime("%Y-%m-%d"), exit_lag=off - trough,
                           exit_rebound=round((close[off] / close[trough] - 1) * 100, 1))
        rows.append(row)
    per_episode = pd.DataFrame(rows)

    inside = np.zeros(day_count, dtype=bool)
    adjust = np.zeros(day_count, dtype=bool)
    for record in episodes.itertuples():
        inside[record.peak_index:min(day_count, record.trough_index + 61)] = True
        if record.grade == "조정":
            adjust[record.peak_index:min(day_count, record.trough_index + 61)] = True
    valid = daily["S"].notna().to_numpy()      # 세 규칙 모두 S가 판정 가능한 기간에서 비교
    years = valid.sum() / 252
    starts = np.flatnonzero(on[1:] & ~on[:-1] & valid[1:]) + 1

    open_ = daily["open"].to_numpy()
    next_ret = np.full(day_count, np.nan)
    next_ret[:-2] = open_[2:] / open_[1:-1] - 1    # 판정 D → D+1 시가에 체결 → D+2 시가까지
    hold = valid & ~np.isnan(next_ret)
    strategy_returns = np.where(on, 0.0, next_ret)
    equity = np.cumprod(1 + strategy_returns[hold])
    bh = np.cumprod(1 + next_ret[hold])

    def mdd(curve):
        return float(np.min(curve / np.maximum.accumulate(curve) - 1))

    crisis = per_episode[per_episode["grade"] == "위기"]
    summary = {
        "rule": label,
        "missed_crisis": int(crisis["missed"].sum()),
        "missed_strict": int((~crisis["new_entry"]).sum()),
        "carried_crisis": int(crisis["carried"].sum()),
        "crisis_count": int(len(crisis)),
        "entry_dd_median": float(crisis["entry_dd"].median()),
        "entry_lag_median": float(crisis["entry_lag"].median()),
        "exit_lag_median": float(crisis["exit_lag"].dropna().median()) if crisis["exit_lag"].notna().any() else None,
        "exit_rebound_median": float(crisis["exit_rebound"].dropna().median()) if crisis["exit_rebound"].notna().any() else None,
        "false_days_per_year": round(float((on & valid & ~inside).sum() / years), 1),
        "adjust_days_per_year": round(float((on & valid & adjust).sum() / years), 1),
        "on_share": round(float((on & valid).sum() / valid.sum()) * 100, 1),
        "entries_per_year": round(len(starts) / years, 2),
        "cagr": round((equity[-1] ** (252 / len(equity)) - 1) * 100, 2),
        "mdd": round(mdd(equity) * 100, 1),
        "bh_cagr": round((bh[-1] ** (252 / len(bh)) - 1) * 100, 2),
        "bh_mdd": round(mdd(bh) * 100, 1),
        "neg_2017_days": int((on & (daily["date"].dt.year == 2017).to_numpy()).sum()),
        "neg_2021_days": int((on & ((daily["date"] >= "2021-07-01") & (daily["date"] <= "2021-10-31")).to_numpy()).sum()),
    }
    return per_episode, summary


# ─── B. 같은 상태 이후 ─────────────────────────────────────────────────────
def find_events(daily: pd.DataFrame, line: float, mode: str = "state") -> list[int]:
    """mode="gap": 사전등록 규칙(직전 정렬일과 120거래일 이상).
    mode="state": 감사 뒤 바꾼 규칙 — 낙폭이 RESET_LINE 위로 회복한 뒤에만 다시 센다.
    mode="fresh": state 중 직전 120거래일에 line보다 깊은 낙폭이 없던 날만(반등 중 재돌파 제외)."""
    dd = daily["dd"].to_numpy()
    events = []
    armed = True
    for index in range(1, len(dd)):
        if mode != "gap" and dd[index] > RESET_LINE:
            armed = True
        if dd[index] <= line < dd[index - 1]:
            if mode == "gap":
                if not events or index - events[-1] >= REARM_GAP:
                    events.append(index)
            elif armed:
                events.append(index)
                armed = False
    if mode == "fresh":
        events = [event_index for event_index in events if dd[max(0, event_index - 120):event_index].min() > line]
    return [event_index for event_index in events if event_index + 1 + HORIZON < len(dd)]


def newey_west_mean(returns: np.ndarray, lags: int = 5) -> tuple[float, float]:
    """일수익 평균과 자기상관을 보정한 표준오차(Newey–West)."""
    centered = returns - returns.mean()
    sample_count = len(returns)
    variance = float(centered @ centered) / sample_count
    for lag in range(1, lags + 1):
        weight = 1 - lag / (lags + 1)
        variance += 2 * weight * float(centered[lag:] @ centered[:-lag]) / sample_count
    return float(returns.mean()), float(np.sqrt(max(variance, 1e-12) / sample_count))


def heterogeneity_statistic(episodes: list[np.ndarray]) -> tuple[float, float]:
    """Cochran Q(에피소드별 평균이 같은가)와 DerSimonian–Laird τ(일수익 평균의 에피소드 간 표준편차)."""
    estimates = np.array([newey_west_mean(episode_returns) for episode_returns in episodes])
    means, errors = estimates[:, 0], estimates[:, 1]
    weights = 1 / errors ** 2
    pooled = float((weights * means).sum() / weights.sum())
    cochran_statistic = float((weights * (means - pooled) ** 2).sum())
    scaling_factor = float(weights.sum() - (weights ** 2).sum() / weights.sum())
    tau2 = max(0.0, (cochran_statistic - (len(episodes) - 1)) / scaling_factor)
    return cochran_statistic, float(np.sqrt(tau2))


def chi2_survival(value: float, dof: int) -> float:
    """χ² 꼬리확률 1 − P(dof/2, value/2). 하부 불완전 감마 급수(scipy 없이)."""
    shape, half = dof / 2, value / 2
    term = 1 / shape
    total = term
    for step in range(1, 500):
        term *= half / (shape + step)
        total += term
        if term < total * 1e-12:
            break

    lower = total * math.exp(-half + shape * math.log(half) - math.lgamma(shape))
    return max(0.0, 1 - lower)


def tau_test(pooled: list[np.ndarray], random_generator: np.random.Generator):
    """검정 2(감사 뒤 추가): Q의 귀무분포를 세 가지로 만든다. 귀무 구성에 따라 p가 갈려서(재감사 지적) 셋을 다 남긴다.
    global: 모은 일수익에서 전체 평균 하나만 빼고 섞음(에피소드 평균 차이가 블록에 남아 보수적).
    within: 에피소드마다 자기 평균을 빼고 자기 안에서만 섞음(평균만 같게, 변동성은 유지. 약간 덜 보수적).
    chi2: Q ~ χ²(k−1) 근사. 검정력은 within 귀무 기준."""
    observed_statistic, observed_tau = heterogeneity_statistic(pooled)
    centre = float(np.concatenate(pooled).mean())
    count = len(pooled)
    pool = np.concatenate(pooled)
    pool = pool - pool.mean()
    residuals = [(episode_returns - episode_returns.mean()) * np.sqrt(len(episode_returns) / (len(episode_returns) - 1))
                 for episode_returns in pooled]
    null_global = np.array([heterogeneity_statistic([centre + synthetic_path(pool, random_generator, HORIZON) for _ in range(count)])[0]
                            for _ in range(N_TAU_NULL)])
    null_within = np.array([heterogeneity_statistic([centre + synthetic_path(episode_returns, random_generator, HORIZON) for episode_returns in residuals])[0]
                            for _ in range(N_TAU_NULL)])
    critical = float(np.percentile(null_within, 95))
    power = {}
    for spread in (0.05, 0.10, 0.15, 0.20):     # 에피소드 간 60일 평균 차이의 표준편차
        hits = 0
        for _ in range(N_POWER):
            shifts = random_generator.normal(0, spread / 60, count)
            statistic, _ = heterogeneity_statistic([centre + shift + synthetic_path(residual, random_generator, HORIZON) for shift, residual in zip(shifts, residuals)])
            hits += statistic >= critical
        power[f"{int(spread * 100)}"] = round(hits / N_POWER, 3)
    p_values = {"global": round(float((null_global >= observed_statistic).mean()), 4),
                "within": round(float((null_within >= observed_statistic).mean()), 4),
                "chi2": round(chi2_survival(observed_statistic, count - 1), 4)}
    return {"q": round(observed_statistic, 2), "q_null_p95": round(critical, 2), "p_by_null": p_values,
            "p": max(p_values.values()), "p_min": min(p_values.values()),
            "tau60": round(observed_tau * 60 * 100, 2), "power": power}


def run_rules(open_path: np.ndarray, close_path: np.ndarray, ma20_path: np.ndarray, start_close: float, start_ma20: float):
    """open_path[0]=D+1 시가 … open_path[H]=D+1+H 시가. close_path[day]=D+1+day 종가. 반환: 보유·현금·20일선 수익."""
    horizon = len(open_path) - 1
    buy_hold = open_path[-1] / open_path[0] - 1 - COST_ROUND
    position = start_close > start_ma20
    equity = 1.0
    trades = 1 if position else 0
    for day in range(horizon):
        if position:
            equity *= open_path[day + 1] / open_path[day]
        wants_position = close_path[day] > ma20_path[day]
        if wants_position != position:
            trades += 1
            position = wants_position
    if position:
        trades += 1     # 끝에 정리
    trend = equity * (1 - COST_ROUND / 2) ** trades - 1
    return {"보유": buy_hold, "현금": 0.0, "20일선 추세": trend}


def synthetic_path(returns: np.ndarray, random_generator: np.random.Generator, length: int) -> np.ndarray:
    blocks = []
    while sum(len(block) for block in blocks) < length:
        start = random_generator.integers(0, len(returns))
        blocks.append(np.take(returns, range(start, start + BLOCK), mode="wrap"))
    return np.concatenate(blocks)[:length]


def event_study(daily: pd.DataFrame, line: float, random_generator: np.random.Generator, detail: bool, mode: str = "state"):
    open_ = daily["open"].to_numpy()
    close = daily["close"].to_numpy()
    moving_average_20 = daily["ma20"].to_numpy()
    events = find_events(daily, line, mode)
    dd = daily["dd"].to_numpy()
    rows, paths, rule_rows = [], [], []
    pooled = []
    for event in events:
        base = open_[event + 1]
        segment_open = open_[event + 1:event + 2 + HORIZON]
        segment_close = close[event + 1:event + 1 + HORIZON]
        segment_moving_average = moving_average_20[event + 1:event + 1 + HORIZON]
        daily_ret = segment_open[1:] / segment_open[:-1] - 1
        pooled.append(daily_ret)
        later = close[event + 1:event + 1 + HORIZON]
        bottom = int(np.argmin(later))
        rules = run_rules(segment_open, segment_close, segment_moving_average, close[event], moving_average_20[event])
        row = {
            "date": daily.at[event, "date"].strftime("%Y-%m-%d"),
            "close": round(float(close[event]), 2),
            "high252": round(float(daily.at[event, "high252"]), 2),
            "fresh": bool(dd[max(0, event - 120):event].min() > line),
            "vix": None if np.isnan(daily.at[event, "vix"]) else round(float(daily.at[event, "vix"]), 1),
            "ret20_before": round(float(close[event] / close[event - 20] - 1) * 100, 1),
            "us10y_chg60": None if np.isnan(daily.at[event, "us10y_chg60"]) else round(float(daily.at[event, "us10y_chg60"]), 2),
            "krw_chg20": None if np.isnan(daily.at[event, "krw_chg20"]) else round(float(daily.at[event, "krw_chg20"]) * 100, 1),
            "S": None if np.isnan(daily.at[event, "S"]) else round(float(daily.at[event, "S"]), 2),
            "r20": round(float(segment_open[20] / base - 1) * 100, 1),
            "r60": round(float(segment_open[60] / base - 1) * 100, 1),
            "r120": round(float(segment_open[HORIZON] / base - 1) * 100, 1),
            "further_mdd": round(float(min(0.0, later.min() / base - 1)) * 100, 1),
            "days_to_bottom": bottom + 1,
            "recovered": bool((later >= close[event]).any()),
        }
        for name, value in rules.items():
            row[f"rule_{name}"] = round(value * 100, 1)
        rows.append(row)
        if detail:
            for day in range(HORIZON + 1):
                paths.append({"event": row["date"], "day": day, "ret": round(float(segment_open[day] / base - 1) * 100, 2)})
            # 확정 1위 재표집
            ranked = sorted(rules.items(), key=lambda item: -item[1])
            winner, gap = ranked[0][0], ranked[0][1] - ranked[1][1]
            same = 0
            for _ in range(N_RANK_BOOT):
                synth = synthetic_path(daily_ret, random_generator, HORIZON)
                path_open = base * np.concatenate([[1.0], np.cumprod(1 + synth)])
                path_close = path_open[1:]
                full = np.concatenate([close[max(0, event - 19):event + 1], path_close])
                path_moving_average = pd.Series(full).rolling(20, min_periods=1).mean().to_numpy()[-HORIZON:]
                sim = run_rules(path_open, path_close, path_moving_average, close[event], moving_average_20[event])
                if max(sim, key=sim.get) == winner:
                    same += 1
            rule_rows.append({"event": row["date"], "winner": winner, "gap": round(gap * 100, 1),
                              "boot_same": round(same / N_RANK_BOOT, 3),
                              "confirmed": bool(gap >= RANK_GAP and same / N_RANK_BOOT >= RANK_SHARE)})
    table = pd.DataFrame(rows)

    # 검정 1: 같은 분포 귀무가설
    pool = np.concatenate(pooled)
    observed = float(table["r60"].std(ddof=1))
    sims = np.empty(N_BOOT)
    for boot_index in range(N_BOOT):
        values = [np.prod(1 + synthetic_path(pool, random_generator, 60)) - 1 for _ in range(len(events))]
        sims[boot_index] = np.std(values, ddof=1) * 100
    p_value = float((sims >= observed).mean())
    result = {"line": line, "events": len(events), "r60_std": round(observed, 2),
              "null_std_median": round(float(np.median(sims)), 2), "null_std_p95": round(float(np.percentile(sims, 95)), 2),
              "p": round(p_value, 4), "tau": tau_test(pooled, random_generator)}
    return table, pd.DataFrame(paths), pd.DataFrame(rule_rows), result


def kendall_coefficient(table: pd.DataFrame, names: list[str]) -> float:
    ranks = table[[f"rule_{name}" for name in names]].rank(axis=1, ascending=False).to_numpy()
    event_count, rule_count = ranks.shape
    totals = ranks.sum(axis=0)
    squared_deviation_sum = float(((totals - totals.mean()) ** 2).sum())
    return round(12 * squared_deviation_sum / (event_count ** 2 * (rule_count ** 3 - rule_count)), 3)


def selector_test(table: pd.DataFrame, names: list[str]):
    """미국10년물 60일 변화 > 0 여부로 그룹을 나눠, 나머지 에피소드 평균 최선 규칙을 빠진 에피소드에 적용."""
    group = (table["us10y_chg60"] > 0).to_numpy()
    values = table[[f"rule_{name}" for name in names]].to_numpy()
    picked, fixed = [], []
    for index in range(len(table)):
        rest = np.arange(len(table)) != index
        same = rest & (group == group[index])
        pool = same if same.sum() > 0 else rest
        picked.append(values[index, int(np.argmax(values[pool].mean(axis=0)))])
        fixed.append(values[index, int(np.argmax(values[rest].mean(axis=0)))])
    return {"selector_mean": round(float(np.mean(picked)), 2), "fixed_mean": round(float(np.mean(fixed)), 2),
            "selector_wins": int(np.sum(np.array(picked) > np.array(fixed))),
            "fixed_wins": int(np.sum(np.array(picked) < np.array(fixed)))}


def main() -> None:
    random_generator = np.random.default_rng(SEED)
    daily = build_daily()
    episodes = locate_episodes(daily)

    rules = {"S": rule_stress(daily, ENTER, EXIT), "B1": rule_bear20(daily), "v0": rule_v0(daily)}
    quality_rows, summaries = [], []
    for label, on in rules.items():
        per, summary = quality(daily, on, episodes, label)
        quality_rows.append(per)
        summaries.append(summary)
    sensitivity = []
    for enter in (0.85, 0.90, 0.95):
        for exit_ in (0.60, 0.70, 0.80):
            _, summary = quality(daily, rule_stress(daily, enter, exit_), episodes, f"S {enter:.2f}/{exit_:.2f}")
            summary.update(enter=enter, exit=exit_)
            sensitivity.append(summary)

    names = ["보유", "현금", "20일선 추세"]
    levels = {}
    primary = None
    runs = [(mode, line) for mode in ("state", "gap") for line in (-0.10, -0.15, -0.20, -0.25)] + [("fresh", BEAR_LINE)]
    for mode, line in runs:
        detail = mode == "state" and line == BEAR_LINE
        table, paths, rank_rows, result = event_study(daily, line, random_generator, detail=detail, mode=mode)
        result["mode"] = mode
        means = table[[f"rule_{name}" for name in names]].mean()
        best_fixed = means.idxmax().replace("rule_", "")
        result["best_fixed"] = best_fixed
        result["kendall_w"] = kendall_coefficient(table, names)
        result["oracle_gap"] = round(float(table[[f"rule_{name}" for name in names]].max(axis=1).mean() - means.max()), 2)
        result["winner_differs"] = int((table[[f"rule_{name}" for name in names]].idxmax(axis=1) != f"rule_{best_fixed}").sum())
        result["selector"] = selector_test(table, names)
        levels[f"{mode}{int(line * 100)}"] = result
        if detail:
            primary = (table, paths, rank_rows)

    table, paths, rank_rows = primary
    table = table.merge(rank_rows.rename(columns={"event": "date"}), on="date")
    main_result = levels[f"state{int(BEAR_LINE * 100)}"]
    main_result["confirmed_winner_differs"] = int(((table["winner"] != main_result["best_fixed"]) & table["confirmed"]).sum())
    main_result["confirmed_count"] = int(table["confirmed"].sum())

    daily_out = daily[["date", "open", "close", "ma20", "ma60", "ma120", "ma200", "dd", "vol20", "vix", "S",
                       "dd_pct", "vol_pct", "vix_pct", "fx_pct"]].copy()
    for label, on in rules.items():
        daily_out[f"on_{label}"] = on.astype(int)
    daily_out["date"] = daily_out["date"].dt.strftime("%Y-%m-%d")
    daily_out.round(4).to_csv(HERE / "daily.csv", index=False)
    pd.concat(quality_rows).merge(episodes.drop(columns=["peak_index", "trough_index"]), on=["key", "grade"]) \
        .to_csv(HERE / "episode_quality.csv", index=False)
    pd.DataFrame(sensitivity).to_csv(HERE / "sensitivity.csv", index=False)
    table.to_csv(HERE / "events.csv", index=False)
    paths.to_csv(HERE / "event_paths.csv", index=False)

    # 사전등록 판정(gap 정렬, 검정 1, 깊이 네 단 중 셋)과 감사 뒤 판정(τ² 검정 귀무 세 가지 모두, 정렬 방식 셋 중 둘)을 둘 다 남긴다
    depth_hits = sum(1 for key, level_result in levels.items() if key.startswith("gap") and level_result["p"] < 0.05)
    align_hits = sum(1 for key in ("state-20", "gap-20", "fresh-20") if levels[key]["tau"]["p"] < 0.05)
    differ = "같은 낙폭 상태 이후 경로 차이가 무작위 변동보다 컸다(에피소드 %d개, 탐색적)"
    same = "차이는 보이나 우연과 구별되지 않는다"
    prereg_result = levels[f"gap{int(BEAR_LINE * 100)}"]
    verdict = {"prereg": (differ % prereg_result["events"]) if prereg_result["p"] < 0.05 and depth_hits >= 3 else same,
               "audit": (differ % main_result["events"]) if main_result["tau"]["p"] < 0.05 and align_hits >= 2 else same}
    metrics = {"detect": summaries, "levels": levels, "verdict": verdict, "seed": SEED,
               "data_start": daily["date"].min().strftime("%Y-%m-%d"), "data_end": daily["date"].max().strftime("%Y-%m-%d"),
               "S_start": daily.loc[daily["S"].notna(), "date"].min().strftime("%Y-%m-%d")}
    (HERE / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(metrics, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
