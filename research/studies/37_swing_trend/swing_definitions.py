"""스터디 37 공용 정의 — 확정 저점·고점, 추세, 쌍바닥, 상승 추세 중 이평선 지지(사용자 합의 2026-10-07).

다른 스크립트가 import 해서 쓰도록 함수만 둔다. 모든 함수는 한 종목의 거래된 날 배열(행 = 거래일)을 받는다.
미래 정보 차단(인자 span = 사용자 정의의 k):
  - 확정 저점 i 는 low[i] 가 i−k .. i+k 중 최저인 날이다. 오른쪽 k 일을 봐야 알 수 있으므로 확정 행은 i+k 이고,
    그 행의 종가가 난 뒤부터만 쓴다(confirm_rows). 고점도 같다.
  - 추세·쌍바닥·이평선 지지 판정은 행 t 에서 confirm_rows ≤ t 인 점만 본다. 이평선은 t 종가까지 넣은 값이다.
같은 값이 이어지면(평평한 바닥) 첫날만 저점으로 본다 — 왼쪽은 엄격히 낮아야 하고 오른쪽은 같아도 된다.
"""
from __future__ import annotations

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

TREND_UP = 1
TREND_SIDEWAYS = 0
TREND_DOWN = -1
TREND_UNKNOWN = -9   # 확정 저점·고점이 2개씩 모이기 전

DOUBLE_BOTTOM_TOLERANCE = 0.03   # 두 저점 차이 한도 (내가 정함 — 사용자 합의 값)
DOUBLE_BOTTOM_GAP_MIN = 10       # 두 저점 간격 거래일 하한
DOUBLE_BOTTOM_GAP_MAX = 60       # 상한
DOUBLE_BOTTOM_NECK_RISE = 0.10   # 목선이 높은 쪽 저점보다 이만큼 위
SUPPORT_TOUCH = 1.01             # 장중 저가 ≤ 이평선 × 1.01


def pivot_rows(values: np.ndarray, span: int, lowest: bool) -> np.ndarray:
    """values[i] 가 앞뒤 k 일 중 최저(lowest) 또는 최고인 행 i. 확정 행은 i + k."""
    values = np.asarray(values, dtype=float)
    count = len(values)

    if count < 2 * span + 1:
        return np.array([], dtype=np.int64)

    signed = values if lowest else -values
    windows = sliding_window_view(signed, 2 * span + 1)          # windows[j] = rows j .. j+2span, 가운데 = j+span
    center = windows[:, span]
    left_best = windows[:, :span].min(axis=1)
    right_best = windows[:, span + 1:].min(axis=1)
    chosen = (center < left_best) & (center <= right_best)
    return np.flatnonzero(chosen).astype(np.int64) + span


def confirmed_pivots(low: np.ndarray, high: np.ndarray, span: int) -> dict:
    """확정 저점·고점. 반환 값마다 rows(그 날), confirm_rows(쓸 수 있는 첫 행 = rows + k), values."""
    low_rows = pivot_rows(low, span, lowest=True)
    high_rows = pivot_rows(high, span, lowest=False)
    return {"low_rows": low_rows, "low_confirm_rows": low_rows + span, "low_values": np.asarray(low, float)[low_rows],
            "high_rows": high_rows, "high_confirm_rows": high_rows + span, "high_values": np.asarray(high, float)[high_rows]}


def latest_two(confirm_rows: np.ndarray, values: np.ndarray, count: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """행 t 마다 확정된 마지막 점(newer)과 그 앞 점(older)의 값, 마지막 점의 순번. 없으면 NaN, 순번 −1."""
    rows = np.arange(count)
    position = np.searchsorted(confirm_rows, rows, side="right") - 1
    padded = np.r_[np.nan, np.asarray(values, float)]
    newer = padded[position + 1]
    older = padded[np.maximum(position, 0)]
    older = np.where(position >= 1, older, np.nan)
    return newer, older, position


def trend_state(pivots: dict, count: int) -> dict:
    """행 t 의 추세. 상승 = 저점↑ 그리고 고점↑, 하향 = 저점↓, 나머지 횡보. 같은 값은 ↑ 도 ↓ 도 아니다."""
    low_newer, low_older, low_position = latest_two(pivots["low_confirm_rows"], pivots["low_values"], count)
    high_newer, high_older, _ = latest_two(pivots["high_confirm_rows"], pivots["high_values"], count)
    known = np.isfinite(low_older) & np.isfinite(high_older)
    rising_low = low_newer > low_older
    rising_high = high_newer > high_older
    falling_low = low_newer < low_older
    state = np.full(count, TREND_SIDEWAYS, dtype=np.int8)
    state[rising_low & rising_high] = TREND_UP
    state[falling_low] = TREND_DOWN
    state[~known] = TREND_UNKNOWN
    return {"state": state, "latest_low": low_newer, "latest_low_position": low_position}


def moving_average(close: np.ndarray, window: int) -> np.ndarray:
    """t 종가까지 넣은 단순 이평선. 앞 window−1 행은 NaN."""
    close = np.asarray(close, float)
    result = np.full(len(close), np.nan)

    if len(close) >= window:
        cumulative = np.r_[0.0, np.cumsum(close)]
        result[window - 1:] = (cumulative[window:] - cumulative[:-window]) / window

    return result


def double_bottom_signals(close: np.ndarray, high: np.ndarray, pivots: dict,
                          tolerance: float = DOUBLE_BOTTOM_TOLERANCE, gap_min: int = DOUBLE_BOTTOM_GAP_MIN,
                          gap_max: int = DOUBLE_BOTTOM_GAP_MAX, neck_rise: float = DOUBLE_BOTTOM_NECK_RISE) -> list[dict]:
    """쌍바닥 돌파 신호. 마지막 확정 저점 둘(앞 a, 뒤 b)이 조건을 갖추면, b 확정 행부터 다음 저점이 확정되기 전까지
    종가가 목선(a 와 b 사이 고가 최고)을 처음 넘는 날이 신호다. b 뒤 확정 전에 이미 넘었으면 그 쌍은 버린다(내가 정함).
    반환: 신호 행, 두 번째 저점 행·값, 목선."""
    close = np.asarray(close, float)
    high = np.asarray(high, float)
    rows = pivots["low_rows"]
    confirm_rows = pivots["low_confirm_rows"]
    values = pivots["low_values"]
    count = len(close)
    signals = []

    for index in range(1, len(rows)):
        first_row, second_row = int(rows[index - 1]), int(rows[index])
        first_value, second_value = float(values[index - 1]), float(values[index])
        gap = second_row - first_row

        if gap < gap_min or gap > gap_max or abs(second_value / first_value - 1.0) > tolerance:
            continue

        neckline = float(high[first_row + 1:second_row].max())

        if neckline < max(first_value, second_value) * (1.0 + neck_rise):
            continue

        active_from = int(confirm_rows[index])
        active_until = int(confirm_rows[index + 1]) - 1 if index + 1 < len(rows) else count - 1
        above = np.flatnonzero(close[second_row + 1:active_until + 1] > neckline)

        if len(above) == 0:
            continue

        signal_row = second_row + 1 + int(above[0])

        if signal_row < active_from:
            continue

        signals.append({"signal_row": signal_row, "base_row": second_row, "base_low": second_value, "neckline": neckline})

    return signals


def moving_average_support_mask(close: np.ndarray, low: np.ndarray, average: np.ndarray, trend: np.ndarray, wanted_trend: int,
                    touch: float = SUPPORT_TOUCH) -> np.ndarray:
    """추세 = wanted_trend, 저가 ≤ 이평선 × touch, 종가 > 이평선, 이평선 > 전일 이평선."""
    close = np.asarray(close, float)
    low = np.asarray(low, float)
    previous = np.r_[np.nan, average[:-1]]

    with np.errstate(invalid="ignore"):
        mask = (trend == wanted_trend) & (low <= average * touch) & (close > average) & (average > previous)

    return mask & np.isfinite(average) & np.isfinite(previous)
