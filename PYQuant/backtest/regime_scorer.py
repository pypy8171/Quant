"""국면 스코어러 — C++ RegimeController(compute_score/classify) 미러 + 연속화·기울기·개장오버레이 애블레이션.

이 모듈은 Track A(구조 국면) 애블레이션의 지적 핵심이다. C++ `Quant/include/core/RegimeController.h`
의 순수 로직(compute_score/classify)을 파이썬으로 1:1 미러링(변형 A)하고, 사용자 문제제기
(①단조 ②일봉이라 5분 재평가 무의미 ③이산·과보수)를 겨냥해 연속화(B)·기울기(C)·SOX오버레이(D)로 확장한다.

변형:
  A v0_2axis   — C++ v0 그대로. 종가>200MA(±1) + 정배열/역배열(±1) → score∈{-2..+2} 이산.
  B continuous — 위치·정배열 스프레드를 tanh로 연속화. 이산성(문제③) 해소.
  C slope      — B + MA200 기울기 축. V반등 bounce vs 진짜추세 구분(문제①).
  D overlay    — 개장 SOX/VIX 오버레이로 classify 임계를 shift. compute_score와 분리(G2 2축분리 유지).

설계 원칙(reviewer 반박 반영 — 과적합/식별불가 방지):
  * tanh 스케일 s는 자유파라미터가 아니라 지수 20일 실현변동성의 배수로 **사전고정**.
    → 6개 하락장 창(인과중첩)에 6연속파라미터를 맞추는 "식별 불가"를 회피.
  * 자유파라미터는 가중치 w(기본 1.0) + classify 임계뿐. 임계는 하락장 전용이 아니라
    BULL/NEUTRAL 포함 전국면 창에서 교정하고 2022 홀드아웃으로 확인.
  * 히스테리시스(dwell)는 **일봉 입력에선 무의미**(입력이 하루 상수)이므로 여기 없음.
    그 관심사는 폴링 입력을 쓰는 Track B(장중 국면)의 것이다.

라이브 패리티: 변형 A는 C++ compute_score/classify와 동일 결과여야 한다(test_regime_scorer.py가 강제).
"""
from __future__ import annotations

import math
from typing import Sequence

BULL = "BULL"
NEUTRAL = "NEUTRAL"
BEAR = "BEAR"
UNKNOWN = "UNKNOWN"


# ─── 기본 통계 (순수) ────────────────────────────────────────────────────────
def sma(closes: Sequence[float], n: int, back: int = 0) -> float | None:
    """closes[-(n+back) : len-back]의 단순이평. back=20 → 20봉 전 시점의 n일 이평.
    표본 부족(NaN 유발)이면 None — 호출부가 판정 보류(C++ RegimeController.cpp:98 가드와 동형)."""
    end = len(closes) - back
    start = end - n
    if start < 0 or end <= start:
        return None
    seg = closes[start:end]
    return sum(seg) / n


def realized_vol(closes: Sequence[float], n: int = 20) -> float | None:
    """최근 n일 로그수익률 표준편차(일간). tanh 스케일 사전고정용. 표본 부족이면 None."""
    if len(closes) < n + 1:
        return None
    rets = []
    for i in range(len(closes) - n, len(closes)):
        p0, p1 = closes[i - 1], closes[i]
        if p0 > 0 and p1 > 0:
            rets.append(math.log(p1 / p0))
    if len(rets) < 2:
        return None
    m = sum(rets) / len(rets)
    var = sum((r - m) ** 2 for r in rets) / (len(rets) - 1)
    return math.sqrt(var)


# ─── 변형 A: C++ v0 미러 (이산 2축) ──────────────────────────────────────────
def score_v0(closes: Sequence[float], ma_long=200, ma_short=20, ma_mid=60, ma_align3=120) -> int | None:
    """C++ RegimeController::compute_score 미러. closes[-1]=결정 종가(전일 확정봉).
    반환 score∈{-2..+2} 또는 표본부족 시 None."""
    ma200 = sma(closes, ma_long)
    ma20 = sma(closes, ma_short)
    ma60 = sma(closes, ma_mid)
    ma120 = sma(closes, ma_align3)
    if ma200 is None or ma20 is None or ma60 is None or ma120 is None:
        return None
    close = closes[-1]
    if close <= 0 or ma200 <= 0 or ma20 <= 0 or ma60 <= 0 or ma120 <= 0:
        return None
    score = 1 if close > ma200 else -1                 # 축1: 200일선
    if ma20 > ma60 > ma120:                            # 축2: 정배열
        score += 1
    elif ma20 < ma60 < ma120:                          # 역배열
        score -= 1
    return score


def classify_v0(score: int | None, bull_th=2, bear_th=-2) -> str:
    """C++ classify 미러. 표본부족(None)이면 NEUTRAL 안전판(cpp:70)."""
    if score is None:
        return NEUTRAL
    if score >= bull_th:
        return BULL
    if score <= bear_th:
        return BEAR
    return NEUTRAL


# ─── 변형 B/C: 연속화 + 기울기 ───────────────────────────────────────────────
# 스케일은 실현변동성 vol의 배수로 고정(자유파라미터 아님). 아래 계수는 "vol의 몇 배를
# tanh 포화 지점으로 볼 것인가"의 사전 선택 — 도메인 상수로 취급.
S_POS = 8.0    # 위치 z1: |close-ma200|/ma200 가 8*일변동성이면 ~포화
S_ALN = 6.0    # 정배열 스프레드 z3
S_SLP = 30.0   # 기울기 z2: 20일간 ma200 변화율. 느린 축이라 스케일 큼


def score_continuous(closes: Sequence[float], with_slope: bool,
                     w_pos=1.0, w_aln=1.0, w_slope=1.0,
                     ma_long=200, ma_short=20, ma_mid=60, ma_align3=120,
                     slope_back=20, vol_n=20) -> float | None:
    """변형 B(with_slope=False)/C(True). 연속 score. 표본부족이면 None.
      z1 = (close-ma200)/ma200                     (위치)
      z3 = (ma20-ma60)/ma60 + (ma60-ma120)/ma120   (정배열 스프레드)
      z2 = (ma200_t - ma200_{t-slope_back})/ma200_{t-slope_back}  (기울기, C 전용)
    각 z를 (S_* * vol)로 나눠 tanh. vol=지수 20일 실현변동성(사전고정 스케일)."""
    ma200 = sma(closes, ma_long)
    ma20 = sma(closes, ma_short)
    ma60 = sma(closes, ma_mid)
    ma120 = sma(closes, ma_align3)
    vol = realized_vol(closes, vol_n)
    if None in (ma200, ma20, ma60, ma120, vol):
        return None
    if min(ma200, ma20, ma60, ma120) <= 0 or vol <= 0 or closes[-1] <= 0:
        return None
    z1 = (closes[-1] - ma200) / ma200
    z3 = (ma20 - ma60) / ma60 + (ma60 - ma120) / ma120
    score = w_pos * math.tanh(z1 / (S_POS * vol)) + w_aln * math.tanh(z3 / (S_ALN * vol))
    if with_slope:
        ma200_prev = sma(closes, ma_long, back=slope_back)
        if ma200_prev is None or ma200_prev <= 0:
            return None
        z2 = (ma200 - ma200_prev) / ma200_prev
        score += w_slope * math.tanh(z2 / (S_SLP * vol))
    return score


def classify_continuous(score: float | None, bull_th: float, bear_th: float) -> str:
    """연속 score → 3-state. 표본부족(None)이면 NEUTRAL 안전판."""
    if score is None:
        return NEUTRAL
    if score >= bull_th:
        return BULL
    if score <= bear_th:
        return BEAR
    return NEUTRAL


# ─── 변형 D: 개장 SOX/VIX 오버레이 (임계 shift, compute_score와 분리) ─────────
S_SOX = 0.03   # SOX 밤사이 수익률 3%가 tanh 포화
def overlay_shift(sox_overnight_ret: float | None = None,
                  vix_z: float | None = None, gain: float = 0.5) -> float:
    """개장 오버레이가 classify 임계에 더할 shift(≥0=상승장벽↑/약세 편향). G2: compute_score에
    가산하지 않고 임계만 민다 → 선택축과 오버레이축의 관심사 분리.
      sox_overnight_ret: 전일 US세션 ^SOX 수익률(< 체결일, 캘린더 정렬은 호출부 책임).
      vix_z: (vix - vix_sma20)/vix_sma20 등 표준화 레벨(양수=공포).
    반환 shift를 bull_th += shift, bear_th += shift 로 적용(둘 다 위로 밀어 약세 편향)."""
    shift = 0.0
    if sox_overnight_ret is not None:
        shift += -math.tanh(sox_overnight_ret / S_SOX)   # SOX↓ → shift↑(약세)
    if vix_z is not None:
        shift += math.tanh(vix_z)                        # VIX↑ → shift↑(약세)
    return gain * shift
