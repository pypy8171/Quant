"""기준선 다시 내기 2단계: 3–11회차에 판정한 칸 전부를 한 묶음으로 같은 부호 뒤집기에 넣는다.

입력: rebaseline_capture.py 가 회차마다 남긴 뒤집기 1,000 개의 최대 t·최소 로그 확률
      (results/rebaseline/captures/roundN/), 11회차 periods.csv(첫 실행 판정 값 = 대조 자기 비용 판).
방식: 모든 회차가 같은 부호 행렬(시드 common.SEED, 1,000 × 202 달)을 쓴다 — 저장한 부호 해시로 확인한다.
      묶음의 뒤집기마다 최대 t = 회차별 최대 t 의 최대. (가) = 그 95% 분위(거래 든 달 24 개월 이상 칸), (나) = 최소 확률 5% 분위의 정규 환산 z.
산출: results/rebaseline/ all_cells_bar.json, cells_by_round.csv, new_judgement_9_11.csv, choices_log.csv, rebaseline_metrics.json

실행: PYTHONIOENCODING=utf-8 py research/studies/38_trend_support_realtime/rebaseline.py
"""
from __future__ import annotations

import json
import math
import time
from pathlib import Path

import common  # numba 설정을 먼저 넣는다

import numpy as np
import pandas as pd

import round3_test

STUDY = Path(__file__).resolve().parent
RESULTS = STUDY / "results"
OUTPUT = RESULTS / "rebaseline"
CAPTURE = OUTPUT / "captures"
ROUNDS = range(3, 12)
QUANTILE = round3_test.NULL_QUANTILE
NORMAL_SEED_OFFSET = 1          # 내가 정함: 정규 근사 판의 난수는 common.SEED + 1
POSITIVE_YEARS = 0.7            # 지시(감사 제안)
FAMILY8_BAR = 4.16              # 11회차에 쓴 값, 비교용


def role_of(round_number: int, label: str) -> str:
    """판정 칸(main)인지, 참고 층(reference)인지, 이미 다른 호출에 든 부분 묶음(subset)인지."""
    if round_number in (3, 4) and label.startswith("가장 잘은 칸"):
        return "reference"      # 3·4회차의 잘은 칸 층은 판정 표가 아니라 참고 층이었다

    if round_number == 4 and label.startswith("판 ") and label.endswith(" 만"):
        return "subset"         # 판정 표 칸(판 0–5)의 부분 묶음

    if round_number == 9 and not label.startswith("A 전부"):
        return "reference"      # 9회차 판정은 A 판, B·C 는 생존편향 진단판

    return "main"


def original_summaries(round_number: int) -> list[dict]:
    """원래 multiple_comparison.json 안의 요약(칸 수·관측 최대 t)을 전부 꺼낸다."""
    found = []

    def walk(node):
        if isinstance(node, dict):
            if ("observed_max_t" in node or "bar_b_normal_equivalent_z" in node or "b_bar_normal_equivalent_z" in node)                     and ("cells_M" in node or "cells_M_all" in node):
                found.append(node)

            for value in node.values():
                walk(value)

        elif isinstance(node, list):
            for value in node:
                walk(value)

    for name in ("multiple_comparison.json", f"round{round_number}_metrics.json"):   # 6회차의 5회차 고침판 요약은 metrics 쪽에 있다
        walk(json.loads((RESULTS / f"round{round_number}" / name).read_text(encoding="utf-8")))

    return found


def matches(record: dict, summaries: list[dict]) -> bool:
    for summary in summaries:
        cells = summary.get("cells_M", summary.get("cells_M_all"))

        if cells != record["cells_M"]:
            continue

        if "observed_max_t" in summary and abs(summary["observed_max_t"] - record["observed_max_t"]) < 1e-6:
            return True

        bar_b = summary.get("bar_b_normal_equivalent_z", summary.get("b_bar_normal_equivalent_z"))

        if bar_b is not None and abs(bar_b - record["b_bar"]) < 1e-9:     # 관측 최대 t 를 안 남긴 요약은 (나) 기준선으로 맞춘다
            return True

    return False


def first_run_round11(signs: np.ndarray):
    """11회차 첫 실행 판정 값(대조가 자기 거래대금으로 충격을 문 판) 16 칸을 periods.csv 에서 다시 만들어 뒤집는다."""
    periods = pd.read_csv(RESULTS / "round11" / "periods.csv")
    judged = periods[(periods["impact"] == "선형 0.3") & np.isfinite(periods["value_control_own_cost"])]
    keys = list(judged.groupby(["hold_days", "control", "mode", "absolute_bin"]).groups)
    cell_of = {key: index for index, key in enumerate(keys)}
    cell = np.array([cell_of[key] for key in zip(judged["hold_days"], judged["control"], judged["mode"], judged["absolute_bin"])])
    month_sum = np.zeros((len(keys), signs.shape[1]))
    month_n = np.zeros((len(keys), signs.shape[1]))
    np.add.at(month_sum, (cell, judged["month"].to_numpy()), judged["value_control_own_cost"].to_numpy())
    np.add.at(month_n, (cell, judged["month"].to_numpy()), 1.0)
    _, summary, maxima, minimum = round3_test.flip_null(month_sum, month_n, signs, "11회차 첫 실행(대조 자기 비용) 16 칸")
    months = (month_n > 0).sum(axis=1)
    long_enough = months >= 24
    _, summary_long, maxima_long, minimum_long = round3_test.flip_null(month_sum[long_enough], month_n[long_enough], signs,
                                                                       "11회차 첫 실행 [24 개월 이상]")
    record = {"round": 11, "index": 900, "label": "11회차 첫 실행(대조 자기 비용) 16 칸", "role": "main",
              "cells_M": summary["cells_M"], "observed_max_t": summary["observed_max_t"], "cells_M_24": summary_long["cells_M"],
              "observed_max_t_24": summary_long["observed_max_t"], "a_bar": summary["a_max_t_null_p95_bar"],
              "b_bar": summary["b_bar_normal_equivalent_z"], "reproduced": "periods.csv 에서 다시 만듦"}
    return record, maxima, minimum, maxima_long


def bars(maxima_24: np.ndarray, maxima_all: np.ndarray, minimum: np.ndarray) -> dict:
    log_p_bar = float(np.quantile(minimum, 1.0 - QUANTILE))
    return {"a_bar_24_months": float(np.quantile(maxima_24, QUANTILE)), "a_null_p50_24_months": float(np.median(maxima_24)),
            "a_bar_all_cells_reference": float(np.quantile(maxima_all, QUANTILE)),
            "b_bar_normal_equivalent_z": round3_test.normal_equivalent(log_p_bar)}


def main() -> None:
    started = time.time()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    generator = np.random.default_rng(common.SEED)
    month_count = None
    records, maxima_24, maxima_all, minima = [], [], [], []
    missing_rounds = []

    for round_number in ROUNDS:
        folder = CAPTURE / f"round{round_number}"

        if not (folder / "done.json").exists():
            missing_rounds.append(round_number)
            continue

        summaries = original_summaries(round_number)

        for record in json.loads((folder / "calls.json").read_text(encoding="utf-8")):
            arrays = np.load(folder / f"call_{record['index']:03d}.npz")
            month_count = month_count or record["signs_shape"][1]
            record = dict(record, round=round_number, role=role_of(round_number, record["label"]),
                          reproduced="일치" if matches(record, summaries) else "불일치")
            records.append(record)
            maxima_all.append(arrays["maxima"])
            maxima_24.append(arrays["maxima_24"])
            minima.append(arrays["minimum_log_p"])

    signs = np.where(generator.random((round3_test.FLIPS, month_count)) < 0.5, -1.0, 1.0)
    sign_hash = __import__("hashlib").sha1(np.ascontiguousarray(signs).tobytes()).hexdigest()
    first, first_max, first_min, first_max_24 = first_run_round11(signs)
    first["signs_sha1"] = sign_hash
    records.append(first)
    maxima_all.append(first_max)
    maxima_24.append(first_max_24)
    minima.append(first_min)
    frame = pd.DataFrame(records)
    same_signs = bool((frame["signs_sha1"] == sign_hash).all())
    main_rows = (frame["role"] == "main").to_numpy()
    stack_24 = np.vstack(maxima_24)
    stack_all = np.vstack(maxima_all)
    stack_min = np.vstack(minima)
    main_bars = bars(stack_24[main_rows].max(axis=0), stack_all[main_rows].max(axis=0), stack_min[main_rows].min(axis=0))
    with_reference = (frame["role"] != "subset").to_numpy()
    reference_bars = bars(stack_24[with_reference].max(axis=0), stack_all[with_reference].max(axis=0),
                          stack_min[with_reference].min(axis=0))
    # 다시 낼 수 없는 칸: 원래 요약과 맞지 않는 호출의 칸 + 캡처가 없는 회차
    unreproduced = int(frame.loc[main_rows & (frame["reproduced"] == "불일치"), "cells_M_24"].sum())
    normal_generator = np.random.default_rng(common.SEED + NORMAL_SEED_OFFSET)
    normal_max = np.full(round3_test.FLIPS, -np.inf)

    for begin in range(0, unreproduced, 20000):
        size = min(20000, unreproduced - begin)
        normal_max = np.maximum(normal_max, normal_generator.standard_normal((round3_test.FLIPS, size)).max(axis=1))

    sensitivity = {}   # 참고: 어느 묶음이 기준선을 끌어올리는지(판정에는 main 만 쓴다)

    for name, chosen in (("5회차 k 별 판(K0–K2) 뺌", main_rows & ~((frame["round"] == 5) & frame["label"].str.startswith("K")).to_numpy()),
                         ("9–11회차만", main_rows & (frame["round"] >= 9).to_numpy()),
                         ("8–11회차만", main_rows & (frame["round"] >= 8).to_numpy())):
        sensitivity[name] = dict(bars(stack_24[chosen].max(axis=0), stack_all[chosen].max(axis=0), stack_min[chosen].min(axis=0)),
                                 cells_M_24=int(frame.loc[chosen, "cells_M_24"].sum()))

    supplemented = np.maximum(stack_24[main_rows].max(axis=0), normal_max)
    main_bar = main_bars["a_bar_24_months"]
    result = {"method": "3–11회차 판정 칸 한 묶음, 같은 부호(시드 common.SEED, 1,000 회, 달마다 한 부호를 모든 칸에 같이 곱함)",
              "signs_sha1": sign_hash, "all_calls_same_signs": same_signs, "flips": round3_test.FLIPS, "months": month_count,
              "missing_rounds": missing_rounds,
              "main": dict(main_bars, cells_M=int(frame.loc[main_rows, "cells_M"].sum()),
                           cells_M_24=int(frame.loc[main_rows, "cells_M_24"].sum()),
                           observed_max_t_24=float(frame.loc[main_rows, "observed_max_t_24"].max())),
              "main_plus_reference_layers": dict(reference_bars, cells_M=int(frame.loc[with_reference, "cells_M"].sum()),
                                                 cells_M_24=int(frame.loc[with_reference, "cells_M_24"].sum())),
              "sensitivity_reference_only": sensitivity,
              "unreproduced_cells_24": unreproduced,
              "normal_supplement": {"cells_added": unreproduced, "seed": common.SEED + NORMAL_SEED_OFFSET,
                                    "a_bar_24_months": float(np.quantile(supplemented, QUANTILE)),
                                    "note": "다시 낸 칸이 원래와 다르거나 없는 만큼 독립 표준정규 최대를 보탬(독립 가정이라 높게 나오는 쪽)"},
              "bar_primary": main_bar, "bar_round11_used": FAMILY8_BAR,
              "cells_counted_twice_note": "같은 칸을 여러 회차가 다시 낸 경우(6회차의 5회차 고침판 등)도 그대로 넣음 — 같은 칸은 최대 t 를 올리지 않는다"}
    (OUTPUT / "all_cells_bar.json").write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
    columns = ["round", "index", "label", "role", "reproduced", "cells_M", "cells_M_24", "observed_max_t", "observed_max_t_24",
               "a_bar", "b_bar"]
    common.write_csv(OUTPUT / "calls.csv", frame[columns])
    by_round = frame.groupby(["round", "role"]).agg(calls=("label", "size"), cells_M=("cells_M", "sum"), cells_M_24=("cells_M_24", "sum"),
                                                     observed_max_t_24=("observed_max_t_24", "max"),
                                                     own_bar_a_max=("a_bar", "max"),
                                                     unreproduced_calls=("reproduced", lambda value: int((value == "불일치").sum())))
    common.write_csv(OUTPUT / "cells_by_round.csv", by_round.reset_index())
    # 9–11회차 칸 새 판정
    tables = []

    for round_number, keys in ((9, ["hold_days", "order_amount_krw", "absolute_label"]),
                               (10, ["hold_days", "control", "absolute_label"]),
                               (11, ["hold_days", "control", "mode", "absolute_label"])):
        table = pd.read_csv(RESULTS / f"round{round_number}" / "cell_table.csv")

        if round_number == 9 and "judged" in table:
            table = table[table["judged"].astype(bool)]

        table = table[np.isfinite(table["t_excess"])].reset_index(drop=True)   # 매매가 없어 t 가 없는 칸은 판정 대상 아님

        old_pass = table["passes_final"] if "passes_final" in table else table["passes"]
        new_pass = ((table["t_excess"] >= main_bar) & table["neighbours_all_same_sign"].astype(bool)
                    & (table["share_positive_excess_years"] >= POSITIVE_YEARS) & (table["mean_excess"] > 0))
        part = table[keys + ["rebalances", "mean_excess", "t_excess", "share_positive_excess_years", "neighbours_all_same_sign"]].copy()
        part.insert(0, "round", round_number)
        part["old_pass"] = old_pass.astype(bool).to_numpy()
        part["new_bar"] = main_bar
        part["new_pass"] = new_pass.to_numpy()
        part["headline"] = table["headline"].to_numpy() if "headline" in table else True
        tables.append(part)

    judgement = pd.concat(tables, ignore_index=True)
    common.write_csv(OUTPUT / "new_judgement_9_11.csv", judgement)
    history = pd.read_csv(RESULTS / "round11" / "choices_log.csv")
    rows = pd.DataFrame([
        {"round": "기준선", "change": "3–11회차 판정 칸 한 묶음 부호 뒤집기로 기준선 다시 냄", "decided_by": "사용자 승인(지시)",
         "chosen_after_seeing_results": "예(11회차까지 본 뒤)", "comparisons_added": "–", "note": f"(가) 주 기준선 {main_bar:.3f}"},
        {"round": "기준선", "change": "(가)는 거래 든 달 24 개월 이상 칸만", "decided_by": "내가 정함(5–11회차 방식 유지)",
         "chosen_after_seeing_results": "아니오", "comparisons_added": "–", "note": "전 칸 판은 참고값으로 같이 냄"},
        {"round": "기준선", "change": "판정 칸 범위: 3·4회차 판정 표 칸, 5–8·10·11회차 가족 전 칸, 9회차 A 판, 11회차 첫 실행판",
         "decided_by": "내가 정함", "chosen_after_seeing_results": "아니오", "comparisons_added": "–",
         "note": "3·4회차 잘은 칸 층·9회차 B·C 판은 참고 묶음에만"},
        {"round": "기준선", "change": f"정규 근사 판 난수 시드 common.SEED+{NORMAL_SEED_OFFSET}", "decided_by": "내가 정함",
         "chosen_after_seeing_results": "아니오", "comparisons_added": "–", "note": "다시 낼 수 없는 칸 수만큼 독립 표준정규"},
        {"round": "기준선", "change": "새 판정 조건: t ≥ 새 (가), 옆 구간 같은 부호, 양수 해 ≥ 0.7, 평균 > 0",
         "decided_by": "지시(감사 제안)", "chosen_after_seeing_results": "아니오", "comparisons_added": "–", "note": "(나)는 참고"},
    ])
    common.write_csv(OUTPUT / "choices_log.csv", pd.concat([history, rows], ignore_index=True))
    metrics = {"seconds": time.time() - started, "seed": common.SEED, "rounds": list(ROUNDS), "missing_rounds": missing_rounds,
               "bar_primary": main_bar, "new_passes": int(judgement["new_pass"].sum()), "old_passes": int(judgement["old_pass"].sum()),
               "rerun": ["for r in 3 4 5 6 7 8 9 10 11: py rebaseline_capture.py --round r", "py rebaseline.py"]}
    (OUTPUT / "rebaseline_metrics.json").write_text(json.dumps(metrics, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=1))
    print(by_round.to_string())
    print(judgement.to_string())


if __name__ == "__main__":
    main()
