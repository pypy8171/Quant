"""스터디 34 리플레이 자료 — 고른 칸(위치 ≤ −50%, 거래량 ≥ 10배) 매매마다 캔들·근거를 만든다.

스터디 33 리플레이 화면(research/studies/33_post_surge_pullback/replay/index.html)의 세 번째 탭이 읽는다.
매매는 "버림 없음" 칸 전부를 싣고, 위로 뻗으면 버림(급등일 종가 × 1.05)에 걸린 매매는 filtered 표시를 단다.
판정 숫자는 filtered 가 아닌 매매만 센다. 걸러진 매매도 끝까지 따라간 결과를 보여 줘 조건이 무엇을 거르는지 본다.

입력: 같은 폴더 candidates.tsv(backtest.py 산출), grid.tsv, 스터디 33 trades_zone.tsv(rule none — 매수·청산이 같은 매매),
      PYQuant/data/bars_all_pit_v2.parquet.
산출: 스터디 33 replay/s34_<구간>.json, replay/s34/<구간>_<번호>.json(차트 묶음).
재실행(저장소 루트, backtest.py 다음): py -X utf8 research/studies/34_bottom_surge_zone/build_replay.py
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
STUDY33 = STUDY.parent / "33_post_surge_pullback"
_specification = importlib.util.spec_from_file_location("study33_replay", STUDY33 / "build_replay.py")
replay = importlib.util.module_from_spec(_specification)
_specification.loader.exec_module(replay)

DRAWDOWN = -50.0
VOLUME = 10.0
RUNUP_LIMIT = 1.05
PREFIX = "s34"
DEV_PARTS = [("2010–2013", 20100101, 20131231), ("2014–2017", 20140101, 20171231), ("2018–2022", 20180101, 20221231)]


def chosen(frame: pd.DataFrame, base: str) -> pd.DataFrame:
    rule = frame[frame["runup_base"] == base]
    return rule[(rule["drawdown_pct"] <= DRAWDOWN) & (rule["volume_multiple"] >= VOLUME)]


def runup_text(group: pd.DataFrame, item: dict) -> str:
    """걸러진 매매에 언제 기준을 넘었는지 한 줄. 매수일 전 고가, 또는 매수일 시가로 판정한다(backtest.find_buy 와 같음)."""
    dates = group["date_int"].to_numpy(np.int64)
    surge_row = replay.row_of(dates, item["surge"])
    buy_row = replay.row_of(dates, item["buy"])
    level = float(group["Close"].iloc[surge_row]) * RUNUP_LIMIT

    for row in range(surge_row + 1, buy_row + 1):
        if row == buy_row and group["Open"].iloc[row] >= level:
            return f"위로 뻗으면 버림: 매수일 {replay.date_text(dates[row])} 시가 {group['Open'].iloc[row]:,.0f} ≥ 기준 {level:,.0f}(급등일 종가 × 1.05)"

        if row < buy_row and group["High"].iloc[row] >= level:
            return (f"위로 뻗으면 버림: {replay.date_text(dates[row])} 고가 {group['High'].iloc[row]:,.0f} ≥ 기준 {level:,.0f}"
                    f"(급등일 종가 × 1.05) → 실제 규칙에서는 사지 않는다. 비교하려고 끝까지 따라간 결과다")

    return f"위로 뻗으면 버림 기준 {level:,.0f}(급등일 종가 × 1.05)"


def main() -> int:
    candidates = pd.read_csv(STUDY / "candidates.tsv", sep="\t", dtype={"code": str})
    grid = pd.read_csv(STUDY / "grid.tsv", sep="\t")
    everything = chosen(candidates, "off")
    kept_keys = set(zip(chosen(candidates, "close")["code"], chosen(candidates, "close")["surge_date"]))
    high_keys = set(zip(chosen(candidates, "high")["code"], chosen(candidates, "high")["surge_date"]))

    trades = pd.read_csv(STUDY33 / "trades_zone.tsv", sep="\t", dtype={"code": str})
    trades = trades[trades["rule"] == "none"]
    trades = trades.merge(everything[["code", "surge_date"]], on=["code", "surge_date"]).reset_index(drop=True)
    bars = replay.load_bars(sorted(trades["code"].unique()))
    built = []

    for _, trade in trades.iterrows():
        group = bars[trade["code"]]
        item = replay.build_trade(trade, group)
        key = (trade["code"], int(trade["surge_date"]))
        item["filtered"] = bool(key not in kept_keys)
        item["filtered_high"] = key not in high_keys
        item["runup_level"] = replay.round_price(float(group["Close"].iloc[replay.row_of(group["date_int"].to_numpy(np.int64), item["surge"])]) * RUNUP_LIMIT)
        item["location"] = "걸러진 매매(위로 뻗음)" if item["filtered"] else "남은 매매(판정)"
        item["why_buy"] = item["why_buy"] + [runup_text(group, item) if item["filtered"]
                                             else f"위로 뻗으면 버림: 매수 전까지 기준 {item['runup_level']:,.0f}(급등일 종가 × 1.05)을 넘지 않음"]
        built.append(item)

    missing = len(everything) - len(built)
    print(f"매매 {len(built)}건(후보 {len(everything)}건, 못 맞춘 것 {missing}건)")
    replay.OUT_DIR.mkdir(exist_ok=True)

    for period in replay.PERIOD_NAMES:
        items = [item for item in built if item["period"] == period]
        table = pd.DataFrame([{"net": item["net"], "surge": item["surge"], "filtered": item["filtered"],
                               "filtered_high": item["filtered_high"]} for item in items])
        kept = table[~table["filtered"]]
        rows = [
            {"axis": "위로 뻗으면 버림", "band": "급등일 종가 × 1.05 넘으면 버림 (판정 칸)", **replay.summarize(kept)},
            {"axis": "위로 뻗으면 버림", "band": "급등일 고가 × 1.05 넘으면 버림 (참고)", **replay.summarize(table[~table["filtered_high"]])},
            {"axis": "위로 뻗으면 버림", "band": "버림 없음 (비교)", **replay.summarize(table)},
            {"axis": "위로 뻗으면 버림", "band": "걸러진 매매만", **replay.summarize(table[table["filtered"]])},
        ]

        if period == "development":
            for name, first, last in DEV_PARTS:
                part = kept[(kept["surge"] >= first) & (kept["surge"] <= last)]
                rows.append({"axis": "개발 소구간(판정 칸)", "band": name, **replay.summarize(part)})

            for _, cell in grid.iterrows():
                rows.append({"axis": "개발 20칸 격자", "band": f"위치 ≤ {cell['drawdown_max']:g}% · 거래량 ≥ {cell['volume_min']:g}배",
                             "count": int(cell["count"]), "average": round(float(cell["average"]), 2),
                             "win_rate": float(cell["win_rate"]), "t": float(cell["t"]),
                             "chosen": bool(cell["drawdown_max"] == DRAWDOWN and cell["volume_min"] == VOLUME)})

        light = replay.split_chunks(items, period, PREFIX)
        payload = {"period": period, "period_name": replay.PERIOD_NAMES[period], "trades": light,
                   "summary": replay.summarize(kept), "location": rows}
        (replay.OUT_DIR / f"{PREFIX}_{period}.json").write_text(
            json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False), encoding="utf-8")
        print(f"{period}: {len(light)}건(남은 {len(kept)}) → replay/{PREFIX}_{period}.json")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
