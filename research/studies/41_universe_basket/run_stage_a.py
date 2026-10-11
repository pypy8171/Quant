"""스터디 41 단계 A — 신호 없이 종목군마다 매달 무작위 N 종목(20 시드)과 종목군 전체를 같은 비중으로 담는다.

질문: 종목군 자체가 시장별 지수(b_full)를 이기는가, 비용·장중/밤 분해는 어떤가. 판정이 아니라 진단(사전 등록 choices_log 8행).
형태: (i) 월말 종가 매수 → 다음 월말 종가 매도  (ii) 월말 다음 날 시가 매수 → 다음 리밸런싱 시가 매도.
분수 주식·명목 2,000만·엔진 밴드 10%. 비용 0 과 S0.
실행: py -I -X utf8 research/studies/41_universe_basket/run_stage_a.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import pandas as pd

import study41 as study
import common

CAPITAL = 20_000_000.0
FORMS = {0: "(i) 월말 종가 → 종가", 1: "(ii) 다음 날 시가 → 시가"}


def main() -> None:
    started = time.time()
    inputs = study.Inputs()
    years = inputs.day_count / study.YEAR_DAYS
    rows = []

    for universe in range(len(study.UNIVERSE_NAMES)):
        for slots in (10, 20):
            for execution in (0, 1):
                for cost in (0, 1):
                    for seed in study.RANDOM_SEEDS:
                        rows.append((-1, universe, slots, 1, execution, 0, CAPITAL, 1, cost, 0, seed))

        for execution in (0, 1):
            for cost in (0, 1):
                rows.append((-2, universe, 20, 1, execution, 0, CAPITAL, 1, cost, 0, 0))

    cells = np.array(rows, dtype=np.float64)
    returns, bench, scalar, _ = inputs.run(cells)
    excess = returns - bench
    frame = pd.DataFrame(cells, columns=study.COLUMNS)
    frame["kind"] = np.where(frame["signal"] == -2, "종목군 전체", "무작위 " + frame["slots"].astype(int).astype(str))
    frame["group"] = frame.groupby(["kind", "ubit", "execution", "cost_on"], sort=False).ngroup()
    groups = frame.groupby("group", sort=True)
    table, year_rows = [], []
    sign_free = None

    mean_excess = np.stack([np.nanmean(excess[part.index.to_numpy()], axis=0) for _, part in groups])
    mean_return = np.stack([np.nanmean(returns[part.index.to_numpy()], axis=0) for _, part in groups])
    mean_bench = np.stack([np.nanmean(bench[part.index.to_numpy()], axis=0) for _, part in groups])
    t_month, t_year, _, _ = study.t_values(mean_excess, inputs, sign_free)
    annual_years, _ = study.yearly(mean_excess, inputs)

    for position, (group, part) in enumerate(groups):
        first = part.iloc[0]
        index = part.index.to_numpy()
        seed_excess = study.annual_mean(excess[index])
        scal = scalar[index].mean(axis=0)
        table.append({
            "종목군": study.UNIVERSE_NAMES[int(first["ubit"])], "종목군_설명": study.UNIVERSE_TEXT[study.UNIVERSE_NAMES[int(first["ubit"])]],
            "바구니": first["kind"], "형태": FORMS[int(first["execution"])], "비용": "S0" if first["cost_on"] else "0",
            "시드_수": len(index), "연수익_평균": common.clean_number(np.nanmean(mean_return[position]) * study.YEAR_DAYS * 100, 3),
            "지수_연수익": common.clean_number(np.nanmean(mean_bench[position]) * study.YEAR_DAYS * 100, 3),
            "지수_대비_연초과": common.clean_number(np.nanmean(mean_excess[position]) * study.YEAR_DAYS * 100, 3),
            "달_t": common.clean_number(t_month[position], 3), "해_t": common.clean_number(t_year[position], 3),
            "t_min": common.clean_number(min(t_month[position], t_year[position]), 3),
            "시드별_연초과_최소": common.clean_number(seed_excess.min() * 100, 3),
            "시드별_연초과_최대": common.clean_number(seed_excess.max() * 100, 3),
            "양수_시드_비율": common.clean_number((seed_excess > 0).mean(), 3),
            "밤_연로그": common.clean_number(scal[0] / years * 100, 3), "장중_연로그": common.clean_number(scal[1] / years * 100, 3),
            "비용_연로그": common.clean_number(scal[2] / years * 100, 3),
            "지수_밤_연로그": common.clean_number(scal[3] / years * 100, 3), "지수_장중_연로그": common.clean_number(scal[4] / years * 100, 3),
            "연_회전율": common.clean_number(scal[5] / years, 3),
            "양수_해_비율": common.clean_number(np.nanmean(annual_years[position] > 0) if np.isfinite(annual_years[position]).any() else np.nan, 3),
        })

        for year_index, value in enumerate(annual_years[position]):
            year_rows.append({"종목군": table[-1]["종목군"], "바구니": first["kind"], "형태": table[-1]["형태"], "비용": table[-1]["비용"],
                              "해": int(inputs.years[0]) + year_index, "지수_대비_연초과": common.clean_number(value * 100, 3)})

    study.OUTPUT.mkdir(parents=True, exist_ok=True)
    common.write_csv(study.OUTPUT / "stage_a_table.csv", pd.DataFrame(table))
    years_frame = pd.DataFrame(year_rows).pivot_table(index=["종목군", "바구니", "형태", "비용"], columns="해", values="지수_대비_연초과",
                                                      sort=False).reset_index()
    common.write_csv(study.OUTPUT / "stage_a_years.csv", years_frame)
    common.write_json(study.OUTPUT / "stage_a_info.json", {"cells": int(len(cells)), "seconds": round(time.time() - started, 1),
                                                     "capital": CAPITAL, "seeds": list(study.RANDOM_SEEDS)})
    print(f"단계 A {len(cells)} 칸 · {time.time() - started:.0f}초", flush=True)


if __name__ == "__main__":
    main()
