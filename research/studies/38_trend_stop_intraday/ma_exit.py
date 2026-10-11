"""스터디 38 — 이평선으로 추세가 깨진 자리를 판정해 파는 청산(사용자 정의, 2026-10-11).

왜: 일수로 정한 선(N 일 최저·확정 저점 k)은 추세를 재지 않는다. 추세가 깨졌다는 것은 이평선 배열이 무너졌는가로 본다.
  지금 규칙의 매수 건(entries_current.parquet, 130,649 건)을 그대로 두고 청산만 바꿔(짝 비교) 같은 구간 대비로 잰다.

청산 규칙(rule):
  - touch n: 가격이 SMA n 에 닿으면 판다. n = 5·10·20·60.
      장중(timing 0): 오늘 가격 p 를 접어 넣은 SMA n 과 같아지는 값 = 앞 n−1 일 종가 평균. 저가가 거기 닿으면
        min(시가, 그 값)에 판다. 종가(1): 종가 < 그날 SMA n 이면 종가에. 다음 날 시가(2): 같은 조건이면 다음 거래일 시가에.
      armed 1 이면 매수 뒤 종가가 그 이평 위에 한 번 올라선 뒤부터 본다(이평 아래에서 산 건이 바로 팔리지 않게).
  - unalign: 종가 기준 SMA5 > SMA10 > SMA20 가 깨지면(정배열 해제) 판다. timing 1·2.
  - inverse: 종가 기준 SMA5 < SMA10 < SMA20(역배열)이 되면 판다. timing 1·2.
  - 익절 tp %(0 이면 없음): 평단 기준 지정가. 같은 날 이평 선과 둘 다 닿으면 이평 쪽(불리)으로 처리하고 ambiguous 로 센다.
  손절선은 두지 않는다. 판정은 매수 다음 거래일부터, 거래정지일은 건너뛴다. 데이터 끝(상장폐지 포함)까지 안 팔리면
  마지막 종가로 평가(reason 6).
[limit] 장중 매도가는 호가 단위로 맞추지 않는다(이평 값 그대로). 수정주가 기준이라 분할 전 구간 호가는 실제와 다르다.

사용: control_test.py ma 가 부른다(산출 data/control_ma_*.csv).
"""
from __future__ import annotations

import numba
import numpy as np
import pandas as pd

import daily_stop_grid as grid

RULES = {1: "touch", 2: "unalign", 3: "inverse"}
TOUCH_LENGTHS = (5, 10, 20, 60)
REASON_LINE, REASON_TAKE_PROFIT, REASON_END = 1, 2, 6


def moving_average_variants() -> list[tuple]:
    """(rule, n, timing, armed, tp). tp 는 0(익절 없음)과 지금 규칙의 3%."""
    variants = []

    for take_profit in (0.0, 3.0):
        for length in TOUCH_LENGTHS:
            for timing in (0, 1, 2):
                for armed in (0, 1):
                    variants.append((1, length, timing, armed, take_profit))

        for rule in (2, 3):
            for timing in (1, 2):
                variants.append((rule, 20, timing, 0, take_profit))

    return variants


def moving_average_key(variant) -> str:
    rule, length, timing, armed, take_profit = variant
    return f"ma_{RULES[rule]}_{length}_{timing}_{armed}_{take_profit:g}"


class Lines:
    """종목 × 날짜 이평(종가는 정지일을 앞 값으로 채워 계산)."""

    def __init__(self, panel: grid.Panel):
        filled = pd.DataFrame(panel.close.T).ffill()
        self.sma = {length: np.ascontiguousarray(filled.rolling(length).mean().to_numpy().T)
                    for length in sorted(set(TOUCH_LENGTHS) | {5, 10, 20})}
        # 장중 닿는 값: 오늘 가격 p 를 넣은 SMA n 이 p 와 같아지는 p = 앞 n−1 일 종가 평균.
        self.touch = {length: np.ascontiguousarray(filled.rolling(length - 1).mean().shift(1).to_numpy().T)
                      for length in TOUCH_LENGTHS}


@numba.njit(parallel=True, cache=True)
def moving_average_exits(open_, high, low, close, halted, ticker, entry_t, entry_price, rule, timing, armed_needed, take_profit,
             line, touch, sma5, sma10, sma20, out):
    """매수 건마다 매도(날, 가격, 이유, ambiguous). line·touch 는 rule 1 에서 쓰는 SMA n·장중 닿는 값."""
    last_day = close.shape[1] - 1

    for index in numba.prange(ticker.shape[0]):
        stock = ticker[index]
        start = entry_t[index]
        price = entry_price[index]
        target = price * (1.0 + take_profit) if take_profit > 0 else np.inf
        armed = (not armed_needed) or (np.isfinite(close[stock, start]) and close[stock, start] > line[stock, start])
        pending = False
        last_valid = start
        out[index, 0] = -1

        for day in range(start + 1, last_day + 1):
            if halted[stock, day] or not np.isfinite(open_[stock, day]) or open_[stock, day] <= 0:
                continue

            last_valid = day

            if pending:
                out[index, 0] = day
                out[index, 1] = open_[stock, day]
                out[index, 2] = 1
                out[index, 3] = 0
                break

            hit_target = high[stock, day] >= target
            line_price = np.nan

            if rule == 1 and timing == 0 and armed and np.isfinite(touch[stock, day]) and low[stock, day] <= touch[stock, day]:
                line_price = min(open_[stock, day], touch[stock, day])

            if np.isfinite(line_price):
                out[index, 0] = day
                out[index, 1] = line_price
                out[index, 2] = 1
                out[index, 3] = 1 if hit_target else 0
                break

            if hit_target:
                out[index, 0] = day
                out[index, 1] = max(open_[stock, day], target)
                out[index, 2] = 2
                out[index, 3] = 0
                break

            broken = False

            if rule == 1 and timing > 0 and armed:
                broken = close[stock, day] < line[stock, day]
            elif rule == 2:
                broken = not (sma5[stock, day] > sma10[stock, day] and sma10[stock, day] > sma20[stock, day])
            elif rule == 3:
                broken = sma5[stock, day] < sma10[stock, day] and sma10[stock, day] < sma20[stock, day]

            if broken and timing == 1:
                out[index, 0] = day
                out[index, 1] = close[stock, day]
                out[index, 2] = 1
                out[index, 3] = 0
                break

            if broken and timing == 2:
                pending = True

            if rule == 1 and not armed and close[stock, day] > line[stock, day]:
                armed = True

        if out[index, 0] < 0:
            out[index, 0] = last_valid
            out[index, 1] = close[stock, last_valid]
            out[index, 2] = 6
            out[index, 3] = 0


def moving_average_trades(panel: grid.Panel, lines: Lines, entries: pd.DataFrame, variant) -> pd.DataFrame:
    rule, length, timing, armed, take_profit = variant
    ticker = entries["ticker_index"].to_numpy(np.int64)
    entry_t = entries["entry_t"].to_numpy(np.int64)
    entry_price = entries["entry_price"].to_numpy(np.float64)
    out = np.zeros((len(entries), 4))
    line = lines.sma[length if rule == 1 else 20]
    touch = lines.touch[length if rule == 1 else 20]
    moving_average_exits(panel.open, panel.high, panel.low, panel.close, panel.halted, ticker, entry_t, entry_price, rule, timing,
             bool(armed), take_profit / 100.0, line, touch, lines.sma[5], lines.sma[10], lines.sma[20], out)
    return pd.DataFrame({"ticker_index": ticker, "entry_t": entry_t, "entry_price": entry_price,
                         "exit_t": out[:, 0].astype(np.int64), "exit_price": out[:, 1], "reason": out[:, 2],
                         "ambiguous": out[:, 3]})
