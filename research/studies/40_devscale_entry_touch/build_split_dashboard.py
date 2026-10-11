"""스터디 40-4(turn_split.py) 결과를 스터디 38 대시보드의 '전환·분할 매도' 탭 데이터로 쓴다.

산출: dashboard/data/split40.txt(표·평가금 곡선·씨앗 0 체결 건), dashboard/data/bars40/(스터디 38 일봉 묶음에 없는 종목).
사용: py research/studies/40_devscale_entry_touch/build_split_dashboard.py  (turn_split.py 다음에)
"""
import base64
import gzip
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

import build_dashboard as board  # noqa: E402
import daily_stop_grid as grid  # noqa: E402
import slot_test  # noqa: E402
from turn_split import WEIGHTS, rules  # noqa: E402

DATA_DIR = HERE / "data" / "split"
EXITS = ("exit5_t", "exit10_t", "exit20_t")


def write_missing_bars(panel: grid.Panel, tickers: set[int]) -> dict:
    """스터디 38 묶음에 없는 종목만 bars40/ 에 같은 형식으로 쓴다."""
    meta = json.loads(gzip.decompress(base64.b64decode((board.OUT / "meta.txt").read_bytes())))
    missing = sorted(ticker for ticker in tickers if str(ticker) not in meta["bundle_of"])
    bundle_of = {}

    for bundle, start in enumerate(range(0, len(missing), board.BUNDLE_SIZE)):
        chunks, offset = [], 0

        for ticker in missing[start:start + board.BUNDLE_SIZE]:
            valid = np.flatnonzero(np.isfinite(panel.close[ticker]))
            first, last = int(valid[0]), int(valid[-1])
            rows = slice(first, last + 1)
            prices = np.stack([panel.open[ticker, rows], panel.high[ticker, rows], panel.low[ticker, rows],
                               panel.close[ticker, rows]], axis=1)
            base = pd.Series(panel.close[ticker, rows]).ffill().shift(1).to_numpy()
            base[0] = 0.0
            delta = np.where(np.isfinite(prices), np.round(prices - base[:, None]), board.MISSING).astype(np.int32)
            chunks.append(delta.ravel())
            bundle_of[str(ticker)] = [bundle, offset, first, last - first + 1]
            offset += delta.size

        board.write_packed(board.OUT / "bars40" / f"b{bundle}.txt", np.concatenate(chunks).astype("<i4").tobytes())

    print(f"  일봉 묶음 추가: 종목 {len(missing)}개")
    return bundle_of


def main() -> None:
    panel = grid.Panel()
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    days, start = len(panel.dates), int(panel.start_index)
    curves, account, tranche_rows, trades_out, years = {}, [], [], {}, []
    traded = set()

    for name in rules():
        trades = pd.read_parquet(DATA_DIR / f"trades_{name}.parquet")
        ticker, entry_t, entry_price = (trades["ticker_index"].to_numpy(), trades["entry_t"].to_numpy(),
                                        trades["entry_price"].to_numpy())

        for weight, column in zip(WEIGHTS, EXITS):
            exit_t = trades[column].to_numpy()
            net = panel.net_return(ticker, entry_t, entry_price, exit_t, panel.close[ticker, exit_t], 1) * 100
            trades[column.replace("_t", "_net")] = net
            trades[column.replace("_t", "_price")] = panel.close[ticker, exit_t]
            tranche_rows.append({"rule": name, "line": column[4:-2], "weight": weight, "mean_net": np.nanmean(net),
                                 "median_net": np.nanmedian(net), "win": np.mean(net > 0) * 100,
                                 "mean_hold": np.mean(exit_t - entry_t + 1)})

        equity_by_seed, yearly_by_seed = [], []

        for seed in range(slot_test.SEEDS):
            equity, taken = slot_test.simulate(trades, close_filled, days, seed)
            run_statistics, yearly = slot_test.statistics(equity, panel.dates, start)
            part = trades.loc[taken]
            account.append({"rule": name, "seed": seed, **run_statistics, "trades": len(part), "mean_net": part["net"].mean(),
                            "win": (part["net"] > 0).mean() * 100, "mean_hold": part["hold"].mean()})
            equity_by_seed.append(equity)
            yearly_by_seed.append(yearly)

            if seed == 0:
                traded.update(part["ticker_index"].tolist())
                trades_out[name] = board.clean(part[["ticker_index", "entry_t", "entry_price", "exit5_t", "exit5_price",
                                                     "exit5_net", "exit10_t", "exit10_price", "exit10_net", "exit20_t",
                                                     "exit20_price", "exit20_net", "exit_t", "net", "excess", "hold", "turn_t",
                                                     "aligned60"]].rename(columns={"ticker_index": "ticker"}))

        curves[name] = np.median(np.array(equity_by_seed), axis=0)
        yearly = pd.concat(yearly_by_seed, axis=1).median(axis=1)
        years += [{"rule": name, "year": int(year), "return": value} for year, value in yearly.items()]
        print(f"  {name} 끝", flush=True)

    curve = pd.DataFrame(curves, index=panel.dates).iloc[start:]
    curve = curve.groupby(curve.index.to_period("M")).tail(1).reset_index(names="date")
    curve["date"] = curve["date"].dt.strftime("%Y-%m-%d")
    payload = {"summary": board.clean(pd.read_csv(DATA_DIR / "summary.csv")), "account": board.clean(pd.DataFrame(account)),
               "tranches": board.clean(pd.DataFrame(tranche_rows)), "years": board.clean(pd.DataFrame(years)),
               "curve": board.clean(curve), "benchmark": board.clean(board.benchmark_curve(curve["date"])),
               "trades": trades_out, "bundle_of": write_missing_bars(panel, traded)}
    print(f"→ split40.txt {board.write_json('split40.txt', payload, compress=True) / 1e3:.0f}KB")


if __name__ == "__main__":
    main()
