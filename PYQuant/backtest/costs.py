"""
체결 비용 정의 — 라이브 원장과 한 소스.

라이브 원장 `Quant/src/risk/PositionLedger.cpp`의 상수(12-13행)와 `on_fill_confirmed`(660행~)의
수식을 그대로 옮긴 파이썬 판이다. 백테스트·스터디·비용 집계 스크립트는 여기서 `LIVE`를 가져다 쓰고,
자기 상수를 따로 두지 않는다. 골든 테스트 `PYQuant/tests/test_costs_golden.py`가 이 파일의
상수를 C++ 소스에서 다시 읽어 대조한다 — 어느 한쪽만 고치면 테스트가 잡는다.

[formula] 원 단위 반올림은 하지 않는다. C++ 원장이 `price * quantity * kCommissionRate`를
double 그대로 들고 가고(PositionLedger.cpp 665-666행) 어디에서도 내림·반올림을 하지 않으므로
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
    """체결 금액 분해. 수수료·세금 수식은 PositionLedger.cpp 665-666행과 곱셈 순서까지 같다."""
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


# 라이브 원장 값(PositionLedger.cpp 12-13행). 매도세 0.20%는 2026년 증권거래세(코스피 거래세 0.05%+농어촌특별세 0.15%,
#  코스닥 0.20%). 2026-09-21까지 엔진은 2024년 값 0.18%로 원장을 썼다(원장 CSV `realized_pnl` 역산 334쌍 중앙값
#  0.1957%로 확인) — 그 원장 행을 다시 계산할 때만 LEDGER_UNTIL_2026_09_21을 쓴다.
LIVE = CostSpec(commission_percent=0.015, sell_tax_percent=0.20, slippage_ticks=0, impact_percent=0.0)
LEDGER_UNTIL_2026_09_21 = CostSpec(commission_percent=0.015, sell_tax_percent=0.18, slippage_ticks=0, impact_percent=0.0)

# 스터디용 세 벌(리셋 2라운드 `research/RESET_2026-09-19_R2/fundamental-quant.md` §2-4). 스터디는 이 셋만 쓰고 `LIVE`를 직접 쓰지 않는다 —
#  LIVE는 슬리피지 0틱이라 원장 재계산에는 맞지만 앞으로의 체결을 낮게 잡는다. 판정은 MID, 세 벌 결과를 모두 적는다.
#  왕복 비용(%) = 수수료 양쪽 + 매도세 + 충격 양쪽 + 호가 단위 슬리피지(가격에 따라 0.02~0.1%). 만원대 주식 기준 대략 LOW 0.23 / MID 0.35 / HIGH 0.55.
RESEARCH_LOW = CostSpec(commission_percent=0.015, sell_tax_percent=0.20, slippage_ticks=0, impact_percent=0.0)
RESEARCH_MID = CostSpec(commission_percent=0.015, sell_tax_percent=0.20, slippage_ticks=1, impact_percent=0.05)
RESEARCH_HIGH = CostSpec(commission_percent=0.015, sell_tax_percent=0.20, slippage_ticks=2, impact_percent=0.12)
RESEARCH_BY_LEVEL = {"low": RESEARCH_LOW, "mid": RESEARCH_MID, "high": RESEARCH_HIGH}


def side_cost_percent_at_price(specification: CostSpec, side: str, price: float) -> float:
    """한쪽(매수 또는 매도) 체결금액 대비 비용률(%) — 호가 단위 슬리피지를 그 가격의 틱으로 환산해 더한다.

    월별 리밸 스터디가 매수·매도 금액에 각각 곱한다. 매도에만 거래세가 붙는다.
    """
    if side not in ("BUY", "SELL"):
        raise ValueError(f"side는 BUY|SELL: {side!r}")

    tick_percent = tick_size(price) * specification.slippage_ticks / price * 100.0 if price > 0 else 0.0
    base = specification.buy_cost_rate if side == "BUY" else specification.sell_cost_rate
    return base * 100.0 + tick_percent


def roundtrip_percent_at_price(specification: CostSpec, price: float) -> float:
    """가격을 알 때의 왕복 비용률(%) = 매수쪽 + 매도쪽."""
    return side_cost_percent_at_price(specification, "BUY", price) + side_cost_percent_at_price(specification, "SELL", price)
