"""스터디 40 — DevScale 매수 자리: 정배열 종목이 장중 이평선에 닿을 때 사는 규칙을 계좌 시험으로 비교한다(2026-10-11).

왜: 사용자 의도는 "추세가 살아 있는 종목이 장중에 순간적으로 눌려 이평선(주로 20일선)에 닿으면 산다"이다. 지금 규칙은
  일봉 20일선 −8%–+5% 를 살 자리로 켜고 그 안에서 산다. 밴드가 넓어 선을 한참 깬 자리에서도 산다. 청산을 D-158
  (SMA60 위에 한 번 선 뒤 종가 이탈 → 다음 날 시가)로 고정하고 매수만 바꿔 2010–2026 계좌 결과를 잰다.

매수 후보(그날 하루 한 번, 종목마다 날마다 독립으로 만든다 — 청산에 따라 막힌 재매수가 빠지지 않게):
  공통 거름: 스터디 38 entry_allowed(ATR14/SMA20 ≤ 4.5%, 가격 ≥ 2,000원, 앞 21일 자료) + 시가 이격 ≥ −1%(라이브 값).
  A      지금 규칙을 일봉으로 근사한 것(스터디 38 entry_price, 경로 1·2·3 전부).
  A2     A 중 경로 2(시가가 20일선 위, 장중 20일선까지 내려와 체결)만. 지금 규칙의 "선에 닿아 산" 몫.
  T<n>   전일 정배열(SMA5 > SMA10 > SMA20, 전일 종가 기준)이고 시가가 n일선 위에서 출발해 장중 저가가 n일선에
         닿으면 n일선 가격(호가 내림)에 산다. n일선은 오늘 가격을 넣은 SMA n 이 그 가격과 같아지는 값
         = 앞 n−1 일 종가 평균(엔진의 "접은 SMA"와 같은 식). n = 10·20·60.
  T20g   T20 에 더해, 시가가 이미 20일선 아래(시가 이격 ≥ −1% 안)에서 출발하면 시가 − 1호가에 산다(저가가 닿을 때).
청산: ma_exit.moving_average_trades (1, 60, 2, 1, 0) — D-158 라이브 규칙.
계좌: 스터디 38 slot_test.simulate(칸 25개 = 동시에 25종목, 같은 날 후보가 빈 칸보다 많으면 무작위, 씨앗 20개).

[limit] 라이브 엔진은 개장 60분 뒤 3분봉 SMA20 을 기준선으로 바꿔 그 아래로 벌어질 때마다 산다. 이 몫은 분봉이
  250일뿐이라 2010–2026 일봉 시험에 넣지 못했다(A 는 개장 60분 일봉 기준선만 본 근사).
[limit] 체결은 일봉 저가로 판정한다. 지정가에 저가가 정확히 닿은 날도 체결로 본다(실제로는 대기 순서 때문에 못 살 수 있다).

산출(data/): summary.csv(후보 × 씨앗), years.csv, curve.csv(씨앗 중앙값), trades_<후보>.parquet(씨앗 0 체결 건),
  candidates.csv(후보별 건수·청산 무제한 건당 순수익).
재실행: py research/studies/40_devscale_entry_touch/entry_touch.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import numba
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import daily_stop_grid as grid  # noqa: E402
import ma_exit  # noqa: E402
import slot_test  # noqa: E402

DATA_DIR = HERE / "data"
EXIT_RULE = (1, 60, 2, 1, 0.0)
MODE_CURRENT, MODE_CURRENT_TOUCH, MODE_TOUCH, MODE_TOUCH_GAP = 0, 1, 2, 3
CANDIDATES = {
    "A": (MODE_CURRENT, 20),
    "A2": (MODE_CURRENT_TOUCH, 20),
    "T10": (MODE_TOUCH, 10),
    "T20": (MODE_TOUCH, 20),
    "T60": (MODE_TOUCH, 60),
    "T20g": (MODE_TOUCH_GAP, 20),
}


@numba.njit(cache=True)
def ticker_entries(open_, high, low, close, halted, market, new_from, start_index, mode, line, prev5, prev10, prev20,
                   out_t, out_price):
    atr = grid.atr_series(high, low, close)
    filled = 0

    for day in range(61, close.shape[0]):
        if not grid.entry_allowed(open_, close, halted, atr, day, start_index):
            continue

        new_table = day >= new_from

        if mode == MODE_CURRENT or mode == MODE_CURRENT_TOUCH:
            price, path = grid.entry_price(open_, high, low, close, day, market, new_table)

            if path == 0 or (mode == MODE_CURRENT_TOUCH and path != 2):
                continue
        else:
            previous_sma20 = grid.window_sum(close, day, 20) / 20.0

            if (open_[day] - previous_sma20) / previous_sma20 * 100.0 < grid.ENTRY_OPEN_DEVIATION_MIN_PERCENT:
                continue

            if not (prev5[day] > prev10[day] and prev10[day] > prev20[day]) or line[day] != line[day]:
                continue

            step = grid.tick_size(line[day], market, new_table)
            price = np.floor(line[day] / step) * step

            if open_[day] > price:
                if low[day] > price:
                    continue
            elif mode == MODE_TOUCH_GAP:
                price = open_[day] - grid.tick_size(open_[day], market, new_table)

                if low[day] > price:
                    continue
            else:
                continue

        out_t[filled] = day
        out_price[filled] = price
        filled += 1

    return filled


@numba.njit(parallel=True, cache=True)
def all_entries(open_, high, low, close, halted, market, new_from, start_index, mode, line, prev5, prev10, prev20,
                out_t, out_price, counts):
    for ticker in numba.prange(close.shape[0]):
        counts[ticker] = ticker_entries(open_[ticker], high[ticker], low[ticker], close[ticker], halted[ticker],
                                        market[ticker], new_from, start_index, mode, line[ticker], prev5[ticker],
                                        prev10[ticker], prev20[ticker], out_t[ticker], out_price[ticker])


def candidate_entries(panel: grid.Panel, lines: ma_exit.Lines, mode: int, length: int) -> pd.DataFrame:
    tickers, days = panel.close.shape
    out_t = np.zeros((tickers, days), np.int64)
    out_price = np.zeros((tickers, days))
    counts = np.zeros(tickers, np.int64)
    # 전일 종가 기준 이평: t 날 판정에 t−1 까지의 값을 쓴다.
    previous = {window_length: np.ascontiguousarray(np.roll(lines.sma[window_length], 1, axis=1)) for window_length in (5, 10, 20)}
    all_entries(panel.open, panel.high, panel.low, panel.close, panel.halted, panel.market, panel.new_from,
                panel.start_index, mode, lines.touch[length], previous[5], previous[10], previous[20], out_t, out_price,
                counts)
    ticker = np.repeat(np.arange(tickers), counts)
    entry_t = np.concatenate([out_t[index, :counts[index]] for index in range(tickers)])
    entry_price = np.concatenate([out_price[index, :counts[index]] for index in range(tickers)])
    return pd.DataFrame({"ticker_index": ticker, "entry_t": entry_t, "entry_price": entry_price})


def main() -> None:
    started = time.time()
    DATA_DIR.mkdir(exist_ok=True)
    panel = grid.Panel()
    lines = ma_exit.Lines(panel)
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    days = len(panel.dates)
    summary_rows, year_rows, candidate_rows, curves = [], [], [], {}
    start = None

    for name, (mode, length) in CANDIDATES.items():
        entries = candidate_entries(panel, lines, mode, length)
        trades = ma_exit.moving_average_trades(panel, lines, entries, EXIT_RULE)
        trades = trades[["ticker_index", "entry_t", "entry_price", "exit_t", "exit_price", "reason"]].reset_index(drop=True)
        trades["net"] = panel.net_return(trades["ticker_index"].to_numpy(np.int64), trades["entry_t"].to_numpy(np.int64),
                                         trades["entry_price"].to_numpy(), trades["exit_t"].to_numpy(np.int64),
                                         trades["exit_price"].to_numpy(), 1) * 100
        start = int(panel.start_index) if start is None else start
        candidate_rows.append({"entry": name, "candidates": len(trades), "per_year": len(trades) / 16.8,
                               "mean_net_all": trades["net"].mean(), "median_net_all": trades["net"].median(),
                               "win_all": (trades["net"] > 0).mean() * 100,
                               "mean_hold_all": (trades["exit_t"] - trades["entry_t"] + 1).mean()})
        equity_by_seed, yearly_by_seed = [], []

        for seed in range(slot_test.SEEDS):
            equity, taken = slot_test.simulate(trades, close_filled, days, seed)
            run_statistics, yearly = slot_test.statistics(equity, panel.dates, start)
            part = trades.loc[taken]
            hold = (part["exit_t"] - part["entry_t"] + 1).to_numpy()
            occupancy = np.zeros(days + 1)
            np.add.at(occupancy, part["entry_t"].to_numpy(), 1)
            np.add.at(occupancy, np.minimum(part["exit_t"].to_numpy() + 1, days), -1)
            summary_rows.append({"entry": name, "seed": seed, **run_statistics, "trades": len(part), "mean_net": part["net"].mean(),
                                 "median_net": part["net"].median(), "win": (part["net"] > 0).mean() * 100,
                                 "mean_hold": hold.mean(),
                                 "slot_use": np.cumsum(occupancy)[start:days].mean() / slot_test.SLOTS * 100})
            equity_by_seed.append(equity)
            yearly_by_seed.append(yearly)

            if seed == 0:
                part.to_parquet(DATA_DIR / f"trades_{name}.parquet", index=False)

        curves[name] = np.median(np.array(equity_by_seed), axis=0)
        yearly = pd.concat(yearly_by_seed, axis=1).median(axis=1)
        year_rows += [{"entry": name, "year": int(year), "return": value} for year, value in yearly.items()]
        print(f"{name} 후보 {len(trades):,}건 끝 · {time.time() - started:.0f}초", flush=True)

    pd.DataFrame(candidate_rows).to_csv(DATA_DIR / "candidates.csv", index=False)
    pd.DataFrame(summary_rows).to_csv(DATA_DIR / "summary.csv", index=False)
    pd.DataFrame(year_rows).to_csv(DATA_DIR / "years.csv", index=False)
    pd.DataFrame(curves, index=panel.dates).iloc[start:].to_csv(DATA_DIR / "curve.csv", index_label="date")


if __name__ == "__main__":
    main()
