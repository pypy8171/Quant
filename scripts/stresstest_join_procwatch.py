"""부하 회차 CSV(bench_engine_load)와 자원 수집기 로그(procwatch)를 시각으로 맞춘다.

bench_engine_load의 마지막 열 started_at(HH:MM:SS)과 logs/procwatch_<이름>.log의 `cpu=` 줄을 겹치는 구간으로 묶어
구성마다 CPU 평균·최대(코어 수로 환산)·스레드 최대를 낸다. 표본이 없는 구성은 빈칸으로 남긴다 — 0으로 채우면
"CPU를 안 썼다"로 읽힌다.

쓰는 법(저장소 루트에서):
  py scripts/stresstest_join_procwatch.py docs/reports/stresstest/data/2026-09-22_A_cpu_sampled.csv \
      logs/procwatch_bench_engine_load.log --samples-out docs/reports/stresstest/data/2026-09-22_A_procwatch_samples.csv

psutil의 cpu_percent(interval=N)은 "직전 N초"를 돌려주므로 표본 시각 t는 [t-N, t] 구간을 대표한다.
그래서 구성 시작 뒤 1초부터 (시작 + seconds + 1초)까지의 표본만 그 구성의 것으로 본다.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from datetime import datetime, timedelta
from pathlib import Path

SAMPLE_PATTERN = re.compile(
    r"^(?P<stamp>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}).*?cpu=(?P<cpu>[\d.]+)% mem=(?P<mem>\d+)MB threads=(?P<threads>\d+)"
)


def read_samples(log_path: Path) -> list[dict]:
    samples = []
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = SAMPLE_PATTERN.match(line)
        if not match:
            continue
        samples.append({
            "at": datetime.strptime(match["stamp"], "%Y-%m-%d %H:%M:%S"),
            "cpu_percent": float(match["cpu"]),
            "memory_mb": int(match["mem"]),
            "threads": int(match["threads"]),
        })
    return samples


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bench_csv", type=Path)
    parser.add_argument("procwatch_log", type=Path)
    parser.add_argument("--samples-out", type=Path, help="수집기 표본을 CSV로 따로 남길 경로(원자료 보존)")
    parser.add_argument("--core-count", type=int, default=16, help="논리 코어 수 — cpu%%를 코어 개수로 나눌 때 쓴다")
    arguments = parser.parse_args()

    samples = read_samples(arguments.procwatch_log)
    if not samples:
        print(f"표본이 없다: {arguments.procwatch_log}", file=sys.stderr)
        return 1

    if arguments.samples_out:
        arguments.samples_out.parent.mkdir(parents=True, exist_ok=True)
        with arguments.samples_out.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(["at", "cpu_percent", "memory_mb", "threads"])
            for sample in samples:
                writer.writerow([sample["at"].strftime("%H:%M:%S"), sample["cpu_percent"], sample["memory_mb"], sample["threads"]])

    run_date = samples[0]["at"].date()
    with arguments.bench_csv.open(encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

    print("lanes,shards,order_every,rate,accepted_per_sec,drop_pct,orders_per_sec,cpu_samples,cpu_avg_cores,cpu_max_cores,threads_max,started_at")
    for row in rows:
        if "started_at" not in row or not row["started_at"]:
            print(f"# started_at 열이 없다 — 이 CSV는 시각 열 추가 전 하네스로 만들어졌다: {arguments.bench_csv}", file=sys.stderr)
            return 1

        started = datetime.combine(run_date, datetime.strptime(row["started_at"], "%H:%M:%S").time())
        window_from = started + timedelta(seconds=1)
        window_to   = started + timedelta(seconds=int(row["seconds"]) + 1)
        in_window   = [sample for sample in samples if window_from <= sample["at"] <= window_to]

        if in_window:
            cpu_average = sum(sample["cpu_percent"] for sample in in_window) / len(in_window) / 100.0
            cpu_max = max(sample["cpu_percent"] for sample in in_window) / 100.0
            threads_max = max(sample["threads"] for sample in in_window)
            cpu_columns = f"{len(in_window)},{cpu_average:.2f},{cpu_max:.2f},{threads_max}"
        else:
            cpu_columns = "0,,,"

        print(",".join([
            row["lanes"], row["shards"], row["order_every"], row["rate"],
            row["accepted_per_sec"], row["drop_pct"], row["orders_per_sec"],
            cpu_columns, row["started_at"],
        ]))

    return 0


if __name__ == "__main__":
    sys.exit(main())
