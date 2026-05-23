"""
전략 공통 기술적 지표 헬퍼 — 정배열·이격도 계산.

Bar 타입은 kis.client.Bar (date, open, high, low, close, volume).
모든 함수는 봉 수 부족 시 None을 반환하며 예외를 던지지 않는다.
"""
from typing import Optional
from kis.client import Bar


def sma(bars: list[Bar], period: int) -> Optional[float]:
    """최근 period개 봉의 단순이동평균 종가. 봉 수 부족 시 None."""
    if len(bars) < period:
        return None
    return sum(b.close for b in bars[-period:]) / period


def is_aligned(bars: list[Bar]) -> bool:
    """SMA5 > SMA10 > SMA20 > SMA60 정배열 여부. bars 60개 이상 필요."""
    s5  = sma(bars, 5)
    s10 = sma(bars, 10)
    s20 = sma(bars, 20)
    s60 = sma(bars, 60)
    if None in (s5, s10, s20, s60):
        return False
    return s5 > s10 > s20 > s60


def deviation_from_sma(bars: list[Bar], period: int) -> Optional[float]:
    """이격도(%) = (최근 종가 - SMAn) / SMAn * 100. 봉 수 부족 시 None."""
    avg = sma(bars, period)
    if avg is None or avg == 0:
        return None
    return (bars[-1].close - avg) / avg * 100
