"""스터디 13·14·16이 쓰는 통계 보조 — 정본은 `PYQuant/backtest/stats.py`로 옮겼다(2026-09-20).

여기는 옛 호출 방식을 지키는 얇은 층이다. `one_sample_t`는 (평균, t, 양측 p, n) 네 값 튜플을 돌려주고
나머지 이름은 그대로 넘긴다. 새 코드는 이 파일이 아니라 `backtest.stats`를 직접 쓴다.
"""
from __future__ import annotations

import sys
from pathlib import Path

_PYQUANT = Path(__file__).resolve().parents[3] / "PYQuant"
if str(_PYQUANT) not in sys.path:
    sys.path.insert(0, str(_PYQUANT))

from backtest.stats import benjamini_hochberg_qvalues, betainc, spearman, t_sf2  # noqa: E402,F401
from backtest.stats import one_sample_t as _one_sample_t  # noqa: E402


def one_sample_t(values) -> tuple[float, float, float, int]:
    """평균, t, 양측 p, n. n<2면 nan. 옛 튜플 인터페이스."""
    return _one_sample_t(values).as_tuple()
