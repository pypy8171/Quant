"""스터디 35 — SURGE_HOLD(D-157) 손실 나는 해에 신규 매수를 멈추는 트리거가 실제로 나쁜 매매를 거르나(사용자 2026-10-06).

대상  : close_depth_events.parquet 에서 실매매 규칙과 같은 칸 — depth_low < 15, wait == 4, move_pct ≥ 30, entry == 'open',
        수익 ret_low3_t15(비용 포함), 청산 종류 kind_low3_t15(0 기한·1 손절·2 익절), 보유 hold_low3_t15(매수일 포함 거래일 수).
        263건. 기간은 자료 전체(D0 2009–2026), 자르지 않는다. 구간 2010–17 / 2018–26 은 D0 연도(close_depth 와 같다).
돈    : 종목당 500만, 자본 2,000만 환산(사용자). 동시 보유 상한은 두지 않는다(기준 숫자 +4,095만·낙폭 −803만과 같은 방식).
        최대 낙폭은 매수일 순서로 손익을 쌓은 곡선(기준과 같은 방식) — 청산일 순서 곡선의 낙폭도 같이 적는다.
        연도별 수익률 = 그해 매수한 매매 손익 합 / 2,000만.
신호일: 매수일(D(k+1)) 직전 지수 거래일 = Dk. 지수 조건은 Dk 종가까지만 본다.
T1 국면 판정기: Quant/src/regime/RegimeFeed.cpp 점수(8개 지표 표) → 라벨 RISK_OFF(점수 ≤ −7)·RISK_ON(≥ 3)·NEUTRAL.
        SurgeHoldStrategy 는 매수 창(장 시작 전)에 regime.json 라벨을 읽는다 → 실매매와 같은 시점은 '매수일 개장 전 점수'.
        복원은 스터디 26 replay_regime_score.py 의 daily 갈래 규칙(미국 직전 세션 + 선물 야간 시가, 한국 지수 0표, 환율 0표)을
        2009 까지 늘려 쓴다. 신호일 종가 점수(미국 직전 세션 + 코스피·코스닥 종가 등락)도 같이 본다.
T2 지수 거름: 코스닥·코스피 Dk 종가 < 20·60·120일 평균이면 매수 안 함 / Dk 종가가 250거래일 최고 종가 대비 −10·−15·−20% 이하면 매수 안 함.
T3 전략 손실 멈춤: 판정은 263건 전부를 가상 매매로 계속 따라간 흐름에서, 매수일보다 앞선 날 청산된 매매만 본다(청산일 < 매수일).
        최근 N건 손익 합 < 0 → 멈춤 / 연속 손절 K회 → 그 청산일부터 M거래일 쉼 / 가상 누적 손익이 고점 대비 −X만 이하 → 멈춤.
판정  : 낙폭 30% 이상 감소, 총 손익 감소 20% 이내 또는 평균 상승, 이웃 값에서 같은 방향(낙폭 개선·걸러낸 쪽 평균 < 남긴 쪽),
        2010–17·2018–26 각 구간 낙폭 개선, 같은 수를 무작위로 뺐을 때(200번, seed 20261006) 낙폭 개선 95% 선 초과.
산출  : halt_trigger.txt(요약), halt_trigger_trades.parquet(매매 한 줄 + 신호일 지표), halt_trigger_conditions.parquet(조건 한 줄),
        halt_trigger_raw/(국면 복원용 시세 캐시 — 처음 받은 값을 고정).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/halt_trigger.py [--refresh]
"""
from __future__ import annotations

import argparse
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
RAW_DIRECTORY = HERE / "halt_trigger_raw"
TEXT_PATH = HERE / "halt_trigger.txt"
TRADES_PATH = HERE / "halt_trigger_trades.parquet"
CONDITIONS_PATH = HERE / "halt_trigger_conditions.parquet"
STUDY26_SCORES = REPOSITORY / "research" / "studies" / "26_regime_threshold_history" / "daily_scores.tsv"
SEED = 20261006
DRAWS = 200
SPLIT_YEAR = 2018
POSITION_WON = 500.0          # 만 원
CAPITAL_WON = 2000.0          # 만 원
HALT_SCORE, ON_SCORE = -7, 3  # RegimeFeed thresholds(config regime_feed.halt_score, regime.json on_score)
HIGH_WINDOW = 250             # 지수 고점 창(내가 정함)
REGIME_START = "2008-12-01"
BASE_DRAWDOWN_REFERENCE = -803.0

PLAN = """시험 목록(돌리기 전에 고정, 2026-10-06)
고정: 실매매 칸 263건(저가 되돌림 < 15%, k=4, 상승폭 ≥ 30%, 다음 날 시가 매수, 손절 관찰 최저 저가 −3%, 익절 +15%, 120거래일),
      비용 포함 수익 ret_low3_t15, 종목당 500만·자본 2,000만, 동시 보유 상한 없음(기준과 같은 방식), 기간 자료 전체.
T1 국면 판정기(6칸): 점수 정지선 −6·−7·−8(엔진 −7, 이웃 확인용 −6·−8) × 시점 2(매수일 개장 전 = 실매매 시점 / 신호일 종가).
    국면별(RISK_ON·NEUTRAL·RISK_OFF) n·평균·t·승률도 적는다.
T2 지수 거름(12칸): 코스닥·코스피 × 종가 < 20·60·120일 평균 / 250일 최고 대비 −10·−15·−20% 이하.
T3 전략 손실 멈춤(12칸): 최근 N건 합 < 0(N 5·10·20) / 연속 손절 K회 뒤 M거래일 쉼(K 3·4·5 × M 20·60) /
    가상 누적 손익 고점 대비 −X만 이하(X 400·600·800, 회복하면 재개 — 내가 정함).
조건마다: 남는 n·평균·t·2010–17·2018–26 평균·연도별 2,000만 대비 %·최대 낙폭(원)·총 손익·걸러낸 n·평균·차이 t·무작위 95% 선.
판정(통과 = 전부): ① 낙폭 |dd| ≤ 0.7 × 기준 ② 총 손익 ≥ 0.8 × 기준 또는 평균 > 기준 평균 ③ 이웃 값 전부 낙폭 개선 + 걸러낸 평균 < 남긴 평균
    ④ 2010–17·2018–26 각 구간 낙폭 개선 ⑤ 낙폭 개선 > 무작위로 같은 수를 뺀 200번의 95% 선(seed 20261006).
    30칸을 비교하므로 걸러낸 쪽 차이 t 는 |t| ≥ 3.1 이어야 '나쁜 매매를 골랐다'고 본다(헌장 20칸 3.0·100칸 3.5 사이, 내가 정함).
2024 분해: 그해 매매 목록 — 시장(코스피·코스닥), 신호일 코스닥 위치, 보유 동안 코스닥 지수 등락, 지수 대비 초과.
"""


def load_module(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=HERE, capture_output=True, text=True).stdout.strip()
    except OSError:
        return "unknown"


def t_value(values) -> float:
    values = np.asarray(values, dtype=float)

    if len(values) < 2 or values.std(ddof=1) == 0:
        return float("nan")

    return float(values.mean() / (values.std(ddof=1) / math.sqrt(len(values))))


def welch_t(left, right) -> float:
    left, right = np.asarray(left, float), np.asarray(right, float)

    if len(left) < 2 or len(right) < 2:
        return float("nan")

    spread = math.sqrt(left.var(ddof=1) / len(left) + right.var(ddof=1) / len(right))
    return float((left.mean() - right.mean()) / spread) if spread > 0 else float("nan")


def max_drawdown(profits: np.ndarray) -> float:
    """손익(만 원)을 순서대로 쌓은 곡선의 최대 낙폭. 시작점 0 을 고점으로 친다."""
    if len(profits) == 0:
        return 0.0

    curve = np.r_[0.0, np.cumsum(profits)]
    return float((curve - np.maximum.accumulate(curve)).min())


# ---------------------------------------------------------------- 매매 적재


def load_trades() -> pd.DataFrame:
    events = pd.read_parquet(EVENTS_PATH)
    trades = events[(events["depth_low"] < 15) & (events["wait"] == 4) & (events["move_pct"] >= 30) & (events["entry"] == "open")]
    trades = trades[["code", "surge", "year", "entry_date", "entry_price", "move_pct", "depth_low",
                     "ret_low3_t15", "kind_low3_t15", "hold_low3_t15"]].rename(
        columns={"ret_low3_t15": "ret", "kind_low3_t15": "kind", "hold_low3_t15": "hold"})
    trades = trades.sort_values(["entry_date", "code"], kind="mergesort").reset_index(drop=True)
    bars = pd.read_parquet(BARS_PATH, columns=["Date", "code", "name", "market"], filters=[("code", "in", sorted(trades["code"].unique()))])
    bars["date_int"] = bars["Date"].dt.strftime("%Y%m%d").astype(np.int64)
    bars = bars.sort_values(["code", "date_int"], kind="mergesort")
    exit_dates, markets, names = [], [], []

    for record in trades.to_dict("records"):
        calendar = bars[bars["code"] == record["code"]]
        dates = calendar["date_int"].to_numpy()
        position = int(np.searchsorted(dates, record["entry_date"]))
        exit_row = min(position + int(record["hold"]) - 1, len(dates) - 1)
        exit_dates.append(int(dates[exit_row]))
        markets.append(str(calendar["market"].iloc[min(position, len(dates) - 1)]))
        names.append(str(calendar["name"].iloc[-1]))

    trades["exit_date"], trades["market"], trades["name"] = exit_dates, markets, names
    trades["profit"] = trades["ret"] / 100.0 * POSITION_WON
    trades["entry_year"] = trades["entry_date"] // 10000
    return trades


# ---------------------------------------------------------------- 지수 조건(T2)


def attach_index(trades: pd.DataFrame) -> pd.DataFrame:
    index = pd.read_parquet(INDEX_PATH).sort_index()
    dates = index.index.to_numpy()

    for symbol, label in (("KQ11", "kosdaq"), ("KS11", "kospi")):
        close = index[symbol]

        for days in (20, 60, 120):
            index[f"{label}_above{days}"] = (close >= close.rolling(days).mean()).where(close.rolling(days).mean().notna())

        index[f"{label}_drawdown"] = (close / close.rolling(HIGH_WINDOW, min_periods=20).max() - 1.0) * 100.0

    positions = np.searchsorted(dates, trades["entry_date"].to_numpy(), side="left") - 1  # 매수일보다 앞선 마지막 지수일 = Dk
    trades["signal_date"] = dates[positions]
    columns = [column for column in index.columns if column.startswith(("kosdaq_", "kospi_"))]

    for column in columns:
        trades[column] = index[column].to_numpy()[positions]

    # 보유 동안 코스닥·코스피 등락(매수일 직전 종가 → 청산일 종가) — 2024 분해용, 판정에는 안 쓴다
    exit_positions = np.searchsorted(dates, trades["exit_date"].to_numpy(), side="right") - 1

    for symbol, label in (("KQ11", "kosdaq"), ("KS11", "kospi")):
        values = index[symbol].to_numpy()
        trades[f"{label}_hold_change"] = (values[exit_positions] / values[positions] - 1.0) * 100.0

    return trades


# ---------------------------------------------------------------- 국면 점수 복원(T1)


def regime_scores(refresh: bool) -> pd.DataFrame:
    """스터디 26 daily 갈래 규칙으로 날짜별 개장 전 점수·종가 점수. 2009 부터. 캐시는 halt_trigger_raw/."""
    replay = load_module("study26_replay", REPOSITORY / "research" / "studies" / "26_regime_threshold_history" / "replay_regime_score.py")
    replay.START = REGIME_START
    replay.RAW_DIR = RAW_DIRECTORY
    RAW_DIRECTORY.mkdir(exist_ok=True)
    kospi = replay.load_naver_index("KOSPI", refresh)
    kosdaq = replay.load_naver_index("KOSDAQ", refresh)
    us = {name: replay.load_fdr(name, refresh) for name in replay.FDR_DAILY}
    days = kospi.index.intersection(kosdaq.index)
    days = days[days >= pd.Timestamp("2009-01-02")]
    records = []

    for day in days:
        nasdaq_percent, _ = replay.last_session_percent(us["IXIC"], day)
        standard_percent, _ = replay.last_session_percent(us["GSPC"], day)
        values = {
            "NQ_F": nasdaq_percent + replay.futures_since_settle_at_open(us["NQF"], day),
            "ES_F": standard_percent + replay.futures_since_settle_at_open(us["ESF"], day),
            "TNX10": replay.last_session_percent(us["TNX"], day)[0],
            "VIX": replay.last_session_percent(us["VIX"], day)[0],
            "WTI": replay.futures_since_settle_at_open(us["CLF"], day),
        }
        valid = sum(1 for value in values.values() if value is not None and np.isfinite(value))
        us_votes = sum(replay.vote_for(key, value) for key, value in values.items())
        previous_kospi = kospi["close"].loc[:day].iloc[-2]
        previous_kosdaq = kosdaq["close"].loc[:day].iloc[-2]
        close_votes = replay.vote_for("KOSPI", (kospi.loc[day, "close"] / previous_kospi - 1.0) * 100.0) + \
            replay.vote_for("KOSDAQ", (kosdaq.loc[day, "close"] / previous_kosdaq - 1.0) * 100.0)
        records.append({"date": int(day.strftime("%Y%m%d")), "score_pre": us_votes, "score_close": us_votes + close_votes, "valid_us": valid})

    return pd.DataFrame(records).set_index("date")


def label_of(score: float, halt: int = HALT_SCORE) -> str:
    if score <= halt:
        return "RISK_OFF"

    return "RISK_ON" if score >= ON_SCORE else "NEUTRAL"


def attach_regime(trades: pd.DataFrame, scores: pd.DataFrame) -> pd.DataFrame:
    trades["score_pre_entry"] = scores["score_pre"].reindex(trades["entry_date"]).to_numpy()
    trades["score_close_signal"] = scores["score_close"].reindex(trades["signal_date"]).to_numpy()
    trades["label_pre_entry"] = [label_of(score) if np.isfinite(score) else "UNKNOWN" for score in trades["score_pre_entry"]]
    trades["label_close_signal"] = [label_of(score) if np.isfinite(score) else "UNKNOWN" for score in trades["score_close_signal"]]
    return trades


# ---------------------------------------------------------------- 전략 손실 멈춤(T3)


def closed_before(trades: pd.DataFrame, entry_date: int) -> pd.DataFrame:
    """매수일 개장 전에 알 수 있는 청산 = 청산일 < 매수일. 가상 매매(263건 전부)에서 고른다."""
    closed = trades[trades["exit_date"] < entry_date]
    return closed.sort_values(["exit_date", "entry_date", "code"], kind="mergesort")


def keep_recent_sum(trades: pd.DataFrame, count: int) -> np.ndarray:
    keep = []

    for entry_date in trades["entry_date"]:
        closed = closed_before(trades, entry_date)
        keep.append(not (len(closed) >= count and closed["profit"].iloc[-count:].sum() < 0))

    return np.array(keep)


def keep_stop_streak(trades: pd.DataFrame, streak_limit: int, rest_days: int, calendar: np.ndarray) -> np.ndarray:
    """연속 손절 K회째 청산일부터 M거래일 쉼. 손절이 아닌 청산(익절·기한)은 연속을 끊는다. 쉼이 시작되면 연속은 0 부터(내가 정함)."""
    ordered = trades.sort_values(["exit_date", "entry_date", "code"], kind="mergesort")
    rest_until = []
    streak = 0

    for record in ordered.to_dict("records"):
        streak = streak + 1 if record["kind"] == 1 else 0

        if streak >= streak_limit:
            start = int(np.searchsorted(calendar, record["exit_date"]))
            rest_until.append((record["exit_date"], int(calendar[min(start + rest_days, len(calendar) - 1)])))
            streak = 0

    keep = []

    for entry_date in trades["entry_date"]:
        # 매수일 기준 '이미 끝난 청산'에서 생긴 쉼만 본다. 쉼 끝 날짜는 청산일 + M거래일(그날부터 매수 가능)
        blocked = any(trigger < entry_date < until for trigger, until in rest_until)
        keep.append(not blocked)

    return np.array(keep)


def keep_drawdown_stop(trades: pd.DataFrame, limit: float) -> np.ndarray:
    keep = []

    for entry_date in trades["entry_date"]:
        closed = closed_before(trades, entry_date)
        curve = np.r_[0.0, np.cumsum(closed["profit"].to_numpy())]
        keep.append(not (curve[-1] - curve.max() <= -limit))

    return np.array(keep)


# ---------------------------------------------------------------- 조건 평가


def evaluate(trades: pd.DataFrame, keep: np.ndarray, base: dict, generator_state: int) -> dict:
    kept = trades[keep]
    dropped = trades[~keep]
    early = trades["year"] < SPLIT_YEAR
    late = ~early
    result = {
        "n": len(kept), "mean": kept["ret"].mean(), "t": t_value(kept["ret"]), "win": (kept["ret"] > 0).mean() * 100,
        "mean_early": kept.loc[kept["year"] < SPLIT_YEAR, "ret"].mean(), "mean_late": kept.loc[kept["year"] >= SPLIT_YEAR, "ret"].mean(),
        "total": kept["profit"].sum(),
        "drawdown": max_drawdown(kept["profit"].to_numpy()),
        "drawdown_exit_order": max_drawdown(kept.sort_values(["exit_date", "entry_date"], kind="mergesort")["profit"].to_numpy()),
        "drawdown_early": max_drawdown(trades.loc[keep & early.to_numpy(), "profit"].to_numpy()),
        "drawdown_late": max_drawdown(trades.loc[keep & late.to_numpy(), "profit"].to_numpy()),
        "dropped_n": len(dropped), "dropped_mean": dropped["ret"].mean() if len(dropped) else float("nan"),
        "difference_t": welch_t(dropped["ret"], kept["ret"]),
    }
    years = sorted(trades["entry_year"].unique())
    yearly = kept.groupby("entry_year")["profit"].sum().reindex(years, fill_value=0.0) / CAPITAL_WON * 100.0

    for year, value in yearly.items():
        result[f"year_{year}"] = value

    # 같은 수를 무작위로 뺐을 때 낙폭 개선 분포
    generator = np.random.default_rng(generator_state)
    profits = trades["profit"].to_numpy()
    improvements = []

    for _ in range(DRAWS):
        mask = np.ones(len(trades), bool)

        if len(dropped):
            mask[generator.choice(len(trades), size=len(dropped), replace=False)] = False

        improvements.append(max_drawdown(profits[mask]) - base["drawdown"])

    result["improvement"] = result["drawdown"] - base["drawdown"]
    result["random95"] = float(np.quantile(improvements, 0.95))
    result["random_median"] = float(np.median(improvements))
    return result


def conditions(trades: pd.DataFrame, calendar: np.ndarray) -> list[dict]:
    """조건 30개. group 이 같은 조건끼리 order 로 이웃을 정한다."""
    items = []

    for timing, column in (("매수일 개장 전", "score_pre_entry"), ("신호일 종가", "score_close_signal")):
        for halt in (-6, -7, -8):
            score = trades[column].to_numpy(float)
            items.append({"family": "T1", "name": f"국면 {timing} 점수 ≤ {halt} 매수 안 함", "group": f"T1_{column}", "order": -halt,
                          "keep": ~(np.isfinite(score) & (score <= halt))})

    for label, korean in (("kosdaq", "코스닥"), ("kospi", "코스피")):
        for order, days in enumerate((20, 60, 120)):
            above = trades[f"{label}_above{days}"].to_numpy(float)
            items.append({"family": "T2", "name": f"{korean} 종가 < {days}일 평균 매수 안 함", "group": f"T2_{label}_average", "order": order,
                          "keep": ~(above == 0.0)})

        for order, depth in enumerate((10, 15, 20)):
            drawdown = trades[f"{label}_drawdown"].to_numpy(float)
            items.append({"family": "T2", "name": f"{korean} 250일 고점 대비 ≤ −{depth}% 매수 안 함", "group": f"T2_{label}_drawdown",
                          "order": order, "keep": ~(drawdown <= -depth)})

    for order, count in enumerate((5, 10, 20)):
        items.append({"family": "T3", "name": f"최근 {count}건 손익 합 < 0 멈춤", "group": "T3_recent", "order": order,
                      "keep": keep_recent_sum(trades, count)})

    for streak_order, streak_limit in enumerate((3, 4, 5)):
        for rest_order, rest_days in enumerate((20, 60)):
            items.append({"family": "T3", "name": f"연속 손절 {streak_limit}회 → {rest_days}거래일 쉼", "group": "T3_streak",
                          "order": (streak_order, rest_order), "keep": keep_stop_streak(trades, streak_limit, rest_days, calendar)})

    for order, limit in enumerate((400, 600, 800)):
        items.append({"family": "T3", "name": f"가상 누적 고점 대비 −{limit}만 이하 멈춤", "group": "T3_drawdown", "order": order,
                      "keep": keep_drawdown_stop(trades, limit)})

    return items


def neighbor_names(items: list[dict], item: dict) -> list[str]:
    result = []

    for other in items:
        if other["group"] != item["group"] or other is item:
            continue

        if isinstance(item["order"], tuple):
            gap = abs(other["order"][0] - item["order"][0]) + abs(other["order"][1] - item["order"][1])
        else:
            gap = abs(other["order"] - item["order"])

        if gap == 1:
            result.append(other["name"])

    return result


def judge(rows: dict, name: str, neighbors: list[str], base: dict) -> dict:
    row = rows[name]
    checks = {
        "낙폭30%": row["drawdown"] >= 0.7 * base["drawdown"],
        "손익": row["total"] >= 0.8 * base["total"] or row["mean"] > base["mean"],
        "이웃": all(rows[other]["improvement"] > 0 and (rows[other]["dropped_n"] == 0 or rows[other]["dropped_mean"] < rows[other]["mean"])
                  for other in neighbors) and row["dropped_n"] > 0 and row["dropped_mean"] < row["mean"],
        "두구간": row["drawdown_early"] > base["drawdown_early"] and row["drawdown_late"] > base["drawdown_late"],
        "무작위95": row["improvement"] > row["random95"],
    }
    return checks


# ---------------------------------------------------------------- 출력


def money(value: float) -> str:
    return f"{value:+,.0f}만"


def format_row(row: dict, years: list[int]) -> list[str]:
    yearly = " ".join(f"{str(year)[2:]}:{row[f'year_{year}']:+.0f}" for year in years)
    return [f"  n{row['n']:3d} 평균 {row['mean']:+5.2f}% t{row['t']:+5.2f} 승{row['win']:4.1f}% | 10–17 {row['mean_early']:+5.2f} 18–26 {row['mean_late']:+5.2f} | "
            f"총 {money(row['total'])} 낙폭 {money(row['drawdown'])}(청산일 순 {money(row['drawdown_exit_order'])}, 10–17 {money(row['drawdown_early'])}, "
            f"18–26 {money(row['drawdown_late'])})",
            f"  걸러냄 n{row['dropped_n']} 평균 {row['dropped_mean']:+.2f}% 차이 t{row['difference_t']:+.2f} | 낙폭 개선 {money(row['improvement'])} "
            f"vs 무작위 중앙 {money(row['random_median'])}·95% {money(row['random95'])}",
            f"  연도(2,000만 대비 %) {yearly}"]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--refresh", action="store_true", help="halt_trigger_raw/ 시세 캐시를 다시 받는다")
    arguments = parser.parse_args()
    TEXT_PATH.write_bytes((PLAN + "\n(실행 중)\n").encode("utf-8"))

    trades = load_trades()
    trades = attach_index(trades)
    scores = regime_scores(arguments.refresh)
    trades = attach_regime(trades, scores)
    calendar = pd.read_parquet(INDEX_PATH).index.to_numpy()
    years = sorted(trades["entry_year"].unique())
    all_keep = np.ones(len(trades), bool)
    early = (trades["year"] < SPLIT_YEAR).to_numpy()
    base = {"drawdown": max_drawdown(trades["profit"].to_numpy()), "total": trades["profit"].sum(), "mean": trades["ret"].mean(),
            "drawdown_early": max_drawdown(trades.loc[early, "profit"].to_numpy()),
            "drawdown_late": max_drawdown(trades.loc[~early, "profit"].to_numpy())}
    base_row = evaluate(trades, all_keep, base, SEED)

    lines = [PLAN, f"스터디 35 halt_trigger — 커밋 {git_commit()}, 매매 {len(trades)}건, 매수일 {trades['entry_date'].min()}–{trades['entry_date'].max()}, seed {SEED}", ""]
    lines += ["기준(거름 없음)"] + format_row(base_row, years)
    lines.append(f"  재현 점검: 총 {money(base['total'])}(기대 +4,095만), 낙폭 {money(base['drawdown'])}(기대 {BASE_DRAWDOWN_REFERENCE:+,.0f}만)")
    exit_year = trades.groupby(trades["exit_date"] // 10000)["profit"].sum() / CAPITAL_WON * 100.0
    lines.append("  참고 — 청산 연도 기준 %: " + " ".join(f"{str(year)[2:]}:{value:+.0f}" for year, value in exit_year.items()))

    # 국면 복원 맞춤 점검: 2015 뒤는 스터디 26 daily_scores 와 같은 규칙이어야 한다
    lines += ["", "국면 복원 점검"]

    if STUDY26_SCORES.exists():
        reference = pd.read_csv(STUDY26_SCORES, sep="\t")
        reference["date"] = pd.to_datetime(reference["date"]).dt.strftime("%Y%m%d").astype(int)
        joined = reference.set_index("date")[["score_pre"]].join(scores[["score_pre"]], rsuffix="_mine", how="inner")
        lines.append(f"  스터디 26 daily_scores.tsv 와 개장 전 점수 일치 {(joined['score_pre'] == joined['score_pre_mine']).mean() * 100:.1f}% ({len(joined):,}일)")

    lines.append(f"  복원 날짜 {scores.index.min()}–{scores.index.max()} {len(scores):,}일, 미국 지표 5개 중 3개 미만인 날 {int((scores['valid_us'] < 3).sum())}일, "
                 f"매매 중 점수 없음 {int(trades['score_pre_entry'].isna().sum())}건")
    lines.append(f"  개장 전 점수 ≤ −7 비율(전체 날) {(scores['score_pre'] <= HALT_SCORE).mean() * 100:.1f}%, 신호일 종가 점수 ≤ −7 {(scores['score_close'] <= HALT_SCORE).mean() * 100:.1f}%")

    # T1 국면별
    lines += ["", "T1 국면별 성적"]

    for column, timing in (("label_pre_entry", "매수일 개장 전(실매매 시점)"), ("label_close_signal", "신호일 종가")):
        lines.append(f"  {timing}")

        for label in ("RISK_ON", "NEUTRAL", "RISK_OFF", "UNKNOWN"):
            part = trades[trades[column] == label]

            if len(part):
                lines.append(f"    {label:8s} n{len(part):3d} 평균 {part['ret'].mean():+6.2f}% t{t_value(part['ret']):+5.2f} 승{(part['ret'] > 0).mean() * 100:4.1f}% "
                             f"총 {money(part['profit'].sum())}")

    items = conditions(trades, calendar)
    rows = {}

    for number, item in enumerate(items):
        rows[item["name"]] = {"family": item["family"], "name": item["name"], **evaluate(trades, item["keep"], base, SEED + number + 1)}

    lines += ["", "조건별(판정: 낙폭30% · 손익 · 이웃 · 두구간 · 무작위95 — O 통과 / x 실패)"]
    table = []

    for item in items:
        row = rows[item["name"]]
        neighbors = neighbor_names(items, item)
        checks = judge(rows, item["name"], neighbors, base)
        row.update({f"pass_{key}": value for key, value in checks.items()})
        row["passed"] = all(checks.values())
        table.append(row)
        marks = " ".join(f"{key}{'O' if value else 'x'}" for key, value in checks.items())
        lines.append(f"[{item['family']}] {item['name']} — {'통과' if row['passed'] else '탈락'} ({marks})")
        lines += format_row(row, years)

    frame = pd.DataFrame(table)
    frame.to_parquet(CONDITIONS_PATH, index=False)
    passed = frame[frame["passed"]]
    lines += ["", f"통과 {len(passed)}/{len(frame)}: " + (", ".join(passed["name"]) if len(passed) else "없음")]
    ranked = frame.assign(ratio=frame["drawdown"] / base["drawdown"]).sort_values("improvement", ascending=False).head(8)
    lines.append("낙폭 개선 상위 8: " + " | ".join(f"{row['name']} {money(row['improvement'])}(무작위95 {money(row['random95'])}, 총 {money(row['total'])})"
                                             for row in ranked.to_dict("records")))

    # 2024 분해
    year_trades = trades[trades["entry_year"] == 2024]
    lines += ["", f"2024 매수 {len(year_trades)}건 분해 — 합 {money(year_trades['profit'].sum())}, 평균 {year_trades['ret'].mean():+.2f}%, "
                  f"손절 {int((year_trades['kind'] == 1).sum())} 익절 {int((year_trades['kind'] == 2).sum())} 기한 {int((year_trades['kind'] == 0).sum())}"]
    lines.append(f"  시장: " + ", ".join(f"{market} {count}건 평균 {year_trades.loc[year_trades['market'] == market, 'ret'].mean():+.2f}%"
                                       for market, count in year_trades["market"].value_counts().items()))
    lines.append(f"  보유 동안 코스닥 지수 평균 {year_trades['kosdaq_hold_change'].mean():+.2f}%, 코스피 {year_trades['kospi_hold_change'].mean():+.2f}% "
                 f"(전 기간 매매 평균 코스닥 {trades['kosdaq_hold_change'].mean():+.2f}%), 신호일 코스닥 60일선 위 {int((year_trades['kosdaq_above60'] == 1).sum())}/{len(year_trades)}건, "
                 f"신호일 코스닥 250일 고점 대비 중앙값 {year_trades['kosdaq_drawdown'].median():+.1f}%")
    other_years = trades[trades["entry_year"] != 2024]
    lines.append(f"  다른 해: 신호일 코스닥 60일선 위 {(other_years['kosdaq_above60'] == 1).mean() * 100:.0f}%, 250일 고점 대비 중앙값 {other_years['kosdaq_drawdown'].median():+.1f}%")
    beta = np.polyfit(trades["kosdaq_hold_change"], trades["ret"], 1)[0]
    lines.append(f"  전 기간 매매 수익을 보유 동안 코스닥 등락에 회귀한 기울기 {beta:+.2f}, 상관 {np.corrcoef(trades['kosdaq_hold_change'], trades['ret'])[0, 1]:+.2f}")
    lines.append(f"  2024 매매의 코스닥 대비 초과 평균 {(year_trades['ret'] - year_trades['kosdaq_hold_change']).mean():+.2f}%p "
                 f"(다른 해 {(other_years['ret'] - other_years['kosdaq_hold_change']).mean():+.2f}%p)")
    lines.append("  매수일 종목 시장 결과 보유일 수익 | 코스닥 보유 중 등락 | 신호일 코스닥 60일선 위 | 250일 고점 대비 | 국면(개장 전)")

    for record in year_trades.to_dict("records"):
        how = {0: "기한", 1: "손절", 2: "익절"}[int(record["kind"])]
        lines.append(f"  {record['entry_date']} {record['code']} {record['name'][:10]:10s} {record['market']:6s} {how} {int(record['hold']):3d}일 {record['ret']:+6.2f}% | "
                     f"{record['kosdaq_hold_change']:+6.2f}% | {'위' if record['kosdaq_above60'] == 1 else '아래'} | {record['kosdaq_drawdown']:+5.1f}% | "
                     f"{record['label_pre_entry']}({record['score_pre_entry']:+.0f})")

    trades.to_parquet(TRADES_PATH, index=False)
    text = "\n".join(lines) + "\n"
    TEXT_PATH.write_bytes(text.encode("utf-8"))  # 이 폴더 파일은 LF 다
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
