"""
평단 원장 — 라이브 `OrderGate::on_fill_confirmed`(Quant/src/risk/OrderGate.cpp 998행~)와 같은 규칙.

- 매수: 평단 = (기존수량 × 기존평단 + 체결수량 × 체결가) / 새수량 (1057-1058행).
  매수 수수료는 평단에 얹지 않고 발생 즉시 누적 실현손익에서 뺀다(1126-1128행).
- 매도: 실현손익 = (체결가 − 평단) × 수량 − 매도 수수료 − 거래세 (1096-1097행). 부분 매도는 평단
  유지(1100행), 전량 매도는 평단·수량 0으로 리셋(1104-1106행). 보유 초과 매도는 0으로 클램프(1082행)
  하되 손익은 요청 수량 그대로 계산한다(C++와 같다). 평단을 모르면 손익 0 + basis_unknown(1089-1093행).

[formula] 반올림 없음 — costs.py 머리말 참조.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from backtest.costs import CostSpec, FillResult, fill_result


@dataclass
class Position:
    quantity: int = 0
    average_price: float = 0.0


@dataclass(frozen=True)
class LedgerFill:
    """체결 한 건이 원장에 남긴 것."""
    fill: FillResult
    average_price: float      # 체결 후 평단(매도는 불변, 전량 매도면 청산 직전 평단)
    net_quantity: int         # 체결 후 보유 수량
    realized_pnl: float       # 이 체결로 확정된 손익(매도만, 비용 후). 매수는 0
    basis_unknown: bool = False


@dataclass
class PositionLedger:
    positions: dict[str, Position] = field(default_factory=dict)
    realized_pnl: float = 0.0     # 누적. OrderGate의 daily_pnl_ 적립분과 같은 규칙(매수 수수료 즉시 차감)

    def quantity(self, ticker: str) -> int:
        position = self.positions.get(ticker)
        return position.quantity if position is not None else 0

    def average_price(self, ticker: str) -> float:
        position = self.positions.get(ticker)
        return position.average_price if position is not None else 0.0

    def holdings(self) -> dict[str, int]:
        """ticker → 수량(0 초과만)."""
        return {ticker: position.quantity for ticker, position in self.positions.items() if position.quantity > 0}

    def on_fill(self, ticker: str, side: str, price: float, quantity: int, specification: CostSpec) -> LedgerFill:
        fill = fill_result(side, price, quantity, specification)
        position = self.positions.get(ticker)
        pre_quantity = position.quantity if position is not None else 0
        current_average = position.average_price if position is not None else 0.0

        if side == "BUY":
            new_quantity = pre_quantity + quantity
            new_average = ((pre_quantity * current_average + quantity * fill.price) / new_quantity
                           if new_quantity > 0 else fill.price)
            self.positions[ticker] = Position(new_quantity, new_average)
            # 매수 비용은 발생 즉시 인식(OrderGate.cpp 1128행). 충격 비용도 같은 자리에서 뺀다(C++는 항상 0).
            self.realized_pnl -= fill.commission + fill.impact
            return LedgerFill(fill, new_average, new_quantity, 0.0)

        new_quantity = max(pre_quantity - quantity, 0)

        if position is None or current_average <= 0.0:
            basis_unknown = True
            realized = 0.0
        else:
            basis_unknown = False
            realized = (fill.price - current_average) * quantity - fill.commission - fill.tax - fill.impact
            self.realized_pnl += realized

        if new_quantity == 0:
            self.positions.pop(ticker, None)
        else:
            position.quantity = new_quantity

        return LedgerFill(fill, current_average, new_quantity, realized, basis_unknown)
