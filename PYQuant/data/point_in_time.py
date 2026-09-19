"""시점 고정 조인 — 왼쪽 표의 기준일(as_of)에 "그날 이미 알려져 있던" 오른쪽 최신 행 하나를 붙인다.

재무·주식수·수급처럼 발효일이 있는 표를 월별 횡단면에 붙일 때 이 함수 하나만 쓴다(`research/COUNCIL_CHARTER.md` §5).
미래 행(발효일 > 기준일)은 구조적으로 붙지 않으므로, 이 함수를 거친 값은 룩어헤드 감사에서 "조인 시점"을 따로 볼 필요가 없다.

규칙
  - `right_time <= left_time`인 행 가운데 가장 늦은 행. 같은 발효일이 여럿이면(정정 공시) `published_at`이 늦은 행이 이긴다
    (`published_at` 열이 없으면 원래 순서에서 뒤의 행).
  - `max_age`를 주면 발효일이 기준일보다 그만큼 넘게 오래된 행은 붙이지 않는다(폐업·상폐 뒤 옛 값이 끌려오는 것을 막는다).
  - 왼쪽 행 순서와 개수는 그대로다. 붙일 것이 없으면 오른쪽 열은 결측.

재현: py -m pytest PYQuant/tests/test_point_in_time.py -q
"""
from __future__ import annotations

import pandas as pd


def as_of_join(left: pd.DataFrame, right: pd.DataFrame, on: str, left_time: str, right_time: str,
               max_age: pd.Timedelta | None = None, published_column: str = "published_at") -> pd.DataFrame:
    """왼쪽 (on, left_time) 행마다 오른쪽에서 `right_time <= left_time`인 최신 행을 붙인다.

    반환 열 = 왼쪽 열 + 오른쪽 열(겹치는 이름은 오른쪽 열을 뺀다, 단 `right_time`은 남겨 어느 발효분이 붙었는지 보이게 한다).
    """
    if on not in left.columns or on not in right.columns:
        raise KeyError(f"조인 키 {on!r}가 양쪽에 있어야 한다")

    left_work = left.copy()
    left_work["__left_order"] = range(len(left_work))
    left_work[left_time] = pd.to_datetime(left_work[left_time]).astype("datetime64[ns]")

    right_work = right.copy()
    right_work[right_time] = pd.to_datetime(right_work[right_time]).astype("datetime64[ns]")   # parquet은 [s]로 올 때가 있다
    sort_keys = [right_time] + ([published_column] if published_column in right_work.columns else [])
    right_work = right_work.sort_values(sort_keys, kind="stable")

    # 같은 (키, 발효일)에 여러 행이면 정렬 뒤 마지막 행(늦게 공시된 것)만 남긴다 — merge_asof는 동률을 하나로 못 가른다.
    right_work = right_work.drop_duplicates([on, right_time], keep="last")

    overlapping = [column for column in right_work.columns if column in left_work.columns and column != on]
    right_work = right_work.drop(columns=overlapping)

    if right_time == left_time:
        raise ValueError("left_time과 right_time 이름이 같으면 어느 쪽 날짜인지 구분할 수 없다")

    left_sorted = left_work.sort_values(left_time, kind="stable")
    right_sorted = right_work.sort_values(right_time, kind="stable")
    joined = pd.merge_asof(left_sorted, right_sorted, left_on=left_time, right_on=right_time, by=on,
                           direction="backward", allow_exact_matches=True)

    if max_age is not None:
        stale = (joined[left_time] - joined[right_time]) > max_age
        right_columns = [column for column in right_sorted.columns if column != on]
        joined.loc[stale, right_columns] = pd.NA
        joined.loc[stale, right_time] = pd.NaT

    joined = joined.sort_values("__left_order").drop(columns="__left_order").reset_index(drop=True)
    return joined
