"""스터디 41 단계 B — 종목군 23 × 신호 10 × 형태 24 = 5,520 칸을 한 가족으로 판정한다.

질문: 신호로 고른 상위 N 바구니가 시장별 지수(b_full)와 같은 형태 무작위 바구니를 둘 다 이기는가.
판정 c1–c9·통과선 = max(가족 기준선, 3.0)은 사전 등록(results/choices_log.csv 17–20·33행) 그대로다.
본판: 자본 200만·정수 주식·비용 S0. 민감도(S1·2,000만·2억)·비용 0 판은 표로만 낸다.
실행: py -I -X utf8 research/studies/41_universe_basket/run_stage_b.py
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import pandas as pd

import study41 as study
import common

CAPITAL = 2_000_000.0
FLOOR = 3.0                      # 통과선 아래 한도(내가 정함, 사전 등록 18행)
SLOTS = (10, 20)
EXECUTIONS = (0, 1, 2)
BUFFERS = (0, 1)
REBALANCES = (0, 1)
UNIVERSE_COUNT = len(study.UNIVERSE_NAMES)
SIGNAL_COUNT = len(study.SIGNAL_NAMES)
FORM_COUNT = UNIVERSE_COUNT * len(SLOTS) * len(REBALANCES) * len(EXECUTIONS) * len(BUFFERS)
EXCLUDE_FROM = 2025              # c8: 2025–26 뺀 판
DEVSCALE = {"signal": "ZONE", "universe": "T200", "slots": 20, "rebalance": 0, "execution": 1, "buffer": 0}
W_GRID = (0.0, 0.1, 0.2, 0.3, 0.5, 1.0)
# 실행 뒤 고친 것(사후 행). 단계 A·B 를 다시 돌린 결과가 지금 파일이다
POST_FIXES = [["사후: 매도가 하한", "1호가 미끄러짐을 뺀 매도가가 0원이 되던 것을 최소 호가(1원)로 막는다. 무수정가 1–2원 종목(예 에스와이코퍼레이션 2012)에서 매도 대금 0원·수익 −100% 가 나왔다",
               "내가 정함(실행 뒤 발견)", "예", "아니오(실행 뒤 수정)", "basket.py 매도 3곳. 단계 A·B 모두 다시 돌림"]]
UNIVERSE_CHAINS = (("T50", "T100", "T200", "T500"), tuple(f"CAP{number}" for number in range(1, 6)),
                   tuple(f"PRICE{number}" for number in range(1, 6)), tuple(f"VOL{number}" for number in range(1, 6)))
SIGNAL_CHAINS = (("FLOW5", "FLOW20"), ("VALUE", "VQ", "QUAL"))


def family_cells(capital: float = CAPITAL, ticks: int = 1, cost: int = 1) -> np.ndarray:
    grid = study.cell_table(signal=range(SIGNAL_COUNT), ubit=range(UNIVERSE_COUNT), slots=SLOTS, rebalance=REBALANCES,
                        execution=EXECUTIONS, buffer=BUFFERS, capital=[capital], ticks=[ticks], cost_on=[cost], integer=[1],
                        seed=[0])
    return grid.to_numpy(dtype=np.float64)


def random_cells(seed: int, signal: int) -> np.ndarray:
    grid = study.cell_table(signal=[signal], ubit=range(UNIVERSE_COUNT), slots=SLOTS, rebalance=REBALANCES, execution=EXECUTIONS,
                        buffer=BUFFERS, capital=[CAPITAL], ticks=[1], cost_on=[1], integer=[1], seed=[seed])
    return grid.to_numpy(dtype=np.float64)


def row_of(signal: int, universe: int, slot_index: int, rebalance: int, execution: int, buffer: int) -> int:
    return ((((signal * UNIVERSE_COUNT + universe) * 2 + slot_index) * 2 + rebalance) * 3 + execution) * 2 + buffer


def neighbours(row: np.ndarray) -> list[int]:
    """사전 등록 20행: 한 축만 다른 칸."""
    signal, universe = int(row[0]), int(row[1])
    slot_index, rebalance, execution, buffer = SLOTS.index(int(row[2])), int(row[3]), int(row[4]), int(row[5])
    result = [row_of(signal, universe, 1 - slot_index, rebalance, execution, buffer),
              row_of(signal, universe, slot_index, 1 - rebalance, execution, buffer),
              row_of(signal, universe, slot_index, rebalance, execution, 1 - buffer)]
    result += [row_of(signal, universe, slot_index, rebalance, other, buffer) for other in EXECUTIONS if other != execution]

    for chain in UNIVERSE_CHAINS:
        name = study.UNIVERSE_NAMES[universe]

        if name in chain:
            position = chain.index(name)
            result += [row_of(signal, study.UNIVERSE_NAMES.index(chain[other]), slot_index, rebalance, execution, buffer)
                       for other in (position - 1, position + 1) if 0 <= other < len(chain)]

    for chain in SIGNAL_CHAINS:
        name = study.SIGNAL_NAMES[signal]

        if name in chain:
            position = chain.index(name)
            result += [row_of(study.SIGNAL_NAMES.index(chain[other]), universe, slot_index, rebalance, execution, buffer)
                       for other in (position - 1, position + 1) if 0 <= other < len(chain)]

    return result


def cell_id(row: np.ndarray) -> str:
    rebalance = "W" if int(row[3]) == 0 else "M"
    return (f"{study.SIGNAL_NAMES[int(row[0])]}-{study.UNIVERSE_NAMES[int(row[1])]}-N{int(row[2])}-{rebalance}"
            f"-E{int(row[4])}-B{int(row[5])}")


def t_min_of(values: np.ndarray, inputs: study.Inputs) -> np.ndarray:
    t_month, t_year, _, _ = study.t_values(values, inputs)
    return np.fmin(t_month, t_year)


def annual(values: np.ndarray) -> np.ndarray:
    return study.annual_mean(values)


def summary_of_run(inputs: study.Inputs, cells: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    returns, bench, _, _ = inputs.run(cells)
    excess = returns - bench
    del returns, bench
    return annual(excess), t_min_of(excess, inputs)


def add_post_rows(rows: list[list]) -> None:
    """사후 행(대표 칸 결과)을 choices_log 끝에 다시 쓴다 — 같은 item 은 지우고 새로 적어 두 번 돌려도 같다."""
    path = study.OUTPUT / "choices_log.csv"
    frame = pd.read_csv(path, dtype=str, keep_default_na=False)
    frame = frame[~frame["item"].str.startswith("사후:")]
    start = int(frame["order"].astype(int).max()) + 1
    extra = pd.DataFrame([[str(start + offset)] + [str(value) for value in row] for offset, row in enumerate(rows)], columns=frame.columns)
    common.write_csv(path, pd.concat([frame, extra], ignore_index=True))


def main() -> None:
    started = time.time()
    inputs = study.Inputs()
    years = inputs.day_count / study.YEAR_DAYS
    cells = family_cells()
    count = len(cells)
    match = np.arange(count) % FORM_COUNT
    print(f"가족 {count} 칸", flush=True)

    # 1. 본판(S0·200만·정수) + 기여
    returns, bench, scalar, contribution = inputs.run(cells, contrib=True)
    excess = returns - bench
    sign_pair = study.signs(inputs)
    t_month, t_year, flip_month, flip_year = study.t_values(excess, inputs, sign_pair)
    bars = study.family_bar(flip_month, flip_year)
    del flip_month, flip_year
    bar = max(bars["family_bar"], FLOOR)
    t_index = np.fmin(t_month, t_year)
    excess_annual = annual(excess)
    return_annual = annual(returns)
    bench_annual = annual(bench)
    alpha, beta = study.beta_alpha(returns, bench)
    year_table, _ = study.yearly(excess, inputs)
    keep = inputs.years < EXCLUDE_FROM
    excess_before = annual(excess[:, keep])
    print(f"본판 끝 {time.time() - started:.0f}초 · 가족 기준선 {bars['family_bar']:.3f} · 통과선 {bar:.3f}", flush=True)

    # 2. 무작위 대조 20 시드. 판정은 점수를 고정한 무작위(버퍼가 실제로 작동), 결정일마다 새로 뽑는 무작위는 참고 열
    def random_block(signal: int) -> dict:
        random_sum = np.zeros((FORM_COUNT, inputs.day_count))
        random_count = np.zeros((FORM_COUNT, inputs.day_count))
        random_excess_sum = np.zeros((FORM_COUNT, inputs.day_count))
        seed_excess = np.zeros((count, len(study.RANDOM_SEEDS)))
        seed_t = np.zeros((count, len(study.RANDOM_SEEDS)))

        for position, seed in enumerate(study.RANDOM_SEEDS):
            random_returns, random_bench, _, _ = inputs.run(random_cells(seed, signal))
            valid = np.isfinite(random_returns)
            random_sum += np.where(valid, random_returns, 0.0)
            random_count += valid
            random_excess_sum += np.where(valid, random_returns - random_bench, 0.0)
            difference = returns - random_returns[match]
            seed_excess[:, position] = annual(difference)
            seed_t[:, position] = t_min_of(difference, inputs)
            del difference, random_returns, random_bench

        with np.errstate(invalid="ignore", divide="ignore"):
            random_mean = np.where(random_count > 0, random_sum / random_count, np.nan)
            random_vs_index = np.where(random_count > 0, random_excess_sum / random_count, np.nan)

        versus_random = returns - random_mean[match]
        return {"mean": random_mean, "annual": annual(versus_random), "t": t_min_of(versus_random, inputs),
                "universe": annual(random_vs_index)[match], "seed_excess": seed_excess, "seed_t": seed_t}

    fixed = random_block(study.basket.SIGNAL_RANDOM_FIXED)
    fresh = random_block(study.basket.SIGNAL_RANDOM)
    random_mean, random_annual, t_random = fixed["mean"], fixed["annual"], fixed["t"]
    universe_effect, seed_excess, seed_t = fixed["universe"], fixed["seed_excess"], fixed["seed_t"]
    print(f"무작위 끝 {time.time() - started:.0f}초", flush=True)

    # 3. 상위 10종목 기여, 옆 칸
    total = contribution.sum(axis=1)
    top10 = -np.sort(-contribution, axis=1)[:, :10].sum(axis=1)

    with np.errstate(invalid="ignore", divide="ignore"):
        top_share = np.where(total > 0, top10 / total, np.nan)

    neighbour_lists = [neighbours(row) for row in cells]
    neighbour_min = np.array([np.nanmin(excess_annual[items]) for items in neighbour_lists])

    # 4. 판정
    positive_years = np.array([np.nanmean(row > 0) if np.isfinite(row).any() else np.nan for row in year_table])
    median_years = np.array([np.nanmedian(row) if np.isfinite(row).any() else np.nan for row in year_table])
    checks = {
        "c1": t_index >= bar,
        "c2": t_random >= bar,
        "c3": alpha > 0,
        "c4": neighbour_min > 0,
        "c5": positive_years >= 0.7,
        "c6": median_years > 0,
        "c7": top_share < 0.30,
        "c8": excess_before > 0,
        "c9": (seed_excess > 0).all(axis=1) & (np.median(seed_t, axis=1) >= bar),
    }
    passed = np.logical_and.reduce(list(checks.values()))

    # 5. 비용 0 판과 민감도
    extra = {}

    for name, capital, ticks, cost in (("비용0", CAPITAL, 1, 0), ("S1_0틱", CAPITAL, 0, 1), ("자본2천만", 20_000_000.0, 1, 1),
                                       ("자본2억", 200_000_000.0, 1, 1)):
        extra[name] = summary_of_run(inputs, family_cells(capital, ticks, cost))
        print(f"{name} 끝 {time.time() - started:.0f}초", flush=True)

    # 6. 판정표
    table = pd.DataFrame({
        "칸": [cell_id(row) for row in cells],
        "신호": [study.SIGNAL_NAMES[int(value)] for value in cells[:, 0]], "종목군": [study.UNIVERSE_NAMES[int(value)] for value in cells[:, 1]],
        "N": cells[:, 2].astype(int), "리밸런싱": [study.REBALANCE_TEXT[int(value)] for value in cells[:, 3]],
        "체결": [study.EXEC_TEXT[int(value)] for value in cells[:, 4]], "버퍼": np.where(cells[:, 5] > 0, "1.5N", "끔"),
        "연수익": return_annual * 100, "지수_연수익": bench_annual * 100, "지수_대비_연초과": excess_annual * 100,
        "달_t": t_month, "해_t": t_year, "t_min": t_index,
        "무작위_대비_연초과": random_annual * 100, "무작위_대비_t_min": t_random,
        "무작위대조_지수_대비_연초과": universe_effect * 100,
        "참고_새로뽑는_무작위_대비_연초과": fresh["annual"] * 100, "참고_새로뽑는_무작위_대비_t_min": fresh["t"],
        "참고_새로뽑는_무작위대조_지수_대비_연초과": fresh["universe"] * 100,
        "베타": beta, "베타조정_연초과": alpha * 100, "옆칸_최소_연초과": neighbour_min * 100,
        "양수_해_비율": positive_years, "해별_중앙값": median_years * 100, "상위10_기여_비중": top_share,
        "2025_26_뺀_연초과": excess_before * 100,
        "시드별_연초과_최소": seed_excess.min(axis=1) * 100, "시드별_t_min_중앙값": np.median(seed_t, axis=1),
        "밤_연로그": scalar[:, 0] / years * 100, "장중_연로그": scalar[:, 1] / years * 100, "비용_연로그": scalar[:, 2] / years * 100,
        "지수_밤_연로그": scalar[:, 3] / years * 100, "지수_장중_연로그": scalar[:, 4] / years * 100,
        "연_회전율": scalar[:, 5] / years, "매수_수": scalar[:, 6], "투자_일_비율": scalar[:, 7] / inputs.day_count,
        "비용0_지수_대비_연초과": extra["비용0"][0] * 100, "비용0_t_min": extra["비용0"][1],
        "S1_0틱_연초과": extra["S1_0틱"][0] * 100, "S1_0틱_t_min": extra["S1_0틱"][1],
        "자본2천만_연초과": extra["자본2천만"][0] * 100, "자본2천만_t_min": extra["자본2천만"][1],
        "자본2억_연초과": extra["자본2억"][0] * 100, "자본2억_t_min": extra["자본2억"][1],
    })

    for name, value in checks.items():
        table[name] = value

    table["통과"] = passed
    rounded = table.copy()

    for column in rounded.columns:
        if rounded[column].dtype.kind == "f":
            rounded[column] = rounded[column].round(4)

    study.OUTPUT.mkdir(parents=True, exist_ok=True)
    common.write_csv(study.OUTPUT / "stage_b_judge.csv", rounded)
    year_frame = pd.DataFrame(np.round(year_table * 100, 4), columns=[str(int(inputs.years[0]) + offset) for offset in range(inputs.year_count)])
    year_frame.insert(0, "칸", table["칸"])
    common.write_csv(study.OUTPUT / "stage_b_years.csv", year_frame)

    # 7. 실패 원인 분해(사전 등록 31행)
    pair_columns = {}

    for axis, (first, second) in (("주간−월간", (3, (1, 0))), ("버퍼켬−끔", (5, (0, 1))), ("다음날시가−같은날종가", (4, (0, 1))),
                                  ("다음날종가−같은날종가", (4, (0, 2)))):
        # 기준 칸(base 값) 에서 한 축만 other 값으로 바꾼 칸의 연 초과 − 기준 칸 연 초과
        column, (low, high) = first, second
        values = np.full(count, np.nan)
        base = cells[:, column] == low

        for row in np.flatnonzero(base):
            other = cells[row].copy()
            other[column] = high
            parts = [int(other[0]), int(other[1]), SLOTS.index(int(other[2])), int(other[3]), int(other[4]), int(other[5])]
            values[row] = excess_annual[row_of(*parts)] - excess_annual[row]

        pair_columns[axis] = values * 100

    def decompose(label: str, mask: np.ndarray) -> dict:
        return {
            "묶음": label, "칸_수": int(mask.sum()),
            "지수_대비_연초과_중앙값": float(np.nanmedian(excess_annual[mask]) * 100),
            "①종목군_무작위대조_지수_대비": float(np.nanmedian(universe_effect[mask]) * 100),
            "신호_효과_무작위_대비": float(np.nanmedian(random_annual[mask]) * 100),
            "②비용_비용0_빼기_S0": float(np.nanmedian(extra["비용0"][0][mask] - excess_annual[mask]) * 100),
            "③밤_바구니_빼기_지수": float(np.nanmedian((scalar[mask, 0] - scalar[mask, 3]) / years) * 100),
            "③장중_바구니_빼기_지수": float(np.nanmedian((scalar[mask, 1] - scalar[mask, 4]) / years) * 100),
            **{f"④{axis}": float(np.nanmedian(values[mask])) for axis, values in pair_columns.items()},
            "⑤t_min_2이상_칸": int((t_index[mask] >= 2).sum()), "⑤t_min_3이상_칸": int((t_index[mask] >= 3).sum()),
            "⑤통과선이상_칸": int((t_index[mask] >= bar).sum()), "통과_칸": int(passed[mask].sum()),
            "c1": int(checks["c1"][mask].sum()), "c2": int(checks["c2"][mask].sum()), "c3": int(checks["c3"][mask].sum()),
            "c4": int(checks["c4"][mask].sum()), "c5": int(checks["c5"][mask].sum()), "c6": int(checks["c6"][mask].sum()),
            "c7": int(checks["c7"][mask].sum()), "c8": int(checks["c8"][mask].sum()), "c9": int(checks["c9"][mask].sum()),
        }

    failure = [decompose("전체", np.ones(count, dtype=bool))]
    failure += [decompose(f"신호 {name}", cells[:, 0] == code) for code, name in enumerate(study.SIGNAL_NAMES)]
    failure += [decompose(f"종목군 {name}", cells[:, 1] == code) for code, name in enumerate(study.UNIVERSE_NAMES)]
    failure_frame = pd.DataFrame(failure)

    for column in failure_frame.columns:
        if failure_frame[column].dtype.kind == "f":
            failure_frame[column] = failure_frame[column].round(4)

    common.write_csv(study.OUTPUT / "stage_b_failure.csv", failure_frame)

    # 8. 대표 칸(사전 등록 23행)
    devscale_row = row_of(study.SIGNAL_NAMES.index(DEVSCALE["signal"]), study.UNIVERSE_NAMES.index(DEVSCALE["universe"]),
                          SLOTS.index(DEVSCALE["slots"]), DEVSCALE["rebalance"], DEVSCALE["execution"], DEVSCALE["buffer"])

    if passed.any():
        chosen = list(np.flatnonzero(passed))
        reason = ["통과"] * len(chosen)
    else:
        chosen, reason = [], []

        for signal in range(SIGNAL_COUNT):
            part = np.flatnonzero(cells[:, 0] == signal)
            chosen.append(int(part[np.nanargmax(t_index[part])]))
            reason.append(f"신호 {study.SIGNAL_NAMES[signal]} 지수 대비 t_min 최대")

    if devscale_row not in chosen:
        chosen.append(devscale_row)
        reason.append("DevScale 근사")
    else:
        reason[chosen.index(devscale_row)] += " + DevScale 근사"

    representative = table.iloc[chosen][["칸", "신호", "종목군", "N", "리밸런싱", "체결", "버퍼", "지수_대비_연초과", "t_min",
                                         "무작위_대비_연초과", "무작위_대비_t_min", "통과"]].copy()
    representative.insert(1, "고른_이유", reason)
    representative.insert(0, "행", chosen)
    representative = representative.round(4)
    common.write_csv(study.OUTPUT / "representative_cells.csv", representative)
    add_post_rows(POST_FIXES + [["사후: 대표 칸 결과", "; ".join(f"{table['칸'].iloc[row]}({why})" for row, why in zip(chosen, reason)),
                    "사전 등록 23행 규칙 적용", "아니오(진단)", "아니오(결과)", "단계 B 결과로 정해짐, 규칙은 실행 전 등록"]])

    daily = {"date": inputs.dates.astype(np.int64)}

    for row in chosen:
        name = table["칸"].iloc[row]
        daily[f"{name}|수익"] = returns[row]
        daily[f"{name}|지수"] = bench[row]
        daily[f"{name}|무작위"] = random_mean[match[row]]

    daily_frame = pd.DataFrame(daily)
    daily_frame.to_parquet(study.OUTPUT / "stage_b_rep_daily.parquet", index=False)

    # 9. 코어(코스피) + 위성 w 격자(사전 등록 32행)
    kospi = np.r_[np.nan, inputs.kospi_close[1:] / inputs.kospi_close[:-1] - 1.0]
    kospi = np.nan_to_num(kospi)
    best_row = int(np.nanargmax(t_index))
    grid_rows = []

    for label, row in (("가족 t_min 최대", best_row), ("DevScale 근사", devscale_row)):
        satellite = np.nan_to_num(returns[row])

        for weight in W_GRID:
            combined = (1.0 - weight) * kospi + weight * satellite
            yearly_return, drawdown, sharpe = study.curve_statistics(combined)
            grid_rows.append({"위성": label, "칸": table["칸"].iloc[row], "w": weight, "연_복리_수익": round(yearly_return, 4),
                              "MDD": round(drawdown, 4), "샤프": round(sharpe, 4),
                              "코스피_대비_연초과": round(float((combined - kospi).mean() * study.YEAR_DAYS * 100), 4)})

    common.write_csv(study.OUTPUT / "stage_b_core_satellite.csv", pd.DataFrame(grid_rows))

    # 10. 수치 요약
    order = np.argsort(-np.nan_to_num(t_index, nan=-99))[:20]
    metrics = {
        "study": "41_universe_basket", "stage": "B", "cells": count, "random_cells": FORM_COUNT * len(study.RANDOM_SEEDS),
        "sensitivity_cells": count * 4, "capital": CAPITAL, "family_bar": bars, "floor": FLOOR, "bar": bar,
        "passed_cells": int(passed.sum()), "check_counts": {name: int(value.sum()) for name, value in checks.items()},
        "t_min_ge_2": int((t_index >= 2).sum()), "t_min_ge_3": int((t_index >= 3).sum()),
        "excess_positive_cells": int((excess_annual > 0).sum()),
        "random_excess_positive_cells": int((random_annual > 0).sum()),
        "top20_by_t_min": [{"칸": table["칸"].iloc[row], "t_min": round(float(t_index[row]), 4),
                            "연초과": round(float(excess_annual[row] * 100), 4),
                            "무작위_대비_t_min": round(float(t_random[row]), 4)} for row in order],
        "representative": [table["칸"].iloc[row] for row in chosen],
        "inputs_sha256": common_inputs_hash(), "git_commit": common.git_commit(), "seed": study.SEED,
        "random_seeds": list(study.RANDOM_SEEDS), "seconds": round(time.time() - started, 1),
    }
    common.write_json(study.OUTPUT / "stage_b_metrics.json", metrics)
    common.write_text(study.OUTPUT / "run_command.txt",
                      "py -I -X utf8 research/studies/41_universe_basket/build_inputs.py\n"
                      "py -I -X utf8 research/studies/41_universe_basket/run_stage_a.py\n"
                      "py -I -X utf8 research/studies/41_universe_basket/run_stage_b.py\n"
                      "py -I -X utf8 research/studies/41_universe_basket/problem_trades.py\n")
    print(f"단계 B 끝 {time.time() - started:.0f}초 · 통과 {int(passed.sum())} 칸 · 통과선 {bar:.3f}", flush=True)


def common_inputs_hash() -> str:
    info = study.CACHE / "inputs_info.json"

    if info.exists():
        payload = json.loads(info.read_text(encoding="utf-8"))
        return payload.get("inputs_sha256", "")

    return ""


if __name__ == "__main__":
    main()
