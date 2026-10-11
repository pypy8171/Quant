"""기준선 다시 내기 1단계: 3–11회차 시험을 그대로 다시 돌리며 부호 뒤집기 분포만 받아 둔다.

왜: 회차마다 칸 × 달 행렬을 파일로 남기지 않았다. 여러 회차의 칸을 한 묶음으로 뒤집으려면
같은 부호(시드 common.SEED, 1,000 회, 달마다 한 부호)로 낸 "뒤집기마다의 최대 t"가 회차별로 있으면 된다 —
묶음 전체의 최대 t 는 회차별 최대 t 의 최대다. 그래서 행렬 대신 round3_test.flip_null 호출마다
뒤집기 1,000 개의 최대 t·최소 로그 확률을 저장한다(거래 든 달 24 개월 이상 칸만 쓴 판도 같이).

원래 결과 파일은 건드리지 않는다: 각 회차 모듈의 OUTPUT 을 임시 폴더 rebaseline_reruns/ 로 바꿔 돌린다.

실행: PYTHONIOENCODING=utf-8 py research/studies/38_trend_support_realtime/rebaseline_capture.py --round 5
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import sys
import time
from pathlib import Path

import common  # numba 설정을 먼저 넣는다

import numpy as np

STUDY = Path(__file__).resolve().parent
CAPTURE = STUDY / "results" / "rebaseline" / "captures"
RERUNS = Path(__import__("tempfile").gettempdir()) / "study38_rebaseline_reruns"   # 다시 돌린 회차의 표는 임시 폴더로(원래 결과와 섞지 않음)
MONTHS_FOR_BAR_A = 24          # 5–11회차 (가) 와 같은 값
SKIP_SUFFIX = "24 개월 이상 칸"  # family_table 이 스스로 부르는 24 개월 판은 따로 받지 않는다(아래에서 같은 것을 낸다)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--round", type=int, required=True)
    arguments = parser.parse_args()
    round_number = arguments.round
    sys.argv = [f"round{round_number}_test.py"]
    import round3_test
    target = importlib.import_module(f"round{round_number}_test")

    for name, module in list(sys.modules.items()):
        if name.startswith("round") and name.endswith("_test") and hasattr(module, "OUTPUT"):
            module.OUTPUT = RERUNS / f"from_round{round_number}" / name.replace("_test", "")

    original = round3_test.flip_null
    output_directory = CAPTURE / f"round{round_number}"
    output_directory.mkdir(parents=True, exist_ok=True)

    for old in output_directory.glob("call_*"):
        old.unlink()

    state = {"index": 0, "calls": []}

    def capturing(month_excess, month_count, signs, label):
        detail, summary, maxima, minimum = original(month_excess, month_count, signs, label)

        if label.endswith(SKIP_SUFFIX):
            return detail, summary, maxima, minimum

        months = (month_count > 0).sum(axis=1)
        long_enough = months >= MONTHS_FOR_BAR_A

        if long_enough.any():
            _, summary_long, maxima_long, minimum_long = original(month_excess[long_enough], month_count[long_enough], signs,
                                                                  label + " [24 개월 이상]")
        else:
            summary_long, maxima_long, minimum_long = {"cells_M": 0, "observed_max_t": float("nan")}, np.full(len(maxima), -np.inf), \
                np.zeros(len(maxima))

        index = state["index"]
        state["index"] += 1
        np.savez(output_directory / f"call_{index:03d}.npz", maxima=maxima, minimum_log_p=minimum, maxima_24=maxima_long,
                 minimum_log_p_24=minimum_long)
        record = {"index": index, "label": label, "cells_M": summary["cells_M"], "observed_max_t": summary["observed_max_t"],
                  "a_bar": summary["a_max_t_null_p95_bar"], "b_bar": summary["b_bar_normal_equivalent_z"],
                  "cells_M_24": summary_long["cells_M"], "observed_max_t_24": summary_long["observed_max_t"],
                  "signs_sha1": hashlib.sha1(np.ascontiguousarray(signs).tobytes()).hexdigest(),
                  "signs_shape": list(signs.shape)}
        state["calls"].append(record)
        (output_directory / "calls.json").write_text(json.dumps(state["calls"], ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"[받음] {index} {label} 칸 {record['cells_M']} / 24 개월 {record['cells_M_24']}", flush=True)
        return detail, summary, maxima, minimum

    round3_test.flip_null = capturing
    started = time.time()
    target.main()
    meta = {"round": round_number, "seconds": time.time() - started, "calls": len(state["calls"]), "finished": True}
    (output_directory / "done.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"[끝] round{round_number} {meta['seconds']:.0f}초, 호출 {meta['calls']}", flush=True)


if __name__ == "__main__":
    main()
