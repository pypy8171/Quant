"""trades.json 요약 — 범위 r × 매수 방식마다 손절 × 익절 격자에서 가장 좋은 칸, 그 칸의 연도별 평균. 통과선 t 4.0(내가 정함).
재실행(저장소 루트): py -X utf8 research/studies/35_surge_box_breakout/summarize.py
"""
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

STUDY = Path(__file__).resolve().parent
sys.stdout.reconfigure(encoding="utf-8")
meta = json.loads((STUDY / "trades.json").read_text(encoding="utf-8"))["meta"]
trades = [trade for path in sorted((STUDY / "kinds").glob("*.json")) if not path.stem.startswith("pullback")
          for trade in json.loads(path.read_text(encoding="utf-8"))]
STOPS, TAKES = len(meta["stop"]), len(meta["take"])


def grid_outcome(exits, stop_index, take_index):
    """take_index == TAKES 는 익절 없음. 둘 다 안 닿으면 120일 종가, 그것도 없으면(보유 중) None."""
    stop_day, stop_net = exits[stop_index]
    take_day, take_net = exits[STOPS + take_index] if take_index < TAKES else (-1, None)

    if stop_day >= 0 and (take_day < 0 or stop_day <= take_day):
        return stop_net, stop_day

    if take_day >= 0:
        return take_net, take_day

    return exits[-1][1], exits[-1][0]


rows = []

for trade in trades:
    exits = trade["exits"]

    for stop_index in range(STOPS):
        for take_index in range(TAKES + 1):
            net, day = grid_outcome(exits, stop_index, take_index)

            if net is not None:
                rows.append((trade["band"], trade["kind"], trade["surge"] // 10000, stop_index, take_index, net, day))

frame = pd.DataFrame(rows, columns=["band", "kind", "year", "stop", "take", "net", "day"])
cells = frame.groupby(["band", "kind", "stop", "take"])["net"].agg(["count", "mean", "std"])
cells["t"] = cells["mean"] / (cells["std"] / np.sqrt(cells["count"]))
cells["win"] = frame.assign(win=frame["net"] > 0).groupby(["band", "kind", "stop", "take"])["win"].mean()
take_name = lambda index: f"+{meta['take'][index]}%" if index < TAKES else "없음"
stop_name = lambda index: f"−{meta['stop'][index]}%" if isinstance(meta["stop"][index], int) else meta["stop"][index]
lines = [f"급등 {meta['surges']}건. 칸 수 = 범위 {len(meta['bands'])} × 매수 6 × 손절 {STOPS} × 익절 {TAKES + 1}"]

for (band, kind), part in cells.groupby(level=[0, 1]):
    best = part.sort_values("mean", ascending=False).head(3)
    text = "; ".join(f"{stop_name(stop)}/{take_name(take)} {row['mean']:+.2f}% t{row['t']:.1f} 승{row['win'] * 100:.0f}%"
                     for (_, _, stop, take), row in best.iterrows())
    lines.append(f"r{band} {kind:15s} n={int(part['count'].max()):5d} 최고: {text}")

positive = cells[(cells["t"] >= 4.0) & (cells["count"] >= 100)]
lines.append(f"\nt ≥ 4.0·n ≥ 100 칸: {len(positive)}개 / 전체 {len(cells)}개")
lines.append(positive.sort_values("t", ascending=False).head(20).round(2).to_string())
picks = [trade for trade in trades if trade["code"] in ("100590", "217590") and trade["surge"] >= 20260101]
lines.append("\n머큐리·티엠씨 2026: " + "; ".join(f"{trade['name']} 급등 {trade['surge']} r{trade['band']} {trade['kind']} 매수 {trade['entry_date']}" for trade in picks))
text = "\n".join(lines)
(STUDY / "summary.txt").write_text(text, encoding="utf-8")
print(text)
