"""`PYQuant/data/point_in_time.py::as_of_join` — 미래 값이 붙으면 실패한다.

케이스: 기준일 정렬 오류(3/31 기준 보고서를 5/15 공시했으면 4월에는 못 쓴다)·정정 이중행(늦은 공시가 이긴다)·
상폐 뒤 옛 값 차단(max_age)·왼쪽 순서 보존.

실행: py -m pytest PYQuant/tests/test_point_in_time.py -q   (pytest 없으면 py PYQuant/tests/test_point_in_time.py)
"""
from __future__ import annotations

import sys
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # 저장소 루트

from PYQuant.data.point_in_time import as_of_join  # noqa: E402


def make_right() -> pd.DataFrame:
    return pd.DataFrame({
        "ticker": ["005930", "005930", "005930", "000660"],
        "effective_date": ["2024-03-12", "2024-05-15", "2024-05-15", "2024-03-20"],
        "published_at": ["2024-03-12", "2024-05-15", "2024-05-20", "2024-03-20"],
        "period_end": ["2023-12-31", "2024-03-31", "2024-03-31", "2023-12-31"],
        "total_equity": [100.0, 110.0, 111.0, 50.0],
    })


def test_future_row_is_not_joined():
    left = pd.DataFrame({"ticker": ["005930", "005930"], "as_of": ["2024-04-30", "2024-05-31"]})
    joined = as_of_join(left, make_right(), on="ticker", left_time="as_of", right_time="effective_date")
    # 4월 말에는 3/31 기준 보고서(5/15 공시)를 아직 모른다 → 연간 값 100.
    assert joined.loc[0, "total_equity"] == 100.0
    assert joined.loc[0, "period_end"] == "2023-12-31"
    # 5월 말에는 알 수 있다.
    assert joined.loc[1, "period_end"] == "2024-03-31"


def test_amendment_latest_published_wins():
    left = pd.DataFrame({"ticker": ["005930"], "as_of": ["2024-06-30"]})
    joined = as_of_join(left, make_right(), on="ticker", left_time="as_of", right_time="effective_date")
    assert joined.loc[0, "total_equity"] == 111.0


def test_before_first_report_is_missing():
    left = pd.DataFrame({"ticker": ["005930"], "as_of": ["2024-01-31"]})
    joined = as_of_join(left, make_right(), on="ticker", left_time="as_of", right_time="effective_date")
    assert pd.isna(joined.loc[0, "total_equity"])


def test_max_age_blocks_stale_rows():
    left = pd.DataFrame({"ticker": ["000660"], "as_of": ["2026-01-31"]})
    joined = as_of_join(left, make_right(), on="ticker", left_time="as_of", right_time="effective_date",
                        max_age=pd.Timedelta(days=450))
    assert pd.isna(joined.loc[0, "total_equity"])


def test_left_order_and_length_preserved():
    left = pd.DataFrame({"ticker": ["000660", "005930", "999999"], "as_of": ["2024-12-31", "2024-04-30", "2024-12-31"]})
    joined = as_of_join(left, make_right(), on="ticker", left_time="as_of", right_time="effective_date")
    assert list(joined["ticker"]) == ["000660", "005930", "999999"]
    assert len(joined) == 3
    assert pd.isna(joined.loc[2, "total_equity"])


if __name__ == "__main__":
    for name, function in list(globals().items()):
        if name.startswith("test_") and callable(function):
            function()
            print(f"통과 {name}")
