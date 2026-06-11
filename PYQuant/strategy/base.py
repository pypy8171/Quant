"""
전략 베이스 클래스 — 모든 전략은 이를 상속
C++ StrategyBase와 동일 인터페이스
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Optional
from kis.client import Bar, OrderSignal


@dataclass
class Position:
    ticker:     str
    quantity:   int   = 0
    avg_price:  float = 0.0
    entry_date: str   = ""


class StrategyBase(ABC):
    uses_flow: bool = False   # True면 엔진이 수급(flow_history)을 수집해 on_rebalance에 전달

    def __init__(self):
        self.positions: dict[str, Position] = {}
        self._kis = None

    @abstractmethod
    def id(self) -> str: ...

    @abstractmethod
    def on_start(self, universe: list[str]) -> list[str]:
        """
        장 시작 전 호출. 구독할 종목 리스트 반환(빈 리스트면 전 종목 감시).
        universe: 전체 조회 가능 종목. 백테스트에선 look-ahead 차단 as-of 어댑터가 주입됨.
        """
        ...

    @abstractmethod
    def on_data(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        """
        종목별 봉 데이터 수신 시 호출.
        매수/매도 신호 반환, 없으면 None.
        """
        ...

    def on_rebalance(self, date: str, visible: dict[str, list[Bar]],
                     flow: dict | None = None):
        """
        횡단면 리밸런싱 훅. **리밸런싱 날이면 목표 보유 종목 집합(set[str], 동일가중)을 반환,
        아니면 None.** 빈 set = 전량 청산(현금). 엔진이 실보유(_positions) 기준으로 이탈 청산 +
        신규 매수 + 비용·가용현금 반영 사이징을 원자적으로 책임진다(전략은 cash·보유를 몰라도 됨).
        visible/flow 모두 `date 미만`으로 잘려 전달(look-ahead·발표시차 차단).
        기본 None(리밸런싱 안 함) — per-ticker 전략(value_contrary)은 영향 없음.
        """
        return None

    def on_stop(self):
        pass

    def set_kis(self, kis):
        """KIS 클라이언트 주입(라이브=실제, 백테스트=as-of 어댑터)."""
        self._kis = kis

    def set_candidates(self, tickers: list[str]):
        """(레거시) 백테스팅 엔진이 후보를 직접 주입할 때. on_start 경로로 대체됨."""
        pass

    # ── 포지션 관리 헬퍼 ────────────────────────────────────────────────────
    def has_position(self, ticker: str) -> bool:
        return ticker in self.positions and self.positions[ticker].quantity > 0

    def open_position(self, ticker: str, quantity: int, price: float, date: str):
        self.positions[ticker] = Position(ticker, quantity, price, date)

    def close_position(self, ticker: str):
        self.positions.pop(ticker, None)
