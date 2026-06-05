"""
TimescaleDB 클라이언트 — ZMQ 이벤트 및 KIS 일봉 데이터 저장
환경변수: TSDB_HOST, TSDB_PORT, TSDB_DB, TSDB_USER, TSDB_PASSWORD
"""
import os
import time
from datetime import datetime, timezone

from core.logger import setup_logger

logger = setup_logger("quant.db")

try:
    import psycopg2
    _PG_AVAILABLE = True
except ImportError:
    _PG_AVAILABLE = False


def _ms_to_dt(ts_ms: int) -> datetime:
    return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc)


def _require(data: dict, *keys: str) -> None:
    missing = [k for k in keys if data.get(k) is None]
    if missing:
        raise ValueError(f"필수 필드 누락: {missing}")


def _exclusive_end(end):
    """기간 상한을 '그날 포함'으로 — 'YYYY-MM-DD'/date/datetime → 다음날(배타상한, `ts < %s` 용).
    'ts <= 날짜'는 그날 00:00로 해석돼 EOD(장마감 후) 적재분을 누락시킨다 (C-1)."""
    from datetime import date as _date, datetime as _dt, timedelta
    if isinstance(end, str):
        d = _dt.strptime(end[:10], "%Y-%m-%d").date()
    elif isinstance(end, _dt):
        d = end.date()
    elif isinstance(end, _date):
        d = end
    else:
        return end  # 알 수 없는 타입 → 그대로 (호출측 책임)
    return d + timedelta(days=1)


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
                logger.info(f"연결 완료: {user}@{host}:{port}/{db}")
                return
            except psycopg2.OperationalError as e:
                if attempt == retries:
                    raise
                logger.info(f"연결 대기 중... ({attempt}/{retries}): {e}")
                time.sleep(retry_interval)

    # ── 이벤트 insert ──────────────────────────────────────────────────────────

    def insert_trade(self, data: dict):
        try:
            _require(data, "ts", "ticker", "price")
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO ticks(ts,ticker,price,volume,direction,market)"
                    " VALUES (%s,%s,%s,%s,%s,%s)",
                    (
                        _ms_to_dt(data["ts"]),
                        data["ticker"],
                        data["price"],
                        data.get("volume"),
                        data.get("direction"),
                        data.get("market", "KR"),
                    ),
                )
        except Exception as e:
            logger.error(f"insert_trade 실패 (data={data}): {e}")

    def insert_signal(self, data: dict):
        try:
            _require(data, "ts", "strategy", "ticker", "side", "qty")
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO signals(ts,strategy,ticker,side,qty,price,market,regime)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s,%s)",
                    (
                        _ms_to_dt(data["ts"]),
                        data["strategy"],
                        data["ticker"],
                        data["side"],
                        data["qty"],
                        data.get("price"),
                        data.get("market", "KR"),
                        data.get("regime"),   # 그때의 국면 stamp (없으면 NULL)
                    ),
                )
        except Exception as e:
            logger.error(f"insert_signal 실패 (data={data}): {e}")

    def insert_order(self, data: dict):
        try:
            _require(data, "ts", "ticker", "side", "qty", "ok")
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO orders(ts,ticker,side,qty,price,ok,market)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s)",
                    (
                        _ms_to_dt(data["ts"]),
                        data["ticker"],
                        data["side"],
                        data["qty"],
                        data.get("price"),
                        data["ok"],
                        data.get("market", "KR"),
                    ),
                )
        except Exception as e:
            logger.error(f"insert_order 실패 (data={data}): {e}")

    def insert_health(self, data: dict):
        try:
            _require(data, "ts")
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
        except Exception as e:
            logger.error(f"insert_health 실패 (data={data}): {e}")

    def insert_trade_batch(self, records: list[dict]):
        """고빈도 tick 배치 insert — executemany로 개별 autocommit 부하 감소."""
        valid = []
        for data in records:
            try:
                _require(data, "ts", "ticker", "price")
                valid.append((
                    _ms_to_dt(data["ts"]),
                    data["ticker"],
                    data["price"],
                    data.get("volume"),
                    data.get("direction"),
                    data.get("market", "KR"),
                ))
            except Exception as e:
                logger.error(f"insert_trade_batch 필드 오류 (data={data}): {e}")
        if not valid:
            return
        try:
            with self._conn.cursor() as cur:
                cur.executemany(
                    "INSERT INTO ticks(ts,ticker,price,volume,direction,market)"
                    " VALUES (%s,%s,%s,%s,%s,%s)",
                    valid,
                )
        except Exception as e:
            logger.error(f"insert_trade_batch 실패 ({len(valid)}건): {e}")

    def insert_fill(self, data: dict):
        """체결통보(H0STCNI0) 1건을 fills 원장에 기록."""
        try:
            _require(data, "ts", "odno", "ticker", "side", "filled_qty", "filled_price")
            ts = data["ts"] if isinstance(data["ts"], datetime) else _ms_to_dt(data["ts"])
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO fills"
                    "(ts,odno,ticker,side,filled_qty,filled_price,commission,tax,market,regime)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                    (
                        ts,
                        data["odno"],
                        data["ticker"],
                        data["side"],
                        data["filled_qty"],
                        data["filled_price"],
                        data.get("commission"),
                        data.get("tax"),
                        data.get("market", "KR"),
                        data.get("regime"),   # 그때의 국면 stamp (없으면 NULL)
                    ),
                )
        except Exception as e:
            logger.error(f"insert_fill 실패 (data={data}): {e}")

    def upsert_position(self, ticker: str, quantity: int,
                        avg_price: float, realized_pnl: float):
        """포지션 원장 갱신 — 체결 후 또는 EOD 배치에서 호출."""
        try:
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO positions(ticker,quantity,avg_price,realized_pnl,updated_at)"
                    " VALUES (%s,%s,%s,%s,NOW())"
                    " ON CONFLICT (ticker) DO UPDATE SET"
                    "   quantity=EXCLUDED.quantity,"
                    "   avg_price=EXCLUDED.avg_price,"
                    "   realized_pnl=EXCLUDED.realized_pnl,"
                    "   updated_at=NOW()",
                    (ticker, quantity, avg_price, realized_pnl),
                )
        except Exception as e:
            logger.error(f"upsert_position 실패 ({ticker}): {e}")

    def insert_bar(self, ticker: str, ts: datetime, o: float, h: float,
                   lo: float, c: float, vol: int, market: str = "KR"):
        with self._conn.cursor() as cur:
            cur.execute(
                "INSERT INTO bars_1d(ts,ticker,open,high,low,close,volume,market)"
                " VALUES (%s,%s,%s,%s,%s,%s,%s,%s)"
                " ON CONFLICT (ticker,ts) DO NOTHING",
                (ts, ticker, o, h, lo, c, vol, market),
            )

    # ── 시장 국면 (RegimeController) ─────────────────────────────────────────

    def ensure_regime_tables(self):
        """기존 DB에도 regime 테이블 + signals/fills.regime 컬럼이 있도록 보장.
        schema.sql은 fresh init에만 적용되므로 기존 DB는 이 마이그레이션 필요."""
        ddl = [
            "CREATE TABLE IF NOT EXISTS regime ("
            " date DATE PRIMARY KEY, regime TEXT NOT NULL, score INT NOT NULL,"
            " above_ma200 BOOLEAN, aligned_bull BOOLEAN, aligned_bear BOOLEAN,"
            " index_close DOUBLE PRECISION, ts TIMESTAMPTZ NOT NULL)",
            "ALTER TABLE signals ADD COLUMN IF NOT EXISTS regime TEXT",
            "ALTER TABLE fills   ADD COLUMN IF NOT EXISTS regime TEXT",
        ]
        try:
            with self._conn.cursor() as cur:
                for stmt in ddl:
                    cur.execute(stmt)
        except Exception as e:
            logger.error(f"ensure_regime_tables 실패: {e}")

    def insert_regime(self, data: dict):
        """국면 스냅샷 1건 적재. date를 PK로 UPSERT (장 시작 1회 → 하루 1행)."""
        try:
            _require(data, "date", "regime", "score")
            ts = data.get("ts")
            ts = ts if isinstance(ts, datetime) else (_ms_to_dt(ts) if ts else datetime.now(timezone.utc))
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO regime"
                    "(date,regime,score,above_ma200,aligned_bull,aligned_bear,index_close,ts)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s,%s)"
                    " ON CONFLICT (date) DO UPDATE SET"
                    "   regime=EXCLUDED.regime, score=EXCLUDED.score,"
                    "   above_ma200=EXCLUDED.above_ma200, aligned_bull=EXCLUDED.aligned_bull,"
                    "   aligned_bear=EXCLUDED.aligned_bear, index_close=EXCLUDED.index_close,"
                    "   ts=EXCLUDED.ts",
                    (
                        data["date"], data["regime"], data["score"],
                        data.get("above_ma200"), data.get("aligned_bull"),
                        data.get("aligned_bear"), data.get("index_close"), ts,
                    ),
                )
        except Exception as e:
            logger.error(f"insert_regime 실패 (data={data}): {e}")

    def get_regime(self, start=None, end=None) -> list[dict]:
        sql = ("SELECT date,regime,score,above_ma200,aligned_bull,aligned_bear,"
               "index_close,ts FROM regime WHERE TRUE")
        p: list = []
        if start: sql += " AND date >= %s"; p.append(start)
        if end:   sql += " AND date <= %s"; p.append(end)
        sql += " ORDER BY date ASC"
        return self._query(sql, tuple(p))

    # ── 계좌 리포트 (스냅샷 / 입출금 / 거래내역) ─────────────────────────────

    def ensure_report_tables(self):
        """기존 DB(초기화 이후 생성)에도 리포트 테이블이 있도록 보장 — schema.sql과 동일 DDL."""
        ddl = [
            "CREATE TABLE IF NOT EXISTS account_snapshots ("
            " ts TIMESTAMPTZ NOT NULL, account TEXT NOT NULL,"
            " cash NUMERIC(18,4), total_eval NUMERIC(18,4),"
            " total_pnl NUMERIC(18,4), total_pnl_rate NUMERIC(10,4))",
            "CREATE INDEX IF NOT EXISTS account_snapshots_acct_ts"
            " ON account_snapshots (account, ts DESC)",
            "CREATE TABLE IF NOT EXISTS cash_flows ("
            " ts TIMESTAMPTZ NOT NULL, account TEXT NOT NULL,"
            " flow_type TEXT NOT NULL, amount NUMERIC(18,4) NOT NULL, memo TEXT)",
            "CREATE INDEX IF NOT EXISTS cash_flows_acct_ts"
            " ON cash_flows (account, ts DESC)",
        ]
        try:
            with self._conn.cursor() as cur:
                for stmt in ddl:
                    cur.execute(stmt)
                # account_snapshots를 하이퍼테이블로 (가능할 때만 — TimescaleDB 없으면 무시)
                try:
                    cur.execute("SELECT create_hypertable('account_snapshots','ts',"
                                "if_not_exists => TRUE)")
                except Exception:
                    pass
        except Exception as e:
            logger.error(f"ensure_report_tables 실패: {e}")

    def insert_account_snapshot(self, account: str, cash: float, total_eval: float,
                                total_pnl: float, total_pnl_rate: float, ts=None):
        try:
            ts = ts or datetime.now(timezone.utc)
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO account_snapshots"
                    "(ts,account,cash,total_eval,total_pnl,total_pnl_rate)"
                    " VALUES (%s,%s,%s,%s,%s,%s)",
                    (ts, account, cash, total_eval, total_pnl, total_pnl_rate),
                )
        except Exception as e:
            logger.error(f"insert_account_snapshot 실패: {e}")

    def insert_cash_flow(self, account: str, flow_type: str, amount: float,
                         memo: str = "", ts=None):
        try:
            if flow_type not in ("DEPOSIT", "WITHDRAW"):
                raise ValueError(f"flow_type은 DEPOSIT/WITHDRAW: {flow_type}")
            ts = ts or datetime.now(timezone.utc)
            with self._conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO cash_flows(ts,account,flow_type,amount,memo)"
                    " VALUES (%s,%s,%s,%s,%s)",
                    (ts, account, flow_type, abs(float(amount)), memo),
                )
        except Exception as e:
            logger.error(f"insert_cash_flow 실패: {e}")

    def _query(self, sql: str, params: tuple) -> list[dict]:
        try:
            with self._conn.cursor() as cur:
                cur.execute(sql, params)
                cols = [c[0] for c in cur.description]
                return [dict(zip(cols, row)) for row in cur.fetchall()]
        except Exception as e:
            logger.error(f"query 실패 ({sql[:40]}...): {e}")
            return []

    def get_snapshots(self, account: str, start=None, end=None) -> list[dict]:
        sql = ("SELECT ts,cash,total_eval,total_pnl,total_pnl_rate FROM account_snapshots"
               " WHERE account=%s")
        p: list = [account]
        if start: sql += " AND ts >= %s"; p.append(start)
        if end:   sql += " AND ts < %s";  p.append(_exclusive_end(end))  # 배타상한 (그날 포함, C-1)
        sql += " ORDER BY ts ASC"
        return self._query(sql, tuple(p))

    def get_cash_flows(self, account: str, start=None, end=None) -> list[dict]:
        sql = "SELECT ts,flow_type,amount,memo FROM cash_flows WHERE account=%s"
        p: list = [account]
        if start: sql += " AND ts >= %s"; p.append(start)
        if end:   sql += " AND ts < %s";  p.append(_exclusive_end(end))  # 배타상한 (그날 포함, C-1)
        sql += " ORDER BY ts ASC"
        return self._query(sql, tuple(p))

    def get_fills(self, start=None, end=None, limit: int = 500) -> list[dict]:
        sql = ("SELECT ts,ticker,side,filled_qty,filled_price,commission,tax"
               " FROM fills WHERE TRUE")
        p: list = []
        if start: sql += " AND ts >= %s"; p.append(start)
        if end:   sql += " AND ts < %s";  p.append(_exclusive_end(end))  # 배타상한 (그날 포함, C-1)
        sql += " ORDER BY ts ASC LIMIT %s"; p.append(limit)
        return self._query(sql, tuple(p))

    def close(self):
        self._conn.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
