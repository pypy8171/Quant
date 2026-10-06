"""스터디 35 — (A) 되돌림 구간에서 이평선을 깼다가 다시 올려놓은 횟수가 성적을 가르나, (B) 되돌림이 깊어진 자리에 지정가로 사면 어떤가
(사용자 2026-10-06).

사건  : close_depth.build 와 같다(+10% 종가 연속 2일 이상 묶음, D0 = 묶음 마지막 날, 묶음 시작 전날 원종가 ≥ 2,000,
        거래대금 시장 비중 ≥ 0.2%, 상승폭 = close[D0] − close[묶음 시작 전날]). 기간 = 자료 시작(D0 2009-06)–2026-10-02 전부.
        두 기간 2010–17 / 2018–26 은 D0 연도(2009 사건은 앞 기간).
시험 A: 1번 칸(되돌림 저가 < 15%, k4, 상승 30%+, 관찰 최저 저가 −3% 손절, +15% 익절, Dk 종가 매수), 추천 칸(같은 조건, D(k+1) 시가),
        넓은 집합(되돌림 < 30%, 두 매수), 기준선 B5_20(k5, 되돌림 < 20%, 상승 0%+, 관찰 저가 −1%, +20%, Dk 종가)의 매매를
        D1..Dk 의 이평선(MA3·MA5·MA10, 수정 종가 단순 평균, 그날 종가 포함) 셈으로 나눈다.
          (a) 장중 회복 = 저가 < 이평선 이고 종가 ≥ 이평선 인 날 수
          (b) 종가 회복 = D1..Dk 에서 종가 < 이평선 인 날 다음 날 종가 ≥ 이평선 인 횟수(D0 상태는 세지 않음 — 내가 정함)
          (c) Dk 종가 ≥ 이평선(위/아래)
        모두 Dk 종가까지의 값만 쓴다. 이평선 값이 없는(상장 직후) 매매는 그 셈에서 뺀다.
시험 B: 1번 칸 사건 조건(묶음 2일+, 상승 30%+)에 되돌림 한도 없음. 지정가는 D0 종가 − X × 상승폭(X 20·25·30·40·50%) 또는
        전날 MA10 × (1 − Y)(Y 0·3·5%). 매수 창 D1..Dk(k 4·5·8 — 내가 정함). 창 안에서 저가 ≤ 지정가인 첫날 체결,
        체결가 = 시가 < 지정가면 시가, 아니면 지정가. 창 안에서 +10% 종가가 나오면 그다음 날부터 주문 취소(내가 정함 — 미래를 보고
        사건을 빼지 않으려고). 하한가로 마감한 날(종가 변화 ≤ −(가격제한폭 − 0.5%p), 2015-06-15 전 15%·뒤 30% — 내가 정함)은 체결 못 함,
        거래정지 날은 자료에 행이 없어 창의 하루로 세지 않는다.
        청산: 손절 매수가 −5·−8·−10% / 체결일까지 최저 저가 −3%(= min(최저 저가 × 0.97, 매수가 × 0.999)), 익절 +10·15·20·30%,
        체결일 + 120거래일 종가(내가 정함). 체결 당일은 저가가 매수가 기준 손절가 이하면 손절로 보고 고가 익절은 인정하지 않는다.
        최저 저가 손절은 체결일 저가까지 넣어 정하므로 다음 날부터 본다. 다음 날부터는 optimize.path_exits 와 같은 규칙
        (같은 날 둘 다 닿으면 손절, 갭은 시가, 비용 ma_sweep.COST).
산출  : ma_reclaim_deep.txt(요약), ma_reclaim_deep_trades.parquet(시험 A 매매), ma_reclaim_deep_fills.parquet(시험 B 체결),
        ma_reclaim_deep_cells.parquet(시험 B 칸).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/ma_reclaim_deep.py [--rebuild]
"""
from __future__ import annotations

import argparse
import importlib.util
import math
import sys
import warnings
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent


def load_module(name: str, file_name: str):
    specification = importlib.util.spec_from_file_location(name, HERE / file_name)
    module = importlib.util.module_from_spec(specification)  # 이름이 backtest 패키지와 겹쳐 파일로 읽는다
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


close_depth = load_module("study35_close_depth", "close_depth.py")
optimize = close_depth.optimize
base = optimize.base
sweep = optimize.sweep
study33 = optimize.study33
SEED = 20261006
SHUFFLES = 200
SPLIT_YEAR = 2018
NULL_MIN = 30
GROUP_MIN = 10
AVERAGE_WINDOWS = [3, 5, 10]
LIMIT_CHANGE_DATE = 20150615
# 시험 A 집합: (이름, k, 되돌림 한도, 상승폭 하한, 매수, 수익 열)
RECLAIM_SETS = [("추천 k4 d15 시가", 4, 15, 30, "open", "ret_low3_t15"),
                ("1번 k4 d15 종가", 4, 15, 30, "close", "ret_low3_t15"),
                ("넓은 k4 d30 시가", 4, 30, 30, "open", "ret_low3_t15"),
                ("넓은 k4 d30 종가", 4, 30, 30, "close", "ret_low3_t15"),
                ("B5_20 k5 d20 종가", 5, 20, 0, "close", "ret_low1_t20")]
MEASURES = [("intraday", "장중 회복"), ("reclaim", "종가 회복"), ("above_end", "Dk 종가 위")]
# 시험 B 축
DEPTH_RULES = [f"x{percent}" for percent in (20, 25, 30, 40, 50)]
AVERAGE_RULES = [f"m10y{percent}" for percent in (0, 3, 5)]
LIMIT_RULES = DEPTH_RULES + AVERAGE_RULES
WINDOWS = [4, 5, 8]
LIMIT_STOPS = [("low3", "low", 3), ("entry5", "entry", 5), ("entry8", "entry", 8), ("entry10", "entry", 10)]
LIMIT_TAKES = [10, 15, 20, 30]
LIMIT_EXITS = [(stop[0], take) for stop in LIMIT_STOPS for take in LIMIT_TAKES]
AXES = ["rule", "window", "stop", "take"]
TRADES_PATH = HERE / "ma_reclaim_deep_trades.parquet"
FILLS_PATH = HERE / "ma_reclaim_deep_fills.parquet"
CELLS_PATH = HERE / "ma_reclaim_deep_cells.parquet"
TEXT_PATH = HERE / "ma_reclaim_deep.txt"

PLAN = """시험 목록과 판정 기준(돌리기 전에 적음, 사용자 2026-10-06 지시 그대로 + 내가 정한 값 표시)

시험 A — 이평선 이탈 후 회복 횟수
  관찰 구간 D1–Dk 안에서 이평선(MA3·MA5·MA10, 종가 이동평균) 기준 셈:
    (a) 장중 회복 = 저가 < 이평선 이고 종가 ≥ 이평선 인 날 수
    (b) 종가 회복 = 종가 < 이평선 이었다가 이후 날 종가 ≥ 이평선 으로 돌아온 횟수(D1 부터 셈, D0 상태는 안 셈 — 내가 정함)
    (c) Dk 종가가 이평선 위인지 아래인지
  대상 집합 5개: 추천 칸(k4 되돌림 저가 < 15% 상승 30%+ 관찰 저가 −3%/+15%, D(k+1) 시가 매수), 1번 칸(같은 조건, Dk 종가 매수),
    넓은 집합(되돌림 < 30%, 같은 손절·익절, 시가·종가 두 매수 — 둘 다 보는 것은 내가 정함), B5_20(k5 되돌림 < 20% 관찰 저가 −1%/+20% Dk 종가).
  (a)(b)는 0 / 1 / 2+ 로, (c)는 위/아래로 나눠 n·평균·t·승률·2010–17/2018–26 평균. 0 대비 1+(c 는 아래 대비 위) 평균 차이의 웰치 t.
  나눔은 Dk 종가까지의 값만 쓴다(시가 매수는 그다음 날, 종가 매수는 그 종가에 판단).
  칸 수 = 집합 5 × 이평선 3 × 셈 3 = 45. 문턱: 차이 |t| ≥ 2 이고 두 기간 차이 부호가 같을 때만 "영향 있음".
  탐색 보정: 집합마다 매매 수익을 무작위로 섞어(이름표 섞기와 같음) 45칸 최대 |t| 를 200번(seed 20261006), 95% 선과 비교.

시험 B — 되돌림 깊은 자리 지정가 매수
  사건 조건은 1번 칸과 같되 되돌림 한도 없음. 지정가 = D0 종가 − X×상승폭, X ∈ {20, 25, 30, 40, 50}%,
    또는 MA10 × (1 − Y), Y ∈ {0, 3, 5}% (매일 전날 종가까지의 MA10).
  매수 창 D1–Dk, k ∈ {4, 5, 8}(내가 정함). 장중 저가 ≤ 지정가인 첫날 체결. 체결가 = 시가 < 지정가면 시가, 아니면 지정가.
  창 안에서 +10% 종가가 나오면 다음 날부터 주문 취소(내가 정함).
  체결 당일: 저가 ≤ 매수가 기준 손절가면 손절로 본다(같은 날 고가 익절은 인정 안 함). 최저 저가 손절은 체결일 저가까지 넣어 정하고 다음 날부터 본다.
  손절 매수가 −5/−8/−10%, 체결일까지 최저 저가 −3%. 익절 +10/15/20/30%. 체결일 + 120거래일 종가 청산(내가 정함).
  하한가로 마감한 날(종가 변화 ≤ −(가격제한폭 − 0.5%p) — 정의는 내가 정함)·거래정지 날은 체결 못 함. 하한가 날도 체결로 본 결과를 민감도로 같이 적는다.
  칸 = 지정가 8 × k 3 × 손절 4 × 익절 4 = 384. 칸별 n·평균·t·승률·두 기간·이웃 부호.
  이웃 = 축 하나만 한 칸 옮김(지정가는 같은 종류 안에서 깊이 순서, 손절 순서 low3·entry5·entry8·entry10 — 내가 정함), 건수 0 이웃은 뺀다.
  통과선 = 전체 t ≥ 3, 2010–17·2018–26 평균 둘 다 ≥ 0, 이웃 칸 평균 전부 같은 부호.
  탐색 보정: 사건마다 수익 부호를 무작위로 뒤집어(그 사건의 모든 칸에 같은 부호) 건수 ≥ 30(내가 정함) 칸 최대 t 를 200번(seed 20261006), 95% 선과 비교.
  추천 칸(+3.11%)과 같은 기간(전체) 비교, 둘을 같이 쓰면(같은 사건은 먼저 산 쪽 하나만 — 내가 정함) 어떤지 한 줄.
"""


def moving_averages(close: np.ndarray) -> dict[int, np.ndarray]:
    return {window: base.moving(close, window) for window in AVERAGE_WINDOWS}


def reclaim_features(close: np.ndarray, low: np.ndarray, averages: dict[int, np.ndarray], first: int, last: int) -> dict:
    """first..last 행(D1..Dk)의 이평선 셈. 그 구간에 이평선 값이 비면 NaN."""
    result = {}

    for window, average in averages.items():
        segment = average[first:last + 1]

        if np.isnan(segment).any():
            result[f"intraday_{window}"] = result[f"reclaim_{window}"] = result[f"above_end_{window}"] = np.nan
            continue

        closes = close[first:last + 1]
        below = closes < segment
        result[f"intraday_{window}"] = float(((low[first:last + 1] < segment) & (closes >= segment)).sum())
        result[f"reclaim_{window}"] = float((below[:-1] & ~below[1:]).sum())
        result[f"above_end_{window}"] = float(not below[-1])

    return result


def limit_down_day(series, row: int) -> bool:
    limit_percent = 15.0 if int(series.dates[row]) < LIMIT_CHANGE_DATE else 30.0
    return (series.close[row] / series.close[row - 1] - 1) * 100 <= -(limit_percent - 0.5)


def limit_exits(series, fill_row: int, entry: float, lowest_low: float, last: int) -> dict:
    """체결일 fill_row 부터 last 까지 16개 청산 칸. 체결 당일은 매수가 기준 손절만 본다."""
    opens = series.open_price[fill_row:last + 1]
    lows = series.low[fill_row:last + 1]
    highs = series.high[fill_row:last + 1]
    result = {}

    for name, kind, percent in LIMIT_STOPS:
        if kind == "entry":
            stop_price = entry * (1 - percent / 100)
            stop_hits = np.flatnonzero(lows <= stop_price)
        else:
            stop_price = min(lowest_low * (1 - percent / 100), entry * 0.999)
            stop_hits = np.flatnonzero(lows[1:] <= stop_price) + 1

        stop_at = stop_hits[0] if len(stop_hits) else math.inf

        for take in LIMIT_TAKES:
            take_price = entry * (1 + take / 100)
            take_hits = np.flatnonzero(highs[1:] >= take_price) + 1
            take_at = take_hits[0] if len(take_hits) else math.inf

            if stop_at <= take_at and stop_at < math.inf:
                price = stop_price if stop_at == 0 else min(opens[stop_at], stop_price)
                code, held = 1, stop_at
            elif take_at < math.inf:
                price, code, held = max(opens[take_at], take_price), 2, take_at
            else:
                price, code, held = series.close[last], 0, last - fill_row

            result[f"ret_{name}_t{take}"] = (price / entry * sweep.COST - 1) * 100
            result[f"kind_{name}_t{take}"] = code
            result[f"hold_{name}_t{take}"] = int(held)

    return result


def limit_price(rule: str, surge_close: float, move: float, average10: np.ndarray, row: int) -> float:
    if rule.startswith("x"):
        return surge_close - int(rule[1:]) / 100 * move

    return average10[row - 1] * (1 - int(rule[4:]) / 100)


def limit_fills(series, surge_row: int, move: float, average10: np.ndarray, rows: int) -> list[dict]:
    """한 사건의 지정가 8개 × (본 규칙, 하한가 날도 체결) 체결 기록. 창은 가장 긴 k 로 한 번 찾고 칸에서 k 로 거른다."""
    close = series.close
    window_end = min(surge_row + max(WINDOWS), rows - 1)
    surge_close = float(close[surge_row])
    fills = []

    for rule in LIMIT_RULES:
        found = {}

        for row in range(surge_row + 1, window_end + 1):
            price = limit_price(rule, surge_close, move, average10, row)

            if not math.isnan(price) and price > 0 and series.low[row] <= price:
                blocked = limit_down_day(series, row)

                if "fill_limitdown" not in found:
                    found["fill_limitdown"] = (row, price)

                if not blocked and "main" not in found:
                    found["main"] = (row, price)

            if "main" in found or (close[row] / close[row - 1] - 1) * 100 >= base.GAIN - 1e-9:
                break

        for variant, (fill_row, price) in found.items():
            if variant == "fill_limitdown" and found.get("main") == (fill_row, price):
                continue

            last = fill_row + base.HORIZON

            if last > rows - 1:
                if not series.delisted:
                    continue

                last = rows - 1

            entry = float(min(series.open_price[fill_row], price))
            lowest_low = float(series.low[surge_row + 1:fill_row + 1].min())
            record = {"rule": rule, "variant": variant, "fill_offset": fill_row - surge_row, "fill_date": int(series.dates[fill_row]),
                      "limit_price": float(price), "entry_price": entry, "opened_below": bool(series.open_price[fill_row] < price)}
            record.update(limit_exits(series, fill_row, entry, lowest_low, last))
            fills.append(record)

    return fills


def build() -> tuple[pd.DataFrame, pd.DataFrame, dict]:
    """시험 A 매매(사건 × 집합 k × 매수)와 시험 B 체결. 사건 조건은 close_depth.build 를 그대로 옮겼다."""
    stocks, _, _ = study33.load_inputs(base.LAST_DATE)
    frames = []

    for series in stocks:
        if len(series.dates) >= 2:
            frames.append(pd.DataFrame({"code": series.code, "date": series.dates,
                                        "turnover": series.close * series.factor * series.volume,
                                        "change": np.r_[np.nan, series.close[1:] / series.close[:-1] - 1.0] * 100.0}))

    market = pd.concat(frames, ignore_index=True)
    market["share"] = market["turnover"] / market.groupby("date")["turnover"].transform("sum") * 100.0
    candidates = market[market["change"] >= base.GAIN - 1e-9]
    relative = dict(zip(zip(candidates["code"], candidates["date"].astype(int)), candidates["share"]))
    del market, frames, candidates
    trades, fills = [], []
    counts = {"open_limit_skipped": 0}

    for series in stocks:
        rows = len(series.dates)

        if rows < 30:
            continue

        close = series.close
        volume = series.volume.astype(float)
        raw_close = close * series.factor
        averages = moving_averages(close)
        change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0

        for surge_row in range(1, rows - 1):
            if change(surge_row) < base.GAIN - 1e-9 or change(surge_row + 1) >= base.GAIN - 1e-9:
                continue

            run_start = surge_row

            while run_start > 1 and change(run_start - 1) >= base.GAIN - 1e-9:
                run_start -= 1

            if surge_row - run_start + 1 < 2:
                continue

            share = max(relative.get((series.code, int(series.dates[row])), 0.0) for row in range(run_start, surge_row + 1))

            if share < base.SHARE_MIN or raw_close[run_start - 1] < study33.MIN_PREVIOUS_CLOSE_KRW or volume[surge_row] <= 0:
                continue

            move = close[surge_row] - close[run_start - 1]

            if move <= 0:
                continue

            move_percent = move / close[run_start - 1] * 100
            common = {"code": series.code, "surge": int(series.dates[surge_row]), "year": int(series.dates[surge_row]) // 10000,
                      "move_pct": move_percent}

            if move_percent >= 30:
                for fill in limit_fills(series, surge_row, move, averages[10], rows):
                    fills.append({**common, **fill})

            for wait in (4, 5):
                row = surge_row + wait

                if row >= rows - 1:
                    break

                if any(change(day) >= base.GAIN - 1e-9 for day in range(surge_row + 2, row + 1)):
                    break

                last = row + base.HORIZON

                if last > rows - 1:
                    if not series.delisted:
                        continue

                    last = rows - 1

                lowest_low = float(series.low[surge_row + 1:row + 1].min())
                lowest_close = float(close[surge_row + 1:row + 1].min())
                depth_low = (close[surge_row] - lowest_low) / move * 100
                features = reclaim_features(close, series.low, averages, surge_row + 1, row)
                shared = {**common, "wait": wait, "depth_low": depth_low, **features}

                if wait == 5:
                    if depth_low < 20:
                        result = optimize.path_exits(series, row, float(close[row]), lowest_low, last)
                        trades.append({**shared, "entry": "close", "entry_date": int(series.dates[row]),
                                       "ret_low1_t20": result["ret_low1_t20"]})

                    continue

                if depth_low >= 30 or move_percent < 30:
                    continue

                result = close_depth.path_exits(series, row + 1, float(close[row]), lowest_low, lowest_close, last)
                trades.append({**shared, "entry": "close", "entry_date": int(series.dates[row]), "ret_low3_t15": result["ret_low3_t15"]})
                next_row = row + 1
                gap = (series.open_price[next_row] / close[row] - 1) * 100
                limit_percent = 15.0 if int(series.dates[next_row]) < LIMIT_CHANGE_DATE else 30.0

                if gap >= limit_percent - 0.5:
                    counts["open_limit_skipped"] += 1
                    continue

                entry = float(series.open_price[next_row])
                result = close_depth.path_exits(series, next_row, entry, lowest_low, lowest_close, last)
                trades.append({**shared, "entry": "open", "entry_date": int(series.dates[next_row]), "ret_low3_t15": result["ret_low3_t15"]})

    return pd.DataFrame(trades), pd.DataFrame(fills), counts


def t_value(values) -> float:
    values = np.asarray(values, float)
    return float(values.mean() / (values.std(ddof=1) / math.sqrt(len(values)))) if len(values) > 1 else float("nan")


def summary(values: np.ndarray, years: np.ndarray) -> dict:
    early, late = values[years < SPLIT_YEAR], values[years >= SPLIT_YEAR]
    return {"n": len(values), "mean": values.mean() if len(values) else float("nan"), "t": t_value(values),
            "win": (values > 0).mean() * 100 if len(values) else float("nan"),
            "n_early": len(early), "mean_early": early.mean() if len(early) else float("nan"), "t_early": t_value(early),
            "n_late": len(late), "mean_late": late.mean() if len(late) else float("nan"), "t_late": t_value(late)}


def welch_matrix(values: np.ndarray, upper: np.ndarray, lower: np.ndarray) -> np.ndarray:
    """values[매매, 반복] 과 그룹 표시[매매, 칸] → 웰치 t[반복, 칸](upper − lower)."""
    statistics = []

    for mask in (upper, lower):
        count = mask.sum(0)[None, :]
        total = values.T @ mask
        square = (values ** 2).T @ mask
        mean = total / count
        variance = (square - count * mean ** 2) / (count - 1)
        statistics.append((mean, variance, count))

    (mean_upper, variance_upper, count_upper), (mean_lower, variance_lower, count_lower) = statistics

    with np.errstate(divide="ignore", invalid="ignore"):
        welch = (mean_upper - mean_lower) / np.sqrt(variance_upper / count_upper + variance_lower / count_lower)

    # 한쪽 그룹이 GROUP_MIN 건 미만이면 분산이 흔들려 t 가 터지므로 칸에서 뺀다
    return np.where((count_upper >= GROUP_MIN) & (count_lower >= GROUP_MIN), welch, np.nan)


def reclaim_set(trades: pd.DataFrame, wait: int, depth: int, move: int, entry: str, column: str) -> pd.DataFrame:
    frame = trades[(trades["wait"] == wait) & (trades["depth_low"] < depth) & (trades["move_pct"] >= move) & (trades["entry"] == entry)]
    return frame[frame[column].notna()].sort_values(["entry_date", "code"], kind="mergesort").reset_index(drop=True)


def group_masks(frame: pd.DataFrame) -> tuple[list[tuple], np.ndarray, np.ndarray]:
    """칸 9개(이평선 3 × 셈 3)의 위 그룹(1+ 또는 위)과 아래 그룹(0 또는 아래) 표시."""
    keys, upper, lower = [], [], []

    for window in AVERAGE_WINDOWS:
        for measure, _ in MEASURES:
            values = frame[f"{measure}_{window}"].to_numpy(float)
            keys.append((window, measure))
            upper.append(values >= 1)
            lower.append(values == 0)

    return keys, np.array(upper, dtype=float).T, np.array(lower, dtype=float).T


def test_reclaim(trades: pd.DataFrame) -> list[str]:
    lines = ["", "=" * 100, "시험 A 결과 — 이평선 이탈 후 회복 횟수", "=" * 100]
    generator = np.random.default_rng(SEED)
    results, null_parts = [], []

    for label, wait, depth, move, entry, column in RECLAIM_SETS:
        frame = reclaim_set(trades, wait, depth, move, entry, column)
        values = frame[column].to_numpy(float)
        years = frame["year"].to_numpy()
        base_row = summary(values, years)
        lines += ["", f"[{label}] {column} 전체 n{base_row['n']} {base_row['mean']:+.2f}% t{base_row['t']:+.2f} 승{base_row['win']:.1f}% | "
                  f"10–17 {base_row['mean_early']:+.2f} n{base_row['n_early']} | 18–26 {base_row['mean_late']:+.2f} n{base_row['n_late']}"]
        keys, upper, lower = group_masks(frame)

        for key_index, (window, measure) in enumerate(keys):
            counts = frame[f"{measure}_{window}"].to_numpy(float)
            name = dict(MEASURES)[measure]
            groups = [("아래", counts == 0), ("위", counts == 1)] if measure == "above_end" else \
                [("0", counts == 0), ("1", counts == 1), ("2+", counts >= 2)]
            parts = []

            for group_name, mask in groups:
                row = summary(values[mask], years[mask])
                parts.append(f"{group_name}: n{row['n']:3d} {row['mean']:+6.2f} t{row['t']:+5.2f} 승{row['win']:4.1f}% "
                             f"(10–17 {row['mean_early']:+6.2f} / 18–26 {row['mean_late']:+6.2f})")

            up, down = upper[:, key_index].astype(bool), lower[:, key_index].astype(bool)
            difference = values[up].mean() - values[down].mean() if up.any() and down.any() else float("nan")
            welch = float(welch_matrix(values[:, None], upper[:, [key_index]], lower[:, [key_index]])[0, 0])
            early_difference = values[up & (years < SPLIT_YEAR)].mean() - values[down & (years < SPLIT_YEAR)].mean()
            late_difference = values[up & (years >= SPLIT_YEAR)].mean() - values[down & (years >= SPLIT_YEAR)].mean()
            effect = bool(abs(welch) >= 2 and np.sign(early_difference) == np.sign(late_difference) == np.sign(difference))
            comparison = "위−아래" if measure == "above_end" else "1+ − 0"
            lines.append(f"  MA{window:<2d} {name:8s} | " + " | ".join(parts))
            lines.append(f"  {'':6s}{'':8s}   차이({comparison}) {difference:+6.2f}%p 웰치 t{welch:+5.2f} | 10–17 {early_difference:+6.2f} / "
                         f"18–26 {late_difference:+6.2f} → {'영향 있음' if effect else '영향 없음'}")
            results.append({"set": label, "window": window, "measure": measure, "difference": difference, "welch_t": welch,
                            "early_difference": early_difference, "late_difference": late_difference, "effect": effect})

        shuffled = np.array([generator.permutation(values) for _ in range(SHUFFLES)]).T
        null_parts.append(np.nanmax(np.abs(welch_matrix(shuffled, upper, lower)), axis=1))

    null = np.max(np.array(null_parts), axis=0)
    observed = max(abs(row["welch_t"]) for row in results if not math.isnan(row["welch_t"]))
    effects = [row for row in results if row["effect"]]
    threshold = np.quantile(null, 0.95)
    valid = sum(1 for row in results if not math.isnan(row["welch_t"]))
    lines += ["", f"칸 {len(results)}개(두 그룹 모두 {GROUP_MIN}건 이상인 칸 {valid}개 — 나머지는 한쪽이 거의 비어 판정 불가). 이름표 섞기 {SHUFFLES}번 최대 |t|: 중앙값 {np.median(null):.2f}, 95% {threshold:.2f}, 최대 {null.max():.2f} — "
              f"실제 최대 |t| {observed:.2f}, 무작위가 그 이상인 비율 {(null >= observed).mean() * 100:.1f}%",
              f"문턱(|t| ≥ 2, 두 기간 같은 부호)을 넘은 칸 {len(effects)}개, 그중 무작위 95%({threshold:.2f}) 이상 "
              f"{sum(1 for row in effects if abs(row['welch_t']) >= threshold)}개:"]

    for row in effects:
        lines.append(f"  {row['set']} MA{row['window']} {dict(MEASURES)[row['measure']]}: 차이 {row['difference']:+.2f}%p t{row['welch_t']:+.2f} "
                     f"(10–17 {row['early_difference']:+.2f} / 18–26 {row['late_difference']:+.2f})")

    return lines


def cell_fills(fills: pd.DataFrame, rule: str, window: int, variant: str = "main") -> pd.DataFrame:
    if variant == "main":
        frame = fills[(fills["variant"] == "main") & (fills["rule"] == rule)]
    else:
        # 하한가 날도 체결로 보면: 다른 체결이 있는 사건은 그 기록으로 바꾼다
        alternative = fills[(fills["variant"] == "fill_limitdown") & (fills["rule"] == rule)]
        main = fills[(fills["variant"] == "main") & (fills["rule"] == rule)]
        replaced = set(zip(alternative["code"], alternative["surge"]))
        keep = [key not in replaced for key in zip(main["code"], main["surge"])]
        frame = pd.concat([main[keep], alternative], ignore_index=True)

    frame = frame[frame["fill_offset"] <= window]
    return frame.sort_values(["fill_date", "code"], kind="mergesort").reset_index(drop=True)


def ordered_values(axis: str, value):
    if axis == "rule":
        return DEPTH_RULES if value in DEPTH_RULES else AVERAGE_RULES

    return {"window": WINDOWS, "stop": [stop[0] for stop in LIMIT_STOPS], "take": LIMIT_TAKES}[axis]


def neighbors(cell: dict) -> list[dict]:
    result = []

    for axis in AXES:
        values = ordered_values(axis, cell[axis])
        position = values.index(cell[axis])

        for step in (position - 1, position + 1):
            if 0 <= step < len(values):
                result.append({**cell, axis: values[step]})

    return result


def limit_cells(fills: pd.DataFrame, variant: str) -> pd.DataFrame:
    rows = []

    for rule in LIMIT_RULES:
        for window in WINDOWS:
            frame = cell_fills(fills, rule, window, variant)
            years = frame["year"].to_numpy()

            for stop, take in LIMIT_EXITS:
                row = {"rule": rule, "window": window, "stop": stop, "take": take,
                       **summary(frame[f"ret_{stop}_t{take}"].to_numpy(float), years)}
                row["stop_share"] = (frame[f"kind_{stop}_t{take}"] == 1).mean() * 100 if len(frame) else float("nan")
                row["opened_below"] = frame["opened_below"].mean() * 100 if len(frame) else float("nan")
                row["fill_offset_mean"] = frame["fill_offset"].mean() if len(frame) else float("nan")
                rows.append(row)

    cells = pd.DataFrame(rows)
    lookup = {tuple(row[axis] for axis in AXES): (row["n"], row["mean"]) for row in rows}
    same_counts, near_counts, near_minimum, passes = [], [], [], []

    for row in rows:
        near = [lookup[tuple(other[axis] for axis in AXES)] for other in neighbors(row)]
        near = [mean for count, mean in near if count > 0]
        same = sum(1 for mean in near if np.sign(mean) == np.sign(row["mean"]))
        same_counts.append(same)
        near_counts.append(len(near))
        near_minimum.append(min(near) if near else float("nan"))
        passes.append(bool(row["t"] >= 3 and row["mean_early"] >= 0 and row["mean_late"] >= 0 and same == len(near)))

    cells["neighbor_same"], cells["neighbor_count"], cells["neighbor_min"], cells["pass"] = same_counts, near_counts, near_minimum, passes
    return cells


def limit_null(fills: pd.DataFrame, variant: str) -> np.ndarray:
    """사건마다 부호 뒤집기(모든 칸 같은 부호), 건수 ≥ NULL_MIN 칸 최대 t 를 SHUFFLES 번."""
    generator = np.random.default_rng(SEED)
    events = fills.drop_duplicates(["code", "surge"])
    event_keys = pd.factorize(events["code"] + "_" + events["surge"].astype(str))[0]
    signs = generator.choice([-1.0, 1.0], size=(event_keys.max() + 1, SHUFFLES))
    key_of = dict(zip(zip(events["code"], events["surge"]), event_keys))
    best = np.full(SHUFFLES, -np.inf)

    for rule in LIMIT_RULES:
        for window in WINDOWS:
            frame = cell_fills(fills, rule, window, variant)

            if len(frame) < NULL_MIN:
                continue

            flips = signs[[key_of[key] for key in zip(frame["code"], frame["surge"])]]
            values = frame[[f"ret_{stop}_t{take}" for stop, take in LIMIT_EXITS]].to_numpy(float)
            count = np.full((SHUFFLES, len(LIMIT_EXITS)), float(len(frame)))
            total = flips.T @ values
            square = np.repeat((values ** 2).sum(0)[None, :], SHUFFLES, 0)
            best = np.maximum(best, np.nanmax(optimize.t_of(total, square, count), axis=1))

    return best


def cell_label(row: dict) -> str:
    rule = row["rule"]
    text = f"D0종가−{rule[1:]}%×상승폭" if rule.startswith("x") else f"전날MA10×(1−{rule[4:]}%)"
    return f"{text:18s} k{row['window']} {row['stop']}/+{row['take']}"


def format_cell(row: dict) -> str:
    return (f"{cell_label(row):40s} n{row['n']:4d} {row['mean']:+6.2f} t{row['t']:+5.2f} 승{row['win']:4.1f}% | "
            f"10–17 {row['mean_early']:+6.2f} n{row['n_early']:3d} | 18–26 {row['mean_late']:+6.2f} n{row['n_late']:3d} | "
            f"이웃 {row['neighbor_same']}/{row['neighbor_count']} (최저 {row['neighbor_min']:+.2f}) | 손절 {row['stop_share']:.0f}% "
            f"시가체결 {row['opened_below']:.0f}% 평균 D{row['fill_offset_mean']:.1f}")


def limit_variant(fills: pd.DataFrame, variant: str, title: str) -> tuple[list[str], pd.DataFrame]:
    """한 체결 가정(본 규칙 / 하한가 날도 체결)의 칸 표·탐색 보정·상위 칸 줄."""
    cells = limit_cells(fills, variant)
    null = limit_null(fills, variant)
    usable = cells[cells["n"] >= NULL_MIN]
    observed = float(usable["t"].max())
    threshold = float(np.quantile(null, 0.95))
    lines = ["", f"--- {title} ---",
             f"칸 {len(cells)}개, 건수 ≥ {NULL_MIN} 칸 {len(usable)}개. 부호 뒤집기 {SHUFFLES}번 최대 t: 중앙값 {np.median(null):.2f}, "
             f"95% {threshold:.2f}, 99% {np.quantile(null, 0.99):.2f}, 최대 {null.max():.2f} — 실제 최대 t {observed:.2f}, "
             f"무작위가 그 이상인 비율 {(null >= observed).mean() * 100:.1f}%",
             f"통과선(t ≥ 3, 두 기간 ≥ 0, 이웃 같은 부호) 통과 {int(cells['pass'].sum())}칸, 그중 무작위 95% 이상 "
             f"{int((cells['pass'] & (cells['t'] >= threshold)).sum())}칸, 전체 평균이 플러스인 칸 {int((cells['mean'] > 0).sum())}/{len(cells)}"]
    passed = cells[cells["pass"]].sort_values("t", ascending=False)

    for row in passed.to_dict("records"):
        lines.append("  통과 " + format_cell(row))

    lines += ["", "t 상위 10칸(건수 ≥ 30):"]

    for row in usable.sort_values("t", ascending=False).head(10).to_dict("records"):
        lines.append("  " + format_cell(row) + (" | 통과" if row["pass"] else ""))

    lines += ["", "지정가 × k 별 16개 청산 칸 평균의 평균과 가장 좋은 칸(청산을 고르지 않고 본 모양):"]

    for rule in LIMIT_RULES:
        for window in WINDOWS:
            group = cells[(cells["rule"] == rule) & (cells["window"] == window)]
            best = group.loc[group["t"].idxmax()].to_dict()
            lines.append(f"  {cell_label({'rule': rule, 'window': window, 'stop': '-', 'take': '-'})[:23]:23s} n{int(group['n'].max()):4d} "
                         f"평균의 평균 {group['mean'].mean():+6.2f}, 플러스 {int((group['mean'] > 0).sum()):2d}/16 | 최고 {best['stop']}/+{best['take']} "
                         f"{best['mean']:+6.2f} t{best['t']:+5.2f} (10–17 {best['mean_early']:+6.2f} / 18–26 {best['mean_late']:+6.2f})")

    return lines, cells.assign(variant=variant)


def union_line(recommended: pd.DataFrame, column: str, fills: pd.DataFrame, best: dict, variant: str) -> str:
    """추천 칸과 지정가 칸을 같이 쓸 때. 같은 사건은 먼저 산 쪽 하나만."""
    best_fills = cell_fills(fills, best["rule"], int(best["window"]), variant)
    combined = pd.concat([pd.DataFrame({"code": recommended["code"], "surge": recommended["surge"], "year": recommended["year"],
                                        "date": recommended["entry_date"], "ret": recommended[column], "source": "추천"}),
                          pd.DataFrame({"code": best_fills["code"], "surge": best_fills["surge"], "year": best_fills["year"],
                                        "date": best_fills["fill_date"], "ret": best_fills[f"ret_{best['stop']}_t{int(best['take'])}"],
                                        "source": "지정가"})], ignore_index=True)
    combined = combined.sort_values(["date", "source"], kind="mergesort").drop_duplicates(["code", "surge"], keep="first")
    row = summary(combined["ret"].to_numpy(float), combined["year"].to_numpy())
    overlap = len(set(zip(recommended["code"], recommended["surge"])) & set(zip(best_fills["code"], best_fills["surge"])))
    return (f"n{row['n']} {row['mean']:+.2f}% t{row['t']:+.2f} 승{row['win']:.1f}% | 10–17 {row['mean_early']:+.2f} n{row['n_early']} | "
            f"18–26 {row['mean_late']:+.2f} n{row['n_late']} (같은 사건 {overlap}건, 지정가 쪽 {int((combined['source'] == '지정가').sum())}건)")


def test_limit(fills: pd.DataFrame, trades: pd.DataFrame) -> tuple[list[str], pd.DataFrame]:
    lines = ["", "=" * 100, "시험 B 결과 — 되돌림 깊은 자리 지정가 매수", "=" * 100]
    main = fills[fills["variant"] == "main"]
    alternative = fills[fills["variant"] == "fill_limitdown"]
    lines += [f"체결 기록 {len(main):,}줄(사건 {fills.groupby(['code', 'surge']).ngroups:,}개 × 지정가 8, k 8 창 안 체결만). "
              f"하한가 마감 날 체결을 막아 결과가 달라진 (사건, 지정가) {len(alternative)}건",
              "주의: 매수 주문에서 하한가 날은 파는 쪽이 넘쳐 오히려 잘 체결되는 날이다. 그 날을 빼면 가장 나쁜 체결을 미리 피하는 셈이라",
              "      낙관 쪽으로 기운다. 그래서 '하한가 날도 지정가에 체결'을 보수 가정으로 같이 돌린다."]
    main_lines, main_cells = limit_variant(fills, "main", "본 규칙(지시대로): 하한가 마감 날 체결 못 함")
    conservative_lines, conservative_cells = limit_variant(fills, "fill_limitdown", "보수 가정: 하한가 마감 날도 지정가에 체결")
    lines += main_lines + conservative_lines
    label, wait, depth, move, entry, column = RECLAIM_SETS[0]
    recommended = reclaim_set(trades, wait, depth, move, entry, column)
    recommended_row = summary(recommended[column].to_numpy(float), recommended["year"].to_numpy())
    lines += ["", f"비교 기준 — {label}: n{recommended_row['n']} {recommended_row['mean']:+.2f}% t{recommended_row['t']:+.2f} | "
              f"10–17 {recommended_row['mean_early']:+.2f} | 18–26 {recommended_row['mean_late']:+.2f}"]

    for cells, variant, title in ((main_cells, "main", "본 규칙"), (conservative_cells, "fill_limitdown", "보수 가정")):
        passed = cells[cells["pass"]].sort_values("t", ascending=False)
        pool = passed if len(passed) else cells[cells["n"] >= NULL_MIN].sort_values("t", ascending=False)
        best = pool.iloc[0].to_dict()
        lines.append(f"{title} {'통과 칸 중' if len(passed) else '(통과 칸 없음) 384칸 중'} t 최고: " + format_cell(best))
        lines.append("  추천 칸과 같이 쓰면(같은 사건은 먼저 산 쪽만): " + union_line(recommended, column, fills, best, variant))

    return lines, pd.concat([main_cells, conservative_cells], ignore_index=True)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    # 빈 그룹(건수 0)의 평균·t 는 NaN 으로 표에 남기므로 그 경고는 끈다
    warnings.filterwarnings("ignore", category=RuntimeWarning)
    parser = argparse.ArgumentParser()
    parser.add_argument("--rebuild", action="store_true", help="매매·체결 parquet 를 다시 만든다")
    arguments = parser.parse_args()
    note = ""

    if arguments.rebuild or not TRADES_PATH.exists() or not FILLS_PATH.exists():
        trades, fills, counts = build()
        trades.to_parquet(TRADES_PATH, index=False)
        fills.to_parquet(FILLS_PATH, index=False)
        note = f", 이번 빌드에서 상한가 시가로 뺀 시가 매수 {counts['open_limit_skipped']}건"

    trades = pd.read_parquet(TRADES_PATH)
    fills = pd.read_parquet(FILLS_PATH)
    lines = [PLAN, "=" * 100, f"스터디 35 ma_reclaim_deep — 커밋 {optimize.git_commit()}, 자료 끝 {base.LAST_DATE}, seed {SEED}{note}",
             f"시험 A 매매 {len(trades):,}줄, 시험 B 체결 {len(fills):,}줄"]
    first = summary(reclaim_set(trades, 4, 15, 30, "close", "ret_low3_t15")["ret_low3_t15"].to_numpy(float),
                    reclaim_set(trades, 4, 15, 30, "close", "ret_low3_t15")["year"].to_numpy())
    recommended = summary(reclaim_set(trades, 4, 15, 30, "open", "ret_low3_t15")["ret_low3_t15"].to_numpy(float),
                          reclaim_set(trades, 4, 15, 30, "open", "ret_low3_t15")["year"].to_numpy())
    baseline = summary(reclaim_set(trades, 5, 20, 0, "close", "ret_low1_t20")["ret_low1_t20"].to_numpy(float),
                       reclaim_set(trades, 5, 20, 0, "close", "ret_low1_t20")["year"].to_numpy())
    lines.append(f"재현 점검: 1번 칸 n{first['n']} {first['mean']:+.2f} t{first['t']:+.2f} (기대 n264 +3.52 t4.32) | 추천 칸 n{recommended['n']} "
                 f"{recommended['mean']:+.2f} t{recommended['t']:+.2f} (기대 n263 +3.11 t3.82) | B5_20 n{baseline['n']} {baseline['mean']:+.2f} "
                 f"t{baseline['t']:+.2f} (기대 n350 +2.33 t3.0)")
    lines += test_reclaim(trades)
    limit_lines, cells = test_limit(fills, trades)
    lines += limit_lines
    cells.to_parquet(CELLS_PATH, index=False)
    text = "\n".join(lines)
    TEXT_PATH.write_bytes((text + "\n").encode("utf-8"))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
