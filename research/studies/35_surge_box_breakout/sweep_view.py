"""스터디 35 대시보드 "탐색" 탭 자료 — ma_sweep.parquet·strength.parquet 를 표와 후보 매매 목록으로 줄여 sweep.json 에 쓴다.

표    : 이평선 N × 매수 방식(limit·close) × 범위 r 의 −10/+20 평균·t·건수, 소화 끝(dig) 24칸, 강한 종목 시험(strength.py) 전 칸.
후보  : A — 급등 2일+ · 종가 확인 이평 매수(N ≤ 11) · 눌림 < 20%·30%, 범위 r50, 사건당 첫 매수 하나(ma_sweep 결과 문단의 그 후보).
        B — 급등 2일+ · D3·D5 종가 매수 · 되돌림 < 20%·30%(strength.py).
        매매마다 −10/+20 과 관찰 저가 −1%/+20 두 청산의 매도일·매도가를 다시 계산한다(차트 표시용, 수익률은 ma_sweep.outcome 과 같은 식).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/sweep_view.py  (ma_sweep.py·strength.py 를 먼저 돌린다)
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
import strength  # noqa: E402  같은 폴더 — ma_sweep·backtest 를 파일로 읽어 둔다

sweep = strength.sweep
base = strength.base
study33 = strength.study33
STUDY = base.STUDY


def stat(values: pd.Series) -> list:
    values = values.dropna()

    if len(values) < 10:
        return [None, None, int(len(values))]

    return [round(float(values.mean()), 2), round(float(values.mean() / (values.std() / math.sqrt(len(values)))), 1), int(len(values))]


def split(part: pd.DataFrame, column: str) -> dict:
    return {"all": stat(part[column]), "early": stat(part[part.year < 2018][column]), "late": stat(part[part.year >= 2018][column])}


def exit_path(series, entry_row: int, entry: float, stop_price: float, take_price: float, last: int) -> tuple[int, float, str]:
    """ma_sweep.outcome 과 같은 판정에 매도일·사유를 더한다."""
    lows = series.low[entry_row + 1:last + 1]
    highs = series.high[entry_row + 1:last + 1]
    opens = series.open_price[entry_row + 1:last + 1]
    stop_hits = np.flatnonzero(lows <= stop_price)
    take_hits = np.flatnonzero(highs >= take_price)
    stop_at = stop_hits[0] if len(stop_hits) else math.inf
    take_at = take_hits[0] if len(take_hits) else math.inf

    if stop_at <= take_at and stop_at < math.inf:
        return entry_row + 1 + int(stop_at), float(min(opens[stop_at], stop_price)), "손절"

    if take_at < math.inf:
        return entry_row + 1 + int(take_at), float(max(opens[take_at], take_price)), "익절"

    return last, float(series.close[last]), "기간"


def trade(series, row: int, entry: float, lowest: float, event: dict) -> dict:
    last = min(row + base.HORIZON, len(series.dates) - 1)
    result = {**event, "entry_date": int(series.dates[row]), "entry": round(entry, 1)}

    for name, stop_price in (("s10t20", entry * 0.9), ("lowbox", min(lowest * 0.99, entry * 0.999))):
        exit_row, price, how = exit_path(series, row, entry, stop_price, entry * 1.2, last)
        result[name] = [int(series.dates[exit_row]), round(price, 1), how, exit_row - row, round((price / entry * sweep.COST - 1) * 100, 2)]

    return result


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    moving = pd.read_parquet(STUDY / "ma_sweep.parquet")
    power = pd.read_parquet(STUDY / "strength.parquet")
    tables = {"ma": [], "dig": [], "strength": []}

    for (mode, window, band), part in moving[moving["mode"].isin(["limit", "close"])].groupby(["mode", "window", "band"]):
        tables["ma"].append({"mode": mode, "window": int(window), "band": int(band), **{cell: split(part, cell) for cell in ("s10t20", "s10end", "boxlow")}})

    for (mode, band), part in moving[moving["mode"].str.startswith("dig")].groupby(["mode", "band"]):
        tables["dig"].append({"mode": mode, "band": int(band), "s10t20": split(part, "s10t20"), "boxlow": split(part, "boxlow")})

    conditions = [("전체(기준)", lambda part: part.index == part.index), ("거래량 날마다 감소", lambda part: part.streak)]
    conditions += [(f"거래량 평균 ≤ D0×{ratio}", lambda part, ratio=ratio: part.ratio <= ratio) for ratio in strength.RATIOS]
    conditions += [(f"되돌림 < {depth}%", lambda part, depth=depth: part.depth < depth) for depth in strength.DEPTHS]

    for depth in strength.DEPTHS:
        conditions.append((f"날마다 감소 + 되돌림 < {depth}%", lambda part, depth=depth: part.streak & (part.depth < depth)))
        conditions += [(f"평균 ≤ {ratio} + 되돌림 < {depth}%", lambda part, ratio=ratio, depth=depth: (part.ratio <= ratio) & (part.depth < depth))
                       for ratio in strength.RATIOS]

    for wait in strength.WAITS:
        for run_label, run_mask in (("1일", power.run == 1), ("2일+", power.run >= 2)):
            part = power[(power.wait == wait) & run_mask]

            for label, rule in conditions:
                chosen = part[rule(part)]
                tables["strength"].append({"wait": wait, "run": run_label, "rule": label,
                                           **{cell: split(chosen, cell) for cell in ("s7t10", "s10t20", "s10t30", "s10end", "lowbox")}})

    stocks, _, _ = study33.load_inputs(base.LAST_DATE)
    by_code = {series.code: series for series in stocks}
    names = {}

    for kind in ("breakout_close", "pullback_body50", "squeeze_5"):
        for item in json.loads((STUDY / "kinds" / f"{kind}.json").read_text(encoding="utf-8")):
            names[item["code"]] = item["name"]

    candles = {}

    for path in sorted((STUDY / "candles").glob("*.json")):
        candles.update(dict.fromkeys(json.loads(path.read_text(encoding="utf-8")).keys(), True))

    sets = {}
    first = moving[(moving["mode"] == "close") & (moving.window <= 11) & (moving.band == 50) & (moving.run >= 2)]
    first = first.sort_values(["row_date", "window"]).drop_duplicates(["code", "surge"])

    for depth in (20, 30):
        rows = []

        for record in first[first.depth < depth].itertuples():
            series = by_code[record.code]
            row = int(np.searchsorted(series.dates, record.row_date))
            surge_row = int(np.searchsorted(series.dates, record.surge))
            lowest = series.low[surge_row + 1:row + 1].min()
            event = {"code": record.code, "name": names.get(record.code, record.code), "surge": int(record.surge), "run": int(record.run),
                     "gain": round(float((series.close[surge_row] / series.close[surge_row - int(record.run)] - 1) * 100), 1),
                     "depth": round(float(record.depth), 1), "detail": f"{int(record.window)}일선 · 횡보 {int(record.days)}일",
                     "chart": f"{record.code}_{int(record.surge)}" in candles}
            rows.append(trade(series, row, float(record.entry), lowest, event))

        sets[f"A{depth}"] = rows

    for wait in (3, 5):
        for depth in (20, 30):
            rows = []
            chosen = power[(power.wait == wait) & (power.run >= 2) & (power.depth < depth)]

            for record in chosen.itertuples():
                series = by_code[record.code]
                surge_row = int(np.searchsorted(series.dates, record.surge))
                row = surge_row + wait
                lowest = series.low[surge_row + 1:row + 1].min()
                event = {"code": record.code, "name": names.get(record.code, record.code), "surge": int(record.surge), "run": int(record.run),
                         "gain": round(float((series.close[surge_row] / series.close[surge_row - int(record.run)] - 1) * 100), 1),
                         "depth": round(float(record.depth), 1), "detail": f"D{wait} 거래량 {record.ratio:.2f}배",
                         "chart": f"{record.code}_{int(record.surge)}" in candles}
                rows.append(trade(series, row, float(series.close[row]), lowest, event))

            sets[f"B{wait}_{depth}"] = rows

    payload = {"cost_ratio": sweep.COST, "tables": tables, "sets": sets}
    (STUDY / "sweep.json").write_text(json.dumps(payload, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print({key: len(value) for key, value in sets.items()}, "차트 있음", {key: sum(item["chart"] for item in value) for key, value in sets.items()})
    return 0


if __name__ == "__main__":
    sys.exit(main())
