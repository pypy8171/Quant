"""
DataGoKrSource._adjust_splits 회귀 테스트 — 수정주가 보정의 오인 차단(C-1).

분할(가격갭+주식수변동)은 보정하고, 거래정지 재개·단발 이상치 갭(주식수 불변)은
건드리지 않아야 한다. 횡단면 모멘텀 점수가 가격비라 이 오인이 결과를 직접 오염시킴.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from data.datagokr_source import DataGoKrSource


def _row(d, c, shares, o=None):
    o = c if o is None else o
    return {"date": d, "o": o, "h": c, "l": c, "c": c, "v": 100, "shares": shares}


class TestAdjustSplits(unittest.TestCase):
    def test_real_split_with_share_change(self):
        """5:1 분할(가격 1/5 + 주식수 5배) → 과거가 보정되어 연속."""
        rows = [_row(f"2023-01-{i+1:02d}", 50000, 1000) for i in range(5)]
        # 분할일: 시가 갭 10000(전일 50000 대비 0.2), 주식수 5배
        rows += [_row(f"2023-02-{i+1:02d}", 10000, 5000, o=10000) for i in range(5)]
        bars = DataGoKrSource._adjust_splits(rows)
        self.assertAlmostEqual(bars[4].close, bars[5].close, delta=1)  # 연속
        self.assertAlmostEqual(bars[0].close, 10000, delta=1)          # 분할전 ×0.2

    def test_halt_resumption_no_share_change(self):
        """거래정지 후 -50% 재개(주식수 불변) → 분할 아님, 보정 안 함(폭락 보존)."""
        rows = [_row(f"2023-01-{i+1:02d}", 50000, 1000) for i in range(5)]
        rows += [_row(f"2023-02-{i+1:02d}", 25000, 1000, o=25000) for i in range(5)]
        bars = DataGoKrSource._adjust_splits(rows)
        self.assertAlmostEqual(bars[0].close, 50000, delta=1)  # 과거가 그대로(보정 X)
        self.assertAlmostEqual(bars[-1].close, 25000, delta=1)

    def test_single_day_spike_no_share_change(self):
        """단발 +50% 갭 후 복귀(주식수 불변) → 보정 안 함(가짜 분할 아님)."""
        rows = [_row(f"d{i:02d}", 100, 1000) for i in range(5)]
        rows += [_row("d05", 150, 1000, o=150)]   # +50% 갭, 주식수 불변
        rows += [_row("d06", 100, 1000, o=100)]   # 복귀
        bars = DataGoKrSource._adjust_splits(rows)
        self.assertAlmostEqual(bars[0].close, 100, delta=1)  # 과거가 안 부풀려짐

    def test_normal_move_within_limit(self):
        """가격제한 내 +29% 정상 등락 → 보정 안 함."""
        rows = [_row(f"d{i:02d}", 100, 1000) for i in range(3)]
        rows += [_row("d03", 129, 1000, o=129)]
        bars = DataGoKrSource._adjust_splits(rows)
        self.assertAlmostEqual(bars[-1].close, 129, delta=1)
        self.assertAlmostEqual(bars[0].close, 100, delta=1)


if __name__ == "__main__":
    unittest.main()
