"""스터디 38 — 칸(슬롯) 제한 계좌 시험: 같은 매수 신호에 청산만 바꿔 2010–2026 계좌 곡선을 낸다(2026-10-11).

왜: 추세 청산(역배열·SMA60)은 건당 순수익을 올리지만 보유가 3 일 → 15–32 일로 늘어 칸이 빨리 찬다. 칸이 차면 새 매수를
  못 받으므로 건당이 올라도 계좌 수익이 오르는지는 따로 재야 한다. 라이브 청산선을 무엇으로 바꿀지 이 결과로 정한다.

계좌(내가 정함):
  - 칸 SLOTS 개(라이브 DevScale 25 칸, 메모리 project_slots30_and_fee_boundary). 한 칸 금액 = 전날 종가 기준 평가금 ÷ 칸 수,
    현금이 모자라면 남은 현금만큼. 복리.
  - 매수 후보 = entries_current.parquet(지금 규칙의 매수 130,649 건). 그 종목을 이미 들고 있으면 건너뛴다.
  - 같은 날 후보가 빈 칸보다 많으면 고르는 순서를 정할 근거가 없어 무작위로 고르고, 씨앗 SEEDS 개의 분포를 낸다.
  - 그날 판 칸은 다음 날부터 쓴다(같은 날 매도 → 매수는 시각 순서를 몰라 보수적으로 막음).
  - 매매 손익은 시험기와 같은 식(수수료 양쪽, 매도일 세율, 1 호가 슬리피지 양쪽, daily_stop_grid.Panel.net_return).
  - 보유 중 평가는 정지일을 앞 값으로 채운 종가.
[limit] 매수 후보는 지금 규칙으로 들고 있던 동안 막힌 재매수가 빠진 목록이다 — 보유가 긴 청산은 그 종목을 들고 있어
  어차피 못 사므로 영향이 작지만, 지금 규칙보다 일찍 판 경우의 재매수 기회는 빠진다.
[limit] 장중 매수 체결은 일봉 근사(entry_price)이고 거래대금 대비 주문 크기는 보지 않는다.

산출(data/): slot_summary.csv(청산 × 씨앗), slot_years.csv(청산 × 연도, 씨앗 중앙값), slot_curve.csv(청산 × 날짜, 씨앗 중앙값
  평가금), slot_trades_<청산>.parquet(씨앗 0 의 실제 체결 건 — 대시보드에서 건마다 확인).
재실행: py research/studies/38_trend_stop_intraday/slot_test.py
"""
from __future__ import annotations

import time

import numpy as np
import pandas as pd

import daily_stop_grid as grid
import ma_exit

SLOTS = 25
SEEDS = 20
EXITS = {
    "current": None,
    "inverse_open": (3, 20, 2, 0, 0.0),
    "inverse_close": (3, 20, 1, 0, 0.0),
    "unalign_open": (2, 20, 2, 0, 0.0),
    "sma60_open": (1, 60, 2, 0, 0.0),
    "sma60_armed_open": (1, 60, 2, 1, 0.0),
    "sma20_armed_open": (1, 20, 2, 1, 0.0),
}


def simulate(trades: pd.DataFrame, close_filled: np.ndarray, days: int, seed: int):
    """하루씩 걸으며 칸에 넣고 뺀다. 반환: 날짜별 평가금, 실제 체결 건 번호."""
    random_engine = np.random.default_rng(seed)
    order = random_engine.permutation(len(trades))
    by_day = pd.Series(order).groupby(trades["entry_t"].to_numpy()[order]).apply(list).to_dict()
    ticker = trades["ticker_index"].to_numpy()
    entry_t = trades["entry_t"].to_numpy()
    entry_price = trades["entry_price"].to_numpy()
    exit_t = trades["exit_t"].to_numpy()
    net = trades["net"].to_numpy() / 100
    cash, equity = 1.0, np.ones(days)
    held: dict[int, tuple[int, float]] = {}   # 건 번호 → (종목, 넣은 금액)
    held_tickers: set[int] = set()
    taken = []
    exits_on: dict[int, list[int]] = {}

    for day in range(days):
        budget = (equity[day - 1] if day > 0 else 1.0) / SLOTS

        for index in by_day.get(day, ()):
            if len(held) >= SLOTS or cash <= 1e-9:
                break

            if ticker[index] in held_tickers:
                continue

            amount = min(budget, cash)
            cash -= amount
            held[index] = (ticker[index], amount)
            held_tickers.add(ticker[index])
            exits_on.setdefault(exit_t[index], []).append(index)
            taken.append(index)

        for index in exits_on.pop(day, ()):
            stock, amount = held.pop(index)
            held_tickers.discard(stock)
            cash += amount * (1 + net[index])

        value = cash

        for index, (stock, amount) in held.items():
            value += amount * close_filled[stock, day] / entry_price[index]

        equity[day] = value

    return equity, np.array(taken)


def statistics(equity: np.ndarray, dates: pd.DatetimeIndex, start: int) -> dict:
    curve = equity[start:]
    years = (dates[-1] - dates[start]).days / 365.25
    peak = np.maximum.accumulate(curve)
    yearly = pd.Series(curve, index=dates[start:]).groupby(dates[start:].year).last()
    yearly = yearly / np.r_[curve[0], yearly.to_numpy()[:-1]] - 1
    return {"final_multiple": curve[-1] / curve[0], "cagr": (curve[-1] / curve[0]) ** (1 / years) - 1,
            "max_drawdown": (curve / peak - 1).min(), "worst_year": yearly.min(), "positive_years": int((yearly > 0).sum()),
            "years": len(yearly)}, yearly


def main() -> None:
    started = time.time()
    panel = grid.Panel()
    lines = ma_exit.Lines(panel)
    entries = pd.read_parquet(grid.DATA_DIR / "entries_current.parquet")
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    days = len(panel.dates)
    start = int(entries["entry_t"].min())
    summary_rows, year_rows, curves = [], [], {}

    for name, variant in EXITS.items():
        trades = entries if variant is None else ma_exit.moving_average_trades(panel, lines, entries, variant)
        trades = trades[["ticker_index", "entry_t", "entry_price", "exit_t", "exit_price", "reason"]].reset_index(drop=True)
        trades = trades.astype({"ticker_index": np.int64, "entry_t": np.int64, "exit_t": np.int64})
        trades["net"] = panel.net_return(trades["ticker_index"].to_numpy(np.int64), trades["entry_t"].to_numpy(np.int64),
                                         trades["entry_price"].to_numpy(), trades["exit_t"].to_numpy(np.int64),
                                         trades["exit_price"].to_numpy(), 1) * 100
        equity_by_seed, yearly_by_seed = [], []

        for seed in range(SEEDS):
            equity, taken = simulate(trades, close_filled, days, seed)
            run_statistics, yearly = statistics(equity, panel.dates, start)
            part = trades.loc[taken]
            hold = (part["exit_t"] - part["entry_t"] + 1).to_numpy()
            # 칸 사용률: 날마다 들고 있는 칸 수 평균 ÷ 칸 수.
            occupancy = np.zeros(days + 1)
            np.add.at(occupancy, part["entry_t"].to_numpy(), 1)
            np.add.at(occupancy, np.minimum(part["exit_t"].to_numpy() + 1, days), -1)
            summary_rows.append({"exit": name, "seed": seed, **run_statistics, "trades": len(part), "mean_net": part["net"].mean(),
                                 "median_net": part["net"].median(), "mean_hold": hold.mean(),
                                 "slot_use": np.cumsum(occupancy)[start:days].mean() / SLOTS * 100})
            equity_by_seed.append(equity)
            yearly_by_seed.append(yearly)

            if seed == 0:
                part.assign(ticker=panel.tickers[part["ticker_index"].to_numpy()],
                            entry_date=panel.dates[part["entry_t"].to_numpy()],
                            exit_date=panel.dates[part["exit_t"].to_numpy()]).to_parquet(
                    grid.DATA_DIR / f"slot_trades_{name}.parquet", index=False)

        curves[name] = np.median(np.array(equity_by_seed), axis=0)
        yearly = pd.concat(yearly_by_seed, axis=1).median(axis=1)
        year_rows += [{"exit": name, "year": int(year), "return": value} for year, value in yearly.items()]
        print(f"{name} 끝 · {time.time() - started:.0f}초", flush=True)

    pd.DataFrame(summary_rows).to_csv(grid.DATA_DIR / "slot_summary.csv", index=False)
    pd.DataFrame(year_rows).to_csv(grid.DATA_DIR / "slot_years.csv", index=False)
    pd.DataFrame(curves, index=panel.dates).iloc[start:].to_csv(grid.DATA_DIR / "slot_curve.csv", index_label="date")


if __name__ == "__main__":
    main()
