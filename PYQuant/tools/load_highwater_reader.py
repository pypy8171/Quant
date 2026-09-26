"""부하시험 큐 고수위 판독기 — 역할별 실행 로그의 `[큐 고수위]` 줄을 CSV 로 펴고 안전성 판정을 낸다.

    py -m tools.load_highwater_reader --round N --output <경로>.csv
    py -m tools.load_highwater_reader --round N --log-dir out/build/x64-release/Quant/logs --judge

왜 로그인가. 갈라 띄우면(D-114) 요청·응답 소켓은 **주문 쪽만** 연다 — 전략·시세 프로세스는 PUB·REP·운영단말
어느 포트도 바인드하지 않는다(Quant/src/core/Engine.cpp 의 `zmq_enabled_ && runs_order_side()`).
그래서 요청·응답 소켓으로 상태를 묻는 방식(옛 load_status_sampler, 지웠다)으로는 그 둘을 못 읽는다. 대신 프로세스마다 실행 로그를 제 파일에
따로 쓰고(quant_trader.order.log · quant_trader.strategy.log · quant_trader.feed.log), 1분마다 나오는 `[큐 고수위]` 줄에
분리판 안전성 계수기가 그대로 실려 있다.

그 줄의 계수기 중 **0 이어야 하는 것**은 엔진 주석이 정본이다(Quant/src/core/Engine.cpp 의 큐 고수위
LOG_INFO). 여기서는 그 목록을 KMustBeZero 로 옮겨 적고 판정만 한다 — 0 이 아닌 칸이 하나라도 있으면
분리판 회차는 안전성 불합격이다.

계수기는 누적값이라 마지막 줄이 회차 전체의 합이다. 표본 여러 줄을 남기는 것은 어느 구간에서 늘었는지
보려는 것이다.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

# scripts/_logdir.py 가 역할별 로그 이름과 찾는 자리의 정본이다. 이 파일은 <저장소>/PYQuant/tools/ 에 있다.
_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPOSITORY_ROOT / "scripts"))

import _logdir  # noqa: E402  (경로를 넣은 뒤라야 들어온다)

# 줄머리 시각 폭. `2026-09-23 22:30:00` 열아홉 글자다.
TIMESTAMP_WIDTH = 19

HIGH_WATER_MARK = "[큐 고수위]"

# `이름=값` 을 뽑는다. 값은 `12/4096` · `37ms` · 맨수가 다 온다 — 뒤엣것은 뽑은 뒤에 가른다.
FIELD_PATTERN = re.compile(r"(?P<name>[a-z_]+)=(?P<value>[^\s]+)")

# 0 이 아니면 안전성 불합격인 칸. 정본은 Quant/src/core/Engine.cpp 의 큐 고수위 줄 주석이다.
#  ledger_foreign  — 장부 사본에 못 실은 남의 계좌 줄
#  control_*       — 제어 요청을 못 보냈거나, 경계 너머로 못 옮겼거나, 반쪽 표로 보고 버린 줄
#  symbol_*        — 티커→번호 등록을 주문 쪽에서 못 받았거나, 표에 없는 티커로 불린 수
#  strategy_*      — 전략 이름표 등록을 못 받은 수(quant-53 이 뒤에 실었다. 없는 판이면 그냥 빠진다)
#  watch_overflow  — 상한에 밀려 소켓에 못 건 종목(그 종목은 틱이 영영 안 온다)
#  feed_channel_*  — 시세 통로가 차서 못 넘겼거나, 값이 말이 안 돼 꺼내는 쪽이 버린 건수
#                    이 둘은 버린 건수다 — 기다린 횟수가 아니다. 보낸 수(sent)와 받은 수(received)가
#                    같아도 유실이 없는 것이 아니다 — 둘 다 push 가 성공한 것만 세기 때문이다
#                    (Quant/src/ipc/MarketFeedChannel.cpp push_trade). 2026-09-26 확인.
#  fill_channel_*  — 체결 통로(시세→주문)가 차서 못 넘겼거나, 값이 말이 안 돼 버린 체결통보 건수.
#                    여기서 새면 주문 쪽 선점분이 안 풀려 총노출을 이중계상한다(D-114 단계 5)
MUST_BE_ZERO = (
    "ledger_foreign",
    "control_dropped",
    "control_relay_dropped",
    "control_discarded",
    "symbol_register_timeout",
    "symbol_lookup_miss",
    "strategy_register_timeout",
    "watch_overflow",
    "feed_channel_overflow",
    "feed_channel_discarded",
    "fill_channel_overflow",
    "fill_channel_discarded",
)

# 0 이 아닐 수 있지만 both 판과 견줘야 하는 칸. 늘었으면 분리가 가져온 값이다.
COMPARE_WITH_BOTH = (
    "shard_dropped",
    "fill_dropped",
    "order_dropped",
    "order_stale",
    "order_duplicate",
    "order_implausible",
    "order_response_dropped",
    "fill_channel_sent",
    "fill_channel_received",
    "beat_gap_max",
)

# 이중 발주 판정. 이 칸이 0 이 아니면 같은 주문이 두 번 만들어졌다는 뜻이라 따로 크게 알린다.
DOUBLE_ORDER_FIELD = "order_duplicate"


def parse_high_water_line(line: str) -> dict[str, str]:
    """`[큐 고수위]` 줄 하나를 {이름: 값} 으로. 값은 문자열 그대로 둔다 — `12/4096` 처럼 둘이 붙은 칸이 있다."""
    body = line.split(HIGH_WATER_MARK, 1)[1]

    return {match.group("name"): match.group("value") for match in FIELD_PATTERN.finditer(body)}


def to_count(value: str) -> int | None:
    """계수기 값을 수로. `12/4096` 은 앞칸, `37ms` 는 단위를 뗀다. 못 읽으면 None — 판정에서 뺀다."""
    head = value.split("/", 1)[0]

    if head.endswith("ms"):
        head = head[:-2]

    try:
        return int(head)
    except ValueError:
        return None


def read_role_log(path: Path, role: str, round_name: str) -> list[dict[str, str]]:
    """로그 한 파일에서 `[큐 고수위]` 줄을 모두 뽑아 행으로. 시각·역할·회차를 앞에 붙인다."""
    rows: list[dict[str, str]] = []

    with _logdir.open_log(path) as handle:
        for line in handle:
            if HIGH_WATER_MARK not in line:
                continue

            row: dict[str, str] = {
                "round": round_name,
                "role": role,
                "timestamp": line[:TIMESTAMP_WIDTH],
            }
            row.update(parse_high_water_line(line))
            rows.append(row)

    return rows


def collect(log_directory: Path | None, round_name: str) -> list[dict[str, str]]:
    """역할별 로그를 다 읽어 행으로. 한 프로세스로 돌았으면 role 이 `both` 한 갈래로만 나온다.

    모르는 이름이 줄에 실려도 그냥 한 열로 더 들어온다 — FIELD_PATTERN 이 `이름=값` 을 다 뽑고,
    판정은 MUST_BE_ZERO 에 적힌 이름만 본다. 없는 칸은 "없음" 으로 적고 넘어간다.
    """
    sources = _logdir.live_logs(log_directory)
    rows: list[dict[str, str]] = []

    for path in sources:
        rows.extend(read_role_log(path, _logdir.role_of(path), round_name))

    rows.sort(key=lambda row: (row["timestamp"], row["role"]))

    return rows


def last_by_role(rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    """역할마다 마지막 줄. 계수기가 누적값이라 이게 회차 전체의 합이다."""
    latest: dict[str, dict[str, str]] = {}

    for row in rows:
        latest[row["role"]] = row

    return latest


def judge(rows: list[dict[str, str]]) -> tuple[list[str], bool]:
    """안전성 판정. (줄 목록, 통과 여부). 0 이어야 하는 칸이 하나라도 0 이 아니면 불합격이다."""
    lines: list[str] = []
    passed = True
    latest = last_by_role(rows)

    if not latest:
        return ["[큐 고수위] 줄이 하나도 없다 — 회차가 1분을 못 넘겼거나 로그 자리가 틀렸다"], False

    for role, row in sorted(latest.items()):
        lines.append(f"== {role} (마지막 표본 {row['timestamp']}) ==")

        for name in MUST_BE_ZERO:
            if name not in row:
                lines.append(f"  {name:28s} 없음 — 이 판에는 그 칸이 없다")
                continue

            count = to_count(row[name])

            if count is None:
                lines.append(f"  {name:28s} 못 읽음 ({row[name]})")
                passed = False
            elif count == 0:
                lines.append(f"  {name:28s} 0")
            else:
                passed = False
                mark = " ← 이중 발주" if name == DOUBLE_ORDER_FIELD else ""
                lines.append(f"  {name:28s} {count}  ← 0 이어야 한다{mark}")

        for name in COMPARE_WITH_BOTH:
            if name in row:
                lines.append(f"  {name:28s} {row[name]}  (both 판과 견준다)")

    # 이중 발주는 역할을 가리지 않고 본다. 전략 쪽이 만든 중복은 주문 쪽 계수기에 안 잡히므로
    #  한쪽만 보면 놓친다.
    for role, row in sorted(latest.items()):
        duplicate = to_count(row.get(DOUBLE_ORDER_FIELD, "")) or 0

        if duplicate > 0:
            passed = False
            lines.append(f"** {role} 이중 발주 {duplicate}건 — 분리판을 라이브에 올리면 안 된다 **")

    return lines, passed


def parse_arguments(argument_list: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="부하시험 큐 고수위 판독기")
    parser.add_argument("--round", default="", help="회차 이름. CSV 의 첫 열로 들어간다")
    parser.add_argument(
        "--log-dir",
        default="",
        help="실행 로그 폴더. 비우면 scripts/_logdir.py 가 찾는 자리를 쓴다",
    )
    parser.add_argument("--output", default="", help="CSV 경로. 비우면 화면에만 낸다")
    parser.add_argument("--judge", action="store_true", help="안전성 판정을 같이 낸다")

    return parser.parse_args(argument_list)


def main(argument_list: list[str] | None = None) -> int:
    arguments = parse_arguments(argument_list)
    log_directory = Path(arguments.log_dir) if arguments.log_dir else None
    rows = collect(log_directory, arguments.round)

    if not rows:
        print("[큐 고수위] 줄을 못 찾았다 — 로그 폴더를 확인한다", file=sys.stderr)
        return 1

    print(f"표본 {len(rows)}줄, 역할 {sorted({row['role'] for row in rows})}")

    if arguments.output:
        columns: list[str] = ["round", "role", "timestamp"]

        for row in rows:
            for name in row:
                if name not in columns:
                    columns.append(name)

        output_path = Path(arguments.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)

        with open(output_path, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)

        print(f"CSV {output_path} ({len(columns)}열)")

    if arguments.judge:
        lines, passed = judge(rows)

        for line in lines:
            print(line)

        print("판정: " + ("통과" if passed else "불합격"))

        return 0 if passed else 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
