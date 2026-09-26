"""적재기(main.py record) 한 프로세스가 어디서 시간을 쓰는지 구간별로 잰다.

    py -m tools.bench_recorder --rows 200000
    py -m tools.bench_recorder --rows 200000 --skip-db     # DB 없이 파이썬 쪽만

엔진이 내는 것과 같은 TRADE 묶음(JSON 배열 500행, ZmqBridge::format_trade 와 같은 키 순서)을 미리 만들어
두고, 적재기가 한 행마다 거치는 단계를 따로 돌려 초당 행 수로 적는다.
  ① json.loads           — 받은 프레임을 dict 목록으로
  ② 콜백·버퍼            — 계좌 확인 + RecordBuffer.append(DB 호출은 빈 함수)
  ③ 행 변환              — insert_trade_batch 가 dict 를 튜플로 바꾸는 반복(_ms_to_dt 포함)
  ④ DB 쓰기              — execute_values(지금 길) / COPY(후보), 묶음 크기별
DB 쓰기는 ticks 와 같은 모양의 bench_ticks 하이퍼테이블에 넣고 끝나면 지운다 — 운영 표는 건드리지 않는다.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

_REPO_ROOT = Path(__file__).resolve().parents[2]
_FRAME_ROWS = 500          # ZmqBridge kTradeBatchMax (D-139)
_ACCOUNT = "BENCH-ACCOUNT"  # 적재기가 거르는 계좌 값. 측정용 가짜


def load_environment_file(path: Path) -> None:
    """저장소 루트 .env 의 TSDB_* 를 환경변수로 올린다(이미 있으면 둔다)."""
    if not path.exists():
        return

    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()

        if not line or line.startswith("#") or "=" not in line:
            continue

        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip('"'))


def make_frames(total_rows: int) -> list[bytes]:
    """엔진이 보내는 TRADE 프레임 모양 그대로 — 2,700종목을 돌려 가며 채운다."""
    frames = []
    base_ms = int(time.time() * 1000)

    for start in range(0, total_rows, _FRAME_ROWS):
        rows = []

        for index in range(start, min(start + _FRAME_ROWS, total_rows)):
            rows.append(
                f'{{"account":"{_ACCOUNT}","direction":{1 if index % 2 else 5},"market":"KR",'
                f'"price":{50000.0 + index % 997},"ticker":"{100000 + index % 2700:06d}",'
                f'"ts":{base_ms + index},"volume":{1 + index % 50}}}'
            )

        frames.append(("[" + ",".join(rows) + "]").encode())

    return frames


def rate(rows: int, seconds: float) -> str:
    return f"{rows / seconds:>12,.0f} 행/초  ({seconds * 1000:,.0f} ms)"


def bench_python(frames: list[bytes], total_rows: int) -> list[list[dict]]:
    from main import RecordBuffer
    from db.client import _ms_to_dt, _require

    started = time.perf_counter()
    decoded = [json.loads(frame) for frame in frames]
    print(f"① json.loads        {rate(total_rows, time.perf_counter() - started)}")

    batches: list[list[dict]] = []

    def is_our_account(data: dict) -> bool:
        return str(data.get("account", "")).strip() == _ACCOUNT

    def keep(batch: list[dict]) -> int:
        batches.append(batch)
        return len(batch)

    buffer = RecordBuffer("TRADE ", keep, 500, 5.0)
    import main as main_module
    saved_level = main_module.logger.level
    main_module.logger.setLevel(100)      # 묶음마다 한 줄 로그는 따로 잰다 — 여기선 끈다

    started = time.perf_counter()

    for frame in decoded:
        for one in frame:
            is_our_account(one) and buffer.append(one)

    buffer.flush(force=True)
    print(f"② 콜백·버퍼          {rate(total_rows, time.perf_counter() - started)}")
    main_module.logger.setLevel(saved_level)

    started = time.perf_counter()

    for batch in batches:
        rows = []

        for record in batch:
            _require(record, "ts", "ticker", "price")
            rows.append((_ms_to_dt(record["ts"]), record["ticker"], record["price"],
                         record.get("volume"), record.get("direction"), record.get("market", "KR")))

    print(f"③ 행 변환            {rate(total_rows, time.perf_counter() - started)}")
    return batches


def to_tuples(batch: list[dict]) -> list[tuple]:
    from db.client import _ms_to_dt

    return [(_ms_to_dt(record["ts"]), record["ticker"], record["price"], record.get("volume"),
             record.get("direction"), record.get("market", "KR")) for record in batch]


def bench_db(batches: list[list[dict]], total_rows: int) -> None:
    import psycopg2
    from psycopg2.extras import execute_values

    from db.client import wsl_direct_host

    requested = os.getenv("TSDB_HOST", "localhost")
    host = wsl_direct_host(requested) or requested
    print(f"DB 주소 {host} (요청 {requested})")
    connection = psycopg2.connect(
        host=host, port=int(os.getenv("TSDB_PORT", "5432")),
        dbname=os.getenv("TSDB_DB", "quant"), user=os.getenv("TSDB_USER", "quant"),
        password=os.environ["TSDB_PASSWORD"])
    connection.autocommit = True
    cursor = connection.cursor()
    cursor.execute("DROP TABLE IF EXISTS bench_ticks")
    cursor.execute("CREATE TABLE bench_ticks (LIKE ticks INCLUDING DEFAULTS)")
    cursor.execute("SELECT create_hypertable('bench_ticks', 'ts')")
    cursor.execute("CREATE INDEX bench_ticks_ticker_ts ON bench_ticks (ticker, ts DESC)")
    columns = "ts,ticker,price,volume,direction,market"
    all_records = [record for batch in batches for record in batch]

    def regroup(size: int) -> list[list[dict]]:
        return [all_records[start:start + size] for start in range(0, len(all_records), size)]

    try:
        for size in (500, 5000):
            groups = regroup(size)
            cursor.execute("TRUNCATE bench_ticks")
            started = time.perf_counter()

            for group in groups:
                execute_values(cursor, f"INSERT INTO bench_ticks({columns}) VALUES %s",
                               to_tuples(group), page_size=500)

            print(f"④ execute_values {size:>5}행 묶음 {rate(total_rows, time.perf_counter() - started)}")

        for size in (500, 5000):
            groups = regroup(size)
            cursor.execute("TRUNCATE bench_ticks")
            started = time.perf_counter()

            for group in groups:
                # COPY 는 탭 구분 글자로 보낸다 — ts 는 epoch ms 를 서버가 바꾸게 두지 않고 ISO 글자로 만든다.
                text = io.StringIO()

                for row in to_tuples(group):
                    text.write(f"{row[0].isoformat()}\t{row[1]}\t{row[2]}\t{row[3]}\t{row[4]}\t{row[5]}\n")

                text.seek(0)
                cursor.copy_expert(f"COPY bench_ticks({columns}) FROM STDIN", text)

            print(f"④ COPY           {size:>5}행 묶음 {rate(total_rows, time.perf_counter() - started)}")

        # 적재기가 실제로 부르는 길(DbClient._insert_batch) — 위 후보 중 무엇이 들어가 있는지와 무관하게
        #  지금 코드의 값을 낸다. 고치기 전후 비교는 이 줄로 한다.
        from db.client import DbClient

        client = DbClient(retries=1)
        cursor.execute("TRUNCATE bench_ticks")
        started = time.perf_counter()

        for group in regroup(500):
            client._insert_batch("bench_ticks", columns, to_tuples(group))

        print(f"⑤ DbClient 지금 코드 500행 묶음 {rate(total_rows, time.perf_counter() - started)}")
        client.close()

        cursor.execute("SELECT count(*) FROM bench_ticks")
        print(f"   (마지막 판 bench_ticks 행 수 {cursor.fetchone()[0]:,})")
    finally:
        cursor.execute("DROP TABLE IF EXISTS bench_ticks")
        connection.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", type=int, default=200_000)
    parser.add_argument("--skip-db", action="store_true")
    arguments = parser.parse_args()

    load_environment_file(_REPO_ROOT / ".env")
    frames = make_frames(arguments.rows)
    print(f"TRADE {arguments.rows:,}행 = 프레임 {len(frames):,}개 × {_FRAME_ROWS}행, "
          f"프레임 평균 {sum(map(len, frames)) / len(frames):,.0f} B")
    batches = bench_python(frames, arguments.rows)

    if not arguments.skip_db:
        bench_db(batches, arguments.rows)


if __name__ == "__main__":
    main()
