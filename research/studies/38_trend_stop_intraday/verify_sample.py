"""스터디 38 대조 — daily_stop_grid.py(numba)의 매매 목록을 원 일봉 파일에서 순수 파이썬으로 따로 계산한 것과 맞춘다.

왜: 시험기는 접은 이평을 가격 구간으로 풀어(닫힌 식) 빠르게 계산한다. 같은 식을 다시 쓰면 같은 실수를 반복하므로
여기서는 이평을 가격마다 직접 계산하고(SMA5·10·20 을 그 가격을 넣어 다시 평균), 존 경계는 호가 격자 위 이분 탐색으로 찾는다.
패널(npz)도 쓰지 않고 PYQuant/data/bars_all_pit_v2.parquet 를 직접 읽는다.

대상: 무작위 종목 N 개(시드 고정)의 2010-01 이후 매매 전부 — 지금 규칙 하나와 사건 손절 변형 몇 개.
산출: data/verify.json (종목·변형별 건수, 일치 건수, 어긋난 건 원문)

사용: py research/studies/38_trend_stop_intraday/verify_sample.py [종목 수]
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import daily_stop_grid as grid  # noqa: E402  비교 대상(numba 결과)과 상수만 가져온다

ROOT = HERE.parents[2]
BARS_PATH = ROOT / "PYQuant" / "data" / "bars_all_pit_v2.parquet"
CHECK_VARIANTS = [grid.CURRENT_RULE, [1, 3, 0, 0, 0, 0, 0], [1, 5, 1, 1, 0, 0, 0], [2, 20, 2, 0, 0, 0, 0],
                  [3, 2, 1, 0, 0, 0, 0], [1, 3, 0, 1, 5, 1, 1]]


def tick(price: float, kosdaq: bool, new_table: bool) -> float:
    if new_table:
        for limit, size in ((2000, 1), (5000, 5), (20000, 10), (50000, 50), (200000, 100), (500000, 500)):
            if price < limit:
                return size

        return 1000

    for limit, size in ((1000, 1), (5000, 5), (10000, 10), (50000, 50)):
        if price < limit:
            return size

    if kosdaq:
        return 100

    for limit, size in ((100000, 100), (500000, 500)):
        if price < limit:
            return size

    return 1000


class Ticker:
    def __init__(self, frame: pd.DataFrame, calendar: pd.DatetimeIndex, kosdaq: bool, new_from: pd.Timestamp):
        frame = frame.set_index("Date").reindex(calendar)
        halted = frame["Close"].notna() & ((frame["Open"] <= 0) | (frame["Volume"].fillna(0) <= 0))
        self.close = frame["Close"].tolist()
        self.open = [close_value if halted_flag else open_price for open_price, close_value, halted_flag in zip(frame["Open"], frame["Close"], halted)]
        self.high = [close_value if halted_flag else bar_value for bar_value, close_value, halted_flag in zip(frame["High"], frame["Close"], halted)]
        self.low = [close_value if halted_flag else bar_value for bar_value, close_value, halted_flag in zip(frame["Low"], frame["Close"], halted)]
        self.halted = halted.tolist()
        self.dates = calendar
        self.kosdaq = kosdaq
        self.new_from = new_from
        valid = [index for index, value in enumerate(self.close) if not math.isnan(value)]
        self.last = valid[-1] if valid else -1

    def tick(self, price, day_index):
        return tick(price, self.kosdaq, self.dates[day_index] >= self.new_from)

    def simple_moving_average_with(self, day_index, length, price):
        return (sum(self.close[day_index - length + 1:day_index]) + price) / length

    def in_entry_zone(self, day_index, price):
        average_5, average_10, average_20 = (self.simple_moving_average_with(day_index, window_length, price) for window_length in (5, 10, 20))
        deviation = (price - average_20) / average_20
        return average_5 > average_10 > average_20 and -grid.PULLBACK <= deviation <= grid.ENTRY_UPPER

    def hold_ok(self, day_index, price):
        previous = [sum(self.close[day_index - window_length:day_index]) / window_length for window_length in (5, 10, 20)]
        average_5, average_10, average_20 = (self.simple_moving_average_with(day_index, window_length, price) for window_length in (5, 10, 20))
        aligned = (previous[0] > previous[1] > previous[2]) or (average_5 > average_10 > average_20)
        deviation = (price - average_20) / average_20
        return aligned and -grid.HOLD_LOW <= deviation <= grid.HOLD_HIGH

    def zone_bounds(self, day_index, test, top):
        """test(t, p) 가 참인 가격 구간 [아래, 위] 를 원 단위 이분 탐색으로 찾는다(구간이 하나라는 가정, 없으면 None)."""
        base = sum(self.close[day_index - 19:day_index]) / 19
        if not test(day_index, base):
            # 기준선 자체가 존 밖이면 구간 안 점을 위아래로 훑어 찾는다
            candidates = [base * (1 + step / 1000) for step in range(-150, 151)]
            inside = [candidate_price for candidate_price in candidates if test(day_index, candidate_price)]
            if not inside:
                return None
            base = inside[0]

        low_out, low_in = 0.0, base
        for _ in range(60):
            middle = (low_out + low_in) / 2
            low_out, low_in = (low_out, middle) if test(day_index, middle) else (middle, low_in)

        high_in, high_out = base, top
        for _ in range(60):
            middle = (high_in + high_out) / 2
            high_in, high_out = (middle, high_out) if test(day_index, middle) else (high_in, middle)

        return low_in, high_in

    def atr(self, day_index):
        total = 0.0
        for bar in range(day_index - 14, day_index):
            previous = self.close[bar - 1]
            total += max(self.high[bar] - self.low[bar], abs(self.high[bar] - previous), abs(self.low[bar] - previous))
        return total / 14

    def entry_ok(self, day_index, start):
        if day_index < start or day_index < 21 or self.halted[day_index] or math.isnan(self.close[day_index]) or self.open[day_index] < grid.MIN_PRICE:
            return False
        if any(math.isnan(value) for value in self.close[day_index - 21:day_index]):
            return False
        sma20 = sum(self.close[day_index - 20:day_index]) / 20
        if self.atr(day_index) / sma20 * 100 > grid.ENTRY_ATR_MAX_PERCENT:
            return False
        return (self.open[day_index] - sma20) / sma20 * 100 >= grid.ENTRY_OPEN_DEVIATION_MIN_PERCENT

    def entry_price(self, day_index):
        bounds = self.zone_bounds(day_index, self.in_entry_zone, self.close[day_index - 1] * 3)
        if bounds is None:
            return None
        zone_low, zone_high = bounds
        base = sum(self.close[day_index - 19:day_index]) / 19
        price_open = self.open[day_index]
        if self.in_entry_zone(day_index, price_open) and price_open <= base:
            price = price_open - self.tick(price_open, day_index)
            return (price, 1) if self.low[day_index] <= price else None
        if price_open >= zone_low:
            step = self.tick(base, day_index)
            price = math.floor(base / step) * step
            return (price, 2) if self.in_entry_zone(day_index, price) and self.low[day_index] <= price else None
        if zone_low <= base:
            step = self.tick(zone_low, day_index)
            price = math.ceil(zone_low / step) * step
            return (price, 3) if self.high[day_index] >= price and self.in_entry_zone(day_index, price) else None
        return None

    def pivots(self, side_bars):
        result = []
        for pivot in range(side_bars, len(self.low) - side_bars):
            value = self.low[pivot]
            if math.isnan(value):
                continue
            neighbours = [self.low[pivot - offset] for offset in range(1, side_bars + 1)] + [self.low[pivot + offset] for offset in range(1, side_bars + 1)]
            if any(math.isnan(neighbour) for neighbour in neighbours):
                continue
            if all(self.low[pivot - offset] > value for offset in range(1, side_bars + 1)) and all(self.low[pivot + offset] >= value for offset in range(1, side_bars + 1)):
                result.append((pivot, value))
        return result

    def reference(self, kind, parameter, day_index, price, pivots):
        if kind in (1, 3):
            known = [(day, value) for day, value in pivots if day + parameter <= day_index - 1 and value < price]
            return known[-1][1] if known else None
        window = [value for value in self.low[max(0, day_index - parameter):day_index] if not math.isnan(value)]
        if window and min(window) < price:
            return min(window)
        for back in range(day_index - 1, -1, -1):
            if not math.isnan(self.low[back]) and self.low[back] < price:
                return self.low[back]
        return None

    def exit(self, variant, day_index, price, stop, pivots):
        kind, parameter, timing, raise_stop, take_profit, zone_exit, entry_day_tp = variant
        target = price * (1 + take_profit / 100)
        intraday = kind in (0, 1, 2) and timing == 0
        known_before = [value for day, value in pivots if day + parameter <= day_index - 1]
        previous_pivot = known_before[-1] if known_before else None
        seen = {day for day, _ in pivots if day + parameter <= day_index - 1}

        for day in range(day_index, self.last + 1):
            if day > day_index and (self.halted[day] or math.isnan(self.close[day])):
                continue
            if day > day_index and raise_stop:
                if kind == 1:
                    for pivot_day, value in pivots:
                        if day_index <= pivot_day + parameter <= day - 1 and value > stop:
                            stop = value
                elif kind == 2:
                    window = [bar_value for bar_value in self.low[day - parameter:day] if not math.isnan(bar_value)]
                    if window and min(window) > stop:
                        stop = min(window)

            zone_low, zone_high = -math.inf, math.inf
            if zone_exit and day > day_index:
                bounds = self.zone_bounds(day, self.hold_ok, self.close[day - 1] * 3)
                if bounds:
                    zone_low, zone_high = bounds
                else:
                    # 어떤 가격도 보유 존이 아니다 — 첫 판정(시가)에 판다
                    zone_low = math.inf
            line = stop if intraday else -math.inf

            if day > day_index:
                open_price = self.open[day]
                if open_price <= line:
                    return day, open_price, 1
                if open_price < zone_low:
                    return day, open_price, 3
                if take_profit > 0 and open_price >= target:
                    return day, open_price, 2
                if open_price > zone_high:
                    return day, open_price, 4

            downs = []
            if self.low[day] <= line:
                downs.append((line, 1))
            if self.low[day] < zone_low:
                downs.append((zone_low, 3))
            ups = []
            if take_profit > 0 and self.high[day] >= target and (day > day_index or entry_day_tp):
                ups.append((target, 2))
            if self.high[day] > zone_high:
                ups.append((zone_high, 4))
            if downs:
                value, reason = max(downs)
                return day, value, reason
            if ups:
                value, reason = min(ups)
                return day, value, reason

            flip = False
            if kind == 3:
                for pivot_day, value in pivots:
                    if pivot_day not in seen and pivot_day + parameter <= day:
                        seen.add(pivot_day)
                        if pivot_day + parameter >= day_index and previous_pivot is not None and value < previous_pivot:
                            flip = True
                        previous_pivot = value
            broke = kind in (0, 1, 2) and timing != 0 and self.close[day] < stop
            if flip or broke:
                reason = 5 if flip else 1
                if timing == 1:
                    return day, self.close[day], reason
                for later in range(day + 1, self.last + 1):
                    if not self.halted[later] and not math.isnan(self.close[later]):
                        return later, self.open[later], reason
                return day, self.close[day], reason

        return self.last, self.close[self.last], 6 if self.last == len(self.close) - 1 else 8

    def run(self, variant, start):
        kind, parameter = variant[0], int(variant[1])
        pivots = self.pivots(parameter if kind in (1, 3) else 1)
        trades = []
        day_index = 21
        while day_index <= self.last:
            if not self.entry_ok(day_index, start):
                day_index += 1
                continue
            found = self.entry_price(day_index)
            if found is None:
                day_index += 1
                continue
            price, path = found
            if kind == 0:
                stop = price * (1 - variant[1] / 100)
            else:
                stop = self.reference(kind, parameter, day_index, price, pivots)
                if stop is None:
                    day_index += 1
                    continue
                if kind == 3:
                    stop = -math.inf
            exit_t, exit_price, reason = self.exit(variant, day_index, price, stop, pivots)
            trades.append((day_index, price, exit_t, exit_price, reason, path))
            day_index = exit_t + 1
        return trades


def main() -> None:
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    panel = grid.Panel()
    random_engine = np.random.default_rng(20261010)
    chosen = sorted(random_engine.choice(len(panel.tickers), count, replace=False))
    codes = [panel.tickers[index] for index in chosen]
    bars = pd.read_parquet(grid.ROOT_BARS if hasattr(grid, "ROOT_BARS") else BARS_PATH,
                           columns=["Date", "Open", "High", "Low", "Close", "Volume", "code"],
                           filters=[("code", "in", codes)])
    calendar = panel.dates
    start = panel.start_index
    report = []

    for variant in CHECK_VARIANTS:
        numba_trades, _ = panel.run(variant)
        for index, code in zip(chosen, codes):
            frame = bars[bars["code"] == code]
            ticker = Ticker(frame, calendar, bool(panel.market[index] == 1), pd.Timestamp(grid.NEW_TICK_TABLE_FROM))
            mine = ticker.run(variant, start)
            theirs = numba_trades[numba_trades["ticker_index"] == index]
            theirs = [(int(record.entry_t), record.entry_price, int(record.exit_t), record.exit_price, int(record.reason), int(record.entry_path))
                      for record in theirs.itertuples()]
            matched = sum(1 for mine_trade, their_trade in zip(mine, theirs) if mine_trade[0] == their_trade[0] and mine_trade[2] == their_trade[2] and mine_trade[4] == their_trade[4]
                          and abs(mine_trade[1] - their_trade[1]) < 1e-6 * max(1, their_trade[1]) and abs(mine_trade[3] - their_trade[3]) < 1e-6 * max(1, their_trade[3]))
            first_difference = next(([list(map(float, mine_trade)), list(map(float, their_trade))] for mine_trade, their_trade in zip(mine, theirs) if mine_trade != their_trade
                               and not (mine_trade[0] == their_trade[0] and mine_trade[2] == their_trade[2] and mine_trade[4] == their_trade[4]
                                        and abs(mine_trade[1] - their_trade[1]) < 1e-6 * max(1, their_trade[1])
                                        and abs(mine_trade[3] - their_trade[3]) < 1e-6 * max(1, their_trade[3]))), None)
            report.append({"variant": grid.variant_key(variant), "ticker": code, "name": str(panel.names[index]),
                           "python_trades": len(mine), "numba_trades": len(theirs), "matched": matched,
                           "first_difference": first_difference})
        rows = [row for row in report if row["variant"] == grid.variant_key(variant)]
        print(f"{grid.variant_key(variant)}: 파이썬 {sum(record['python_trades'] for record in rows)}건 · numba "
              f"{sum(record['numba_trades'] for record in rows)}건 · 일치 {sum(record['matched'] for record in rows)}건", flush=True)

    (HERE / "data" / "verify.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
