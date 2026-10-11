"""스터디 40-3 — 하락 추세가 상승 추세로 돌아서는 자리를 먼저 찾는다(2026-10-11).

왜: 사용자가 보여 준 실패 건(036580 팜스코 2018-10-31 등)은 하락 추세 안에서 단기 정배열만 보고 샀다. 눌림 매수보다
  앞서 "추세가 돌아선 자리"를 찾아야 하고, 그 자리를 잡는 정의가 맞는지부터 확인한다.

추세 전환 자리(종가로 판정하고 그날 종가에 산다 — 15:20 동시호가 직전 가격으로 판정해 종가에 내는 것으로 본다):
  align  역배열(SMA5 < SMA10 < SMA20 < SMA60)을 거친 뒤 처음으로 정배열(SMA5 > SMA10 > SMA20 > SMA60)이 된 날.
  cross  역배열을 거친 뒤 SMA20 이 SMA60 을 아래에서 위로 뚫은 날.
  cloud  역배열을 거친 뒤 종가가 일목 구름대 윗변을 처음 넘은 날(전환선 9·기준선 26·선행스팬2 52, 26일 앞으로 민 값).
추세 끝(종가로 판정하고 그날 종가에 판다):
  short  단기 정배열(SMA5 > SMA10 > SMA20)이 깨진 날.
  long   SMA20 이 SMA60 아래로 내려간 날.
  inverse  역배열(SMA5 < SMA10 < SMA20)이 된 날.
같은 기간 시장(전 종목 종가 동일가중) 대비 초과수익도 낸다. 비용은 수수료·세금·1호가 미끄러짐.

산출(data/turn/): events.parquet(전환 자리 × 끝 정의별 매매 한 건씩), summary.csv, examples.csv(사용자가 보여 준 종목).
재실행: py research/studies/40_devscale_entry_touch/turn_points.py
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
import ma_exit  # noqa: E402
from trend_filter import cloud_top  # noqa: E402

DATA_DIR = HERE / "data" / "turn"
EVENTS = {"align": 0, "cross": 1, "cloud": 2}
ENDS = {"short": 0, "long": 1, "inverse": 2}
EXAMPLES = {"036580": ("2018-06-01", "2019-06-30"), "179290": ("2026-05-01", "2026-10-10"),
            "045300": ("2026-05-01", "2026-10-10"), "056190": ("2018-06-01", "2019-03-31")}


@numba.njit(cache=True)
def trend_ended(end_mode, sma5, sma10, sma20, sma60):
    if end_mode == 0:
        return not (sma5 > sma10 and sma10 > sma20)

    if end_mode == 1:
        return sma20 < sma60

    return sma5 < sma10 and sma10 < sma20


@numba.njit(cache=True)
def ticker_trades(open_, close, halted, start_index, event_mode, end_mode, sma5, sma10, sma20, sma60, cloud, out):
    days = close.shape[0]
    filled = 0
    reset = False
    day = 61

    while day < days - 1:
        if sma5[day] < sma10[day] and sma10[day] < sma20[day] and sma20[day] < sma60[day]:
            reset = True

        fired = False

        if reset and day >= start_index:
            if event_mode == 0:
                fired = sma5[day] > sma10[day] and sma10[day] > sma20[day] and sma20[day] > sma60[day]
            elif event_mode == 1:
                fired = sma20[day] > sma60[day] and sma20[day - 1] <= sma60[day - 1]
            else:
                fired = close[day] > cloud[day] and close[day - 1] <= cloud[day - 1]

        if not fired:
            day += 1
            continue

        reset = False

        if halted[day]:
            day += 1
            continue

        entry_t = day
        exit_t = days - 1
        reason = 6
        exit_search_day = entry_t + 1

        while exit_search_day < days:
            if not halted[exit_search_day] and trend_ended(end_mode, sma5[exit_search_day], sma10[exit_search_day], sma20[exit_search_day], sma60[exit_search_day]):
                exit_t = exit_search_day
                reason = 1
                break

            exit_search_day += 1

        out[filled, 0] = day
        out[filled, 1] = entry_t
        out[filled, 2] = close[entry_t]
        out[filled, 4] = close[exit_t]
        out[filled, 3] = exit_t
        out[filled, 5] = reason
        filled += 1
        day = max(day + 1, exit_search_day)

    return filled


@numba.njit(parallel=True, cache=True)
def all_trades(open_, close, halted, start_index, event_mode, end_mode, sma5, sma10, sma20, sma60, cloud, out, counts):
    for ticker in numba.prange(close.shape[0]):
        counts[ticker] = ticker_trades(open_[ticker], close[ticker], halted[ticker], start_index, event_mode, end_mode,
                                       sma5[ticker], sma10[ticker], sma20[ticker], sma60[ticker], cloud[ticker],
                                       out[ticker], )


def market_index(panel: grid.Panel) -> np.ndarray:
    close = pd.DataFrame(panel.close.T).ffill()
    daily = close.pct_change().where(~pd.DataFrame(panel.halted.T)).clip(-0.3, 0.3).mean(axis=1).fillna(0.0)
    return (1.0 + daily).cumprod().to_numpy()


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    panel = grid.Panel()
    lines = ma_exit.Lines(panel)
    cloud = cloud_top(panel)
    index = market_index(panel)
    tickers, days = panel.close.shape
    frames = []

    for event_name, event_mode in EVENTS.items():
        for end_name, end_mode in ENDS.items():
            out = np.zeros((tickers, 400, 6))
            counts = np.zeros(tickers, np.int64)
            all_trades(panel.open, panel.close, panel.halted, panel.start_index, event_mode, end_mode, lines.sma[5],
                       lines.sma[10], lines.sma[20], lines.sma[60], cloud, out, counts)
            rows = np.concatenate([out[index_, :counts[index_]] for index_ in range(tickers) if counts[index_]])
            frame = pd.DataFrame(rows, columns=["signal_t", "entry_t", "entry_price", "exit_t", "exit_price", "reason"])
            frame.insert(0, "ticker_index", np.repeat(np.arange(tickers), counts))
            frame[["signal_t", "entry_t", "exit_t", "reason"]] = frame[["signal_t", "entry_t", "exit_t",
                                                                         "reason"]].astype(np.int64)
            frame = frame[frame["entry_price"] >= 2000].reset_index(drop=True)
            frame["net"] = panel.net_return(frame["ticker_index"].to_numpy(), frame["entry_t"].to_numpy(),
                                            frame["entry_price"].to_numpy(), frame["exit_t"].to_numpy(),
                                            frame["exit_price"].to_numpy(), 1) * 100
            frame["market"] = (index[frame["exit_t"]] / index[frame["entry_t"]] - 1) * 100
            frame["excess"] = frame["net"] - frame["market"]
            frame["hold"] = frame["exit_t"] - frame["entry_t"] + 1
            frame.insert(0, "end", end_name)
            frame.insert(0, "event", event_name)
            frames.append(frame)

    events = pd.concat(frames, ignore_index=True)
    events["ticker"] = panel.tickers[events["ticker_index"]]
    events["name"] = panel.names[events["ticker_index"]]
    events["signal_date"] = panel.dates[events["signal_t"]].strftime("%Y-%m-%d")
    events["exit_date"] = panel.dates[events["exit_t"]].strftime("%Y-%m-%d")
    events.to_parquet(DATA_DIR / "events.parquet", index=False)
    top = lambda values: values.sort_values().iloc[:int(len(values) * 0.99)].mean()  # noqa: E731
    summary = events.groupby(["event", "end"]).agg(
        trades=("net", "size"), mean_net=("net", "mean"), median_net=("net", "median"),
        win=("net", lambda values: (values > 0).mean() * 100), mean_excess=("excess", "mean"),
        median_excess=("excess", "median"), beat_market=("excess", lambda values: (values > 0).mean() * 100),
        mean_without_top1=("net", top), mean_hold=("hold", "mean"), median_hold=("hold", "median")).reset_index()
    summary.to_csv(DATA_DIR / "summary.csv", index=False)
    pd.set_option("display.width", 250)
    print(summary.round(2).to_string(index=False))
    example_rows = []

    for ticker, (begin, end) in EXAMPLES.items():
        part = events[(events["ticker"] == ticker) & (events["signal_date"] >= begin) & (events["signal_date"] <= end)]
        example_rows.append(part)

    examples = pd.concat(example_rows)[["ticker", "name", "event", "end", "signal_date", "exit_date", "entry_price",
                                         "exit_price", "net", "excess", "hold"]]
    examples.to_csv(DATA_DIR / "examples.csv", index=False)
    print(examples.round(2).to_string(index=False))


if __name__ == "__main__":
    main()
