"""스터디 43 재현 대조 — 하네스의 매수 후보 행렬 + OLD 매도로 종목마다 순차로 걸어 스터디 38 entries_current 와 맞춰 본다.

(가) 38 원본 패널(3,242종목, 비 보정 없음): 하네스 함수(candidate_matrix·exit_of)가 38 과 같은 매매를 내는지 본다.
(나) 하네스 가격(41 종목·41 기준 가격): 비 보정으로 매수·매도가 얼마나 달라지는지 본다.
슬롯 제한 없이 종목마다 독립으로 걷는다(38 과 같은 조건). 결과는 results/reproduction.csv.

실행: py -I -X utf8 research/studies/43_devscale_real_rules/reproduce.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import harness     # noqa: E402
import numpy as np     # noqa: E402
import pandas as pd     # noqa: E402
from harness import grid     # noqa: E402
from numba import njit, prange     # noqa: E402

COLUMNS = ("entry_t", "entry_price", "exit_t", "exit_price", "reason")


@njit(parallel=True, cache=True)
def walk_all(open_price, high, low, close, halted, lock_down, entry_fill, old_rule, records, counts):
    """종목마다 후보 행렬을 앞에서부터 훑어 사고, OLD 로 판 다음 날부터 다시 본다(38 simulate_ticker 와 같은 순서)."""
    empty_days = np.empty(0, dtype=np.int64)
    empty_levels = np.empty(0)
    last_valid = np.empty(close.shape[0], dtype=np.int64)

    for stock in range(close.shape[0]):
        last_valid[stock] = grid.last_valid_index(close[stock])

    for stock in prange(close.shape[0]):
        day = 0
        filled = 0

        while day < close.shape[1]:
            price = entry_fill[stock, day]

            if price != price:
                day += 1
                continue

            exit_day, exit_price, reason, at_open = harness.exit_of(
                harness.RULE_OLD, stock, day, price, open_price, high, low, close, halted, average_unused(close),
                lock_down, last_valid, old_rule, empty_days, empty_levels)

            if filled < records.shape[1]:
                records[stock, filled, 0] = day
                records[stock, filled, 1] = price
                records[stock, filled, 2] = exit_day
                records[stock, filled, 3] = exit_price
                records[stock, filled, 4] = reason
                filled += 1

            if exit_day >= close.shape[1] or reason == harness.REASON_DELIST:
                break

            day = exit_day + 1

        counts[stock] = filled


@njit(cache=True)
def average_unused(close):
    """OLD 는 SMA60 을 쓰지 않는다 — exit_of 인자 자리만 채운다."""
    return close


def walk(open_price, high, low, close, halted, lock_down, market, new_from, start_index) -> pd.DataFrame:
    stock_count, day_count = close.shape
    entry_fill = np.full((stock_count, day_count), np.nan)
    entry_path = np.zeros((stock_count, day_count), dtype=np.int8)
    harness.candidate_matrix(open_price, high, low, close, halted, market, new_from, start_index, entry_fill,
                             entry_path)
    records = np.zeros((stock_count, grid.CAPACITY, len(COLUMNS)))
    counts = np.zeros(stock_count, dtype=np.int64)
    walk_all(open_price, high, low, close, halted, lock_down, entry_fill, harness.OLD_RULE, records, counts)
    frame = pd.DataFrame(np.concatenate([records[stock, :count] for stock, count in enumerate(counts) if count]),
                         columns=COLUMNS)
    frame.insert(0, "ticker_index", np.repeat(np.arange(stock_count), counts))
    return frame


def compare(label: str, mine: pd.DataFrame, reference: pd.DataFrame) -> dict:
    keys = ["ticker_index", "entry_t"]
    merged = reference.merge(mine, on=keys, how="outer", suffixes=("_38", "_43"), indicator=True)
    both = merged[merged["_merge"] == "both"]
    same_entry = np.isclose(both["entry_price_38"], both["entry_price_43"], rtol=1e-9, atol=1e-6)
    same_exit_day = both["exit_t_38"] == both["exit_t_43"]
    same_exit = same_exit_day & np.isclose(both["exit_price_38"], both["exit_price_43"], rtol=1e-9, atol=1e-6)
    return {"대조": label, "38 매수 건수": len(reference), "43 매수 건수": len(mine), "둘 다 있음": len(both),
            "38에만": int((merged["_merge"] == "left_only").sum()), "43에만": int((merged["_merge"] == "right_only").sum()),
            "매수 일치율 %": round(len(both) / len(reference) * 100, 4),
            "매수가 일치율 %": round(same_entry.mean() * 100, 4),
            "매도일 일치율 %": round(same_exit_day.mean() * 100, 4),
            "매도일·매도가 일치율 %": round(same_exit.mean() * 100, 4)}


def main() -> None:
    started = time.time()
    reference = pd.read_parquet(grid.DATA_DIR / "entries_current.parquet")
    reference = reference[["ticker_index", "entry_t", "entry_price", "exit_t", "exit_price", "reason"]]
    panel = np.load(grid.PANEL_PATH, allow_pickle=True)
    dates = pd.DatetimeIndex(panel["dates"])
    new_from = int(np.searchsorted(dates, pd.Timestamp(grid.NEW_TICK_TABLE_FROM)))
    start_index = int(np.searchsorted(dates, pd.Timestamp(grid.TEST_START)))
    close = np.ascontiguousarray(panel["close"])
    no_lock = np.zeros(close.shape, dtype=np.bool_)
    original = walk(np.ascontiguousarray(panel["open"]), np.ascontiguousarray(panel["high"]),
                    np.ascontiguousarray(panel["low"]), close, np.ascontiguousarray(panel["halted"]), no_lock,
                    panel["market"].astype(np.int64), new_from, start_index)
    # 38 은 시험 끝·거래 끝 건을 마지막 유효일 종가로 적는다 — 43 의 폐지·시험 끝 표기를 같은 꼴로 맞춘다
    last_valid = np.array([grid.last_valid_index(row) for row in close])
    open_ended = original["reason"].isin([harness.REASON_DELIST, harness.REASON_END])
    original.loc[open_ended, "exit_t"] = last_valid[original.loc[open_ended, "ticker_index"].astype(int)]
    rows = [compare("(가) 38 원본 패널·잠김 미룸 끔", original.astype({"ticker_index": int, "entry_t": int}), reference)]
    del panel, close
    market = harness.Market()
    adjusted = walk(market.open, market.high, market.low, market.close, market.halted, market.lock_down,
                    market.static_market, market.new_from, market.start_index)
    adjusted = adjusted[adjusted["entry_t"] >= market.first_day].copy()
    adjusted["ticker_index"] = market.panel_rows[adjusted["ticker_index"].astype(int)]
    open_ended = adjusted["reason"].isin([harness.REASON_DELIST, harness.REASON_END])
    adjusted.loc[open_ended, "exit_t"] = last_valid[adjusted.loc[open_ended, "ticker_index"].astype(int)]
    in_scope = reference[reference["ticker_index"].isin(market.panel_rows)
                         & (reference["entry_t"] >= market.first_day)]
    # 가격 비교는 비 보정 전 값끼리 — 43 가격을 38 기준으로 되돌린다
    ratio = market.close / np.load(grid.PANEL_PATH, allow_pickle=True)["close"][market.panel_rows]
    position = pd.Index(market.panel_rows).get_indexer(adjusted["ticker_index"])
    adjusted["entry_price"] = adjusted["entry_price"].to_numpy() / ratio[position, adjusted["entry_t"].astype(int)]
    exit_index = np.minimum(adjusted["exit_t"].astype(int), market.day_count - 1)
    back_exit = ratio[position, exit_index]
    adjusted["exit_price"] = adjusted["exit_price"].to_numpy() / back_exit
    rows.append(compare("(나) 43 가격(41 종목·비 보정)·잠김 미룸 켬", adjusted.astype({"ticker_index": int, "entry_t": int}),
                        in_scope))
    table = pd.DataFrame(rows)
    table["걸린 초"] = round(time.time() - started, 1)
    harness.RESULTS.mkdir(exist_ok=True)
    table.to_csv(harness.RESULTS / "reproduction.csv", index=False, encoding="utf-8-sig", lineterminator="\n")
    print(table.to_string())


if __name__ == "__main__":
    main()
