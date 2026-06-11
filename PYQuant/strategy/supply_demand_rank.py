"""
SupplyDemandRankStrategy (버전 A) — 수급 단독 횡단면 랭킹.

외인+기관 N거래일 누적 순매수금액을 거래대금으로 정규화한 score 상위 top_n 종목을
동일가중 보유, rebalance_every 거래일마다 리밸런싱. on_rebalance만 사용(횡단면).
거래 수가 결정론적(편입/이탈 회전)이라 0거래 위험이 구조적으로 없다 — "결과 floor" 측정용.

look-ahead: 엔진이 flow/visible을 date 미만으로 잘라 넘기므로(T-1 확정만) 전략은 그대로 사용.
사이징: BUY 신호에 quantity=0 + order_type="TARGET_WEIGHT" → 엔진이 동일가중 사이징.
"""
from typing import Optional
from kis.client import Bar, OrderSignal
from strategy.base import StrategyBase


class SupplyDemandRankStrategy(StrategyBase):
    uses_flow = True   # 엔진이 수급 데이터를 수집·전달하도록

    def __init__(self, top_n: int = 10, rebalance_every: int = 5, flow_window: int = 5):
        super().__init__()
        self.top_n           = top_n
        self.rebalance_every = rebalance_every
        self.flow_window     = flow_window
        self._tick = 0
        self._held: set[str] = set()

    def id(self) -> str:
        return f"SUPPLY_DEMAND_RANK (top{self.top_n}, rb{self.rebalance_every}d, w{self.flow_window})"

    def on_start(self, universe: list[str]) -> list[str]:
        return []   # 전 종목 감시 — 랭킹은 on_rebalance에서

    def on_data(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        return None  # 횡단면 전략 — on_rebalance만 사용

    def on_rebalance(self, date, visible, flow=None):
        self._tick += 1
        if (self._tick - 1) % self.rebalance_every != 0:  # 첫날부터 N거래일마다 (every=1도 동작)
            return None   # 리밸런싱 날 아님
        flow = flow or {}

        # 외인+기관 N일 누적 순매수금액 / 거래대금 (대형주 쏠림 방지 정규화)
        scores: dict[str, float] = {}
        for t, flows in flow.items():
            f = flows[-self.flow_window:]
            if len(f) < self.flow_window:
                continue
            net  = sum(x.foreign_net + x.inst_net for x in f)
            turn = sum(x.turnover for x in f)
            if turn <= 0:
                continue
            scores[t] = net / turn

        ranked = sorted(((s, t) for t, s in scores.items() if s > 0), reverse=True)
        return {t for _, t in ranked[:self.top_n]}   # 목표 집합 — 엔진이 청산·매수·사이징 책임
