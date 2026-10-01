"""스터디 26 — 국면 점수 과거 복원과 정지선·청산선 격자.

엔진 규칙 원본: Quant/src/regime/RegimeFeed.cpp
  kVotingSymbols(8개, 방향·warn·strong), vote_for(강도 1/2), kIntraSymbols(NQ_F 0.3·TNX10 0.6 장초 대비 방향표,
  기준점은 09시 첫 계산). 점수 = 8개 표 + 장초 대비 표. 범위 −18~+18.

두 갈래로 복원한다. 둘 다 각 시각에 알 수 있던 값만 쓴다(미국 지표는 그 시각까지 끝난 세션·봉).
  hourly (2024-05~, Yahoo 1시간봉 730일 한도) — 엔진에 가장 가깝다. 표본 시각 08:50(개장 전, 한국 0표)·09:00(네이버 시가)·
    10~15시 정각·15:30. 각 시각 값:
      코스피·코스닥  Yahoo ^KS11·^KQ11 1시간봉 종가(그 시각에 끝난 봉) / 네이버 전일 종가.
      NQ_F·ES_F      현물 ^IXIC·^GSPC 직전 세션 등락 + 선물 1시간봉 종가 / 직전 선물 일봉 종가(정산 근사).
      WTI            CL=F 1시간봉 종가 / 직전 선물 일봉 종가.
      USDKRW         KRW=X 1시간봉 종가 / 전날 23:00 UTC(런던 하루 시작, 08:00 KST) 시가.
      TNX10·VIX      직전 미국 세션 등락(한국 장중에는 안 움직인다 — 실측 로그도 같다).
      장초 대비      NQ=F 09:00 KST 값 대비 0.3% 이상이면 ±1. TNX10은 한국 장중 거래가 없어 0.
  daily (2015~, 일봉) — 근사. 미국 쪽은 위와 같되 선물은 '다음 봉 시가(18:00 ET) / 직전 종가'로 개장 전 값만 안다.
    USDKRW는 개장 전·시가 0표, 하루 최저 점수에는 KRW=X 런던 일봉 고가/시가(달러 강세 쪽 최악, 장 뒤 시간 포함 — 비관 근사).
    한국 지수는 네이버 시가·저가. 하루 경로는 '시가 → 저가 직선, 두 지수가 같은 비율로'로 본다.
    장초 대비 표와 선물의 한국 장중 변동은 0 — 이 갈래의 최저 점수는 엔진보다 덜 내려갈 수 있다(hourly와 대조해 잰다).

청산 모형: 보유 중 표본 점수가 X 이하가 되는 첫 표본에서 그 시각 지수값으로 판다(개장 전에 이미 X 이하면 시가).
되사기는 다음 거래일 개장 전 점수 > X 일 때 시가. 왕복 비용 0.215%를 매도 때 뗀다.
정지 모형: 점수가 H 이하가 된 첫 표본에서 샀다면(= 정지가 막은 매수) 그 뒤 당일 종가·다음날 종가·5일 뒤 종가 수익.

사용: py -X utf8 research/studies/26_regime_threshold_history/replay_regime_score.py [--refresh]
산출: 같은 폴더 daily_scores.tsv, hourly_scores.tsv, live_check.tsv, score_bins.tsv, liquidate_grid.tsv,
      halt_grid.tsv, metrics.json, raw/*.parquet(내려받은 시세 캐시).
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import requests

STUDY_DIR = Path(__file__).resolve().parent
RAW_DIR = STUDY_DIR / "raw"
REPO_ROOT = STUDY_DIR.parents[2]
START = "2014-12-01"
ROUND_TRIP_COST = 0.00215
KST = "Asia/Seoul"
THRESHOLDS = list(range(-3, -15, -1))

# RegimeFeed.cpp kVotingSymbols 그대로: (방향, warn, strong)
VOTING_RULES = {
    "KOSPI": (+1, 0.7, 1.5),
    "KOSDAQ": (+1, 0.8, 1.8),
    "NQ_F": (+1, 0.4, 0.9),
    "ES_F": (+1, 0.4, 0.9),
    "TNX10": (-1, 1.5, 3.0),
    "VIX": (-1, 4.0, 9.0),
    "USDKRW": (-1, 0.4, 0.9),
    "WTI": (-1, 2.0, 4.0),
}
INTRA_NQ_WARN = 0.3
FDR_DAILY = {"IXIC": "IXIC", "GSPC": "US500", "NQF": "NQ=F", "ESF": "ES=F", "TNX": "^TNX", "VIX": "VIX",
             "CLF": "CL=F"}
YAHOO_HOURLY = {"KS11": "^KS11", "KQ11": "^KQ11", "NQF": "NQ=F", "ESF": "ES=F", "CLF": "CL=F", "KRW": "KRW=X"}
HEADERS = {"User-Agent": "Mozilla/5.0"}


def vote_for(key: str, percent: float) -> int:
    """RegimeFeed.cpp vote_for와 같다. percent가 NaN이면 0표."""
    if percent is None or not np.isfinite(percent):
        return 0

    direction, warn, strong = VOTING_RULES[key]
    magnitude = abs(percent)
    strength = 2 if magnitude >= strong else (1 if magnitude >= warn else 0)

    if strength == 0:
        return 0

    return direction * (1 if percent > 0 else -1) * strength


# ---------------------------------------------------------------- 시세 적재


def load_naver_index(symbol: str, refresh: bool) -> pd.DataFrame:
    cache = RAW_DIR / f"naver_{symbol}.parquet"

    if cache.exists() and not refresh:
        return pd.read_parquet(cache)

    url = ("https://api.finance.naver.com/siseJson.naver?symbol=" + symbol + "&requestType=1&startTime=" +
           START.replace("-", "") + "&endTime=20261231&timeframe=day")
    rows = json.loads(requests.get(url, timeout=30, headers=HEADERS).text.replace("'", '"'))
    frame = pd.DataFrame(rows[1:], columns=["date", "open", "high", "low", "close", "volume", "foreign"])
    frame["date"] = pd.to_datetime(frame["date"], format="%Y%m%d")
    frame = frame.set_index("date")[["open", "high", "low", "close"]].astype(float)
    frame = frame[frame["close"] > 0]
    frame.to_parquet(cache)
    return frame


def load_fdr(name: str, refresh: bool) -> pd.DataFrame:
    cache = RAW_DIR / f"fdr_{name}.parquet"

    if cache.exists() and not refresh:
        return pd.read_parquet(cache)

    import FinanceDataReader as fdr

    frame = fdr.DataReader(FDR_DAILY[name], START).rename(columns=str.lower)[["open", "high", "low", "close"]]
    frame = frame.astype(float)
    frame = frame[frame["close"].notna() & (frame["open"] != 0)]
    frame.index = pd.to_datetime(frame.index).tz_localize(None).normalize()
    frame.to_parquet(cache)
    return frame


def load_yahoo(symbol: str, interval: str, chart_range: str, name: str, refresh: bool) -> pd.DataFrame:
    """Yahoo chart API 직접. 인덱스는 봉 시작 UTC. 진행 중인(정각이 아닌) 봉은 버린다."""
    cache = RAW_DIR / f"yahoo_{name}_{interval}.parquet"

    if cache.exists() and not refresh:
        return pd.read_parquet(cache)

    body = requests.get("https://query1.finance.yahoo.com/v8/finance/chart/" + symbol,
                        params={"range": chart_range, "interval": interval}, headers=HEADERS, timeout=60).json()
    result = body["chart"]["result"][0]
    quote = result["indicators"]["quote"][0]
    frame = pd.DataFrame({column: quote[column] for column in ("open", "high", "low", "close")},
                         index=pd.to_datetime(result["timestamp"], unit="s", utc=True)).astype(float)
    frame = frame.dropna()
    frame = frame[(frame.index.minute == 0) & (frame.index.second == 0)]
    frame.to_parquet(cache)
    return frame


def last_session_percent(frame: pd.DataFrame, before: pd.Timestamp):
    """before(달력일) 전 마지막 일봉의 종가 등락 %와 그 날짜."""
    earlier = frame.loc[frame.index < before]

    if len(earlier) < 2:
        return np.nan, None

    return (earlier["close"].iloc[-1] / earlier["close"].iloc[-2] - 1.0) * 100.0, earlier.index[-1]


def last_close_before(frame: pd.DataFrame, before: pd.Timestamp) -> float:
    earlier = frame.loc[frame.index < before]
    return float(earlier["close"].iloc[-1]) if len(earlier) else np.nan


def futures_since_settle_at_open(frame: pd.DataFrame, korean_day: pd.Timestamp) -> float:
    """직전 선물 일봉 종가 → 다음 봉 시가(18:00 ET, 한국 07~08시) %. 다음 봉 날짜가 한국 날짜보다 늦으면 0."""
    earlier = frame.loc[frame.index < korean_day]
    later = frame.loc[frame.index >= korean_day]

    if earlier.empty or later.empty or later.index[0] != korean_day:
        return 0.0

    return (later["open"].iloc[0] / earlier["close"].iloc[-1] - 1.0) * 100.0


# ---------------------------------------------------------------- 하루 표본 구조
# 하루 = {"pre": 개장 전 점수, "samples": [(시각 표지, 점수, 코스피 값, 코스닥 값), ...], 전일 종가·시가·종가}


def build_daily(kospi, kosdaq, us, krw_daily) -> tuple[pd.DataFrame, dict]:
    days = kospi.index.intersection(kosdaq.index)
    days = days[days >= pd.Timestamp("2015-01-01")]
    # Yahoo KRW=X 일봉은 런던 하루(23:00 UTC 시작) — 한국 날짜로 붙인다.
    krw = krw_daily.copy()
    krw.index = (krw.index + pd.Timedelta(hours=1)).tz_convert(None).normalize()
    krw = krw[~krw.index.duplicated(keep="last")]
    records = []
    structures = {}

    for day in days:
        previous_kospi = kospi["close"].loc[:day].iloc[-2]
        previous_kosdaq = kosdaq["close"].loc[:day].iloc[-2]
        nasdaq_percent, _ = last_session_percent(us["IXIC"], day)
        sp_percent, _ = last_session_percent(us["GSPC"], day)
        values = {
            "NQ_F": nasdaq_percent + futures_since_settle_at_open(us["NQF"], day),
            "ES_F": sp_percent + futures_since_settle_at_open(us["ESF"], day),
            "TNX10": last_session_percent(us["TNX"], day)[0],
            "VIX": last_session_percent(us["VIX"], day)[0],
            "WTI": futures_since_settle_at_open(us["CLF"], day),
        }
        us_votes = sum(vote_for(key, value) for key, value in values.items())
        fx_worst = (krw.loc[day, "high"] / krw.loc[day, "open"] - 1.0) * 100.0 if day in krw.index else np.nan
        own_kospi = kospi.loc[day]
        own_kosdaq = kosdaq.loc[day]
        samples = []

        for fraction in np.linspace(0.0, 1.0, 201):
            kospi_value = own_kospi["open"] + fraction * (own_kospi["low"] - own_kospi["open"])
            kosdaq_value = own_kosdaq["open"] + fraction * (own_kosdaq["low"] - own_kosdaq["open"])
            fx_vote = 0 if fraction == 0.0 else vote_for("USDKRW", fraction * fx_worst)
            score = us_votes + fx_vote + vote_for("KOSPI", (kospi_value / previous_kospi - 1.0) * 100.0) + \
                vote_for("KOSDAQ", (kosdaq_value / previous_kosdaq - 1.0) * 100.0)
            samples.append(("open" if fraction == 0.0 else f"path{fraction:.3f}", score, kospi_value, kosdaq_value))

        close_score = us_votes + vote_for("KOSPI", (own_kospi["close"] / previous_kospi - 1.0) * 100.0) + \
            vote_for("KOSDAQ", (own_kosdaq["close"] / previous_kosdaq - 1.0) * 100.0)
        structures[day] = {"pre": us_votes, "samples": samples}
        records.append({"date": day, **{key: round(value, 3) for key, value in values.items()},
                        "USDKRW_worst": round(fx_worst, 3), "score_pre": us_votes, "score_open": samples[0][1],
                        "score_min": min(sample[1] for sample in samples), "score_close": close_score,
                        "kospi_prev_close": previous_kospi, "kospi_open": own_kospi["open"],
                        "kospi_low": own_kospi["low"], "kospi_close": own_kospi["close"],
                        "kosdaq_prev_close": previous_kosdaq, "kosdaq_open": own_kosdaq["open"],
                        "kosdaq_low": own_kosdaq["low"], "kosdaq_close": own_kosdaq["close"]})

    return pd.DataFrame(records).set_index("date"), structures


def price_at(frame: pd.DataFrame, moment_utc: pd.Timestamp, bar_minutes: int = 60):
    """moment까지 끝난 마지막 1시간봉 종가(봉 끝 = 시작 + 60분)."""
    ended = frame.loc[frame.index + pd.Timedelta(minutes=bar_minutes) <= moment_utc]
    return float(ended["close"].iloc[-1]) if len(ended) else np.nan


def build_hourly(kospi, kosdaq, us, hourly) -> tuple[pd.DataFrame, dict]:
    start = max(frame.index[0] for frame in hourly.values()).tz_convert(KST).normalize().tz_localize(None)
    start = start + pd.Timedelta(days=1)
    days = kospi.index.intersection(kosdaq.index)
    days = days[days >= start]
    korean_hourly = {}

    for name in ("KS11", "KQ11"):
        frame = hourly[name].copy()
        local = frame.index.tz_convert(KST)
        frame["day"] = local.tz_localize(None).normalize()
        frame["end"] = [min(stamp + pd.Timedelta(hours=1), stamp.normalize() + pd.Timedelta(hours=15, minutes=30))
                        for stamp in local]
        korean_hourly[name] = frame

    records = []
    structures = {}

    for day in days:
        previous_kospi = kospi["close"].loc[:day].iloc[-2]
        previous_kosdaq = kosdaq["close"].loc[:day].iloc[-2]
        nasdaq_percent, _ = last_session_percent(us["IXIC"], day)
        sp_percent, _ = last_session_percent(us["GSPC"], day)
        tnx_vote = vote_for("TNX10", last_session_percent(us["TNX"], day)[0])
        vix_vote = vote_for("VIX", last_session_percent(us["VIX"], day)[0])
        settle = {name: last_close_before(us[name], day) for name in ("NQF", "ESF", "CLF")}
        day_local = pd.Timestamp(day).tz_localize(KST)
        fx_base_frame = hourly["KRW"].loc[hourly["KRW"].index <= (day_local - pd.Timedelta(hours=1)).tz_convert("UTC")]
        fx_base_frame = fx_base_frame[fx_base_frame.index.hour == 23]
        fx_base = float(fx_base_frame["open"].iloc[-1]) if len(fx_base_frame) else np.nan
        nq_reference = price_at(hourly["NQF"], (day_local + pd.Timedelta(hours=9)).tz_convert("UTC"))

        def us_votes_at(moment_local, use_intra: bool):
            moment = moment_local.tz_convert("UTC")
            nq_price = price_at(hourly["NQF"], moment)
            es_price = price_at(hourly["ESF"], moment)
            oil_price = price_at(hourly["CLF"], moment)
            fx_price = price_at(hourly["KRW"], moment)
            parts = {
                "NQ_F": nasdaq_percent + (nq_price / settle["NQF"] - 1.0) * 100.0,
                "ES_F": sp_percent + (es_price / settle["ESF"] - 1.0) * 100.0,
                "WTI": (oil_price / settle["CLF"] - 1.0) * 100.0,
                "USDKRW": (fx_price / fx_base - 1.0) * 100.0,
            }
            votes = tnx_vote + vix_vote + sum(vote_for(key, value) for key, value in parts.items())
            if use_intra and np.isfinite(nq_reference):
                intra = (nq_price / nq_reference - 1.0) * 100.0
                votes += (1 if intra > 0 else -1) if abs(intra) >= INTRA_NQ_WARN else 0
            return votes, parts

        pre_votes, pre_parts = us_votes_at(day_local + pd.Timedelta(hours=8, minutes=50), False)
        open_votes, _ = us_votes_at(day_local + pd.Timedelta(hours=9), True)
        own_kospi = kospi.loc[day]
        own_kosdaq = kosdaq.loc[day]
        samples = [("09:00", open_votes + vote_for("KOSPI", (own_kospi["open"] / previous_kospi - 1) * 100) +
                    vote_for("KOSDAQ", (own_kosdaq["open"] / previous_kosdaq - 1) * 100),
                    own_kospi["open"], own_kosdaq["open"])]
        kospi_bars = korean_hourly["KS11"][korean_hourly["KS11"]["day"] == day]
        kosdaq_bars = korean_hourly["KQ11"][korean_hourly["KQ11"]["day"] == day]

        for end in sorted(set(kospi_bars["end"]) & set(kosdaq_bars["end"])):
            kospi_bar = kospi_bars[kospi_bars["end"] == end].iloc[-1]
            kosdaq_bar = kosdaq_bars[kosdaq_bars["end"] == end].iloc[-1]
            votes, _ = us_votes_at(end, True)
            # 봉 안 바닥까지 — 3분 주기 엔진은 봉 안 저가 근처를 본다. 직전 표본 값에서 두 지수 저가까지 직선 10걸음으로
            #  가며, 선을 처음 넘는 걸음의 값이 매도가가 된다(저가 자체로 팔면 지나치게 비관).
            start_kospi, start_kosdaq = samples[-1][2], samples[-1][3]
            for fraction in np.linspace(0.1, 1.0, 10):
                kospi_value = start_kospi + fraction * (min(kospi_bar["low"], start_kospi) - start_kospi)
                kosdaq_value = start_kosdaq + fraction * (min(kosdaq_bar["low"], start_kosdaq) - start_kosdaq)
                path_score = votes + vote_for("KOSPI", (kospi_value / previous_kospi - 1) * 100) + \
                    vote_for("KOSDAQ", (kosdaq_value / previous_kosdaq - 1) * 100)
                samples.append((end.strftime("%H:%M") + "~", path_score, kospi_value, kosdaq_value))
            close_score = votes + vote_for("KOSPI", (kospi_bar["close"] / previous_kospi - 1) * 100) + \
                vote_for("KOSDAQ", (kosdaq_bar["close"] / previous_kosdaq - 1) * 100)
            samples.append((end.strftime("%H:%M"), close_score, kospi_bar["close"], kosdaq_bar["close"]))

        if len(samples) < 4 or not np.isfinite(fx_base):
            continue

        structures[day] = {"pre": pre_votes, "samples": samples}
        worst = min(samples, key=lambda sample: sample[1])
        records.append({"date": day, **{key: round(value, 3) for key, value in pre_parts.items()},
                        "score_pre": pre_votes, "score_open": samples[0][1], "score_min": worst[1],
                        "score_min_at": worst[0], "score_close": samples[-1][1],
                        "kospi_prev_close": previous_kospi, "kospi_open": own_kospi["open"],
                        "kospi_low": own_kospi["low"], "kospi_close": own_kospi["close"],
                        "kosdaq_prev_close": previous_kosdaq, "kosdaq_open": own_kosdaq["open"],
                        "kosdaq_low": own_kosdaq["low"], "kosdaq_close": own_kosdaq["close"]})

    return pd.DataFrame(records).set_index("date"), structures


# ---------------------------------------------------------------- 평가


def first_trigger(structure: dict, threshold: int):
    """(표본 순번, 표본) — 개장 전 점수가 이미 X 이하면 시가 표본."""
    if structure["pre"] <= threshold:
        return 0, structure["samples"][0]

    for position, sample in enumerate(structure["samples"]):
        if sample[1] <= threshold:
            return position, sample

    return None


def score_bins(frame: pd.DataFrame, score_column: str, source: str) -> pd.DataFrame:
    edges = [(-99, -11), (-10, -9), (-8, -7), (-6, -5), (-4, -3), (-2, 0), (1, 3), (4, 99)]
    frame = frame.copy()
    for name in ("kospi", "kosdaq"):
        frame[name + "_ret"] = frame[name + "_close"] / frame[name + "_prev_close"] - 1.0
        frame[name + "_next_ret"] = frame[name + "_ret"].shift(-1)
        frame[name + "_drawdown"] = frame[name + "_low"] / frame[name + "_prev_close"] - 1.0
    rows = []
    for low, high in edges:
        subset = frame[(frame[score_column] >= low) & (frame[score_column] <= high)]
        label = f"<={high}" if low == -99 else (f">={low}" if high == 99 else f"{low}..{high}")
        row = {"source": source, "score_kind": score_column, "bin": label, "days": len(subset),
               "share_pct": round(100.0 * len(subset) / len(frame), 2)}
        for name in ("kospi", "kosdaq"):
            mean = (lambda series: round(100 * series.mean(), 3)) if len(subset) else (lambda series: np.nan)
            row[name + "_same_day_pct"] = mean(subset[name + "_ret"])
            row[name + "_low_vs_prev_pct"] = mean(subset[name + "_drawdown"])
            row[name + "_next_day_pct"] = mean(subset[name + "_next_ret"])
            row[name + "_next_day_worst_pct"] = round(100 * subset[name + "_next_ret"].min(), 2) if len(subset) else np.nan
        rows.append(row)
    return pd.DataFrame(rows)


def max_drawdown(equity: np.ndarray) -> float:
    peak = np.maximum.accumulate(equity)
    return float((equity / peak - 1.0).min())


def liquidation_run(frame: pd.DataFrame, structures: dict, index_name: str, threshold: int, rebuy: str = "next_open"):
    """rebuy: next_open(다음날 개장 전 점수 > X면 시가) 또는 same_close(그날 종가에 되산다 — 장중 낙폭만 피하는 경우)."""
    holding = True
    returns = []
    events = 0
    days_out = 0
    column = 2 if index_name == "kospi" else 3

    for day, row in frame.iterrows():
        structure = structures[day]
        start_price = row[index_name + "_prev_close"]

        if not holding:
            if structure["pre"] > threshold:
                holding = True
                start_price = row[index_name + "_open"]
            else:
                days_out += 1
                returns.append(0.0)
                continue

        trigger = first_trigger(structure, threshold)

        if trigger is not None:
            exit_return = trigger[1][column] / start_price - 1.0 - ROUND_TRIP_COST
            events += 1
            if rebuy == "same_close":
                returns.append(exit_return)
                continue
            returns.append(exit_return)
            holding = False
        else:
            returns.append(row[index_name + "_close"] / start_price - 1.0)

    return np.array(returns), events, days_out


def evaluate(frame: pd.DataFrame, structures: dict, source: str):
    periods = {"all": None}
    if source == "daily":
        periods.update({"2015-2019": ("2015", "2019"), "2020-2022": ("2020", "2022"), "2023-2026": ("2023", "2026"),
                        "hourly_window": ("2024-05-09", "2026-12-31")})
    else:
        periods.update({"2024H2-2025": ("2024", "2025"), "2026": ("2026", "2026")})
    grid_rows = []
    for index_name in ("kospi", "kosdaq"):
        for period_name, period in periods.items():
            rows = frame if period is None else frame.loc[period[0]:period[1]]
            hold_equity = np.cumprod((rows[index_name + "_close"] / rows[index_name + "_prev_close"]).values)
            years = len(rows) / 250.0
            for threshold, rebuy in [(line, mode) for mode in ("next_open", "same_close") for line in THRESHOLDS]:
                returns, events, days_out = liquidation_run(rows, structures, index_name, threshold, rebuy)
                equity = np.cumprod(1.0 + returns)
                grid_rows.append({
                    "source": source, "index": index_name, "period": period_name, "rebuy": rebuy,
                    "liquidate_at": threshold,
                    "liquidations": events, "per_year": round(events / years, 1), "days_out": days_out,
                    "hold_total_pct": round(100 * (hold_equity[-1] - 1), 1),
                    "rule_total_pct": round(100 * (equity[-1] - 1), 1),
                    "edge_pct_per_year": round(100 * (np.log(equity[-1]) - np.log(hold_equity[-1])) / years, 2),
                    "cost_pct_per_year": round(100 * events * ROUND_TRIP_COST / years, 2),
                    "hold_mdd_pct": round(100 * max_drawdown(hold_equity), 1),
                    "rule_mdd_pct": round(100 * max_drawdown(equity), 1),
                })

    halt_rows = []
    for index_name in ("kospi", "kosdaq"):
        column = 2 if index_name == "kospi" else 3
        next_close = frame[index_name + "_close"].shift(-1)
        close_5 = frame[index_name + "_close"].shift(-5)
        base_to_close = frame[index_name + "_close"] / frame[index_name + "_open"] - 1.0
        base_next = next_close / frame[index_name + "_open"] - 1.0
        for threshold in THRESHOLDS:
            entries = []
            for day in frame.index:
                trigger = first_trigger(structures[day], threshold)
                if trigger is None:
                    continue
                price = trigger[1][column]
                entries.append({"to_close": frame.loc[day, index_name + "_close"] / price - 1.0,
                                "to_next_close": next_close.loc[day] / price - 1.0,
                                "to_close_5d": close_5.loc[day] / price - 1.0})
            entries = pd.DataFrame(entries, columns=["to_close", "to_next_close", "to_close_5d"])
            halt_rows.append({
                "source": source, "index": index_name, "halt_at": threshold, "days": len(entries),
                "per_year": round(len(entries) / (len(frame) / 250.0), 1),
                "after_trigger_to_close_pct": round(100 * entries["to_close"].mean(), 3),
                "to_next_close_pct": round(100 * entries["to_next_close"].mean(), 3),
                "to_next_close_hit_pct": round(100 * (entries["to_next_close"] > 0).mean(), 1),
                "to_close_5d_pct": round(100 * entries["to_close_5d"].mean(), 3),
                "all_days_open_to_close_pct": round(100 * base_to_close.mean(), 3),
                "all_days_open_to_next_close_pct": round(100 * base_next.mean(), 3),
            })
    return pd.DataFrame(grid_rows), pd.DataFrame(halt_rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--refresh", action="store_true")
    arguments = parser.parse_args()
    RAW_DIR.mkdir(exist_ok=True)
    kospi = load_naver_index("KOSPI", arguments.refresh)
    kosdaq = load_naver_index("KOSDAQ", arguments.refresh)
    us = {name: load_fdr(name, arguments.refresh) for name in FDR_DAILY}
    krw_daily = load_yahoo("KRW=X", "1d", "max", "KRW", arguments.refresh)
    hourly = {name: load_yahoo(symbol, "60m", "730d", name, arguments.refresh) for name, symbol in YAHOO_HOURLY.items()}

    daily, daily_structures = build_daily(kospi, kosdaq, us, krw_daily)
    hourly_frame, hourly_structures = build_hourly(kospi, kosdaq, us, hourly)
    daily.round(3).to_csv(STUDY_DIR / "daily_scores.tsv", sep="\t")
    hourly_frame.round(3).to_csv(STUDY_DIR / "hourly_scores.tsv", sep="\t")

    live_rows = [json.loads(line) for line in open(REPO_ROOT / "logs" / "regime_history.jsonl", encoding="utf-8")]
    live = pd.DataFrame([{"date": pd.Timestamp(record["ts"][:10]), "hhmm": record["ts"][11:16], "score": record["risk_score"]}
                         for record in live_rows if record.get("valid") and "KOSPI" in record.get("vote", {})])
    regular = live[(live["hhmm"] >= "09:00") & (live["hhmm"] <= "15:30")]
    live_daily = regular.groupby("date")["score"].agg(live_min_0900_1530="min", live_max_0900_1530="max")
    live_daily["live_min_all_hours"] = live.groupby("date")["score"].min()
    check = live_daily.join(hourly_frame[["score_pre", "score_open", "score_min", "score_min_at"]].add_prefix("hourly_"),
                            how="inner").join(daily[["score_min"]].add_prefix("daily_"), how="left")
    check.to_csv(STUDY_DIR / "live_check.tsv", sep="\t")

    overlap = daily.join(hourly_frame[["score_min"]].add_suffix("_hourly"), how="inner")
    bins = pd.concat([score_bins(daily, "score_min", "daily"), score_bins(daily, "score_pre", "daily"),
                      score_bins(hourly_frame, "score_min", "hourly"), score_bins(hourly_frame, "score_pre", "hourly")])
    bins.to_csv(STUDY_DIR / "score_bins.tsv", sep="\t", index=False)

    daily_grid, daily_halt = evaluate(daily, daily_structures, "daily")
    hourly_grid, hourly_halt = evaluate(hourly_frame, hourly_structures, "hourly")
    pd.concat([daily_grid, hourly_grid]).to_csv(STUDY_DIR / "liquidate_grid.tsv", sep="\t", index=False)
    pd.concat([daily_halt, hourly_halt]).to_csv(STUDY_DIR / "halt_grid.tsv", sep="\t", index=False)

    def distribution(frame):
        return {
            "period": [str(frame.index[0].date()), str(frame.index[-1].date())], "days": len(frame),
            "score_min_lowest": int(frame["score_min"].min()),
            "score_min_quantiles": {str(level): float(frame["score_min"].quantile(level)) for level in (0.005, 0.01, 0.05, 0.1, 0.5)},
            "days_score_min_le": {str(line): int((frame["score_min"] <= line).sum()) for line in THRESHOLDS},
        }

    metrics = {
        "daily": distribution(daily),
        "hourly": distribution(hourly_frame),
        "daily_vs_hourly_overlap_days": len(overlap),
        "daily_minus_hourly_score_min_mean": round(float((overlap["score_min"] - overlap["score_min_hourly"]).mean()), 2),
        "daily_minus_hourly_score_min_abs_mean": round(float((overlap["score_min"] - overlap["score_min_hourly"]).abs().mean()), 2),
        "live_check_days": len(check),
        "live_minus_hourly_min_mean": round(float((check["live_min_0900_1530"] - check["hourly_score_min"]).mean()), 2),
        "live_minus_hourly_min_abs_mean": round(float((check["live_min_0900_1530"] - check["hourly_score_min"]).abs().mean()), 2),
    }
    (STUDY_DIR / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(metrics, ensure_ascii=False, indent=2))
    print(check.to_string())
    return 0


if __name__ == "__main__":
    sys.exit(main())
