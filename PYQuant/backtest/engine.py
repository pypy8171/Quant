"""
백테스팅 엔진
과거 일봉 데이터로 전략을 시뮬레이션
"""
from bisect import bisect_right
from dataclasses import dataclass, field
from typing import Optional
from kis.client import Bar, KisClient, OrderSignal
from strategy.base import StrategyBase
from backtest.costs import CostSpec, LIVE, fill_result
from backtest.ledger import PositionLedger
import time


def CostModel(commission_rate: float = LIVE.commission_rate, tax_rate: float = LIVE.sell_tax_rate,
              slippage_bps: float = 0.0) -> CostSpec:
    """옛 호출부(11_signal_axes 등) 호환 — 비율 인자를 `backtest.costs.CostSpec`으로 옮긴다.
    옛 slippage_bps(체결금액 대비 양쪽 비용)는 impact_pct로 간다. 새 코드는 `costs.LIVE`를 직접 쓴다."""
    return CostSpec(commission_percent=commission_rate * 100.0, sell_tax_percent=tax_rate * 100.0,
                    slippage_ticks=0, impact_percent=slippage_bps / 100.0)


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
    sharpe:        float   # 샤프지수(위험조정수익)
    win_rate:      float   # 승률(%)
    total_pnl:     float   # 총 손익(원)
    trade_count:   int
    # ── 기간 ──
    start_date:    str = ""    # 첫 평가일
    end_date:      str = ""    # 마지막 평가일(최신 누적 시점)
    # ── 벤치마크(동일가중 유니버스 매수 후 보유) / 알파 ──
    bench_return:  float = 0.0   # 동일가중 매수 후 보유 수익률(%)
    bench_mdd:     float = 0.0
    bench_sharpe:  float = 0.0
    alpha:         float = 0.0   # 전략 - 동일가중 벤치 (초과수익 %p)
    kodex_return:  float | None = None   # KODEX200 매수 후 보유 수익률(%), 데이터 없으면 None
    regime_off:    int = 0               # 시장국면 필터로 현금화한 리밸런싱 횟수
    # ── 일별 상태(데일리 export용) ──
    equity_dates:  list = None   # list[str]
    equity_curve:  list = None   # list[float] 전략
    bench_curve:   list = None   # list[float] 동일가중
    daily_cash:    list = None
    daily_npos:    list = None
    daily_holdings: list = None   # [[(ticker,qty,value),...] per day]


def market_risk_on(visible: dict, ma: int = 200, thresh: float = 0.5) -> bool:
    """시장 국면 = 유니버스 종목 중 ma일 이평선 위 비율(breadth) >= thresh면 risk-on(상승),
    미만이면 risk-off(하락→현금). visible: ticker→bars(평가일까지, look-ahead 없음).
    백테스트 엔진과 라이브 ForwardTrader가 **공유**(결정 로직 단일 소스 = 신호 패리티)."""
    above = total = 0
    for bars in visible.values():
        if len(bars) < ma:
            continue
        sma = sum(b.close for b in bars[-ma:]) / ma
        total += 1
        if bars[-1].close > sma:
            above += 1
    if total == 0:
        return True   # 워밍업 부족 등 판단 불가 → 투자 유지(보수적으로 막지 않음)
    return (above / total) >= thresh


def index_risk_on(bars: list, ma: int = 200) -> bool:
    """지수(예: KODEX200) 종가가 ma일 이평선 위면 risk-on, 아래면 risk-off(현금).
    bars: 평가일까지의 지수 일봉(look-ahead 없음). 유니버스 breadth보다 신호가 매끄러워
    경계선 진동(whipsaw)이 적다 → 매일 체크에 적합. 워밍업 부족 시 보수적으로 투자 유지."""
    if len(bars) < ma:
        return True
    sma = sum(b.close for b in bars[-ma:]) / ma
    return bars[-1].close > sma


def equal_weight_qty(equity: float, n_target: int, price: float,
                     cost_rate: float, available_cash: float) -> int:
    """동일가중 1슬롯 매수 수량(정수주). 슬롯=equity/N, 가용현금 cap, 비용 반영.
    백테스트·라이브 공유 사이징(집행 환경 달라도 수량 규칙 동일)."""
    if price <= 0 or n_target <= 0:
        return 0
    budget = min(equity / max(1, n_target), available_cash)
    return int(budget / (price * (1 + cost_rate)))


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
                 cost_model: CostSpec | None = None,
                 target_positions: int = 10,
                 warmup_days: int = 14,
                 regime_on: bool = False, regime_ma: int = 200,
                 regime_thresh: float = 0.5, daily_regime: bool = False,
                 regime_mode: str = "breadth", regime_index: str = "^KS11",
                 vol_target: float = 0.0, vol_window: int = 20):
        self.kis        = kis
        self.strategy   = strategy
        self.init_cash  = initial_cash
        self.cash       = initial_cash
        self.cost       = cost_model or LIVE     # 비용은 라이브 원장과 한 소스(backtest/costs.py)
        self.target_positions = target_positions   # TARGET_WEIGHT 동일가중 분모
        self.warmup_days = warmup_days             # start_date 이전 워밍업 일수(모멘텀 lookback)
        self.regime_on = regime_on                 # 시장국면 필터(하락장 현금화) on/off
        self.regime_ma = regime_ma                 # 국면판정 이평 기간(거래일)
        self.regime_thresh = regime_thresh         # breadth(이평위 비율) 임계 — 미만이면 risk-off
        self.daily_regime = daily_regime           # True=매일 국면체크(전환시 즉시 현금화/재진입), False=리밸런싱일만
        self.regime_mode = regime_mode             # "breadth"=유니버스 이평위 비율, "index"=지수 200MA(매끄러움)
        self.regime_index = regime_index           # index 모드 지수 티커(yfinance 심볼, 기본 ^KS11 코스피)
        self._index_bars: list[Bar] = []           # index 모드용 지수 일봉(pre_start~end, 200MA 워밍업 포함)
        self.vol_target = vol_target               # 변동성 타게팅: 연환산 목표변동성(0=비활성). 실현변동성>목표면 노출↓
        self.vol_window = vol_window               # 실현변동성 측정 일수(일간 포트수익률)
        self._regime_off_days = 0                  # 리스크오프로 현금화한 횟수(리포트용)
        self._last_target: set = set()             # 최신 종목선정(재진입용 — 국면 회복시 이걸로 복귀)
        self._regime_state = True                  # 직전 국면(risk-on=True). 전환 감지용
        self._trades:   list[Trade] = []
        self._equity:   list[float] = []      # 날짜별 포트폴리오 평가금액 (현금 + 보유 포지션 시가)
        self._equity_dates: list[str] = []    # _equity와 1:1 정렬된 거래일
        self._ledger = PositionLedger()       # 보유 수량·평단·실현손익 — 라이브 OrderGate와 같은 규칙
        self._date_index: dict[str, list[str]] = {}   # ticker → 정렬된 봉 날짜(이분 탐색용)
        self._names:    dict[str, str] = {}   # ticker → 종목명 (소스가 제공 시)
        self._daily_cash: list[float] = []    # 일별 현금 잔고 (상태 export용)
        self._daily_npos: list[int] = []      # 일별 보유 종목수 (상태 export용)
        self._daily_holdings: list = []       # 일별 보유 상세 [[(ticker,qty,value),...], ...]

    @property
    def _positions(self) -> dict[str, int]:
        """ticker → 보유 수량. 원장이 정본이고 이 뷰는 읽기 전용이다."""
        return self._ledger.holdings()

    def _label(self, code: str) -> str:
        nm = self._names.get(code)
        return f"{code}({nm})" if nm else code

    def run(self, universe: list[str], start_date: str, end_date: str,
            verbose: bool = True) -> BacktestResult:
        from datetime import date as _date, timedelta
        self._verbose = verbose   # 스윕 등 대량 실행 시 False로 출력 억제
        if verbose:
            print(f"\n{'='*60}")
            print(f"백테스팅: {self.strategy.id()}")
            print(f"기간: {start_date} ~ {end_date}  |  종목: {len(universe)}개")
            print(f"초기 자금: {self.init_cash:,.0f}원")
            print('='*60)

        # 워밍업(스크리닝/모멘텀 lookback)용 여분 과거 데이터
        pre_start = (_date.fromisoformat(start_date) - timedelta(days=self.warmup_days)).isoformat()

        # 1. 전 종목 일봉 데이터 수집 (날짜 범위 기반)
        #    소스가 병렬 prefetch를 지원하면 루프 전 한 번에 캐시를 채운다(대량 수집 가속).
        if hasattr(self.kis, "prefetch_ohlcv"):
            self.kis.prefetch_ohlcv(universe, pre_start, end_date)
        # per-ticker throttle — 소스가 지정(fetch_sleep)하면 사용, 없으면 0.3(기존 KrxSource).
        per_sleep = getattr(self.kis, "fetch_sleep", 0.3)
        raw_bars:  dict[str, list[Bar]] = {}   # pre_start ~ end_date
        all_bars:  dict[str, list[Bar]] = {}   # start_date ~ end_date (시뮬레이션용)
        for i, ticker in enumerate(universe):
            if verbose:
                print(f"\r  데이터 수집 중... {i+1}/{len(universe)} ({ticker})", end="", flush=True)
            bars = sorted(self.kis.get_historical_ohlcv(ticker, pre_start, end_date), key=lambda bar: bar.date)
            sim  = [b for b in bars if b.date >= start_date]
            if sim:
                raw_bars[ticker] = bars
                all_bars[ticker] = sim
                self._date_index[ticker] = [bar.date for bar in bars]
            if per_sleep:
                time.sleep(per_sleep)
        if verbose:
            print(f"\r  데이터 수집 완료: {len(all_bars)}종목{' '*20}")

        # 종목명 prefetch (소스가 제공할 때) — 로그/리포트 가독성
        if hasattr(self.kis, "ticker_name"):
            for t in all_bars:
                self._names[t] = self.kis.ticker_name(t)

        # 지수 regime 모드용 지수 일봉(pre_start부터 — 200MA 워밍업 포함). breadth 모드면 불필요.
        # 지수/해외지표는 datagokr(주식 전용)로 못 받으므로 yfinance IndexSource 사용.
        # ⚠️ 연구용: 라이브(forward_trader)는 breadth만 지원 — index/daily 채택 시 신호 패리티 별도 작업 필요(C-2).
        if self.regime_on and self.regime_mode == "index":
            from data.index_source import IndexSource
            self._index_bars = IndexSource().get_historical_ohlcv(self.regime_index, pre_start, end_date) or []
            # C-1: 봉 부족이면 index_risk_on이 조용히 True(전구간 풀투자)로 무력화되어
            # "regime ON" 결과가 실제로는 regime OFF가 된다 → 과적합 판단 오염. 명시적 중단.
            if len(self._index_bars) < self.regime_ma:
                raise RuntimeError(
                    f"index regime 활성인데 지수({self.regime_index}) 봉 부족"
                    f"({len(self._index_bars)}<{self.regime_ma}). yfinance 설치/네트워크/티커 확인. "
                    f"조용한 풀투자 방지 위해 중단.")
            if verbose:
                cov = sum(1 for b in self._index_bars if b.date >= start_date)
                print(f"  지수({self.regime_index}) 일봉 수집: {len(self._index_bars)}봉 (시뮬구간 {cov}봉)")

        # 1b. 수급 데이터 — 전략이 수급을 쓰고(uses_flow) 소스가 제공할 때만 수집(불필요 호출/노이즈 방지)
        all_flow: dict[str, list] = {}
        if getattr(self.strategy, "uses_flow", False) and hasattr(self.kis, "flow_history"):
            for ticker in list(all_bars.keys()):
                all_flow[ticker] = self.kis.flow_history(ticker, pre_start, end_date)
            if verbose:
                print(f"  수급 수집 완료: {sum(1 for v in all_flow.values() if v)}종목")

        # 2. 전략 자체 스크리닝 — look-ahead 차단 as-of 어댑터로 on_start 호출.
        #    엔진은 전략 로직을 모른다(전략-불가지). 빈 리스트 반환 시 전 종목 감시.
        asof = _AsOfKisAdapter(self.kis, raw_bars, start_date)
        self.strategy.set_kis(asof)
        watch = self.strategy.on_start(universe) or []
        watch = [t for t in watch if t in all_bars]
        if not watch:
            watch = list(all_bars.keys())
        if verbose:
            print(f"  감시 종목: {len(watch)}개")

        # 3. 날짜 순으로 시뮬레이션
        all_dates = sorted({b.date for bars in all_bars.values() for b in bars})

        # 종목별 커서 — 봉이 날짜순으로 정렬돼 있으므로 "date까지 보이는 봉"은 앞에서부터 cursor개.
        #  커서는 앞으로만 움직여 전 기간 합쳐 O(봉 수). 전략에 넘기는 리스트는 접두 슬라이스(얕은 복사)라
        #  미래 봉이 구조적으로 들어갈 수 없다. 신호는 종가 확정 후, 체결은 다음 봉 시가.
        cursor: dict[str, int] = {ticker: 0 for ticker in watch}

        for date in all_dates:
            visible_all: dict[str, list[Bar]] = {}
            for ticker in watch:
                bars = raw_bars.get(ticker)
                if not bars:
                    continue
                position = cursor[ticker]
                while position < len(bars) and bars[position].date <= date:
                    position += 1
                cursor[ticker] = position
                if position > 0:
                    visible_all[ticker] = bars[:position]

            # 수급도 date 미만으로 잘라 전달 (T-1 확정만 — look-ahead·발표시차 차단)
            flow_visible = {t: [f for f in all_flow.get(t, []) if f.date < date]
                            for t in visible_all}

            # 종목 재선정(주기적) — 목표집합 갱신. 종목 선정은 느려도 됨(6개월 신호).
            target = self.strategy.on_rebalance(date, visible_all, flow_visible)
            is_rebal = target is not None
            if is_rebal:
                self._last_target = set(target)

            # 국면(regime) 게이트 — daily_regime이면 매일 체크(빠른 대응), 아니면 리밸런싱일만.
            # 하락국면→현금화, 회복→마지막 종목선정으로 재진입. "현금화는 빠르게, 종목교체는 느리게".
            if self.regime_on and (is_rebal or self.daily_regime):
                risk_on = self._market_risk_on(visible_all, date)
            else:
                risk_on = True   # regime 미사용 또는 비체크일 → 보유 유지
            desired = self._last_target if risk_on else set()
            flipped = self.regime_on and self.daily_regime and (risk_on != self._regime_state)
            if is_rebal or flipped:
                self._rebalance_to_target(set(desired), date, raw_bars)
                if not risk_on:
                    self._regime_off_days += 1
            self._regime_state = risk_on

            # per-ticker 신호(개별 MARKET 주문) — 다음봉 시가 체결
            for ticker, vis in visible_all.items():
                sig = self.strategy.on_data(ticker, vis)
                if sig is not None:
                    self._execute(sig, date, raw_bars)

            # 일별 포트폴리오 평가: 현금 + 보유 포지션 당일 종가 기준 (매일 기록 — 데일리 상태)
            port_value = self.cash
            holds: list = []
            for t, qty in self._positions.items():
                day_bar = self._bar_on(t, date, raw_bars)
                if day_bar:
                    val = qty * day_bar.close
                    port_value += val
                    holds.append((t, qty, val))
            self._equity.append(port_value)
            self._equity_dates.append(date)
            self._daily_cash.append(self.cash)
            self._daily_npos.append(len(self._positions))
            self._daily_holdings.append(holds)

        self.strategy.on_stop()

        # 벤치마크: 유니버스 동일가중 매수 후 보유 + KODEX200(069500) 매수 후 보유 — 알파/베타 분리용
        self._bench_eq  = self._buy_and_hold_equity(list(all_bars.keys()), all_dates, all_bars)
        kodex_bars = {}
        try:
            kb = self.kis.get_historical_ohlcv("069500", start_date, end_date)
            if kb:
                kodex_bars["069500"] = kb
        except Exception:
            kodex_bars = {}
        # 지수 ETF 하나는 데이터가 일찍 끝나도 상폐가 아니므로 −100% 처리를 끈다.
        self._kodex_eq = (self._buy_and_hold_equity(["069500"], all_dates, kodex_bars, delist_to_zero=False)
                          if kodex_bars else None)
        return self._calc_result()

    def _buy_and_hold_equity(self, tickers: list[str], all_dates: list[str],
                        bars_by_ticker: dict, delist_to_zero: bool = True) -> list[float] | None:
        """초기자금을 종목들에 동일가중 분배해 시작일 시가 매수 후 보유. 일별 평가액 시계열 반환.
        매수 비용(수수료·충격)은 전략과 같은 `self.cost`로 뗀다. 청산 비용은 안 뗀다 — 전략 쪽 최종 equity도
        보유분을 평가액 그대로 두므로 같은 잣대다.
        결측일(거래정지)은 마지막 종가로 평가하고, 마지막 봉 이후(상폐·데이터 종료)는 0원(−100%)이다.
        전방채움으로 상폐 손실을 감추면 벤치마크가 살아남은 종목만 세는 것이 된다."""
        present = {t: bars for t in tickers
                   if (bars := bars_by_ticker.get(t)) }
        n = len(present)
        if n == 0 or not all_dates:
            return None
        alloc = self.init_cash / n
        shares: dict[str, float] = {}
        close_map: dict[str, dict] = {}
        last_bar_date: dict[str, str] = {}
        for t, bars in present.items():
            first_price = bars[0].open if bars[0].open > 0 else bars[0].close
            shares[t] = (alloc / (first_price * (1.0 + self.cost.buy_cost_rate))) if first_price > 0 else 0.0
            close_map[t] = {b.date: b.close for b in bars}
            last_bar_date[t] = max(bar.date for bar in bars)
        last_close = {t: 0.0 for t in present}
        eq: list[float] = []
        for d in all_dates:
            v = 0.0
            for t in present:
                c = close_map[t].get(d)
                if c is not None:
                    last_close[t] = c
                elif delist_to_zero and d > last_bar_date[t]:
                    last_close[t] = 0.0
                v += shares[t] * last_close[t]
            eq.append(v)
        return eq

    def _bar_on(self, ticker: str, date: str, all_bars: dict):
        """date 당일 봉. 없으면 None. 정렬된 날짜 배열을 이분 탐색한다."""
        bars = all_bars.get(ticker)
        if not bars:
            return None
        dates = self._date_index.get(ticker)
        if dates is None or len(dates) != len(bars):
            return next((bar for bar in bars if bar.date == date), None)
        position = bisect_right(dates, date) - 1
        return bars[position] if position >= 0 and dates[position] == date else None

    def _next_bar_index(self, ticker: str, date: str, all_bars: dict) -> int:
        """date 다음 봉의 인덱스(없으면 len). 정렬된 날짜 배열을 이분 탐색한다."""
        bars = all_bars.get(ticker, [])
        dates = self._date_index.get(ticker)
        if dates is None or len(dates) != len(bars):
            return next((index for index, bar in enumerate(bars) if bar.date > date), len(bars))
        return bisect_right(dates, date)

    def _market_risk_on(self, visible: dict, date: str) -> bool:
        """국면 판정. index 모드=지수 200MA(매끄러움, whipsaw 적음), breadth 모드=유니버스 이평위 비율.
        둘 다 평가일까지 데이터만 사용(look-ahead 없음). 백테스트·라이브 단일 소스."""
        if self.regime_mode == "index":
            vis_idx = [b for b in self._index_bars if b.date <= date]
            return index_risk_on(vis_idx, self.regime_ma)
        return market_risk_on(visible, self.regime_ma, self.regime_thresh)

    def _execute(self, sig, date: str, all_bars: dict):
        """신호를 다음 봉 시가로 체결(look-ahead 방지). cash/원장/trades 갱신.
        비용·평단·실현손익은 `PositionLedger`(라이브 OrderGate와 같은 규칙)가 계산한다."""
        bars = all_bars.get(sig.ticker, [])
        next_index = self._next_bar_index(sig.ticker, date, all_bars)
        if next_index < len(bars):
            next_bar = bars[next_index]
            price = next_bar.open if next_bar.open > 0 else next_bar.close
        elif sig.side == "SELL":
            # 미래봉 없음(상폐/데이터종료) + 매도 → 마지막 알려진 종가로 강제 청산(W-1).
            # 자본이 포지션에 영구 잠겨 equity에서 증발하는 것 방지.
            price = bars[next_index - 1].close if next_index > 0 else 0.0
        else:
            return  # 미래봉 없음 + 매수 → 체결 불가
        if price <= 0:
            return

        qty = sig.quantity   # 횡단면 동일가중 사이징은 _rebalance_to_target가 끝내고 MARKET으로 전달
        if qty <= 0:
            return

        if sig.side == "BUY":
            total_cost = fill_result("BUY", price, qty, self.cost).net
            if self.cash >= total_cost:
                filled = self._ledger.on_fill(sig.ticker, "BUY", price, qty, self.cost)
                self.cash -= filled.fill.net
                self._trades.append(Trade(sig.ticker, "BUY", date, filled.fill.price, qty))
        elif sig.side == "SELL":
            held = self._ledger.quantity(sig.ticker)
            sell_qty = min(qty, held)  # 보유 수량 초과 매도 방지
            if sell_qty > 0:
                # 손익 = (체결가 − 평단) × 수량 − 매도 수수료·세금. 매수 수수료는 매수 시점에 원장이 뺐다.
                filled = self._ledger.on_fill(sig.ticker, "SELL", price, sell_qty, self.cost)
                self.cash += filled.fill.net
                self._trades.append(Trade(sig.ticker, "SELL", date, filled.fill.price, sell_qty, filled.realized_pnl))

    def _peek_next_open(self, ticker: str, date: str, all_bars: dict):
        """다음 봉 시가(체결가) 미리보기 — 사이징용. 없으면 None."""
        bars = all_bars.get(ticker, [])
        next_index = self._next_bar_index(ticker, date, all_bars)
        if next_index >= len(bars):
            return None
        next_bar = bars[next_index]
        return next_bar.open if next_bar.open > 0 else next_bar.close

    def _vol_exposure(self) -> float:
        """변동성 타게팅 노출계수 ∈ (0,1]. 최근 vol_window 일간 포트수익률의 연환산 실현변동성이
        vol_target보다 크면 노출<1(현금↑), 이하면 1.0(무레버리지 cap). vol_target<=0이면 비활성.
        self._equity는 직전일까지의 평가금액(과거)만 담겨 look-ahead 없음."""
        if self.vol_target <= 0:
            return 1.0
        eq = self._equity
        if len(eq) < self.vol_window + 1:
            return 1.0   # 측정 표본 부족 → 풀노출
        import statistics
        rets = [eq[i] / eq[i - 1] - 1.0
                for i in range(len(eq) - self.vol_window, len(eq))
                if i > 0 and eq[i - 1] > 0]
        if len(rets) < 2:
            return 1.0
        ann = statistics.pstdev(rets) * (252 ** 0.5)
        return 1.0 if ann <= 0 else min(1.0, self.vol_target / ann)

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

        # 2. 변동성 타게팅 노출 적용 — 투자가능자본 = equity * expo. expo<1이면 전체 노출을 줄임.
        equity   = self._equity[-1] if self._equity else self.init_cash
        expo     = self._vol_exposure()
        invest_equity = equity * expo
        cost_rate = self.cost.buy_cost_rate
        slot_val = invest_equity / max(1, len(target))   # 슬롯당 목표 평가금액

        # 2a. expo<1: 목표를 유지하는 보유분도 슬롯 목표가치 초과분만큼 매도(전체 노출을 expo로 수렴).
        #     vol_target=0이면 expo=1.0이라 이 블록 스킵 → 기존 동작(top-up/trim 없음) 그대로.
        if expo < 1.0:
            for t in sorted(target & set(self._positions.keys())):
                price = self._peek_next_open(t, date, all_bars)
                if price is None or price <= 0:
                    continue
                cur_val = self._positions[t] * price
                if cur_val > slot_val:
                    trim = int((cur_val - slot_val) / price)
                    if trim > 0:
                        self._execute(OrderSignal(t, "SELL", trim, "MARKET"), date, all_bars)

        # 2b. 목표 중 미보유분 동일가중 매수 (선택 종목끼리 동일가중, 비용+가용현금 반영).
        #     분모를 len(target)으로 — target<N일 때 (N-target)/N 자본이 유휴로 남는 문제 해소(W-1).
        for t in sorted(target):   # 정렬 — set 순회순서 randomization 제거(백테스트 재현성)
            if t in self._positions:
                continue   # 이미 보유(목표 유지) — top-up 안 함(턴오버 절감)
            price = self._peek_next_open(t, date, all_bars)
            if price is None or price <= 0:
                continue
            qty = equal_weight_qty(invest_equity, len(target), price, cost_rate, self.cash)  # 노출반영 사이징
            if qty > 0:
                self._execute(OrderSignal(t, "BUY", qty, "MARKET"), date, all_bars)

        if (new_in or gone) and getattr(self, "_verbose", True):
            print(f"  [{date}] 리밸런싱: 신규 {[self._label(t) for t in new_in]} / "
                  f"청산 {[self._label(t) for t in gone]} → 보유 {len(self._positions)}종목")

    @staticmethod
    def _curve_stats(equity: list[float]) -> tuple[float, float, float]:
        """equity 시계열 → (총수익률%, 최대낙폭(MDD)%, 연환산 샤프). 초기값 대비."""
        if not equity:
            return 0.0, 0.0, 0.0
        total_return = (equity[-1] - equity[0]) / equity[0] * 100 if equity[0] > 0 else 0.0
        peak, mdd = equity[0], 0.0
        for e in equity:
            peak = max(peak, e)
            dd   = (peak - e) / peak * 100 if peak > 0 else 0.0
            mdd  = max(mdd, dd)
        sharpe = 0.0
        if len(equity) > 1:
            import statistics
            rets = [(equity[i] - equity[i-1]) / equity[i-1]
                    for i in range(1, len(equity)) if equity[i-1] > 0]
            if len(rets) > 1:
                std = statistics.stdev(rets)
                sharpe = (statistics.mean(rets) / std * (252 ** 0.5)) if std > 0 else 0.0
        return total_return, mdd, sharpe

    def _calc_result(self) -> BacktestResult:
        sells    = [t for t in self._trades if t.side == "SELL"]
        total_pnl = sum(t.pnl for t in sells)
        wins      = [t for t in sells if t.pnl > 0]
        win_rate  = len(wins) / len(sells) * 100 if sells else 0.0

        # 전략: 미청산 보유 평가액 포함 최종 equity 기반 (init_cash 대비)
        final_equity = self._equity[-1] if self._equity else self.cash
        total_return = (final_equity - self.init_cash) / self.init_cash * 100
        _, mdd, sharpe = self._curve_stats(self._equity)

        # 벤치마크(동일가중 유니버스 매수 후 보유) 통계 + 알파
        bench_eq = getattr(self, "_bench_eq", None)
        bench_return = bench_mdd = bench_sharpe = 0.0
        if bench_eq:
            bench_return, bench_mdd, bench_sharpe = self._curve_stats(bench_eq)
        kodex_eq = getattr(self, "_kodex_eq", None)
        kodex_return = self._curve_stats(kodex_eq)[0] if kodex_eq else None

        return BacktestResult(
            trades       = self._trades,
            total_return = total_return,
            mdd          = mdd,
            sharpe       = sharpe,
            win_rate     = win_rate,
            total_pnl    = total_pnl,
            trade_count  = len(sells),
            start_date   = self._equity_dates[0]  if self._equity_dates else "",
            end_date     = self._equity_dates[-1] if self._equity_dates else "",
            bench_return = bench_return,
            bench_mdd    = bench_mdd,
            bench_sharpe = bench_sharpe,
            alpha        = total_return - bench_return,
            kodex_return = kodex_return,
            regime_off   = self._regime_off_days,
            equity_dates = list(self._equity_dates),
            equity_curve = list(self._equity),
            bench_curve  = list(bench_eq) if bench_eq else None,
            daily_cash   = list(self._daily_cash),
            daily_npos   = list(self._daily_npos),
            daily_holdings = list(self._daily_holdings),
        )
