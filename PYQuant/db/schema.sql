-- TimescaleDB 익스텐션 활성화
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- ── 실시간 체결 (ZMQ TRADE) ───────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS ticks (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    price      NUMERIC(18,4),
    volume     BIGINT,
    direction  SMALLINT,        -- 1=매수체결, 5=매도체결
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('ticks', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS ticks_ticker_ts ON ticks (ticker, ts DESC);

-- ── 전략 시그널 (ZMQ SIGNAL) ──────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS signals (
    ts         TIMESTAMPTZ  NOT NULL,
    strategy   TEXT,
    ticker     TEXT         NOT NULL,
    side       TEXT,            -- BUY / SELL
    qty        INTEGER,
    price      NUMERIC(18,4),
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('signals', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS signals_ticker_ts ON signals (ticker, ts DESC);

-- ── 주문 결과 (ZMQ ORDER) ─────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS orders (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    side       TEXT,
    qty        INTEGER,
    price      NUMERIC(18,4),
    ok         BOOLEAN,
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('orders', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS orders_ticker_ts ON orders (ticker, ts DESC);

-- ── 엔진 상태 (ZMQ HEALTH) ───────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS health (
    ts         TIMESTAMPTZ  NOT NULL,
    data_cnt   BIGINT       DEFAULT 0,
    signal_cnt BIGINT       DEFAULT 0,
    order_cnt  BIGINT       DEFAULT 0
);
SELECT create_hypertable('health', 'ts', if_not_exists => TRUE);

-- ── KIS REST 일봉 (bars_1d) ──────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS bars_1d (
    ts         TIMESTAMPTZ  NOT NULL,
    ticker     TEXT         NOT NULL,
    open       NUMERIC(18,4),
    high       NUMERIC(18,4),
    low        NUMERIC(18,4),
    close      NUMERIC(18,4),
    volume     BIGINT,
    market     TEXT DEFAULT 'KR'
);
SELECT create_hypertable('bars_1d', 'ts', if_not_exists => TRUE);
CREATE UNIQUE INDEX IF NOT EXISTS bars_1d_ticker_ts ON bars_1d (ticker, ts DESC);
