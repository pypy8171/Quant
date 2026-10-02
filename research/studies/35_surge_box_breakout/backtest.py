"""스터디 35 — 급등 뒤 되돌림 없이 좁게 횡보하다 위로 나가는 자리. 머큐리(100590)·티엠씨(217590) 2026-10 차트형.

사건  : 급등일 D0 = 종가 +10% 이상, 전일 수정 전 종가 2,000원 이상, 거래대금(수정 전 종가 × 거래량)이 그날 시장 전체
        거래대금의 0.2% 이상. 시장이 커져 금액 기준은 연도마다 뜻이 달라서 비율로 건다(사용자 2026-10-02 "2010–2017
        거래대금을 지금과 같다고 생각하지 마라"). 0.2%는 요즘 시장 하루 20–30조 기준 약 500억(내가 정함).
        share = 그 비율(%), rank = 그날 거래대금 순위, turnover = 억 원. 시장 전체는 불러온 전 종목의 합.
        +10% 이상이 며칠 이어지면 그 묶음을 기준봉 하나로 본다 — 마지막 날이 D0, 상승폭은 묶음 전날 종가부터(run = 일수).
        HD현대에너지솔루션(322000) 2026-02-04 +29.8%·02-05 +16.2%를 따로 보면 02-04는 다음 날 바로 뻗어 버려지고 02-05는
        범위가 좁아 02-27 +30% 돌파를 놓쳤다(사용자 2026-10-02). share·rank 는 묶음 중 가장 큰 날, 비율 조건도 그날로 본다.
횡보  : 급등봉 상승폭 move = D0 종가 − 전일 종가. 범위 r 이면 D0 종가 ± r × move 안에서 종가가 머문 날(사용자 2026-10-02).
        예: +10% 급등, r 35% → D0 종가 기준 +3.5%–−3.5%. 판정은 종가로 한다(내가 정함). r = 20·25·30·35·40·50%.
        종가가 위로 나가면 돌파, 아래로 나가면 깨짐(사건 끝). 횡보 5일 전에 위로 나가면 "바로 뻗음"이라 버린다.
매수  : breakout_close — 횡보 5–30일 뒤 종가가 상단을 넘은 날 종가.
        breakout_next  — 같은 날의 다음 날 시가.
        pullback_*     — 돌파봉(종가가 상단을 넘은 날) 다음 날부터 10일 동안 지정가. 몸통(종가 − 시가) 25·50·75·100% 되돌림,
                         종가 −1·2·3·5%, 범위 상단, 상단 −2%. 저가가 뚫은 날만 체결(시가가 아래면 시가). 체결 전 종가가 범위
                         하단 아래면 취소(내가 정함). 체결일은 손절만 보고 익절은 다음 날부터(사용자 2026-10-02 "추격 대신 되돌림").
        box_maN        — 횡보 중 단기 이평선 지정가(사용자 2026-10-02 "돌파가 아니라 횡보·되돌림 때 단기 이평선에서 산다",
                         삼화콘덴서(001820) 09-29 돌파 매수가 5일선에서 너무 떨어졌다). 횡보 3일째 이후, 전날 5일선 ≥ 20일선이고
                         전날 종가가 N일선 위일 때 전날 N일선 값(호가 단위 내림)에 지정가. 저가가 닿으면 체결(시가가 아래면 시가),
                         범위마다 첫 체결 하나. 그날 종가가 범위를 벗어나도 체결은 남는다. N = 5·10·20. box_days = 체결 전까지 횡보 일수.
        box_maNc       — 같은 날 저가가 N일선에 닿고 종가가 그 선 위에서 끝난 날 종가(지지 확인 뒤 매수). 지정가 체결일
                         종가가 강하면 결과가 크게 갈려(r50 10일선 −10/+10: 종가 약 −3.2% t−9.8, 강 +2.3% t+7.0) 그 종가를 미리 알 수
                         없는 지정가 대신 종가에 산다(2026-10-02). hold = 종가 ÷ 그 선 − 1(%).
                         결과: 종가로 사면 그 차이가 사라진다(r50 −10/+20: 5일선 −0.74% t−3.7, 10일선 −0.66% t−3.0, 20일선 −1.16% t−3.6).
        squeeze_X      — 횡보 5일째 이후 5·10·20일선 간격이 종가의 X% 이하가 된 첫날의 다음 날 시가(X = 2, 3, 4, 5).
청산  : 매수 뒤 120거래일 안에서 가격으로만. 손절 15 × 익절 13(+없음) 칸, 고점 대비 하락 5칸, 이평선 이탈 3칸.
        같은 날 손절·익절 둘 다 닿으면 손절. 시가가 선을 넘어 열리면 시가. 종가 매수는 다음 날부터 본다.
        칸마다 [첫 도달일, 순수익%(비용 뺌)]만 적고, 화면이 손절일 ≤ 익절일이면 손절로 고른다.
기간  : 2010–2026 전체(사용자 2026-10-02). 연도별 표는 화면에서.
지표  : 소화·응축·폭발과 돈의 의도(사용자 2026-10-02 "소화가 끝난 뒤 힘이 쌓여 터진다", "큰 거래대금은 올리려는 의도").
        모두 돌파일 종가에 알 수 있는 값. 횡보 = D0 다음 날부터 돌파 전날까지.
        dry3      횡보 마지막 3일 평균 거래량 ÷ 급등일 거래량(작을수록 매물 소화 끝)
        contract  마지막 3일 평균 (고가−저가)/종가 ÷ 처음 3일 평균(작을수록 변동폭 축소), nr7 = 돌파 전날이 7일 중 가장 좁은 날
        burst     돌파일 거래량 ÷ 횡보 평균 거래량(돌파 때 다시 터짐)
        top3      마지막 3일 종가의 범위 안 위치 평균(0 하단, 1 상단 — 위에서 버팀)
        lowslope  횡보 저가의 하루 기울기 ÷ (r × 상승폭)(양수면 저가가 올라옴)
        depth     (급등일 종가 − 횡보 최저 저가) ÷ 급등봉 몸통 %(얕을수록 매물 흡수)
        spike60   급등일 거래대금 ÷ 직전 60일 평균, d0pos = 급등일 (종가−저가)/(고가−저가), low52 = 급등 전날 종가 ÷ 1년 최저가 − 1(%)
다시 사기: 돌파일 종가 매수(breakout_close)만. 보성파워텍(006910) 2026-02처럼 익절 뒤에도 추세가 이어지는 경우(사용자 2026-10-02).
        한 다리 청산 = 손절 −10%, 익절 +T%, 종가 20일선 아래. 익절로 판 경우에만 다시 산다 — 종가가 20일선 위·20일선이
        5일 전보다 높은 동안, 종가가 5일선 +2%(또는 10일선 +3%) 안으로 붙은 날이 나온 뒤, 종가가 전일 고가와 5일선을 함께
        넘는 날 종가에 산다. 종가가 20일선 아래로 가면 기다림을 끝낸다. 다시 사기는 3번까지, 첫 매수 뒤 120거래일 안.
        반 매도 = 절반은 +T% 익절, 나머지는 손절 −10% 또는 종가 20일선 이탈. 사건 손익은 다리 순수익의 합(다리마다 같은 금액).
        T, 붙는 기준, 횟수는 내가 정함.

산출: trades.json, candles/<연도>.json(차트용). 재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/backtest.py
"""
from __future__ import annotations

import importlib.util
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
_specification = importlib.util.spec_from_file_location("study33_backtest", STUDY.parent / "33_post_surge_pullback/backtest.py")
study33 = importlib.util.module_from_spec(_specification)
sys.modules["study33_backtest"] = study33
_specification.loader.exec_module(study33)

LAST_DATE = 20261002
GAIN = 10.0
SHARE_MIN = 0.2
BANDS = [20, 25, 30, 35, 40, 50]
BOX_MIN = 5
BOX_MAX = 30
HORIZON = 120
SQUEEZE = [2.0, 3.0, 4.0, 5.0]
TAKE = [3, 5, 7, 10, 12, 15, 20, 25, 30, 40, 50, 70, 100]
STOP = [2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 25]
STOP_NAMES = STOP + ["횡보 저가 −1%", "범위 하단 −1%", "급등일 시가 −1%"]
TRAIL = [5, 8, 10, 15, 20]
MA_EXIT = [5, 10, 20]
PULLBACK_DAYS = 10
PULLBACK_BODY = [25, 50, 75, 100]
PULLBACK_CLOSE = [1, 2, 3, 5]
MA_ENTRY = [5, 10, 20]
MA_ENTRY_MIN = 3
CHAIN_KINDS = ("breakout_close", "box_ma5c", "box_ma10c", "box_ma20c")
CHAIN_STOP = 10
CHAIN_TAKES = [10, 20, 30]
CHAIN_NEAR = {"ma5": (5, 2.0), "ma10": (10, 3.0)}
CHAIN_REBUYS = 3
CANDLES_BEFORE = 60
CANDLES_AFTER = 10


def moving(values: np.ndarray, window: int) -> np.ndarray:
    sums = np.cumsum(np.insert(values.astype(float), 0, 0.0))
    result = np.full(len(values), np.nan)
    result[window - 1:] = (sums[window:] - sums[:-window]) / window
    return result


def spread_percent(averages: list[np.ndarray], close: np.ndarray, row: int) -> float:
    values = [average[row] for average in averages]

    if any(math.isnan(value) for value in values):
        return float("nan")

    return (max(values) - min(values)) / close[row] * 100.0


def exits_for(series, entry_row: int, entry: float, mode: str, stop_levels: list[float], averages: dict) -> list:
    """[손절 15칸, 익절 13칸, 고점 대비 5칸, 이평 3칸, 120일 종가] 순서로 [도달일, 순수익%]. 안 닿으면 [-1, None].
    mode: close = 종가 매수(다음 날부터), open = 시가 매수(그날부터), limit = 장중 지정가(그날 손절만, 익절은 다음 날부터)."""
    last = min(entry_row + HORIZON, len(series.dates) - 1)
    window = slice(entry_row, last + 1)
    opens, highs, lows, closes = series.open_price[window], series.high[window], series.low[window], series.close[window]
    first = 1 if mode == "close" else 0
    take_first = 0 if mode == "open" else 1
    net = lambda price: round(study33.net_percent(entry, price), 2)
    result = []

    def fill(day: int, level: float, below: bool) -> list:
        if day == 0:
            return [0, net(level)]

        return [day, net(min(opens[day], level) if below else max(opens[day], level))]

    for level in stop_levels:
        if level >= entry:  # 매수 때 이미 손절선 아래 — 지정가는 체결가에, 종가 매수는 다음 날 시가에 바로 판다(그날 시가는 체결 전이라 못 쓴다)
            result.append([first, net(entry if mode == "limit" else opens[first])] if first < len(opens) else [-1, None])
            continue

        hits = np.nonzero(lows[first:] <= level)[0]
        result.append(fill(int(hits[0]) + first, level, True) if len(hits) else [-1, None])

    for value in TAKE:
        level = entry * (1 + value / 100)
        hits = np.nonzero(highs[take_first:] >= level)[0]
        result.append(fill(int(hits[0]) + take_first, level, False) if len(hits) else [-1, None])

    initial = min(stop_levels[-3], entry * 0.999)  # 횡보 저가 −1%

    for value in TRAIL:
        outcome = [-1, None]
        peak = entry

        for day in range(first, len(opens)):
            level = max(initial, peak * (1 - value / 100))

            if day and opens[day] <= level:
                outcome = [day, net(opens[day])]
                break

            if lows[day] <= level:
                outcome = [day, net(level)]
                break

            peak = max(peak, highs[day])

        result.append(outcome)

    for window_size in MA_EXIT:
        average = averages[window_size][window]
        outcome = [-1, None]

        for day in range(first, len(opens)):
            if lows[day] <= initial:
                outcome = [day, net(min(opens[day], initial) if day else initial)]
                break

            if day and closes[day] < average[day]:
                outcome = [day, net(closes[day])]
                break

        result.append(outcome)

    complete = entry_row + HORIZON <= len(series.dates) - 1 or series.delisted
    result.append([len(opens) - 1, net(closes[-1])] if complete else [-1, None])
    return result


def leg_exit(series, start_row: int, entry: float, take: float | None, last: int, averages: dict) -> tuple:
    """start_row 종가에 산 다리를 다음 날부터 본다. 손절 → 익절 → 종가 20일선 이탈 순. (청산 행, 가격, 방식)."""
    stop = entry * (1 - CHAIN_STOP / 100)
    target = entry * (1 + take / 100) if take else math.inf

    for row in range(start_row + 1, last + 1):
        if series.low[row] <= stop:
            return row, min(series.open_price[row], stop), "stop"

        if series.high[row] >= target:
            return row, max(series.open_price[row], target), "take"

        if series.close[row] < averages[20][row]:
            return row, series.close[row], "ma20"

    return last, series.close[last], "end"


def rebuy_row(series, after_row: int, last: int, averages: dict, near: tuple) -> int:
    """익절 뒤 추세가 살아 있는 동안 이격이 다시 붙었다가 오르는 날. 없으면 -1."""
    window, limit = near
    seen = False

    for row in range(after_row + 1, last + 1):
        trend = averages[20][row]

        if math.isnan(trend) or series.close[row] < trend:
            return -1

        if series.close[row] <= averages[window][row] * (1 + limit / 100):
            seen = True
            continue

        rising = trend > averages[20][row - 5]

        if seen and rising and series.close[row] > series.high[row - 1] and series.close[row] > averages[5][row]:
            return row

    return -1


def chains_for(series, entry_row: int, entry: float, averages: dict) -> dict | None:
    """청산 방식마다 [사건 순수익% 합, 다리 [매수일, 매수가, 매도일, 매도가, 방식, 순수익%] 목록]. 120일이 안 찼으면 None."""
    if entry_row + HORIZON > len(series.dates) - 1 and not series.delisted:
        return None

    last = min(entry_row + HORIZON, len(series.dates) - 1)
    net = lambda buy, sell: round(study33.net_percent(buy, sell), 2)
    date = lambda row: int(series.dates[row])
    result = {}
    configs = [(f"t{take}", take, None) for take in CHAIN_TAKES] + [("t0", None, None)]
    configs += [(f"t{take}_{name}", take, near) for take in CHAIN_TAKES for name, near in CHAIN_NEAR.items()]

    for key, take, near in configs:
        legs, row, price = [], entry_row, entry

        while True:
            exit_row, exit_price, how = leg_exit(series, row, price, take, last, averages)
            legs.append([date(row), round(price, 2), date(exit_row), round(float(exit_price), 2), how, net(price, exit_price)])

            if how != "take" or near is None or len(legs) > CHAIN_REBUYS:
                break

            row = rebuy_row(series, exit_row, last, averages, near)

            if row < 0:
                break

            price = float(series.close[row])

        result[key] = [round(sum(leg[5] for leg in legs), 2), legs]

    for take in CHAIN_TAKES[:2]:
        exit_row, exit_price, how = leg_exit(series, entry_row, entry, take, last, averages)
        legs = [[date(entry_row), round(entry, 2), date(exit_row), round(float(exit_price), 2), how, net(entry, exit_price)]]

        if how == "take":
            rest_row, rest_price, rest_how = leg_exit(series, exit_row - 1, entry, None, last, averages) if exit_row - 1 >= entry_row else (exit_row, exit_price, how)
            legs.append([date(entry_row), round(entry, 2), date(rest_row), round(float(rest_price), 2), rest_how, net(entry, rest_price)])
            result[f"h{take}"] = [round((legs[0][5] + legs[1][5]) / 2, 2), legs]
        else:
            result[f"h{take}"] = [legs[0][5], legs]

    return result


def box_signals(series, run_start: int, surge_row: int, row: int, upper: float, lower: float, width: float, burst: bool = True) -> dict:
    """돌파일 row 종가에 아는 소화·응축·폭발 값. 횡보는 surge_row+1 .. row−1(5일 이상). 기준봉 = run_start..surge_row 묶음."""
    volume, high, low, close = series.volume.astype(float), series.high, series.low, series.close
    box = slice(surge_row + 1, row)
    last3, first3 = slice(row - 3, row), slice(surge_row + 1, surge_row + 4)
    span = (high - low) / close
    lows = low[box]
    body = close[surge_row] - series.open_price[run_start]
    signals = {"dry3": volume[last3].mean() / max(volume[run_start:surge_row + 1].max(), 1.0),
               "contract": span[last3].mean() / max(span[first3].mean(), 1e-9),
               "nr7": bool((high - low)[row - 1] <= (high - low)[max(surge_row + 1, row - 7):row].min()),
               "burst": volume[row] / max(volume[box].mean(), 1.0) if burst else None,
               "top3": ((close[last3] - lower) / max(upper - lower, 1e-9)).mean(),
               "lowslope": np.polyfit(np.arange(len(lows)), lows, 1)[0] / max(width, 1e-9),
               "depth": (close[surge_row] - lows.min()) / body * 100 if body > 0 else None}
    return {key: value if isinstance(value, bool) or value is None else round(float(value), 3) for key, value in signals.items()}


def without_nan(value):
    """JSON 에는 NaN 이 없다 — 화면이 파일을 못 읽는다. 빈 값으로 바꾼다."""
    if isinstance(value, float) and math.isnan(value):
        return None

    if isinstance(value, dict):
        return {key: without_nan(item) for key, item in value.items()}

    if isinstance(value, list):
        return [without_nan(item) for item in value]

    return value


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    stocks, _, _ = study33.load_inputs(LAST_DATE)
    trades, pullbacks, statuses, watching = [], [], defaultdict(int), []
    candle_rows: dict[str, dict] = defaultdict(dict)
    surges = 0
    # 날짜마다 시장 전체 거래대금과, 급등 후보(+10% 이상)의 그날 순위
    frames = []

    for series in stocks:
        if len(series.dates) >= 2:
            frames.append(pd.DataFrame({"code": series.code, "date": series.dates,
                                        "turnover": series.close * series.factor * series.volume,
                                        "change": np.r_[np.nan, series.close[1:] / series.close[:-1] - 1.0] * 100.0}))

    market = pd.concat(frames, ignore_index=True)
    market["rank"] = market.groupby("date")["turnover"].rank(ascending=False, method="min")
    market["share"] = market["turnover"] / market.groupby("date")["turnover"].transform("sum") * 100.0
    candidates = market[market["change"] >= GAIN - 1e-9]
    relative = {(code, int(date)): (share, int(rank)) for code, date, share, rank
                in zip(candidates["code"], candidates["date"], candidates["share"], candidates["rank"])}
    del market, frames, candidates

    for series in stocks:
        rows = len(series.dates)

        if rows < 30:
            continue

        close = series.close
        raw_close = close * series.factor
        turnover = raw_close * series.volume
        averages = {window: moving(close, window) for window in (5, 10, 20, 60, 120)}
        short_set = [averages[5], averages[10], averages[20]]
        long_set = short_set + [averages[60]]

        for surge_row in range(1, rows - 1):
            day_change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0

            if day_change(surge_row) < GAIN - 1e-9 or day_change(surge_row + 1) >= GAIN - 1e-9:
                continue  # 다음 날도 급등이면 묶음이 이어진다 — 마지막 날에 한 번만 본다

            run_start = surge_row

            while run_start > 1 and day_change(run_start - 1) >= GAIN - 1e-9:
                run_start -= 1

            share, rank = max(relative.get((series.code, int(series.dates[row])), (0.0, 0)) for row in range(run_start, surge_row + 1))
            change = (close[surge_row] / close[run_start - 1] - 1.0) * 100.0

            if share < SHARE_MIN:
                continue

            if raw_close[run_start - 1] < study33.MIN_PREVIOUS_CLOSE_KRW or series.open_price[run_start] <= 0:
                continue

            surges += 1
            start = max(0, run_start - 250)
            drawdown = (close[run_start - 1] / close[start:run_start].max() - 1.0) * 100.0 if run_start - start >= 120 else None
            average120 = averages[120][run_start - 1]
            spike60 = turnover[run_start:surge_row + 1].max() / turnover[run_start - 60:run_start].mean() if run_start >= 60 else None
            day_range = series.high[surge_row] - series.low[surge_row]
            move = close[surge_row] - close[run_start - 1]
            base_open = series.open_price[run_start]
            base = {"code": series.code, "name": series.name, "surge": int(series.dates[surge_row]),
                    "gain": round(change, 1), "run": surge_row - run_start + 1, "turnover": round(turnover[run_start:surge_row + 1].max() / 1e8),
                    "share": round(share, 2), "rank": rank,
                    "drawdown": round(drawdown, 1) if drawdown is not None else None,
                    "below120_before": None if math.isnan(average120) else bool(close[run_start - 1] < average120),
                    "spike60": round(float(spike60), 1) if spike60 is not None and np.isfinite(spike60) else None,
                    "d0pos": round(float((close[surge_row] - series.low[surge_row]) / day_range), 2) if day_range > 0 else None,
                    "low52": round(float((close[run_start - 1] / series.low[start:run_start].min() - 1) * 100), 1) if run_start - start >= 120 else None}
            last_entry_row = -1

            for band in BANDS:
                upper = close[surge_row] + band / 100 * move
                lower = close[surge_row] - band / 100 * move
                box_low, box_high = math.inf, -math.inf
                found = {value: False for value in SQUEEZE}
                touched = {window: False for window in MA_ENTRY}
                held = {window: False for window in MA_ENTRY}
                entries, status = [], "expired"

                for row in range(surge_row + 1, min(surge_row + BOX_MAX + 2, rows)):
                    days = row - surge_row - 1  # 이날 전까지 범위 안에 있던 날 수

                    if days >= MA_ENTRY_MIN and averages[5][row - 1] >= averages[20][row - 1]:
                        for window in MA_ENTRY:
                            level = averages[window][row - 1]

                            if math.isnan(level) or close[row - 1] <= level:
                                continue

                            level = series.floor_to_tick(level, row)

                            if not held[window] and series.low[row] <= level <= close[row]:
                                held[window] = True
                                entries.append((f"box_ma{window}c", row, float(close[row]), "close", days, box_high, box_low, row,
                                                {**box_signals(series, run_start, surge_row, row, upper, lower, band / 100 * move),
                                                 "hold": round(float((close[row] / level - 1) * 100), 1)}))

                            if touched[window]:
                                continue

                            if series.low[row] <= level:
                                touched[window] = True
                                entries.append((f"box_ma{window}", row, float(min(series.open_price[row], level)), "limit", days, box_high, box_low, row,
                                                box_signals(series, run_start, surge_row, row, upper, lower, band / 100 * move, burst=False)))

                    if close[row] > upper:
                        if days < BOX_MIN:
                            status = "early_run"
                            break

                        status = "breakout"
                        common = (days, box_high, box_low, row)
                        bar_open, bar_high, bar_low = series.open_price[row], series.high[row], series.low[row]
                        shape = {"wick": round((bar_high - close[row]) / max(bar_high - bar_low, 1e-9) * 100, 1),
                                 "body": round((close[row] - bar_open) / max(bar_high - bar_low, 1e-9) * 100, 1),
                                 **box_signals(series, run_start, surge_row, row, upper, lower, band / 100 * move)}
                        entries.append(("breakout_close", row, float(close[row]), "close", *common, shape))

                        if row + 1 < rows:
                            entries.append(("breakout_next", row + 1, float(series.open_price[row + 1]), "open", *common, shape))

                        levels = {f"pullback_body{value}": close[row] - value / 100 * (close[row] - bar_open)
                                  for value in PULLBACK_BODY if close[row] > bar_open}
                        levels.update({f"pullback_close{value}": close[row] * (1 - value / 100) for value in PULLBACK_CLOSE})
                        levels.update({"pullback_upper": upper, "pullback_upper2": upper * 0.98})

                        for kind, level in levels.items():
                            level = series.floor_to_tick(level, row)

                            if level >= close[row]:
                                continue

                            for fill_row in range(row + 1, min(row + PULLBACK_DAYS + 1, rows)):
                                if close[fill_row - 1] < lower:
                                    break

                                if series.low[fill_row] < level:
                                    price = float(min(series.open_price[fill_row], level))
                                    entries.append((kind, fill_row, price, "limit", *common, {**shape, "fill_day": fill_row - row}))
                                    break

                        break

                    if close[row] < lower:
                        status = "broken"
                        break

                    if days + 1 > BOX_MAX:
                        break

                    box_low, box_high = min(box_low, series.low[row]), max(box_high, series.high[row])

                    if days + 1 >= BOX_MIN and row + 1 < rows:
                        spread = spread_percent(short_set, close, row)

                        for value in SQUEEZE:
                            if not found[value] and spread <= value:
                                found[value] = True
                                entries.append((f"squeeze_{value:g}", row + 1, float(series.open_price[row + 1]), "open",
                                                days + 1, box_high, box_low, row, {}))

                if status == "expired" and surge_row + BOX_MAX + 1 >= rows:
                    status = "watching"
                    watching.append({**base, "band": band, "box_days": rows - surge_row - 1,
                                     "upper": round(float(upper * series.factor[-1]), 0), "lower": round(float(lower * series.factor[-1]), 0),
                                     "last_close": round(float(close[-1] * series.factor[-1]), 0),
                                     "spread3": round(spread_percent(short_set, close, rows - 1), 2)})
                    last_entry_row = max(last_entry_row, rows - 1)

                statuses[f"{band}:{status}"] += 1

                for kind, entry_row, entry, mode, days, box_high_value, box_low_value, signal_row, extra in entries:
                    stops = [entry * (1 - value / 100) for value in STOP] + [box_low_value * 0.99, lower * 0.99,
                                                                             base_open * 0.99]
                    volume_ratio = series.volume[surge_row + 1:signal_row + 1].mean() / series.volume[run_start:surge_row + 1].max()
                    (pullbacks if kind.startswith("pullback") else trades).append({**base, **extra, "band": band, "kind": kind, "box_days": days,
                                   "box_width": round((box_high_value / box_low_value - 1) * 100, 1),
                                   "spread3": round(spread_percent(short_set, close, signal_row), 2),
                                   "spread4": round(spread_percent(long_set, close, signal_row), 2),
                                   "above120": None if math.isnan(averages[120][signal_row]) else bool(close[signal_row] > averages[120][signal_row]),
                                   "box_volume": round(float(volume_ratio), 2),
                                   "signal": int(series.dates[signal_row]), "entry_date": int(series.dates[entry_row]),
                                   "entry": round(entry, 2),
                                   "chase": round((entry / upper - 1) * 100, 1),  # 매수가가 범위 상단보다 몇 % 위인가
                                   "from_surge": round((entry / close[surge_row] - 1) * 100, 1),
                                   "signal_gain": round((close[signal_row] / close[signal_row - 1] - 1) * 100, 1), "exits": exits_for(series, entry_row, entry, mode, stops, averages),
                                   "gap5": round(float((entry / averages[5][entry_row - 1] - 1) * 100), 1),  # 매수가가 전날 5일선보다 몇 % 위인가
                                   "gap20": round(float((entry / averages[20][entry_row - 1] - 1) * 100), 1),
                                   **({"chains": chains_for(series, entry_row, entry, averages)} if kind in CHAIN_KINDS else {})})
                    last_entry_row = max(last_entry_row, entry_row)

            if last_entry_row >= 0:
                first = max(0, run_start - CANDLES_BEFORE)
                last = min(rows - 1, last_entry_row + HORIZON + CANDLES_AFTER)
                year = str(int(series.dates[surge_row]) // 10000)
                candle_rows[year][f"{series.code}_{int(series.dates[surge_row])}"] = [
                    [int(series.dates[index]), round(float(series.open_price[index]), 1), round(float(series.high[index]), 1),
                     round(float(series.low[index]), 1), round(float(close[index]), 1), int(series.volume[index])]
                    for index in range(first, last + 1)]

    (STUDY / "candles").mkdir(exist_ok=True)

    for year, payload in candle_rows.items():
        (STUDY / "candles" / f"{year}.json").write_text(json.dumps(payload, separators=(",", ":")), encoding="utf-8")

    meta = {"take": TAKE, "stop": STOP_NAMES, "trail": TRAIL, "ma": MA_EXIT, "horizon": HORIZON, "bands": BANDS,
            "squeeze": SQUEEZE, "surges": surges, "chain_stop": CHAIN_STOP, "chain_takes": CHAIN_TAKES, "statuses": statuses, "last_date": LAST_DATE}
    meta["watching"] = watching
    meta["cost_ratio"] = 1 + study33.net_percent(10000.0, 10000.0) / 100  # 비용이 가격에 비례해 대시보드가 순수익에서 매도가를 되짚는다
    (STUDY / "trades.json").write_text(json.dumps({"meta": meta}, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    # 매매는 방식마다 나눈다 — 합치면 아티팩트 파일 한도(16MB)를 넘는다. 대시보드가 고른 방식만 읽는다
    (STUDY / "kinds").mkdir(exist_ok=True)
    by_kind = defaultdict(list)

    for trade in trades + pullbacks:
        by_kind[trade["kind"]].append(trade)

    for kind, part in by_kind.items():
        (STUDY / "kinds" / f"{kind}.json").write_text(
            json.dumps(without_nan(part), ensure_ascii=False, separators=(",", ":"), allow_nan=False), encoding="utf-8")
    kinds = defaultdict(int)

    for trade in trades + pullbacks:
        kinds[(trade["band"], trade["kind"])] += 1

    print("급등 사건", surges)
    print("범위별 상태:", dict(statuses))
    print("범위·매수별 건수:", dict(kinds))
    print("지금 횡보 중:", [(item["name"], item["surge"], item["band"]) for item in watching])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
