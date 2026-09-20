"""스터디 20 · 네 축 국면 표 만들기 — PYQuant/features/regime_axes.py 를 전 기간에 돌려 out/axes_<cell>.parquet 로 남긴다.

실행(저장소 루트에서):
  py research/studies/20_macro_overlay/build_axes.py --cell market_only
  py research/studies/20_macro_overlay/build_axes.py --cell four_axis
  py research/studies/20_macro_overlay/build_axes.py --cell four_axis --deadband 0.35 --window-months 144 --base-contraction 0.6

한 행 = 결정일(평일) 하나. 열은 regime_axes.build_axes_table 설명 참고. 시리즈별 z 는 out/z_<cell>.parquet 에 같이 남긴다.
난수 없음. 입력은 PYQuant/data/macro/*.parquet · PYQuant/.index_cache/idx_* · derived_foreign_flow_kospi.parquet.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))

from PYQuant.features import regime_axes  # noqa: E402

STUDY_DIRECTORY = Path(__file__).resolve().parent   # --study-dir 로 바꾼다(스터디 21 은 이 러너를 그대로 쓴다)
OUTPUT_DIRECTORY = STUDY_DIRECTORY / "out"
DEFAULT_AS_OF = "2026-08-14"   # 코스피 캐시 마지막 봉


def build(cell: str, as_of: str, parameters: regime_axes.Parameters) -> pd.DataFrame:
    calendar = regime_axes.decision_calendar(as_of)
    table = regime_axes.build_axes_table(calendar, cell, parameters)
    return table


def main() -> int:
    global OUTPUT_DIRECTORY
    parser = argparse.ArgumentParser(description="네 축 국면 표")
    parser.add_argument("--cell", choices=["market_only", "four_axis"], default="four_axis")
    parser.add_argument("--as-of", default=DEFAULT_AS_OF)
    parser.add_argument("--deadband", type=float, default=0.25)
    parser.add_argument("--window-months", type=int, default=120)
    parser.add_argument("--base-contraction", type=float, default=0.5)
    parser.add_argument("--scale-update", choices=["daily", "on_state_change"], default="daily",
                        help="배수 갱신 규칙(regime_axes.Parameters.scale_update). 스터디 20 은 daily, 21 은 on_state_change")
    parser.add_argument("--study-dir", default=str(STUDY_DIRECTORY), help="산출물을 남길 스터디 폴더")
    arguments = parser.parse_args()
    OUTPUT_DIRECTORY = Path(arguments.study_dir).resolve() / "out"

    parameters = regime_axes.Parameters(deadband=arguments.deadband, window_months=arguments.window_months,
                                        base_contraction=arguments.base_contraction,
                                        scale_update=arguments.scale_update)
    table = build(arguments.cell, arguments.as_of, parameters)
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
    axes_path = OUTPUT_DIRECTORY / f"axes_{arguments.cell}.parquet"
    z_path = OUTPUT_DIRECTORY / f"z_{arguments.cell}.parquet"
    z_frame = table.attrs["z_frame"].copy()
    z_frame.index = pd.to_datetime(z_frame.index)
    z_frame.index.name = "decision_date"
    table.attrs = {}   # z 표는 따로 저장한다(attrs 는 parquet 메타데이터로 못 나간다)
    table.to_parquet(axes_path, index=False)
    z_frame.reset_index().to_parquet(z_path, index=False)

    grades = {}

    for rule in regime_axes.rules_for_cell(arguments.cell):
        raw = regime_axes.load_series(rule)
        grades[rule.name] = "missing" if raw.empty else ("B" if (raw["grade"] == "B").any() else "A")

    summary = {
        "cell": arguments.cell,
        "as_of": arguments.as_of,
        "parameters": {"deadband": parameters.deadband, "window_months": parameters.window_months,
                       "base_contraction": parameters.base_contraction, "scale_update": parameters.scale_update},
        "rows": int(len(table)),
        "first_growth_valid": _first_valid(table, "growth"),
        "first_inflation_valid": _first_valid(table, "inflation"),
        "first_liquidity_valid": _first_valid(table, "liquidity"),
        "first_risk_valid": _first_valid(table, "risk"),
        "regime_days": table["regime"].value_counts().to_dict(),
        "macro_scale_mean": round(float(table["macro_scale"].mean()), 4),
        "data_grades": grades,
        "missing_series": [name for name, _, _ in regime_axes.MISSING_SERIES],
    }
    (OUTPUT_DIRECTORY / f"axes_{arguments.cell}_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"저장: {axes_path.relative_to(REPO_ROOT)} · {z_path.relative_to(REPO_ROOT)}")
    return 0


def _first_valid(table: pd.DataFrame, axis: str) -> str | None:
    valid = table.loc[table[axis].notna(), "decision_date"]
    return None if valid.empty else valid.iloc[0].strftime("%Y-%m-%d")


if __name__ == "__main__":
    sys.exit(main())
