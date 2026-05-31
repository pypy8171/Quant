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

-- ── 체결 원장 (H0STCNI0 체결통보) ───────────────────────────────────────────
CREATE TABLE IF NOT EXISTS fills (
    ts           TIMESTAMPTZ   NOT NULL,
    odno         TEXT          NOT NULL,   -- KIS 주문번호
    ticker       TEXT          NOT NULL,
    side         TEXT          NOT NULL,   -- BUY / SELL
    filled_qty   INTEGER       NOT NULL,
    filled_price NUMERIC(18,4) NOT NULL,
    commission   NUMERIC(18,4),            -- 수수료 (매수·매도 0.015%)
    tax          NUMERIC(18,4),            -- 거래세 (매도 0.18%)
    market       TEXT DEFAULT 'KR'
);
SELECT create_hypertable('fills', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS fills_odno        ON fills (odno);
CREATE INDEX IF NOT EXISTS fills_ticker_ts   ON fills (ticker, ts DESC);

-- ── 포지션 원장 (계좌 현재 상태) ─────────────────────────────────────────────
-- 체결 발생 시 UPSERT, 장 마감 EOD 배치에서도 갱신
CREATE TABLE IF NOT EXISTS positions (
    ticker       TEXT          PRIMARY KEY,
    quantity     INTEGER       NOT NULL DEFAULT 0,
    avg_price    NUMERIC(18,4) NOT NULL DEFAULT 0,  -- 매수 평균단가
    realized_pnl NUMERIC(18,4) NOT NULL DEFAULT 0,  -- 당일 실현손익
    updated_at   TIMESTAMPTZ   NOT NULL DEFAULT NOW()
);

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
