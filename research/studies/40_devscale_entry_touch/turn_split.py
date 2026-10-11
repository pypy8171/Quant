"""스터디 40-4 — 추세 전환 자리 매수 vs 전환 뒤 눌림 매수, 5·10·20일선 분할 매도(2026-10-11, 사용자 정의).

사용자 정의(2026-10-11):
  이평선: 단기 5·10·20, 중기 60, 장기 120·240. 단기선이 240일선을 뚫어 올리는 자리가 가장 강한 전환이다.
  매수: 전환 자리를 잡는 지표가 통하면 전환 자리에서 바로 사고, 안 통하면 전환 뒤 눌림(5·10·20일선 닿음)에서 산다.
  매도: 종가가 5일선 아래면 30%, 10일선 아래면 30%, 20일선 아래면 나머지 — 전부 그날 종가.
매수 규칙:
  cross_<s>_<L>  SMA s(5·10·20) 가 SMA L(60·120·240) 을 아래에서 위로 뚫은 날 종가에 산다.
  cloud          종가가 일목 구름대 윗변을 아래에서 위로 넘은 날 종가에 산다.
  pull_<L>       전일 SMA20 > SMA L 이고 전일 5 > 10 > 20 정배열인 종목이 장중 5·10·20일선 중 시가 아래 가장 높은 선에
                 닿으면 그 선 가격(호가 내림)에 산다. 공통 거름은 1차(entry_touch.py)와 같다.
  turn_<s>_<L>   SMA s 가 SMA L 을 아래에서 위로 뚫은 뒤(전환), 그 뒤 처음으로 전일 5 > 10 > 20 정배열에서 장중
                 5·10·20일선에 눌려 닿은 날 그 선 가격에 산다. 눌리기 전에 SMA s 가 SMA L 아래로 내려가면 그 전환은 버린다.
                 한 전환에 한 번만 산다(돌파한 날 사지 않는다 — cross_* 는 돌파 종가 매수라 직전 20일 저점보다 중앙 13–17% 위였다).
  turn_cloud     같은 방식, 전환 = 종가가 구름대 윗변을 넘은 날, 무효 = 종가가 구름대 윗변 아래.
  한 종목에 한 번에 한 건만 든다(다 팔기 전 새 신호는 건너뜀).
판정: 건당 순수익(수수료·세금·1호가)과 같은 기간 시장(전 종목 동일가중) 대비. 각 몫의 수익을 30·30·40 으로 더한다.
  aligned60 열 = 매수일 전일 5 > 10 > 20 > 60 이었던 건만의 평균.
계좌: slot_test.simulate(25칸, 씨앗 20개). 먼저 판 몫의 돈은 마지막 몫을 팔 때까지 칸에 묶인 것으로 본다(보수적).

산출(data/split/): summary.csv, account.csv, trades_<규칙>.parquet. 재실행: py research/studies/40_devscale_entry_touch/turn_split.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import numba
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import daily_stop_grid as grid  # noqa: E402
import slot_test  # noqa: E402
from trend_filter import cloud_top  # noqa: E402
from turn_points import market_index  # noqa: E402

DATA_DIR = HERE / "data" / "split"
LENGTHS = (5, 10, 20, 60, 120, 240)
WEIGHTS = (0.3, 0.3, 0.4)


def rules() -> dict[str, tuple[int, int, int]]:
    """이름 → (종류 0 돌파·1 구름·2 눌림·3 돌파 뒤 첫 눌림·4 구름 뒤 첫 눌림, 단기선, 기준선)."""
    table = {f"cross_{short}_{long}": (0, short, long) for long in (60, 120, 240) for short in (5, 10, 20)}
    table["cloud"] = (1, 0, 0)
    table.update({f"pull_{long}": (2, 20, long) for long in (60, 120, 240)})
    table.update({f"turn_{short}_{long}": (3, short, long) for long in (60, 120, 240) for short in (5, 10, 20)})
    table["turn_cloud"] = (4, 0, 0)
    return table


@numba.njit(cache=True)
def first_close_below(close, halted, line, begin, entry_t):
    last = entry_t

    for day in range(begin, close.shape[0]):
        if halted[day] or not close[day] > 0:
            continue

        last = day

        if close[day] < line[day]:
            return day, 1

    return last, 6


@numba.njit(cache=True)
def ticker_trades(open_, high, low, close, halted, market, new_from, start_index, kind, short, long_, sma5, sma10,
                  sma20, sma60, touch5, touch10, touch20, cloud, out):
    atr = grid.atr_series(high, low, close)
    days = close.shape[0]
    filled = 0
    free_from = 0
    turn_day = -1

    for day in range(241, days):
        previous_day = day - 1

        if kind == 3:
            if short[previous_day] > long_[previous_day] and short[previous_day - 1] <= long_[previous_day - 1]:
                turn_day = previous_day
            elif not short[previous_day] > long_[previous_day]:
                turn_day = -1
        elif kind == 4:
            if close[previous_day] > cloud[previous_day] and close[previous_day - 1] <= cloud[previous_day - 1]:
                turn_day = previous_day
            elif not close[previous_day] > cloud[previous_day]:
                turn_day = -1

        if day < start_index or day < free_from or halted[day]:
            continue

        price = np.nan
        begin = day + 1

        if kind == 0:
            if short[day] > long_[day] and short[day - 1] <= long_[day - 1]:
                price = close[day]
        elif kind == 1:
            if close[day] > cloud[day] and close[day - 1] <= cloud[day - 1]:
                price = close[day]
        elif kind == 2 or turn_day >= 0:
            if ((kind != 2 or sma20[previous_day] > long_[previous_day]) and sma5[previous_day] > sma10[previous_day] and sma10[previous_day] > sma20[previous_day]
                    and grid.entry_allowed(open_, close, halted, atr, day, start_index)
                    and (open_[day] - grid.window_sum(close, day, 20) / 20.0) / (grid.window_sum(close, day, 20) / 20.0)
                    * 100.0 >= grid.ENTRY_OPEN_DEVIATION_MIN_PERCENT):
                line = np.nan

                for value in (touch5[day], touch10[day], touch20[day]):
                    if open_[day] > value and low[day] <= value and not line >= value:
                        line = value

                if line == line:
                    step = grid.tick_size(line, market, day >= new_from)
                    price = np.floor(line / step) * step
                    begin = day

        if not (price >= 2000.0):
            continue

        exit5, reason5 = first_close_below(close, halted, sma5, begin, day)
        exit10, reason10 = first_close_below(close, halted, sma10, begin, day)
        exit20, reason20 = first_close_below(close, halted, sma20, begin, day)
        out[filled, 0] = day
        out[filled, 1] = price
        out[filled, 2] = exit5
        out[filled, 3] = exit10
        out[filled, 4] = exit20
        out[filled, 5] = max(reason5, reason10, reason20)
        out[filled, 6] = 1.0 if sma5[previous_day] > sma10[previous_day] and sma10[previous_day] > sma20[previous_day] and sma20[previous_day] > sma60[previous_day] else 0.0
        out[filled, 7] = day if kind <= 1 else turn_day
        filled += 1
        free_from = max(exit5, exit10, exit20) + 1
        turn_day = -1

    return filled


@numba.njit(parallel=True, cache=True)
def all_trades(open_, high, low, close, halted, market, new_from, start_index, kind, short, long_, sma5, sma10, sma20,
               sma60, touch5, touch10, touch20, cloud, out, counts):
    for ticker in numba.prange(close.shape[0]):
        counts[ticker] = ticker_trades(open_[ticker], high[ticker], low[ticker], close[ticker], halted[ticker],
                                       market[ticker], new_from, start_index, kind, short[ticker], long_[ticker],
                                       sma5[ticker], sma10[ticker], sma20[ticker], sma60[ticker], touch5[ticker],
                                       touch10[ticker], touch20[ticker], cloud[ticker], out[ticker])


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    panel = grid.Panel()
    filled = pd.DataFrame(panel.close.T).ffill()
    moving_averages = {length: np.ascontiguousarray(filled.rolling(length).mean().to_numpy().T) for length in LENGTHS}
    touch = {length: np.ascontiguousarray(filled.rolling(length - 1).mean().shift(1).to_numpy().T)
             for length in (5, 10, 20)}
    cloud = cloud_top(panel)
    index = market_index(panel)
    close_filled = filled.to_numpy().T.copy()
    tickers, days = panel.close.shape
    start = int(panel.start_index)
    summary_rows, account_rows = [], []

    for name, (kind, short, long_) in rules().items():
        out = np.zeros((tickers, 1500, 8))
        counts = np.zeros(tickers, np.int64)
        all_trades(panel.open, panel.high, panel.low, panel.close, panel.halted, panel.market, panel.new_from, start,
                   kind, moving_averages[short or 5], moving_averages[long_ or 60], moving_averages[5], moving_averages[10], moving_averages[20], moving_averages[60], touch[5], touch[10],
                   touch[20], cloud, out, counts)
        rows = np.concatenate([out[number, :counts[number]] for number in range(tickers) if counts[number]])
        trades = pd.DataFrame(rows, columns=["entry_t", "entry_price", "exit5_t", "exit10_t", "exit20_t", "reason",
                                             "aligned60", "turn_t"])
        trades.insert(0, "ticker_index", np.repeat(np.arange(tickers), counts))
        columns = ["entry_t", "exit5_t", "exit10_t", "exit20_t", "reason", "turn_t"]
        trades[columns] = trades[columns].astype(np.int64)
        ticker = trades["ticker_index"].to_numpy()
        entry_t = trades["entry_t"].to_numpy()
        entry_price = trades["entry_price"].to_numpy()
        net, market = np.zeros(len(trades)), np.zeros(len(trades))

        for weight, column in zip(WEIGHTS, ("exit5_t", "exit10_t", "exit20_t")):
            exit_t = trades[column].to_numpy()
            net += weight * panel.net_return(ticker, entry_t, entry_price, exit_t, panel.close[ticker, exit_t], 1)
            market += weight * (index[exit_t] / index[entry_t - (kind >= 2)] - 1)

        trades["net"] = net * 100
        trades["market"] = market * 100
        trades["excess"] = trades["net"] - trades["market"]
        trades["exit_t"] = trades[["exit5_t", "exit10_t", "exit20_t"]].max(axis=1)
        trades["hold"] = trades["exit_t"] - trades["entry_t"] + 1
        trades.to_parquet(DATA_DIR / f"trades_{name}.parquet", index=False)
        aligned = trades[trades["aligned60"] == 1]
        ordered = trades["net"].sort_values()
        summary_rows.append({"rule": name, "trades": len(trades), "per_year": len(trades) / 16.8,
                             "mean_net": trades["net"].mean(), "median_net": trades["net"].median(),
                             "win": (trades["net"] > 0).mean() * 100, "mean_excess": trades["excess"].mean(),
                             "beat_market": (trades["excess"] > 0).mean() * 100,
                             "mean_without_top1": ordered.iloc[:int(len(ordered) * 0.99)].mean(),
                             "mean_hold": trades["hold"].mean(), "aligned60_share": trades["aligned60"].mean() * 100,
                             "aligned60_mean_net": aligned["net"].mean(), "aligned60_excess": aligned["excess"].mean()})
        multiples = []

        for seed in range(slot_test.SEEDS):
            equity, taken = slot_test.simulate(trades, close_filled, days, seed)
            run_statistics, _ = slot_test.statistics(equity, panel.dates, start)
            multiples.append(run_statistics["final_multiple"])
            account_rows.append({"rule": name, "seed": seed, **run_statistics, "trades": len(taken)})

        print(f"{name}: {len(trades):,}건 · 건당 {trades['net'].mean():.2f}% · 시장 대비 {trades['excess'].mean():.2f}% "
              f"· 계좌 배수 중앙 {np.median(multiples):.2f}", flush=True)

    summary = pd.DataFrame(summary_rows)
    summary.to_csv(DATA_DIR / "summary.csv", index=False)
    pd.DataFrame(account_rows).to_csv(DATA_DIR / "account.csv", index=False)
    pd.set_option("display.width", 250)
    print(summary.round(2).to_string(index=False))


if __name__ == "__main__":
    main()
