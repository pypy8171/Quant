"""
백테스팅 엔진
과거 일봉 데이터로 전략을 시뮬레이션
"""
from dataclasses import dataclass, field
from typing import Optional
from kis.client import Bar, KisClient
from strategy.base import StrategyBase
import time


@dataclass
class Trade:
    ticker:     str
    side:       str       # "BUY" | "SELL"
    date:       str
    price:      float
    quantity:   int
    pnl:        float = 0.0   # SELL 시 확정 손익


@dataclass
class BacktestResult:
    trades:        list[Trade]
    total_return:  float   # 총 수익률(%)
    mdd:           float   # 최대낙폭(%)
    sharpe:        float   # 샤프지수
    win_rate:      float   # 승률(%)
    total_pnl:     float   # 총 손익(원)
    trade_count:   int


class BacktestEngine:
    def __init__(self, kis: KisClient, strategy: StrategyBase,
                 initial_cash: float = 10_000_000):
        self.kis        = kis
        self.strategy   = strategy
        self.init_cash  = initial_cash
        self.cash       = initial_cash
        self._trades:   list[Trade] = []
        self._equity:   list[float] = []   # 날짜별 평가금액

    def run(self, universe: list[str], start_date: str, end_date: str) -> BacktestResult:
        print(f"\n{'='*60}")
        print(f"백테스팅: {self.strategy.id()}")
        print(f"기간: {start_date} ~ {end_date}  |  종목: {len(universe)}개")
        print(f"초기 자금: {self.init_cash:,.0f}원")
        print('='*60)

        # 1. 전 종목 일봉 데이터 수집
        all_bars: dict[str, list[Bar]] = {}
        for i, ticker in enumerate(universe):
            print(f"\r  데이터 수집 중... {i+1}/{len(universe)} ({ticker})", end="")
            bars = self.kis.get_daily_ohlcv(ticker, 300)
            # 기간 필터
            bars = [b for b in bars if start_date <= b.date <= end_date]
            if bars:
                all_bars[ticker] = bars
            time.sleep(0.2)
        print(f"\r  데이터 수집 완료: {len(all_bars)}종목{' '*20}")

        # 2. 스크리닝 (on_start)
        self.strategy.set_kis(self.kis)
        watch = self.strategy.on_start(list(all_bars.keys()))

        # 3. 날짜 순으로 시뮬레이션
        all_dates = sorted({b.date for bars in all_bars.values() for b in bars})

        for date in all_dates:
            equity = self.cash
            for ticker in watch:
                bars = all_bars.get(ticker, [])
                # 해당 날짜까지의 봉만 전달 (미래 데이터 차단)
                visible = [b for b in bars if b.date <= date]
                if not visible:
                    continue

                signal = self.strategy.on_data(ticker, visible)
                if signal is None:
                    continue

                price = visible[-1].close
                if signal.side == "BUY":
                    cost = price * signal.quantity
                    if self.cash >= cost:
                        self.cash -= cost
                        self._trades.append(Trade(
                            ticker   = ticker,
                            side     = "BUY",
                            date     = date,
                            price    = price,
                            quantity = signal.quantity,
                        ))
                elif signal.side == "SELL":
                    # 진입가 찾기
                    buy = next((t for t in reversed(self._trades)
                                if t.ticker == ticker and t.side == "BUY"), None)
                    entry_price = buy.price if buy else price
                    pnl = (price - entry_price) * signal.quantity
                    self.cash += price * signal.quantity
                    self._trades.append(Trade(
                        ticker   = ticker,
                        side     = "SELL",
                        date     = date,
                        price    = price,
                        quantity = signal.quantity,
                        pnl      = pnl,
                    ))

                # 포지션 평가금액
                if self.strategy.has_position(ticker):
                    equity += price * signal.quantity

            self._equity.append(self.cash + equity - self.cash)

        self.strategy.on_stop()
        return self._calc_result()

    def _calc_result(self) -> BacktestResult:
        sells    = [t for t in self._trades if t.side == "SELL"]
        total_pnl = sum(t.pnl for t in sells)
        wins      = [t for t in sells if t.pnl > 0]
        win_rate  = len(wins) / len(sells) * 100 if sells else 0.0

        final_cash    = self.cash
        total_return  = (final_cash - self.init_cash) / self.init_cash * 100

        # MDD 계산
        mdd = 0.0
        peak = self.init_cash
        running = self.init_cash
        for t in self._trades:
            if t.side == "BUY":
                running -= t.price * t.quantity
            else:
                running += t.price * t.quantity
            peak = max(peak, running)
            dd   = (peak - running) / peak * 100 if peak > 0 else 0
            mdd  = max(mdd, dd)

        # 샤프지수 (일별 수익률 기준, 간이 계산)
        trade_returns = [t.pnl / (t.price * t.quantity) for t in sells
                         if t.price * t.quantity > 0]
        sharpe = 0.0
        if len(trade_returns) > 1:
            import statistics
            avg = statistics.mean(trade_returns)
            std = statistics.stdev(trade_returns)
            sharpe = (avg / std * (252 ** 0.5)) if std > 0 else 0.0

        return BacktestResult(
            trades       = self._trades,
            total_return = total_return,
            mdd          = mdd,
            sharpe       = sharpe,
            win_rate     = win_rate,
            total_pnl    = total_pnl,
            trade_count  = len(sells),
        )
