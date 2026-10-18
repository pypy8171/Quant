"""스터디 38 — SMA60 청산에서 "한 번도 무장하지 못한 보유"에만 손실 하한·기한을 붙이는 칸 계좌 시험(2026-10-12).

왜: DevScale 청산(D-158)은 매수 뒤 종가가 SMA60 위에 한 번 선 뒤(무장)부터 SMA60 종가 이탈 → 다음 날 시가 매도다.
  SMA60 아래에서 사서 한 번도 위로 못 서면 팔 조건이 생기지 않는다(시험 최악 005880 −81%, 487 일 보유).
  이 미무장 보유에만 출구를 붙이면 계좌 성과가 어떻게 바뀌는지 잰다. 무장된 뒤에는 모두 기존 SMA60 규칙 그대로다.

변형(한 번에 변수 하나):
  - sma60_armed_open / sma20_armed_open: 기준. slot_test.py 의 같은 이름 행과 비트 단위로 같아야 한다(실행 때 대조).
  - deadline_N: 매수 뒤 N 거래일(정지일 제외, 매수일 다음 날부터 셈) 종가까지 무장 못 하면 다음 날 시가 매도. N = 10·20·40.
  - floor_X: 미무장 동안 종가 ≤ 매수가 × (1 − X%) 이면 다음 날 시가 매도. X = 10·15·20.
  - mix_sma20: 미무장 동안은 SMA20 무장 규칙(매수 뒤 종가가 SMA20 위에 선 뒤 SMA20 아래 종가 → 다음 날 시가),
    SMA60 에 무장되면 SMA60 규칙으로 넘어간다.
  같은 날 판정 순서(내가 정함): 무장 상태는 그날 시작 기준으로 보고 이탈·하한을 먼저 판정한 뒤 무장을 갱신한다
  (기존 ma_exit 순서와 같다). 기한은 그날 무장 갱신 뒤에 본다(N 번째 날 종가에 무장하면 팔지 않는다).
  매도 이유(reason): 1 SMA60 이탈, 3 기한, 4 평단 하한, 5 SMA20 이탈(미무장), 6 데이터 끝 평가.

계좌·비용은 slot_test.py 그대로(칸 25, 매수 순서를 섞은 20 회, panel.net_return 수수료·세금·1 호가 슬리피지 양쪽).
기간 2010–현재 전체 한 번(entries_current.parquet 의 첫 매수일부터 패널 끝까지).
[limit] 매수 후보는 지금 규칙의 매수 목록이라 일찍 판 뒤 같은 종목 재매수 기회는 빠진다(slot_test.py 와 같은 한계).

산출(data/): unarmed_floor_summary.csv(변형 × 회차), unarmed_floor_years.csv(변형 × 연도, 회차 중앙값),
  unarmed_floor_trades_<변형>.parquet(회차 0 의 실제 체결 건).
재실행: py research/studies/38_trend_stop_intraday/unarmed_floor_test.py
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

import numba
import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))

import daily_stop_grid as grid  # noqa: E402
import ma_exit  # noqa: E402
import slot_test  # noqa: E402

MODE_BASE, MODE_DEADLINE, MODE_FLOOR, MODE_MIX = 0, 1, 2, 3
REASON_DEADLINE, REASON_FLOOR, REASON_SMA20 = 3, 4, 5
# (이평 길이, 모드, 모수). 모수: 기한은 거래일 수, 하한은 % .
VARIANTS = {
    "sma60_armed_open": (60, MODE_BASE, 0.0),
    "sma20_armed_open": (20, MODE_BASE, 0.0),
    "deadline_10": (60, MODE_DEADLINE, 10.0),
    "deadline_20": (60, MODE_DEADLINE, 20.0),
    "deadline_40": (60, MODE_DEADLINE, 40.0),
    "floor_10": (60, MODE_FLOOR, 10.0),
    "floor_15": (60, MODE_FLOOR, 15.0),
    "floor_20": (60, MODE_FLOOR, 20.0),
    "mix_sma20": (60, MODE_MIX, 0.0),
}
BASELINE = "sma60_armed_open"
BAD_TRADE = -15.0


@numba.njit(parallel=True, cache=True)
def unarmed_exits(open_, close, halted, ticker, entry_t, entry_price, line, line20, mode, parameter, out):
    """매수 건마다 (매도일, 매도가, 이유, 매도 결정 때 무장 여부). mode 0 은 ma_exit (1, n, 2, 1, 0) 과 같은 동작."""
    last_day = close.shape[1] - 1

    for index in numba.prange(ticker.shape[0]):
        stock = ticker[index]
        start = entry_t[index]
        price = entry_price[index]
        armed = np.isfinite(close[stock, start]) and close[stock, start] > line[stock, start]
        armed20 = np.isfinite(close[stock, start]) and close[stock, start] > line20[stock, start]
        pending = False
        pending_reason = 1
        pending_armed = 1.0
        last_valid = start
        counted = 0
        out[index, 0] = -1

        for day in range(start + 1, last_day + 1):
            if halted[stock, day] or not np.isfinite(open_[stock, day]) or open_[stock, day] <= 0:
                continue

            last_valid = day
            counted += 1

            if pending:
                out[index, 0] = day
                out[index, 1] = open_[stock, day]
                out[index, 2] = pending_reason
                out[index, 3] = pending_armed
                break

            if armed and close[stock, day] < line[stock, day]:
                pending = True
                pending_reason = 1
                pending_armed = 1.0
            elif not armed and mode == MODE_FLOOR and close[stock, day] <= price * (1.0 - parameter / 100.0):
                pending = True
                pending_reason = REASON_FLOOR
                pending_armed = 0.0
            elif not armed and mode == MODE_MIX and armed20 and close[stock, day] < line20[stock, day]:
                pending = True
                pending_reason = REASON_SMA20
                pending_armed = 0.0

            if mode == MODE_MIX and not armed and not armed20 and close[stock, day] > line20[stock, day]:
                armed20 = True

            if not armed and close[stock, day] > line[stock, day]:
                armed = True

            if not pending and not armed and mode == MODE_DEADLINE and counted >= parameter:
                pending = True
                pending_reason = REASON_DEADLINE
                pending_armed = 0.0

        if out[index, 0] < 0:
            out[index, 0] = last_valid
            out[index, 1] = close[stock, last_valid]
            out[index, 2] = 6
            out[index, 3] = 1.0 if armed else 0.0


def variant_trades(panel: grid.Panel, lines: ma_exit.Lines, entries: pd.DataFrame, variant) -> pd.DataFrame:
    length, mode, parameter = variant
    ticker = entries["ticker_index"].to_numpy(np.int64)
    entry_t = entries["entry_t"].to_numpy(np.int64)
    entry_price = entries["entry_price"].to_numpy(np.float64)
    out = np.zeros((len(entries), 4))
    unarmed_exits(panel.open, panel.close, panel.halted, ticker, entry_t, entry_price, lines.sma[length], lines.sma[20],
                  mode, parameter, out)
    trades = pd.DataFrame({"ticker_index": ticker, "entry_t": entry_t, "entry_price": entry_price,
                           "exit_t": out[:, 0].astype(np.int64), "exit_price": out[:, 1], "reason": out[:, 2],
                           "armed_at_exit": out[:, 3].astype(np.int64)})
    trades["net"] = panel.net_return(ticker, entry_t, trades["entry_price"].to_numpy(), trades["exit_t"].to_numpy(np.int64),
                                     trades["exit_price"].to_numpy(), 1) * 100
    return trades


def check_kernel(panel, lines, entries, name, trades) -> bool:
    """mode 0 체결이 ma_exit 의 같은 규칙 체결과 같은지."""
    length = VARIANTS[name][0]
    reference = ma_exit.moving_average_trades(panel, lines, entries, (1, length, 2, 1, 0.0))
    same = all(np.array_equal(reference[column].to_numpy(), trades[column].to_numpy(), equal_nan=True)
               for column in ("exit_t", "exit_price", "reason"))
    print(f"[재현] {name} 체결 {len(trades):,} 건 ma_exit 대비 {'같음' if same else '다름'}", flush=True)
    return same


def check_summary(summary: pd.DataFrame) -> list[str]:
    """기존 slot_summary.csv 의 같은 이름 행과 열마다 정확히 같은지."""
    reference = pd.read_csv(grid.DATA_DIR / "slot_summary.csv", float_precision="round_trip")
    columns = [column for column in reference.columns if column not in ("exit", "seed")]
    lines_out = []

    for name in ("sma60_armed_open", "sma20_armed_open"):
        old = reference[reference["exit"] == name].sort_values("seed").reset_index(drop=True)
        new = summary[summary["exit"] == name].sort_values("seed").reset_index(drop=True)
        differences = [column for column in columns
                       if not np.array_equal(old[column].to_numpy(np.float64), new[column].to_numpy(np.float64))]
        lines_out.append(f"{name}: 20 회 × {len(columns)} 열 " + ("모두 같음" if not differences and len(old) == len(new)
                                                                 else f"다른 열 {differences}"))

    return lines_out


def main() -> None:
    started = time.time()
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                            cwd=Path(__file__).resolve().parent).stdout.strip()
    print(f"커밋 {commit} · 칸 {slot_test.SLOTS} · 회차 {slot_test.SEEDS}", flush=True)
    panel = grid.Panel()
    lines = ma_exit.Lines(panel)
    entries = pd.read_parquet(grid.DATA_DIR / "entries_current.parquet")
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    days = len(panel.dates)
    start = int(entries["entry_t"].min())
    print(f"패널 {panel.dates[start].date()}–{panel.dates[-1].date()} · 매수 후보 {len(entries):,} · "
          f"{time.time() - started:.0f}초", flush=True)
    summary_rows, year_rows, kernel_ok = [], [], {}

    for name, variant in VARIANTS.items():
        trades = variant_trades(panel, lines, entries, variant).reset_index(drop=True)

        if variant[1] == MODE_BASE:
            kernel_ok[name] = check_kernel(panel, lines, entries, name, trades)

        yearly_by_seed = []

        for seed in range(slot_test.SEEDS):
            equity, taken = slot_test.simulate(trades, close_filled, days, seed)
            run_statistics, yearly = slot_test.statistics(equity, panel.dates, start)
            part = trades.loc[taken]
            hold = (part["exit_t"] - part["entry_t"] + 1).to_numpy()
            occupancy = np.zeros(days + 1)
            np.add.at(occupancy, part["entry_t"].to_numpy(), 1)
            np.add.at(occupancy, np.minimum(part["exit_t"].to_numpy() + 1, days), -1)
            unarmed_exit = part[part["reason"].isin([REASON_DEADLINE, REASON_FLOOR, REASON_SMA20])]
            unarmed_end = part[(part["reason"] == 6) & (part["armed_at_exit"] == 0)]
            worst = part.loc[part["net"].idxmin()]
            summary_rows.append({
                "exit": name, "seed": seed, **run_statistics, "trades": len(part), "mean_net": part["net"].mean(),
                "median_net": part["net"].median(), "mean_hold": hold.mean(),
                "slot_use": np.cumsum(occupancy)[start:days].mean() / slot_test.SLOTS * 100,
                "bad_share": (part["net"] <= BAD_TRADE).mean() * 100,
                "unarmed_exits": len(unarmed_exit), "unarmed_exit_mean_net": unarmed_exit["net"].mean(),
                "unarmed_end": len(unarmed_end), "unarmed_end_mean_net": unarmed_end["net"].mean(),
                "worst_net": worst["net"], "worst_ticker": panel.tickers[int(worst["ticker_index"])],
                "worst_entry": panel.dates[int(worst["entry_t"])].date(), "worst_exit": panel.dates[int(worst["exit_t"])].date(),
                "worst_reason": int(worst["reason"])})
            yearly_by_seed.append(yearly)

            if seed == 0:
                part.assign(ticker=panel.tickers[part["ticker_index"].to_numpy()],
                            entry_date=panel.dates[part["entry_t"].to_numpy()],
                            exit_date=panel.dates[part["exit_t"].to_numpy()]).to_parquet(
                    grid.DATA_DIR / f"unarmed_floor_trades_{name}.parquet", index=False)

        yearly = pd.concat(yearly_by_seed, axis=1).median(axis=1)
        year_rows += [{"exit": name, "year": int(year), "return": value} for year, value in yearly.items()]
        print(f"{name} 끝 · {time.time() - started:.0f}초", flush=True)

    summary = pd.DataFrame(summary_rows)
    baseline = summary[summary["exit"] == BASELINE].set_index("seed")["final_multiple"]
    summary["beats_baseline"] = (summary["final_multiple"].to_numpy() > baseline.loc[summary["seed"]].to_numpy()).astype(int)
    summary.to_csv(grid.DATA_DIR / "unarmed_floor_summary.csv", index=False)
    pd.DataFrame(year_rows).to_csv(grid.DATA_DIR / "unarmed_floor_years.csv", index=False)

    for name, same in kernel_ok.items():
        print(f"[재현] 체결 {name}: {'같음' if same else '다름'}")

    for line in check_summary(summary):
        print(f"[재현] slot_summary.csv {line}")

    print(f"끝 · {time.time() - started:.0f}초")


if __name__ == "__main__":
    main()
