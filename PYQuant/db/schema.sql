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

-- ── 계좌 일별 스냅샷 (원금추적/기간수익률용) ────────────────────────────────
-- EOD 1회 get_kr_balance() 결과를 적재. account='paper'|'real' 로 모의/실전 분리.
CREATE TABLE IF NOT EXISTS account_snapshots (
    ts             TIMESTAMPTZ   NOT NULL,
    account        TEXT          NOT NULL,   -- 'paper' | 'real'
    cash           NUMERIC(18,4),            -- 예수금
    total_eval     NUMERIC(18,4),            -- 총평가금액
    total_pnl      NUMERIC(18,4),            -- 총평가손익
    total_pnl_rate NUMERIC(10,4)             -- 총수익률(%)
);
SELECT create_hypertable('account_snapshots', 'ts', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS account_snapshots_acct_ts ON account_snapshots (account, ts DESC);

-- ── 입출금 내역 (원금 추적) ─────────────────────────────────────────────────
-- 실전은 입출금이 의미 있음(현재 수동 기록 — KIS 입출금 TR 확정 시 자동화).
-- 모의는 입출금 개념이 없어 비워 둠.
CREATE TABLE IF NOT EXISTS cash_flows (
    ts         TIMESTAMPTZ   NOT NULL,
    account    TEXT          NOT NULL,   -- 'paper' | 'real'
    flow_type  TEXT          NOT NULL,   -- DEPOSIT | WITHDRAW
    amount     NUMERIC(18,4) NOT NULL,   -- 양수 (방향은 flow_type)
    memo       TEXT
);
CREATE INDEX IF NOT EXISTS cash_flows_acct_ts ON cash_flows (account, ts DESC);

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

-- ── 시장 국면 (RegimeController, 장 시작 1회 판정) ───────────────────────────
-- 학습 데이터 축적 시작점. 개별 지표 분해 저장 (어느 지표가 국면을 갈랐나).
CREATE TABLE IF NOT EXISTS regime (
    date          DATE PRIMARY KEY,
    regime        TEXT NOT NULL,        -- BULL / NEUTRAL / BEAR
    score         INT  NOT NULL,        -- v0: -2..+2
    above_ma200   BOOLEAN,
    aligned_bull  BOOLEAN,
    aligned_bear  BOOLEAN,
    index_close   DOUBLE PRECISION,
    ts            TIMESTAMPTZ NOT NULL
);

-- ── 거래 라벨: 그때의 국면을 행동(signals)·결과(fills)에 stamp ────────────────
-- (상황→행동→결과) 삼각형 완성. 지금 안 붙이면 소급 불가.
ALTER TABLE signals ADD COLUMN IF NOT EXISTS regime TEXT;
ALTER TABLE fills   ADD COLUMN IF NOT EXISTS regime TEXT;
