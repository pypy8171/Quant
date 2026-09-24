"""
비용·평단 골든 테스트 — 파이썬 `backtest.costs`·`backtest.ledger`가 C++ `PositionLedger::on_fill_confirmed`와
같은 원을 내는지 본다. 오차 허용 0원(부동소수 잔차 1e-9 이하).

기대값 근거:
  - 케이스 1·2 평단 1000 → 1050: `Quant/tests/test_position_ledger.cpp` 49-68행(test_partial_fill_average_price).
  - 나머지: `Quant/src/risk/PositionLedger.cpp` 수식을 손으로 푼 값. 수수료 = 가격×수량×0.00015(665행),
    거래세 = 매도 가격×수량×0.0020(666행), 매수 평단 (724-725행), 매도 실현손익 (763-764행),
    부분 매도 평단 불변(767행), 전량 매도 리셋(772-775행), 초과 매도 0 클램프(751행),
    평단 미상 손익 0(756-760행), 매수 수수료 즉시 차감(802행).
  - 케이스 11: 원장 CSV `Quant/build_win/logs/trades_20260910.csv` ORD-000033(ITB_441270) 실제 행 — 그날 엔진 세율 0.18%(LEDGER_UNTIL_2026_09_21).
    8200원 1주 → realized_pnl 319.13, 8190원 1주 → 309.15. 두 행에서 평단 7864.88이 나온다.

실행: py -m pytest PYQuant/tests/test_costs_golden.py -q   (pytest 없으면 py PYQuant/tests/test_costs_golden.py)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/

from backtest.costs import LEDGER_UNTIL_2026_09_21, LIVE, CostSpec, fill_result, tick_size  # noqa: E402
from backtest.ledger import PositionLedger  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
ORDER_GATE_SOURCE = REPO / "Quant" / "src" / "risk" / "PositionLedger.cpp"
ZERO_WON = 1e-9


def assert_won(actual: float, expected: float, label: str, tolerance: float = ZERO_WON) -> None:
    assert abs(actual - expected) <= tolerance, f"{label}: {actual!r} != {expected!r}"


def test_constants_match_cpp_source():
    """상수를 C++ 소스에서 다시 읽어 대조 — 한쪽만 고치면 여기서 잡힌다."""
    source = ORDER_GATE_SOURCE.read_text(encoding="utf-8")
    commission = float(re.search(r"kCommissionRate\s*=\s*([0-9.]+)", source).group(1))
    sell_tax = float(re.search(r"kSellTaxRate\s*=\s*([0-9.]+)", source).group(1))
    assert LIVE.commission_rate == commission, (LIVE.commission_rate, commission)
    assert LIVE.sell_tax_rate == sell_tax, (LIVE.sell_tax_rate, sell_tax)
    assert LIVE.slippage_ticks == 0 and LIVE.impact_percent == 0.0


def test_tick_size_table():
    expected = {1_999: 1, 2_000: 5, 4_999: 5, 5_000: 10, 19_999: 10, 20_000: 50, 49_999: 50,
                50_000: 100, 199_999: 100, 200_000: 500, 499_999: 500, 500_000: 1_000, 1_000_000: 1_000}
    for price, tick in expected.items():
        assert tick_size(price) == tick, (price, tick_size(price), tick)


def test_golden_sequence_buy_partial_sell_add_full_sell():
    """케이스 1~5: 매수 → 추가 매수 → 부분 매도 → 추가 매수 → 전량 매도. 누적 실현손익까지."""
    ledger = PositionLedger()

    # 1. BUY 5 @1000 → 평단 1000, 수수료 5000×0.00015 = 0.75 (C++ 테스트 237-239행)
    fill_1 = ledger.on_fill("005930", "BUY", 1000.0, 5, LIVE)
    assert_won(fill_1.average_price, 1000.0, "케이스1 평단")
    assert fill_1.net_quantity == 5
    assert_won(fill_1.fill.commission, 0.75, "케이스1 수수료")
    assert_won(fill_1.fill.tax, 0.0, "케이스1 세금")
    assert_won(fill_1.fill.net, 5000.75, "케이스1 매수 총지출")

    # 2. BUY 5 @1100 → 평단 (5×1000 + 5×1100)/10 = 1050 (C++ 테스트 243-245행), 수수료 0.825
    fill_2 = ledger.on_fill("005930", "BUY", 1100.0, 5, LIVE)
    assert_won(fill_2.average_price, 1050.0, "케이스2 평단")
    assert fill_2.net_quantity == 10
    assert_won(fill_2.fill.commission, 0.825, "케이스2 수수료")

    # 3. SELL 4 @1200 부분 매도 → (1200−1050)×4 − 4800×0.00015 − 4800×0.0020 = 600 − 0.72 − 9.6 = 589.68
    fill_3 = ledger.on_fill("005930", "SELL", 1200.0, 4, LIVE)
    assert_won(fill_3.realized_pnl, 589.68, "케이스3 실현손익")
    assert_won(fill_3.average_price, 1050.0, "케이스3 평단 유지")
    assert fill_3.net_quantity == 6
    assert_won(fill_3.fill.tax, 9.6, "케이스3 세금")
    assert_won(fill_3.fill.net, 4800.0 - 0.72 - 9.6, "케이스3 실수령")
    assert_won(ledger.average_price("005930"), 1050.0, "케이스3 원장 평단")

    # 4. BUY 4 @900 → 평단 (6×1050 + 4×900)/10 = 990, 수수료 3600×0.00015 = 0.54
    fill_4 = ledger.on_fill("005930", "BUY", 900.0, 4, LIVE)
    assert_won(fill_4.average_price, 990.0, "케이스4 평단")
    assert fill_4.net_quantity == 10
    assert_won(fill_4.fill.commission, 0.54, "케이스4 수수료")

    # 5. SELL 10 @1000 전량 → (1000−990)×10 − 1.5 − 20 = 78.5, 평단·수량 리셋
    fill_5 = ledger.on_fill("005930", "SELL", 1000.0, 10, LIVE)
    assert_won(fill_5.realized_pnl, 78.5, "케이스5 실현손익")
    assert fill_5.net_quantity == 0
    assert ledger.quantity("005930") == 0
    assert_won(ledger.average_price("005930"), 0.0, "케이스5 평단 리셋")

    # 누적 = −0.75 −0.825 +589.68 −0.54 +78.5 = 666.065 (매수 수수료 즉시 차감, 1128행)
    assert_won(ledger.realized_pnl, 666.065, "케이스1~5 누적 실현손익")


def test_golden_low_price_one_won_tick():
    """케이스 6: 저가주(호가 1원). BUY 1000 @1234 → 수수료 185.1. SELL 1000 @1235 → 1000 − 185.25 − 2470 = −1655.25"""
    ledger = PositionLedger()
    buy = ledger.on_fill("000001", "BUY", 1234.0, 1000, LIVE)
    assert_won(buy.fill.commission, 185.1, "케이스6 매수 수수료")
    assert tick_size(1234.0) == 1
    sell = ledger.on_fill("000001", "SELL", 1235.0, 1000, LIVE)
    assert_won(sell.fill.commission, 185.25, "케이스6 매도 수수료")
    assert_won(sell.fill.tax, 2470.0, "케이스6 세금")
    assert_won(sell.realized_pnl, -1655.25, "케이스6 실현손익")


def test_golden_high_price_single_share():
    """케이스 7: 고가주 1주. BUY 1 @1,000,000 → 수수료 150. SELL 1 @1,010,000 → 10000 − 151.5 − 2020 = 7828.5"""
    ledger = PositionLedger()
    buy = ledger.on_fill("000002", "BUY", 1_000_000.0, 1, LIVE)
    assert_won(buy.fill.commission, 150.0, "케이스7 매수 수수료")
    assert tick_size(1_000_000.0) == 1_000
    sell = ledger.on_fill("000002", "SELL", 1_010_000.0, 1, LIVE)
    assert_won(sell.realized_pnl, 7828.5, "케이스7 실현손익")
    assert_won(ledger.realized_pnl, 7828.5 - 150.0, "케이스7 누적")


def test_golden_one_share_small_gain():
    """케이스 8: 1주 8000 → 8200. 200 − 8200×0.00015(1.23) − 8200×0.0020(16.4) = 182.37"""
    ledger = PositionLedger()
    ledger.on_fill("000003", "BUY", 8000.0, 1, LIVE)
    sell = ledger.on_fill("000003", "SELL", 8200.0, 1, LIVE)
    assert_won(sell.realized_pnl, 182.37, "케이스8 실현손익")


def test_golden_sell_without_basis():
    """케이스 9: 평단 미상 매도(1089-1093행). 손익 0 + basis_unknown, 비용은 그대로 계산."""
    ledger = PositionLedger()
    sell = ledger.on_fill("000004", "SELL", 5000.0, 3, LIVE)
    assert sell.basis_unknown
    assert_won(sell.realized_pnl, 0.0, "케이스9 실현손익")
    assert_won(sell.fill.commission, 2.25, "케이스9 수수료")
    assert_won(sell.fill.tax, 30.0, "케이스9 세금")
    assert_won(ledger.realized_pnl, 0.0, "케이스9 누적(평단 미상은 적립 안 함)")


def test_golden_oversell_clamps_to_zero():
    """케이스 10: 보유 2주에 5주 매도(1082행). 수량은 0으로 클램프, 손익은 요청 수량 5주로 계산(C++와 같다).
    (10100−10000)×5 − 50500×0.00015(7.575) − 50500×0.0020(101) = 391.425"""
    ledger = PositionLedger()
    ledger.on_fill("000005", "BUY", 10_000.0, 2, LIVE)
    sell = ledger.on_fill("000005", "SELL", 10_100.0, 5, LIVE)
    assert sell.net_quantity == 0
    assert ledger.quantity("000005") == 0
    assert_won(sell.realized_pnl, 391.425, "케이스10 실현손익")


def test_golden_ledger_csv_row_2026_09_10():
    """케이스 11: 원장 실제 행(엔진 세율 0.18%이던 날). 평단 7864.88에서 1주 8200 매도 → 319.13, 1주 8190 매도 → 309.15 (CSV는 소수 2자리)."""
    ledger = PositionLedger()
    ledger.on_fill("441270", "BUY", 7864.88, 2, LEDGER_UNTIL_2026_09_21)
    sell_8200 = ledger.on_fill("441270", "SELL", 8200.0, 1, LEDGER_UNTIL_2026_09_21)
    sell_8190 = ledger.on_fill("441270", "SELL", 8190.0, 1, LEDGER_UNTIL_2026_09_21)
    assert_won(sell_8200.realized_pnl, 319.13, "케이스11 8200원 행", tolerance=0.005)
    assert_won(sell_8190.realized_pnl, 309.15, "케이스11 8190원 행", tolerance=0.005)


def test_fill_result_slippage_and_impact():
    """LIVE 밖 가정: 슬리피지 1칸·충격 0.05%. 매수는 위 칸, 매도는 아래 칸."""
    specification = CostSpec(commission_percent=0.015, sell_tax_percent=0.20, slippage_ticks=1, impact_percent=0.05)
    buy = fill_result("BUY", 10_000.0, 10, specification)
    assert_won(buy.price, 10_010.0, "슬리피지 매수가")
    assert_won(buy.impact, 100_100.0 * 0.0005, "충격 비용")
    sell = fill_result("SELL", 10_000.0, 10, specification)
    assert_won(sell.price, 9_990.0, "슬리피지 매도가")
    assert_won(specification.roundtrip_percent, (0.00015 + 0.0005) * 100 + (0.00015 + 0.0020 + 0.0005) * 100, "왕복 %")


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_") and callable(value)]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"{len(tests)} tests passed")
