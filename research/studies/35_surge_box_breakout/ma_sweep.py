"""스터디 35 — 횡보 중 단기 이평선 매수 전수 비교와 "소화 끝" 지표 탐색(사용자 2026-10-02).

요청  : "3·5·7·…·21일선 다 해봐", "거래량이 줄면서 하락이 더 진행 안 되는 것 같은 걸 찾아 시험해".
사건  : backtest.py 와 같다(급등 묶음, 거래대금 비율 0.2%, 범위 r = D0 종가 ± r × 상승폭, 종가로 판정). 범위 r = 30·35·40·50.
매수  : 횡보 3일째 이후, 범위 안에 있는 동안, 범위마다 이평선마다 첫 번 하나.
        limit  — 전날 종가가 전날 N일선 위, 전날 5일선 ≥ 20일선이면 전날 N일선(호가 내림)에 지정가. 저가가 닿으면 체결(시가가 아래면 시가).
        close  — 같은 조건에서 그날 저가가 N일선에 닿고 종가가 그 위면 종가에 산다.
        dig    — 이평선 없이 "소화 끝"만으로: 횡보 5일째 이후, 마지막 3일 평균 거래량 ÷ 급등 묶음 최대 거래량 ≤ X 이고
                 횡보 최저 저가가 K일 넘게 새로 안 나왔고 그날 종가가 전날보다 높으면 종가에 산다. X·K 는 격자(내가 정함).
        N = 3·5·7·9·11·13·15·17·19·21.
지표  : 모두 판단 시점까지 아는 값(limit = 전날까지, close·dig = 그날 종가까지). 횡보 = D0 다음 날부터 판단일까지.
        dry3     마지막 3일 평균 거래량 ÷ 급등 묶음 최대 거래량
        vfall    마지막 3일 평균 거래량 ÷ 횡보 첫 3일 평균
        since_low 횡보 최저 저가가 나온 뒤 지난 날 수(0 = 판단일에 새 저가) — "하락이 더 진행 안 됨"
        downup   하락 마감일 평균 거래량 ÷ 상승 마감일 평균 거래량(작을수록 파는 힘이 마름)
        obv      횡보 중 (종가 방향 × 거래량) 합 ÷ 거래량 합
        depth    (D0 종가 − 횡보 최저 저가) ÷ 급등 묶음 몸통 %, pos = 판단일 종가의 범위 안 위치(0 하단, 1 상단)
        contract 마지막 3일 평균 변동폭 ÷ 첫 3일 평균, volnow = 판단일 거래량 ÷ 횡보 평균, slope = N일선 3일 변화 %
청산  : 다음 날부터 본다. 손절 −7·−10%, 익절 +10·20·30%, 120거래일 종가. 같은 날 둘 다 닿으면 손절. 갭은 시가.
        boxlow = 판단 시점 횡보 최저 저가 −1% 손절 + 20% 익절. 비용은 study33.net_percent 와 같은 비율.
결과(2026-10-02): 이평선 10개 × 두 방식 × 범위 4개 × 청산 6개 480칸 전부 0 이하. dig 24칸도 전부 음수. 지표 하나씩(5등분)도
        거래량 마름(dry3·vfall)·저가 멈춤(since_low)은 오히려 낮은 쪽이 나빴다. 두 지표 조합 40,329번 중 t≥3·두 기간 양수 13개(우연
        기대치보다 적음). 되풀이된 것은 하나 — 급등 묶음 2일 이상 + depth < 30(급등 몸통의 70% 이상 지킴) + close 방식:
        사건당 한 건 r50 −10/+20 +1.34%(t 2.2, 572건) [2010–17 +1.10 | 2018–26 +1.43], depth < 20 +2.58%(t 2.9). 같은 조건
        1일 급등은 −1.33%(t −3.6). 지정가(limit)로는 0 근처. t 3 에 못 미쳐 채택하지 않음.
산출  : ma_sweep.parquet(매수 한 건 한 줄). 재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/ma_sweep.py
"""
from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

_specification = importlib.util.spec_from_file_location("study35_backtest", Path(__file__).resolve().parent / "backtest.py")
base = importlib.util.module_from_spec(_specification)  # 이름이 backtest 패키지와 겹쳐 파일로 읽는다
sys.modules["study35_backtest"] = base
_specification.loader.exec_module(base)

study33 = base.study33
WINDOWS = [3, 5, 7, 9, 11, 13, 15, 17, 19, 21]
BANDS = [30, 35, 40, 50]
ENTRY_MIN = 3
DIG_DRY = [0.15, 0.2, 0.3, 0.4]
DIG_LOW = [2, 3, 5]
CELLS = [("s7t10", 7, 10), ("s10t10", 10, 10), ("s10t20", 10, 20), ("s10t30", 10, 30), ("s10end", 10, None)]
COST = 1 + study33.net_percent(100000.0, 100000.0) / 100  # 왕복 비용 비율


def outcome(series, entry_row: int, entry: float, stop_price: float, take_price: float, last: int) -> float:
    """entry_row 다음 날부터 last 까지. 순수익 %."""
    opens = series.open_price[entry_row + 1:last + 1]
    lows = series.low[entry_row + 1:last + 1]
    highs = series.high[entry_row + 1:last + 1]
    stop_hits = np.flatnonzero(lows <= stop_price)
    take_hits = np.flatnonzero(highs >= take_price)
    stop_at = stop_hits[0] if len(stop_hits) else math.inf
    take_at = take_hits[0] if len(take_hits) else math.inf

    if stop_at <= take_at and stop_at < math.inf:
        price = min(opens[stop_at], stop_price)
    elif take_at < math.inf:
        price = max(opens[take_at], take_price)
    else:
        price = series.close[last]

    return (price / entry * COST - 1) * 100


def features(series, run_start: int, surge_row: int, known: int, upper: float, lower: float, average: np.ndarray | None) -> dict:
    """known 행 종가까지 아는 값. 횡보 = surge_row+1 .. known."""
    volume = series.volume.astype(float)
    close, high, low = series.close, series.high, series.low
    box = slice(surge_row + 1, known + 1)
    box_volume, box_close, box_low = volume[box], close[box], low[box]
    previous = close[surge_row:known]
    up = box_close > previous
    down = box_close < previous
    span = (high - low) / close
    run_max = max(volume[run_start:surge_row + 1].max(), 1.0)
    body = close[surge_row] - series.open_price[run_start]
    last3 = slice(known - 2, known + 1)
    first3 = slice(surge_row + 1, surge_row + 4)
    signals = {"dry3": volume[last3].mean() / run_max,
               "vfall": volume[last3].mean() / max(volume[first3].mean(), 1.0),
               "since_low": len(box_low) - 1 - int(np.argmin(box_low)),
               "downup": box_volume[down].mean() / max(box_volume[up].mean(), 1.0) if up.any() and down.any() else np.nan,
               "obv": float((np.sign(box_close - previous) * box_volume).sum() / max(box_volume.sum(), 1.0)),
               "depth": (close[surge_row] - box_low.min()) / body * 100 if body > 0 else np.nan,
               "pos": (close[known] - lower) / max(upper - lower, 1e-9),
               "contract": span[last3].mean() / max(span[first3].mean(), 1e-9),
               "volnow": volume[known] / max(box_volume.mean(), 1.0),
               "slope": (average[known] / average[known - 3] - 1) * 100 if average is not None else np.nan}
    return signals


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
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
    records = []

    for series in stocks:
        rows = len(series.dates)

        if rows < 30:
            continue

        close = series.close
        raw_close = close * series.factor
        averages = {window: base.moving(close, window) for window in set(WINDOWS) | {20, 60}}
        change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0

        for surge_row in range(1, rows - 1):
            if change(surge_row) < base.GAIN - 1e-9 or change(surge_row + 1) >= base.GAIN - 1e-9:
                continue

            run_start = surge_row

            while run_start > 1 and change(run_start - 1) >= base.GAIN - 1e-9:
                run_start -= 1

            share = max(relative.get((series.code, int(series.dates[row])), 0.0) for row in range(run_start, surge_row + 1))

            if share < base.SHARE_MIN or raw_close[run_start - 1] < study33.MIN_PREVIOUS_CLOSE_KRW or series.open_price[run_start] <= 0:
                continue

            move = close[surge_row] - close[run_start - 1]
            event = {"code": series.code, "surge": int(series.dates[surge_row]), "year": int(series.dates[surge_row]) // 10000,
                     "run": surge_row - run_start + 1, "share": share}

            for band in BANDS:
                upper = close[surge_row] + band / 100 * move
                lower = close[surge_row] - band / 100 * move
                done: set = set()

                for row in range(surge_row + 1, min(surge_row + base.BOX_MAX + 2, rows)):
                    days = row - surge_row - 1
                    entries = []

                    if days >= ENTRY_MIN and averages[5][row - 1] >= averages[20][row - 1]:
                        for window in WINDOWS:
                            level = averages[window][row - 1]

                            if math.isnan(level) or close[row - 1] <= level:
                                continue

                            level = series.floor_to_tick(level, row)

                            if ("limit", window) not in done and series.low[row] <= level:
                                done.add(("limit", window))
                                entries.append(("limit", window, row - 1, float(min(series.open_price[row], level))))

                            if ("close", window) not in done and series.low[row] <= level <= close[row]:
                                done.add(("close", window))
                                entries.append(("close", window, row, float(close[row])))

                    if days >= 5 and close[row] > close[row - 1]:
                        known = features(series, run_start, surge_row, row, upper, lower, None)

                        for dry in DIG_DRY:
                            for quiet in DIG_LOW:
                                if ("dig", dry, quiet) not in done and known["dry3"] <= dry and known["since_low"] >= quiet:
                                    done.add(("dig", dry, quiet))
                                    entries.append((f"dig{dry}_{quiet}", 0, row, float(close[row])))

                    for mode, window, known_row, entry in entries:
                        last = row + base.HORIZON

                        if last > rows - 1:
                            if not series.delisted:
                                continue

                            last = rows - 1

                        if last <= row:
                            continue

                        record = {**event, "band": band, "mode": mode, "window": window, "days": days, "row_date": int(series.dates[row]),
                                  "entry": entry, "gap5": (entry / averages[5][row - 1] - 1) * 100,
                                  **features(series, run_start, surge_row, known_row, upper, lower, averages[window] if window else None)}
                        box_low = series.low[surge_row + 1:known_row + 1].min()

                        for name, stop, take in CELLS:
                            record[name] = outcome(series, row, entry, entry * (1 - stop / 100), entry * (1 + take / 100) if take else math.inf, last)

                        record["boxlow"] = outcome(series, row, entry, min(box_low * 0.99, entry * 0.999), entry * 1.2, last)
                        records.append(record)

                    if close[row] > upper or close[row] < lower:
                        break

    table = pd.DataFrame(records)
    table.to_parquet(base.STUDY / "ma_sweep.parquet", index=False)
    print(f"매수 {len(table):,}건", table.groupby(["mode", "window"]).size().to_string())
    return 0


if __name__ == "__main__":
    sys.exit(main())
