"""부하시험 상태 표본기 — 엔진의 요청·응답 소켓에서 버린 건수를 역할별로 따로 읽어 CSV로 남긴다.

    py -m tools.load_status_sampler --endpoint both=tcp://127.0.0.1:5556 --round M
    py -m tools.load_status_sampler --endpoint order=tcp://127.0.0.1:5556 \
                                    --endpoint strategy=tcp://127.0.0.1:5576 --round N

왜 요청·응답 소켓인가. 버린 건수는 HEALTH 메시지로도 나가지만 그쪽은 발행 큐를 탄다. 그 큐에서 HEALTH 는
체결·주문·신호보다 작은 한도를 써서, 큐가 차면 가장 먼저 버려진다. 정작 버리는 중일 때 버린 건수가
사라지는 것이다. 요청·응답 소켓은 그 큐를 안 거쳐서 포화 중에도 답한다(Quant/src/core/Engine.cpp 의
STATUS 처리, 커밋 9d1940a).

프로세스를 역할로 가른 뒤(D-114)에는 요청·응답 소켓도 둘이 된다. --endpoint 를 역할 이름과 함께 여러 번
주면 같은 시각에 양쪽을 읽어 한 줄씩 남기므로, 어느 쪽이 버렸는지 갈린다.

내는 CSV 는 docs/reports/stresstest/data/ 의 회차 파일과 같은 열에 role 하나만 더한 모양이다.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import psutil
import zmq

# 윈도우 콘솔 기본 코드페이지가 cp949 라 한글·줄표를 못 찍고 멈춘다.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# STATUS 응답에서 받아 적는 값들. 엔진이 아직 원인별 넷을 안 싣는 옛 바이너리면 뒤 넷이 빈 칸으로 남는다.
STATUS_FIELDS = (
    "data",
    "signal",
    "order",
    "drop",
    "drop_socket_full",
    "drop_socket_error",
    "drop_send_queue_full",
    "drop_trade_ring_full",
)

CSV_COLUMNS = (
    "round",
    "role",
    "recorder",
    "elapsed_seconds",
    "data_events",
    "signal_count",
    "order_count",
    "drop_total",
    "drop_socket_full",
    "drop_socket_error",
    "drop_send_queue_full",
    "drop_trade_ring_full",
    "available_gb",
)


@dataclass
class Endpoint:
    """한 역할의 요청·응답 소켓 하나. role 은 order·strategy·both 중 하나를 쓴다."""

    role: str
    address: str
    socket: zmq.Socket


def parse_endpoint(text: str) -> tuple[str, str]:
    """`역할=주소` 를 가른다. 역할을 빼고 주소만 주면 both 로 본다."""
    if "=" in text:
        role, address = text.split("=", 1)
        role = role.strip()
        address = address.strip()

        if not role or not address:
            raise argparse.ArgumentTypeError(f"역할과 주소를 둘 다 적어야 한다: {text}")

        return role, address

    return "both", text.strip()


def open_endpoints(context: zmq.Context, pairs: list[tuple[str, str]], timeout_ms: int) -> list[Endpoint]:
    """역할마다 REQ 소켓을 하나씩 연다. 답이 없으면 그 회만 건너뛰도록 시간 제한을 건다."""
    endpoints: list[Endpoint] = []

    for role, address in pairs:
        socket = context.socket(zmq.REQ)
        socket.setsockopt(zmq.RCVTIMEO, timeout_ms)
        socket.setsockopt(zmq.SNDTIMEO, timeout_ms)
        # 답을 못 받은 소켓을 그대로 두면 다음 요청이 막힌다. LINGER 0 으로 바로 닫고 새로 연다.
        socket.setsockopt(zmq.LINGER, 0)
        socket.connect(address)
        endpoints.append(Endpoint(role=role, address=address, socket=socket))

    return endpoints


def reopen(context: zmq.Context, endpoint: Endpoint, timeout_ms: int) -> None:
    """REQ 소켓은 한 번 어긋나면 보내기·받기 순서가 꼬인다. 통째로 새로 연다."""
    endpoint.socket.close()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.RCVTIMEO, timeout_ms)
    socket.setsockopt(zmq.SNDTIMEO, timeout_ms)
    socket.setsockopt(zmq.LINGER, 0)
    socket.connect(endpoint.address)
    endpoint.socket = socket


def ask_status(endpoint: Endpoint) -> dict | None:
    """STATUS 를 한 번 묻는다. 답이 없거나 JSON 이 아니면 None 을 돌려준다."""
    try:
        endpoint.socket.send_string("STATUS")
        answer = endpoint.socket.recv_string()
    except zmq.ZMQError:
        return None

    try:
        return json.loads(answer)
    except json.JSONDecodeError:
        return None


def available_gigabytes() -> float:
    """남은 메모리. 앞선 회차에서 이 값이 0.3GB 아래로 내려갔을 때 WSL 이 내려가 DB 가 같이 죽었다."""
    return round(psutil.virtual_memory().available / (1024 ** 3), 2)


def build_row(round_name: str, recorder: str, endpoint: Endpoint, status: dict,
              elapsed_seconds: float) -> dict:
    """STATUS 응답 한 건을 CSV 한 줄로 옮긴다. 엔진이 안 실은 값은 빈 칸으로 둔다."""
    return {
        "round": round_name,
        "role": endpoint.role,
        "recorder": recorder,
        "elapsed_seconds": round(elapsed_seconds, 1),
        "data_events": status.get("data", ""),
        "signal_count": status.get("signal", ""),
        "order_count": status.get("order", ""),
        "drop_total": status.get("drop", ""),
        "drop_socket_full": status.get("drop_socket_full", ""),
        "drop_socket_error": status.get("drop_socket_error", ""),
        "drop_send_queue_full": status.get("drop_send_queue_full", ""),
        "drop_trade_ring_full": status.get("drop_trade_ring_full", ""),
        "available_gb": available_gigabytes(),
    }


def format_line(row: dict) -> str:
    """화면에 한 줄로 보여 준다. 버린 것이 있으면 원인별로 같이 적는다."""
    head = (f"[{row['elapsed_seconds']:>6}초] {row['role']:<8}"
            f" 시세 {row['data_events']:>12}  신호 {row['signal_count']:>8}"
            f"  주문 {row['order_count']:>8}  버림 {row['drop_total']:>12}")

    if row["drop_total"] in ("", 0, "0"):
        return head + f"  남은메모리 {row['available_gb']}GB"

    return (head + f"  (소켓 {row['drop_socket_full']}·예외 {row['drop_socket_error']}"
            f"·발행큐 {row['drop_send_queue_full']}·체결링 {row['drop_trade_ring_full']})"
            f"  남은메모리 {row['available_gb']}GB")


def parse_arguments(argument_list: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--endpoint",
        action="append",
        required=True,
        metavar="역할=주소",
        help="읽을 요청·응답 소켓. 역할은 order·strategy·both. 여러 번 줄 수 있다",
    )
    parser.add_argument("--round", dest="round_name", required=True, help="회차 이름(CSV 의 round 열)")
    parser.add_argument(
        "--recorder",
        default="off",
        choices=("off", "on"),
        help="파이썬 적재기를 붙였는지. CSV 의 recorder 열에 그대로 들어간다",
    )
    parser.add_argument("--interval-seconds", type=float, default=10.0, help="표본 간격")
    parser.add_argument("--duration-seconds", type=float, default=80.0, help="총 재는 시간")
    parser.add_argument("--timeout-ms", type=int, default=2000, help="한 번 물을 때 기다리는 시간")
    parser.add_argument("--output", type=Path, help="CSV 파일 경로. 없으면 화면에만 낸다")
    return parser.parse_args(argument_list)


def main(argument_list: list[str] | None = None) -> int:
    arguments = parse_arguments(argument_list)

    try:
        pairs = [parse_endpoint(text) for text in arguments.endpoint]
    except argparse.ArgumentTypeError as error:
        print(f"인자가 잘못됐다: {error}", file=sys.stderr)
        return 2

    roles = [role for role, _ in pairs]

    if len(set(roles)) != len(roles):
        print(f"역할 이름이 겹친다: {roles}", file=sys.stderr)
        return 2

    context = zmq.Context.instance()
    endpoints = open_endpoints(context, pairs, arguments.timeout_ms)
    rows: list[dict] = []
    started_at = time.monotonic()
    silent_rounds = 0

    print(f"회차 {arguments.round_name}, 적재기 {arguments.recorder}, "
          f"{arguments.duration_seconds:g}초 동안 {arguments.interval_seconds:g}초마다 읽는다")

    for endpoint in endpoints:
        print(f"  {endpoint.role:<8} {endpoint.address}")

    try:
        while True:
            elapsed_seconds = time.monotonic() - started_at

            if elapsed_seconds > arguments.duration_seconds:
                break

            answered = 0

            for endpoint in endpoints:
                status = ask_status(endpoint)

                if status is None:
                    print(f"[{elapsed_seconds:>6.1f}초] {endpoint.role:<8} 답이 없다 — 소켓을 다시 연다")
                    reopen(context, endpoint, arguments.timeout_ms)
                    continue

                answered += 1
                row = build_row(arguments.round_name, arguments.recorder, endpoint,
                                status, elapsed_seconds)
                rows.append(row)
                print(format_line(row))

            # 한 역할도 답을 안 하면 엔진이 안 떠 있거나 주소가 틀린 것이다. 빈 CSV 를 만들지 않는다.
            silent_rounds = silent_rounds + 1 if answered == 0 else 0

            if silent_rounds >= 3:
                print("세 번 이어서 아무도 답하지 않았다. 엔진이 떠 있는지·주소가 맞는지 보라.",
                      file=sys.stderr)
                return 1

            time.sleep(arguments.interval_seconds)
    except KeyboardInterrupt:
        print("\n멈춘다. 여기까지 읽은 것은 남긴다.")
    finally:
        for endpoint in endpoints:
            endpoint.socket.close()

    if not rows:
        print("남길 표본이 없다.", file=sys.stderr)
        return 1

    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)

        with arguments.output.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(CSV_COLUMNS))
            writer.writeheader()
            writer.writerows(rows)

        print(f"\n{arguments.output} 에 {len(rows)}줄 남겼다")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
