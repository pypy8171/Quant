"""부하시험 구간 지연 판독기 — `logs/latency_trace.csv` 를 구간별 분위수 표로 펴고 두 회차를 견준다.

    py -m tools.load_latency_reader --round N --trace <경로>/latency_trace_N.csv
    py -m tools.load_latency_reader --round peak30k_split --trace <셋으로 가른 판>.csv --baseline-round peak30k_one --baseline <한 프로세스 판>.csv

무엇을 재나. 주문 하나가 지나는 길을 **시세 수신 → 전략 판단 → 주문 전송 → 응답** 으로 갈라서 잰다.
엔진이 신호마다 구간 시각을 트레이스 한 줄로 남기고 있어(Quant/src/core/LatencyTrace.cpp 의 csv_header),
여기서는 그 열을 사람이 읽는 순서로 세워 분위수를 낸다. 한 덩이 숫자 하나만 있으면 느려진 자리를 못 짚는다.

왜 회차 견주기가 붙어 있나. 프로세스를 가르면(D-114) 전략 쪽이 만든 신호가 경계를 한 번 더 건너 주문 쪽으로
간다. 늘어난 홉은 `signal_to_pop` 한 칸에 그대로 얹히므로, 같은 시나리오를 한 프로세스로 돌린 회차와
칸별로 견주면 분리가 어디에 얼마를 얹었는지가 한 줄로 나온다.

출력은 ASCII 로만 쓴다 — 콘솔이 cp949 라 한글 print 가 터진 적이 있다(09-23 회차 J프라임).
"""

from __future__ import annotations

import argparse
import csv
import sys
from array import array
from pathlib import Path

# 표에 세우는 순서. (CSV 열 이름, 사람이 읽는 이름, 이 칸이 다른 칸 안에 든 몫인지)
# 마지막 칸이 참이면 합에 두 번 들어가므로 "안에 든 몫" 으로 따로 적는다 — 정본은 엔진 csv_header 주석이다.
SEGMENTS = (
    ("tick_to_signal_us", "tick recv -> strategy decision", False),
    ("signal_to_pop_us", "strategy decision -> order thread pop", False),
    ("gate_us", "order gate check", True),
    ("history_guard_us", "order history lock + duplicate guard", True),
    ("history_lock_wait_us", "  of which: waiting for the lock", True),
    ("journal_us", "ledger write-ahead (disk)", True),
    ("bucket_wait_us", "broker rate limit queueing", True),
    ("transport_us", "order send -> broker response (round trip)", True),
    ("record_us", "after send: accept, publish, history, ledger csv", True),
    ("open_orders_us", "  of which: pending-order file rewrite", True),
    ("pop_to_done_us", "pop -> router return (holds the six above)", False),
    ("total_us", "tick recv -> router return (whole chain)", False),
    ("previous_tail_us", "router return -> loop end (holds the three below)", False),
    ("previous_trace_us", "  of which: latency csv write + percentile add", True),
    ("previous_post_us", "  of which: paper fill tick, ops broadcast, answer", True),
    ("previous_publish_us", "  of which: ledger snapshot publish", True),
    ("previous_wait_us", "loop end -> next pop (order thread idle, queue empty)", False),
)

# 견줄 때 통과선. 회차 계획서 3절의 성능 판정선과 같은 값이다.
PERCENTILE_LIMITS = {"p50": 1.10, "p99": 1.20}

PERCENTILES = (0.50, 0.90, 0.99)


def read_segments(trace_path: Path) -> dict[str, array]:
    """트레이스 CSV 를 구간별 값 배열로. -1 은 "그 지점을 안 지났다" 라 뺀다(게이트 거부는 원장·전송이 없다).

    값은 `array('q')` 에 담는다 — 회차 하나가 수백만 줄이라 리스트로 들면 한 줄에 파이썬 객체 하나가 붙는다.
    """
    columns: dict[str, array] = {name: array("q") for name, _, _ in SEGMENTS}

    with open(trace_path, "r", encoding="utf-8", errors="replace", newline="") as handle:
        for row in csv.DictReader(handle):
            for name in columns:
                text = row.get(name)

                if not text:
                    continue

                try:
                    value = int(text)
                except ValueError:
                    continue

                if value >= 0:
                    columns[name].append(value)

    return columns


def percentile(values: array, ratio: float) -> int | None:
    """정렬된 값에서 분위수. 표본이 없으면 None. 칸(히스토그램)이 아니라 원값이라 근사가 아니다."""
    if not values:
        return None

    index = int(ratio * (len(values) - 1))

    return values[index]


def summarize(columns: dict[str, array]) -> dict[str, dict[str, int | None]]:
    """구간마다 표본 수와 분위수. 정렬은 여기서 한 번만 한다."""
    summary: dict[str, dict[str, int | None]] = {}

    for name, values in columns.items():
        ordered = array("q", sorted(values))
        summary[name] = {
            "count": len(ordered),
            "p50": percentile(ordered, 0.50),
            "p90": percentile(ordered, 0.90),
            "p99": percentile(ordered, 0.99),
            "max": ordered[-1] if ordered else None,
        }

    return summary


def format_number(value: int | None) -> str:
    return "-" if value is None else f"{value:,}"


def render_table(round_name: str, summary: dict[str, dict[str, int | None]]) -> list[str]:
    """한 회차의 구간 표. us 단위. 이 표 자체가 회차 문서에 그대로 들어간다."""
    lines = [
        f"=== round {round_name} - per-segment latency (us) ===",
        f"{'segment':50s} {'count':>10s} {'p50':>9s} {'p90':>9s} {'p99':>9s} {'max':>11s}",
    ]

    for name, label, inside in SEGMENTS:
        statistics = summary.get(name)

        if statistics is None or statistics["count"] == 0:
            continue

        marker = " *" if inside else "  "
        lines.append(
            f"{label:48s}{marker} {format_number(statistics['count']):>10s} "
            f"{format_number(statistics['p50']):>9s} {format_number(statistics['p90']):>9s} "
            f"{format_number(statistics['p99']):>9s} {format_number(statistics['max']):>11s}"
        )

    lines.append("  * = a share of the row below it (pop -> router return); do not add these twice")

    return lines


def render_comparison(round_name: str, summary: dict[str, dict[str, int | None]],
                      baseline_name: str, baseline: dict[str, dict[str, int | None]]) -> tuple[list[str], bool]:
    """회차 둘을 칸별로 견준다. (줄 목록, 통과 여부).

    통과선은 회차 계획서와 같다 — p50 은 기준선의 +10% 안, p99 는 +20% 안. 프로세스를 가르며 홉이 하나
    늘었으니 꼬리가 더 흔들리는 것은 미리 받아들인 값이다. 통과선을 넘은 칸이 곧 분리가 값을 치른 자리다.
    """
    lines = [
        f"=== {round_name} vs {baseline_name} - what the split cost, per segment ===",
        f"{'segment':46s} {'base p50':>9s} {'p50':>9s} {'d%':>7s} {'base p99':>9s} {'p99':>9s} {'d%':>7s}  verdict",
    ]
    passed = True

    for name, label, _ in SEGMENTS:
        statistics = summary.get(name)
        base = baseline.get(name)

        if statistics is None or base is None or statistics["count"] == 0 or base["count"] == 0:
            continue

        cells = []
        verdict = "ok"

        for key in ("p50", "p99"):
            current = statistics[key]
            reference = base[key]

            if current is None or reference is None:
                cells.append((reference, current, None))
                continue

            ratio = (current / reference) if reference > 0 else None
            cells.append((reference, current, ratio))

            if ratio is not None and reference > 0 and ratio > PERCENTILE_LIMITS[key]:
                verdict = f"OVER {key} limit"
                passed = False

        pieces = []

        for reference, current, ratio in cells:
            change = "-" if ratio is None else f"{(ratio - 1.0) * 100:+.0f}%"
            pieces.append(f"{format_number(reference):>9s} {format_number(current):>9s} {change:>7s}")

        lines.append(f"{label:46s} {' '.join(pieces)}  {verdict}")

    lines.append("")
    lines.append("verdict: " + ("pass - the split did not slow any segment past the line"
                                if passed else "FAIL - a segment went past the line above"))

    return lines, passed


def parse_arguments(argument_list: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="load test per-segment latency reader")
    parser.add_argument("--round", default="", help="round name shown on the table")
    parser.add_argument("--trace", required=True, help="latency_trace csv for this round")
    parser.add_argument("--baseline-round", default="", help="round name of the baseline")
    parser.add_argument("--baseline", default="", help="latency_trace csv to compare against")
    parser.add_argument("--output", default="", help="write the per-segment summary as csv")

    return parser.parse_args(argument_list)


def write_summary_csv(output_path: Path, round_name: str,
                      summary: dict[str, dict[str, int | None]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with open(output_path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["round", "segment", "label", "inside_pop_to_done", "count", "p50_us",
                         "p90_us", "p99_us", "max_us"])

        for name, label, inside in SEGMENTS:
            statistics = summary.get(name)

            if statistics is None or statistics["count"] == 0:
                continue

            writer.writerow([round_name, name, label, int(inside), statistics["count"],
                             statistics["p50"], statistics["p90"], statistics["p99"], statistics["max"]])


def main(argument_list: list[str] | None = None) -> int:
    arguments = parse_arguments(argument_list)
    trace_path = Path(arguments.trace)

    if not trace_path.exists():
        print(f"no trace file: {trace_path}", file=sys.stderr)

        return 1

    summary = summarize(read_segments(trace_path))

    if not any(statistics["count"] for statistics in summary.values()):
        print(f"trace has no usable rows: {trace_path}", file=sys.stderr)

        return 1

    for line in render_table(arguments.round or trace_path.stem, summary):
        print(line)

    if arguments.output:
        write_summary_csv(Path(arguments.output), arguments.round, summary)
        print(f"csv written: {arguments.output}")

    if arguments.baseline:
        baseline_path = Path(arguments.baseline)

        if not baseline_path.exists():
            print(f"no baseline trace: {baseline_path}", file=sys.stderr)

            return 1

        baseline = summarize(read_segments(baseline_path))
        print("")
        lines, passed = render_comparison(arguments.round, summary,
                                          arguments.baseline_round or baseline_path.stem, baseline)

        for line in lines:
            print(line)

        return 0 if passed else 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
