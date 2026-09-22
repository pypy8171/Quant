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
    from psycopg2.extras import execute_values
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
    'ts <= 날짜'는 그날 00:00로 해석돼 장 마감(장마감 후) 적재분을 누락시킨다 (C-1)."""
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
        # 비밀번호는 하드코딩 기본값(구 'changeme')을 제거 — 소스에 크레덴셜을 심지 않는다.
        # 명시 인자 > TSDB_PASSWORD env 순. 둘 다 없으면 조용히 약한 기본으로 붙지 않고 기동 실패.
        # (docker-compose는 컨테이너에 TSDB_PASSWORD를 항상 주입하므로 도커 경로는 영향 없음.)
        password = password or os.getenv("TSDB_PASSWORD")
        if not password:
            raise RuntimeError(
                "TSDB_PASSWORD 미설정 — DB 비밀번호를 환경변수로 지정하세요 "
                "(하드코딩 기본값 제거됨). 로컬 개발은 .env.example 참고."
            )

        self._connect_parameters = dict(host=host, port=port, dbname=db, user=user, password=password)
        self._health_columns_ready = False   # 첫 HEALTH에서 한 번 ALTER TABLE을 돌린다
        self._connect(retries, retry_interval)

    def _connect(self, retries: int, retry_interval: float):
        """DB가 준비될 때까지 재시도 (Docker 기동 순서 대응)."""
        for attempt in range(1, retries + 1):
            try:
                self._conn = psycopg2.connect(connect_timeout=5, **self._connect_parameters)
                self._conn.autocommit = True
                parameters = self._connect_parameters
                logger.info(f"연결 완료: {parameters['user']}@{parameters['host']}:{parameters['port']}/{parameters['dbname']}")
                return
            except psycopg2.OperationalError as e:
                if attempt == retries:
                    raise
                logger.info(f"연결 대기 중... ({attempt}/{retries}): {e}")
                time.sleep(retry_interval)

    def _cursor(self):
        """커서를 연다. DB 컨테이너 재기동 등으로 연결이 끊겼으면(psycopg2가 closed를 세운다) 한 번 다시 붙는다 —
        리코더는 하루 종일 떠 있어서, 연결이 한 번 끊기면 남은 체결이 전부 빠지는 일을 막는다."""
        if self._conn.closed:
            logger.warning("DB 연결이 끊겨 있어 다시 붙는다")
            self._connect(retries=3, retry_interval=2.0)

        return self._conn.cursor()

    # ── 이벤트 insert ──────────────────────────────────────────────────────────

    def insert_trade(self, data: dict):
        try:
            _require(data, "ts", "ticker", "price")
            with self._cursor() as cursor:
                cursor.execute(
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
            with self._cursor() as cursor:
                cursor.execute(
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
            with self._cursor() as cursor:
                cursor.execute(
                    "INSERT INTO orders(ts,ticker,side,qty,price,ok,market,account)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s,%s)",
                    (
                        _ms_to_dt(data["ts"]),
                        data["ticker"],
                        data["side"],
                        data["qty"],
                        data.get("price"),
                        data["ok"],
                        data.get("market", "KR"),
                        data.get("account"),
                    ),
                )
        except Exception as e:
            logger.error(f"insert_order 실패 (data={data}): {e}")

    # 엔진이 HEALTH에 싣는 큐·지연 열. 옛 엔진(이 필드를 안 싣는 exe)이 보낸 행은 NULL로 들어간다.
    _HEALTH_METRIC_COLUMNS = (
        "drop_cnt",
        "queue_shard_high_water",
        "queue_shard_capacity",
        "queue_shard_out_size",
        "queue_shard_out_capacity",
        "queue_order_high_water",
        "queue_order_capacity",
        "queue_fill_high_water",
        "queue_fill_capacity",
        "dropped_shard",
        "dropped_order",
        "dropped_fill",
        "latency_samples",
        "tick_to_signal_p50_us",
        "tick_to_signal_p99_us",
        "signal_to_pop_p50_us",
        "signal_to_pop_p99_us",
        "pop_to_done_p50_us",
        "pop_to_done_p99_us",
        "total_p50_us",
        "total_p99_us",
    )

    # 열 이름과 payload 키가 다른 것. 엔진은 예전부터 "drop"을 보냈는데 적재기가 버리고 있었다.
    _HEALTH_COLUMN_KEYS = {"drop_cnt": "drop"}

    def ensure_health_metric_columns(self):
        """기존 DB의 health 표에도 큐·지연 열이 있도록 보장. schema.sql은 DB를 새로 만들 때만 돈다."""
        try:
            with self._cursor() as cursor:
                for name in self._HEALTH_METRIC_COLUMNS:
                    cursor.execute(f"ALTER TABLE health ADD COLUMN IF NOT EXISTS {name} BIGINT")

            self._health_columns_ready = True
        except Exception as error:
            logger.error(f"ensure_health_metric_columns 실패: {error}")

    def insert_health(self, data: dict):
        try:
            _require(data, "ts")

            if not self._health_columns_ready:   # 리코더 기동 뒤 첫 HEALTH 한 번만
                self.ensure_health_metric_columns()

            names = ",".join(self._HEALTH_METRIC_COLUMNS)
            placeholders = ",".join(["%s"] * (4 + len(self._HEALTH_METRIC_COLUMNS)))

            with self._cursor() as cursor:
                cursor.execute(
                    f"INSERT INTO health(ts,data_cnt,signal_cnt,order_cnt,{names})"
                    f" VALUES ({placeholders})",
                    (
                        _ms_to_dt(data["ts"]),
                        data.get("data", 0),
                        data.get("signal", 0),
                        data.get("order", 0),
                        *(data.get(self._HEALTH_COLUMN_KEYS.get(name, name))
                          for name in self._HEALTH_METRIC_COLUMNS),
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
            with self._cursor() as cursor:
                cursor.executemany(
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
            with self._cursor() as cursor:
                cursor.execute(
                    "INSERT INTO fills"
                    "(ts,odno,ticker,side,filled_qty,filled_price,commission,tax,market,regime,strategy,account)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
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
                        data.get("strategy"),
                        data.get("account"),  # 브로커 계좌번호 (D-090, 실계좌·모의계좌 분리)
                    ),
                )
        except Exception as e:
            logger.error(f"insert_fill 실패 (data={data}): {e}")

    def upsert_position(self, ticker: str, quantity: int,
                        avg_price: float, realized_pnl: float,
                        account: str = "unknown"):
        """포지션 원장 갱신 — 체결 후 또는 장 마감 배치에서 호출.
        PK가 (account,ticker)라 계좌를 안 넘기면 'unknown' 계좌로 쌓인다 — 실계좌·모의계좌
        원장이 섞이는 것(D-090)보다는 안전한 기본값."""
        try:
            with self._cursor() as cursor:
                cursor.execute(
                    "INSERT INTO positions(account,ticker,quantity,avg_price,realized_pnl,updated_at)"
                    " VALUES (%s,%s,%s,%s,%s,NOW())"
                    " ON CONFLICT (account,ticker) DO UPDATE SET"
                    "   quantity=EXCLUDED.quantity,"
                    "   avg_price=EXCLUDED.avg_price,"
                    "   realized_pnl=EXCLUDED.realized_pnl,"
                    "   updated_at=NOW()",
                    (account, ticker, quantity, avg_price, realized_pnl),
                )
        except Exception as e:
            logger.error(f"upsert_position 실패 ({account}:{ticker}): {e}")

    def insert_bar(self, ticker: str, ts: datetime, o: float, h: float,
                   lo: float, c: float, vol: int, market: str = "KR"):
        with self._cursor() as cursor:
            cursor.execute(
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
            with self._cursor() as cursor:
                for stmt in ddl:
                    cursor.execute(stmt)
        except Exception as e:
            logger.error(f"ensure_regime_tables 실패: {e}")

    def ensure_fills_amount_columns(self):
        """기존 DB에도 fills.total_amount/net_amount 생성 컬럼이 있도록 보장.
        schema.sql은 fresh init에만 적용되므로 기존 DB는 이 마이그레이션 필요."""
        ddl = [
            "ALTER TABLE fills ADD COLUMN IF NOT EXISTS total_amount NUMERIC(18,4)"
            " GENERATED ALWAYS AS (filled_qty * filled_price) STORED",
            "ALTER TABLE fills ADD COLUMN IF NOT EXISTS net_amount NUMERIC(18,4)"
            " GENERATED ALWAYS AS ("
            "  CASE WHEN side = 'SELL'"
            "       THEN filled_qty * filled_price - COALESCE(commission, 0) - COALESCE(tax, 0)"
            "       ELSE filled_qty * filled_price + COALESCE(commission, 0)"
            "  END"
            " ) STORED",
        ]
        try:
            with self._cursor() as cursor:
                for stmt in ddl:
                    cursor.execute(stmt)
        except Exception as e:
            logger.error(f"ensure_fills_amount_columns 실패: {e}")

    def ensure_proc_statistics_table(self):
        """기존 DB에도 proc_stats·proc_thread_stats·proc_hotspots 표가 있도록 보장. schema.sql은 fresh init에만 적용된다."""
        ddl = [
            "CREATE TABLE IF NOT EXISTS proc_stats ("
            " ts TIMESTAMPTZ NOT NULL, process_name TEXT NOT NULL, pid INTEGER,"
            " cpu_percent DOUBLE PRECISION, memory_mb DOUBLE PRECISION, thread_count INTEGER)",
            "CREATE INDEX IF NOT EXISTS proc_stats_name_ts ON proc_stats (process_name, ts DESC)",
            "ALTER TABLE proc_stats ADD COLUMN IF NOT EXISTS core_count INTEGER",
            "CREATE TABLE IF NOT EXISTS proc_thread_stats ("
            " ts TIMESTAMPTZ NOT NULL, process_name TEXT NOT NULL, pid INTEGER, tid INTEGER,"
            " thread_name TEXT, cpu_percent DOUBLE PRECISION)",
            "CREATE INDEX IF NOT EXISTS proc_thread_stats_name_ts ON proc_thread_stats (thread_name, ts DESC)",
            "CREATE TABLE IF NOT EXISTS proc_hotspots ("
            " ts TIMESTAMPTZ NOT NULL, process_name TEXT NOT NULL, pid INTEGER, sample_seconds DOUBLE PRECISION,"
            " symbol TEXT NOT NULL, shared_object TEXT, self_percent DOUBLE PRECISION, samples BIGINT)",
            "CREATE INDEX IF NOT EXISTS proc_hotspots_ts ON proc_hotspots (ts DESC)",
        ]
        try:
            with self._cursor() as cursor:
                for stmt in ddl:
                    cursor.execute(stmt)

                for table in ("proc_stats", "proc_thread_stats", "proc_hotspots"):
                    try:
                        cursor.execute(f"SELECT create_hypertable('{table}','ts', if_not_exists => TRUE)")
                    except Exception:
                        pass
        except Exception as error:
            logger.error(f"ensure_proc_statistics_table 실패: {error}")

    def ensure_query_statistics(self) -> bool:
        """pg_stat_statements 확장을 켠다(쿼리별 호출 수·누적 시간). 서버가 shared_preload_libraries에
        그 모듈을 싣고 떠야만 켜지므로(docker-compose.yml timescaledb command), 못 켜면 False."""
        try:
            with self._cursor() as cursor:
                cursor.execute("CREATE EXTENSION IF NOT EXISTS pg_stat_statements")
                cursor.execute("SELECT 1 FROM pg_stat_statements LIMIT 1")

            return True
        except Exception as error:
            logger.warning(f"pg_stat_statements 못 켬 — DB 컨테이너를 docker-compose.yml의 command로 다시 띄워야 한다: {error}")
            return False

    def insert_proc_thread_statistics(self, rows: list):
        """스레드별 CPU 표본 한 묶음(같은 ts). rows: [{process_name,pid,tid,thread_name,cpu_percent}]"""
        if not rows:
            return

        try:
            now = datetime.now(timezone.utc)
            with self._cursor() as cursor:   # 표본 하나가 스레드 수만큼 왕복하지 않도록 한 문장으로 보낸다
                execute_values(
                    cursor,
                    "INSERT INTO proc_thread_stats(ts,process_name,pid,tid,thread_name,cpu_percent)"
                    " VALUES %s",
                    [(now, row["process_name"], row.get("pid"), row.get("tid"), row.get("thread_name"),
                      row.get("cpu_percent")) for row in rows],
                )
        except Exception as error:
            logger.error(f"insert_proc_thread_stats 실패 ({len(rows)}행): {error}")

    def insert_proc_hotspots(self, rows: list):
        """perf 표본 한 회차의 함수별 자기 시간 비율. rows: [{process_name,pid,sample_seconds,symbol,shared_object,self_percent,samples}]"""
        if not rows:
            return

        try:
            now = datetime.now(timezone.utc)
            with self._cursor() as cursor:   # 위와 같은 이유로 한 문장
                execute_values(
                    cursor,
                    "INSERT INTO proc_hotspots(ts,process_name,pid,sample_seconds,symbol,shared_object,self_percent,samples)"
                    " VALUES %s",
                    [(now, row["process_name"], row.get("pid"), row.get("sample_seconds"), row["symbol"],
                      row.get("shared_object"), row.get("self_percent"), row.get("samples")) for row in rows],
                )
        except Exception as error:
            logger.error(f"insert_proc_hotspots 실패 ({len(rows)}행): {error}")

    def insert_proc_stat(self, data: dict):
        try:
            _require(data, "process_name", "cpu_percent", "memory_mb")
            with self._cursor() as cursor:
                cursor.execute(
                    "INSERT INTO proc_stats(ts,process_name,pid,cpu_percent,memory_mb,thread_count,core_count)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s)",
                    (
                        datetime.now(timezone.utc),
                        data["process_name"],
                        data.get("pid"),
                        data["cpu_percent"],
                        data["memory_mb"],
                        data.get("thread_count"),
                        data.get("core_count"),
                    ),
                )
        except Exception as error:
            logger.error(f"insert_proc_stat 실패 (data={data}): {error}")

    # ── 벤치마크(bench_*) — bench_market_open.py 전용, 실계좌/모의계좌 원장과 분리 ──────

    def ensure_bench_tables(self):
        """기존 DB에도 bench_* 테이블이 있도록 보장. schema.sql은 fresh init에만 적용된다."""
        ddl = [
            "CREATE TABLE IF NOT EXISTS bench_ticks ("
            " ts TIMESTAMPTZ NOT NULL, ticker TEXT NOT NULL, price NUMERIC(18,4),"
            " volume BIGINT, direction SMALLINT, market TEXT DEFAULT 'KR')",
            "CREATE INDEX IF NOT EXISTS bench_ticks_ticker_ts ON bench_ticks (ticker, ts DESC)",
            "CREATE TABLE IF NOT EXISTS bench_signals ("
            " ts TIMESTAMPTZ NOT NULL, strategy TEXT, ticker TEXT NOT NULL, side TEXT,"
            " qty INTEGER, price NUMERIC(18,4), market TEXT DEFAULT 'KR')",
            "CREATE INDEX IF NOT EXISTS bench_signals_ticker_ts ON bench_signals (ticker, ts DESC)",
            "CREATE TABLE IF NOT EXISTS bench_orders ("
            " ts TIMESTAMPTZ NOT NULL, ticker TEXT NOT NULL, side TEXT, qty INTEGER,"
            " price NUMERIC(18,4), ok BOOLEAN, market TEXT DEFAULT 'KR', account TEXT)",
            "CREATE INDEX IF NOT EXISTS bench_orders_ticker_ts ON bench_orders (ticker, ts DESC)",
            "CREATE TABLE IF NOT EXISTS bench_fills ("
            " ts TIMESTAMPTZ NOT NULL, odno TEXT NOT NULL, ticker TEXT NOT NULL, side TEXT NOT NULL,"
            " filled_qty INTEGER NOT NULL, filled_price NUMERIC(18,4) NOT NULL,"
            " commission NUMERIC(18,4), tax NUMERIC(18,4), market TEXT DEFAULT 'KR', account TEXT)",
            "CREATE INDEX IF NOT EXISTS bench_fills_ticker_ts ON bench_fills (ticker, ts DESC)",
            "CREATE TABLE IF NOT EXISTS bench_positions ("
            " account TEXT NOT NULL DEFAULT 'bench', ticker TEXT NOT NULL,"
            " quantity INTEGER NOT NULL DEFAULT 0, avg_price NUMERIC(18,4) NOT NULL DEFAULT 0,"
            " realized_pnl NUMERIC(18,4) NOT NULL DEFAULT 0, updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
            " PRIMARY KEY (account, ticker))",
        ]
        try:
            with self._cursor() as cursor:
                for stmt in ddl:
                    cursor.execute(stmt)
                for table in ("bench_ticks", "bench_signals", "bench_orders", "bench_fills"):
                    try:
                        cursor.execute(f"SELECT create_hypertable('{table}','ts', if_not_exists => TRUE)")
                    except Exception:
                        pass
        except Exception as error:
            logger.error(f"ensure_bench_tables 실패: {error}")

    def truncate_bench_tables(self):
        """벤치마크 재실행 전 이전 결과 비우기."""
        try:
            with self._cursor() as cursor:
                cursor.execute("TRUNCATE bench_ticks, bench_signals, bench_orders, bench_fills, bench_positions")
        except Exception as error:
            logger.error(f"truncate_bench_tables 실패: {error}")

    def insert_bench_ticks_batch(self, records: list[tuple]):
        """records: (ts, ticker, price, volume, direction, market) 튜플 리스트."""
        if not records:
            return
        try:
            with self._cursor() as cursor:
                cursor.executemany(
                    "INSERT INTO bench_ticks(ts,ticker,price,volume,direction,market) VALUES (%s,%s,%s,%s,%s,%s)",
                    records,
                )
        except Exception as error:
            logger.error(f"insert_bench_ticks_batch 실패 ({len(records)}건): {error}")

    def insert_bench_signals_batch(self, records: list[tuple]):
        """records: (ts, strategy, ticker, side, qty, price, market) 튜플 리스트."""
        if not records:
            return
        try:
            with self._cursor() as cursor:
                cursor.executemany(
                    "INSERT INTO bench_signals(ts,strategy,ticker,side,qty,price,market) VALUES (%s,%s,%s,%s,%s,%s,%s)",
                    records,
                )
        except Exception as error:
            logger.error(f"insert_bench_signals_batch 실패 ({len(records)}건): {error}")

    def insert_bench_orders_batch(self, records: list[tuple]):
        """records: (ts, ticker, side, qty, price, ok, market, account) 튜플 리스트."""
        if not records:
            return
        try:
            with self._cursor() as cursor:
                cursor.executemany(
                    "INSERT INTO bench_orders(ts,ticker,side,qty,price,ok,market,account) VALUES (%s,%s,%s,%s,%s,%s,%s,%s)",
                    records,
                )
        except Exception as error:
            logger.error(f"insert_bench_orders_batch 실패 ({len(records)}건): {error}")

    def insert_bench_fills_batch(self, records: list[tuple]):
        """records: (ts, odno, ticker, side, filled_qty, filled_price, commission, tax, market, account) 튜플 리스트."""
        if not records:
            return
        try:
            with self._cursor() as cursor:
                cursor.executemany(
                    "INSERT INTO bench_fills(ts,odno,ticker,side,filled_qty,filled_price,commission,tax,market,account)"
                    " VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                    records,
                )
        except Exception as error:
            logger.error(f"insert_bench_fills_batch 실패 ({len(records)}건): {error}")

    def upsert_bench_position(self, ticker: str, quantity: int, average_price: float,
                              realized_pnl: float, account: str = "bench"):
        try:
            with self._cursor() as cursor:
                cursor.execute(
                    "INSERT INTO bench_positions(account,ticker,quantity,avg_price,realized_pnl,updated_at)"
                    " VALUES (%s,%s,%s,%s,%s,NOW())"
                    " ON CONFLICT (account,ticker) DO UPDATE SET"
                    "   quantity=EXCLUDED.quantity,"
                    "   avg_price=EXCLUDED.avg_price,"
                    "   realized_pnl=EXCLUDED.realized_pnl,"
                    "   updated_at=NOW()",
                    (account, ticker, quantity, average_price, realized_pnl),
                )
        except Exception as error:
            logger.error(f"upsert_bench_position 실패 ({account}:{ticker}): {error}")

    def insert_regime(self, data: dict):
        """국면 스냅샷 1건 적재. date를 PK로 UPSERT (장 시작 1회 → 하루 1행)."""
        try:
            _require(data, "date", "regime", "score")
            ts = data.get("ts")
            ts = ts if isinstance(ts, datetime) else (_ms_to_dt(ts) if ts else datetime.now(timezone.utc))
            with self._cursor() as cursor:
                cursor.execute(
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
            with self._cursor() as cursor:
                for stmt in ddl:
                    cursor.execute(stmt)
                # account_snapshots를 하이퍼테이블로 (가능할 때만 — TimescaleDB 없으면 무시)
                try:
                    cursor.execute("SELECT create_hypertable('account_snapshots','ts',"
                                "if_not_exists => TRUE)")
                except Exception:
                    pass
        except Exception as e:
            logger.error(f"ensure_report_tables 실패: {e}")

    def insert_account_snapshot(self, account: str, cash: float, total_eval: float,
                                total_pnl: float, total_pnl_rate: float, ts=None):
        try:
            ts = ts or datetime.now(timezone.utc)
            with self._cursor() as cursor:
                # 같은 날 중복 적재 방지 (C-2): 하이퍼테이블이라 (account,date) UNIQUE가 까다로워
                # 동일 (account, 날짜) 기존 행을 제거 후 삽입 → 하루 1행 보장(결정적 begin/end)
                cursor.execute("DELETE FROM account_snapshots"
                            " WHERE account=%s AND ts::date = %s::date", (account, ts))
                cursor.execute(
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
            with self._cursor() as cursor:
                cursor.execute(
                    "INSERT INTO cash_flows(ts,account,flow_type,amount,memo)"
                    " VALUES (%s,%s,%s,%s,%s)",
                    (ts, account, flow_type, abs(float(amount)), memo),
                )
        except Exception as e:
            logger.error(f"insert_cash_flow 실패: {e}")

    def _query(self, sql: str, params: tuple) -> list[dict]:
        # SQL 예외를 삼키지 않는다 (C-3): []를 반환하면 "조회 실패"와 "무데이터"가 구분 불가 →
        # 자금 리포트가 DB 오류를 '거래 없음'으로 오인할 수 있다. 예외를 전파해 호출측이 인지하게 한다.
        with self._cursor() as cursor:
            cursor.execute(sql, params)
            column_names = [column[0] for column in cursor.description]
            return [dict(zip(column_names, row)) for row in cursor.fetchall()]

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
