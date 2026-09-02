"""regime_scorer 검증 — 변형 A의 C++ RegimeController 패리티 + 연속화/기울기/오버레이 성질.

실행 (PYQuant/ 디렉토리에서):
    python -m pytest tests/test_regime_scorer.py -v
"""
import math
import pytest
from backtest.regime_scorer import (
    sma, realized_vol, score_v0, classify_v0,
    score_continuous, classify_continuous, overlay_shift,
    BULL, NEUTRAL, BEAR,
)


def _uptrend(n=240, base=100.0, step=0.5):
    return [base + step * i for i in range(n)]


def _downtrend(n=240, base=220.0, step=0.5):
    return [base - step * i for i in range(n)]


def _up_then_pullback(nup=250, step=3.0, depth=6, plen=20):
    """가파른 장기상승 → 얕은 최근 풀백. ma200은 낮게 유지되어 close>ma200(+1)이나
    정배열은 깨진(ma20<ma60) 혼조 상태를 만든다 → score=+1."""
    base = [100.0 + step * i for i in range(nup)]
    return base + [base[-1] - depth * i for i in range(1, plen + 1)]


def _noisy(n=240, base=100.0, drift=0.15, amp=6.0):
    """추세+사인 노이즈. 완전선형(변동성≈0→tanh 포화)과 달리 연속 score가 중간값에 안착."""
    return [base + drift * i + amp * math.sin(i * 0.7) for i in range(n)]


# ── 변형 A: C++ compute_score/classify 패리티 ────────────────────────────────
class TestV0Parity:
    def test_clean_uptrend_is_bull(self):
        # 선형 상승 → close>ma200, ma20>ma60>ma120(정배열) → score=+2 → BULL
        c = _uptrend()
        assert score_v0(c) == 2
        assert classify_v0(score_v0(c)) == BULL

    def test_clean_downtrend_is_bear(self):
        c = _downtrend()
        assert score_v0(c) == -2
        assert classify_v0(score_v0(c)) == BEAR

    def test_above_ma200_but_not_aligned_is_neutral(self):
        # close>ma200(+1)이나 정배열 붕괴(0) → score=+1 → NEUTRAL
        s = score_v0(_up_then_pullback())
        assert s == 1, f"expected +1(above200·혼조), got {s}"
        assert classify_v0(s) == NEUTRAL

    def test_insufficient_sample_is_none_then_neutral(self):
        assert score_v0([100.0] * 50) is None       # <200봉 → 표본부족
        assert classify_v0(None) == NEUTRAL          # C++ cpp:70 안전판

    def test_nonpositive_guard(self):
        c = _uptrend()
        c[-1] = -1.0
        assert score_v0(c) is None                    # close<=0 방어


# ── 변형 B/C: 연속화 + 기울기 ────────────────────────────────────────────────
class TestContinuous:
    def test_continuous_sign_matches_trend(self):
        assert score_continuous(_uptrend(), with_slope=False) > 0
        assert score_continuous(_downtrend(), with_slope=False) < 0

    def test_continuous_is_bounded(self):
        # 두 축 tanh 합이라 |score| <= w_pos+w_aln = 2
        s = score_continuous(_uptrend(), with_slope=False)
        assert -2.0 <= s <= 2.0

    def test_not_discrete(self):
        # 이산 {-2..+2}가 아니라 연속: 노이즈 강도가 다르면 다른 중간 score
        low_noise = score_continuous(_noisy(amp=3.0), with_slope=False)
        high_noise = score_continuous(_noisy(amp=10.0), with_slope=False)
        assert low_noise != high_noise
        assert 0.0 < low_noise < 2.0 and 0.0 < high_noise < 2.0  # 포화 아닌 중간값

    def test_slope_axis_distinguishes_bounce_from_trend(self):
        # 하락추세 속 최근 반등: ma200은 아직 하락(음 기울기) → slope축이 순수상승보다 낮은 score
        downthenup = _downtrend(n=210) + [(_downtrend(n=210)[-1]) + 3.0 * i for i in range(30)]
        up = _uptrend(n=240)
        s_bounce = score_continuous(downthenup, with_slope=True)
        s_trend = score_continuous(up, with_slope=True)
        assert s_bounce < s_trend

    def test_classify_thresholds(self):
        assert classify_continuous(1.5, bull_th=1.0, bear_th=-1.0) == BULL
        assert classify_continuous(-1.5, bull_th=1.0, bear_th=-1.0) == BEAR
        assert classify_continuous(0.2, bull_th=1.0, bear_th=-1.0) == NEUTRAL
        assert classify_continuous(None, 1.0, -1.0) == NEUTRAL


# ── 변형 D: 개장 오버레이 ────────────────────────────────────────────────────
class TestOverlay:
    def test_sox_down_biases_bearish(self):
        # SOX 밤사이 급락 → shift>0 → 임계 상승(약세 편향)
        assert overlay_shift(sox_overnight_ret=-0.05) > 0
        assert overlay_shift(sox_overnight_ret=+0.05) < 0

    def test_vix_spike_biases_bearish(self):
        assert overlay_shift(vix_z=+1.0) > 0

    def test_no_inputs_is_zero(self):
        assert overlay_shift() == 0.0

    def test_overlay_can_flip_bull_to_neutral(self):
        # score가 임계 살짝 위 BULL인데 SOX 급락 오버레이가 임계를 밀어 NEUTRAL로 강등
        score = 1.05
        bull_th, bear_th = 1.0, -1.0
        assert classify_continuous(score, bull_th, bear_th) == BULL
        sh = overlay_shift(sox_overnight_ret=-0.05)
        assert classify_continuous(score, bull_th + sh, bear_th + sh) == NEUTRAL


# ── 유틸 ─────────────────────────────────────────────────────────────────────
class TestStats:
    def test_sma_back_offset(self):
        c = list(range(1, 101))  # 1..100
        assert sma(c, 10) == pytest.approx(sum(range(91, 101)) / 10)
        assert sma(c, 10, back=10) == pytest.approx(sum(range(81, 91)) / 10)

    def test_realized_vol_positive(self):
        c = _uptrend()
        assert realized_vol(c) is not None and realized_vol(c) >= 0
