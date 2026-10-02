"""스터디 34 — 바닥권 거래량 급등 뒤 40–60% 되돌림 매수. 규칙은 SPEC.md(2026-10-02 사전등록).

스터디 33 zone 칸(research/studies/33_post_surge_pullback/backtest_zone.py, 거래량 조건 없음)에 세 가지를 더한다.
  급등 전 위치  : 전일 종가 ÷ 직전 250거래일 종가 최고 − 1 ≤ D
  거래량 배수   : 급등일 거래량 ÷ 직전 20거래일 평균 ≥ V
  위로 뻗으면 버림: 지켜보는 날 고가가 급등일 종가 × 1.05 이상이면 버린다(매수일 당일은 시가만 본다)
D·V는 개발 구간(2010–2022) 20칸 격자에서 고른다(SPEC 3절). 검증·최근 1년은 참고로만 적는다.

입력: 스터디 33 backtest.py load_inputs 와 같음(PYQuant/data/bars_all_pit_v2.parquet 외).
산출: candidates.tsv(위치·거래량을 거르기 전 모든 매수 건과 그 값. runup_base = close 판정 칸 / high 고가 기준 참고 /
      off 버림 없음 비교), grid.tsv(개발 구간 격자),
      result.tsv(고른 칸의 구간별·하위 구간별 숫자), 화면에 통과 여부.
재실행(저장소 루트): py -X utf8 research/studies/34_bottom_surge_zone/backtest.py
"""
from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
STUDY33 = STUDY.parent / "33_post_surge_pullback"
_specification = importlib.util.spec_from_file_location("study33_backtest", STUDY33 / "backtest.py")
study33 = importlib.util.module_from_spec(_specification)
sys.modules["study33_backtest"] = study33
_specification.loader.exec_module(study33)

GAIN = 10.0
WINDOW = 5
HOLD = 10
ZONE_UPPER = 0.4
ZONE_LOWER = 0.6
RUNUP_LIMIT = 1.05        # 지켜보는 날 고가가 급등일 종가의 이 배수 이상이면 버림(사용자 2026-10-02)
LOOKBACK_YEAR = 250
MIN_HISTORY = 120
LOOKBACK_VOLUME = 20
LAST_DATE = 20260929
PERIODS = [("development", 20100101, 20221231), ("validation", 20230101, 20250930), ("recent_1y", 20251001, LAST_DATE)]
DEV_PARTS = [("2010–2013", 20100101, 20131231), ("2014–2017", 20140101, 20171231), ("2018–2022", 20180101, 20221231)]
DRAWDOWN_GRID = [-30.0, -40.0, -50.0, -60.0]
VOLUME_GRID = [3.0, 5.0, 7.0, 10.0, 15.0]
MIN_COUNT = 300
PASS_T = 2.0


def period_of(date_int: int) -> str | None:
    for name, first, last in PERIODS:
        if first <= date_int <= last:
            return name

    return None


def surge_features(series, surge_row: int) -> tuple[float, float]:
    """급등 전 위치(%)와 거래량 배수. 급등일 종가까지의 값만 쓴다."""
    close, volume = series.close, series.volume
    history_start = max(0, surge_row - LOOKBACK_YEAR)
    drawdown = float("nan")

    if surge_row - history_start >= MIN_HISTORY:
        drawdown = (close[surge_row - 1] / close[history_start:surge_row].max() - 1.0) * 100.0

    volume_start = max(0, surge_row - LOOKBACK_VOLUME)
    average_volume = volume[volume_start:surge_row].mean() if surge_row > volume_start else float("nan")
    multiple = volume[surge_row] / average_volume if average_volume and average_volume > 0 else float("nan")
    return drawdown, multiple


def find_buy(series, rows: int, surge_row: int, flags: np.ndarray, zone_top: float, zone_bottom: float,
             runup_level: float) -> tuple[int, str]:
    """(매수 행, 상태). 상태: bought | watching | runup | gap_below | broke_open | replaced | expired."""
    surge_open = series.open_price[surge_row]

    for row in range(surge_row + 1, surge_row + WINDOW + 1):
        if row >= rows:
            return -1, "watching"

        if flags[row]:
            return -1, "replaced"

        if series.open_price[row] >= runup_level:
            return -1, "runup"

        if series.open_price[row] < zone_bottom:
            return -1, "gap_below"

        if series.low[row] <= zone_top:
            return row, "bought"

        if series.low[row] < surge_open:
            return -1, "broke_open"

        if series.high[row] >= runup_level:
            return -1, "runup"

    return -1, "expired"


def trade_event(series, rows: int, surge_row: int, flags: np.ndarray, runup_base: str = "close") -> dict | None:
    """급등 한 건을 규칙대로 걷는다. 매수 전 버림·지켜보는 중도 상태로 돌려준다(그림자 기록이 쓴다)."""
    surge_open, surge_close = series.open_price[surge_row], series.close[surge_row]
    body = surge_close - surge_open

    if body <= 0:
        return None

    zone_top = surge_close - ZONE_UPPER * body
    zone_bottom = surge_close - ZONE_LOWER * body
    runup_level = {"close": surge_close * RUNUP_LIMIT, "high": series.high[surge_row] * RUNUP_LIMIT, "off": math.inf}[runup_base]
    drawdown, multiple = surge_features(series, surge_row)
    event = {"code": series.code, "name": series.name, "surge_date": int(series.dates[surge_row]),
             "surge_change_pct": (surge_close / series.close[surge_row - 1] - 1.0) * 100.0,
             "drawdown_pct": drawdown, "volume_multiple": multiple, "zone_top": zone_top, "zone_bottom": zone_bottom,
             "runup_level": runup_level, "target": series.high[surge_row], "status": "", "buy_date": None, "entry": None,
             "stop": None, "exit_date": None, "exit_price": None, "exit_reason": None, "net_pct": None}
    buy_row, status = find_buy(series, rows, surge_row, flags, zone_top, zone_bottom, runup_level)
    event["status"] = status

    if buy_row < 0:
        return event

    at_open = series.open_price[buy_row] <= zone_top
    entry = series.tick_up(series.open_price[buy_row], buy_row) if at_open else series.floor_to_tick(zone_top, buy_row)
    stop = series.floor_to_tick(surge_open * study33.STOP_BELOW_LOW, buy_row)
    event.update(buy_date=int(series.dates[buy_row]), entry=entry, stop=stop)

    if series.low[buy_row] <= stop:
        exit_row, exit_price, reason, closed = buy_row, series.tick_down(stop, buy_row), "stop_same_day", True
    elif buy_row + 1 >= rows:
        exit_row, exit_price, reason, closed = buy_row, None, "holding", False
    else:
        result = study33.run_exit(series, rows, buy_row + 1, entry, stop, series.high[surge_row], HOLD - 1, series.delisted)
        exit_row, exit_price, reason, closed = result.exit_row, result.exit_price, result.reason, result.closed

    if not closed:
        event["status"] = "holding"
        return event

    event.update(status="closed", exit_date=int(series.dates[exit_row]), exit_price=exit_price, exit_reason=reason,
                 net_pct=study33.net_percent(entry, exit_price))
    return event


def summary_of(values: pd.Series) -> dict:
    return {"count": int(len(values)), "average": round(float(values.mean()), 3) if len(values) else None,
            "win_rate": round(float((values > 0).mean() * 100), 1) if len(values) else None,
            "t": round(float(study33.t_value(values)), 2) if len(values) > 2 else None}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    stocks, _, _ = study33.load_inputs(LAST_DATE)
    records = []

    for series in stocks:
        rows = len(series.dates)
        flags = study33.surge_flags(series, rows, GAIN)

        for surge_row in np.nonzero(flags)[0]:
            period = period_of(int(series.dates[surge_row]))

            if period is None:
                continue

            for base in ("close", "high", "off"):
                event = trade_event(series, rows, surge_row, flags, base)

                if event is not None and event["status"] == "closed":
                    records.append({"period": period, "runup_base": base, **event})

    candidates = pd.DataFrame(records)
    candidates.to_csv(STUDY / "candidates.tsv", sep="\t", index=False)
    main_rule = candidates[candidates["runup_base"] == "close"]
    development = main_rule[main_rule["period"] == "development"]

    grid_rows = []

    for drawdown in DRAWDOWN_GRID:
        for multiple in VOLUME_GRID:
            cell = development[(development["drawdown_pct"] <= drawdown) & (development["volume_multiple"] >= multiple)]
            grid_rows.append({"drawdown_max": drawdown, "volume_min": multiple, **summary_of(cell["net_pct"])})

    grid = pd.DataFrame(grid_rows)
    grid.to_csv(STUDY / "grid.tsv", sep="\t", index=False)
    print(grid.to_string(index=False))

    eligible = grid[grid["count"] >= MIN_COUNT]

    if eligible.empty:
        print("건수 300 이상인 칸이 없다 — 보류")
        return 0

    chosen = eligible.sort_values("t", ascending=False).iloc[0]
    drawdown, multiple = float(chosen["drawdown_max"]), float(chosen["volume_min"])

    def pick(frame: pd.DataFrame) -> pd.DataFrame:
        return frame[(frame["drawdown_pct"] <= drawdown) & (frame["volume_multiple"] >= multiple)]

    result_rows = []

    for base in ("close", "high", "off"):
        rule = candidates[candidates["runup_base"] == base]

        for name, _, _ in PERIODS:
            result_rows.append({"runup_base": base, "slice": name, **summary_of(pick(rule[rule["period"] == name])["net_pct"])})

        for name, first, last in DEV_PARTS:
            part = rule[(rule["surge_date"] >= first) & (rule["surge_date"] <= last)]
            result_rows.append({"runup_base": base, "slice": f"development {name}", **summary_of(pick(part)["net_pct"])})

    result = pd.DataFrame(result_rows)
    result.to_csv(STUDY / "result.tsv", sep="\t", index=False)
    print(f"\n고른 칸: 위치 ≤ {drawdown:g}%, 거래량 ≥ {multiple:g}배")
    print(result.to_string(index=False))

    parts = result[(result["runup_base"] == "close") & result["slice"].str.startswith("development 2")]
    positive_parts = int((parts["average"] > 0).sum())
    passed = float(chosen["t"]) >= PASS_T and positive_parts >= 2
    print(f"\n통과 조건: t {chosen['t']} (≥ {PASS_T}), 하위 구간 플러스 {positive_parts}/3 (≥ 2) → {'통과' if passed else '통과 못 함'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
