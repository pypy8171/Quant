"""
CrossMomentumStrategy — 횡단면(cross-sectional) 모멘텀.

유니버스 전 종목의 과거 수익률(lookback ~ skip 구간, 최근 skip일 제외 = 12-1 스타일)을 랭킹,
상위 top_n 종목 동일가중 보유, rebalance_every 거래일마다 리밸런싱. on_rebalance만 사용.
OHLCV만 필요(수급/외부로그인 불필요) — 한국시장 검증 팩터, 가장 빠른 "검증된 엣지" 후보.

look-ahead: 결정은 date 종가 기준, 체결은 엔진이 다음봉 시가(표준 모멘텀, 누설 없음).
사이징: BUY에 quantity=0 + order_type="TARGET_WEIGHT" → 엔진 동일가중 사이징.
"""
from typing import Optional
from kis.client import Bar, OrderSignal
from strategy.base import StrategyBase


class CrossMomentumStrategy(StrategyBase):
    def __init__(self, top_n: int = 10, rebalance_every: int = 20,
                 lookback: int = 120, skip: int = 20, vol_adjust: bool = False):
        super().__init__()
        self.top_n           = top_n
        self.rebalance_every = rebalance_every
        self.lookback        = lookback   # 거래일 (≈6개월)
        self.skip            = skip       # 최근 제외 거래일 (≈1개월, 단기 반전 회피)
        self.vol_adjust      = vol_adjust # True면 점수=수익률/변동성(펌프주 강등)
        self._tick = 0
        self._held: set[str] = set()

    def id(self) -> str:
        mode = "voladj" if self.vol_adjust else "raw"
        return (f"CROSS_MOMENTUM (top{self.top_n}, rb{self.rebalance_every}d, "
                f"lb{self.lookback}/skip{self.skip}, {mode})")

    def on_start(self, universe: list[str]) -> list[str]:
        return []   # 전 종목 감시 — 랭킹은 on_rebalance에서

    def on_data(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        return None  # 횡단면 전략 — on_rebalance만 사용

    def on_rebalance(self, date, visible, flow=None):
        self._tick += 1
        if (self._tick - 1) % self.rebalance_every != 0:
            return None   # 리밸런싱 날 아님

        # 모멘텀: skip일 전 종가 / lookback일 전 종가 - 1  (최근 skip일 제외)
        # vol_adjust=True면 형성구간 일별수익률 변동성으로 나눠 위험조정(펌프주 강등).
        import statistics
        scores: dict[str, float] = {}
        need = self.lookback + 1
        for t, bars in visible.items():
            n = len(bars)
            if n < need:
                continue
            i_past   = n - 1 - self.lookback
            i_recent = n - 1 - self.skip
            past   = bars[i_past].close
            recent = bars[i_recent].close
            if past <= 0:
                continue
            mom = recent / past - 1.0
            if not self.vol_adjust:
                scores[t] = mom
                continue
            # 형성구간(past~recent) 일별수익률 표준편차로 위험조정
            seg = bars[i_past : i_recent + 1]
            rets = [seg[i].close / seg[i-1].close - 1.0
                    for i in range(1, len(seg)) if seg[i-1].close > 0]
            if len(rets) > 1:
                vol = statistics.stdev(rets)
                scores[t] = (mom / vol) if vol > 0 else 0.0
            # 변동성 산출 불가 → 점수 미부여(제외, 보수적)

        ranked = sorted(((s, t) for t, s in scores.items() if s > 0), reverse=True)
        return {t for _, t in ranked[:self.top_n]}   # 목표 집합 — 엔진이 청산·매수·사이징 책임
