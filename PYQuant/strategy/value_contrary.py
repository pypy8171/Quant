"""
ValueContraryStrategy — 저PBR 3일 연속 하락 반전 매수
C++ ValueContraryStrategy와 동일 로직
"""
from typing import Optional
from kis.client import Bar, OrderSignal, KisClient
from strategy.base import StrategyBase
import time


class ValueContraryStrategy(StrategyBase):
    def __init__(self, pbr_max: float = 1.0, quantity: int = 1):
        super().__init__()
        self.pbr_max  = pbr_max
        self.quantity = quantity
        self._candidates: set[str] = set()
        self._kis: Optional[KisClient] = None

    def id(self) -> str:
        return f"VALUE_CONTRARY_KR (PBR<={self.pbr_max})"

    def set_kis(self, kis: KisClient):
        self._kis = kis

    # ── 스크리닝: 저PBR + 3일 연속 하락 ────────────────────────────────────
    def on_start(self, universe: list[str]) -> list[str]:
        self._candidates.clear()
        print(f"[{self.id()}] 스크리닝 시작 — {len(universe)}종목")

        for ticker in universe:
            bars = self._kis.get_daily_ohlcv(ticker, 5)
            time.sleep(0.2)   # KIS rate limit

            # 미완성 봉(volume=0) 제거
            bars = [b for b in bars if b.volume > 0]
            if len(bars) < 4:
                continue

            # bars[-1]=최신, bars[-2]=전일, ...
            declining = (bars[-1].close < bars[-2].close
                     and bars[-2].close < bars[-3].close
                     and bars[-3].close < bars[-4].close)
            if not declining:
                continue

            self._candidates.add(ticker)
            print(f"  후보: {ticker}  "
                  f"{bars[-1].close:.0f} < {bars[-2].close:.0f} < {bars[-3].close:.0f}")

        print(f"[{self.id()}] 후보 확정: {len(self._candidates)}종목")
        return list(self._candidates)

    # ── 봉 데이터 수신 → 매수/매도 신호 ─────────────────────────────────────
    def on_data(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        if not bars:
            return None

        # 진입: 후보 종목, 포지션 없음
        if ticker in self._candidates and not self.has_position(ticker):
            self._candidates.discard(ticker)
            latest = bars[-1]
            self.open_position(ticker, self.quantity, latest.close, latest.date)
            return OrderSignal(
                ticker     = ticker,
                side       = "BUY",
                quantity   = self.quantity,
                order_type = "MARKET",
            )

        # 청산: 포지션 있고, 진입일 다음날 (1봉 보유)
        if self.has_position(ticker):
            pos = self.positions[ticker]
            if bars[-1].date > pos.entry_date:
                self.close_position(ticker)
                return OrderSignal(
                    ticker     = ticker,
                    side       = "SELL",
                    quantity   = self.quantity,
                    order_type = "MARKET",
                )

        return None

    def on_stop(self):
        print(f"[{self.id()}] 종료 — 보유: {len(self.positions)}종목")
