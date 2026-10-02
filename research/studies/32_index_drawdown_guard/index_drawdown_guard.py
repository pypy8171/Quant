"""스터디 32 — 국내 지수 장중 낙폭 규칙(신규 매수 정지·보유 청산) vs 국면 점수 −11 청산.

질문: 코스피·코스닥이 장중 X% 빠지면(전일 종가 대비 / 당일 고점 대비) 신규 매수를 멈추거나 보유를 판다는 규칙이
국면 점수 −11 청산보다 낫거나, 같이 쓰면 나은가. 지수 보유 대비 수익·최대낙폭·청산 뒤 놓친 반등을 본다.

재사용: research/studies/26_regime_threshold_history/replay_regime_score.py 의 시세 캐시·점수 복원(build_daily·build_hourly)을
그대로 불러 쓴다(내려받기 없음, 26의 raw/ 캐시만 읽는다).

지수 경로(각 시각에 알 수 있던 값만 쓴다 — 규칙은 지나온 경로의 최고값·전일 종가만 본다):
  daily  2015-01~  네이버 일봉. 하루 경로 = 시가 → (첫 극값) → (둘째 극값) → 종가. 음봉(종가<시가)은 시가→고가→저가→종가,
         양봉은 시가→저가→고가→종가(흔히 쓰는 OHLC 경로 근사). 시각은 09:00·11:10·13:20·15:30.
  hourly 2024-05~  Yahoo ^KS11·^KQ11 1시간봉. 첫 점 = 네이버 시가(09:00), 봉마다 같은 규칙으로 봉 시가→극값→극값→종가.
  선을 지나는 순간 선 값에 판다(연속 감시 근사). 개장 시가가 이미 선 아래면 시가에 판다.

청산 모형: 첫 발동(지수 규칙·점수 규칙 중 이른 것)에 팔고 왕복 0.215%를 뗀다. 되사기 두 가지 —
  next_open  다음 거래일 시가. 단 그날 시가가 다시 규칙에 걸리면(전일 종가 기준 갭 ≤ X, 또는 개장 전 점수 ≤ −11) 그날은 밖에 있다.
  same_close 그날 종가(장중 낙폭만 피한다).
정지 모형: 발동 지점에서 샀다면(= 정지가 막은 매수) 그 뒤 당일 종가·다음날 종가 수익.

DevScale 적용(근사): 스터디 28 `zone_grid_days.tsv`의 실계좌 칸(z_lo8_up5_tp5, 2025-09-19~2026-09-18, 172건)에서
발동 시각에 들고 있던 종목을 1분봉 종가로 팔았다면 원래 경로 대비 얼마였나를 종목별로 더한다.

사용: py -X utf8 research/studies/32_index_drawdown_guard/index_drawdown_guard.py [--no-devscale]
산출: 같은 폴더 liquidate_grid.tsv, halt_grid.tsv, crash_days.tsv, devscale_grid.tsv, devscale_events.tsv, metrics.json
"""
import argparse
import ast
import importlib.util
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY_DIR = Path(__file__).resolve().parent
REPO_ROOT = STUDY_DIR.parents[2]
SOURCE_26 = REPO_ROOT / "research" / "studies" / "26_regime_threshold_history" / "replay_regime_score.py"
DEVSCALE_DAYS = REPO_ROOT / "research" / "studies" / "28_devscale_zone_width" / "zone_grid_days.tsv"
DEVSCALE_VARIANT = "z_lo8_up5_tp5"
MINUTE_DIR = REPO_ROOT / "PYQuant" / "data" / "minute"
ROUND_TRIP_COST = 0.00215
SCORE_LINE = -11
DROPS = [-1.5, -2.0, -2.5, -3.0, -4.0, -5.0]
BASES = ("prev", "high")
OPEN_MINUTE = 540.0
CLOSE_MINUTE = 930.0
FRONT_SHARE = 0.7


def load_study_26():
    module_specification = importlib.util.spec_from_file_location("replay_regime_score", SOURCE_26)
    module = importlib.util.module_from_spec(module_specification)
    module_specification.loader.exec_module(module)
    return module


# ---------------------------------------------------------------- 지수 경로


def bar_points(open_value, high, low, close, start, end, mode="by_close"):
    """한 봉 안의 점 (시각, 값). by_close: 음봉은 고가 먼저, 양봉은 저가 먼저. high_first·low_first: 늘 그 순서(경로 감도 대조)."""
    span = end - start
    if mode == "high_first":
        first, second = high, low
    elif mode == "low_first":
        first, second = low, high
    else:
        first, second = (high, low) if close < open_value else (low, high)
    return [(start, open_value), (start + span / 3.0, first), (start + 2.0 * span / 3.0, second), (end, close)]


def daily_paths(frame: pd.DataFrame, name: str, mode: str = "by_close") -> dict:
    paths = {}
    for day, row in frame.iterrows():
        paths[day] = bar_points(row[name + "_open"], row[name + "_high"], row[name + "_low"], row[name + "_close"],
                                OPEN_MINUTE, CLOSE_MINUTE, mode)
    return paths


def hourly_paths(frame: pd.DataFrame, bars: pd.DataFrame, name: str) -> dict:
    local = bars.index.tz_convert("Asia/Seoul")
    bars = bars.assign(day=local.tz_localize(None).normalize(), start=local.hour * 60.0 + local.minute)
    paths = {}
    for day, group in bars.groupby("day"):
        if day not in frame.index:
            continue
        points = [(OPEN_MINUTE, frame.loc[day, name + "_open"])]
        for _, bar in group.sort_values("start").iterrows():
            start = max(bar["start"], OPEN_MINUTE)
            end = min(start + 60.0, CLOSE_MINUTE)
            points += bar_points(bar["open"], bar["high"], bar["low"], bar["close"], start, end)
        paths[day] = points
    return paths


def index_trigger(points, previous_close: float, basis: str, drop_percent: float):
    """첫 발동 (분, 가격) 또는 None. prev = 전일 종가 대비, high = 개장 뒤 지나온 최고값 대비."""
    ratio = 1.0 + drop_percent / 100.0
    first_time, first_value = points[0]
    if basis == "prev" and first_value <= previous_close * ratio:
        return first_time, first_value

    running_high = first_value
    for (time_a, value_a), (time_b, value_b) in zip(points[:-1], points[1:]):
        running_high = max(running_high, value_a)
        line = previous_close * ratio if basis == "prev" else running_high * ratio
        if value_b <= line < value_a:
            moment = time_a + (value_a - line) / (value_a - value_b) * (time_b - time_a)
            return moment, line
        if value_b <= line and value_a <= line:
            return time_a, value_a

    return None


def score_trigger_hourly(structure: dict, line: int, column: int):
    """26 hourly 표본에 시각을 붙여 첫 점수 발동 (분, 지수값)."""
    samples = structure["samples"]
    if structure["pre"] <= line:
        return OPEN_MINUTE, samples[0][column]

    pending = []
    for sample in samples:
        label = sample[0]
        if label == "09:00":
            if sample[1] <= line:
                return OPEN_MINUTE, sample[column]
            continue
        if label.endswith("~"):
            pending.append(sample)
            continue
        end = int(label[:2]) * 60.0 + int(label[3:5])
        start = 900.0 if end == CLOSE_MINUTE else end - 60.0
        for step, path_sample in enumerate(pending, start=1):
            if path_sample[1] <= line:
                return start + step / len(pending) * (end - start), path_sample[column]
        pending = []
        if sample[1] <= line:
            return end, sample[column]

    return None


def score_trigger_daily(structure: dict, line: int, column: int, low_minute: float):
    """26 daily 표본(시가→저가 직선 201점) — 시각은 시가와 저가 시각 사이 비례."""
    samples = structure["samples"]
    if structure["pre"] <= line:
        return OPEN_MINUTE, samples[0][column]

    for position, sample in enumerate(samples):
        if sample[1] <= line:
            fraction = position / (len(samples) - 1)
            return OPEN_MINUTE + fraction * (low_minute - OPEN_MINUTE), sample[column]

    return None


def earliest(*triggers):
    found = [trigger for trigger in triggers if trigger is not None]
    if not found:
        return None
    return min(found, key=lambda trigger: (trigger[0], -trigger[1]))


# ---------------------------------------------------------------- 평가


def max_drawdown(equity: np.ndarray) -> float:
    peak = np.maximum.accumulate(equity)
    return float((equity / peak - 1.0).min())


def build_rules(frame, structures, paths, name, source):
    """규칙 이름 -> {day: (분, 가격) | None}, 그리고 규칙별 '그날 시가에 되사도 되는가'."""
    column = 2 if name == "kospi" else 3
    score = {}
    for day in frame.index:
        if source == "hourly":
            score[day] = score_trigger_hourly(structures[day], SCORE_LINE, column)
        else:
            low_minute = min(paths[day], key=lambda point: point[1])[0]
            score[day] = score_trigger_daily(structures[day], SCORE_LINE, column, low_minute)
    score_reentry = {day: structures[day]["pre"] > SCORE_LINE for day in frame.index}
    gap = (frame[name + "_open"] / frame[name + "_prev_close"] - 1.0) * 100.0

    rules = {f"score{SCORE_LINE}": (score, score_reentry)}
    for basis in BASES:
        for drop in DROPS:
            triggers = {day: index_trigger(paths[day], frame.loc[day, name + "_prev_close"], basis, drop)
                        for day in frame.index}
            if basis == "prev":
                reentry = {day: gap.loc[day] > drop for day in frame.index}
            else:
                reentry = {day: True for day in frame.index}
            rules[f"{basis}{drop:+.1f}"] = (triggers, reentry)
            combined = {day: earliest(triggers[day], score[day]) for day in frame.index}
            combined_reentry = {day: reentry[day] and score_reentry[day] for day in frame.index}
            rules[f"{basis}{drop:+.1f}|score{SCORE_LINE}"] = (combined, combined_reentry)
    return rules


def liquidation_run(frame, name, triggers, reentry, rebuy):
    holding = True
    returns = []
    events = []
    days_out = 0
    for day, row in frame.iterrows():
        start_price = row[name + "_prev_close"]
        if not holding:
            if reentry[day]:
                holding = True
                start_price = row[name + "_open"]
                events[-1]["rebuy_price"] = start_price
            else:
                days_out += 1
                returns.append(0.0)
                continue
        trigger = triggers[day]
        if trigger is None:
            returns.append(row[name + "_close"] / start_price - 1.0)
            continue
        returns.append(trigger[1] / start_price - 1.0 - ROUND_TRIP_COST)
        event = {"date": day, "minute": trigger[0], "sell_price": trigger[1], "rebuy_price": np.nan}
        if rebuy == "same_close":
            event["rebuy_price"] = row[name + "_close"]
            returns[-1] = (1.0 + returns[-1]) * (row[name + "_close"] / trigger[1]) - 1.0
        else:
            holding = False
        events.append(event)
    return np.array(returns), pd.DataFrame(events, columns=["date", "minute", "sell_price", "rebuy_price"]), days_out


def evaluate(frame, rules, name, source, periods):
    rows = []
    for period_name, (start, end) in periods.items():
        subset = frame.loc[start:end]
        hold_daily = (subset[name + "_close"] / subset[name + "_prev_close"]).values
        hold_equity = np.cumprod(hold_daily)
        years = len(subset) / 250.0
        worst = subset.index[np.argsort(hold_daily)[:20]]
        for rule_name, (triggers, reentry) in rules.items():
            for rebuy in ("next_open", "same_close"):
                returns, events, days_out = liquidation_run(subset, name, triggers, reentry, rebuy)
                equity = np.cumprod(1.0 + returns)
                rebound = (events["rebuy_price"] / events["sell_price"] - 1.0).dropna()
                by_day = pd.Series(returns, index=subset.index)
                rows.append({
                    "source": source, "index": name, "period": period_name, "rule": rule_name, "rebuy": rebuy,
                    "days": len(subset), "events": len(events), "per_year": round(len(events) / years, 1),
                    "days_out": days_out,
                    "hold_total_pct": round(100 * (hold_equity[-1] - 1), 1),
                    "rule_total_pct": round(100 * (equity[-1] - 1), 1),
                    "edge_pct_per_year": round(100 * (np.log(equity[-1]) - np.log(hold_equity[-1])) / years, 2),
                    "hold_mdd_pct": round(100 * max_drawdown(hold_equity), 1),
                    "rule_mdd_pct": round(100 * max_drawdown(equity), 1),
                    "rebound_mean_pct": round(100 * rebound.mean(), 3) if len(rebound) else np.nan,
                    "rebound_t": round(rebound.mean() / rebound.std(ddof=1) * np.sqrt(len(rebound)), 2)
                    if len(rebound) > 2 else np.nan,
                    "rebound_up_share_pct": round(100 * (rebound > 0).mean(), 1) if len(rebound) else np.nan,
                    "worst20_hold_mean_pct": round(100 * (hold_daily[np.argsort(hold_daily)[:20]] - 1).mean(), 2),
                    "worst20_rule_mean_pct": round(100 * by_day.loc[worst].mean(), 2),
                })
    return rows


def halt_rows(full_frame, rules, name, source, periods):
    rows = []
    for period_name, (start, end) in periods.items():
        rows += halt_period(full_frame, full_frame.loc[start:end].index, rules, name, source, period_name)
    return rows


def halt_period(full_frame, days, rules, name, source, period_name):
    rows = []
    next_close = full_frame[name + "_close"].shift(-1)
    frame = full_frame.loc[days]
    next_close = next_close.loc[days]
    base_close = (frame[name + "_close"] / frame[name + "_open"] - 1.0).mean()
    base_next = (next_close / frame[name + "_open"] - 1.0).mean()
    for rule_name, (triggers, _) in rules.items():
        if "|" in rule_name:
            continue
        entries = [(frame.loc[day, name + "_close"] / triggers[day][1] - 1.0, next_close.loc[day] / triggers[day][1] - 1.0)
                   for day in days if triggers[day] is not None]
        entries = pd.DataFrame(entries, columns=["to_close", "to_next_close"]).dropna()
        rows.append({"source": source, "index": name, "period": period_name, "rule": rule_name, "days": len(entries),
                     "per_year": round(len(entries) / (len(frame) / 250.0), 1),
                     "to_close_pct": round(100 * entries["to_close"].mean(), 3) if len(entries) else np.nan,
                     "to_next_close_pct": round(100 * entries["to_next_close"].mean(), 3) if len(entries) else np.nan,
                     "to_next_close_up_pct": round(100 * (entries["to_next_close"] > 0).mean(), 1) if len(entries) else np.nan,
                     "all_days_open_to_close_pct": round(100 * base_close, 3),
                     "all_days_open_to_next_close_pct": round(100 * base_next, 3)})
    return rows


def crash_table(frame, rules, name, source):
    """보유 기준 최악 15일 — 규칙별 그날 매도 시각·매도 시점 낙폭."""
    daily = frame[name + "_close"] / frame[name + "_prev_close"] - 1.0
    rows = []
    picks = ["score-11", "prev-2.0", "prev-3.0", "high-2.0", "high-3.0"]
    for day in daily.nsmallest(15).index:
        row = {"source": source, "index": name, "date": day.date(), "close_vs_prev_pct": round(100 * daily.loc[day], 2),
               "low_vs_prev_pct": round(100 * (frame.loc[day, name + "_low"] / frame.loc[day, name + "_prev_close"] - 1), 2),
               "next_day_pct": round(100 * daily.shift(-1).loc[day], 2) if np.isfinite(daily.shift(-1).loc[day]) else np.nan}
        for rule_name in picks:
            trigger = rules[rule_name][0][day]
            row[rule_name + "_sell_vs_prev_pct"] = (round(100 * (trigger[1] / frame.loc[day, name + "_prev_close"] - 1), 2)
                                                   if trigger else np.nan)
        rows.append(row)
    return rows


# ---------------------------------------------------------------- DevScale 적용


def minute_frame(ticker: str, date_yyyymmdd: str, cache: dict):
    key = (ticker, date_yyyymmdd)
    if key not in cache:
        path = MINUTE_DIR / ticker / f"{date_yyyymmdd}.parquet"
        if path.exists():
            frame = pd.read_parquet(path)
            frame["minute"] = frame["hms"].astype(str).str.zfill(6).str[:2].astype(int) * 60 + \
                frame["hms"].astype(str).str.zfill(6).str[2:4].astype(int)
            cache[key] = frame
        else:
            cache[key] = None
    return cache[key]


def price_at_minute(frame, minute: float) -> float:
    """그 분까지 끝난 1분봉 종가. 개장 직후(09:00 이전 봉 없음)면 첫 봉 시가."""
    done = frame[frame["minute"] + 1 <= minute]
    return float(done["close"].iloc[-1]) if len(done) else float(frame["open"].iloc[0])


def exit_minute(row, carried: bool, average: float, frame) -> float:
    """원래 경로의 그날 매도 시각 근사. 존 이탈은 넘긴 보유면 첫 3분봉(09:03), 아니면 15:15."""
    exits = json.loads(row["exits"]) if isinstance(row["exits"], str) and row["exits"].startswith("{") else {}
    if "stop" in exits and frame is not None:
        hit = frame[frame["low"] <= average * (1 - 0.065)]
        return float(hit["minute"].iloc[0] + 3) if len(hit) else 915.0
    if "tp" in exits and frame is not None:
        hit = frame[frame["high"] >= average * 1.05]
        return float(hit["minute"].iloc[0] + 3) if len(hit) else 915.0
    if "zone_exit" in exits:
        return 543.0 if carried else 915.0
    return CLOSE_MINUTE


def devscale_apply(rule_triggers: dict, signal_name: str):
    """rule_triggers: 규칙 -> {day: (분, 가격)|None} (hourly 갈래). 종목별 원래 경로 대비 차이(원)."""
    days = pd.read_csv(DEVSCALE_DAYS, sep="\t", dtype={"ticker": str, "ymd": str}, low_memory=False)
    days = days[days["variant"] == DEVSCALE_VARIANT].sort_values(["ticker", "ymd"]).reset_index(drop=True)
    days["day"] = pd.to_datetime(days["ymd"], format="%Y%m%d")
    days["carry_dict"] = [ast.literal_eval(value) if isinstance(value, str) and value.startswith("{") else None
                          for value in days["carry"]]
    # 매수 한 건 = 매수일부터 carry가 끝나는 날까지. 사슬 손익 = 그 구간 net_pnl 합.
    chain_pnl = {}
    chain_of_row = {}
    for ticker, group in days.groupby("ticker"):
        current = None
        for index, row in group.iterrows():
            if row["buys"] > 0:
                current = index
                chain_pnl[current] = 0.0
            if current is not None:
                chain_pnl[current] += row["net_pnl"]
                chain_of_row[index] = current
                if row["carry_dict"] is None:
                    current = None

    previous_row = {}
    following_row = {}
    for _, group in days.groupby("ticker"):
        indexes = list(group.index)
        for position, index in enumerate(indexes):
            previous_row[index] = indexes[position - 1] if position > 0 else None
            following_row[index] = indexes[position + 1] if position + 1 < len(indexes) else None
    cache = {}
    rows = []
    event_rows = []
    window = days["day"].min(), days["day"].max()
    for rule_name, triggers in rule_triggers.items():
        totals = {"next_open": 0.0, "same_close": 0.0, "halt_only": 0.0}
        counts = {"trigger_days": 0, "positions_hit": 0, "buys_blocked": 0}
        for day, trigger in triggers.items():
            if trigger is None or not (window[0] <= day <= window[1]):
                continue
            counts["trigger_days"] += 1
            minute = trigger[0]
            today = days[days["day"] == day]
            for index, row in today.iterrows():
                previous = days.loc[previous_row[index]] if previous_row[index] is not None else None
                carried = previous is not None and previous["carry_dict"] is not None
                bought_today = row["buys"] > 0
                if not carried and not bought_today:
                    continue
                frame = minute_frame(row["ticker"], row["ymd"], cache)
                if frame is None:
                    continue
                if bought_today and not carried:
                    # 매수는 첫 3분봉(09:00~09:03)에 난다고 본다. 발동이 그보다 앞서면 정지·청산 둘 다 이 매수를 막는다.
                    if minute <= 543.0:
                        lost = chain_pnl.get(chain_of_row.get(index), 0.0)
                        for mode in totals:
                            totals[mode] -= lost
                        counts["buys_blocked"] += 1
                        event_rows.append({"rule": rule_name, "signal": signal_name, "date": day.date(),
                                           "ticker": row["ticker"], "kind": "buy_blocked", "delta_won": -lost})
                        continue
                    shares = (row["carry_dict"] or {}).get("position") or \
                        round(row["buy_notional"] / float(frame["open"].iloc[0]))
                    average = row["buy_notional"] / max(shares, 1)
                else:
                    shares = previous["carry_dict"]["position"]
                    average = previous["carry_dict"]["average"]
                end = exit_minute(row, carried, average, frame) if row["carry_dict"] is None else None
                if end is not None and end <= minute:
                    continue
                price = price_at_minute(frame, minute)
                counts["positions_hit"] += 1
                if end is not None:
                    # 원래 경로도 그날 판다 — 발동 시각에 먼저 판 값과 원래 매도 금액의 차이(되사기 없음, 비용 같음)
                    delta = {"next_open": shares * price - row["sell_notional"]}
                    delta["same_close"] = delta["next_open"]
                else:
                    extra_cost = shares * price * ROUND_TRIP_COST
                    following = days.loc[following_row[index]] if following_row[index] is not None else None
                    next_frame = minute_frame(row["ticker"], following["ymd"], cache) if following is not None else None
                    next_open = float(next_frame["open"].iloc[0]) if next_frame is not None else float(frame["close"].iloc[-1])
                    delta = {"next_open": shares * (price - next_open) - extra_cost,
                             "same_close": shares * (price - float(frame["close"].iloc[-1])) - extra_cost}
                totals["next_open"] += delta["next_open"]
                totals["same_close"] += delta["same_close"]
                event_rows.append({"rule": rule_name, "signal": signal_name, "date": day.date(), "ticker": row["ticker"],
                                   "kind": "sold_same_day_anyway" if end is not None else "carried_past_day",
                                   "minute": round(minute, 1), "sell_price": price, "shares": shares,
                                   "delta_won": round(delta["next_open"], 0),
                                   "delta_same_close_won": round(delta["same_close"], 0)})
        rows.append({"signal": signal_name, "rule": rule_name, **counts,
                     "delta_next_open_won": round(totals["next_open"]), "delta_same_close_won": round(totals["same_close"]),
                     "delta_halt_only_won": round(totals["halt_only"]),
                     "base_net_pnl_won": round(days["net_pnl"].sum()), "base_buys": int(days["buys"].sum())})
    return rows, event_rows


# ---------------------------------------------------------------- 실행


def split_periods(frame: pd.DataFrame) -> dict:
    cut = int(len(frame) * FRONT_SHARE)
    return {"all": (frame.index[0], frame.index[-1]), "front70": (frame.index[0], frame.index[cut - 1]),
            "back30": (frame.index[cut], frame.index[-1])}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-devscale", action="store_true")
    arguments = parser.parse_args()
    study26 = load_study_26()
    kospi = study26.load_naver_index("KOSPI", False)
    kosdaq = study26.load_naver_index("KOSDAQ", False)
    us = {name: study26.load_fdr(name, False) for name in study26.FDR_DAILY}
    krw_daily = study26.load_yahoo("KRW=X", "1d", "max", "KRW", False)
    hourly = {name: study26.load_yahoo(symbol, "60m", "730d", name, False)
              for name, symbol in study26.YAHOO_HOURLY.items()}
    daily_frame, daily_structures = study26.build_daily(kospi, kosdaq, us, krw_daily)
    hourly_frame, hourly_structures = study26.build_hourly(kospi, kosdaq, us, hourly)
    for frame in (daily_frame, hourly_frame):
        frame["kospi_high"] = kospi["high"].reindex(frame.index)
        frame["kosdaq_high"] = kosdaq["high"].reindex(frame.index)

    grid, halts, crashes = [], [], []
    hourly_rules = {}
    periods_used = {}
    for source, frame, structures in (("daily", daily_frame, daily_structures), ("hourly", hourly_frame, hourly_structures)):
        for name, bars_key in (("kospi", "KS11"), ("kosdaq", "KQ11")):
            paths = daily_paths(frame, name) if source == "daily" else hourly_paths(frame, hourly[bars_key], name)
            frame_used = frame.loc[[day for day in frame.index if day in paths]]
            rules = build_rules(frame_used, structures, paths, name, source)
            if source == "hourly":
                hourly_rules[name] = rules
            periods = split_periods(frame_used)
            periods_used[f"{source}_{name}"] = {key: [str(start.date()), str(end.date()), len(frame_used.loc[start:end])]
                                                for key, (start, end) in periods.items()}
            grid += evaluate(frame_used, rules, name, source, periods)
            halts += halt_rows(frame_used, rules, name, source, periods)
            crashes += crash_table(frame_used, rules, name, source)

    # 경로 감도: 일봉 안 고가·저가 순서를 종가로 정하는 것은 그날 종가를 미리 아는 근사다. 늘 고가 먼저·늘 저가 먼저로도 잰다.
    for mode in ("high_first", "low_first"):
        for name in ("kospi", "kosdaq"):
            paths = daily_paths(daily_frame, name, mode)
            rules = build_rules(daily_frame, daily_structures, paths, name, "daily")
            rules = {key: value for key, value in rules.items() if "|" not in key and not key.startswith("score")}
            grid += evaluate(daily_frame, rules, name, "daily_" + mode, split_periods(daily_frame))

    grid = pd.DataFrame(grid)
    grid.to_csv(STUDY_DIR / "liquidate_grid.tsv", sep="\t", index=False)
    pd.DataFrame(halts).to_csv(STUDY_DIR / "halt_grid.tsv", sep="\t", index=False)
    pd.DataFrame(crashes).to_csv(STUDY_DIR / "crash_days.tsv", sep="\t", index=False)

    metrics = {"periods": periods_used, "round_trip_cost": ROUND_TRIP_COST, "score_line": SCORE_LINE, "drops": DROPS}
    if not arguments.no_devscale:
        devscale_rows, devscale_events = [], []
        for signal_name, rules in hourly_rules.items():
            picks = {rule_name: triggers for rule_name, (triggers, _) in rules.items()
                     if rule_name == "score-11" or ("|" not in rule_name)}
            rows, events = devscale_apply(picks, signal_name)
            devscale_rows += rows
            devscale_events += events
        pd.DataFrame(devscale_rows).to_csv(STUDY_DIR / "devscale_grid.tsv", sep="\t", index=False)
        pd.DataFrame(devscale_events).to_csv(STUDY_DIR / "devscale_events.tsv", sep="\t", index=False)
        metrics["devscale_variant"] = DEVSCALE_VARIANT

    view = grid[(grid["rebuy"] == "next_open")].pivot_table(index=["source", "index", "rule"], columns="period",
                                                            values="edge_pct_per_year")
    metrics["edge_next_open_summary"] = {f"{key[0]}|{key[1]}|{key[2]}": row.dropna().to_dict()
                                         for key, row in view.iterrows()}
    (STUDY_DIR / "metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=1, default=str),
                                            encoding="utf-8")
    print(view.round(2).to_string())
    return 0


if __name__ == "__main__":
    sys.exit(main())
