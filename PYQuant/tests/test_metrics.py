# -*- coding: utf-8 -*-
"""backtest.metrics.simulate 경로 시뮬레이션 검산 (KIS 불필요, 합성 봉).

게이트 평가 결론이 전부 이 함수 하나에 걸려 있어서, 손계산과 맞는지 확인한다.
검증 항목:
  1) 진입은 신호 다음날 시가, 만기 청산은 max_hold일 종가
  2) 손절은 손절선 체결, 시가가 이미 손절선 아래면 시가 체결(갭)
  3) 같은 날 목표가와 손절선에 모두 닿으면 손절을 먼저 잡는다(최악 가정)
  4) 트레일링은 최고 종가 기준
  5) 마지막 봉은 진입하지 않는다(다음날 시가가 없음)
  6) 왕복비용은 수익률에서 순수 차감

실행: python PYQuant/tests/test_metrics.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # PYQuant/

import numpy as np
import pandas as pd

import backtest.metrics as M
from backtest.metrics import ExitRule, ROUNDTRIP_COST_PCT as COST, simulate

FAILS = []


def bars(rows):
    idx = pd.date_range("2026-01-01", periods=len(rows), freq="D")
    return pd.DataFrame(rows, columns=["Open", "High", "Low", "Close"], index=idx)


def check(name, got, want, tol=1e-6):
    ok = (got is None and want is None) or (want is not None and abs(got - want) < tol)
    print(f"{'OK  ' if ok else 'FAIL'}  {name:52s} got={got}  want={want}")
    if not ok:
        FAILS.append(name)


def test_expiry_exit():
    """손절에 닿지 않으면 max_hold일 종가로 청산한다."""
    rule = ExitRule(stop_pct=6, max_hold=3)
    d = bars([[99, 100, 98, 99],
              [100, 102, 99, 101],
              [101, 105, 100, 104],
              [104, 112, 103, 110],
              [110, 111, 109, 110]])
    p = simulate(d, rule)
    check("기간만료: 진입가 = 다음날 시가", p.entry.iloc[0], 100.0)
    check("기간만료: 청산가 = D+3 종가", p.exit_price.iloc[0], 110.0)
    check("기간만료: 수익 = 10% - 비용", p.ret_pct.iloc[0], 10.0 - COST, 1e-9)
    check("기간만료: R = 수익/손절폭", p.r_mult.iloc[0], (10.0 - COST) / 6.0, 1e-9)
    check("기간만료: 보유일 3", float(p.hold_days.iloc[0]), 3.0)
    check("MFE = 최고가 112 대비 +12%", p.mfe_pct.iloc[0], 12.0, 1e-9)
    check("MAE = 최저가 99 대비 -1%", p.mae_pct.iloc[0], -1.0, 1e-9)


def test_stop_fill_at_stop_line():
    """저가가 손절선을 깨면 손절선에서 체결한다(시가는 손절선 위)."""
    rule = ExitRule(stop_pct=6, max_hold=3)
    d = bars([[99, 100, 98, 99],
              [100, 102, 99, 101],
              [101, 102, 93, 95],
              [95, 96, 94, 95],
              [95, 96, 94, 95]])
    p = simulate(d, rule)
    check("손절: 체결가 = 손절선 94", p.exit_price.iloc[0], 94.0)
    check("손절: 수익 = -6% - 비용", p.ret_pct.iloc[0], -6.0 - COST, 1e-9)
    check("손절: R = -1 - 비용/6", p.r_mult.iloc[0], (-6.0 - COST) / 6.0, 1e-9)
    check("손절: 보유일 2", float(p.hold_days.iloc[0]), 2.0)


def test_gap_down_fills_at_open():
    """시가가 이미 손절선 아래면 손절선이 아니라 시가로 체결한다."""
    rule = ExitRule(stop_pct=6, max_hold=3)
    d = bars([[99, 100, 98, 99],
              [100, 102, 99, 101],
              [90, 91, 88, 89],
              [89, 90, 88, 89],
              [89, 90, 88, 89]])
    p = simulate(d, rule)
    check("갭손절: 체결가 = 시가 90", p.exit_price.iloc[0], 90.0)
    check("갭손절: 수익 = -10% - 비용", p.ret_pct.iloc[0], -10.0 - COST, 1e-9)


def test_same_day_stop_wins_over_target():
    """일봉만으로는 도달 순서를 모른다. 손절을 먼저 잡아 낙관을 배제한다."""
    d = bars([[99, 100, 98, 99],
              [100, 102, 99, 101],
              [101, 130, 93, 120],
              [120, 130, 119, 125],
              [125, 130, 119, 125]])
    p = simulate(d, ExitRule(stop_pct=6, max_hold=3, target_r=2.0))
    check("동시 도달: 목표(112)가 아니라 손절(94)", p.exit_price.iloc[0], 94.0)


def test_trailing_from_peak_close():
    """트레일선은 최고 '종가' 기준. 진입 100, D+1 종가 120 → 트레일선 108."""
    d = bars([[99, 100, 98, 99],
              [100, 125, 99, 120],
              [119, 121, 105, 110],
              [110, 111, 109, 110],
              [110, 111, 109, 110]])
    p = simulate(d, ExitRule(stop_pct=6, trail_pct=10, max_hold=3))
    check("트레일: 체결가 = 최고종가120의 -10% = 108", p.exit_price.iloc[0], 108.0)
    check("마지막 봉 진입 없음",
          None if np.isnan(p.entry.iloc[-1]) else p.entry.iloc[-1], None)


def test_cost_is_pure_subtraction():
    """왕복비용은 경로와 무관하게 수익률에서 그대로 빠진다."""
    rule = ExitRule(stop_pct=6, max_hold=3)
    d = bars([[99, 100, 98, 99],
              [100, 102, 99, 101],
              [101, 105, 100, 104],
              [104, 112, 103, 110],
              [110, 111, 109, 110]])
    with_cost = simulate(d, rule).ret_pct.iloc[0]
    M.ROUNDTRIP_COST_PCT = 0.0
    try:
        no_cost = simulate(d, rule).ret_pct.iloc[0]
    finally:
        M.ROUNDTRIP_COST_PCT = COST
    check("비용 차감분 = 0.31%p", no_cost - with_cost, COST, 1e-9)


def main():
    for fn in (test_expiry_exit, test_stop_fill_at_stop_line, test_gap_down_fills_at_open,
               test_same_day_stop_wins_over_target, test_trailing_from_peak_close,
               test_cost_is_pure_subtraction):
        print(f"\n--- {fn.__name__} ---")
        fn()
    print("\n실패 " + (f"{len(FAILS)}건: {FAILS}" if FAILS else "0건 — 검산 통과"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
