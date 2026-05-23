"""
Strategy A — 강세 테마주 5일선 눌림목 추종

필터:
  C1: 유니버스 (config/strategy_a.json 테마 4그룹)
  C2: 정배열 SMA5 > SMA10 > SMA20 > SMA60
  C3.5: 전일 종가 >= SMA5 (5일선 위)
  C3: 수급 필터 (pykrx 검증 후 STEP F에서 추가 — 현재 미구현)

진입 (E):
  E1: 유니버스 종목
  E2: 종가 >= SMA5 (C3.5 동일)
  E3: 이격도 <= Z% (5일선 눌림목)

청산 (X):
  X1: 이격도 >= A% (과익 청산)
  X2: 종가 < SMA5 * (1 - B/100) (추세 이탈 손절)

백테스트: BacktestEngine이 on_start를 호출하지 않으므로, 모든 필터링은
          on_data 안에서 visible bars 기준으로 수행한다. (look-ahead 없음)
라이브:   on_start에서 KIS API로 pre-screening 후 self._watch에 저장.
"""
import json
import time
from pathlib import Path
from typing import Optional

from kis.client import Bar, KisClient, OrderSignal
from strategy.base import StrategyBase
from strategy.indicators import sma, is_aligned


_DEFAULT_CONFIG = Path(__file__).parent.parent / "config" / "strategy_a.json"


class StrategyA(StrategyBase):
    def __init__(
        self,
        tickers: list[str] | None = None,
        config_path: str | Path | None = None,
        z: float = 1.0,
        a: float = 7.0,
        b: float = 2.0,
        quantity: int = 1,
    ):
        """
        tickers: 직접 종목 코드 주입 (테스트용). None이면 config 파일에서 로드.
        config_path: None이면 config/strategy_a.json 자동 탐색.
        """
        super().__init__()
        self.z = z
        self.a = a
        self.b = b
        self.quantity = quantity
        self._kis: KisClient | None = None
        self._watch: set[str] = set()  # 라이브 pre-screening 결과

        if tickers is not None:
            self._universe: set[str] = set(tickers)
        else:
            path = Path(config_path) if config_path else _DEFAULT_CONFIG
            with open(path, encoding="utf-8") as f:
                cfg = json.load(f)
            themes = cfg.get("themes", {})
            self._universe = {t for tickers_list in themes.values() for t in tickers_list}

    def id(self) -> str:
        return "strategy_a_v1"

    def set_kis(self, kis: KisClient):
        self._kis = kis

    def set_candidates(self, tickers: list[str]):
        # BacktestEngine이 ValueContrary 스크리닝 결과를 주입하지만 이 전략과 무관.
        pass

    # ── 라이브 트레이딩 pre-screening ────────────────────────────────────────

    def on_start(self, universe: list[str]) -> list[str]:
        """
        라이브 전용 — 장 전에 C2+C3.5로 유니버스를 스크리닝해 self._watch를 채운다.
        BacktestEngine은 이 메서드를 호출하지 않는다.
        """
        from datetime import date as _date, timedelta
        assert self._kis is not None, "set_kis() 먼저 호출 필요"

        today     = _date.today().isoformat()
        pre_start = (_date.today() - timedelta(days=100)).isoformat()

        watch: list[str] = []
        for ticker in sorted(self._universe):
            bars = self._kis.get_historical_ohlcv(ticker, pre_start, today)
            time.sleep(0.3)
            if len(bars) < 60:
                continue
            if not is_aligned(bars):
                continue
            sma5 = sma(bars, 5)
            if sma5 and bars[-1].close >= sma5:
                watch.append(ticker)

        self._watch = set(watch)
        return watch

    # ── 시그널 생성 ──────────────────────────────────────────────────────────

    def on_data(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        # 보유 여부로 먼저 분기 — 동일 봉에서 진입/청산 동시 평가 금지
        if self.has_position(ticker):
            return self._check_exit(ticker, bars)
        return self._check_entry(ticker, bars)

    def _check_entry(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        # E1: 유니버스 멤버십
        if ticker not in self._universe:
            return None
        # C2 판정에 60봉 필요
        if len(bars) < 60:
            return None
        # C2: 정배열 SMA5 > SMA10 > SMA20 > SMA60
        if not is_aligned(bars):
            return None

        sma5 = sma(bars, 5)
        if sma5 is None or sma5 == 0:
            return None
        close = bars[-1].close

        # E2 = C3.5: 종가 >= SMA5 (5일선 위)
        if close < sma5:
            return None
        # E3: 이격도 <= Z (5일선에 충분히 근접)
        dev = (close - sma5) / sma5 * 100
        if dev > self.z:
            return None

        self.open_position(ticker, self.quantity, close, bars[-1].date)
        return OrderSignal(ticker=ticker, side="BUY", quantity=self.quantity)

    def _check_exit(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        if len(bars) < 5:
            return None
        sma5 = sma(bars, 5)
        if sma5 is None or sma5 == 0:
            return None
        close = bars[-1].close
        dev = (close / sma5 - 1) * 100

        # X1: 이격도 >= A (과익 청산)
        if dev >= self.a:
            self.close_position(ticker)
            return OrderSignal(ticker=ticker, side="SELL", quantity=self.quantity)
        # X2: 종가 < SMA5 * (1 - B/100) (추세 이탈 손절)
        if close < sma5 * (1 - self.b / 100):
            self.close_position(ticker)
            return OrderSignal(ticker=ticker, side="SELL", quantity=self.quantity)
        return None
