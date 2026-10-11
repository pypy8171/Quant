"""스터디 42-4 — 다음 거래일 매수 후보(2026-10-11).

swing_trend.py의 retest 규칙을 패널 마지막 날까지 돌린 뒤, 다음 거래일 아침 기준 상태를 낸다.
  대기: 추세 확인(L2 > L1, 종가가 H2를 처음 넘음)이 났고, 아직 H2까지 되돌리지 않았고, 종가가 L2 아래로 간 적도 없는 종목.
  매수 주문: H2(호가 내림) 지정가. 다음 거래일 시가가 H2 이하면 사지 않는다(시험과 같게 — 시가가 위에 있다가 내려와 닿을 때만).
  손절선: L2. 보유 중 더 높은 저점이 확정되면 그 저점으로 올린다. 종가가 손절선 아래면 그날 종가에 판다.
마지막 날 거래가 없는 종목(상장폐지·거래정지)은 뺀다. reachable = 매수가가 마지막 종가의 −30%(하한가) 안 — 다음 날 닿을 수 있는 종목.
거름(filter_rule.py 앞 4개, 신호일 기준): year_high ≥ 0.809, above240 0.0169–0.32, run20 ≤ 0.264, atr_percent ≤ 5.9.
산출: data/candidates_<패널 마지막 날>.csv, live/plan.json(엔진 SwingTrend가 읽는 다음 거래일 매수 계획 — 닿을 수 있는 종목만).
사용: py research/studies/42_swing_trend/candidates.py
      py research/studies/42_swing_trend/candidates.py --nightly --wait-minutes 45   # 밤 예약작업: 일봉 갱신을 기다려 패널부터 다시 만든다
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path

import numba
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from swing_trend import FIELDS, features, grid, is_extreme, market_index  # noqa: E402

WINDOW = 20
PLAN_PATH = HERE / "live" / "plan.json"
BARS_READY_AFTER = dt.time(15, 40)  # 일봉 갱신이 이 시각 뒤에 끝났으면 오늘 종가까지 들어 있다


@numba.njit(cache=True)
def next_day_state(high, low, close, halted, window, out):
    """다음 거래일 아침의 대기 상태. out = [armed, level, cancel, signal_t, high1, high2, low1, low2, high1_t]."""
    days = close.shape[0]
    high1 = high2 = low1 = low2 = np.nan
    high1_t = high2_t = -1
    armed = False
    level = cancel = 0.0
    signal_t = 0
    snapshot = np.zeros(5)

    # day = days 는 다음 거래일(자료 없음). 그날 아침까지 확정되는 고점·저점과 신호만 반영한다.
    for day in range(2 * window + 2, days + 1):
        yesterday = day - 1
        candidate = yesterday - window

        if is_extreme(high, candidate, window, 1.0):
            high1, high2, high1_t, high2_t = high2, high[candidate], high2_t, candidate

        if is_extreme(low, candidate, window, -1.0):
            low1, low2 = low2, low[candidate]

        if not (high1 == high1 and low1 == low1):
            continue

        if low2 > low1 and close[yesterday] > high2 and close[yesterday - 1] <= high2:
            armed, level, cancel, signal_t = True, high2, low2, yesterday
            snapshot[0] = high1
            snapshot[1] = high2
            snapshot[2] = low1
            snapshot[3] = low2
            snapshot[4] = float(high1_t)

        if armed and close[yesterday] < cancel:
            armed = False

        if day < days and armed and not halted[day] and close[day] > 0 and high[day] > level and low[day] <= level:
            armed = False

    out[0] = 1.0 if armed else 0.0
    out[1] = level
    out[2] = cancel
    out[3] = signal_t
    out[4:9] = snapshot


def wait_for_bars(wait_minutes: int) -> None:
    """밤 일봉 갱신(naver_bars_backfill --refresh)이 오늘 장 마감 뒤에 끝날 때까지 기다린다. 시간이 다 되면 있는 자료로 간다."""
    import daily_panel

    cut = dt.datetime.combine(dt.date.today(), BARS_READY_AFTER)
    deadline = dt.datetime.now() + dt.timedelta(minutes=wait_minutes)

    while dt.datetime.fromtimestamp(daily_panel.BARS_PATH.stat().st_mtime) < cut and dt.datetime.now() < deadline:
        time.sleep(60)

    modified = dt.datetime.fromtimestamp(daily_panel.BARS_PATH.stat().st_mtime)
    print(f"일봉 파일 갱신 시각 {modified:%Y-%m-%d %H:%M}", flush=True)
    daily_panel.build()


def write_plan(picks: pd.DataFrame, as_of: str) -> None:
    entries = [{"ticker": row.code, "name": row.name, "entry_price": int(row.entry_price),
                "cancel_below": int(row.stop), "signal_date": row.signal_date}
               for row in picks[picks["reachable"]].itertuples(index=False)]
    plan = {"as_of": as_of, "generated_at": dt.datetime.now().strftime("%Y-%m-%d %H:%M"), "entries": entries}
    PLAN_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary = PLAN_PATH.with_suffix(".tmp")
    temporary.write_bytes(json.dumps(plan, ensure_ascii=False, indent=1).encode("utf-8"))
    temporary.replace(PLAN_PATH)
    print(f"계획 {len(entries)}종목 → {PLAN_PATH}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nightly", action="store_true")
    parser.add_argument("--wait-minutes", type=int, default=45)
    arguments = parser.parse_args()
    sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))

    if arguments.nightly:
        wait_for_bars(arguments.wait_minutes)

    panel = grid.Panel()
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    market = market_index(panel)
    tickers = panel.close.shape[0]
    raw = np.load(grid.PANEL_PATH, allow_pickle=True)
    rows = []

    for ticker in range(tickers):
        out = np.zeros(9)
        next_day_state(panel.high[ticker], panel.low[ticker], panel.close[ticker], panel.halted[ticker], WINDOW, out)

        if out[0] == 1.0:
            rows.append([ticker, *out[1:]])

    state = pd.DataFrame(rows, columns=["ticker", "level", "cancel", "signal_t", "high1", "high2", "low1", "low2",
                                        "high1_t"])
    state[["ticker", "signal_t", "high1_t"]] = state[["ticker", "signal_t", "high1_t"]].astype(np.int64)
    last = panel.end_index
    new_rules = last + 1 >= panel.new_from
    state["entry_price"] = [np.floor(level / grid.tick_size(level, panel.market[ticker], new_rules))
                            * grid.tick_size(level, panel.market[ticker], new_rules)
                            for ticker, level in zip(state["ticker"], state["level"])]
    state["stop"] = state["cancel"]
    state = features(state, panel, close_filled, market)
    passed = ((state["year_high"] >= 0.809) & state["above240"].between(0.0169, 0.32) & (state["run20"] <= 0.264)
              & (state["atr_percent"] <= 5.9) & (state["entry_price"] >= 2000.0)
              & np.isfinite(panel.close[state["ticker"], last]) & ~panel.halted[state["ticker"], last])
    picks = state[passed].copy()
    picks.insert(0, "code", raw["tickers"][picks["ticker"]])
    picks.insert(1, "name", raw["names"][picks["ticker"]])
    picks["last_close"] = close_filled[picks["ticker"], last]
    picks["to_entry"] = picks["entry_price"] / picks["last_close"] - 1
    picks["signal_date"] = panel.dates[picks["signal_t"]].strftime("%Y-%m-%d")
    columns = ["code", "name", "signal_date", "last_close", "entry_price", "to_entry", "stop", "stop_gap", "year_high",
               "above240", "run20", "atr_percent", "cap_decile", "turnover20_decile"]
    picks["reachable"] = picks["to_entry"] > -0.30
    picks = picks[columns + ["reachable"]].sort_values("to_entry", ascending=False)
    path = HERE / "data" / f"candidates_{panel.dates[last]:%Y-%m-%d}.csv"
    picks.to_csv(path, index=False, encoding="utf-8-sig")
    pd.set_option("display.width", 250)
    pd.set_option("display.max_rows", 300)
    print(f"대기 {len(state)}종목 · 거름 통과 {len(picks)}종목 → {path}")
    print(picks.round(3).to_string(index=False))
    write_plan(picks, f"{panel.dates[last]:%Y-%m-%d}")


if __name__ == "__main__":
    main()
