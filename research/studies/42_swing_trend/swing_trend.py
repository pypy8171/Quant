"""스터디 42-1 — 고점·저점 추세 매매 건 전부와 매수 시점 조건 값(2026-10-11).

고점 = 좌우 w일 안에서 가장 높은 고가, 저점 = 가장 낮은 저가. w일 뒤에야 확정되므로 확정된 것만 쓴다(앞을 보지 않음).
최근 확정 고점 H1(앞)·H2(뒤), 저점 L1·L2.
추세 확인 = 저점이 높아진 상태(L2 > L1)에서 종가가 H2를 처음 넘은 날(신호일).
  breakout 매수: 신호일 종가.
  retest 매수: 신호일 뒤 장중 H2까지 내려와 닿으면 H2(호가 내림)에 산다. 그 전에 종가가 L2 아래면 취소. 기다리는 날 제한 없음.
매도: 종가가 손절선 아래인 날 그 종가. 손절선 = 매수 때 L2, 이후 더 높은 저점이 확정되면 그 저점으로 올린다.
한 종목 한 번에 한 건. 보유 종목 수 제한 없음. 비용 = Panel.net_return(수수료·세금·1호가).
하루 ±30%(가격제한폭)를 넘는 날이 보유 중에 낀 건은 시세 자료 오류(액면 병합 미반영 등)로 보고 data_error 열에 표시한다.

조건 값(전부 신호일까지의 자료):
  base_days      신호일 − H1 날짜(바닥을 다진 기간)
  base_depth     1 − min(L1, L2) / H2(바닥 깊이)
  low_rise       L2 / L1 − 1(저점이 높아진 정도)
  high_rise      H2 / H1 − 1(고점이 높아진 정도, 음수면 앞 고점 아래 돌파)
  year_high      신호일 종가 / 직전 250일 최고가(1 넘으면 1년 신고가)
  run20          신호일 종가 / 20일 전 종가 − 1
  stop_gap       1 − 손절선 / 매수가(손절까지 거리)
  market_trend   시장지수 / 시장지수 60일 평균 − 1
  relative60     종목 60일 수익 − 시장 60일 수익
  above240       신호일 종가 / 240일 평균 − 1
  atr_percent    14일 ATR / 20일 평균 × 100(신호일 하루 등락폭 크기 — 42-0 측정에서 4.5% 이하 건당 +4.15%, 초과 −1.64%)
  cap_decile·turnover20_decile  시가총액·20일 거래대금 순위(0 작음 – 9 큼, 신호일)

산출: data/trades_w<w>_<mode>.parquet. 사용: py research/studies/42_swing_trend/swing_trend.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import numba
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "38_trend_stop_intraday"))
sys.path.insert(0, str(HERE.parent / "40_devscale_entry_touch"))

import daily_stop_grid as grid  # noqa: E402
from turn_points import market_index  # noqa: E402

DATA_DIR = HERE / "data"
WINDOWS = (10, 20)
MODES = ("breakout", "retest")
FIELDS = ["entry_t", "entry_price", "exit_t", "exit_price", "signal_t", "high1", "high2", "low1", "low2", "high1_t",
          "stop"]


@numba.njit(cache=True)
def is_extreme(values, day, window, sign):
    center = values[day]

    if not center == center:
        return False

    for index in range(day - window, day + window + 1):
        value = values[index]

        if value == value and sign * value > sign * center:
            return False

    return True


@numba.njit(cache=True)
def remember(snapshot, high1, high2, low1, low2, high1_t):
    snapshot[0] = high1
    snapshot[1] = high2
    snapshot[2] = low1
    snapshot[3] = low2
    snapshot[4] = float(high1_t)


@numba.njit(cache=True)
def ticker_trades(open_, high, low, close, halted, market, new_from, start, window, retest, out):
    days = close.shape[0]
    high1 = high2 = low1 = low2 = np.nan
    high1_t = high2_t = -1
    filled = 0
    holding = armed = False
    level = cancel = stop = entry_price = 0.0
    entry_t = signal_t = 0
    snapshot = np.zeros(5)

    for day in range(2 * window + 2, days):
        yesterday = day - 1
        candidate = yesterday - window

        if is_extreme(high, candidate, window, 1.0):
            high1, high2, high1_t, high2_t = high2, high[candidate], high2_t, candidate

        if is_extreme(low, candidate, window, -1.0):
            low1, low2 = low2, low[candidate]

            if holding and low2 > stop:
                stop = low2

        if not (high1 == high1 and low1 == low1) or halted[day] or not close[day] > 0:
            continue

        if holding:
            if (day > entry_t or retest) and close[day] < stop:
                out[filled, 2] = day
                out[filled, 3] = close[day]
                filled += 1
                holding = False

            continue

        if day < start:
            continue

        if not retest:
            if low2 > low1 and close[day] > high2 and close[yesterday] <= high2 and close[day] >= 2000.0:
                holding, entry_t, entry_price, stop, signal_t = True, day, close[day], low2, day
                remember(snapshot, high1, high2, low1, low2, high1_t)
        else:
            if low2 > low1 and close[yesterday] > high2 and close[yesterday - 1] <= high2:
                armed, level, cancel, signal_t = True, high2, low2, yesterday
                remember(snapshot, high1, high2, low1, low2, high1_t)

            if armed and close[yesterday] < cancel:
                armed = False

            if armed and open_[day] > level and low[day] <= level:
                step = grid.tick_size(level, market, day >= new_from)
                price = np.floor(level / step) * step
                armed = False

                if price >= 2000.0:
                    holding, entry_t, entry_price, stop = True, day, price, cancel

        if holding:
            out[filled, 0] = entry_t
            out[filled, 1] = entry_price
            out[filled, 4] = signal_t
            out[filled, 5:10] = snapshot
            out[filled, 10] = stop

    if holding:
        out[filled, 2] = days - 1
        out[filled, 3] = close[days - 1]
        filled += 1

    return filled


@numba.njit(parallel=True, cache=True)
def all_trades(open_, high, low, close, halted, market, new_from, start, window, retest, out, counts):
    for ticker in numba.prange(close.shape[0]):
        counts[ticker] = ticker_trades(open_[ticker], high[ticker], low[ticker], close[ticker], halted[ticker],
                                       market[ticker], new_from, start, window, retest, out[ticker])


def features(trades: pd.DataFrame, panel: grid.Panel, close_filled: np.ndarray, market: np.ndarray) -> pd.DataFrame:
    ticker = trades["ticker"].to_numpy()
    signal = trades["signal_t"].to_numpy()
    close_at = close_filled[ticker, signal]
    year_high = pd.DataFrame(panel.high.T).rolling(250, min_periods=120).max().shift(1).to_numpy().T
    sma240 = pd.DataFrame(close_filled.T).rolling(240).mean().to_numpy().T
    market60 = pd.Series(market).rolling(60).mean().to_numpy()
    deciles = np.load(grid.PANEL_PATH)
    trades["base_days"] = signal - trades["high1_t"].to_numpy()
    trades["base_depth"] = 1 - np.minimum(trades["low1"], trades["low2"]) / trades["high2"]
    trades["low_rise"] = trades["low2"] / trades["low1"] - 1
    trades["high_rise"] = trades["high2"] / trades["high1"] - 1
    trades["year_high"] = close_at / year_high[ticker, signal]
    trades["run20"] = close_at / close_filled[ticker, signal - 20] - 1
    trades["stop_gap"] = 1 - trades["stop"] / trades["entry_price"]
    trades["market_trend"] = market[signal] / market60[signal] - 1
    trades["relative60"] = close_at / close_filled[ticker, signal - 60] - market[signal] / market[signal - 60]
    trades["above240"] = close_at / sma240[ticker, signal] - 1
    atr = np.vstack([grid.atr_series(panel.high[code], panel.low[code], panel.close[code])
                     for code in range(panel.close.shape[0])])
    sma20 = pd.DataFrame(close_filled.T).rolling(20).mean().to_numpy().T
    trades["atr_percent"] = atr[ticker, signal] / sma20[ticker, signal] * 100
    trades["cap_decile"] = deciles["cap_decile"][ticker, signal]
    trades["turnover20_decile"] = deciles["turnover20_decile"][ticker, signal]
    return trades


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    panel = grid.Panel()
    close_filled = pd.DataFrame(panel.close.T).ffill().to_numpy().T.copy()
    daily_move = np.abs(close_filled[:, 1:] / close_filled[:, :-1] - 1)
    market = market_index(panel)
    tickers = panel.close.shape[0]

    for window in WINDOWS:
        for retest, mode in enumerate(MODES):
            out = np.zeros((tickers, 3000, len(FIELDS)))
            counts = np.zeros(tickers, np.int64)
            all_trades(panel.open, panel.high, panel.low, panel.close, panel.halted, panel.market, panel.new_from,
                       int(panel.start_index), window, bool(retest), out, counts)
            rows = np.concatenate([out[ticker, :counts[ticker]] for ticker in range(tickers) if counts[ticker]])
            trades = pd.DataFrame(rows, columns=FIELDS)
            trades.insert(0, "ticker", np.repeat(np.arange(tickers), counts))
            for column in ("entry_t", "exit_t", "signal_t", "high1_t"):
                trades[column] = trades[column].astype(np.int64)
            ticker, entry_t, exit_t = trades["ticker"].to_numpy(), trades["entry_t"].to_numpy(), trades["exit_t"].to_numpy()
            trades["net"] = panel.net_return(ticker, entry_t, trades["entry_price"].to_numpy(), exit_t,
                                             trades["exit_price"].to_numpy(), 1) * 100
            trades["hold"] = exit_t - entry_t + 1
            trades["open"] = exit_t == panel.end_index
            trades["data_error"] = [bool(exit > entry and np.nanmax(daily_move[code, entry:exit]) > 0.305)
                                    for code, entry, exit in zip(ticker, entry_t, exit_t)]
            trades = features(trades[np.isfinite(trades["net"])].reset_index(drop=True), panel, close_filled, market)
            trades.to_parquet(DATA_DIR / f"trades_w{window}_{mode}.parquet", index=False)
            clean = trades[~trades["data_error"]]
            print(f"w{window} {mode}: {len(trades):,}건(자료 오류 {trades['data_error'].sum()}) · 건당 {clean['net'].mean():.2f}% "
                  f"· 중앙 {clean['net'].median():.2f}% · 이긴 비율 {(clean['net'] > 0).mean() * 100:.1f}%", flush=True)


if __name__ == "__main__":
    main()
