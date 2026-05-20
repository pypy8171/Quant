"""
TimescaleDB 클라이언트 — ZMQ 이벤트 및 KIS 일봉 데이터 저장
환경변수: TSDB_HOST, TSDB_PORT, TSDB_DB, TSDB_USER, TSDB_PASSWORD
"""
import os
import time
from datetime import datetime, timezone

try:
    import psycopg2
    _PG_AVAILABLE = True
except ImportError:
    _PG_AVAILABLE = False


def _ms_to_dt(ts_ms: int) -> datetime:
    return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc)


class DbClient:
    def __init__(
        self,
        host: str = "",
        port: int = 0,
        db: str = "",
        user: str = "",
        password: str = "",
        retries: int = 12,
        retry_interval: float = 5.0,
    ):
        if not _PG_AVAILABLE:
            raise RuntimeError("psycopg2가 설치되지 않았습니다: pip install psycopg2-binary")

        host     = host     or os.getenv("TSDB_HOST",     "localhost")
        port     = port     or int(os.getenv("TSDB_PORT", "5432"))
        db       = db       or os.getenv("TSDB_DB",       "quant")
        user     = user     or os.getenv("TSDB_USER",     "quant")
        password = password or os.getenv("TSDB_PASSWORD", "changeme")

        # DB가 준비될 때까지 재시도 (Docker 기동 순서 대응)
        for attempt in range(1, retries + 1):
            try:
                self._conn = psycopg2.connect(
                    host=host, port=port, dbname=db,
                    user=user, password=password,
                    connect_timeout=5,
                )
                self._conn.autocommit = True
                print(f"[DB] 연결 완료: {user}@{host}:{port}/{db}")
                return
            except psycopg2.OperationalError as e:
                if attempt == retries:
                    raise
                print(f"[DB] 연결 대기 중... ({attempt}/{retries}): {e}")
                time.sleep(retry_interval)

    # ── 이벤트 insert ──────────────────────────────────────────────────────────

    def insert_trade(self, data: dict):
        with self._conn.cursor() as cur:
            cur.execute(
                "INSERT INTO ticks(ts,ticker,price,volume,direction,market)"
                " VALUES (%s,%s,%s,%s,%s,%s)",
                (
                    _ms_to_dt(data["ts"]),
                    data.get("ticker"),
                    data.get("price"),
                    data.get("volume"),
                    data.get("direction"),
                    data.get("market", "KR"),
                ),
            )

    def insert_signal(self, data: dict):
        with self._conn.cursor() as cur:
            cur.execute(
                "INSERT INTO signals(ts,strategy,ticker,side,qty,price,market)"
                " VALUES (%s,%s,%s,%s,%s,%s,%s)",
                (
                    _ms_to_dt(data["ts"]),
                    data.get("strategy"),
                    data.get("ticker"),
                    data.get("side"),
                    data.get("qty"),
                    data.get("price"),
                    data.get("market", "KR"),
                ),
            )

    def insert_order(self, data: dict):
        with self._conn.cursor() as cur:
            cur.execute(
                "INSERT INTO orders(ts,ticker,side,qty,price,ok,market)"
                " VALUES (%s,%s,%s,%s,%s,%s,%s)",
                (
                    _ms_to_dt(data["ts"]),
                    data.get("ticker"),
                    data.get("side"),
                    data.get("qty"),
                    data.get("price"),
                    data.get("ok"),
                    data.get("market", "KR"),
                ),
            )

    def insert_health(self, data: dict):
        with self._conn.cursor() as cur:
            cur.execute(
                "INSERT INTO health(ts,data_cnt,signal_cnt,order_cnt)"
                " VALUES (%s,%s,%s,%s)",
                (
                    _ms_to_dt(data["ts"]),
                    data.get("data", 0),
                    data.get("signal", 0),
                    data.get("order", 0),
                ),
            )

    def insert_bar(self, ticker: str, ts: datetime, o: float, h: float,
                   lo: float, c: float, vol: int, market: str = "KR"):
        with self._conn.cursor() as cur:
            cur.execute(
                "INSERT INTO bars_1d(ts,ticker,open,high,low,close,volume,market)"
                " VALUES (%s,%s,%s,%s,%s,%s,%s,%s)"
                " ON CONFLICT (ticker,ts) DO NOTHING",
                (ts, ticker, o, h, lo, c, vol, market),
            )

    def close(self):
        self._conn.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
