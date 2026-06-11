"""
백테스팅 엔진
과거 일봉 데이터로 전략을 시뮬레이션
"""
from dataclasses import dataclass, field
from typing import Optional
from kis.client import Bar, KisClient, OrderSignal
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


class _AsOfKisAdapter:
    """백테스트 on_start 전용 KIS 어댑터 — start_date 이전 데이터만 노출(look-ahead 차단).
    엔진이 이미 수집한 raw_bars를 재사용하고, 시계열 외 메서드는 실제 kis로 위임한다."""
    def __init__(self, real_kis, raw_bars: dict, as_of: str):
        self._kis = real_kis
        self._raw = raw_bars      # ticker → list[Bar] (pre_start ~ end_date)
        self._as_of = as_of       # start_date — 이 날짜 미만만 노출

    def get_daily_ohlcv(self, ticker: str, count: int = 30):
        bars = [b for b in self._raw.get(ticker, [])
                if b.date < self._as_of and b.volume > 0]
        return bars[-count:]

    def get_historical_ohlcv(self, ticker: str, start: str, end: str):
        cap = min(end, self._as_of)   # as-of 상한 (start_date 미만)
        return [b for b in self._raw.get(ticker, []) if start <= b.date < cap]

    def __getattr__(self, name):
        return getattr(self._kis, name)  # 인증 등 시계열 외 메서드 위임


class BacktestEngine:
    def __init__(self, kis: KisClient, strategy: StrategyBase,
                 initial_cash: float = 10_000_000,
                 cost_model: CostModel | None = None,
                 target_positions: int = 10,
                 warmup_days: int = 14):
        self.kis        = kis
        self.strategy   = strategy
        self.init_cash  = initial_cash
        self.cash       = initial_cash
        self.cost       = cost_model or CostModel()
        self.target_positions = target_positions   # TARGET_WEIGHT 동일가중 분모
        self.warmup_days = warmup_days             # start_date 이전 워밍업 일수(모멘텀 lookback)
        self._trades:   list[Trade] = []
        self._equity:   list[float] = []      # 날짜별 포트폴리오 평가금액 (현금 + 보유 포지션 시가)
        self._positions: dict[str, int] = {}  # ticker → 현재 보유 수량
        self._names:    dict[str, str] = {}   # ticker → 종목명 (소스가 제공 시)

    def _label(self, code: str) -> str:
        nm = self._names.get(code)
        return f"{code}({nm})" if nm else code

    def run(self, universe: list[str], start_date: str, end_date: str) -> BacktestResult:
        from datetime import date as _date, timedelta
        print(f"\n{'='*60}")
        print(f"백테스팅: {self.strategy.id()}")
        print(f"기간: {start_date} ~ {end_date}  |  종목: {len(universe)}개")
        print(f"초기 자금: {self.init_cash:,.0f}원")
        print('='*60)

        # 워밍업(스크리닝/모멘텀 lookback)용 여분 과거 데이터
        pre_start = (_date.fromisoformat(start_date) - timedelta(days=self.warmup_days)).isoformat()

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

        # 종목명 prefetch (소스가 제공할 때) — 로그/리포트 가독성
        if hasattr(self.kis, "ticker_name"):
            for t in all_bars:
                self._names[t] = self.kis.ticker_name(t)

        # 1b. 수급 데이터 — 전략이 수급을 쓰고(uses_flow) 소스가 제공할 때만 수집(불필요 호출/노이즈 방지)
        all_flow: dict[str, list] = {}
        if getattr(self.strategy, "uses_flow", False) and hasattr(self.kis, "flow_history"):
            for ticker in list(all_bars.keys()):
                all_flow[ticker] = self.kis.flow_history(ticker, pre_start, end_date)
            print(f"  수급 수집 완료: {sum(1 for v in all_flow.values() if v)}종목")

        # 2. 전략 자체 스크리닝 — look-ahead 차단 as-of 어댑터로 on_start 호출.
        #    엔진은 전략 로직을 모른다(전략-불가지). 빈 리스트 반환 시 전 종목 감시.
        asof = _AsOfKisAdapter(self.kis, raw_bars, start_date)
        self.strategy.set_kis(asof)
        watch = self.strategy.on_start(universe) or []
        watch = [t for t in watch if t in all_bars]
        if not watch:
            watch = list(all_bars.keys())
        print(f"  감시 종목: {len(watch)}개")

        # 3. 날짜 순으로 시뮬레이션
        all_dates = sorted({b.date for bars in all_bars.values() for b in bars})

        for date in all_dates:
            # 해당 날짜까지 보이는 봉 — raw_bars(워밍업 포함 전체)에서, 미래 차단. watch 순서 보존.
            visible_all: dict[str, list[Bar]] = {}
            for ticker in watch:
                vis = [b for b in raw_bars.get(ticker, []) if b.date <= date]
                if vis:
                    visible_all[ticker] = vis

            # 수급도 date 미만으로 잘라 전달 (T-1 확정만 — look-ahead·발표시차 차단)
            flow_visible = {t: [f for f in all_flow.get(t, []) if f.date < date]
                            for t in visible_all}

            # 횡단면 리밸런싱: 목표 집합 반환 시 엔진이 실보유 기준 reconcile(청산+매수+사이징)
            target = self.strategy.on_rebalance(date, visible_all, flow_visible)
            if target is not None:
                self._rebalance_to_target(set(target), date, raw_bars)

            # per-ticker 신호(개별 MARKET 주문) — 다음봉 시가 체결
            for ticker, vis in visible_all.items():
                sig = self.strategy.on_data(ticker, vis)
                if sig is not None:
                    self._execute(sig, date, raw_bars)

            # 일별 포트폴리오 평가: 현금 + 보유 포지션 당일 종가 기준
            port_value = self.cash
            for t, qty in self._positions.items():
                day_bar = next((b for b in raw_bars.get(t, []) if b.date == date), None)
                if day_bar:
                    port_value += qty * day_bar.close
            self._equity.append(port_value)

        self.strategy.on_stop()
        return self._calc_result()

    def _execute(self, sig, date: str, all_bars: dict):
        """신호를 다음 봉 시가로 체결(look-ahead 방지). cash/positions/trades 갱신.
        order_type=="TARGET_WEIGHT"면 엔진이 동일가중 사이징: BUY=floor(직전equity/N/price), SELL=전량."""
        bars = all_bars.get(sig.ticker, [])
        future = [b for b in bars if b.date > date]
        if not future:
            return  # 다음 봉 없음 → 체결 불가(마지막 봉)
        price = future[0].open if future[0].open > 0 else future[0].close
        if price <= 0:
            return

        qty = sig.quantity   # 횡단면 동일가중 사이징은 _rebalance_to_target가 끝내고 MARKET으로 전달
        if qty <= 0:
            return

        if sig.side == "BUY":
            total_cost = self.cost.buy_total_cost(price, qty)
            if self.cash >= total_cost:
                self.cash -= total_cost
                self._positions[sig.ticker] = self._positions.get(sig.ticker, 0) + qty
                self._trades.append(Trade(sig.ticker, "BUY", date, price, qty))
        elif sig.side == "SELL":
            held = self._positions.get(sig.ticker, 0)
            sell_qty = min(qty, held)  # 보유 수량 초과 매도 방지
            if sell_qty > 0:
                buy = next((t for t in reversed(self._trades)
                            if t.ticker == sig.ticker and t.side == "BUY"), None)
                entry_price = buy.price if buy else price
                proceeds = self.cost.sell_net_proceeds(price, sell_qty)
                pnl = proceeds - entry_price * sell_qty
                self.cash += proceeds
                self._positions[sig.ticker] = held - sell_qty
                if self._positions[sig.ticker] == 0:
                    del self._positions[sig.ticker]
                self._trades.append(Trade(sig.ticker, "SELL", date, price, sell_qty, pnl))

    def _peek_next_open(self, ticker: str, date: str, all_bars: dict):
        """다음 봉 시가(체결가) 미리보기 — 사이징용. 없으면 None."""
        fut = [b for b in all_bars.get(ticker, []) if b.date > date]
        if not fut:
            return None
        return fut[0].open if fut[0].open > 0 else fut[0].close

    def _rebalance_to_target(self, target: set, date: str, all_bars: dict):
        """목표 동일가중 포트폴리오로 재조정 — 실보유(_positions) 기준 diff.
        이탈 청산(현금 확보 먼저) → 신규 매수. 사이징은 비용 반영 + 가용현금 cap이라
        목표 종목이 마지막까지 체결된다(C-1 디싱크/C-2 미체결 해소). 전략은 cash·보유를 몰라도 됨."""
        before = set(self._positions.keys())
        new_in = sorted(target - before)     # 신규 편입(매수 예정)
        gone   = sorted(before - target)     # 이탈(청산 예정)

        # 1. 목표에 없는 보유 종목 전량 청산 (현금 확보)
        for t in list(self._positions.keys()):
            if t not in target:
                self._execute(OrderSignal(t, "SELL", self._positions[t], "MARKET"), date, all_bars)

        # 2. 목표 중 미보유분 동일가중 매수 (선택 종목끼리 풀투자 동일가중, 비용+가용현금 반영).
        #    분모를 len(target)으로 — target<N일 때 (N-target)/N 자본이 유휴로 남는 문제 해소(W-1).
        equity   = self._equity[-1] if self._equity else self.init_cash
        slot_val = equity / max(1, len(target))
        cost_rate = self.cost.commission_rate + self.cost.slippage_bps / 10_000
        for t in sorted(target):   # 정렬 — set 순회순서 randomization 제거(백테스트 재현성)
            if t in self._positions:
                continue   # 이미 보유(목표 유지) — v1은 top-up 안 함(턴오버 절감)
            price = self._peek_next_open(t, date, all_bars)
            if price is None or price <= 0:
                continue
            budget = min(slot_val, self.cash)             # 가용현금 cap → 항상 체결 가능
            qty = int(budget / (price * (1 + cost_rate)))
            if qty > 0:
                self._execute(OrderSignal(t, "BUY", qty, "MARKET"), date, all_bars)

        if new_in or gone:
            print(f"  [{date}] 리밸런싱: 신규 {[self._label(t) for t in new_in]} / "
                  f"청산 {[self._label(t) for t in gone]} → 보유 {len(self._positions)}종목")

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
