#!/usr/bin/env python3
"""원장 저널 파일을 TimescaleDB로 따라 적는 적재기 (D-113).

엔진은 주문을 보내기 전에 원장을 파일(ledger_YYYYMMDD.bin)에 적는다. 이 적재기는 그 파일을
꼬리부터 읽어 DB에 넣는다 — 그래서 DB나 이 프로세스가 죽어 있어도 매매는 멈추지 않고,
올라오면 안 읽은 구간부터 이어 읽어 따라잡는다. ZMQ 구독(main.py record)과 달리 죽어 있는
동안의 이벤트가 사라지지 않는 것이 이 방식의 이유다.

멱등: 읽은 바이트 위치를 DB(ledger_offsets)에 두고, 레코드는 (거래일, seq)를 PK로 넣는다.
같은 파일을 처음부터 다시 읽혀도 원장이 부풀지 않는다(--from-start 가 그 용도).

사용:
    py PYQuant/tools/ledger_recorder.py --dir Quant/build_win/logs          # 오늘 파일을 따라간다
    py PYQuant/tools/ledger_recorder.py --dir <폴더> --once                  # 한 번 따라잡고 끝(마감 배치)
    py PYQuant/tools/ledger_recorder.py --file <경로> --from-start           # 지난 날 파일 다시 적재
"""
from __future__ import annotations

import argparse
import datetime as dt
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS.parent))   # PYQuant/ — db.client, core.logger
sys.path.insert(0, str(TOOLS))          # ledger_dump

from ledger_dump import Record, read_records  # noqa: E402

from core.logger import setup_logger  # noqa: E402

logger = setup_logger("quant.ledger")

KST = dt.timezone(dt.timedelta(hours=9))

# FILL 한 건을 기존 fills 원장에도 넣을지 — 대시보드·리포트가 그 테이블을 본다.
#  체결통보를 받은 ZMQ 적재기와 같은 행이 들어가므로 odno가 겹치면 그쪽이 이미 넣은 것이다.
FILL_KINDS = ("FILL",)


def journal_for_today(directory: Path, date: dt.date | None = None) -> Path:
    day = date or dt.datetime.now(KST).date()
    return directory / f"ledger_{day:%Y%m%d}.bin"


def to_row(record: Record, trade_date: dt.date) -> tuple:
    """레코드 → ledger_events 한 행. 컬럼 순서는 DbClient.insert_ledger_events와 한 벌."""
    return (
        trade_date, record.sequence, record.wall_time, record.kind,
        record.account or None, record.ticker or None, record.side, record.order_type,
        record.order_id or None, record.kis_order_number or None,
        record.quantity, record.reserved_quantity, record.sellable,
        record.price, record.cash, record.equity, record.pnl,
        record.strategy or None, record.reason or None,
    )


def catch_up(database, path: Path, from_start: bool = False) -> tuple[int, bool]:
    """파일에서 안 읽은 구간을 읽어 DB에 넣는다. (넣은 건수, 꼬리 잘림)."""
    if not path.exists():
        return (0, False)

    start_offset, _ = (0, 0) if from_start else database.get_ledger_offset(path.name)
    result = read_records(path, start_offset)

    if not result.records:
        return (0, result.truncated)

    trade_date = dt.datetime.strptime(str(result.header_date), "%Y%m%d").date()
    rows = [to_row(record, trade_date) for record in result.records]
    inserted = database.insert_ledger_events(rows)

    if inserted == 0:
        # 넣지 못했으면 위치를 옮기지 않는다 — 다음 회차가 같은 구간을 다시 시도한다.
        logger.error(f"원장 적재 실패 — 위치 유지 ({path.name} {len(rows)}건)")
        return (0, result.truncated)

    database.set_ledger_offset(path.name, trade_date, result.offset, result.records[-1].sequence)
    return (inserted, result.truncated)


def mirror_fills(database, path: Path, trade_date: dt.date) -> int:
    """FILL 레코드를 기존 fills 원장에도 넣는다(대시보드 호환). 이미 있는 체결은 건너뛴다."""
    result = read_records(path)
    count = 0

    for record in result.records:
        if record.kind not in FILL_KINDS or not record.kis_order_number:
            continue

        database.insert_fill({
            "ts": record.wall_time,
            "odno": str(record.kis_order_number),
            "ticker": record.ticker,
            "side": record.side,
            "filled_qty": record.quantity,
            "filled_price": record.price,
            "strategy": record.strategy or None,
            "account": record.account or None,
        })
        count += 1

    logger.info(f"fills 거울 적재 {count}건 ({trade_date})")
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description="원장 저널 → TimescaleDB 적재")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--dir", help="저널 폴더(오늘 날짜 파일을 스스로 고른다)")
    group.add_argument("--file", help="저널 파일 하나")
    parser.add_argument("--interval", type=float, default=2.0, help="따라가는 주기(초)")
    parser.add_argument("--once", action="store_true", help="한 번 따라잡고 끝낸다")
    parser.add_argument("--from-start", action="store_true", help="저장된 위치를 무시하고 처음부터")
    parser.add_argument("--mirror-fills", action="store_true", help="FILL을 fills 테이블에도 넣는다")
    arguments = parser.parse_args()

    from db.client import DbClient

    database = DbClient()
    database.ensure_ledger_tables()

    directory = Path(arguments.dir) if arguments.dir else None
    path = Path(arguments.file) if arguments.file else journal_for_today(directory)
    logger.info(f"원장 적재 시작: {path} (주기 {arguments.interval}초{', 한 번만' if arguments.once else ''})")

    from_start = arguments.from_start
    warned_truncated = False
    total = 0

    try:
        while True:
            # 날이 바뀌면 파일도 바뀐다 — 폴더를 받았으면 오늘 파일을 다시 고른다.
            if directory:
                today_path = journal_for_today(directory)

                if today_path != path:
                    logger.info(f"날짜 바뀜 — {today_path.name}으로 옮긴다")
                    path, warned_truncated = today_path, False

            inserted, truncated = catch_up(database, path, from_start)
            from_start = False  # 처음부터는 한 번만
            total += inserted

            if inserted:
                logger.info(f"원장 적재 {inserted}건 (누계 {total})")

            if truncated and not warned_truncated:
                # 쓰다 만 레코드 — 엔진이 그 지점에서 죽었다는 뜻이고, 다음 기동이 꼬리를 자른다.
                logger.warning(f"{path.name}: 꼬리가 잘렸다 — 엔진이 쓰는 도중 죽은 자리에서 멈춘다")
                warned_truncated = True

            if arguments.once:
                break

            time.sleep(arguments.interval)

        if arguments.mirror_fills:
            trade_date = dt.datetime.now(KST).date()
            mirror_fills(database, path, trade_date)
    except KeyboardInterrupt:
        logger.info(f"중단 — 누계 {total}건")
    finally:
        database.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
