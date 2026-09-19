"""
체결 비용 정의 — 라이브 원장과 한 소스.

라이브 원장 `Quant/src/risk/OrderGate.cpp`의 상수(14-15행)와 `on_fill_confirmed`(998행~)의
수식을 그대로 옮긴 파이썬 판이다. 백테스트·스터디·비용 집계 스크립트는 여기서 `LIVE`를 가져다 쓰고,
자기 상수를 따로 두지 않는다. 골든 테스트 `PYQuant/tests/test_costs_golden.py`가 이 파일의
상수를 C++ 소스에서 다시 읽어 대조한다 — 어느 한쪽만 고치면 테스트가 잡는다.

[formula] 원 단위 반올림은 하지 않는다. C++ 원장이 `price * quantity * kCommissionRate`를
double 그대로 들고 가고(OrderGate.cpp 1003-1004행) 어디에서도 내림·반올림을 하지 않으므로
여기서도 같은 순서의 부동소수 곱을 그대로 둔다. 곱셈 순서(가격 × 수량 × 요율)까지 맞춰야
비트 단위로 같은 값이 나온다.

호가 단위 함수는 C++ 소스에 없어(2026-09-19 `Quant/src` grep) KRX 2023-01-25 개편 규칙을 직접 적는다.
"""
from __future__ import annotations

from dataclasses import dataclass

# KRX 호가 단위(2023-01-25 개편, 주권 기준. ETF·ETN·ELW는 5원 균일이라 이 표를 쓰지 않는다).
#  (가격 상한 미만, 호가 단위) 순서대로 첫 번째 맞는 칸을 쓴다.
_TICK_TABLE: tuple[tuple[float, int], ...] = (
    (2_000, 1),
    (5_000, 5),
    (20_000, 10),
    (50_000, 50),
    (200_000, 100),
    (500_000, 500),
)
_TICK_ABOVE_TABLE = 1_000


def tick_size(price: float) -> int:
    """가격이 속한 구간의 호가 단위(원)."""
    for upper_bound, tick in _TICK_TABLE:
        if price < upper_bound:
            return tick

    return _TICK_ABOVE_TABLE


@dataclass(frozen=True)
class CostSpec:
    """비용 가정 한 벌. 값은 퍼센트 단위(0.015 = 0.015%)."""
    commission_percent: float   # 위탁수수료. 매수·매도 양쪽
    sell_tax_percent: float     # 증권거래세(농특세 포함). 매도만
    slippage_ticks: int     # 호가 단위 몇 칸 불리하게 체결되는지(매수는 위로, 매도는 아래로)
    impact_percent: float       # 충격 비용. 체결금액 대비, 양쪽

    @property
    def commission_rate(self) -> float:
        return self.commission_percent / 100.0

    @property
    def sell_tax_rate(self) -> float:
        return self.sell_tax_percent / 100.0

    @property
    def impact_rate(self) -> float:
        return self.impact_percent / 100.0

    @property
    def buy_cost_rate(self) -> float:
        """매수 1원당 붙는 비용률(사이징에 쓴다). 슬리피지는 가격에 얹히므로 여기 없다."""
        return self.commission_rate + self.impact_rate

    @property
    def sell_cost_rate(self) -> float:
        """매도 1원당 떨어지는 비용률."""
        return self.commission_rate + self.sell_tax_rate + self.impact_rate

    @property
    def roundtrip_percent(self) -> float:
        """왕복 비용률(%). 금액 기반 지표(metrics.py)가 수익률에서 뺄 때 쓴다."""
        return (self.buy_cost_rate + self.sell_cost_rate) * 100.0


@dataclass(frozen=True)
class FillResult:
    """체결 한 건의 금액 분해. `net`은 현금 관점 — 매수는 나가는 돈, 매도는 들어오는 돈."""
    side: str            # "BUY" | "SELL"
    price: float         # 슬리피지를 얹은 실제 체결가
    quantity: int
    gross: float         # price × quantity
    commission: float
    tax: float
    impact: float
    net: float


def slipped_price(side: str, price: float, slippage_ticks: int) -> float:
    """호가 단위 몇 칸 불리한 가격. 칸 수 0이면 그대로."""
    if slippage_ticks <= 0:
        return price

    step = tick_size(price) * slippage_ticks
    return price + step if side == "BUY" else max(price - step, tick_size(price))


def fill_result(side: str, price: float, quantity: int, specification: CostSpec) -> FillResult:
    """체결 금액 분해. 수수료·세금 수식은 OrderGate.cpp 1003-1004행과 곱셈 순서까지 같다."""
    if side not in ("BUY", "SELL"):
        raise ValueError(f"side는 BUY|SELL: {side!r}")

    fill_price = slipped_price(side, price, specification.slippage_ticks)
    gross = fill_price * quantity
    commission = fill_price * quantity * specification.commission_rate
    tax = fill_price * quantity * specification.sell_tax_rate if side == "SELL" else 0.0
    impact = fill_price * quantity * specification.impact_rate

    if side == "BUY":
        net = gross + commission + impact
    else:
        net = gross - commission - tax - impact

    return FillResult(side, fill_price, quantity, gross, commission, tax, impact, net)


# 라이브 원장 값(OrderGate.cpp 14-15행). 매도세 0.18%는 원장 CSV `realized_pnl` 역산(같은 주문 안의
#  체결가가 다른 두 행에서 Δ주당손익/Δ가격 = 1 − 매도비용률, 334쌍 중앙값 0.1957%)으로 원장이 실제로
#  이 값을 쓰고 있음을 확인한 것이다. 증권사가 실제 떼는 세율의 독립 근거는 저장소에 없다.
LIVE = CostSpec(commission_percent=0.015, sell_tax_percent=0.18, slippage_ticks=0, impact_percent=0.0)
