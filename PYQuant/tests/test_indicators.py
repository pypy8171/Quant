"""
STEP A 검증 — strategy/indicators.py pytest

실행 방법 (PYQuant/ 디렉토리에서):
    python -m pytest tests/test_indicators.py -v
"""
import pytest
from kis.client import Bar
from strategy.indicators import sma, is_aligned, deviation_from_sma


def _bars(closes: list[float]) -> list[Bar]:
    return [Bar(date=f"2024-01-{i+1:02d}", open=c, high=c, low=c, close=c, volume=1)
            for i, c in enumerate(closes)]


# ── sma ────────────────────────────────────────────────────────────────────────

class TestSma:
    def test_exact(self):
        bars = _bars([1, 2, 3, 4, 5])
        assert sma(bars, 3) == pytest.approx(4.0)  # (3+4+5)/3

    def test_full_window(self):
        bars = _bars([10, 20, 30])
        assert sma(bars, 3) == pytest.approx(20.0)

    def test_uses_last_n(self):
        # 5개 봉 중 마지막 2개 평균
        bars = _bars([100, 200, 300, 10, 20])
        assert sma(bars, 2) == pytest.approx(15.0)

    def test_insufficient_returns_none(self):
        bars = _bars([1, 2])
        assert sma(bars, 5) is None

    def test_single_bar(self):
        bars = _bars([42.0])
        assert sma(bars, 1) == pytest.approx(42.0)

    def test_empty_returns_none(self):
        assert sma([], 5) is None


# ── is_aligned ─────────────────────────────────────────────────────────────────

def _aligned_bars() -> list[Bar]:
    """SMA5 > SMA10 > SMA20 > SMA60 을 만족하는 단조증가 시계열."""
    # 60개 봉을 1, 2, ..., 60으로 구성하면
    # SMA5=mean(56..60)=58, SMA10=mean(51..60)=55.5, SMA20=mean(41..60)=50.5, SMA60=mean(1..60)=30.5
    return _bars(list(range(1, 61)))


def _misaligned_bars() -> list[Bar]:
    """정배열 아님 — 60개지만 단조감소."""
    return _bars(list(range(60, 0, -1)))


class TestIsAligned:
    def test_true_for_uptrend(self):
        assert is_aligned(_aligned_bars()) is True

    def test_false_for_downtrend(self):
        assert is_aligned(_misaligned_bars()) is False

    def test_false_when_insufficient_bars(self):
        bars = _bars([10] * 59)  # 60 미만
        assert is_aligned(bars) is False

    def test_false_when_sma5_equals_sma10(self):
        # 모두 같은 값이면 SMA5 == SMA10 → 정배열 아님 (strict >)
        bars = _bars([100.0] * 60)
        assert is_aligned(bars) is False

    def test_border_60_bars(self):
        # 정확히 60개 — 계산 가능해야 함
        bars = _aligned_bars()
        assert len(bars) == 60
        result = is_aligned(bars)
        assert isinstance(result, bool)


# ── deviation_from_sma ─────────────────────────────────────────────────────────

class TestDeviationFromSma:
    def test_above_sma(self):
        # close=110, SMA5=100 → 이격도 +10%
        bars = _bars([100, 100, 100, 100, 110])
        dev = deviation_from_sma(bars, 5)
        # SMA5 = (100+100+100+100+110)/5 = 102
        # deviation = (110 - 102) / 102 * 100 ≈ 7.84%
        assert dev is not None
        assert dev > 0

    def test_below_sma(self):
        # 단조감소: close<SMA → 음수
        bars = _bars([100, 90, 80, 70, 60])
        dev = deviation_from_sma(bars, 5)
        assert dev is not None
        assert dev < 0

    def test_at_sma(self):
        # 모두 같으면 SMA=close → 이격도 0
        bars = _bars([50.0] * 5)
        assert deviation_from_sma(bars, 5) == pytest.approx(0.0)

    def test_insufficient_returns_none(self):
        bars = _bars([100, 200])
        assert deviation_from_sma(bars, 5) is None

    def test_formula(self):
        # 직접 계산값과 비교
        closes = [10.0, 20.0, 30.0]
        bars = _bars(closes)
        avg = sum(closes) / 3  # 20.0
        expected = (closes[-1] - avg) / avg * 100  # (30-20)/20*100 = 50%
        result = deviation_from_sma(bars, 3)
        assert result == pytest.approx(expected)
