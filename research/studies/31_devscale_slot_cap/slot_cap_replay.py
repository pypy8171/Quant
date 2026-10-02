#!/usr/bin/env python3
"""스터디 31 — DevScale 동시 보유 상한 N × 종목당 예산(자본 200만 ÷ N) 포트폴리오 리플레이.

`PYQuant/backtest/devscale_replay.py`는 종목·날짜를 서로 독립으로 재생해 슬롯 경합이 없다. 여기서는 같은 규칙
(`ZONE_WIDTH_VARIANTS["z_lo8_up5_tp5"]` = 10-01 실계좌 칸: 넘김·익절 5·손절 6.5·ATR≤4.5·개장 이격≥−1·
그날 재진입 없음·가격 2,000~100,000)을 **모든 종목을 같은 3분봉 시각에 맞춰 함께** 돌리고, 그 위에 포트폴리오 층을 얹는다.

  - 슬롯 = 보유 종목 ∪ 매수 지정가가 걸린 종목(엔진 OrderGate 3c와 같은 셈). 상한이면 새 베이스 BUY를 내지 않는다.
  - 같은 봉에 신호가 여럿이면 우선순위 키 순으로 슬롯을 준다. 엔진은 스캐너 점수 순위(유효 랭크 비율)로 막지만
    점수를 복원하지 못해 근사한다 — `turnover`(전일까지 20일 평균 거래대금 큰 순) 또는 `random`(seed별 무작위).
    점수 우선순위 바(남은 슬롯이 적을수록 상위만 통과)는 옮기지 않았다.
  - 수량 = floor(min(예산, 쓸 수 있는 현금) / 지정가). 현금 = 200만 + 실현손익 − 보유 원가 − 다른 종목 매수 대기 예약.
  - 비용은 `PYQuant/backtest/costs.py` LIVE(하네스와 같다).

N=0은 상한 없음·현금 무제한·종목당 50만 — 하네스 단독 리플레이와 같아야 한다(패리티: 스터디 28의 같은 칸 172건·59.2만).

    py -X utf8 research/studies/31_devscale_slot_cap/slot_cap_replay.py --jobs 14
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import pickle
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[2]
sys.path.insert(0, str(_REPO / "PYQuant"))
from backtest.costs import LIVE, fill_result  # noqa: E402
from backtest.devscale_replay import (DAILY_PARQUET, MINUTE_DIR, ZONE_WIDTH_VARIANTS, DailyBook, aligned,  # noqa: E402
                                      load_pairs, resample, round_tick, tick_size)

CAPITAL = 2_000_000.0
PARAMS = ZONE_WIDTH_VARIANTS["z_lo8_up5_tp5"]
SPLIT = 0.70


# ───────────────────────── 데이터 적재(종목 단위 병렬) ─────────────────────────

def _load_ticker_chunk(chunk_arguments: tuple[list[str], dict[str, list[str]]]) -> dict:
    """종목 묶음의 (종목, 날짜) → 3분봉 배열·전일 SMA·ATR14·연속 여부. 하네스 `_replay_pairs`의 건너뛰기 규칙 그대로."""
    tickers, days_by_ticker = chunk_arguments
    book = DailyBook(set(tickers))
    out = {}
    for ticker in tickers:
        last_ymd = None
        for day in sorted(days_by_ticker[ticker]):
            path = MINUTE_DIR / ticker / f"{day}.parquet"
            if not path.exists():
                continue
            minute_bars = pd.read_parquet(path)
            if len(minute_bars) < 300:
                continue
            moving_averages = book.moving_averages_before(ticker, day)
            if moving_averages is None:
                continue
            bars = resample(minute_bars, 3)
            atr14 = book.atr14_before(ticker, day)
            contiguous = book.is_next_trading_day(ticker, last_ymd, day)
            last_ymd = day
            turnover = float((minute_bars["close"] * minute_bars["volume"]).sum())
            out[(ticker, day)] = {"arr": bars[["open", "high", "low", "close", "hhmm"]].to_numpy(dtype=float),
                                  "ma": moving_averages, "atr14": atr14, "contiguous": contiguous, "turnover": turnover}
    return out


def load_all(pairs: list[tuple[str, str]], jobs: int, cache: Path | None) -> dict:
    if cache and cache.exists():
        return pickle.loads(cache.read_bytes())
    days_by_ticker: dict[str, list[str]] = {}
    for ticker, day in pairs:
        days_by_ticker.setdefault(ticker, []).append(day)
    tickers = sorted(days_by_ticker)
    chunks = [tickers[index::jobs] for index in range(jobs)]
    data = {}
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for part in pool.map(_load_ticker_chunk, [(chunk, {ticker: days_by_ticker[ticker] for ticker in chunk}) for chunk in chunks]):
            data.update(part)
    if cache:
        cache.write_bytes(pickle.dumps(data))
    return data


def priority_turnover(data: dict) -> dict:
    """(종목, 날짜) → 전 재생일까지 20일 평균 거래대금(일봉 Close×Volume, 일봉이 끝난 뒤는 1분봉 합). 그날 값은 안 본다."""
    tickers = sorted({ticker for ticker, _ in data})
    daily = pd.read_parquet(DAILY_PARQUET, columns=["Date", "code", "Close", "Volume"])
    daily = daily[daily["code"].isin(tickers)]
    series: dict[str, pd.Series] = {}
    for code, group in daily.groupby("code"):
        values = (group["Close"] * group["Volume"]).astype(float)
        values.index = group["Date"].dt.strftime("%Y%m%d")
        series[code] = values
    minute_turnover = {}
    for (ticker, day), record in data.items():
        minute_turnover.setdefault(ticker, {})[day] = record["turnover"]
    out = {}
    for ticker in tickers:
        base = series.get(ticker, pd.Series(dtype=float))
        last = base.index.max() if len(base) else "00000000"
        extra = pd.Series({day: value for day, value in minute_turnover.get(ticker, {}).items() if day > last}, dtype=float)
        combined = pd.concat([base, extra]).sort_index()
        mean20 = combined.rolling(20, min_periods=5).mean().shift(1)    # 그날 값 제외
        for day in minute_turnover.get(ticker, {}):
            value = mean20.get(day, np.nan)
            out[(ticker, day)] = 0.0 if pd.isna(value) else float(value)
    return out


# ───────────────────────── 종목-하루 상태 기계(하네스 replay_day를 봉 단위로 쪼갠 것) ─────────────────────────

class TickerDay:
    """`devscale_replay.replay_day`의 본문을 봉 하나씩 부르게 나눴다. 이 스터디 칸이 쓰는 경로만 옮겼다
    (분할 매수·트레일·진입 확인·SMA 트레일 끔). 체결 판정·존 게이트·손절·익절·마감 넘김은 같은 순서·같은 식."""

    def __init__(self, ticker: str, day: str, record: dict, carry: dict | None):
        parameters = PARAMS
        self.ticker, self.day = ticker, day
        self.arr = record["arr"]
        self.n = len(self.arr)
        moving_averages = record["ma"]
        self.al = aligned(moving_averages, parameters.align_tol_pct)
        self.average_20 = moving_averages[20]
        atr14 = record["atr14"]
        open_deviation = (float(self.arr[0, 3]) - self.average_20) / self.average_20 * 100.0
        atr_percent = atr14 / self.average_20 * 100.0 if atr14 > 0 else 0.0
        self.entry_allowed = ((parameters.entry_atr_max_percent <= 0.0 or atr_percent <= parameters.entry_atr_max_percent)
                              and parameters.entry_open_deviation_min_percent <= open_deviation <= parameters.entry_open_deviation_max_percent)
        self.position, self.average, self.cost_basis, self.held_days = 0, 0.0, 0.0, 0
        self.entry_day, self.entry_hhmm = "", 0
        if carry:
            self.position, self.average, self.cost_basis = carry["position"], carry["average"], carry["cost_basis"]
            self.held_days = carry["held_days"] + 1
            self.entry_day, self.entry_hhmm = carry["entry_day"], carry["entry_hhmm"]
        self.orders: list[tuple] = []
        self.in_zone = False
        self.cooldown_until = -1
        self.reentry_until = -1
        self.closes: list[float] = []
        self.done = False
        self.index = 0
        self.closed_trades: list[dict] = []
        self.buy_fills = 0

    def next_hhmm(self) -> int:
        return int(self.arr[self.index, 4]) if self.index < self.n else 99999

    def pending_buy(self) -> float:
        return sum(price * quantity for kind, price, quantity, _ in self.orders if kind == "BUY")

    # 체결 — 반환: 현금 변화(매수 −, 매도 +)
    def _fill_buy(self, price: float, quantity: int, hhmm: int) -> float:
        net = fill_result("BUY", price, quantity, LIVE).net
        if self.position == 0:
            self.entry_day, self.entry_hhmm = self.day, hhmm
        self.average = (self.average * self.position + price * quantity) / (self.position + quantity)
        self.position += quantity
        self.cost_basis += net
        self.buy_fills += 1
        return -net

    def _fill_sell(self, price: float, quantity: int, tag: str, hhmm: int) -> float:
        quantity = min(quantity, self.position)
        if quantity <= 0:
            return 0.0
        proceeds = fill_result("SELL", price, quantity, LIVE).net
        basis = self.cost_basis * quantity / self.position
        self.closed_trades.append({"ticker": self.ticker, "entry_day": self.entry_day, "entry_hhmm": self.entry_hhmm,
                                   "exit_day": self.day, "exit_hhmm": hhmm, "quantity": quantity,
                                   "average": round(self.average, 2), "exit_price": price, "cost_basis": round(basis, 1),
                                   "net_pnl": round(proceeds - basis, 1), "exit": tag})
        self.cost_basis -= basis
        self.position -= quantity
        if self.position == 0:
            self.average, self.cost_basis = 0.0, 0.0
            self.reentry_until = self.index + 1 + PARAMS.reentry_cooldown_bars
        return proceeds

    def fill_phase(self) -> float:
        """직전 봉에서 낸 주문을 이 봉 범위로 체결 판정. 하네스 1) 단계."""
        open_price, high_price, low_price, close_price, hhmm = (float(value) for value in self.arr[self.index])
        hhmm = int(hhmm)
        cash = 0.0
        up = close_price >= open_price
        for kind, price, quantity, tag in sorted(self.orders, key=lambda order: (order[0] != ("SELL" if up else "BUY"))):
            if kind == "MKT":
                cash += self._fill_sell(round_tick(open_price - tick_size(open_price), "BUY"), quantity, tag, hhmm)
            elif kind == "BUY":
                if open_price <= price:
                    cash += self._fill_buy(open_price, quantity, hhmm)
                elif low_price <= price:
                    cash += self._fill_buy(price, quantity, hhmm)
            elif kind == "SELL":
                if open_price >= price:
                    cash += self._fill_sell(open_price, quantity, tag, hhmm)
                elif high_price >= price:
                    cash += self._fill_sell(price, quantity, tag, hhmm)
        self.orders = []
        self.closes.append(close_price)
        return cash

    def signal_phase(self, gate) -> float:
        """하네스 2) 이후. gate(self, buy_price) → 수량(0이면 거부). 반환: 즉시 체결(마지막 봉 청산)의 현금 변화."""
        parameters = PARAMS
        bar_index = self.index
        self.index += 1
        if self.done:
            return 0.0
        open_price, high_price, low_price, close_price, hhmm = (float(value) for value in self.arr[bar_index])
        hhmm = int(hhmm)
        deviation = (close_price - self.average_20) / self.average_20 * 100.0
        widen = self.in_zone or (parameters.hold_widens_zone and self.position > 0)
        up_threshold = parameters.entry_upper_pct + (parameters.zone_hyst_pct if widen else 0.0)
        low_threshold = -(parameters.pullback_percent + (parameters.zone_hyst_pct if widen else 0.0))
        zone = self.al and (low_threshold <= deviation <= up_threshold)
        self.in_zone = zone
        last_bar = bar_index == self.n - 1
        cash = 0.0

        def liquidate(tag: str) -> float:
            if self.position > 0:
                if last_bar:
                    return self._fill_sell(close_price, self.position, tag, hhmm)
                self.orders = [("MKT", 0.0, self.position, tag)]
            return 0.0

        if hhmm >= parameters.market_close_hhmm:
            self.orders = []                    # 넘김: 미체결만 거두고 보유는 내일로
            self.done = True
            return 0.0
        if not zone:
            return liquidate("zone_exit")
        if self.position > 0 and close_price <= self.average * (1.0 - parameters.stop_loss_pct / 100.0):
            cash = liquidate("stop")
            self.cooldown_until = bar_index + parameters.stop_cooldown_bars
            return cash
        warming = len(self.closes) < parameters.sma_period
        simple_moving_average = self.average_20 if warming else float(np.mean(self.closes[-parameters.sma_period:]))
        plan = []
        if (self.position <= 0 and bar_index >= self.cooldown_until and bar_index >= self.reentry_until and self.entry_allowed
                and close_price >= parameters.entry_price_min and close_price <= parameters.entry_price_max):
            buy_price = round_tick(min(simple_moving_average, close_price), "BUY")
            if buy_price >= close_price:
                buy_price = round_tick(close_price - tick_size(close_price), "BUY")
            quantity = gate(self, buy_price)
            if quantity > 0:
                plan.append(("BUY", buy_price, quantity, "base"))
        if self.position > 0:
            sell_price = round_tick(self.average * (1.0 + parameters.dev_sell_pct / 100.0), "SELL")
            if sell_price <= close_price:
                sell_price = round_tick(close_price, "SELL")
            plan.append(("SELL", sell_price, self.position, "tp"))
        self.orders = plan
        return cash

    def end_of_day(self) -> tuple[dict | None, float]:
        """반환: (넘길 보유, 자료 절단 청산의 현금 변화)."""
        if self.position > 0 and self.done:
            return ({"position": self.position, "average": self.average, "cost_basis": self.cost_basis,
                     "held_days": self.held_days, "last_close": self.closes[-1] if self.closes else float(self.arr[-1, 3]),
                     "entry_day": self.entry_day, "entry_hhmm": self.entry_hhmm}, 0.0)
        if self.position > 0:
            last = self.closes[-1] if self.closes else float(self.arr[-1, 3])
            return None, self._fill_sell(last, self.position, "trunc", 1530)
        return None, 0.0


# ───────────────────────── 포트폴리오 층 ─────────────────────────

def close_carry_trade(ticker: str, carry: dict, day: str) -> tuple[dict, float]:
    price, quantity = carry["last_close"], carry["position"]
    proceeds = fill_result("SELL", price, quantity, LIVE).net
    return ({"ticker": ticker, "entry_day": carry["entry_day"], "entry_hhmm": carry["entry_hhmm"], "exit_day": day,
             "exit_hhmm": 0, "quantity": quantity, "average": round(carry["average"], 2), "exit_price": price,
             "cost_basis": round(carry["cost_basis"], 1), "net_pnl": round(proceeds - carry["cost_basis"], 1),
             "exit": "carry_end"}, proceeds)


def simulate(data: dict, slots: int, budget: float, priority: str, seed: int, turnover_key: dict) -> dict:
    """slots=0이면 상한·현금 제한 없음. 반환: 일별 평가·거래·놓친 신호."""
    unlimited = slots <= 0
    days = sorted({day for _, day in data})
    by_day: dict[str, list[str]] = {}
    for ticker, day in data:
        by_day.setdefault(day, []).append(ticker)
    # 종목 → 재생일 목록(다음 재생일의 연속 여부로 넘긴 보유를 이어갈지 정한다 — 하네스 flush_carry와 같은 손익)
    ticker_days: dict[str, list[str]] = {}
    for ticker, day in sorted(data):
        ticker_days.setdefault(ticker, []).append(day)
    random_engine = np.random.default_rng(seed)

    cash = CAPITAL
    carries: dict[str, dict] = {}
    trades: list[dict] = []
    daily_rows: list[dict] = []
    missed_events = 0
    missed_days: set[tuple[str, str]] = set()
    cash_short = 0
    deployed_sum, step_count = 0.0, 0

    for day in days:
        today = set(by_day[day])
        # 1) 오늘 재생하지 않는 넘긴 보유 — 다음 재생일이 연속(거래정지 등)이면 들고 가고, 아니면 마지막 종가로 정리
        for ticker in list(carries):
            if ticker in today:
                if not data[(ticker, day)]["contiguous"]:
                    trade, proceeds = close_carry_trade(ticker, carries.pop(ticker), day)
                    trades.append(trade)
                    cash += proceeds
                continue
            later = [later_day for later_day in ticker_days[ticker] if later_day > day]
            if not later or not data[(ticker, later[0])]["contiguous"]:
                trade, proceeds = close_carry_trade(ticker, carries.pop(ticker), day)
                trades.append(trade)
                cash += proceeds
        # 2) 오늘 움직일 수 있는 종목만 — 보유가 있거나 정배열·진입 필터를 통과한 날
        states: list[TickerDay] = []
        for ticker in sorted(today):
            carry = carries.pop(ticker, None)
            state = TickerDay(ticker, day, data[(ticker, day)], carry)
            if carry or (state.al and state.entry_allowed):
                states.append(state)
        if priority == "turnover":
            key = {ticker_state.ticker: -turnover_key.get((ticker_state.ticker, day), 0.0) for ticker_state in states}
        else:
            draws = random_engine.random(len(states))
            key = {ticker_state.ticker: float(draws[index]) for index, ticker_state in enumerate(states)}
        ordered = sorted(states, key=lambda ticker_state: (key[ticker_state.ticker], ticker_state.ticker))
        held_carried = {name for name in carries}       # 오늘 재생 안 하고 들고 가는 보유(거래정지)도 슬롯을 먹는다

        def used_slots(exclude: TickerDay) -> int:
            used = len(held_carried)
            for ticker_state in states:
                if ticker_state is exclude:
                    continue
                if ticker_state.position > 0 or ticker_state.pending_buy() > 0:
                    used += 1
            return used

        def reserved_cash(exclude: TickerDay) -> float:
            return sum(ticker_state.pending_buy() * (1 + LIVE.buy_cost_rate) for ticker_state in states if ticker_state is not exclude)

        def gate(state: TickerDay, buy_price: float) -> int:
            nonlocal missed_events, cash_short
            if unlimited:
                return int(budget // buy_price)
            if used_slots(state) >= slots:
                missed_events += 1
                missed_days.add((state.ticker, day))
                return 0
            available = cash - reserved_cash(state)
            spend = min(budget, available / (1 + LIVE.buy_cost_rate))
            if spend < budget:
                cash_short += 1
            return max(int(spend // buy_price), 0)

        # 3) 3분봉 시각을 맞춰 한 걸음씩: 먼저 모든 종목 체결, 그다음 우선순위 순으로 신호
        while True:
            step = min((ticker_state.next_hhmm() for ticker_state in states), default=99999)
            if step == 99999:
                break
            moving = [ticker_state for ticker_state in ordered if ticker_state.next_hhmm() == step]
            for ticker_state in moving:
                cash += ticker_state.fill_phase()
            for ticker_state in moving:
                cash += ticker_state.signal_phase(gate)
            deployed_sum += sum(ticker_state.cost_basis for ticker_state in states if ticker_state.position > 0) + sum(carry_record["cost_basis"] for carry_record in carries.values())
            step_count += 1
        # 4) 마감 — 넘김·평가
        market_value = 0.0
        for ticker_state in states:
            carry, proceeds = ticker_state.end_of_day()
            trades.extend(ticker_state.closed_trades)
            cash += proceeds
            if carry:
                carries[ticker_state.ticker] = carry
                market_value += carry["position"] * carry["last_close"]
        for name in held_carried:
            market_value += carries[name]["position"] * carries[name]["last_close"]
        entered_missed = {(ticker_state.ticker, day) for ticker_state in states if ticker_state.buy_fills > 0}
        daily_rows.append({"day": day, "cash": cash, "market_value": market_value, "equity": cash + market_value,
                           "held": len(carries), "candidates": len(states),
                           "missed_never_entered": len({missed_key for missed_key in missed_days if missed_key[1] == day} - entered_missed)})
    for ticker in list(carries):
        trade, proceeds = close_carry_trade(ticker, carries.pop(ticker), days[-1])
        trades.append(trade)
        cash += proceeds
    if daily_rows:
        daily_rows[-1]["equity"] = cash
    return {"daily": pd.DataFrame(daily_rows), "trades": pd.DataFrame(trades), "missed_events": missed_events,
            "missed_days": len(missed_days), "cash_short": cash_short,
            "deployment": deployed_sum / step_count / CAPITAL if step_count else 0.0}


# ───────────────────────── 집계 ─────────────────────────

def _drawdown(equity: pd.Series, start: float) -> float:
    """시작값을 첫 고점으로 둔 최대낙폭(%)."""
    values = np.concatenate([[start], equity.to_numpy()])
    peak = np.maximum.accumulate(values)
    return float(((values - peak) / peak).min() * 100.0)


def metrics(result: dict, days: list[str]) -> dict:
    daily, trades = result["daily"], result["trades"]
    equity = daily["equity"]
    split_index = int(len(days) * SPLIT)
    boundary = days[split_index - 1]
    front_equity, back_equity = equity.iloc[:split_index], equity.iloc[split_index:]
    out = {"net_man": (equity.iloc[-1] - CAPITAL) / 1e4, "return_pct": (equity.iloc[-1] / CAPITAL - 1) * 100.0,
           "mdd_pct": _drawdown(equity, CAPITAL), "trades": len(trades),
           "per_trade_pct": float(trades["net_pnl"].sum() / trades["cost_basis"].sum() * 100.0) if len(trades) else 0.0,
           "win_rate": float((trades["net_pnl"] > 0).mean()) if len(trades) else 0.0,
           "missed_ticker_days": result["missed_days"], "missed_never_entered": int(daily["missed_never_entered"].sum()),
           "missed_events": result["missed_events"], "cash_short": result["cash_short"],
           "deployment_pct": result["deployment"] * 100.0, "held_max": int(daily["held"].max()),
           "top5_ticker_share": float(trades.groupby("ticker")["net_pnl"].sum().nlargest(5).sum() / trades["net_pnl"].sum())
           if len(trades) and trades["net_pnl"].sum() > 0 else float("nan"),
           "front_net_man": (front_equity.iloc[-1] - CAPITAL) / 1e4, "front_mdd_pct": _drawdown(front_equity, CAPITAL),
           "back_net_man": (equity.iloc[-1] - front_equity.iloc[-1]) / 1e4,
           "back_mdd_pct": _drawdown(back_equity, float(front_equity.iloc[-1])),
           "front_trades": int((trades["entry_day"] <= boundary).sum()) if len(trades) else 0,
           "back_trades": int((trades["entry_day"] > boundary).sum()) if len(trades) else 0}
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pairs", default=str(_HERE / "pairs_100.json"))
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--seeds", type=int, default=30, help="무작위 우선순위 seed 수")
    parser.add_argument("--cache", default=None, help="적재 결과 pickle(재실행 가속). 비우면 매번 읽는다")
    parser.add_argument("--out-dir", default=str(_HERE))
    arguments = parser.parse_args()
    out_directory = Path(arguments.out_dir)
    pairs_path = Path(arguments.pairs)
    pairs = load_pairs(pairs_path)
    data = load_all(pairs, arguments.jobs, Path(arguments.cache) if arguments.cache else None)
    days = sorted({day for _, day in data})
    turnover_key = priority_turnover(data)
    print(f"[load] pairs {len(pairs)} · 재생 가능 {len(data)}(종목,일) · 거래일 {len(days)} "
          f"({days[0]}~{days[-1]}) · 앞 70% 끝 {days[int(len(days) * SPLIT) - 1]}", flush=True)

    runs, trade_frames, daily_frames = [], [], []
    for slots in (0, 2, 3, 4, 5, 6):
        budget = 500_000.0 if slots == 0 else CAPITAL / slots
        plans = [("turnover", 0)] + ([("random", seed) for seed in range(arguments.seeds)] if slots > 0 else [])
        for priority, seed in plans:
            result = simulate(data, slots, budget, priority, seed, turnover_key)
            row = {"slots": slots, "budget": budget, "priority": priority, "seed": seed, **metrics(result, days)}
            runs.append(row)
            if priority == "turnover":
                trade_frames.append(result["trades"].assign(slots=slots))
                daily_frames.append(result["daily"].assign(slots=slots))
                print(f"[N={slots or '무제한'}] 예산 {budget/1e4:.1f}만 · 거래 {row['trades']} · 순손익 {row['net_man']:.1f}만 · "
                      f"MDD {row['mdd_pct']:.1f}% · 놓친 종목일 {row['missed_ticker_days']} · 투입률 {row['deployment_pct']:.0f}%", flush=True)
    run_table = pd.DataFrame(runs)
    run_table.to_csv(out_directory / "slot_cap_runs.tsv", sep="\t", index=False, float_format="%.4f")
    pd.concat(trade_frames).to_csv(out_directory / "slot_cap_trades.tsv", sep="\t", index=False)
    pd.concat(daily_frames).to_csv(out_directory / "slot_cap_daily.tsv", sep="\t", index=False, float_format="%.0f")

    summary_rows = []
    for slots, group in run_table.groupby("slots"):
        turnover_row = group[group["priority"] == "turnover"].iloc[0]
        randoms = group[group["priority"] == "random"]
        row = {"slots": slots, "budget_man": turnover_row["budget"] / 1e4}
        for column in ("net_man", "mdd_pct", "trades", "per_trade_pct", "missed_ticker_days", "missed_never_entered",
                       "deployment_pct", "top5_ticker_share", "front_net_man", "back_net_man", "front_mdd_pct", "back_mdd_pct", "front_trades", "back_trades"):
            row[f"{column}_turnover"] = turnover_row[column]
            if len(randoms):
                row[f"{column}_rand_median"] = randoms[column].median()
                row[f"{column}_rand_p10"] = randoms[column].quantile(0.10)
                row[f"{column}_rand_p90"] = randoms[column].quantile(0.90)
        summary_rows.append(row)
    summary = pd.DataFrame(summary_rows)
    # N=4 대비 월 블록 부트스트랩 — 우선순위 turnover 일별 손익 차이를 달 단위로 복원추출(5,000회, seed 0)
    daily_all = pd.concat(daily_frames)
    pnl = daily_all.pivot(index="day", columns="slots", values="equity").diff()
    pnl.iloc[0] = daily_all.pivot(index="day", columns="slots", values="equity").iloc[0] - CAPITAL
    months = pnl.index.str[:6]
    monthly = pnl.groupby(months).sum()
    random_engine = np.random.default_rng(0)
    draws = random_engine.integers(0, len(monthly), size=(5000, len(monthly)))
    for slots in summary["slots"]:
        difference = (monthly[slots] - monthly[4]).to_numpy()
        sums = difference[draws].sum(axis=1)
        summary.loc[summary["slots"] == slots, "vs4_month_boot_p_gt0"] = float((sums > 0).mean()) if slots != 4 else np.nan
        summary.loc[summary["slots"] == slots, "months_positive"] = int((monthly[slots] > 0).sum())
    summary["months"] = len(monthly)
    summary.to_csv(out_directory / "slot_cap_summary.tsv", sep="\t", index=False, float_format="%.3f")
    (out_directory / "metrics.json").write_text(json.dumps({
        "study": "31_devscale_slot_cap", "params_variant": "devscale_replay.ZONE_WIDTH_VARIANTS[z_lo8_up5_tp5]",
        "capital": CAPITAL, "split": SPLIT, "days": [days[0], days[-1]], "n_days": len(days),
        "front_end": days[int(len(days) * SPLIT) - 1], "seeds": arguments.seeds,
        "pairs_sha1": hashlib.sha1(pairs_path.read_bytes()).hexdigest(),
        "summary": summary.round(4).to_dict(orient="records")}, ensure_ascii=False, indent=1), encoding="utf-8")
    pd.set_option("display.width", 250)
    print(summary[[column for column in summary.columns if column.endswith("_turnover") or column.endswith("rand_median") or column in ("slots", "budget_man")]]
          .round(2).T.to_string())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
