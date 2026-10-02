"""스터디 35 — 탐색 1번 칸에 외국인+기관 순매수 조건 하나를 더하는 사전 지정 시험(2026-10-02, 결과 보기 전 규칙 고정).

규칙(내가 정함): D1..D(k−1)(매수일 Dk 장중에 이미 아는 날만) 외국인 순매수 수량 + 기관 순매수 수량 합 > 0 이면 산다.
판정: 조건을 건 묶음이 전체 t ≥ 3, 2010–17·2018–26 둘 다 0 이상, 조건 묶음 − 반대 묶음 차이의 t ≥ 2.
보조 보고: D0 하루 수급만 본 경우, 기준선 B5_20 에 같은 조건.
"""
import importlib.util
import sys
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[3]
STUDY = Path(__file__).resolve().parent
module_specification = importlib.util.spec_from_file_location("study35_optimize", STUDY / "optimize.py")
optimize = importlib.util.module_from_spec(module_specification)
sys.modules["study35_optimize"] = optimize
module_specification.loader.exec_module(optimize)

CELLS = {
    "1번 칸": {"wait": 4, "depth": 15, "run": 2, "move": 30, "close_rule": "any", "regime": "any", "stop": "low3", "take": 15},
    "기준선 B5_20": optimize.BASELINE,
}


def stat(values: np.ndarray) -> str:
    if len(values) < 2:
        return f"n{len(values)}"

    t_value = values.mean() / values.std(ddof=1) * np.sqrt(len(values))
    return f"{values.mean():+.2f}% t{t_value:.2f} n{len(values)}"


def difference_t(first: np.ndarray, second: np.ndarray) -> float:
    error = np.sqrt(first.var(ddof=1) / len(first) + second.var(ddof=1) / len(second))
    return (first.mean() - second.mean()) / error


def main() -> None:
    events = pd.read_parquet(optimize.EVENTS_PATH)
    flow = pd.read_parquet(ROOT / "PYQuant/data/investor_flow_pit_2010.parquet", columns=["date", "ticker", "foreign_net_quantity", "institution_net_quantity"])
    flow["day"] = flow["date"].dt.strftime("%Y%m%d").astype(int)
    flow["smart"] = flow["foreign_net_quantity"] + flow["institution_net_quantity"]
    by_ticker = {ticker: (group["day"].to_numpy(), group["smart"].to_numpy()) for ticker, group in flow.groupby("ticker")}
    lines = [__doc__.strip(), ""]
    for label, cell in CELLS.items():
        trades = optimize.cell_trades(events, cell).copy()
        trades[["code", "surge"]] = events.loc[trades.index, ["code", "surge"]]
        window_sum, day0 = [], []
        for trade in trades.itertuples():
            days, smart = by_ticker.get(trade.code, (np.array([], int), np.array([], int)))
            inside = (days > trade.surge) & (days < trade.entry_date)
            window_sum.append(smart[inside].sum() if inside.any() else np.nan)
            same = days == trade.surge
            day0.append(smart[same][0] if same.any() else np.nan)

        trades["window"], trades["day0"] = window_sum, day0
        returns = trades["ret"].to_numpy()
        lines.append(f"[{label}] {optimize.cell_name(cell)} — 전체 {stat(returns)}, 수급 자료 없는 건 {int(trades['window'].isna().sum())}")
        for name, key in [("D1..D(k−1) 합", "window"), ("D0 하루(보조)", "day0")]:
            known = trades[key].notna().to_numpy()
            plus = known & (trades[key].to_numpy() > 0)
            minus = known & (trades[key].to_numpy() <= 0)
            early = trades["year"].to_numpy() < optimize.SPLIT_YEAR
            lines.append(f"  {name} > 0 : {stat(returns[plus])} | 2010–17 {stat(returns[plus & early])} | 2018–26 {stat(returns[plus & ~early])}")
            lines.append(f"  {name} ≤ 0 : {stat(returns[minus])} | 2010–17 {stat(returns[minus & early])} | 2018–26 {stat(returns[minus & ~early])}")
            lines.append(f"  차이 t {difference_t(returns[plus], returns[minus]):.2f}")

        lines.append("")

    text = "\n".join(lines)
    (STUDY / "flow_test.txt").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
