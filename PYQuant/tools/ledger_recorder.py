#!/usr/bin/env python3
"""원장 저널 파일을 TimescaleDB로 따라 적는 적재기 (D-113).

엔진은 주문을 보내기 전에 원장을 파일(ledger_YYYYMMDD.bin)에 적는다. 이 적재기는 그 파일을
꼬리부터 읽어 DB에 넣는다 — 그래서 DB나 이 프로세스가 죽어 있어도 매매는 멈추지 않고,
올라오면 안 읽은 구간부터 이어 읽어 따라잡는다. ZMQ 구독(main.py record)과 달리 죽어 있는
동안의 이벤트가 사라지지 않는 것이 이 방식의 이유다.

멱등: 읽은 바이트 위치를 DB(ledger_offsets)에 두고, 레코드는 (거래일, 저널, seq)를 PK로 넣는다.
저널은 파일이 있는 폴더 이름(logs_paper·logs_live)이다 — 모의·실계좌가 같은 날 돌면 파일 이름과 seq가
둘 다 겹친다.
같은 파일을 처음부터 다시 읽혀도 원장이 부풀지 않는다(--from-start 가 그 용도).

대시보드·리포트가 보는 fills·orders·positions도 여기서 채운다(DbClient.mirror_ledger_range). 예전에는
ZMQ 적재기가 체결·주문 메시지를 받아 넣었는데, 그 길은 적재기가 죽어 있거나 ZMQ 대기칸이 차면 조용히
빠진다. 저널은 엔진이 주문 전에 적는 정본이라 여기서 옮기면 빠지는 건이 없다.

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

def journal_for_today(directory: Path, date: dt.date | None = None) -> Path:
    day = date or dt.datetime.now(KST).date()
    return directory / f"ledger_{day:%Y%m%d}.bin"


def journal_of(path: Path) -> str:
    """저널을 가르는 이름 — 파일이 있는 폴더 이름. 계좌마다 config의 ledger_journal_dir이 다르다."""
    return path.parent.name


def to_row(record: Record, trade_date: dt.date, journal: str) -> tuple:
    """레코드 → ledger_events 한 행. 컬럼 순서는 DbClient.insert_ledger_events와 한 벌.
    체결 결과 네 칸은 FILL이면서 그 칸이 있는 파일일 때만 채운다 — 나머지는 NULL(모름)이다."""
    detail = ((record.commission, record.tax, record.average_price, record.net_quantity)
              if record.fill_detail else (None, None, None, None))
    return (
        trade_date, journal, record.sequence, record.wall_time, record.kind,
        record.account or None, record.ticker or None, record.side, record.order_type,
        record.order_id or None, record.kis_order_number or None,
        record.quantity, record.reserved_quantity, record.sellable,
        record.price, record.cash, record.equity, record.pnl,
        record.strategy or None, record.reason or None,
        *detail,
    )


def catch_up(database, path: Path, from_start: bool = False) -> tuple[int, bool]:
    """파일에서 안 읽은 구간을 읽어 DB에 넣는다. (넣은 건수, 꼬리 잘림)."""
    if not path.exists():
        return (0, False)

    journal = journal_of(path)
    offset_key = f"{journal}/{path.name}"
    start_offset, last_sequence = (0, 0) if from_start else database.get_ledger_offset(offset_key)
    result = read_records(path, start_offset)

    if not result.records:
        return (0, result.truncated)

    trade_date = dt.datetime.strptime(str(result.header_date), "%Y%m%d").date()
    rows = [to_row(record, trade_date, journal) for record in result.records]
    inserted = database.insert_ledger_events(rows)

    if inserted == 0:
        # 넣지 못했으면 위치를 옮기지 않는다 — 다음 회차가 같은 구간을 다시 시도한다.
        logger.error(f"원장 적재 실패 — 위치 유지 ({path.name} {len(rows)}건)")
        return (0, result.truncated)

    through_sequence = result.records[-1].sequence

    # 옮기기가 실패해도 위치를 옮기지 않는다. 다시 읽은 레코드는 PK가, 다시 옮긴 행은 journal 고유 색인이 막는다.
    if not database.mirror_ledger_range(journal, trade_date, last_sequence, through_sequence):
        logger.error(f"fills·orders·positions 옮기기 실패 — 위치 유지 ({path.name} seq {last_sequence}~{through_sequence})")
        return (0, result.truncated)

    database.set_ledger_offset(offset_key, trade_date, result.offset, through_sequence)
    return (inserted, result.truncated)


def main() -> int:
    parser = argparse.ArgumentParser(description="원장 저널 → TimescaleDB 적재")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--dir", help="저널 폴더(오늘 날짜 파일을 스스로 고른다)")
    group.add_argument("--file", help="저널 파일 하나")
    parser.add_argument("--interval", type=float, default=2.0, help="따라가는 주기(초)")
    parser.add_argument("--once", action="store_true", help="한 번 따라잡고 끝낸다")
    parser.add_argument("--from-start", action="store_true", help="저장된 위치를 무시하고 처음부터")
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
    except KeyboardInterrupt:
        logger.info(f"중단 — 누계 {total}건")
    finally:
        database.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
