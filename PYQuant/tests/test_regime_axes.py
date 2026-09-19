"""regime_axes 시점 고정 검증 — as_of 뒤에 발표된 값은 점수를 바꾸지 못한다.

실행 (PYQuant/ 디렉토리에서):
    python -m pytest tests/test_regime_axes.py -v
실제 parquet·캐시를 읽는다. 없으면 건너뛴다.
"""
import sys
from pathlib import Path

import pandas as pd
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from features import regime_axes  # noqa: E402

AS_OF = pd.Timestamp("2024-06-03")


def _prepared_with_future_rows(mutate, cell, parameters):
    """as_of 이후 발표 행을 mutate 로 바꾼 뒤 준비한 시리즈 목록."""
    prepared_list = []

    for rule in regime_axes.rules_for_cell(cell):
        raw = regime_axes.load_series(rule)

        if not raw.empty:
            future = raw["published_at"] >= AS_OF
            raw = mutate(raw, future)

        prepared_list.append(regime_axes.prepare_series(rule, parameters, raw))

    return prepared_list


def _perturb(raw, future):
    raw = raw.copy()
    raw.loc[future, "value"] = raw.loc[future, "value"] * 7.0 + 1000.0
    return raw


def _drop(raw, future):
    return raw[~future].reset_index(drop=True)


@pytest.mark.skipif(not regime_axes.MACRO_DIRECTORY.exists(), reason="거시 parquet 없음")
@pytest.mark.parametrize("cell", ["market_only", "four_axis"])
def test_values_published_after_as_of_do_not_change_score(cell):
    parameters = regime_axes.Parameters()
    baseline = regime_axes.score(AS_OF, cell, parameters)
    perturbed = regime_axes.score(AS_OF, cell, parameters, _prepared_with_future_rows(_perturb, cell, parameters))
    dropped = regime_axes.score(AS_OF, cell, parameters, _prepared_with_future_rows(_drop, cell, parameters))
    assert perturbed == baseline
    assert dropped == baseline


def test_deadband_hysteresis_only_flips_beyond_threshold():
    assert regime_axes.next_sign("+", -0.2, 0.25) == "+"
    assert regime_axes.next_sign("+", -0.3, 0.25) == "-"
    assert regime_axes.next_sign("-", 0.2, 0.25) == "-"
    assert regime_axes.next_sign("-", 0.3, 0.25) == "+"
    assert regime_axes.next_sign("-", float("nan"), 0.25) == "-"


def test_macro_scale_rounds_to_five_hundredths_and_clips():
    assert regime_axes.macro_scale_of(0.75, 0.75, 0.9) == 0.5
    assert regime_axes.macro_scale_of(1.0, 1.0, 1.0) == 1.0
    assert regime_axes.liquidity_multiplier(-3.0) == 0.8
    assert regime_axes.liquidity_multiplier(2.0) == 1.0
    assert regime_axes.risk_multiplier(-1.5) == 0.75
    assert regime_axes.risk_multiplier(-2.5) == 0.5
