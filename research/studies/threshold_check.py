"""합격선 숫자가 뜻하는 바를 직접 세어 보는 스크립트.

`research/studies/READING_NUMBERS.md`의 표가 이 출력에서 나온다. 효과가 전혀 없는 가짜 전략을 많이 만들어
우리 합격선(t ≥ 2.0, 5창 중 3창, 격자 최고 칸)을 얼마나 자주 통과하는지 센다.

실행: py research/studies/threshold_check.py [--curves research/studies/22_pbr_roe_value_tilt/curves.csv]
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
import pandas as pd

MONTHS = 60                 # 판정 구간 2021-01~2025-12
MONTHLY_STD = 0.04198       # 스터디 22 판정 구간 월 초과수익의 표준편차
TRIALS = 400_000
GRID_CELLS = 27             # 스터디 22 격자 칸 수
SEED = 20260920


def t_statistic(samples: np.ndarray) -> np.ndarray:
    """행마다 t = 평균 ÷ (표준편차 ÷ √n)."""
    count = samples.shape[1]
    return samples.mean(axis=1) / (samples.std(axis=1, ddof=1) / math.sqrt(count))


def binomial_tail(successes_at_least: int, windows: int, probability: float = 0.5) -> float:
    """창 windows개가 동전 던지기일 때 successes_at_least개 이상이 +일 확률."""
    return sum(math.comb(windows, count) * probability**windows for count in range(successes_at_least, windows + 1))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--curves", default="research/studies/22_pbr_roe_value_tilt/curves.csv")
    arguments = parser.parse_args()

    generator = np.random.default_rng(SEED)
    fake = generator.normal(0.0, MONTHLY_STD, (TRIALS, MONTHS))
    fake_t = t_statistic(fake)
    one_sided = float((fake_t >= 2.0).mean())
    print(f"효과 0인 가짜 전략 {TRIALS:,}개, {MONTHS}개월, 월 흔들림 {MONTHLY_STD * 100:.2f}%")
    print(f"  |t| >= 2.0 (양쪽)     : {float((np.abs(fake_t) >= 2.0).mean()) * 100:.2f}%")
    print(f"  t >= 2.0 (한쪽, 우리 판정): {one_sided * 100:.2f}%")
    print(f"  t >= 2.41 (스터디 22 값) : {float((fake_t >= 2.41).mean()) * 100:.2f}%")
    print(f"  격자 {GRID_CELLS}칸 중 최고 칸만 고르면 하나라도 t >= 2.0 (칸 독립 가정) : {(1 - (1 - one_sided) ** GRID_CELLS) * 100:.1f}%")
    print(f"  격자 9칸(스터디 19)일 때 같은 값 : {(1 - (1 - one_sided) ** 9) * 100:.1f}%")

    curves_path = Path(arguments.curves)

    if curves_path.exists():
        curves = pd.read_csv(curves_path)
        judged = curves[(curves.month >= "2021-01") & (curves.month <= "2025-12")]
        excess = judged.excess.to_numpy()
        centered = excess - excess.mean()
        resampled = generator.choice(centered, (TRIALS // 2, len(centered)))
        resampled_t = t_statistic(resampled)
        standard_error = excess.std(ddof=1) / math.sqrt(len(excess))
        print(f"\n{curves_path} 판정 구간 실제 값")
        print(f"  n={len(excess)} 월평균={excess.mean() * 100:.3f}% 표준편차={excess.std(ddof=1) * 100:.3f}% "
              f"표준오차={standard_error * 100:.3f}% t={excess.mean() / standard_error:.2f}")
        print(f"  같은 달들을 평균 0으로 맞춰 다시 뽑았을 때 t >= 2.0 : {float((resampled_t >= 2.0).mean()) * 100:.2f}%, "
              f"t >= 2.41 : {float((resampled_t >= 2.41).mean()) * 100:.2f}%")

    print("\nwalk-forward 창이 동전 던지기일 때 통과 확률")

    for windows, at_least in ((5, 3), (5, 4), (7, 5), (7, 6)):
        print(f"  {windows}창 중 >= {at_least} : {binomial_tail(at_least, windows) * 100:.1f}%")


if __name__ == "__main__":
    main()
