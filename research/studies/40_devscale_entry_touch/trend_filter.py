"""스터디 40-2 — 장기 추세가 돌아선 종목만 눌림에서 사는 거름과 추세 매도를 엮어 계좌 시험으로 비교한다(2026-10-11).

왜: 1차(entry_touch.py)의 체결 건 35–39%가 60일선 아래, 장기 추세가 하락인 자리에서 샀다(예: 036580 팜스코 2018-10-31,
  60일선이 내려가는 중 단기 정배열만 보고 매수 → 63일 보유 −10%). 사용자 의도는 "추세가 돌아선 것을 확인한 뒤,
  정배열 종목이 5·10·20일선에 눌려 닿을 때 산다, 추세가 깨지면(역배열) 판다"이다.

매수 자리(전일 정배열 SMA5 > SMA10 > SMA20, 공통 거름은 1차와 같음):
  T20   시가가 20일선 위에서 출발해 장중 저가가 20일선에 닿으면 20일선 가격(호가 내림)에 산다.
  Tany  5·10·20일선 중 시가 아래이고 저가가 닿은 선 가운데 가장 높은 선(먼저 닿는 선)에 산다.
  n일선 = 앞 n−1 일 종가 평균(오늘 가격을 넣은 SMA n 이 그 가격과 같아지는 값).
추세 거름(전일 값으로 판정):
  none   거름 없음(1차와 같음).
  above60  종가 > SMA60.
  long   SMA20 > SMA60(장기 정배열 — 20일선이 60일선을 뚫고 올라간 뒤의 상태).
  cross  20일선이 60일선을 뚫고 올라간 뒤 첫 매수 후보 하루만(다시 아래로 내려가면 초기화).
  cloud  종가 > 일목 구름대 윗변. 전환선 9·기준선 26·선행스팬2 52, 선행스팬은 26일 앞으로 민다
         (t 날 구름 = t−26 날에 계산한 선행스팬 1·2).
매도(전부 매수 다음 날부터 판정, 신호 다음 날 시가 — 아래에서 산 건이 판정 밖에 남지 않게 armed 없음):
  inverse  종가 기준 역배열(SMA5 < SMA10 < SMA20).
  sma20    종가 < SMA20.
  sma60    종가 < SMA60.
계좌: 스터디 38 slot_test.simulate(칸 25개, 같은 날 후보가 빈 칸보다 많으면 무작위, 씨앗 20개).

산출(data/filter/): summary.csv, years.csv, curve.csv, candidates.csv, trades_<매수>_<거름>_<매도>.parquet(씨앗 0).
재실행: py research/studies/40_devscale_entry_touch/trend_filter.py [매도 이름 …]  (매도별로 나눠 돌린 뒤 merge)
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

DATA_DIR = HERE / "data" / "filter"
ENTRIES = {"T20": 0, "Tany": 1}
FILTERS = {"none": 0, "above60": 1, "long": 2, "cross": 3, "cloud": 4}
EXITS = {"inverse": (3, 20, 2, 0, 0.0), "sma20": (1, 20, 2, 0, 0.0), "sma60": (1, 60, 2, 0, 0.0)}


def cloud_top(panel: grid.Panel) -> np.ndarray:
    """종목 × 날짜 일목 구름대 윗변(t 날 값 = t−26 날 선행스팬 1·2 중 큰 값)."""
    high = pd.DataFrame(panel.high.T).ffill()
    low = pd.DataFrame(panel.low.T).ffill()
    middle = lambda length: (high.rolling(length).max() + low.rolling(length).min()) / 2  # noqa: E731
    span1 = (middle(9) + middle(26)) / 2
    span2 = middle(52)
    return np.ascontiguousarray(np.fmax(span1, span2).shift(26).to_numpy().T)


@numba.njit(cache=True)
def ticker_entries(open_, high, low, close, halted, market, new_from, start_index, entry_mode, filter_mode, touch5,
                   touch10, touch20, sma5, sma10, sma20, sma60, cloud, out_t, out_price):
    atr = grid.atr_series(high, low, close)
    filled = 0
    cross_open = False

    for day in range(61, close.shape[0]):
        previous_day = day - 1
        long_up = sma20[previous_day] > sma60[previous_day]

        if not long_up:
            cross_open = False
        elif previous_day >= 1 and sma20[previous_day - 1] <= sma60[previous_day - 1]:
            cross_open = True

        if not grid.entry_allowed(open_, close, halted, atr, day, start_index):
            continue

        previous_sma20 = grid.window_sum(close, day, 20) / 20.0

        if (open_[day] - previous_sma20) / previous_sma20 * 100.0 < grid.ENTRY_OPEN_DEVIATION_MIN_PERCENT:
            continue

        if not (sma5[previous_day] > sma10[previous_day] and sma10[previous_day] > sma20[previous_day]):
            continue

        if filter_mode == 1 and not close[previous_day] > sma60[previous_day]:
            continue

        if filter_mode == 2 and not long_up:
            continue

        if filter_mode == 3 and not cross_open:
            continue

        if filter_mode == 4 and not close[previous_day] > cloud[previous_day]:
            continue

        line = np.nan

        if entry_mode == 0:
            if open_[day] > touch20[day] and low[day] <= touch20[day]:
                line = touch20[day]
        else:
            for value in (touch5[day], touch10[day], touch20[day]):
                if open_[day] > value and low[day] <= value and not line >= value:
                    line = value

        if line != line:
            continue

        step = grid.tick_size(line, market, day >= new_from)
        out_t[filled] = day
        out_price[filled] = np.floor(line / step) * step
        filled += 1

        if filter_mode == 3:
            cross_open = False

    return filled


@numba.njit(parallel=True, cache=True)
def all_entries(open_, high, low, close, halted, market, new_from, start_index, entry_mode, filter_mode, touch5,
                touch10, touch20, sma5, sma10, sma20, sma60, cloud, out_t, out_price, counts):
    for ticker in numba.prange(close.shape[0]):
        counts[ticker] = ticker_entries(open_[ticker], high[ticker], low[ticker], close[ticker], halted[ticker],
                                        market[ticker], new_from, start_index, entry_mode, filter_mode,
                                        touch5[ticker], touch10[ticker], touch20[ticker], sma5[ticker],
                                        sma10[ticker], sma20[ticker], sma60[ticker], cloud[ticker], out_t[ticker],
                                        out_price[ticker])


def candidate_entries(panel: grid.Panel, lines: ma_exit.Lines, cloud: np.ndarray, entry_mode: int,
                      filter_mode: int) -> pd.DataFrame:
    tickers, days = panel.close.shape
    out_t = np.zeros((tickers, days), np.int64)
    out_price = np.zeros((tickers, days))
    counts = np.zeros(tickers, np.int64)
    all_entries(panel.open, panel.high, panel.low, panel.close, panel.halted, panel.market, panel.new_from,
                panel.start_index, entry_mode, filter_mode, lines.touch[5], lines.touch[10], lines.touch[20],
                lines.sma[5], lines.sma[10], lines.sma[20], lines.sma[60], cloud, out_t, out_price, counts)
    ticker = np.repeat(np.arange(tickers), counts)
    entry_t = np.concatenate([out_t[index, :counts[index]] for index in range(tickers)])
    entry_price = np.concatenate([out_price[index, :counts[index]] for index in range(tickers)])
    return pd.DataFrame({"ticker_index": ticker, "entry_t": entry_t, "entry_price": entry_price})


def main(exit_names: list[str]) -> None:
    started = time.time()
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    panel = grid.Panel()
    lines = ma_exit.Lines(panel)
    cloud = cloud_top(panel)
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    days = len(panel.dates)
    start = int(panel.start_index)
    summary_rows, year_rows, candidate_rows, curves = [], [], [], {}
    entries_cache = {}

    for exit_name in exit_names:
        for entry_name, entry_mode in ENTRIES.items():
            for filter_name, filter_mode in FILTERS.items():
                name = f"{entry_name}_{filter_name}_{exit_name}"
                key = (entry_mode, filter_mode)

                if key not in entries_cache:
                    entries_cache[key] = candidate_entries(panel, lines, cloud, entry_mode, filter_mode)

                trades = ma_exit.moving_average_trades(panel, lines, entries_cache[key], EXITS[exit_name])
                trades = trades[["ticker_index", "entry_t", "entry_price", "exit_t", "exit_price",
                                 "reason"]].reset_index(drop=True)
                trades["net"] = panel.net_return(trades["ticker_index"].to_numpy(np.int64),
                                                 trades["entry_t"].to_numpy(np.int64), trades["entry_price"].to_numpy(),
                                                 trades["exit_t"].to_numpy(np.int64), trades["exit_price"].to_numpy(),
                                                 1) * 100
                candidate_rows.append({"name": name, "entry": entry_name, "filter": filter_name, "exit": exit_name,
                                       "candidates": len(trades), "per_year": len(trades) / 16.8,
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
                    summary_rows.append({"name": name, "entry": entry_name, "filter": filter_name, "exit": exit_name,
                                         "seed": seed, **run_statistics, "trades": len(part), "mean_net": part["net"].mean(),
                                         "median_net": part["net"].median(), "win": (part["net"] > 0).mean() * 100,
                                         "mean_hold": hold.mean(),
                                         "slot_use": np.cumsum(occupancy)[start:days].mean() / slot_test.SLOTS * 100})
                    equity_by_seed.append(equity)
                    yearly_by_seed.append(yearly)

                    if seed == 0:
                        part.to_parquet(DATA_DIR / f"trades_{name}.parquet", index=False)

                curves[name] = np.median(np.array(equity_by_seed), axis=0)
                yearly = pd.concat(yearly_by_seed, axis=1).median(axis=1)
                year_rows += [{"name": name, "year": int(year), "return": value} for year, value in yearly.items()]
                print(f"{name} 후보 {len(trades):,}건 끝 · {time.time() - started:.0f}초", flush=True)

    suffix = "_".join(exit_names)
    pd.DataFrame(candidate_rows).to_csv(DATA_DIR / f"candidates_{suffix}.csv", index=False)
    pd.DataFrame(summary_rows).to_csv(DATA_DIR / f"summary_{suffix}.csv", index=False)
    pd.DataFrame(year_rows).to_csv(DATA_DIR / f"years_{suffix}.csv", index=False)
    pd.DataFrame(curves, index=panel.dates).iloc[start:].to_csv(DATA_DIR / f"curve_{suffix}.csv", index_label="date")


if __name__ == "__main__":
    main(sys.argv[1:] or list(EXITS))
