"""
STEP C/D/E 검증 — strategy/strategy_a.py pytest

실행 방법 (PYQuant/ 디렉토리에서):
    python -m pytest tests/test_strategy_a.py -v

픽스처 설계:
  _aligned_60(last_close): 정배열 60봉, 마지막 봉 close를 조정 가능.
  closes = [100, 101, ..., 159] (기본) → last_close로 마지막값 교체.
  SMA5 기준봉(기본): mean(155,156,157,158,159) = 157.0
  last_close를 교체할 경우: SMA5 = (155+156+157+158+last_close)/5 = (626+last_close)/5
"""
import pytest
from kis.client import Bar
from strategy.strategy_a import StrategyA

TICKER = "005930"


def _bars(closes: list[float]) -> list[Bar]:
    return [Bar(date=f"2024-{(i//28)+1:02d}-{(i%28)+1:02d}",
                open=c, high=c*1.01, low=c*0.99, close=c, volume=1000)
            for i, c in enumerate(closes)]


def _aligned_60(last_close: float | None = None) -> list[Bar]:
    """정배열(단조증가) 60봉. last_close로 마지막 봉 종가 교체 가능."""
    closes = list(range(100, 160))  # 100..159, 60개
    if last_close is not None:
        closes[-1] = last_close
    return _bars(closes)


def _sma5_of(last_close: float) -> float:
    """_aligned_60(last_close) 기준 SMA5. bars[-5:] = 155,156,157,158,last_close"""
    return (155 + 156 + 157 + 158 + last_close) / 5


def _strategy(ticker: str = TICKER) -> StrategyA:
    return StrategyA(tickers=[ticker], z=1.0, a=7.0, b=2.0, quantity=1)


# ── STEP C: C2(정배열) + C3.5(5일선 위) 필터 ────────────────────────────────

class TestC2C35Filter:
    def test_not_aligned_blocks_entry(self):
        """C2 미통과(단조감소) → None"""
        s = _strategy()
        bars = _bars(list(range(159, 99, -1)))  # 60봉 감소
        assert s.on_data(TICKER, bars) is None

    def test_insufficient_bars_blocks_entry(self):
        """59봉 이하 → None (C2 판정 불가)"""
        s = _strategy()
        bars = _aligned_60()[:59]
        assert s.on_data(TICKER, bars) is None

    def test_close_below_sma5_blocks_entry(self):
        """C3.5: 종가 < SMA5 → None"""
        s = _strategy()
        # last_close = 152.0 → SMA5 = (155+156+157+158+152)/5 = 155.6
        # close (152) < SMA5 (155.6) → E2 실패
        bars = _aligned_60(last_close=152.0)
        assert s.on_data(TICKER, bars) is None

    def test_not_in_universe_blocks_entry(self):
        """E1: 유니버스 외 종목 → None"""
        s = _strategy(ticker=TICKER)
        bars = _aligned_60(last_close=157.5)
        assert s.on_data("999999", bars) is None


# ── STEP D: 진입 조건 E1~E3 ─────────────────────────────────────────────────

class TestCheckEntry:
    def test_entry_within_z(self):
        """이격도 0.5% (< Z=1.0%) + 정배열 + 5일선 위 → BUY"""
        s = _strategy()
        # last_close=157.5: SMA5=(626+157.5)/5=156.7, dev≈0.51%
        bars = _aligned_60(last_close=157.5)
        sig = s.on_data(TICKER, bars)
        assert sig is not None
        assert sig.side == "BUY"
        assert sig.ticker == TICKER

    def test_entry_at_sma5(self):
        """이격도 ≈0% (close ≈ SMA5) → BUY"""
        s = _strategy()
        # last_close=157.0: dev ≈ 0.26% (< Z=1%)
        bars = _aligned_60(last_close=157.0)
        sig = s.on_data(TICKER, bars)
        assert sig is not None and sig.side == "BUY"

    def test_no_entry_above_z(self):
        """이격도 > Z=1.0% → None"""
        s = _strategy()
        # last_close=160.0: SMA5=157.2, dev≈1.78%
        bars = _aligned_60(last_close=160.0)
        assert s.on_data(TICKER, bars) is None

    def test_no_entry_when_position_held(self):
        """이미 포지션 보유 → _check_exit만 호출, _check_entry 미호출"""
        s = _strategy()
        s.open_position(TICKER, 1, 100.0, "2024-01-01")
        # 진입 조건 만족하는 bars를 줘도 BUY 나오면 안 됨
        bars = _aligned_60(last_close=157.5)
        sig = s.on_data(TICKER, bars)
        # hold 구간(dev<A, close>SMA5*(1-B)) → None (청산도 미발생)
        assert sig is None

    def test_open_position_recorded(self):
        """진입 시그널 후 포지션 기록됨"""
        s = _strategy()
        bars = _aligned_60(last_close=157.5)
        s.on_data(TICKER, bars)
        assert s.has_position(TICKER)

    def test_quantity_passed_through(self):
        """quantity=3 설정 시 신호도 3주"""
        s = StrategyA(tickers=[TICKER], quantity=3)
        bars = _aligned_60(last_close=157.5)
        sig = s.on_data(TICKER, bars)
        assert sig is not None and sig.quantity == 3


# ── STEP E: 청산 조건 X1/X2 ─────────────────────────────────────────────────

class TestCheckExit:
    def _with_position(self) -> StrategyA:
        s = _strategy()
        s.open_position(TICKER, 1, 100.0, "2024-01-01")
        return s

    def test_exit_overextended_x1(self):
        """X1: 이격도 >= A=7% → SELL"""
        s = self._with_position()
        # last_close=171: SMA5=159.4, dev≈7.28%
        bars = _aligned_60(last_close=171.0)
        sig = s.on_data(TICKER, bars)
        assert sig is not None and sig.side == "SELL"

    def test_exit_stop_loss_x2(self):
        """X2: close < SMA5 * (1-B/100=0.98) → SELL"""
        s = self._with_position()
        # last_close=152: SMA5=155.6, threshold=152.49 → 152 < 152.49
        bars = _aligned_60(last_close=152.0)
        sig = s.on_data(TICKER, bars)
        assert sig is not None and sig.side == "SELL"

    def test_hold_within_range(self):
        """이격도 < A%, close > SMA5*(1-B%) → 청산 없음(None)"""
        s = self._with_position()
        # last_close=160: SMA5=157.2, dev≈1.78% (<7%), threshold≈154.1 (160>154.1)
        bars = _aligned_60(last_close=160.0)
        assert s.on_data(TICKER, bars) is None

    def test_position_cleared_after_x1(self):
        """X1 청산 후 포지션 해제됨"""
        s = self._with_position()
        bars = _aligned_60(last_close=171.0)
        s.on_data(TICKER, bars)
        assert not s.has_position(TICKER)

    def test_position_cleared_after_x2(self):
        """X2 청산 후 포지션 해제됨"""
        s = self._with_position()
        bars = _aligned_60(last_close=152.0)
        s.on_data(TICKER, bars)
        assert not s.has_position(TICKER)

    def test_insufficient_bars_no_exit(self):
        """봉 4개 이하 → 청산 불가(None)"""
        s = self._with_position()
        bars = _bars([100.0, 200.0, 300.0, 400.0])
        assert s.on_data(TICKER, bars) is None


# ── 진입/청산 상호배타 ────────────────────────────────────────────────────────

class TestMutualExclusive:
    def test_entry_only_when_no_position(self):
        """미보유 + 진입 조건 → _check_entry만 평가"""
        s = _strategy()
        assert not s.has_position(TICKER)
        bars = _aligned_60(last_close=157.5)
        sig = s.on_data(TICKER, bars)
        assert sig is not None and sig.side == "BUY"

    def test_exit_only_when_position_held(self):
        """보유 중 + X1 조건 → _check_exit만 평가, BUY 불가"""
        s = _strategy()
        s.open_position(TICKER, 1, 100.0, "2024-01-01")
        # X1 조건 bars
        bars = _aligned_60(last_close=171.0)
        sig = s.on_data(TICKER, bars)
        # SELL이어야 하고 BUY가 아님
        assert sig is not None and sig.side == "SELL"

    def test_no_buy_after_sell(self):
        """청산 후 미보유 → 다음 on_data에서 진입 가능"""
        s = _strategy()
        s.open_position(TICKER, 1, 100.0, "2024-01-01")
        bars_exit = _aligned_60(last_close=171.0)
        s.on_data(TICKER, bars_exit)              # X1 청산
        assert not s.has_position(TICKER)
        bars_entry = _aligned_60(last_close=157.5)
        sig = s.on_data(TICKER, bars_entry)       # 재진입 가능
        assert sig is not None and sig.side == "BUY"
