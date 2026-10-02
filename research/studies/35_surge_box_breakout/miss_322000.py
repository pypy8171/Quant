"""스터디 35 — HD현대에너지솔루션(322000) 2026-02-04 – 2026-02-26 구간을 규칙들이 왜 놓쳤나(사용자 2026-10-02). 관찰만 하고 새 칸은 만들지 않는다.

표 1 : 2026-01-01 – 2026-03-15 일봉(시가·고가·저가·종가·등락·거래량·거래대금 시장 비중·5/10/20일선).
표 2 : 그 기간의 +10% 묶음과, 1번 칸·B5_20 의 첫 매수 판정(rebuy.first_check, optimize.build 와 같은 조건)과 청산,
       다시 사기(rebuy.chain, 칸 = rebuy.txt 의 가장 좋은 칸과 N5·40일·익절 뒤만·+15)의 체결.
표 3 : A20(sweep_view.py — 급등 2일+ · 범위 r50 · 종가 확인 이평 매수 N ≤ 11 · 사건당 첫 매수 · 몸통 대비 눌림 < 20%)의
       실제 매수(ma_sweep.parquet)와, D0 다음 날부터 날마다 N = 3·5·7·9·11 마다 걸린 조건(ma_sweep.main 의 close 방식 조건을 읽어서 적음).
표 4 : 다시 사기 조건(그날 저가 ≤ N일선 ≤ 종가, 5일선 ≥ 20일선, 종가 ≥ 20일선)이 날마다 섰는지 N = 5·10·20.
표 5 : 02-04 – 02-26 가격 흐름(구간 저점·고점·02-26 종가, 그 안에서 샀을 때 벌 수 있던 폭)과 02-27 뒤 열흘.
산출 : miss_322000.txt. 재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/miss_322000.py
"""
from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
_specification = importlib.util.spec_from_file_location("study35_rebuy", HERE / "rebuy.py")
rebuy = importlib.util.module_from_spec(_specification)  # 이름이 backtest 패키지와 겹쳐 파일로 읽는다
sys.modules["study35_rebuy"] = rebuy
_specification.loader.exec_module(rebuy)

base = rebuy.base
sweep = rebuy.sweep
view = rebuy.view
study33 = rebuy.study33
CODE = "322000"
TABLE_FROM, TABLE_TO = 20260101, 20260315
FOCUS_FROM, FOCUS_TO = 20260204, 20260226
A_WINDOWS = [3, 5, 7, 9, 11]
A_BAND = 50
A_DEPTH = 20
TEXT_PATH = HERE / "miss_322000.txt"


def best_cells() -> dict:
    """rebuy.txt 를 다시 돌리지 않고 원장에서 칸별 t 를 내 첫 매수 규칙마다 가장 좋은 칸을 고른다."""
    ledger = pd.read_parquet(rebuy.LEDGER_PATH)
    result = {}

    for label, part in ledger.groupby("rule"):
        scores = part.groupby(rebuy.AXES)["ret"].apply(lambda values: rebuy.t_value(values.to_numpy(float)))
        result[label] = dict(zip(rebuy.AXES, scores.idxmax()))

    return result


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    stocks, _, _ = study33.load_inputs(base.LAST_DATE)
    turnover = np.concatenate([item.close * item.factor * item.volume for item in stocks])
    totals = pd.Series(turnover).groupby(np.concatenate([item.dates for item in stocks])).sum().to_dict()
    series = next(item for item in stocks if item.code == CODE)
    del stocks
    share_of = lambda item, row: item.close[row] * item.factor[row] * item.volume[row] / totals[int(item.dates[row])] * 100
    cache: dict = {}
    averages = rebuy.averages_of(series, cache)
    moving = {window: base.moving(series.close, window) for window in sorted(set(A_WINDOWS) | {5, 10, 20})}
    close, low, high, opens = series.close, series.low, series.high, series.open_price
    date = lambda row: int(series.dates[row])
    start = int(np.searchsorted(series.dates, TABLE_FROM))
    end = int(np.searchsorted(series.dates, TABLE_TO, side="right")) - 1
    lines = [f"스터디 35 miss_322000 — {series.name}({CODE}), 자료 끝 {base.LAST_DATE}, 커밋 {rebuy.optimize.git_commit()}", "",
             "표 1. 일봉 (가격은 수정주가, 비중 = 그날 시장 전체 거래대금 중 %)",
             "  날짜       시가     고가     저가     종가    등락%   거래량        비중%   5일선    10일선   20일선"]

    for row in range(start, end + 1):
        change = (close[row] / close[row - 1] - 1) * 100
        lines.append(f"  {date(row)} {opens[row]:8,.0f} {high[row]:8,.0f} {low[row]:8,.0f} {close[row]:8,.0f} {change:+6.1f} {series.volume[row]:12,.0f} "
                     f"{share_of(series, row):6.2f} {averages[5][row]:8,.0f} {averages[10][row]:8,.0f} {averages[20][row]:8,.0f}")

    best = best_cells()
    preset = {"window": 5, "limit": 40, "after": "take", "take": 15}
    lines += ["", "표 2. +10% 묶음과 1번 칸·B5_20 첫 매수 판정, 청산, 다시 사기"]

    for run_start, surge_row in rebuy.surge_runs(series, start, end):
        lines.append(f"  묶음 {date(run_start)}–{date(surge_row)} (D0 {date(surge_row)}, D0 종가 {close[surge_row]:,.0f}, 묶음 전날 종가 {close[run_start - 1]:,.0f})")

        for wait in range(1, 8):
            row = surge_row + wait

            if row < len(close):
                lowest = low[surge_row + 1:row + 1].min()
                depth = (close[surge_row] - lowest) / (close[surge_row] - close[run_start - 1]) * 100
                lines.append(f"    D{wait} {date(row)} 종가 {close[row]:,.0f} 관찰 최저 저가 {lowest:,.0f} 되돌림 {depth:.1f}% "
                             f"등락 {(close[row] / close[row - 1] - 1) * 100:+.1f}%")

        for label, rule in rebuy.FIRST_RULES.items():
            reason, row, stop_price, _ = rebuy.first_check(series, run_start, surge_row, rule, share_of)
            lines.append(f"    [{label}] {reason}")

            if row < 0:
                continue

            entry = float(close[row])
            last = min(row + base.HORIZON, len(close) - 1)
            exit_row, price, how = view.exit_path(series, row, entry, stop_price, entry * (1 + rule["take"] / 100), last)
            kind = {"손절": 1, "익절": 2, "기간": 0}[how]
            lines.append(f"      첫 매수 {date(row)} {entry:,.0f} 손절가 {stop_price:,.0f} 익절가 {entry * (1 + rule['take'] / 100):,.0f} → "
                         f"{date(exit_row)} {price:,.0f} {how} {(price / entry * sweep.COST - 1) * 100:+.2f}%")

            for cell in ([best[label]] if best[label] != preset else []) + [preset]:
                legs, _ = rebuy.chain(series, row, exit_row, kind, averages, cell, allow_open=True)
                text = "; ".join(f"{leg['entry_date']} {leg['entry']:,.0f} → {leg['exit_date']} {leg['exit']:,.0f} {leg['how']} {leg['ret']:+.2f}%"
                                 for leg in legs) or "다시 사기 없음"
                lines.append(f"      다시 사기 [{rebuy.cell_name(cell)}] {text}")

    lines += ["", f"표 3. A20 (r{A_BAND}, 종가 확인 N ≤ 11, 몸통 대비 눌림 < {A_DEPTH}%) — ma_sweep.parquet 실제 매수"]
    moving_table = pd.read_parquet(HERE / "ma_sweep.parquet")
    chosen = moving_table[(moving_table["code"] == CODE) & (moving_table["mode"] == "close") & (moving_table["window"] <= 11)
                          & (moving_table["band"] == A_BAND) & (moving_table["run"] >= 2)
                          & (moving_table["surge"] >= TABLE_FROM) & (moving_table["surge"] <= TABLE_TO)]
    first = chosen.sort_values(["row_date", "window"]).drop_duplicates(["code", "surge"])

    for record in chosen.sort_values("row_date").itertuples():
        mark = " ← 사건당 첫 매수" if record.Index in first.index else ""
        lines.append(f"  D0 {record.surge} {record.row_date} {int(record.window)}일선 종가 {record.entry:,.0f} 몸통 대비 눌림 {record.depth:.1f}%"
                     f"{' (< 20 통과)' if record.depth < A_DEPTH else ' (≥ 20 탈락)'}{mark}")

    for record in first.itertuples():
        row = int(np.searchsorted(series.dates, record.row_date))
        surge_row = int(np.searchsorted(series.dates, record.surge))
        lowest = low[surge_row + 1:row + 1].min()
        result = view.trade(series, row, float(record.entry), lowest, {})
        lines.append(f"  A20 매매 {record.row_date} {record.entry:,.0f}: −10/+20 → {result['s10t20']}, 관찰 저가 −1%/+20 → {result['lowbox']}"
                     "  [매도일, 매도가, 사유, 보유일, 순수익%]")

    lines += ["", "  날마다 A 조건(N 마다: 전날 5일선≥20일선 / 전날 종가 > 전날 N일선 / 저가 ≤ 전날 N일선(호가 내림) ≤ 종가). 횡보 3일째부터, 종가가 r50 범위 밖이면 끝"]

    for run_start, surge_row in rebuy.surge_runs(series, start, end):
        if surge_row - run_start + 1 < 2:
            continue

        move = close[surge_row] - close[run_start - 1]
        upper, lower = close[surge_row] + A_BAND / 100 * move, close[surge_row] - A_BAND / 100 * move
        body = close[surge_row] - opens[run_start]
        lines.append(f"  D0 {date(surge_row)}: r50 범위 {lower:,.0f}–{upper:,.0f}, 몸통 {opens[run_start]:,.0f}→{close[surge_row]:,.0f}")

        for row in range(surge_row + 1, min(surge_row + base.BOX_MAX + 2, len(close))):
            days = row - surge_row - 1
            depth = (close[surge_row] - low[surge_row + 1:row + 1].min()) / body * 100
            trend = moving[5][row - 1] >= moving[20][row - 1]
            parts = []

            for window in A_WINDOWS:
                level = series.floor_to_tick(moving[window][row - 1], row)

                if close[row - 1] <= moving[window][row - 1]:
                    parts.append(f"{window}:전날종가≤선")
                elif not (low[row] <= level <= close[row]):
                    parts.append(f"{window}:{'안 닿음' if low[row] > level else '종가<선'}({level:,.0f})")
                else:
                    parts.append(f"{window}:닿음({level:,.0f})")

            note = "횡보 3일 전" if days < rebuy.base.MA_ENTRY_MIN else ("전날 5<20" if not trend else "")
            lines.append(f"    {date(row)} 횡보{days:2d}일 종가 {close[row]:,.0f} 눌림 {depth:5.1f}% {note:9s} " + " ".join(parts))

            if close[row] > upper or close[row] < lower:
                lines.append(f"    → 종가 {close[row]:,.0f} 범위 {'위' if close[row] > upper else '아래'}로 나감, 이 사건 끝")
                break

    lines += ["", "표 4. 다시 사기 조건이 선 날(저가 ≤ N일선 ≤ 종가, 5일선 ≥ 20일선, 종가 ≥ 20일선) — 첫 매수 청산 뒤에만 실제로 산다"]

    for row in range(int(np.searchsorted(series.dates, FOCUS_FROM)) - 3, int(np.searchsorted(series.dates, 20260313, side="right"))):
        fast, slow = averages[5][row], averages[20][row]
        parts = []

        for window in rebuy.WINDOWS:
            level = averages[window][row]
            touch = low[row] <= level <= close[row]
            parts.append(f"{window}일선 {level:,.0f} {'닿고 위' if touch else ('안 닿음' if low[row] > level else '종가<선')}")

        trend = "추세 살아 있음" if fast >= slow and close[row] >= slow else f"추세 아님(5일선 {fast:,.0f} vs 20일선 {slow:,.0f})"
        lines.append(f"  {date(row)} 저가 {low[row]:,.0f} 종가 {close[row]:,.0f} | " + " | ".join(parts) + f" | {trend}")

    focus = slice(int(np.searchsorted(series.dates, FOCUS_FROM)), int(np.searchsorted(series.dates, FOCUS_TO, side="right")))
    rows = np.arange(len(close))[focus]
    low_row, high_row = rows[np.argmin(low[focus])], rows[np.argmax(high[focus])]
    after = rows[rows > low_row]
    lines += ["", f"표 5. {FOCUS_FROM}–{FOCUS_TO} 가격 흐름",
              f"  시작 전날 종가 {close[rows[0] - 1]:,.0f}, 구간 첫날 시가 {opens[rows[0]]:,.0f}",
              f"  구간 최고 고가 {high[high_row]:,.0f} ({date(high_row)}), 구간 최저 저가 {low[low_row]:,.0f} ({date(low_row)}), {FOCUS_TO} 종가 {close[rows[-1]]:,.0f}",
              f"  최저 저가 뒤 구간 안 최고 고가 {high[after].max():,.0f} → 최저에서 산 경우 최대 {(high[after].max() / low[low_row] - 1) * 100:+.1f}%"
              if len(after) else "  최저 저가가 마지막 날",
              f"  02-05 D0 종가 {close[rows[1]]:,.0f} 에서 {FOCUS_TO} 종가까지 {(close[rows[-1]] / close[rows[1]] - 1) * 100:+.1f}%"]

    for row in range(rows[-1] + 1, min(rows[-1] + 11, len(close))):
        lines.append(f"  {date(row)} 시가 {opens[row]:,.0f} 고가 {high[row]:,.0f} 저가 {low[row]:,.0f} 종가 {close[row]:,.0f} "
                     f"{(close[row] / close[row - 1] - 1) * 100:+.1f}%")

    text = "\n".join(lines)
    TEXT_PATH.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
