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
    def __init__(self):
        self.positions: dict[str, Position] = {}

    @abstractmethod
    def id(self) -> str: ...

    @abstractmethod
    def on_start(self, universe: list[str]) -> list[str]:
        """
        장 시작 전 호출. 구독할 종목 리스트 반환.
        universe: 전체 조회 가능 종목 (빈 리스트면 전략이 직접 결정)
        """
        ...

    @abstractmethod
    def on_data(self, ticker: str, bars: list[Bar]) -> Optional[OrderSignal]:
        """
        종목별 봉 데이터 수신 시 호출.
        매수/매도 신호 반환, 없으면 None.
        """
        ...

    def on_stop(self):
        pass

    # ── 포지션 관리 헬퍼 ────────────────────────────────────────────────────
    def has_position(self, ticker: str) -> bool:
        return ticker in self.positions and self.positions[ticker].quantity > 0

    def open_position(self, ticker: str, quantity: int, price: float, date: str):
        self.positions[ticker] = Position(ticker, quantity, price, date)

    def close_position(self, ticker: str):
        self.positions.pop(ticker, None)
