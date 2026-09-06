"""
DonchianBreakout — per-stock 시계열(절대) 돌파 추세추종.

각 종목 독립 신호. 진입: 종가 ≥ 직전 period일 최고가(당일 미포함, t-1까지). 청산: 종가 ≤ 직전 period일 최저가.
돌파 상태인 종목을 동일가중 보유하고 청산 신호까지 유지한다. 상태(_in)를 유지하며
on_rebalance가 매일 "현재 보유해야 할 집합"을 반환 → 엔진이 diff로 신규매수/청산·동일가중 사이징.

look-ahead 차단: period일 최고/최저는 bars[-(period+1):-1] (직전 period봉, 신호봉 t 제외)로
계산해 '당일 종가가 당일 포함 최고를 넘는' 자기참조 누출을 막는다. 신호는 종가 t까지만,
체결은 엔진이 다음봉 시가. 정렬(sorted)로 결정론 유지.
"""
from strategy.base import StrategyBase


class DonchianBreakoutStrategy(StrategyBase):
    def __init__(self, period: int = 20, top_n: int = 0):
        super().__init__()
        self.period = period      # 채널 창(거래일)
        self.top_n  = top_n       # >0이면 최근 돌파강도 상위 N만 보유(0=돌파 전종목)
        self._in: set[str] = set()

    def id(self) -> str:
        cap = f", top{self.top_n}" if self.top_n else ""
        return f"DONCHIAN_BREAKOUT (period{self.period}{cap})"

    def on_start(self, universe):
        return []   # 전 종목 감시

    def on_data(self, ticker, bars):
        return None  # 상태형 집합은 on_rebalance에서

    def on_rebalance(self, date, visible, flow=None):
        for t, bars in visible.items():
            if len(bars) < self.period + 1:
                continue
            prior = bars[-(self.period + 1):-1]        # 직전 period봉(신호봉 t 제외)
            hi = max(b.high for b in prior)
            lo = min(b.low for b in prior)
            c = bars[-1].close
            if t in self._in:
                if c <= lo:                            # 청산 신호
                    self._in.discard(t)
            else:
                if c >= hi:                            # 돌파 진입
                    self._in.add(t)
        held = {t for t in self._in if t in visible}   # 데이터 있는 것만
        if self.top_n and len(held) > self.top_n:
            # 돌파강도(종가/직전최고 - 1) 상위 top_n만 — 과다분산 방지(선택 노브)
            strength = {}
            for t in held:
                bars = visible[t]
                prior = bars[-(self.period + 1):-1]
                hi = max(b.high for b in prior)
                strength[t] = (bars[-1].close / hi - 1.0) if hi > 0 else 0.0
            held = {t for t, _ in sorted(strength.items(), key=lambda kv: kv[1], reverse=True)[:self.top_n]}
        return set(held)
