#!/usr/bin/env python3
"""원장 저널(ledger_YYYYMMDD.bin)을 사람이·스크립트가 읽는 도구 (D-113).

원장을 파일에 먼저 적고 주문을 보내는 구조라, 정본은 이 파일이다. DB(TimescaleDB)는 이 파일을
따라 적는 복제본이라서, DB가 죽어 있어도 여기서 그날 원장을 그대로 읽을 수 있어야 한다.
이 파일이 그 "쿼리 없이 조회하는" 자리다 — 적재기(ledger_recorder)도 여기의 read_records를 쓴다.

레코드는 192바이트 고정, 레코드마다 CRC32. 꼬리가 잘렸거나 CRC가 틀린 레코드가 나오면 거기서
멈춘다 — 엔진의 리플레이와 같은 지점에서 끊어야 둘이 본 원장이 같다.

사용:
    py PYQuant/tools/ledger_dump.py Quant/build_win/logs/ledger_20260922.bin
    py PYQuant/tools/ledger_dump.py <파일> --ticker 005930 --kind FILL
    py PYQuant/tools/ledger_dump.py <파일> --positions          # 종목별 보유·평단·선점 재구성
    py PYQuant/tools/ledger_dump.py <파일> --open-intents       # 결말 못 본 주문(재기동 대조 대상)
    py PYQuant/tools/ledger_dump.py <파일> --csv out.csv        # 표로 빼서 엑셀·판다스로
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import struct
import sys
import zlib
from pathlib import Path
from typing import Iterator, NamedTuple

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# Quant/include/risk/LedgerJournal.h 와 한 벌이다. 레코드가 바뀌면 그쪽 kVersion을 올리고 여기도 고친다.
MAGIC = 0x47444C51
VERSION = 1
HEADER_FORMAT = "<IIII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
RECORD_FORMAT = "<QqQQHBBiiidddd16s12s24s48sIII"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
assert HEADER_SIZE == 16 and RECORD_SIZE == 192, (HEADER_SIZE, RECORD_SIZE)

KIND_NAMES = {1: "SEED", 2: "INTENT", 3: "ACCEPT", 4: "REJECT", 5: "FILL",
              6: "CANCEL", 7: "ADJUST", 8: "RESET_RESERVED", 9: "CASH", 10: "DAILY_PNL"}
SIDE_NAMES = {0: "BUY", 1: "SELL", 2: "NONE"}
TYPE_NAMES = {0: "MARKET", 1: "LIMIT"}


class Record(NamedTuple):
    """저널 레코드 하나. 필드 뜻은 LedgerJournal.h의 같은 이름 주석이 정본."""

    sequence: int
    wall_us: int
    order_id: int
    kis_order_number: int
    kind: str
    side: str
    order_type: str
    quantity: int
    reserved_quantity: int
    sellable: int
    price: float
    cash: float
    equity: float
    pnl: float
    account: str
    ticker: str
    strategy: str
    reason: str

    @property
    def wall_time(self) -> dt.datetime:
        """체결·주문 시각(KST). wall_us는 엔진의 system_clock 마이크로초다."""
        return dt.datetime.fromtimestamp(self.wall_us / 1_000_000, dt.timezone(dt.timedelta(hours=9)))


class ReadResult(NamedTuple):
    records: list[Record]
    offset: int          # 온전하게 읽은 마지막 레코드의 끝 — 적재기가 여기부터 이어 읽는다
    truncated: bool      # 꼬리가 잘렸거나 CRC가 틀려 도중에 멈췄다
    header_date: int     # 헤더에 적힌 YYYYMMDD


def _text(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def read_records(path: Path, start_offset: int = 0) -> ReadResult:
    """저널 파일을 읽어 레코드 목록을 준다.

    start_offset이 0보다 크면 거기서 이어 읽는다(적재기가 저장해 둔 바이트 위치). 헤더가 틀리면
    빈 결과를 준다 — 남의 파일을 원장으로 읽는 것이 CRC 깨진 원장보다 위험하다.
    """
    data = path.read_bytes()

    if len(data) < HEADER_SIZE:
        return ReadResult([], 0, len(data) > 0, 0)

    magic, version, record_size, header_date = struct.unpack_from(HEADER_FORMAT, data, 0)

    if magic != MAGIC or version != VERSION or record_size != RECORD_SIZE:
        raise ValueError(f"{path}: 원장 저널 헤더가 아니다 (magic={magic:#x} version={version} record_size={record_size})")

    offset = max(start_offset, HEADER_SIZE)
    records: list[Record] = []
    truncated = False

    while offset + RECORD_SIZE <= len(data):
        chunk = data[offset:offset + RECORD_SIZE]
        stored_crc = struct.unpack_from("<I", chunk, RECORD_SIZE - 4)[0]

        # CRC는 crc32 필드를 0으로 둔 레코드 전체에 대해 계산한다(C++ record_crc와 같은 정의).
        if zlib.crc32(chunk[:RECORD_SIZE - 4] + b"\0\0\0\0") != stored_crc:
            truncated = True
            break

        fields = struct.unpack(RECORD_FORMAT, chunk)
        records.append(Record(
            sequence=fields[0], wall_us=fields[1], order_id=fields[2], kis_order_number=fields[3],
            kind=KIND_NAMES.get(fields[4], str(fields[4])), side=SIDE_NAMES.get(fields[5], str(fields[5])),
            order_type=TYPE_NAMES.get(fields[6], str(fields[6])), quantity=fields[7],
            reserved_quantity=fields[8], sellable=fields[9], price=fields[10], cash=fields[11],
            equity=fields[12], pnl=fields[13], account=_text(fields[14]), ticker=_text(fields[15]),
            strategy=_text(fields[16]), reason=_text(fields[17])))
        offset += RECORD_SIZE

    # 남은 바이트가 레코드 하나가 안 되면 쓰다 만 꼬리다.
    if offset + RECORD_SIZE > len(data) and offset < len(data):
        truncated = True

    return ReadResult(records, offset, truncated, header_date)


def iterate_records(path: Path, start_offset: int = 0) -> Iterator[Record]:
    yield from read_records(path, start_offset).records


# ── 재구성 — 파일만으로 "지금 무엇을 들고 있었나"를 답한다 ────────────────────
def rebuild_positions(records: list[Record]) -> dict[tuple[str, str], dict]:
    """엔진 리플레이와 같은 순서로 (계좌, 종목)별 보유·평단·선점·매도가능을 되쌓는다.

    엔진 쪽 OrderGate::apply_record가 정본이고 여기는 읽기용 근사다 — SEED·ADJUST가 절대값을
    덮어쓰기 때문에, 대조 레코드가 있는 날은 두 결과가 같아야 한다(어긋나면 그게 볼 거리다).
    """
    positions: dict[tuple[str, str], dict] = {}

    for record in records:
        if not record.ticker:
            continue

        key = (record.account, record.ticker)
        state = positions.setdefault(key, {"quantity": 0, "average": 0.0, "reserved": 0, "sellable": 0,
                                           "realized": 0.0, "fills": 0})

        if record.kind in ("SEED", "ADJUST"):
            state["quantity"] = record.quantity
            state["average"] = record.price
            state["reserved"] = record.reserved_quantity
            state["sellable"] = max(record.sellable, 0)
        elif record.kind == "INTENT":
            state["reserved"] += record.quantity if record.side == "BUY" else -record.quantity
        elif record.kind in ("REJECT", "CANCEL"):
            state["reserved"] -= record.quantity if record.side == "BUY" else -record.quantity
        elif record.kind == "FILL":
            state["reserved"] -= record.quantity if record.side == "BUY" else -record.quantity
            state["fills"] += 1
            state["realized"] += record.pnl

            if record.side == "BUY":
                total = state["average"] * state["quantity"] + record.price * record.quantity
                state["quantity"] += record.quantity
                state["average"] = total / state["quantity"] if state["quantity"] else 0.0
                state["sellable"] += record.quantity
            else:
                state["quantity"] = max(state["quantity"] - record.quantity, 0)
                state["sellable"] = max(state["sellable"] - record.quantity, 0)

                if state["quantity"] == 0:
                    state["average"] = 0.0
        elif record.kind == "RESET_RESERVED":
            state["reserved"] = 0

    return positions


def rebuild_open_intents(records: list[Record]) -> list[dict]:
    """INTENT가 열고 FILL·CANCEL·REJECT가 닫는다 — 남은 것이 결말을 못 본 주문이다."""
    open_orders: dict[int, dict] = {}

    for record in records:
        if record.order_id == 0:
            continue

        if record.kind == "INTENT":
            intent = open_orders.setdefault(record.order_id, {
                "order_id": record.order_id, "kis_order_number": 0, "account": record.account,
                "ticker": record.ticker, "strategy": record.strategy, "side": record.side,
                "price": record.price, "remaining": 0, "accepted": False,
                "wall_time": record.wall_time})
            intent["remaining"] += record.quantity
            continue

        intent = open_orders.get(record.order_id)

        if intent is None:
            continue

        if record.kind == "ACCEPT":
            intent["accepted"] = True
            intent["kis_order_number"] = record.kis_order_number
        elif record.kind in ("FILL", "CANCEL", "REJECT"):
            intent["remaining"] -= record.quantity

            if intent["remaining"] <= 0:
                del open_orders[record.order_id]

    return sorted(open_orders.values(), key=lambda intent: intent["order_id"])


# ── 출력 ─────────────────────────────────────────────────────────────────────
def print_records(records: list[Record]) -> None:
    print(f"{'sequence':>6} {'시각':<12} {'종류':<14} {'종목':<7} {'방향':<4} {'수량':>6} {'가격':>10} "
          f"{'ODNO':>10} {'전략':<16} 비고")

    for record in records:
        note = record.reason

        if record.kind == "CASH":
            note = f"현금 {record.cash:,.0f} · 평가 {record.equity:,.0f}"
        elif record.kind == "DAILY_PNL":
            note = f"당일손익 {record.pnl:,.0f}"
        elif record.kind == "FILL" and record.pnl:
            note = f"실현 {record.pnl:,.0f}" + (f" · {record.reason}" if record.reason else "")
        elif record.kind in ("SEED", "ADJUST"):
            note = f"선점 {record.reserved_quantity} · 매도가능 {record.sellable}" + (f" · {record.reason}" if record.reason else "")

        print(f"{record.sequence:>6} {record.wall_time:%H:%M:%S.%f}"[:19].ljust(19)
              + f" {record.kind:<14} {record.ticker:<7} {record.side:<4} {record.quantity:>6} "
                f"{record.price:>10,.0f} {record.kis_order_number:>10} {record.strategy:<16} {note}")


def print_positions(records: list[Record]) -> None:
    positions = rebuild_positions(records)
    print(f"{'계좌':<10} {'종목':<7} {'보유':>6} {'평단':>10} {'선점':>6} {'매도가능':>8} {'체결':>4} {'실현손익':>12}")

    for (account, ticker), state in sorted(positions.items()):
        if not state["quantity"] and not state["reserved"] and not state["fills"]:
            continue

        print(f"{account:<10} {ticker:<7} {state['quantity']:>6} {state['average']:>10,.0f} "
              f"{state['reserved']:>6} {state['sellable']:>8} {state['fills']:>4} {state['realized']:>12,.0f}")


def print_open_intents(records: list[Record]) -> None:
    intents = rebuild_open_intents(records)

    if not intents:
        print("결말 못 본 주문 없음")
        return

    print(f"{'주문':>8} {'ODNO':>10} {'종목':<7} {'방향':<4} {'잔량':>6} {'가격':>10} {'접수':<5} {'시각':<8} 전략")

    for intent in intents:
        print(f"{intent['order_id']:>8} {intent['kis_order_number']:>10} {intent['ticker']:<7} "
              f"{intent['side']:<4} {intent['remaining']:>6} {intent['price']:>10,.0f} "
              f"{'예' if intent['accepted'] else '아니오':<5} {intent['wall_time']:%H:%M:%S} {intent['strategy']}")


def write_csv(records: list[Record], destination: Path) -> None:
    with destination.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["sequence", "time", "kind", "account", "ticker", "side", "type", "quantity",
                         "price", "order_id", "odno", "strategy", "reserved_quantity", "sellable",
                         "cash", "equity", "pnl", "reason"])

        for record in records:
            writer.writerow([record.sequence, f"{record.wall_time:%Y-%m-%d %H:%M:%S.%f}", record.kind,
                             record.account, record.ticker, record.side, record.order_type,
                             record.quantity, record.price, record.order_id, record.kis_order_number,
                             record.strategy, record.reserved_quantity, record.sellable,
                             record.cash, record.equity, record.pnl, record.reason])

    print(f"CSV {len(records)}행 → {destination}")


def main() -> int:
    parser = argparse.ArgumentParser(description="원장 저널(ledger_YYYYMMDD.bin) 조회")
    parser.add_argument("journal", help="저널 파일 경로")
    parser.add_argument("--ticker", help="종목코드로 거른다")
    parser.add_argument("--kind", help="레코드 종류로 거른다(쉼표로 여럿): " + ",".join(KIND_NAMES.values()))
    parser.add_argument("--order", type=int, help="내부 주문번호 하나만 본다")
    parser.add_argument("--since-sequence", type=int, default=0, help="이 sequence 뒤만 본다")
    parser.add_argument("--positions", action="store_true", help="종목별 보유·평단·선점 재구성")
    parser.add_argument("--open-intents", action="store_true", help="결말 못 본 주문만")
    parser.add_argument("--csv", help="CSV로 내보낼 경로")
    arguments = parser.parse_args()

    path = Path(arguments.journal)

    if not path.exists():
        print(f"저널 파일 없음: {path}", file=sys.stderr)
        return 2

    result = read_records(path)
    print(f"{path} — {len(result.records)}건 (헤더 날짜 {result.header_date}, "
          f"마지막 sequence {result.records[-1].sequence if result.records else 0}"
          + (", 꼬리 잘림" if result.truncated else "") + ")")

    # 재구성은 거르기 전 전체 레코드로 한다 — 중간을 빼면 보유·선점이 안 맞는다.
    if arguments.positions:
        print_positions(result.records)
        return 0

    if arguments.open_intents:
        print_open_intents(result.records)
        return 0

    records = result.records

    if arguments.ticker:
        records = [record for record in records if record.ticker == arguments.ticker]

    if arguments.kind:
        wanted = {name.strip().upper() for name in arguments.kind.split(",")}
        records = [record for record in records if record.kind in wanted]

    if arguments.order:
        records = [record for record in records if record.order_id == arguments.order]

    if arguments.since_sequence:
        records = [record for record in records if record.sequence > arguments.since_sequence]

    if arguments.csv:
        write_csv(records, Path(arguments.csv))
        return 0

    print_records(records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
