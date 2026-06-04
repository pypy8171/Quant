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
class CostModel:
    """거래 비용 모델 (한국 현물)"""
    commission_rate: float = 0.00015  # 수수료 0.015% (증권사별 상이)
    tax_rate:        float = 0.0018   # 거래세 0.18% (매도 시만)
    slippage_bps:    float = 5.0      # 슬리피지 5bp

    def buy_total_cost(self, price: float, qty: int) -> float:
        """매수 총비용 (체결금액 + 수수료 + 슬리피지)"""
        notional = price * qty
        return notional * (1 + self.commission_rate + self.slippage_bps / 10_000)

    def sell_net_proceeds(self, price: float, qty: int) -> float:
        """매도 실수령액 (체결금액 - 수수료 - 거래세 - 슬리피지)"""
        notional = price * qty
        return notional * (1 - self.commission_rate - self.tax_rate - self.slippage_bps / 10_000)


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
                 initial_cash: float = 10_000_000,
                 cost_model: CostModel | None = None):
        self.kis        = kis
        self.strategy   = strategy
        self.init_cash  = initial_cash
        self.cash       = initial_cash
        self.cost       = cost_model or CostModel()
        self._trades:   list[Trade] = []
        self._equity:   list[float] = []      # 날짜별 포트폴리오 평가금액 (현금 + 보유 포지션 시가)
        self._positions: dict[str, int] = {}  # ticker → 현재 보유 수량

    def run(self, universe: list[str], start_date: str, end_date: str) -> BacktestResult:
        from datetime import date as _date, timedelta
        print(f"\n{'='*60}")
        print(f"백테스팅: {self.strategy.id()}")
        print(f"기간: {start_date} ~ {end_date}  |  종목: {len(universe)}개")
        print(f"초기 자금: {self.init_cash:,.0f}원")
        print('='*60)

        # 스크리닝용 여분 데이터: start_date -14일
        pre_start = (_date.fromisoformat(start_date) - timedelta(days=14)).isoformat()

        # 1. 전 종목 일봉 데이터 수집 (날짜 범위 기반)
        raw_bars:  dict[str, list[Bar]] = {}   # pre_start ~ end_date
        all_bars:  dict[str, list[Bar]] = {}   # start_date ~ end_date (시뮬레이션용)
        for i, ticker in enumerate(universe):
            print(f"\r  데이터 수집 중... {i+1}/{len(universe)} ({ticker})", end="", flush=True)
            bars = self.kis.get_historical_ohlcv(ticker, pre_start, end_date)
            sim  = [b for b in bars if b.date >= start_date]
            if sim:
                raw_bars[ticker] = bars
                all_bars[ticker] = sim
            time.sleep(0.3)
        print(f"\r  데이터 수집 완료: {len(all_bars)}종목{' '*20}")

        # 2. 역사적 스크리닝: start_date 직전 4봉 기준 3일 연속 하락
        self.strategy.set_kis(self.kis)
        candidates = []
        for ticker, bars in raw_bars.items():
            pre = [b for b in bars if b.date < start_date and b.volume > 0][-4:]
            if len(pre) >= 4 and (
                pre[-1].close < pre[-2].close
                and pre[-2].close < pre[-3].close
                and pre[-3].close < pre[-4].close
            ):
                candidates.append(ticker)
        self.strategy.set_candidates(candidates)
        print(f"  스크리닝 완료: {len(candidates)}종목 후보 → {[c for c in candidates]}")

        watch = list(all_bars.keys())

        # 3. 날짜 순으로 시뮬레이션
        all_dates = sorted({b.date for bars in all_bars.values() for b in bars})

        for date in all_dates:
            for ticker in watch:
                bars = all_bars.get(ticker, [])
                # 해당 날짜까지의 봉만 전달 (미래 데이터 차단)
                visible = [b for b in bars if b.date <= date]
                if not visible:
                    continue

                signal = self.strategy.on_data(ticker, visible)
                if signal is None:
                    continue

                # 체결 가정: 시그널 발생 봉의 다음 봉 시가 (look-ahead bias 방지)
                future = [b for b in bars if b.date > date]
                if not future:
                    continue  # 마지막 봉에선 체결 불가
                price = future[0].open if future[0].open > 0 else future[0].close

                if signal.side == "BUY":
                    total_cost = self.cost.buy_total_cost(price, signal.quantity)
                    if self.cash >= total_cost:
                        self.cash -= total_cost
                        self._positions[ticker] = self._positions.get(ticker, 0) + signal.quantity
                        self._trades.append(Trade(
                            ticker   = ticker,
                            side     = "BUY",
                            date     = date,
                            price    = price,
                            quantity = signal.quantity,
                        ))
                elif signal.side == "SELL":
                    held = self._positions.get(ticker, 0)
                    sell_qty = min(signal.quantity, held)  # 보유 수량 초과 매도 방지
                    if sell_qty > 0:
                        buy = next((t for t in reversed(self._trades)
                                    if t.ticker == ticker and t.side == "BUY"), None)
                        entry_price = buy.price if buy else price
                        proceeds = self.cost.sell_net_proceeds(price, sell_qty)
                        pnl = proceeds - entry_price * sell_qty
                        self.cash += proceeds
                        self._positions[ticker] = held - sell_qty
                        if self._positions[ticker] == 0:
                            del self._positions[ticker]
                        self._trades.append(Trade(
                            ticker   = ticker,
                            side     = "SELL",
                            date     = date,
                            price    = price,
                            quantity = sell_qty,
                            pnl      = pnl,
                        ))

            # 일별 포트폴리오 평가: 현금 + 보유 포지션 당일 종가 기준
            port_value = self.cash
            for t, qty in self._positions.items():
                t_bars = all_bars.get(t, [])
                day_bar = next((b for b in t_bars if b.date == date), None)
                if day_bar:
                    port_value += qty * day_bar.close
            self._equity.append(port_value)

        self.strategy.on_stop()
        return self._calc_result()

    def _calc_result(self) -> BacktestResult:
        sells    = [t for t in self._trades if t.side == "SELL"]
        total_pnl = sum(t.pnl for t in sells)
        wins      = [t for t in sells if t.pnl > 0]
        win_rate  = len(wins) / len(sells) * 100 if sells else 0.0

        # 미청산 보유 포지션 평가액을 포함한 최종 자산(equity)으로 수익률 계산.
        # self.cash만 쓰면 미청산분을 0으로 친 셈이라 장기 보유형 전략 수익률이 왜곡됨.
        final_equity  = self._equity[-1] if self._equity else self.cash
        total_return  = (final_equity - self.init_cash) / self.init_cash * 100

        # MDD 계산 — 일별 포트폴리오 평가금액(equity) 시계열 기반
        mdd = 0.0
        if self._equity:
            peak = self._equity[0]
            for e in self._equity:
                peak = max(peak, e)
                dd   = (peak - e) / peak * 100 if peak > 0 else 0
                mdd  = max(mdd, dd)

        # 샤프지수 — 일별 equity 수익률 기반 연환산 (252 거래일)
        sharpe = 0.0
        if len(self._equity) > 1:
            import statistics
            daily_rets = [
                (self._equity[i] - self._equity[i - 1]) / self._equity[i - 1]
                for i in range(1, len(self._equity))
                if self._equity[i - 1] > 0
            ]
            if len(daily_rets) > 1:
                avg = statistics.mean(daily_rets)
                std = statistics.stdev(daily_rets)
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
