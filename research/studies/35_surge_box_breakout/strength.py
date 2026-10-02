"""스터디 35 — "거래량 터진 상승봉 뒤 거래량이 줄면서 되돌림이 작은 종목이 강하다"(사용자 2026-10-02) 시험.

사건  : ma_sweep.py 와 같다(+10% 종가, 거래대금 비율 0.2%, 연속 급등은 묶음 하나, D0 = 묶음 마지막 날, 2010–2026 전체).
관찰  : D1..Dk, k = 2·3·5(내가 정함). 관찰 중 종가가 +10% 이상인 날이 오면 그 사건은 버린다(다음 묶음이 된다).
거래량 줄어듦 — streak : D1 < D0, D2 < D1, … 날마다 전날보다 적다.
               ratio X : D1..Dk 평균 거래량 ÷ D0 거래량 ≤ X, X = 0.3·0.5·0.7(내가 정함).
되돌림 작음  — D1..Dk 최저 저가 ≥ D0 종가 − Y × 상승폭(상승폭 = D0 종가 − 묶음 시작 전날 종가), Y = 20·30·50%.
매수  : Dk 종가. 조건이 확인된 날 바로 산다.
청산  : 다음 날부터. 손절 −7·−10%, 익절 +10·20·30%, 120거래일 종가(s10end). 같은 날 둘 다 닿으면 손절. 갭은 시가.
        lowbox = 관찰 기간 최저 저가 −1% 손절 + 20% 익절. 비용은 ma_sweep.COST 와 같다.
통과선: 전 기간 t ≥ 3, 이웃 칸 같은 부호, 2010–17·2018–26 둘 다 ≥ 0.
산출  : strength.parquet(사건 × k 한 줄), strength.txt(표). 재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/strength.py
"""
from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

_specification = importlib.util.spec_from_file_location("study35_ma_sweep", Path(__file__).resolve().parent / "ma_sweep.py")
sweep = importlib.util.module_from_spec(_specification)  # 이름이 backtest 패키지와 겹쳐 파일로 읽는다
sys.modules["study35_ma_sweep"] = sweep
_specification.loader.exec_module(sweep)

base = sweep.base
study33 = sweep.study33
WAITS = [2, 3, 5]
RATIOS = [0.3, 0.5, 0.7]
DEPTHS = [20, 30, 50]
CELLS = [("s7t10", 7, 10), ("s10t20", 10, 20), ("s10t30", 10, 30), ("s10end", 10, None)]


def build() -> pd.DataFrame:
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
        volume = series.volume.astype(float)
        raw_close = close * series.factor
        change = lambda row: (close[row] / close[row - 1] - 1.0) * 100.0

        for surge_row in range(1, rows - 1):
            if change(surge_row) < base.GAIN - 1e-9 or change(surge_row + 1) >= base.GAIN - 1e-9:
                continue

            run_start = surge_row

            while run_start > 1 and change(run_start - 1) >= base.GAIN - 1e-9:
                run_start -= 1

            share = max(relative.get((series.code, int(series.dates[row])), 0.0) for row in range(run_start, surge_row + 1))

            if share < base.SHARE_MIN or raw_close[run_start - 1] < study33.MIN_PREVIOUS_CLOSE_KRW or volume[surge_row] <= 0:
                continue

            move = close[surge_row] - close[run_start - 1]
            surge_volume = volume[surge_row]
            average20 = volume[max(surge_row - 20, 0):surge_row].mean()

            for wait in WAITS:
                row = surge_row + wait

                if row >= rows - 1:
                    break

                window = slice(surge_row + 1, row + 1)

                if any(change(day) >= base.GAIN - 1e-9 for day in range(surge_row + 2, row + 1)):
                    break

                last = row + base.HORIZON

                if last > rows - 1:
                    if not series.delisted:
                        continue

                    last = rows - 1

                path = volume[surge_row:row + 1]
                lowest = series.low[window].min()
                entry = float(close[row])
                record = {"code": series.code, "surge": int(series.dates[surge_row]), "year": int(series.dates[surge_row]) // 10000,
                          "run": surge_row - run_start + 1, "share": share, "wait": wait,
                          "streak": bool(np.all(np.diff(path) < 0)),
                          "ratio": volume[window].mean() / surge_volume,
                          "spike": surge_volume / max(average20, 1.0),
                          "depth": (close[surge_row] - lowest) / move * 100 if move > 0 else np.nan,
                          "hold": (close[row] / close[surge_row] - 1) * 100}

                for name, stop, take in CELLS:
                    record[name] = sweep.outcome(series, row, entry, entry * (1 - stop / 100), entry * (1 + take / 100) if take else math.inf, last)

                record["lowbox"] = sweep.outcome(series, row, entry, min(lowest * 0.99, entry * 0.999), entry * 1.2, last)
                records.append(record)

    return pd.DataFrame(records)


def stat(values: pd.Series) -> str:
    values = values.dropna()

    if len(values) < 20:
        return f"n{len(values)}"

    return f"{values.mean():+.2f} t{values.mean() / (values.std() / math.sqrt(len(values))):+.1f} n{len(values)}"


def line(label: str, part: pd.DataFrame, columns: list[str]) -> str:
    early, late = part[part.year < 2018], part[part.year >= 2018]
    cells = [f"{column} {stat(part[column])} [{stat(early[column]).split(' n')[0]} | {stat(late[column]).split(' n')[0]}]" for column in columns]
    return f"{label:34s} " + " ; ".join(cells)


def report(table: pd.DataFrame) -> str:
    columns = ["s10t20", "s10t30", "s10end", "lowbox"]
    lines = []

    for wait in WAITS:
        frame = table[table.wait == wait]

        for run_label, part in (("1일 급등", frame[frame.run == 1]), ("2일+ 급등", frame[frame.run >= 2])):
            lines.append(f"\n=== D{wait} 종가 매수, {run_label}")
            lines.append(line("전체(기준)", part, columns))
            lines.append(line("거래량 날마다 감소", part[part.streak], columns))

            for ratio in RATIOS:
                lines.append(line(f"거래량 평균 ≤ D0×{ratio}", part[part.ratio <= ratio], columns))

            for depth in DEPTHS:
                lines.append(line(f"되돌림 < {depth}%", part[part.depth < depth], columns))

            for depth in DEPTHS:
                lines.append(line(f"날마다 감소 + 되돌림 < {depth}%", part[part.streak & (part.depth < depth)], columns))

                for ratio in RATIOS:
                    lines.append(line(f"평균 ≤ {ratio} + 되돌림 < {depth}%", part[(part.ratio <= ratio) & (part.depth < depth)], columns))

    return "\n".join(lines)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    table = build()
    table.to_parquet(base.STUDY / "strength.parquet", index=False)
    text = f"사건 × 관찰일 {len(table):,}줄\n" + report(table)
    (base.STUDY / "strength.txt").write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
